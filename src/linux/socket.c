/*
 * Copyright (c) 2009 Mark Heily <mark@heily.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/sockios.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>

#include "sys/event.h"
#include "private.h"


char *
epoll_event_dump(struct epoll_event *evt)
{

#define EPEVT_DUMP(attrib) \
    if (evt->events & attrib) \
       fputs(#attrib" ", stdout);

    fprintf(stdout, " { data = %p, events = ", evt->data.ptr);
    EPEVT_DUMP(EPOLLIN);
    EPEVT_DUMP(EPOLLOUT);
    EPEVT_DUMP(EPOLLRDHUP);
    EPEVT_DUMP(EPOLLONESHOT);
    EPEVT_DUMP(EPOLLET);
    fputs("}\n", stdout);
    fflush(stdout);

    return "";  //XXX-BROKEN
}

static int 
socket_knote_delete(int epfd, int fd)
{
    return epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}

int
evfilt_socket_init(struct filter *filt)
{
    filt->kf_pfd = epoll_create(1);
    if (filt->kf_pfd < 0)
        return (-1);

    dbg_printf("socket epollfd = %d", filt->kf_pfd);
    return (0);
}

void
evfilt_socket_destroy(struct filter *filt)
{
    close(filt->kf_pfd);
}

int
evfilt_socket_copyin(struct filter *filt, 
        struct knote *dst, const struct kevent *src)
{
    struct epoll_event ev;
    int op, rv;

    /* Determine which operation to perform */
    if (src->flags & EV_ADD && KNOTE_EMPTY(dst)) {
        op = EPOLL_CTL_ADD;
        memcpy(&dst->kev, src, sizeof(*src));
    }
    memset(&ev, 0, sizeof(ev));
    if (src->flags & EV_DELETE) 
        op = EPOLL_CTL_DEL;
    if (src->flags & EV_ENABLE || src->flags & EV_DISABLE) 
        op = EPOLL_CTL_MOD;
    // FIXME: probably won't work with EV_ADD | EV_DISABLE 
    // XXX-FIXME: need to update dst for delete/modify

    /* Convert the kevent into an epoll_event */
    if (src->filter == EVFILT_READ)
        ev.events = EPOLLIN | EPOLLRDHUP;
    else
        ev.events = EPOLLOUT;
    if (src->flags & EV_ONESHOT || src->flags & EV_DISPATCH)
        ev.events |= EPOLLONESHOT;
    if (src->flags & EV_CLEAR)
        ev.events |= EPOLLET;
    ev.data.fd = src->ident;

    if (src->flags & EV_DISABLE) 
        ev.events = 0;

    /* Set the low water mark */
    /* PORTABILITY -
       BSD kqueue(2) NOTE_LOWAT does not modify the underlying
       socket.

       It appears that epoll(2) does not respect the SO_RCVLOWAT
       setting.

     */
#if LINUX_SUPPORTS_SO_RCVLOWAT
    if (src->fflags & NOTE_LOWAT) {
        const socklen_t optlen = sizeof(int);
        int sockopt;

        sockopt = src->data;
        if (setsockopt(src->ident, SOL_SOCKET, SO_RCVLOWAT, &sockopt, optlen) < 0) {
            dbg_printf("setsockopt(2): %s", strerror(errno));
            return (-1);
        }
        dbg_printf("Low watermark set to %d", sockopt);
    }
#endif

    dbg_printf("epoll_ctl(2): epfd=%d, op=%d, fd=%d event=%s", 
            filt->kf_pfd, 
            op, 
            (int)src->ident, 
            epoll_event_dump(&ev)
            );
    rv = epoll_ctl(filt->kf_pfd, op, src->ident, &ev);
    if (rv < 0) {
        dbg_printf("epoll_ctl(2): %s", strerror(errno));
        return (-1);
    }

    return (0);
}

int
evfilt_socket_copyout(struct filter *filt, 
            struct kevent *dst, 
            int nevents)
{
    struct epoll_event epevt[MAX_KEVENT];
    struct epoll_event *ev;
    struct knote *kn;
    int i, nret;

    for (;;) {
        nret = epoll_wait(filt->kf_pfd, &epevt[0], nevents, 0);
        if (nret < 0) {
            if (errno == EINTR)
                continue;
            dbg_perror("epoll_wait");
            return (-1);
        } else {
            break;
        }
    }

    for (i = 0, nevents = 0; i < nret; i++) {
        ev = &epevt[i];
        epoll_event_dump(ev);
        kn = knote_lookup(filt, ev->data.fd);
        if (kn != NULL) {
            memcpy(dst, &kn->kev, sizeof(*dst));
            if (ev->events & EPOLLRDHUP || ev->events & EPOLLHUP)
                dst->flags |= EV_EOF;
            if (ev->events & EPOLLERR)
                dst->fflags = 1; /* FIXME: Return the actual socket error */
          
            /* On return, data contains the number of bytes of protocol
               data available to read.
             */
            if (ioctl(dst->ident, 
                        (dst->filter == EVFILT_READ) ? SIOCINQ : SIOCOUTQ, 
                        &dst->data) < 0) {
                /* race condition with socket close, so ignore this error */
                dbg_puts("ioctl(2) of socket failed");
                dst->data = 0;
            }

            if (kn->kev.flags & EV_DISPATCH) 
                KNOTE_DISABLE(kn);
            if (kn->kev.flags & EV_ONESHOT) {
                socket_knote_delete(filt->kf_pfd, kn->kev.ident);
                knote_free(kn);
            }

            nevents++;
            dst++;
        }
    }

    return (nevents);
}

const struct filter evfilt_read = {
    EVFILT_READ,
    evfilt_socket_init,
    evfilt_socket_destroy,
    evfilt_socket_copyin,
    evfilt_socket_copyout,
};

const struct filter evfilt_write = {
    EVFILT_WRITE,
    evfilt_socket_init,
    evfilt_socket_destroy,
    evfilt_socket_copyin,
    evfilt_socket_copyout,
};