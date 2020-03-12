/*
 * Copyright (c) 2009, 2010, 2011, 2013, 2015 Nicira, Inc.
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

#ifndef ASYNC_IO_H
#define ASYNC_IO_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "openvswitch/types.h"
#include "openvswitch/ofpbuf.h"
#include "ovs-thread.h"
#include "latch.h"
#include "byteq.h"
#include "socket-util.h"
#include "util.h"

#define ASYNC_BUFFER_SIZE (8192)

struct stream;

struct async_data {
    struct ovs_list list_node;
    struct stream *stream;
    struct ovs_list output;
    size_t backlog;
    size_t output_count;
    int rx_error, tx_error;
    uint32_t async_id;
    struct ovs_mutex mutex;
    struct latch rx_notify, tx_notify, run_notify;
    bool async_mode;
    bool flush_required;
    struct byteq input;
    uint8_t input_buffer[ASYNC_BUFFER_SIZE];
};

struct async_io_control {
    struct latch async_latch;
    struct ovs_list work_items;
    struct ovs_mutex mutex;
};

struct async_io_pool {
    struct ovs_list list_node;
    struct async_io_control *controls;
    int size;
};


struct async_io_pool *add_pool(void *(*start)(void *));

int async_get_backlog(const struct async_data *data);
int async_get_received_bytes(const struct async_data *data);
bool async_byteq_is_empty(struct async_data *);
int async_stream_enqueue(struct async_data *, struct ofpbuf *buf);
int async_stream_flush(struct async_data *);
ssize_t async_stream_recv(struct async_data *);
void async_stream_run(struct async_data *);
long long int async_last_activity(struct async_data *);

void async_stream_enable(struct async_data *);
void async_stream_disable(struct async_data *);

void async_init_data(struct async_data *, struct stream *);
void async_recv_wait(struct async_data *);
void async_cleanup_data(struct async_data *);
void async_invoke_notify(struct async_data *);

void async_io_enable(void);


#endif /* async-io.h */
