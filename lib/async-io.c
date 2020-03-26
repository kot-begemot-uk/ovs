/*
 * Copyright (c) 2020 Red Hat Inc
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
#include "stream-provider.h"
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include "coverage.h"
#include "fatal-signal.h"
#include "flow.h"
#include "jsonrpc.h"
#include "openflow/nicira-ext.h"
#include "openflow/openflow.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/ofp-print.h"
#include "openvswitch/ofpbuf.h"
#include "openvswitch/vlog.h"
#include "ovs-thread.h"
#include "ovs-atomic.h"
#include "packets.h"
#include "openvswitch/poll-loop.h"
#include "random.h"
#include "socket-util.h"
#include "util.h"
#include "timeval.h"
#include "async-io.h"
#include "ovs-numa.h"

VLOG_DEFINE_THIS_MODULE(async_io);

static bool allow_async_io = false;

static bool async_io_setup = false;
static bool kill_async_io = false;

static struct ovs_mutex init_mutex = OVS_MUTEX_INITIALIZER;

static struct ovs_list io_pools = OVS_LIST_INITIALIZER(&io_pools);

static int pool_size;

static struct async_io_pool *io_pool = NULL;

static int do_async_recv(struct async_data *data);
static int do_stream_flush(struct async_data *data);

static inline bool not_in_error(struct async_data *data) {
    int rx_error, tx_error;

    atomic_read_relaxed(&data->rx_error, &rx_error);
    atomic_read_relaxed(&data->tx_error, &tx_error);

    return (
        ((rx_error > 0) || (rx_error == -EAGAIN)) &&
        ((tx_error >= 0) || (tx_error == -EAGAIN))
    );
}

static inline bool in_error(struct async_data *data) {
    return ! not_in_error(data);
}


static void *default_async_io_helper(void *arg) {
    struct async_io_control *io_control =
        (struct async_io_control *) arg;
    struct async_data *data;
    int retval;

    do {
        ovs_mutex_lock(&io_control->mutex);
        latch_poll(&io_control->async_latch);
        LIST_FOR_EACH (data, list_node, &io_control->work_items) {

            ovs_mutex_lock(&data->mutex);
            retval = -EAGAIN;
            if (not_in_error(data)) {
                /*
                 * We stop reading if the input queue is full
                 */
                if (byteq_headroom(&data->input) != 0) {
                    retval = do_async_recv(data);
                } else {
                    poll_timer_wait(1);
                    retval = 0;
                }

                if (!byteq_is_empty(&data->input)) {
                    latch_set(&data->rx_notify);
                }
            }
            if (not_in_error(data) && (retval > 0 || retval == -EAGAIN)) {
                stream_recv_wait(data->stream);
            }
            if (not_in_error(data)) {
                stream_run(data->stream);
                do_stream_flush(data);
            }
            if (not_in_error(data) && data->backlog) {
                stream_send_wait(data->stream);
            }
            if (in_error(data)) {
                /* make sure that the other thread(s) notice any errors.
                 * this should not be an else because errors may have
                 * changed inside the ifs above.
                 */
                latch_set(&data->rx_notify);
            }
            ovs_mutex_unlock(&data->mutex);
        }
        ovs_mutex_unlock(&io_control->mutex);
        latch_wait(&io_control->async_latch);
        poll_block();
    } while (!kill_async_io);
    return arg;
}

static void async_io_hook(void *aux OVS_UNUSED) {
    int i;
    static struct async_io_pool *pool;
    kill_async_io = true;
    LIST_FOR_EACH (pool, list_node, &io_pools) {
        for (i = 0; i < pool->size ; i++) {
            latch_set(&pool->controls[i].async_latch);
            latch_destroy(&pool->controls[i].async_latch);
        }
    }
}

static void setup_async_io(void) {
    int cores, nodes;

    nodes = ovs_numa_get_n_numas();
    if (nodes == OVS_NUMA_UNSPEC || nodes <= 0) {
        nodes = 1;
    }
    cores = ovs_numa_get_n_cores();
    if (cores == OVS_CORE_UNSPEC || cores <= 0) {
        pool_size = 4;
    } else {
        pool_size = cores / nodes;
    }
    fatal_signal_add_hook(async_io_hook, NULL, NULL, true);
    async_io_setup = true;
}

