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
#include "ovs-atomic.h"
#include "packets.h"
#include "openvswitch/poll-loop.h"
#include "random.h"
#include "socket-util.h"
#include "util.h"
#include "async-io.h"

VLOG_DEFINE_THIS_MODULE(async_io);

static bool allow_async_io = false;

void
async_init_data(struct async_data *data, struct stream *stream)
{

    data->stream = stream;
    byteq_init(&data->input, data->input_buffer, ASYNC_BUFFER_SIZE);
    ovs_list_init(&data->output);
    data->backlog = 0;
    data->output_count = 0;
    data->rx_error = -EAGAIN;
    data->tx_error = 0;
    ovs_mutex_init(&data->mutex);
    data->async_mode = false;
}

void
async_stream_enable(struct async_data *data)
{
    data->async_mode = allow_async_io;
}

void
async_stream_disable(struct async_data *data)
{
    data->async_mode = false;
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
    int retval = -EAGAIN;
    struct ofpbuf *buf;
    int count = 0;

    ovs_mutex_lock(&data->mutex);

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
                data->backlog -= retval;
                ofpbuf_pull(buf, retval);
                if (!buf->size) {
                    ovs_list_remove(&buf->list_node); /* stream now owns buf */
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
            data->backlog -= retval;
        }
    }
    ovs_mutex_unlock(&data->mutex);
    return retval;
}

int async_stream_flush(struct async_data *data) {
    /* for now just return the stream flush status */
    return do_stream_flush(data);
}

static void do_async_recv(struct async_data *data) {
    size_t chunk;
    int retval;

    ovs_mutex_lock(&data->mutex);
    retval = data->rx_error;
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
        if (byteq_is_empty(&data->input)) {
            retval = -EAGAIN;
        } else {
            retval = byteq_used(&data->input);
        }
    }
    data->rx_error = retval;
    ovs_mutex_unlock(&data->mutex);
}


void async_stream_recv(struct async_data *data) {
    /* for now just return the stream receive status */
    do_async_recv(data);
}


