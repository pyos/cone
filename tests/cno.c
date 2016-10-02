#include "../libco/coro.h"

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

static void sigint(int signum) {
    (void)signum;
    signal(SIGINT, SIG_DFL);
    shutdown(fd, SHUT_RDWR);
}

struct cno_data_t {
    struct cno_connection_t conn;
    int fd;
};

static int writeall(struct cno_data_t *d, const char *data, size_t size) {
    while (size) {
        ssize_t w = write(d->fd, data, size);
        if (w <= 0)
            return CNO_ERROR(TRANSPORT, "could not write to socket");
        data += w;
        size -= w;
    }
    return CNO_OK;
}

static int sendhello(struct cno_data_t *d, uint32_t stream, const struct cno_message_t *rmsg) {
    (void)rmsg;
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
    struct cno_data_t d = {.fd = client};
    cno_connection_init(&d.conn, CNO_SERVER);
    d.conn.cb_data = &d;
    d.conn.on_write = (int(*)(void*, const char*, size_t)) &writeall;
    d.conn.on_message_start = (int(*)(void*, uint32_t, const struct cno_message_t*)) &sendhello;
    if (cno_connection_made(&d.conn, CNO_HTTP1))
        goto error;
    ssize_t rd;
    char data[4096];
    while ((rd = read(client, data, sizeof(data))))
        if (rd < 0 || cno_connection_data_received(&d.conn, data, (size_t) rd))
            goto error;
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

    const int one = 1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));
    struct sockaddr_in servaddr = {};
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);
    servaddr.sin_port = htons(8000);
    bind(fd, (struct sockaddr *) &servaddr, sizeof(servaddr));
    listen(fd, 127);
    int client;
    while ((client = accept(fd, NULL, NULL)) >= 0 || errno == ECONNABORTED)
        coro_decref(coro(&handle_connection, (void*)client));
    if (errno != EINVAL)
        perror("accept");
    close(fd);
    return 0;
}
