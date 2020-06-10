/*
 * Copyright (c) 2020 Red Hat Inc
 * Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013, 2014 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "openvswitch/poll-loop.h"
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include "coverage.h"
#include "openvswitch/dynamic-string.h"
#include "fatal-signal.h"
#include "openvswitch/list.h"
#include "ovs-thread.h"
#include "seq.h"
#include "socket-util.h"
#include "timeval.h"
#include "openvswitch/vlog.h"
#include "openvswitch/hmap.h"
#include "hash.h"

VLOG_DEFINE_THIS_MODULE(poll_loop);

COVERAGE_DEFINE(poll_create_node);
COVERAGE_DEFINE(poll_zero_timeout);

#define POLLFD_INCREMENT 16;

/* The poll_node structures are used solely as metadata for
 * the pollfd array associated with the loop. That pollfd
 * array is persistent and does not need to be regenerated
 * on every iteration.
 */

struct poll_node {
    struct hmap_node hmap_node;
    int index;                  /* index in the pollfd array */
    const char *where;          /* Where poll_node was created. */
};

struct poll_loop {
    /* All active poll waiters. */
    struct hmap poll_nodes;

    /* Time at which to wake up the next call to poll_block(), LLONG_MIN to
     * wake up immediately, or LLONG_MAX to wait forever. */
    long long int timeout_when; /* In msecs as returned by time_msec(). */
    const char *timeout_where;  /* Where 'timeout_when' was set. */
    struct pollfd * watched;    /* list of descriptors and event masks passed to poll */
    int watched_size;           /* size of the watched allocation */
};

static struct poll_loop *poll_loop(void);

/* Look up the node with same fd or wevent. */
static struct poll_node *
find_poll_node(struct poll_loop *loop, int fd)
{
    struct poll_node *node;

    HMAP_FOR_EACH_WITH_HASH (node, hmap_node,
                             hash_2words(fd, 0),
                             &loop->poll_nodes) {
        if (fd && loop->watched[node->index].fd == fd) {
            return node;
        }
    }
    return NULL;
}

bool poll_can_read(int fd)
{
    struct poll_loop *loop = poll_loop();
    struct poll_node *node = find_poll_node(loop, fd);

    if (node) {
        return (
            loop->watched[node->index].revents &
            (POLLIN | POLLHUP | POLLNVAL | POLLERR)
        ) != 0;
    }
    return true;
}

bool poll_can_write(int fd)
{
    struct poll_loop *loop = poll_loop();
    struct poll_node *node = find_poll_node(loop, fd);

    if (node) {
        return (
            loop->watched[node->index].revents &
            (POLLOUT | POLLHUP | POLLNVAL | POLLERR)
        ) != 0;
    }
    return true;
}

/* On Unix based systems:
 *
 *     Registers 'fd' as waiting for the specified 'events' (which should be
 *     POLLIN or POLLOUT or POLLIN | POLLOUT).  The following call to
 *     poll_block() will wake up when 'fd' becomes ready for one or more of the
 *     requested events. The 'fd's are given to poll() function later.
 *
 * The event registration is one-shot: only the following call to
 * poll_block() is affected.  The event will need to be re-registered after
 * poll_block() is called if it is to persist.
 *
 * ('where' is used in debug logging.  Commonly one would use poll_fd_wait() to
 * automatically provide the caller's source file and line number for
 * 'where'.) */


static void
poll_create_node(int fd, short int events, const char *where)
{
    struct poll_loop *loop = poll_loop();
    struct poll_node *node;

    COVERAGE_INC(poll_create_node);

    /* Check for duplicate.  If found, "or" the events. */
    node = find_poll_node(loop, fd);
    if (node) {
        loop->watched[node->index].events |= events;
        /* we overwrite the original where with the value for
         * this invocation */
        node->where = where;
    } else {
        node = xzalloc(sizeof *node);
        hmap_insert(&loop->poll_nodes, &node->hmap_node,
                    hash_2words(fd, 0));

        /* If the hash has grown bigger than its matching pollfd array
         * allocation, allocate a new one and copy all elements there
         */
        if (hmap_count(&loop->poll_nodes) > loop->watched_size) {
            struct pollfd *resized_watched;

            loop->watched_size += POLLFD_INCREMENT;
            resized_watched = xzalloc(sizeof(struct pollfd) * loop->watched_size);
            memcpy(resized_watched, loop->watched, sizeof(struct pollfd) * (loop->watched_size - 1));
            free(loop->watched);
            loop->watched = resized_watched;
        }
        /* insert the new record at the end of the pollfd array */
        node->index = hmap_count(&loop->poll_nodes) - 1;
        loop->watched[node->index].fd = fd;
        loop->watched[node->index].events = events | POLLHUP | POLLERR;
        node->where = where;
    }
}