struct async_io_pool *add_pool(void *(*start)(void *)){

    struct async_io_pool *new_pool = NULL;
    struct async_io_control *io_control;
    int i;

    ovs_mutex_lock(&init_mutex);

    if (!async_io_setup) {
         setup_async_io();
    }

    new_pool = xmalloc(sizeof(struct async_io_pool));
    new_pool->size = pool_size; /* we may make this more dynamic later */

    ovs_list_push_back(&io_pools, &new_pool->list_node);

    new_pool->controls =
        xmalloc(sizeof(struct async_io_control) * new_pool->size);
    for (i = 0; i < new_pool->size; i++) {
        io_control = &new_pool->controls[i];
        latch_init(&io_control->async_latch);
        ovs_mutex_init(&io_control->mutex);
        ovs_list_init(&io_control->work_items);
    }
    for (i = 0; i < pool_size; i++) {
        ovs_thread_create("async io helper", start, &new_pool->controls[i]);
    }
    ovs_mutex_unlock(&init_mutex);
    return new_pool;
}

void
async_init_data(struct async_data *data, struct stream *stream)
{
    struct async_io_control *target_control;

    data->stream = stream;
    byteq_init(&data->input, data->input_buffer, ASYNC_BUFFER_SIZE);
    ovs_list_init(&data->output);
    data->backlog = 0;
    data->output_count = 0;
    data->rx_error = ATOMIC_VAR_INIT(-EAGAIN);
    data->tx_error = ATOMIC_VAR_INIT(0);
    data->active = ATOMIC_VAR_INIT(false);
    ovs_mutex_init(&data->mutex);
    data->async_mode = allow_async_io;
    if (data->async_mode) {
        if (!io_pool) {
            io_pool = add_pool(default_async_io_helper);
        }
        data->async_id = random_uint32();
        target_control = &io_pool->controls[data->async_id % io_pool->size];
        /* these are just fd pairs, no need to play with pointers, we
         * can pass them around
         */
        data->tx_run_notify = target_control->async_latch;
        latch_init(&data->rx_notify);
        ovs_mutex_lock(&target_control->mutex);
        ovs_list_push_back(&target_control->work_items, &data->list_node);
        ovs_mutex_unlock(&target_control->mutex);
        latch_set(&target_control->async_latch);
    }
}

void
async_stream_enable(struct async_data *data)
{
    data->async_mode = allow_async_io;
}

void
async_stream_disable(struct async_data *data)
{
    struct async_io_control *target_control;
    int count = 0;

    if (data->async_mode) {
        while (not_in_error(data) && data->backlog && count < 5) {
            latch_set(&data->tx_run_notify);
            latch_poll(&data->rx_notify);
            count ++;
        }
        target_control = &io_pool->controls[data->async_id % io_pool->size];
        ovs_mutex_lock(&target_control->mutex);
        ovs_list_remove(&data->list_node);
        ovs_mutex_unlock(&target_control->mutex);
        data->async_mode = false;
        latch_destroy(&data->rx_notify);
    }
}

void
async_cleanup_data(struct async_data *data)
{
    if (data->backlog) {
        ofpbuf_list_delete(&data->output);
    }
    data->backlog = 0;
    data->output_count = 0;
}

/* Routines intended for async IO */

int async_stream_enqueue(struct async_data *data, struct ofpbuf *buf) {
    int retval = -EAGAIN;

    ovs_mutex_lock(&data->mutex);
    if (buf) {
        ovs_list_push_back(&data->output, &buf->list_node);
        data->output_count ++;
        data->backlog += buf->size;
        atomic_thread_fence(memory_order_release);
    }
    retval = data->backlog;
    ovs_mutex_unlock(&data->mutex);
    return retval;
}

