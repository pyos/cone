#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <libco/coro.h>

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <cno/core.h>

static int fd;
static int one = 1;


static void sigint(int signum) {
    (void)signum;
    signal(SIGINT, SIG_DFL);
    shutdown(fd, SHUT_RDWR);
}


struct cno_data_t {
    struct cno_connection_t conn;
    int fd;
};


static int writeall(void *dptr, const char *data, size_t size) {
    struct cno_data_t* d = (struct cno_data_t*) dptr;
    while (size) {
        ssize_t w = write(d->fd, data, size);
        if (w <= 0)
            return CNO_ERROR(TRANSPORT, "could not write to socket");
        data += w;
        size -= w;
    }
    return CNO_OK;
}


static int sendhello(void *dptr, uint32_t stream, const struct cno_message_t *rmsg) {
    (void)rmsg;
    struct cno_data_t* d = (struct cno_data_t*) dptr;
    struct cno_header_t headers[] = {
        { CNO_BUFFER_STRING("server"), CNO_BUFFER_STRING("libcno/1"), 0 },
    };
    struct cno_message_t msg = { 200, CNO_BUFFER_EMPTY, CNO_BUFFER_EMPTY, headers, 0 };
    if (cno_write_message(&d->conn, stream, &msg, 0))
        return CNO_ERROR_UP();
    if (cno_write_data(&d->conn, stream, "Hello, World!\n", 14, 1) != 14)
        return CNO_ERROR(TRANSPORT, "could not send 14 bytes");
    return CNO_OK;
}


static int handle_connection(int client) {
    struct cno_data_t d;
    cno_connection_init(&d.conn, CNO_SERVER);
    d.fd = client;
    d.conn.cb_data = &d;
    d.conn.on_write = &writeall;
    d.conn.on_message_start = &sendhello;
    if (cno_connection_made(&d.conn, CNO_HTTP1))
        goto error;

    ssize_t rd;
    char data[4096];
    while ((rd = read(client, data, sizeof(data)))) {
        if (rd < 0)
            break;
        if (cno_connection_data_received(&d.conn, data, (size_t) rd))
            goto error;
    }

    if (cno_connection_lost(&d.conn))
        goto error;

error:
    cno_connection_reset(&d.conn);
    close(client);
    return 0;
}

int amain() {
    signal(SIGINT, &sigint);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));
    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);
    servaddr.sin_port = htons(8000);
    bind(fd, (struct sockaddr *) &servaddr, sizeof(servaddr));
    listen(fd, 127);

    while (1) {
        int client = accept(fd, (struct sockaddr*) NULL, NULL);
        if (client < 0) {
            if (errno == ECONNABORTED)
                continue;
            if (errno != EINVAL)
                perror("accept");
            break;
        }
        coro_decref(coro(&handle_connection, (void*)client));
    }

    close(fd);
    return 0;
}