/* Registers 'fd' as waiting for the specified 'events' (which should be POLLIN
 * or POLLOUT or POLLIN | POLLOUT).  The following call to poll_block() will
 * wake up when 'fd' becomes ready for one or more of the requested events.
 *
 * The event registration is one-shot: only the following call to poll_block()
 * is affected.  The event will need to be re-registered after poll_block() is
 * called if it is to persist.
 *
 * ('where' is used in debug logging.  Commonly one would use poll_fd_wait() to
 * automatically provide the caller's source file and line number for
 * 'where'.) */
void
poll_fd_wait_at(int fd, short int events, const char *where)
{
    poll_create_node(fd, events, where);
}

/* Causes the following call to poll_block() to block for no more than 'msec'
 * milliseconds.  If 'msec' is nonpositive, the following call to poll_block()
 * will not block at all.
 *
 * The timer registration is one-shot: only the following call to poll_block()
 * is affected.  The timer will need to be re-registered after poll_block() is
 * called if it is to persist.
 *
 * ('where' is used in debug logging.  Commonly one would use poll_timer_wait()
 * to automatically provide the caller's source file and line number for
 * 'where'.) */
void
poll_timer_wait_at(long long int msec, const char *where)
{
    long long int now = time_msec();
    long long int when;

    if (msec <= 0) {
        /* Wake up immediately. */
        when = LLONG_MIN;
    } else if ((unsigned long long int) now + msec <= LLONG_MAX) {
        /* Normal case. */
        when = now + msec;
    } else {
        /* now + msec would overflow. */
        when = LLONG_MAX;
    }

    poll_timer_wait_until_at(when, where);
}

/* Causes the following call to poll_block() to wake up when the current time,
 * as returned by time_msec(), reaches 'when' or later.  If 'when' is earlier
 * than the current time, the following call to poll_block() will not block at
 * all.
 *
 * The timer registration is one-shot: only the following call to poll_block()
 * is affected.  The timer will need to be re-registered after poll_block() is
 * called if it is to persist.
 *
 * ('where' is used in debug logging.  Commonly one would use
 * poll_timer_wait_until() to automatically provide the caller's source file
 * and line number for 'where'.) */
void
poll_timer_wait_until_at(long long int when, const char *where)
{
    struct poll_loop *loop = poll_loop();
    if (when < loop->timeout_when) {
        loop->timeout_when = when;
        loop->timeout_where = where;
    }
}

/* Causes the following call to poll_block() to wake up immediately, without
 * blocking.
 *
 * ('where' is used in debug logging.  Commonly one would use
 * poll_immediate_wake() to automatically provide the caller's source file and
 * line number for 'where'.) */
void
poll_immediate_wake_at(const char *where)
{
    poll_timer_wait_at(0, where);
}

/* Logs, if appropriate, that the poll loop was awakened by an event
 * registered at 'where' (typically a source file and line number).  The other
 * arguments have two possible interpretations:
 *
 *   - If 'pollfd' is nonnull then it should be the "struct pollfd" that caused
 *     the wakeup.  'timeout' is ignored.
 *
 *   - If 'pollfd' is NULL then 'timeout' is the number of milliseconds after
 *     which the poll loop woke up.
 */
static void
log_wakeup(const char *where, const struct pollfd *pollfd, int timeout)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(10, 10);
    enum vlog_level level;
    int cpu_usage;
    struct ds s;

    cpu_usage = get_cpu_usage();
    if (VLOG_IS_DBG_ENABLED()) {
        level = VLL_DBG;
    } else if (cpu_usage > 50
               && !thread_is_pmd()
               && !VLOG_DROP_INFO(&rl)) {
        level = VLL_INFO;
    } else {
        return;
    }

    ds_init(&s);
    ds_put_cstr(&s, "wakeup due to ");
    if (pollfd) {
        char *description = describe_fd(pollfd->fd);
        if (pollfd->revents & POLLIN) {
            ds_put_cstr(&s, "[POLLIN]");
        }
        if (pollfd->revents & POLLOUT) {
            ds_put_cstr(&s, "[POLLOUT]");
        }
        if (pollfd->revents & POLLERR) {
            ds_put_cstr(&s, "[POLLERR]");
        }
        if (pollfd->revents & POLLHUP) {
            ds_put_cstr(&s, "[POLLHUP]");
        }
        if (pollfd->revents & POLLNVAL) {
            ds_put_cstr(&s, "[POLLNVAL]");
        }
        ds_put_format(&s, " on fd %d (%s)", pollfd->fd, description);
        free(description);
    } else {
        ds_put_format(&s, "%d-ms timeout", timeout);
    }
    if (where) {
        ds_put_format(&s, " at %s", where);
    }
    if (cpu_usage >= 0) {
        ds_put_format(&s, " (%d%% CPU usage)", cpu_usage);
    }
    VLOG(level, "%s", ds_cstr(&s));
    ds_destroy(&s);
}

