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

#include <sys/types.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>

#include "common.h"

struct unit_test {
    const char *ut_name;
    int         ut_enabled;
    void      (*ut_func)(int);
};

/*
 * Test the method for detecting when one end of a socketpair 
 * has been closed. This technique is used in kqueue_validate()
 */
static void
test_peer_close_detection(void)
{
    int sockfd[2];
    char buf[1];
    struct pollfd pfd;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockfd) < 0)
        die("socketpair");

    pfd.fd = sockfd[0];
    pfd.events = POLLIN | POLLHUP;
    pfd.revents = 0;

    if (poll(&pfd, 1, 0) > 0) 
        die("unexpected data");

    if (close(sockfd[1]) < 0)
        die("close");

    if (poll(&pfd, 1, 0) > 0) {
        if (recv(sockfd[0], buf, sizeof(buf), MSG_PEEK | MSG_DONTWAIT) != 0) 
            die("failed to detect peer shutdown");
    }
}

void
test_kqueue(void)
{
    int kqfd;

    if ((kqfd = kqueue()) < 0)
        die("kqueue()");
    test_no_kevents(kqfd);
    if (close(kqfd) < 0)
        die("close()");
}

/*
 * Verify that a kqueue descriptor can be monitored for readiness
 * with pselect().
 */
void
test_kqueue_pselect(void)
{
    int kq;
    struct kevent kev1, kev2;
    fd_set rfds;
    struct timespec ts = { 60, 0 };

    puts("    TODO -- not implemented yet");
    return;

    if ((kq = kqueue()) < 0)
        die("kqueue()");
    test_no_kevents(kq);

    kevent_add(kq, &kev1, 2, EVFILT_TIMER, EV_ADD | EV_ONESHOT | EV_CLEAR, 0, 2000, NULL);

    FD_ZERO(&rfds);
    FD_SET(kq, &rfds);
    if (pselect(kq + 1, &rfds, NULL, NULL, &ts, NULL) < 1) {
        die("pselect() timed out or returned an error");
    }

    (void)kevent(kq, NULL, 0, &kev2, 1, &ts);
    kev1.data = kev2.data; /* Ignore this field */
    kevent_cmp(&kev1, &kev2);
    test_no_kevents(kq);
}

void
test_kevent(void)
{
    struct kevent kev;

    memset(&kev, 0, sizeof(kev));

    /* Provide an invalid kqueue descriptor */
    if (kevent(-1, &kev, 1, NULL, 0, NULL) == 0)
        die("invalid kq parameter");
}


void
test_ev_receipt(void)
{
    int kq;
    struct kevent kev;

    if ((kq = kqueue()) < 0)
        die("kqueue()");
#if HAVE_EV_RECEIPT

    EV_SET(&kev, SIGUSR2, EVFILT_SIGNAL, EV_ADD | EV_RECEIPT, 0, 0, NULL);
    if (kevent(kq, &kev, 1, &kev, 1, NULL) < 0)
        die("kevent");

    /* TODO: check the receipt */

    close(kq);
#else
    memset(&kev, 0, sizeof(kev));
    puts("Skipped -- EV_RECEIPT is not available");
#endif
}

#ifndef __ANDROID__
static void
test_cancel_state_unchanged(void)
{
    int kq, rc, state;
    struct timespec ts = { 0, 1000 };
    struct kevent kev;

    if ((rc = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL)) != 0)
        err(rc, "pthread_setcancelstate");

    if ((kq = kqueue()) < 0)
        die("kqueue()");

    if ((rc = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &state)) != 0)
        err(rc, "pthread_setcancelstate");
    if (state != PTHREAD_CANCEL_ENABLE)
        die("kqueue() changed cancel state");

    if ((rc = kevent(kq, NULL, 0, &kev, 1, &ts)) != 0)
        err(rc, "kevent");

    if ((rc = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &state)) != 0)
        err(rc, "pthread_setcancelstate");
    if (state != PTHREAD_CANCEL_ENABLE)
        die("kevent() changed cancel state");

    close(kq);
}


