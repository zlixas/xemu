/*
 * Sleeping instead of spinning in guest polling loops (xemu)
 *
 * Games commonly spin on a memory word that the GPU updates, e.g. waiting for
 * a push buffer fence. Emulated, that burns a host core that the GPU thread
 * could use. The translator detects such loops (see helper_xbox_poll) and they
 * sleep here until device emulation reports progress.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/xbox-poll.h"

static pthread_mutex_t poll_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t poll_cond = PTHREAD_COND_INITIALIZER;
static uint64_t poll_seq;
static int poll_waiters;

void xbox_poll_notify(void)
{
    qatomic_inc(&poll_seq);
    smp_mb();
    if (qatomic_read(&poll_waiters)) {
        pthread_mutex_lock(&poll_lock);
        pthread_cond_broadcast(&poll_cond);
        pthread_mutex_unlock(&poll_lock);
    }
}

void xbox_poll_wait(int64_t timeout_ns)
{
    uint64_t seq = qatomic_read(&poll_seq);

    pthread_mutex_lock(&poll_lock);
    qatomic_inc(&poll_waiters);
    smp_mb();
    if (qatomic_read(&poll_seq) == seq) {
#ifdef __APPLE__
        struct timespec ts = { .tv_sec = timeout_ns / 1000000000,
                               .tv_nsec = timeout_ns % 1000000000 };
        pthread_cond_timedwait_relative_np(&poll_cond, &poll_lock, &ts);
#else
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        int64_t ns = ts.tv_nsec + timeout_ns;
        ts.tv_sec += ns / 1000000000;
        ts.tv_nsec = ns % 1000000000;
        pthread_cond_timedwait(&poll_cond, &poll_lock, &ts);
#endif
    }
    qatomic_dec(&poll_waiters);
    pthread_mutex_unlock(&poll_lock);
}