static void
free_poll_nodes(struct poll_loop *loop)
{
    struct poll_node *node, *next;

    HMAP_FOR_EACH_SAFE (node, next, hmap_node, &loop->poll_nodes) {
        hmap_remove(&loop->poll_nodes, &node->hmap_node);
        free(node);
    }
}

/* Blocks until one or more of the events registered with poll_fd_wait()
 * occurs, or until the minimum duration registered with poll_timer_wait()
 * elapses, or not at all if poll_immediate_wake() has been called. */
void
poll_block(void)
{
    struct poll_loop *loop = poll_loop();
    struct poll_node *node, *moved_node;
    int elapsed;
    int retval;
    int i;

    /* Register fatal signal events before actually doing any real work for
     * poll_block. */
    fatal_signal_wait();

    if (loop->timeout_when == LLONG_MIN) {
        COVERAGE_INC(poll_zero_timeout);
    }

    timewarp_run();

    /* We do not need to pre-process the pollfd array in any way - it is
     * ready for use.
     */

    retval = time_poll(loop->watched, hmap_count(&loop->poll_nodes), NULL,
                       loop->timeout_when, &elapsed);
    if (retval < 0) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
        VLOG_ERR_RL(&rl, "poll: %s", ovs_strerror(-retval));
    } else if (!retval) {
        log_wakeup(loop->timeout_where, NULL, elapsed);
    } else if (get_cpu_usage() > 50 || VLOG_IS_DBG_ENABLED()) {
        for (i = 0; i < hmap_count(&loop->poll_nodes); i++) {
            if (loop->watched[i].revents) {
                node = find_poll_node(loop, loop->watched[i].fd);
                ovs_assert(node != NULL);
                log_wakeup(node->where, &loop->watched[i], 0);
            }
        }
    }

    /* Update the pollfd array to reproduce one-shot behaviour
     * and reap any fds which have been closed in the meantime
     */

    i = 0;
    while (i < hmap_count(&loop->poll_nodes)) {
        node = find_poll_node(loop, loop->watched[i].fd);
        ovs_assert(node != NULL);
        if (loop->watched[i].revents & (POLLHUP | POLLNVAL)) {
            /* FD was closed - reap */
            if (i < (hmap_count(&loop->poll_nodes) - 1)) {
                /* move last record to this index position */
                moved_node = find_poll_node(
                        loop,
                        loop->watched[hmap_count(&loop->poll_nodes) - 1].fd);
                ovs_assert(moved_node != NULL);
                loop->watched[i] = loop->watched[hmap_count(&loop->poll_nodes) - 1];
                moved_node->index = i;
            }
            hmap_remove(&loop->poll_nodes, &node->hmap_node);
            /* note - we do not i++ here as we have not processed the node
             * which we have moved from the tail of the array into the hole
             */
        } else {
            /* Clear events to replicate one-shot behaviour. Leave file
             * close related events intact so we can track file closures
             */
            loop->watched[i].events = (POLLHUP & POLLERR);
            i++;
        }
    }

    loop->timeout_when = LLONG_MAX;
    loop->timeout_where = NULL;

    /* Handle any pending signals before doing anything else. */
    fatal_signal_run();

    seq_woke();
}

static void
free_poll_loop(void *loop_)
{
    struct poll_loop *loop = loop_;

    free_poll_nodes(loop);
    hmap_destroy(&loop->poll_nodes);
    free(loop->watched);
    free(loop);
}

static struct poll_loop *
poll_loop(void)
{
    static struct ovsthread_once once = OVSTHREAD_ONCE_INITIALIZER;
    static pthread_key_t key;
    struct poll_loop *loop;

    if (ovsthread_once_start(&once)) {
        xpthread_key_create(&key, free_poll_loop);
        ovsthread_once_done(&once);
    }

    loop = pthread_getspecific(key);
    if (!loop) {
        loop = xzalloc(sizeof *loop);
        loop->timeout_when = LLONG_MAX;
        hmap_init(&loop->poll_nodes);
        loop->watched_size = POLLFD_INCREMENT;
        loop->watched = xzalloc(sizeof(struct pollfd) * loop->watched_size);
        xpthread_setspecific(key, loop);
    }
    return loop;
}
