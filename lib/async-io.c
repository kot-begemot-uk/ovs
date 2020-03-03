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

#include <config.h>
#include "stream-provider.h"
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
#include "openvswitch/thread.h"
#include "latch.h"
#include "random.h"
#include "socket-util.h"
#include "util.h"
#include "ovs-numa.h"
#include "async-io.h"

VLOG_DEFINE_THIS_MODULE(async_io);

bool kill_async_io = false;

static bool async_io_setup = false;

static unsigned int seedp;

static struct ovs_mutex init_mutex = OVS_MUTEX_INITIALIZER;

static struct ovs_list io_pools = OVS_LIST_INITIALIZER(&io_pools);

static int pool_size;

static void async_io_hook(void *aux OVS_UNUSED) {
    int i;
    kill_async_io = true;
    struct async_io_pool *pool; 
    LIST_FOR_EACH(pool, list_node, &io_pools) {
        for (i = 0; i < pool->size ; i++) {
            latch_set(&pool->controls[i].async_latch);
            latch_destroy(&pool->controls[i].async_latch);
        }
    }
}

static void setup_async_io(void) {
    int cores, nodes;

    seedp = getpid();
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

    struct async_io_pool *pool;
    struct async_io_control *io_control;
    int i;

    ovs_mutex_lock(&init_mutex);

    if (!async_io_setup) {
         setup_async_io();
    }

    pool = xmalloc(sizeof(struct async_io_pool));
    pool->size = pool_size; /* we may make this more dynamic later */

    ovs_list_push_back(&io_pools, &pool->list_node);

    pool->controls = xmalloc(sizeof(struct async_io_control) * pool->size);
    for (i = 0; i < pool->size; i++) {
        io_control = &pool->controls[i];
        latch_init(&io_control->async_latch);
        ovs_mutex_init(&io_control->async_io_mutex);
        ovs_list_init(&io_control->work_items);
    }
    for (i = 0; i < pool_size; i++) {
        ovs_thread_create("async io helper", start, &pool->controls[i]);
    }
    ovs_mutex_unlock(&init_mutex);
    return pool;
}


int async_io_id(void) {
    return rand_r(&seedp);
}
