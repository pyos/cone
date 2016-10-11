#if __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "mun.h"
#include "cone.h"
#include "nero.h"
#include "deck.h"
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netinet/in.h>

enum
{
    mun_errno_input = mun_errno_custom + 251,
};

static int parse_arg(const char *arg, int server) {
    char *port = strchr(arg, ':');
    if (port == NULL || !port[1])
        return mun_error(input, "`%s`: no port specified", arg);
    *port++ = 0;

    int ret;
    struct addrinfo *addrs = NULL;
    struct addrinfo hints = {};
    hints.ai_flags = server ? AI_PASSIVE : 0;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if ((ret = getaddrinfo(*arg ? arg : NULL, port, &hints, &addrs)))
        return mun_error(input, "`%s`: getaddrinfo: %s", arg, gai_strerror(ret));

    for (struct addrinfo *p = addrs; p; p = p->ai_next) {
        int sk = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sk < 0)
            continue;
        if (!setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int))
         && !(server ? bind(sk, p->ai_addr, p->ai_addrlen) : connect(sk, p->ai_addr, p->ai_addrlen))
         && !cone_unblock(sk))
            return freeaddrinfo(addrs), sk;
        close(sk);
    }
    return freeaddrinfo(addrs), mun_error(input, "`%s`: connection failed", arg);
}

struct node
{
    struct nero rpc;
    struct deck *deck;
    struct cone_event *fail;
};

int write_status(struct deck *lk, int fd) {
    if (!deck_is_acquired_by_this(lk))
        return mun_error(assert, "must be holding the lock to do this");
    if (flock(fd, LOCK_EX | LOCK_NB) MUN_RETHROW_OS)
        return -1;
    dprintf(fd, "%u - %u\n", lk->pid, lk->time);
    return flock(fd, LOCK_UN) MUN_RETHROW_OS;
}

int interface_command(struct node *n, const char *cmd) {
    if (!cmd[0])
        return 0;
    if (!strcmp(cmd, "help"))
        return printf("\033[33;1m # commands\033[0m:\n"
               "    - \033[1mhelp\033[0m ...... show this message;\n"
               "    - \033[1mexit\033[0m ...... teminate the process;\n"
               "    - \033[1mlock\033[0m ...... acquire the Lamport lock;\n"
               "    - \033[1mwrite <F>\033[0m . write current state to file named <F>;\n"
               "    - \033[1munlock\033[0m .... release the Lamport lock.\n"
               "\033[33;1m # notes\033[0m:\n"
               "    - Ctrl+D doesn't work because this program uses async I/O, but\n"
               "      not readline. Use \033[1m> exit\033[0m or \033[1m> quit\033[0m.\n"
               "    - local time (T) is only displayed right after entering a command.\n"
               "      If a message arrives while you decide what to do, the prompt\n"
               "      will \033[5mnot\033[0m update.\n"
               "    - Any operation that does not touch the lock is instantaneous.\n"), 0;
    if (!strcmp(cmd, "exit") || !strcmp(cmd, "quit"))
        return cone_cancel(cone);
    if (!strcmp(cmd, "lock"))
        return deck_acquire(n->deck);
    if (!strcmp(cmd, "unlock"))
        return deck_release(n->deck);
    if (!strncmp(cmd, "write ", 6)) {
        int fd = open(cmd + 6, O_CREAT | O_APPEND | O_WRONLY, 0644);
        if (fd < 0 MUN_RETHROW_OS)
            return -1;
        int ret = write_status(n->deck, fd) MUN_RETHROW_OS;
        return close(fd), ret;
    }
    return mun_error(input, "unknown command");
}

