#include "mun.h"
#include "cone.h"
#include "nero.h"
#include "deck.h"
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

enum
{
    mun_errno_input = mun_errno_os | EINVAL,
};

static int parse_arg(const char *arg, int (*connector)(int, const struct sockaddr *, socklen_t)) {
    char *port = strchr(arg, ':');
    if (port == NULL || !port[1])
        return mun_error(input, "`%s`: no port specified", arg);
    *port++ = 0;

    int ret;
    struct addrinfo *addrs = NULL;
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if ((ret = getaddrinfo(arg, port, &hints, &addrs)))
        return mun_error(input, "`%s`: getaddrinfo: %s", arg, gai_strerror(ret));

    for (struct addrinfo *p = addrs; p; p = p->ai_next) {
        int sk = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sk < 0)
            continue;
        if (setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int)) < 0
         || connector(sk, p->ai_addr, p->ai_addrlen) < 0) {
            close(sk);
            continue;
        }
        if (cone_unblock(sk)) {
            close(sk);
            freeaddrinfo(addrs);
            return mun_error_up();
        }
        freeaddrinfo(addrs);
        return sk;
    }
    freeaddrinfo(addrs);
    return mun_error(input, "`%s`: connection failed", arg);
}

struct node
{
    int socket;
    struct deck *deck;
    struct cone_event *fail;
};

int handle_connection_inner(struct node *n) {
    struct nero rpc = {.fd = n->socket};
    if (deck_add(n->deck, &rpc))
        return nero_fini(&rpc), mun_error_up();
    if (nero_run(&rpc))
        return deck_del(n->deck, &rpc), nero_fini(&rpc), mun_error_up();
    return deck_del(n->deck, &rpc), nero_fini(&rpc), mun_ok;
}

int interface_command(struct node *n, const char *cmd) {
    if (!cmd[0])
        return mun_ok;
    if (!strcmp(cmd, "help"))
        return printf("\033[33;1m # commands\033[0m:\n"
               "    - \033[1mhelp\033[0m ...... show this message;\n"
               "    - \033[1mexit\033[0m ...... teminate the process;\n"
               "    - \033[1mlock\033[0m ...... acquire the Lamport lock;\n"
               "    - \033[1mwrite <F>\033[0m . write current state to file named F;\n"
               "    - \033[1munlock\033[0m .... release the Lamport lock.\n"
               "\033[33;1m # notes\033[0m:\n"
               "    - Ctrl+D doesn't work because this program uses async I/O, but\n"
               "      not readline. Use \033[1m> exit\033[0m or \033[1m> quit\033[0m.\n"
               "    - local time (T) is only displayed right after entering a command.\n"
               "      If a message arrives while you decide what to do, the prompt\n"
               "      will \033[5mnot\033[0m update.\n"
               "    - Any operation that does not touch the lock is instantaneous.\n"), mun_ok;
    if (!strcmp(cmd, "exit") || !strcmp(cmd, "quit"))
        return cone_cancel(cone);
    if (!strcmp(cmd, "lock"))
        return deck_acquire(n->deck);
    if (!strcmp(cmd, "unlock"))
        return deck_release(n->deck);
    return mun_error(input, "unknown command");
}

int interface_inner(struct node *n) {
    if (cone_unblock(0))
        return mun_error_up();
    char stkbuf[1024];
    struct mun_vec(char) buffer = mun_vec_init_static(char, 1024);
    int prompt = 1;
    while (1) {
        if (prompt) {
            prompt = 0;
            printf(" T=%u \033[34;1m>\033[0m ", n->deck->time);
            fflush(stdout);
        }
        ssize_t rd = read(0, stkbuf, sizeof(stkbuf));
        if (rd < 0)
            return mun_error_os();
        if (mun_vec_extend(&buffer, stkbuf, rd))
            return mun_error_up();
        char *eol = memchr(buffer.data, '\n', buffer.size);
        if (eol == NULL)
            continue;
        *eol++ = 0;
        if (interface_command(n, buffer.data)) {
            if (mun_last_error()->code == mun_errno_cancelled)
                break;
            mun_error_show("command triggered", NULL);
        }
        mun_vec_erase(&buffer, 0, eol - buffer.data);
        prompt = 1;
    }
    return mun_ok;
}

int handle_connection(struct node *n) {
    int ret = handle_connection_inner(n);
    if (cone_event_emit(n->fail))
        return mun_error_up();
    return ret;
}

int interface(struct node *n) {
    int ret = interface_inner(n);
    if (cone_event_emit(n->fail))
        return mun_error_up();
    return ret;
}

int comain(int argc, const char **argv) {
    struct cone_event fail = {};
    struct deck d = {.pid = getpid(), .name = "flock"};

    if (argc < 3) {
        fprintf(stderr, "\033[33;1m # usage\033[0m: %s N :port [host:port [...]]\n", argv[0]);
        fprintf(stderr, "          \033[8m%s\033[28m ^  ^     ^---- addresses of previously started nodes\n", argv[0]);
        fprintf(stderr, "          \033[8m%s\033[28m |  \\---- where to bind\n", argv[0]);
        fprintf(stderr, "          \033[8m%s\033[28m \\---- total number of expected nodes\n", argv[0]);
        return 2;
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
        nodes[i] = (struct node){-1, &d, &fail};
    for (int i = 0; i < known; i++)
        if ((nodes[i].socket = parse_arg(argv[i + 3], &connect)) < 0)
            return mun_error_up();

    if (n > known) {
        int srv = parse_arg(argv[2], &bind);
        if (srv < 0)
            return mun_error_up();
        if (listen(srv, 127) < 0)
            return mun_error_os();
        while (n > known) {
            fprintf(stderr, "\033[33;1m # main\033[0m: awaiting %d more connection(s) on %s\n", n - known, argv[2]);
            if ((nodes[known].socket = accept(srv, NULL, NULL)) < 0)
                return mun_error_os();
            known++;
        }
    }

    struct cone *children[known + 1];
    memset(children, 0, sizeof(struct cone *) * (known + 1));
    int ret = 0;
    for (int i = 0; i < known; i++) {
        if ((children[i] = cone(&handle_connection, &nodes[i])) == NULL) {
            mun_error_show("couldn't start coroutine due to", NULL), ret = 1;
            goto failall;
        }
    }
    struct node fake = {0, &d, &fail};
    if ((children[known] = cone(&interface, &fake)) == NULL)
        mun_error_show("interface failed to start due to", NULL), ret = 1;
    else
        cone_wait(&fail);
failall:
    for (int i = 0; i < known + 1; i++) {
        if (children[i]) {
            cone_cancel(children[i]);
            if (cone_join(children[i]) && mun_last_error()->code != mun_errno_cancelled)
                mun_error_show("main caught", NULL), ret = 1;
        }
    }
    deck_fini(&d);
    return ret;
}