static void *
thr_cancel_enabled(void *arg)
{
    fd_set rfds;
    int *kq = arg;
    struct kevent kev;
    struct timespec ts = { 60, 0 };

    FD_ZERO(&rfds);
    FD_SET(*kq, &rfds);
    (void)pselect(1, &rfds, NULL, NULL, &ts, NULL);
    die("should never get here due to cancel");

    /* NOTREACHED - but for example purposes */
    memset(&ts, 0, sizeof(ts));
    (void)kevent(*kq, NULL, 0, &kev, 1, &ts);

    return NULL;
}

static void
test_cancel_enabled(void)
{
    int kq, rc;
    pthread_t thr;
    void *retval = NULL;
    time_t cancelled_at;

    if ((kq = kqueue()) < 0)
        die("kqueue()");

    if ((rc = pthread_create(&thr, NULL, thr_cancel_enabled, &kq)) != 0)
        err(rc, "pthread_create");

    cancelled_at = time(NULL);
    if ((rc = pthread_cancel(thr)) != 0)
        err(rc, "pthread_cancel");
    if ((rc = pthread_join(thr, &retval)) != 0)
        err(rc, "pthread_join");
    if (retval != PTHREAD_CANCELED)
        die("thread not cancelled");

    if ((time(NULL) - cancelled_at) > 5)
        die("cancellation took too long");

    close(kq);
}

static void *
thr_cancel_disabled(void *arg)
{
    int *kq = arg;
    struct kevent kev;
    struct timespec ts = { 1, 0 };
    int rc, state;

    if ((rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL)) != 0)
        err(rc, "pthread_setcancelstate");

    if ((rc = kevent(*kq, NULL, 0, &kev, 1, &ts)) != 0)
        err(rc, "kevent");

    if ((rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &state)) != 0)
        err(rc, "pthread_setcancelstate");

    if (state != PTHREAD_CANCEL_DISABLE)
        die("kevent() didn't preserve pthread cancel state");

    return NULL; /* success */
}

static void
test_cancel_disabled(void)
{
    int kq, rc;
    pthread_t thr;
    void *retval = NULL;

    if ((kq = kqueue()) < 0)
        die("kqueue()");

    if ((rc = pthread_create(&thr, NULL, thr_cancel_disabled, &kq)) != 0)
        err(rc, "pthread_create");
    if ((rc = pthread_cancel(thr)) != 0)
        err(rc, "pthread_cancel");
    if ((rc = pthread_join(thr, &retval)) != 0)
        err(rc, "pthread_join");
    if (retval != NULL)
        die("thread not cancelled");

    close(kq);
}
#endif /* ! __ANDROID__ */

int 
main(int argc, char **argv)
{
    struct unit_test tests[] = {
        { "socket", 1, test_evfilt_read },
        { "signal", 1, test_evfilt_signal },
#if FIXME
        { "proc", 1, test_evfilt_proc },
#endif
        { "vnode", 1, test_evfilt_vnode },
        { "timer", 1, test_evfilt_timer },
#if HAVE_EVFILT_USER
        { "user", 1, test_evfilt_user },
#endif
        { NULL, 0, NULL },
    };
    struct unit_test *test;
    char *arg;
    int match, kqfd;

    /* If specific tests are requested, disable all tests by default */
    if (argc > 1) {
        for (test = &tests[0]; test->ut_name != NULL; test++) {
            test->ut_enabled = 0;
        }
    }

    while (argc > 1) {
        match = 0;
        arg = argv[1];
        for (test = &tests[0]; test->ut_name != NULL; test++) {
            if (strcmp(arg, test->ut_name) == 0) {
                test->ut_enabled = 1;
                match = 1;
                break;
            }
        }
        if (!match) {
            printf("ERROR: invalid option: %s\n", arg);
            exit(1);
        } else {
            printf("enabled test: %s\n", arg);
        }
        argv++;
        argc--;
    }

    testing_begin();

    test(peer_close_detection);

    test(kqueue);
    test(kqueue_pselect);
    test(kevent);
#ifndef __ANDROID__
    test(cancel_state_unchanged);
    test(cancel_enabled);
    test(cancel_disabled);
#endif

    if ((kqfd = kqueue()) < 0)
        die("kqueue()");

    for (test = &tests[0]; test->ut_name != NULL; test++) {
        if (test->ut_enabled)
            test->ut_func(kqfd);
    }

    test(ev_receipt);

    testing_end();

    return (0);
}
