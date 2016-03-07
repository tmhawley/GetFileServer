#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include "gfserver.h"
#include "content.h"
#include "steque.h"

#define USAGE                                                                 \
"usage:\n"                                                                    \
"  webproxy [options]\n"                                                      \
"options:\n"                                                                  \
"  -p                  Listen port (Default: 8888)\n"                         \
"  -t                  Number of threads (Default: 1)"                         \
"  -c                  Content file mapping keys to content files\n"          \
"  -h                  Show this help message\n"

#define BUFFER_SIZE 4096

static steque_t *queue;
static int nthreads;
static volatile int cancellationRequested;  //Needs to be volatile because the worker threads are checking this on every loop, but it can get modified by the main thread
static int cleanupPerformed;
static pthread_t *workerThreadIDs;
static gfserver_t *gfs;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t c_worker = PTHREAD_COND_INITIALIZER;
pthread_mutex_t cleanupMutex = PTHREAD_MUTEX_INITIALIZER;

extern ssize_t handler_get(gfcontext_t *ctx, char *path, void *arg);

typedef struct gfccontextwithpath_t {
    gfcontext_t *ctx;
    char * path;
} gfccontextwithpath_t;

extern void enqueuerequest(gfcontext_t *ctx, char *path) {
    gfccontextwithpath_t *request = malloc(sizeof(gfccontextwithpath_t));
    request->ctx = ctx;
    request->path = strdup(path);
    steque_enqueue(queue, request);
}

/* Worker Thread Function ========================================================= */
void *processrequest(void *arg) {
    while (1) {
        //Add cancellation exits at the top of loop and also after conditional
        //This should cover all cases needed for cancellation
        if (cancellationRequested) {
            pthread_exit(0);
        }

        //Acquire the lock
        pthread_mutex_lock(&mutex);

        //While the queue is empty, block
        while (steque_isempty(queue)) {
            pthread_cond_wait(&c_worker, &mutex);

            if (cancellationRequested) {
                pthread_mutex_unlock(&mutex);
                pthread_exit(0);
            }
        }

        //Pop a request off the queue
        gfccontextwithpath_t *ctxwithpath = steque_pop(queue);

        //Release the lock
        pthread_mutex_unlock(&mutex);

        ///Begin transferring the file
        int fildes;
        ssize_t file_len, bytes_transferred;
        ssize_t read_len, write_len;
        char buffer[BUFFER_SIZE];

        //Get file contents
        if ((fildes = content_get(ctxwithpath->path)) < 0) {
            gfs_sendheader(ctxwithpath->ctx, GF_FILE_NOT_FOUND, 0);
        }
        else { //File descriptor acquired
            //Calculating the file size
            file_len = lseek(fildes, 0, SEEK_END);

            if (gfs_sendheader(ctxwithpath->ctx, GF_OK, file_len) < 0) {
                fprintf(stderr, "Failed to OK header for file %s", ctxwithpath->path);
            }
            else { //Header successfully send to client, now send file
                bytes_transferred = 0;
                while (bytes_transferred < file_len) {
                    read_len = pread(fildes, buffer, BUFFER_SIZE, bytes_transferred);
                    if (read_len <= 0) {
                        fprintf(stderr, "Error reading file contents for file %s, read length: %zd, bytes transferred: %zu, file length: %zu\n",
                        ctxwithpath->path, read_len, bytes_transferred, file_len);
                        gfs_abort(ctxwithpath->ctx);
                        break;
                    }
                    write_len = gfs_send(ctxwithpath->ctx, buffer, read_len);
                    if (write_len != read_len) {
                        fprintf(stderr, "Error transferring file contents for file %s to client\n", ctxwithpath->path);
                        gfs_abort(ctxwithpath->ctx);
                        break;
                    }
                    bytes_transferred += write_len;
                }
            }
        }

        //Free request resources. ctxwithpath->ctx was allocated in gfserver.c, so it is freed there in gfs_send()
        //ctxwithpath->path and ctxwithpath are allocated in gfserver_main.c, so they are freed in this file
        free(ctxwithpath->path);
        free(ctxwithpath);
    }
}

void globalCleanup() {
    //This could be called from main() or signalHandler(), so this lock ensures that the cleanup
    //is only performed once by synchronizing on the cleanupPerformed boolean.
    pthread_mutex_lock(&cleanupMutex);

    if (!cleanupPerformed) {
        //Signal cancellation to workers and wait on them
        cancellationRequested = 1;

        pthread_cond_broadcast(&c_worker);

        //Wait for other threads to exit
        int i;
        for (i = 0; i < nthreads; ++i) {
            if (pthread_join(workerThreadIDs[i], NULL) < 0) {
                fprintf(stderr, "Failed to join pthread %d", i);
            }
        }

        steque_destroy(queue);
        free(queue);
        free(workerThreadIDs);
        free(gfs);
        content_destroy();

        cleanupPerformed = 1;
    }

    pthread_mutex_unlock(&cleanupMutex);
}

void signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        globalCleanup();
        exit(0);
    }
}

/* Main ========================================================= */
int main(int argc, char **argv) {
    int option_char = 0;
    nthreads = 1;
    unsigned short port = 8888;
    char *content = "content.txt";

    //Set up cancellation to handle a graceful shutdown of the server
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = signalHandler;
    sigaction(SIGINT, &act, NULL);
    cancellationRequested = 0;
    cleanupPerformed = 0;

    // Parse and set command line arguments
    while ((option_char = getopt(argc, argv, "p:t:c:h")) != -1) {
        switch (option_char) {
            case 'p': // listen-port
                port = atoi(optarg);
                break;
            case 't': // nthreads
                nthreads = atoi(optarg);
                break;
            case 'c': // file-path
                content = optarg;
                break;
            case 'h': // help
                fprintf(stdout, "%s", USAGE);
                exit(0);
            default:
                fprintf(stderr, "%s", USAGE);
                exit(1);
        }
    }

    content_init(content);

    /*Initializing server*/
    gfs = gfserver_create();

    /*Setting options*/
    gfserver_set_port(gfs, port);
    gfserver_set_maxpending(gfs, 100);
    gfserver_set_handler(gfs, handler_get);
    gfserver_set_handlerarg(gfs, NULL);

    queue = malloc(sizeof(steque_t));
    steque_init(queue);

    workerThreadIDs = malloc(nthreads * sizeof(pthread_t));

    int i = 0;

    //Create the worker threads
    for (i = 0; i < nthreads; i++) {
        if (pthread_create(&workerThreadIDs[i], NULL, processrequest, NULL) != 0) {
            fprintf(stderr, "Creating worker threads failed\n");
            exit(EXIT_FAILURE);
        }
    }

    /*Loops forever*/
    gfserver_serve(gfs);

    /* Should never be reached because the server loops forever, but free memory anyways */
    globalCleanup();

    return EXIT_SUCCESS;
}