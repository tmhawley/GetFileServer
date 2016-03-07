#include <unistd.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "gfserver.h"

#define BUFSIZE 4096
#define HEADER_VALID 1
#define HEADER_INCOMPLETE 0
#define HEADER_INVALID -1
#define SCHEME_NAME_LENGTH 8
#define METHOD_NAME_LENGTH 4
#define PATH_MAX 256
#define HEADERSIZE 300 /* The largest allowable header would be
                        "GETFILE GET <maxpathlength usually 256 char> \r\n\r\n" which is 273 bytes,
                        so 300 byte is plenty for allocating a header buffer */

struct gfserver_t {
    int server_socket_fd;
    unsigned short port;
    int max_npending;
    ssize_t (*handler)(gfcontext_t *, char *, void *);
    void *arg;
};

struct gfcontext_t {
    int client_socket_fd;
    size_t fileLength;
    size_t bytesTransferred;
};

static const char *GetFileSchemeName = "GETFILE";
static const char *HeaderDelimeter = "\r\n\r\n";
static const char *GetMethodName = "GET";
static const char *OkResponse = "OK";
static const char *FnfResponse = "FILE_NOT_FOUND";
static const char *ErrorResponse = "ERROR";

//Calls send() until all contents in the buffer are sent
ssize_t gfs_sendcontents(int socketDesc, char *buffer, ssize_t *length)
{
    assert(buffer != NULL);
    assert(length != NULL);

    ssize_t total = 0;        // total bytes sent so far
    ssize_t bytesleft = *length; // bytes remaining to send
    ssize_t bytesSent = 0;  //bytes sent to client

    //Loop until all contents have been sent
    while(total < *length) {
        if ((bytesSent = send(socketDesc, buffer+total, bytesleft, 0)) < 0) {
            fprintf(stderr, "Error sending request over socket %d to server\n", socketDesc);
            break;
        }
        total += bytesSent;
        bytesleft -= bytesSent;
    }

    return bytesSent;
}

ssize_t gfs_sendheader(gfcontext_t *ctx, gfstatus_t status, size_t file_len) {
    assert(ctx != NULL);

    char header[50];

    ctx->fileLength = file_len;
    ctx->bytesTransferred = 0;

    if (status == GF_OK) {
        snprintf(header, sizeof(header), "%s %s %zd %s", GetFileSchemeName, OkResponse, file_len, HeaderDelimeter);
    }
    else if (status == GF_FILE_NOT_FOUND) {
        snprintf(header, sizeof(header), "%s %s %s", GetFileSchemeName, FnfResponse, HeaderDelimeter);
    }
    else {
        snprintf(header, sizeof(header), "%s %s %s", GetFileSchemeName, ErrorResponse, HeaderDelimeter);
    }

    ssize_t headerSize = strlen(header);
    if (gfs_sendcontents(ctx->client_socket_fd, header, &headerSize) < headerSize) {
        fprintf(stderr, "Failed to send header to the client with socket %d\n", ctx->client_socket_fd);
        return -1;
    }

    return headerSize;
}

ssize_t gfs_send(gfcontext_t *ctx, void *data, size_t len) {
    assert(ctx != NULL);
    assert(data != NULL);

    ssize_t bytesSent = 0;

    if ((bytesSent = gfs_sendcontents(ctx->client_socket_fd, data, &len)) < len) {
        fprintf(stderr, "Failed to send file to the client with socket %d\n", ctx->client_socket_fd);
    }

    ctx->bytesTransferred += bytesSent;

    if (ctx->bytesTransferred >= ctx->fileLength) {
        if (shutdown(ctx->client_socket_fd, SHUT_RDWR)) {
            fprintf(stderr, "Failed to shutdown client socket number %d for writing\n", ctx->client_socket_fd);
        }
        if (close(ctx->client_socket_fd) < 0) {
            fprintf(stderr, "Failed to close client socket number %d\n", ctx->client_socket_fd);
        }
        free(ctx);
    }

    return bytesSent;
}

void gfs_abort(gfcontext_t *ctx) {
    assert(ctx != NULL);

    if (shutdown(ctx->client_socket_fd, SHUT_RDWR)) {
        fprintf(stderr, "Failed to shutdown client socket number %d\n", ctx->client_socket_fd);
    }
    if (close(ctx->client_socket_fd) < 0) {
        fprintf(stderr, "Failed to close client socket number %d\n", ctx->client_socket_fd);
    }

    free(ctx);
}

//Checks the validity of the header including:
//Scheme name is valid e.g. is GETFILE
//Method name is valid e.g. GET.  This is the only supported method as of today
//File path is valid e.g. does not contain any illegal unix path characters
//End position is valid e.g. is greater than 0.  This ensures that the /r/n/r/n delimeter was encountered
int gfs_isheadervalid(char *scheme, char *method, char *filePath, int *endPosition) {
    assert(scheme != NULL);
    assert(method != NULL);
    assert(filePath != NULL);
    assert(endPosition != NULL);

    int returnValue = HEADER_VALID;

    //Scheme has been populated, so validate it
    if (strcmp(scheme, "") != 0) {
        if (strcmp(scheme, GetFileSchemeName) != 0) {
            fprintf(stderr, "Schema name was invalid in request header");
            returnValue = HEADER_INVALID; //Scheme is invalid
        }
    }
    else {
        returnValue = HEADER_INCOMPLETE; //Header not complete yet
    }

    //Method has been populated, so validate it
    if (strcmp(method, "") != 0) {
        if (strcmp(method, GetMethodName)) {
            fprintf(stderr, "Method name was invalid in request header");
            returnValue = HEADER_INVALID; //Method is invalid
        }
    }
    else {
        returnValue = HEADER_INCOMPLETE; //Header not complete yet
    }

    //Filename has been populated, so validate it
    if (strcmp(filePath, "") != 0) {
        if (filePath[0] != '/') {
            fprintf(stderr, "File path was not a valid unix path and should start with '/'");
            returnValue = HEADER_INVALID;
        }
    }
    else {
        returnValue = HEADER_INCOMPLETE; //Header not complete yet
    }

    //End position was populated, so validate it
    if (*endPosition == 0) {
        returnValue = HEADER_INCOMPLETE; //Header not complete yet
    }

    return returnValue;
}

