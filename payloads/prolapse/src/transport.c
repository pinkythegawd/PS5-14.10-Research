#include "prolapse.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int set_linger(int fd, int onoff, int seconds, char *why,
                      size_t why_len)
{
    struct linger lg;
    lg.l_onoff = onoff;
    lg.l_linger = seconds;
    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) != 0) {
        snprintf(why, why_len, "setsockopt(SO_LINGER {%d,%d}) failed: %s",
                 onoff, seconds, strerror(errno));
        return -1;
    }
    return 0;
}

static int open_tcp(prolapse_transport_t *t, char *why, size_t why_len)
{
    int listener = -1, client = -1, server = -1;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int one = 1;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        snprintf(why, why_len, "socket(AF_INET) failed: %s", strerror(errno));
        return -1;
    }
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(why, why_len, "bind(127.0.0.1:0) failed: %s",
                 strerror(errno));
        goto fail;
    }
    if (listen(listener, 1) != 0) {
        snprintf(why, why_len, "listen failed: %s", strerror(errno));
        goto fail;
    }
    if (getsockname(listener, (struct sockaddr *)&addr, &addr_len) != 0) {
        snprintf(why, why_len, "getsockname failed: %s", strerror(errno));
        goto fail;
    }
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0) {
        snprintf(why, why_len, "client socket failed: %s", strerror(errno));
        goto fail;
    }
    if (connect(client, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(why, why_len, "connect loopback failed: %s", strerror(errno));
        goto fail;
    }
    server = accept(listener, NULL, NULL);
    if (server < 0) {
        snprintf(why, why_len, "accept loopback failed: %s", strerror(errno));
        goto fail;
    }
    close(listener);
    t->target = client;
    t->peer = server;
    return 0;

fail:
    if (listener >= 0) close(listener);
    if (client >= 0) close(client);
    if (server >= 0) close(server);
    return -1;
}

int prolapse_transport_open(prolapse_transport_t *t, const char *kind,
                            int linger_onoff, int linger_seconds,
                            char *why, size_t why_len)
{
    int sv[2];
    if (!t || !kind)
        return -1;
    memset(t, 0, sizeof(*t));
    t->target = -1;
    t->peer = -1;
    t->linger_onoff = linger_onoff;
    t->linger_seconds = linger_seconds;

    if (strcmp(kind, "tcp_loopback") == 0) {
        if (open_tcp(t, why, why_len) != 0)
            return -1;
    } else if (strcmp(kind, "unix_socketpair") == 0) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            snprintf(why, why_len, "socketpair failed: %s", strerror(errno));
            return -1;
        }
        t->target = sv[0];
        t->peer = sv[1];
    } else {
        snprintf(why, why_len, "unknown transport '%s'", kind);
        return -1;
    }

    if (set_linger(t->target, linger_onoff, linger_seconds, why, why_len) != 0) {
        prolapse_transport_close(t);
        return -1;
    }
    return 0;
}

void prolapse_transport_close(prolapse_transport_t *t)
{
    if (!t)
        return;
    if (t->target >= 0) {
        close(t->target);
        t->target = -1;
    }
    if (t->peer >= 0) {
        close(t->peer);
        t->peer = -1;
    }
}

int prolapse_transport_release_peer(int *peer_fd, char *why, size_t why_len)
{
    if (!peer_fd || *peer_fd < 0) {
        snprintf(why, why_len, "release has no live peer fd");
        return -1;
    }
    if (set_linger(*peer_fd, 0, 0, why, why_len) != 0) {
        snprintf(why, why_len,
                 "release could not set peer SO_LINGER {0,0}: %s",
                 strerror(errno));
        return -1;
    }
    (void)shutdown(*peer_fd, SHUT_RDWR);
    if (close(*peer_fd) != 0) {
        snprintf(why, why_len, "release close(peer) failed: %s",
                 strerror(errno));
        return -1;
    }
    *peer_fd = -1;
    return 0;
}

int prolapse_transport_release(prolapse_transport_t *t, char *why,
                               size_t why_len)
{
    int ok = 0;
    if (!t)
        return -1;
    /*
     * The AIO request owns the target fd after submission; userland closes
     * its copy.  The peer is the only live handle.  Set {0,0} on it, then
     * shut it down and close it.  This is the on-demand release path.  If the
     * target copy is somehow still open, put {0,0} there too.
     */
    if (t->target >= 0) {
        (void)set_linger(t->target, 0, 0, why, why_len);
        (void)shutdown(t->target, SHUT_RDWR);
        close(t->target);
        t->target = -1;
    }
    if (t->peer >= 0) {
        if (prolapse_transport_release_peer(&t->peer, why, why_len) != 0)
            return -1;
        ok = 1;
    }
    if (!ok) {
        snprintf(why, why_len, "release has no live peer fd");
        return -1;
    }
    return 0;
}