static int do_stream_flush(struct async_data *data) {
    struct ofpbuf *buf;
    int count = 0;
    bool stamp = false;
    int retval = -stream_connect(data->stream);

    if (!retval) {
        while (!ovs_list_is_empty(&data->output) && count < 10) {
            buf = ofpbuf_from_list(data->output.next);
            if (data->stream->class->enqueue) {
                ovs_list_remove(&buf->list_node);
                retval = (data->stream->class->enqueue)(data->stream, buf);
                if (retval > 0) {
                    data->output_count--;
                } else {
                    ovs_list_push_front(&data->output, &buf->list_node);
                }
            } else {
                retval = stream_send(data->stream, buf->data, buf->size);
                if (retval > 0) {
                    stamp = true;
                    data->backlog -= retval;
                    ofpbuf_pull(buf, retval);
                    if (!buf->size) {
                        /* stream now owns buf */
                        ovs_list_remove(&buf->list_node);
                        data->output_count--;
                        ofpbuf_delete(buf);
                    }
                }
            }
            if (retval <= 0) {
                break;
            }
            count++;
        }
        if (data->stream->class->flush && (retval >= 0 || retval == -EAGAIN)) {
            (data->stream->class->flush)(data->stream, &retval);
            if (retval > 0) {
                stamp = true;
                data->backlog -= retval;
            }
        }
        if (stamp) {
            atomic_store_relaxed(&data->active, true);
        }
    }
    atomic_store_relaxed(&data->tx_error, retval);
    return retval;
}

int async_stream_flush(struct async_data *data) {
    int retval;

    if (data->async_mode) {
        atomic_read_relaxed(&data->tx_error, &retval);
        if (retval >= 0) {
            retval = -EAGAIN; /* fake a busy so that upper layers do not
                               * retry, we will flush the backlog in the
                               * background
                               */
        }
        latch_set(&data->tx_run_notify);
    } else {
        retval = do_stream_flush(data);
    }
    return retval;
}

static int do_async_recv(struct async_data *data) {
    size_t chunk;
    int retval;

    atomic_read_relaxed(&data->rx_error, &retval);
    if (retval > 0 || retval == -EAGAIN) {
        chunk = byteq_headroom(&data->input);
        if (chunk > 0) {
            retval = stream_recv(
                    data->stream, byteq_head(&data->input), chunk);
            if (retval > 0) {
                byteq_advance_head(&data->input, retval);
           }
        }
    }
    if (retval > 0 || retval == -EAGAIN) {
        retval = byteq_used(&data->input);
        if (retval == 0) {
            retval = -EAGAIN;
        }
    }
    atomic_store_relaxed(&data->rx_error, retval);
    return retval;
}


int async_stream_recv(struct async_data *data) {
    int retval = -EAGAIN;

    if (data->async_mode) {
        atomic_read_relaxed(&data->rx_error, &retval);
        /* clear RX notifications */
        latch_poll(&data->rx_notify);
        /* fake a retval from byteq usage */
        if (retval > 0 || retval == -EAGAIN) {
            retval = byteq_used(&data->input);
            if (retval == 0) {
                retval = -EAGAIN;
            }
        }
    } else {
        retval = do_async_recv(data);
    }
    return retval;
}

void async_stream_run(struct async_data *data) {
    if (!data->async_mode) {
        stream_run(data->stream);
    }
 }

void async_io_kick(struct async_data *data) {
    if (data->async_mode) {
        latch_set(&data->tx_run_notify);
    }
}

void async_recv_wait(struct async_data *data) {
    if (data->async_mode) {
        latch_wait(&data->rx_notify);
    } else {
        stream_recv_wait(data->stream);
    }
}

void async_io_enable(void) {
    allow_async_io = true;
}

/* Accessors for JSON RPC */

struct byteq *async_get_input(struct async_data *data) {
    return &data->input;
}
struct stream *async_get_stream(struct async_data *data) {
    return data->stream;
}

bool async_output_is_empty(struct async_data *data) {
    bool retval;
    ovs_mutex_lock(&data->mutex);
    /* backlog tracks backlog across the full stack all the
     * way to the actual send. It is the source of truth
     * if we have output or not so anybody asking if we
     * have output should be told if we have backlog
     * instead.
     */
    retval = (data->backlog == 0);
    ovs_mutex_unlock(&data->mutex);
    return retval;
}

long async_get_backlog(struct async_data *data) {
    if (data->async_mode) {
        return 0; /* we pretend we have no backlog if we are async */
    }
    return data->backlog;
}

bool async_get_active(struct async_data *data) {
    bool test = true;
    return atomic_compare_exchange_weak(&data->active, &test, false);
}