gfserver_t *gfserver_create() {
    gfserver_t *gfs = malloc(sizeof(gfserver_t));
    return gfs;
}

void gfserver_set_port(gfserver_t *gfs, unsigned short port) {
    assert(gfs != NULL);

    gfs->port = port;
}

void gfserver_set_maxpending(gfserver_t *gfs, int max_npending) {
    assert(gfs != NULL);

    if (max_npending < 0) {
        fprintf(stderr, "Maximum number pending connections cannot be negative\n");
        exit(EXIT_FAILURE);
    }

    gfs->max_npending = max_npending;
}

void gfserver_set_handler(gfserver_t *gfs, ssize_t (*handler)(gfcontext_t *, char *, void *)) {
    assert(gfs != NULL);
    assert(handler != NULL);

    gfs->handler = handler;
}

void gfserver_set_handlerarg(gfserver_t *gfs, void *arg) {
    assert(gfs != NULL);

    gfs->arg = arg;
}

void gfserver_serve(gfserver_t *gfs) {
    assert(gfs != NULL);

    // Create socket (IPv4, stream-based, protocol likely set to TCP)
    if ((gfs->server_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "Failed to create socket for file requests\n");
    }

    struct sockaddr_in server;

    //Allow for reuse of IP addresses with the same port
    int set_reuse_addr = 1;
    if (setsockopt(gfs->server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &set_reuse_addr, sizeof(set_reuse_addr)) < 0) {
        fprintf(stderr, "Failed to set socket options to reuse address\n");
    }

    // Set up server socket address structure using local wildcard (0.0.0.0)
    bzero(&server, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(gfs->port);

    // Bind the socket
    if (bind(gfs->server_socket_fd, (struct sockaddr *) &server, sizeof(server)) < 0) {
        fprintf(stderr, "Failed to bind socket for file requests\n");
    }

    // Listen on the socket for up to some maximum pending connections
    if (listen(gfs->server_socket_fd, gfs->max_npending) < 0) {
        fprintf(stderr, "Failed to listen for incoming file transfer requests\n");
    }

    //Accept new clients and process their requests indefinitely
    while (1) {
        int client_socket_fd = 0;
        struct sockaddr_in client;
        socklen_t client_addr_len = 0;

        // Accept a new client
        if ((client_socket_fd = accept(gfs->server_socket_fd, (struct sockaddr *) &client, &client_addr_len)) < 0) {
            fprintf(stderr, "Failed to accept a new client\n");
            usleep(5000);  //To avoid thrashing CPU, sleep for 5 seconds before trying to accept another client
            continue;
        }

        //Allocate the client context
        gfcontext_t *ctx = malloc(sizeof(gfcontext_t));

        ctx->client_socket_fd = client_socket_fd;

        //Create buffers to read the request header from the client and store it
        char buffer[BUFSIZE] = "";
        char header[HEADERSIZE] = "";
        size_t numBytesReceivedOnRead = 0;
        ssize_t totalHeaderBytesReceived = 0;

        while (1) {
            //Read the request from the client
            numBytesReceivedOnRead = read(ctx->client_socket_fd, buffer, BUFSIZE);

            if (numBytesReceivedOnRead > 0) {
                //Copy up to the maximum header length worth of bytes into the header buffer
                if (totalHeaderBytesReceived + numBytesReceivedOnRead <= HEADERSIZE) {
                    memcpy(header + totalHeaderBytesReceived, buffer, numBytesReceivedOnRead);
                    totalHeaderBytesReceived += numBytesReceivedOnRead;
                }
                else {
                    memcpy(header + totalHeaderBytesReceived, buffer, HEADERSIZE - totalHeaderBytesReceived);
                    totalHeaderBytesReceived = HEADERSIZE;
                }

                //New bytes have been put into the header buffer, so attempt to read and validate the header
                char scheme[SCHEME_NAME_LENGTH] = "";
                char method[METHOD_NAME_LENGTH] = "";
                char filepath[PATH_MAX] = "";
                int endPosition = 0;

                if (sscanf(header, "%7s %4s %255s\r\n\r\n%n", scheme, method, filepath, &endPosition) > EOF) {
                    //Validate header.  Filepath is excluded because it can be a blank string
                    int isValid = gfs_isheadervalid(scheme, method, filepath, &endPosition);

                    if (isValid == HEADER_VALID) {
                        //If header is valid, call handler
                        gfs->handler(ctx, filepath, gfs->arg);
                        break;
                    }
                    else if (isValid == HEADER_INVALID || totalHeaderBytesReceived == HEADERSIZE) {
                        //Header not complete or malformed
                        fprintf(stderr, "Request header from the client was malformed or incomplete\n");
                        gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
                        break;
                    }
                }
            }
            //Socket was closed and header was not valid
            else if (numBytesReceivedOnRead == 0) {
                fprintf(stderr, "The socket was closed and the request header was incomplete or malformed.  The format should be \"GETFILE GET <file path>\\r\\n\\r\\n\"\n");
                break;
            }
            //Error reading from socket
            else {
                fprintf(stderr, "Error reading data from socket\n");
                break;
            }
        }
    }
}