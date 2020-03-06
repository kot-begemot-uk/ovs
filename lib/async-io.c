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
    data->rx_error = 0;
    data->tx_error = 0;
    ovs_mutex_init(&data->mutex);
    // latch rx_notify, tx_notify;
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
    int retval;
    
    ovs_mutex_lock(&data->mutex);
    if (buf) {
        ovs_list_push_back(&data->output, &buf->list_node);
        data->output_count ++;
        data->backlog += buf->size;
        retval = data->backlog;
    } else {
        retval = data->backlog;
    }
    ovs_mutex_unlock(&data->mutex);
    return retval;
}

static int do_stream_flush(struct async_data *data) {
    ssize_t retval = -EAGAIN;

    ovs_mutex_lock(&data->mutex);
    while (!ovs_list_is_empty(&data->output)) {
        struct ofpbuf *buf = ofpbuf_from_list(data->output.next);
        ovs_assert(buf != NULL);
        if (data->stream->class->sendbuf) {
            ovs_list_remove(&buf->list_node);
            if ((data->stream->class->sendbuf)(data->stream, buf, &retval)) {
                /* successful enqueue, we surrender the buffer */
                if (retval > 0) {
                    data->output_count--;
                    data->backlog -= retval;
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
            } 
        }
        if (retval <= 0) {
            break;
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

    /* Fill our input buffer if it's empty. */
    ovs_mutex_lock(&data->mutex);
    retval = data->rx_error;
    if (byteq_is_empty(&data->input)) {
        chunk = byteq_headroom(&data->input);
        retval = stream_recv(data->stream, byteq_head(&data->input), chunk);
        if (retval > 0) {
            byteq_advance_head(&data->input, retval);
        }
    }
    data->rx_error = retval;
    ovs_mutex_unlock(&data->mutex);
}


void async_stream_recv(struct async_data *data) {
    /* for now just return the stream flush status */
    do_async_recv(data);
}


