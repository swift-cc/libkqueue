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


static const char *
epoll_event_dump(struct epoll_event *evt)
{
//     static char __thread buf[128];

//     if (evt == NULL)
//         return "(null)";

// #define EPEVT_DUMP(attrib) \
//     if (evt->events & attrib) \
//        strcat(&buf[0], #attrib" ");

//     snprintf(&buf[0], 128, " { data = %p, events = ", evt->data.ptr);
//     EPEVT_DUMP(EPOLLIN);
//     EPEVT_DUMP(EPOLLOUT);
// #if defined(HAVE_EPOLLRDHUP)
//     EPEVT_DUMP(EPOLLRDHUP);
// #endif
//     EPEVT_DUMP(EPOLLONESHOT);
//     EPEVT_DUMP(EPOLLET);
//     strcat(&buf[0], "}\n");

//     return (&buf[0]);
// #undef EPEVT_DUMP
    return "";
}

static int
epoll_update(int op, struct filter *filt, struct knote *kn, struct epoll_event *ev)
{
    dbg_printf("op=%d fd=%d events=%s", op, (int)kn->kev.ident, 
            epoll_event_dump(ev));
    if (epoll_ctl(filt->kf_pfd, op, kn->kev.ident, ev) < 0) {
        dbg_printf("epoll_ctl(2): %s", strerror(errno));
        return (-1);
    }

    return (0);
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
                return (-EINTR);
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
#if defined(HAVE_EPOLLRDHUP)
            if (ev->events & EPOLLRDHUP || ev->events & EPOLLHUP)
                dst->flags |= EV_EOF;
#else
            if (ev->events & EPOLLHUP)
                dst->flags |= EV_EOF;
#endif
            if (ev->events & EPOLLERR)
                dst->fflags = 1; /* FIXME: Return the actual socket error */
          
            if (kn->flags & KNFL_PASSIVE_SOCKET) {
                /* On return, data contains the length of the 
                   socket backlog. This is not available under Linux.
                 */
                dst->data = 1;
            } else {
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
            }

            if (kn->kev.flags & EV_DISPATCH) {
                socket_knote_delete(filt->kf_pfd, kn->kev.ident);
                KNOTE_DISABLE(kn);
            } else if (kn->kev.flags & EV_ONESHOT) {
                socket_knote_delete(filt->kf_pfd, kn->kev.ident);
                knote_free(filt, kn);
            }

            nevents++;
            dst++;
        }
    }

    return (nevents);
}

int
evfilt_socket_knote_create(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;

    if (knote_get_socket_type(kn) < 0)
        return (-1);

    /* Convert the kevent into an epoll_event */
    if (kn->kev.filter == EVFILT_READ)
#if defined(HAVE_EPOLLRDHUP)
        kn->data.events = EPOLLIN | EPOLLRDHUP;
#else
        kn->data.events = EPOLLIN;
#endif
    else
        kn->data.events = EPOLLOUT;
    if (kn->kev.flags & EV_ONESHOT || kn->kev.flags & EV_DISPATCH)
        kn->data.events |= EPOLLONESHOT;
    if (kn->kev.flags & EV_CLEAR)
        kn->data.events |= EPOLLET;

    memset(&ev, 0, sizeof(ev));
    ev.events = kn->data.events;
    ev.data.fd = kn->kev.ident;

    return epoll_update(EPOLL_CTL_ADD, filt, kn, &ev);
}

int
evfilt_socket_knote_modify(struct filter *filt, struct knote *kn, 
        const struct kevent *kev)
{
    return (-1); /* STUB */
}

int
evfilt_socket_knote_delete(struct filter *filt, struct knote *kn)
{
    if (kn->kev.flags & EV_DISABLE)
        return (0);
    else
        return epoll_update(EPOLL_CTL_DEL, filt, kn, NULL);
}

int
evfilt_socket_knote_enable(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.events = kn->data.events;
    ev.data.fd = kn->kev.ident;

    return epoll_update(EPOLL_CTL_ADD, filt, kn, &ev);
}

int
evfilt_socket_knote_disable(struct filter *filt, struct knote *kn)
{
    return epoll_update(EPOLL_CTL_DEL, filt, kn, NULL);
}


const struct filter evfilt_read = {
    EVFILT_READ,
    evfilt_socket_init,
    evfilt_socket_destroy,
    evfilt_socket_copyout,
    evfilt_socket_knote_create,
    evfilt_socket_knote_modify,
    evfilt_socket_knote_delete,
    evfilt_socket_knote_enable,
    evfilt_socket_knote_disable,         
};

const struct filter evfilt_write = {
    EVFILT_WRITE,
    evfilt_socket_init,
    evfilt_socket_destroy,
    evfilt_socket_copyout,
    evfilt_socket_knote_create,
    evfilt_socket_knote_modify,
    evfilt_socket_knote_delete,
    evfilt_socket_knote_enable,
    evfilt_socket_knote_disable,         
};
