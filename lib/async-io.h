/*
 * Copyright (c) 2019 Anton Ivanov anivanov@redhat.com
 *                                 anton.ivanov@cambridgegreys.com 
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
#include "util.h"
#include "openvswitch/list.h"
#include "openvswitch/thread.h"

struct async_io_control {
    struct latch async_latch;
    struct ovs_list work_items;
    struct ovs_mutex async_io_mutex;
};

struct async_io_pool {
    struct ovs_list list_node;
    struct async_io_control *controls;
    int size;
};

extern bool kill_async_io;

struct async_io_pool *add_pool(void *(*start)(void *));

int async_io_id(void);

#endif /* async_io.h */
