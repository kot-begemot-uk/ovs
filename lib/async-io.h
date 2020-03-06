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
#include "socket-util.h"
#include "util.h"

#define ASYNC_BUFFER_SIZE (512)

struct stream;

struct async_data {
    struct stream *stream;
    struct ovs_list output;
    size_t backlog;
    size_t output_count;
    int rx_error, tx_error;
    struct ovs_mutex mutex;
    struct latch *rx_notify, *tx_notify, *run_notify;
    bool async_mode;
    struct byteq input;
    uint8_t input_buffer[ASYNC_BUFFER_SIZE];
};


int async_stream_enqueue(struct async_data *, struct ofpbuf *buf);
int async_stream_flush(struct async_data *);
void async_stream_recv(struct async_data *);

void async_stream_enable(struct async_data *);
void async_stream_disable(struct async_data *);

void async_init_data(struct async_data *, struct stream *);
void async_cleanup_data(struct async_data *);

#endif /* async-io.h */
