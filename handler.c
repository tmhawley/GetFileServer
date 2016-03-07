#include <stdlib.h>
#include <pthread.h>

#include "gfserver.h"


extern void enqueuerequest(gfcontext_t *ctx, char *path);
extern pthread_mutex_t mutex;
extern pthread_cond_t c_worker;

ssize_t handler_get(gfcontext_t *ctx, char *path, void *arg) {
    //Acquire the lock
    pthread_mutex_lock(&mutex);

    //Queue up the request for the worker threads to consume
    enqueuerequest(ctx, path);

    //Signal to a worker thread to consume the request
    pthread_cond_signal(&c_worker);

    //Release the lock
    pthread_mutex_unlock(&mutex);

    return 0;
}

