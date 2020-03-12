/*
 * Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013, 2015 Nicira, Inc.
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
#include "packets.h"
#include "openvswitch/poll-loop.h"
#include "openvswitch/list.h"
#include "latch.h"
#include "ovs-numa.h"
#include "random.h"
#include "socket-util.h"
#include "util.h"
#include "async-io.h"

VLOG_DEFINE_THIS_MODULE(async_io);


static struct ovs_mutex init_mutex = OVS_MUTEX_INITIALIZER;

static struct ovs_list io_pools = OVS_LIST_INITIALIZER(&io_pools);

static int pool_size;

static struct async_io_pool *io_pool = NULL;

static bool allow_async_io = false;
static bool async_io_setup = false;
static bool kill_async_io = false;


static ssize_t do_async_recv(struct async_data *data);
static int do_async_flush(struct async_data *data);

void async_io_enable(void)
{
    allow_async_io = true;
}

static void stream_run_or_flush(struct async_data *data)
{
    ssize_t dummy;
    if (data->stream->class->sendbuf) {
         if ((data->stream->class->sendbuf)(data->stream, NULL, &dummy)) {
             data->last_activity = time_msec();
         }
    } else {
         stream_run(data->stream);
    }
}

static void *default_async_io_helper(void *arg) {
    struct async_io_control *io_control =
        (struct async_io_control *) arg;
    struct async_data *data;
    int retval;
    int counter;

    do {
        ovs_mutex_lock(&io_control->mutex);
        latch_poll(&io_control->async_latch);
        LIST_FOR_EACH(data, list_node, &io_control->work_items) {
            ovs_mutex_lock(&data->mutex);
            if (((data->rx_error > 0) || (data->rx_error == -EAGAIN)) &&
                    ((data->tx_error >= 0) || (data->tx_error == -EAGAIN))) {
                if (!byteq_is_full(&data->input)) {
                    retval = do_async_recv(data);
                    if (!byteq_is_empty(&data->input)) {
                        latch_set(&data->rx_notify);
                    }
                    if (retval > 0 || retval == -EAGAIN) {
                        stream_recv_wait(data->stream);
                        stream_run_or_flush(data);
                    }
                } else {
                    retval = -EAGAIN;
                    stream_run_or_flush(data);
                }
                do_async_flush(data);
                counter = 0;
                while ((data->flush_required) && (data->backlog > 0)) {
                    if (((counter < 50) && (data->tx_error < 0)) || (data->tx_error != -EAGAIN)) {
                        break;
                    }
                    do_async_flush(data);
                    counter++;
                }
                if (data->backlog) {
                    stream_send_wait(data->stream);
                }
            } else {
                /* make sure that the other thread(s) notice any errors */
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
    LIST_FOR_EACH(pool, list_node, &io_pools) {
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
        pool_size = cores/nodes;
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

    new_pool->controls = xmalloc(sizeof(struct async_io_control) * new_pool->size);
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
    data->rx_error = -EAGAIN;
    data->tx_error = 0;
    ovs_mutex_init(&data->mutex);
    data->async_mode = allow_async_io;
    if (data->async_mode) {
        if (!io_pool) {
            io_pool = add_pool(default_async_io_helper);
        }
        data->flush_required = false;
        data->async_id = random_uint32();
        target_control = &io_pool->controls[data->async_id % io_pool->size];
        /* these are just fd pairs, no need to play with pointers, we
         * can pass them around
         */
        data->tx_notify = target_control->async_latch;
        data->run_notify = target_control->async_latch;
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
    if (data->async_mode) {
        ovs_mutex_lock(&data->mutex);
        if (data->backlog > 0) {
            data->flush_required = true;
            latch_set(&data->rx_notify);
        } 
        ovs_mutex_unlock(&data->mutex);
        while ((async_get_backlog(data) > 0) && (data->rx_error > 0) && (data->tx_error >= 0)) {
            latch_poll(&data->rx_notify);
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
    ofpbuf_list_delete(&data->output);
    data->backlog = 0;
    data->output_count = 0;
}

/* Routines intended for async IO */

int async_stream_enqueue(struct async_data *data, struct ofpbuf *buf) {
    int retval = -EAGAIN;
    
    ovs_mutex_lock(&data->mutex);
    switch (data->stream->state) {
    case SCS_CONNECTING:
        retval = -EAGAIN;
        break;

    case SCS_CONNECTED:
        retval = 0;
        break;

    case SCS_DISCONNECTED:
        retval = data->stream->error;
        break;

    default:
        OVS_NOT_REACHED();
    }

    if (buf && retval == 0) {
        ovs_list_push_back(&data->output, &buf->list_node);
        data->output_count ++;
        data->backlog += buf->size;
    } 
    retval = data->backlog;
    ovs_mutex_unlock(&data->mutex);
    if (data->async_mode) {
        /* Once we have set the latch, the io thread will do the job */
        latch_set(&data->tx_notify);
    }
    return retval;
}

static int do_async_flush(struct async_data *data) {
    ssize_t retval = -EAGAIN;

    if (data->tx_error == -EAGAIN) {
        data->tx_error = 0;
    }
    while ((data->tx_error >= 0) && !ovs_list_is_empty(&data->output)) {
        struct ofpbuf *buf = ofpbuf_from_list(data->output.next);
        if (data->stream->class->sendbuf) {
            ovs_list_remove(&buf->list_node);
            if ((data->stream->class->sendbuf)(data->stream, buf, &retval)) {
                /* successful enqueue, we surrender the buffer */
                if (retval > 0) {
                    data->output_count--;
                    data->backlog -= retval;
                    data->last_activity = time_msec();
                }
            } else {
                /* unsuccessful enqueue - push element back onto list*/
                ovs_list_push_front(&data->output, &buf->list_node);
            }
        } else {
            retval = stream_send(data->stream, buf->data, buf->size);
            if (retval > 0) {
                data->backlog -= retval;
                ofpbuf_pull(buf, retval);
                if (!buf->size) {
                    ovs_list_remove(&buf->list_node);
                    data->output_count--;
                    ofpbuf_delete(buf);
                }
                data->last_activity = time_msec();
            } 
        }
        if (retval <= 0) {
            break;
        }
    }
    data->tx_error = retval;
    return retval;
}

int async_stream_flush(struct async_data *data) {
    /* for now just return the stream flush status */
    int retval;

    if (data->async_mode) {
        ovs_mutex_lock(&data->mutex);
        retval = data->tx_error; /* we are one error late */
        ovs_mutex_unlock(&data->mutex);
        if (retval >= 0) {
            retval = -EAGAIN; /* fake a busy so that upper layers do not
                               * retry, we will flush the backlog in the
                               * background
                               */
        }
    } else {
        retval = do_async_flush(data);
    }
    return retval;
}

static ssize_t do_async_recv(struct async_data *data) {
    size_t chunk;
    int retval = data->rx_error; 

    /* Fill our input buffer if there is a space in it.
     * We are being called under a mutex so the reader does
     * not race with us
     */
    if (byteq_is_empty(&data->input)) {
        chunk = byteq_headroom(&data->input);
        retval = stream_recv(data->stream, byteq_head(&data->input), chunk);
        if (retval > 0) {
            byteq_advance_head(&data->input, retval);
            data->last_activity = time_msec();
        }
        data->rx_error = retval;
    } 
    return retval;
}

ssize_t async_stream_recv(struct async_data *data) {
    /* for now just return the stream flush status */
    int retval;
    if (!data->async_mode) {
        if (byteq_is_empty(&data->input)) {
            retval = do_async_recv(data);
        } else {
            retval = -EAGAIN;
        }
    } else {
        latch_poll(&data->rx_notify); /* clear any rx notifies */
        ovs_mutex_lock(&data->mutex);
        retval = data->rx_error;
        ovs_mutex_unlock(&data->mutex);
    }
    return retval;
}

void async_stream_run(struct async_data *data) {
    if (data->async_mode) {
        //latch_set(&data->run_notify);
    } else {
        stream_run(data->stream);
    }
}

void async_invoke_notify(struct async_data *data) {
    if (data->async_mode) {
        latch_set(&data->tx_notify);
    }
}

long long int async_last_activity(struct async_data *data) {
    long long int retval;
    ovs_mutex_lock(&data->mutex);
    retval = data->last_activity;
    ovs_mutex_unlock(&data->mutex);
    return retval;
}

int async_get_backlog(const struct async_data *data) {
    int retval;
    ovs_mutex_lock(&data->mutex);
    retval = data->backlog;
    ovs_mutex_unlock(&data->mutex);
    return retval;
}

int async_get_received_bytes(const struct async_data *data) {
    int retval;
    ovs_mutex_lock(&data->mutex);
    retval = data->input.head;
    ovs_mutex_unlock(&data->mutex);
    return retval;
}

bool async_byteq_is_empty(struct async_data *data) {
    bool retval;
    ovs_mutex_lock(&data->mutex);
    retval = byteq_is_empty(&data->input);
    ovs_mutex_unlock(&data->mutex);
    return retval;
}

void async_recv_wait(struct async_data *data) {
    if (data->async_mode) {
        latch_wait(&data->rx_notify);
    } else {
        stream_recv_wait(data->stream);
    }
}