int interface_inner(struct node *n) {
    while (!isatty(1))
        if (deck_acquire(n->deck) || write_status(n->deck, 1) || deck_release(n->deck) MUN_RETHROW)
            return -1;
    if (cone_unblock(0) MUN_RETHROW)
        return -1;
    struct mun_vec(char) buffer = mun_vec_init_static(char, 1024);
    while (1) {
        printf(" T=%u \033[34;1m>\033[0m ", n->deck->time);
        fflush(stdout);
        char *eol;
        do {
            if (buffer.size == 1024)
                return mun_error(input, "command too long");
            ssize_t rd = read(0, buffer.data + buffer.size, 1024 - buffer.size);
            if (rd < 0)
                return errno != ECANCELED MUN_RETHROW_OS;
            buffer.size += rd;
        } while ((eol = memchr(buffer.data, '\n', buffer.size)) == NULL);
        *eol++ = 0;
        if (interface_command(n, buffer.data)) {
            if (mun_last_error()->code == mun_errno_cancelled)
                return 0;
            mun_error_show("command triggered", NULL);
        }
        mun_vec_erase(&buffer, 0, eol - buffer.data);
    }
}

int interface(struct node *n) {
    int ret = interface_inner(n) MUN_RETHROW;
    return (cone_event_emit(n->fail) MUN_RETHROW) ? -1 : ret;
}

int handle_connection(struct node *n) {
    int ret = nero_run(&n->rpc) MUN_RETHROW;
    deck_del(n->deck, &n->rpc);
    return (cone_event_emit(n->fail) MUN_RETHROW) ? -1 : ret;
}

int comain(int argc, const char **argv) {
    struct cone_event fail = {};
    struct deck d = {.pid = getpid(), .name = "flock"};

    if (argc < 3) {
        fprintf(stderr, "\033[33;1m # usage\033[0m: %s N [iface]:port [host:port [...]]\n", argv[0]);
        fprintf(stderr, "          \033[8m%s\033[28m ^  ^            ^---- addresses of previously started nodes\n", argv[0]);
        fprintf(stderr, "          \033[8m%s\033[28m |  \\---- where to bind\n", argv[0]);
        fprintf(stderr, "          \033[8m%s\033[28m \\---- total number of expected nodes\n", argv[0]);
        return mun_error(input, "not enough arguments");
    }
    char *end;
    int n = strtol(argv[1], &end, 10) - 1;
    if (!*argv[1] || *end)
        return mun_error(input, "N must be an integer.");
    int known = argc - 3;
    if (n < known)
        return mun_error(input, "more arguments than nodes in total");

    struct node nodes[n];
    for (int i = 0; i < n; i++)
        nodes[i] = (struct node){{}, &d, &fail};
    for (int i = 0; i < known; i++)
        if ((nodes[i].rpc.fd = parse_arg(argv[i + 3], 0)) < 0 MUN_RETHROW)
            return -1;

    if (n > known) {
        int srv = parse_arg(argv[2], 1);
        if (srv < 0 MUN_RETHROW)
            return -1;
        if (listen(srv, 127) < 0 MUN_RETHROW_OS)
            return -1;
        if (isatty(2))
            fprintf(stderr, "\033[33;1m # main\033[0m: awaiting %d more connection(s)\n", n - known);
        for (; n > known; known++)
            if ((nodes[known].rpc.fd = accept(srv, NULL, NULL)) < 0 MUN_RETHROW_OS)
                return -1;
    }
    for (int i = 0; i < known; i++)
        if (deck_add(&d, &nodes[i].rpc) MUN_RETHROW)
            return -1;

    struct cone *children[known + 1];
    memset(children, 0, sizeof(struct cone *) * (known + 1));
    int ret = 0;
    for (int i = 0; i < known; i++) {
        if ((children[i] = cone(&handle_connection, &nodes[i])) == NULL) {
            mun_error_show("couldn't start coroutine due to", NULL), ret = 1;
            goto failall;
        }
    }
    struct node fake = {{}, &d, &fail};
    if ((children[known] = cone(&interface, &fake)) == NULL)
        mun_error_show("interface failed to start due to", NULL), ret = 1;
    else
        cone_wait(&fail);
failall:
    for (int i = known + 1; i--;) {
        if (children[i]) {
            cone_cancel(children[i]);
            if (cone_join(children[i]) && mun_last_error()->code != mun_errno_cancelled)
                mun_error_show("main caught", NULL), ret = 1;
        }
    }
    deck_fini(&d);
    for (int i = 0; i < known; i++)
        nero_fini(&nodes[i].rpc);
    return ret;
}
