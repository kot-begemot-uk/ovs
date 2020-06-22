/*
 * Copyright (c) 2020 Red Hat, Inc.
 * Copyright (c) 2008, 2009, 2010, 2012, 2013, 2015, 2019 Nicira, Inc.
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
#include <stdint.h>
#include <string.h>
#include <semaphore.h>
#include "fatal-signal.h"
#include "util.h"
#include "openvswitch/vlog.h"
#include "openvswitch/hmap.h"
#include "openvswitch/thread.h"
#include "fasthmap.h"
#include "ovs-atomic.h"
#include "ovs-thread.h"
#include "ovs-numa.h"

VLOG_DEFINE_THIS_MODULE(fasthmap);


static bool worker_pool_setup = false;
static bool workers_must_exit = false;

static struct ovs_list worker_pools = OVS_LIST_INITIALIZER(&worker_pools);

static struct ovs_mutex init_mutex = OVS_MUTEX_INITIALIZER;

static int pool_size;

static void worker_pool_hook(void *aux OVS_UNUSED) {
    int i;
    static struct worker_pool *pool;
    workers_must_exit = true; /* all workers must honour this flag */
    atomic_thread_fence(memory_order_release);
    LIST_FOR_EACH (pool, list_node, &worker_pools) {
        for (i = 0; i < pool->size ; i++) {
            sem_post(&pool->controls[i].fire);
        }
    }
}

static void setup_worker_pools(void) {
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
    fatal_signal_add_hook(worker_pool_hook, NULL, NULL, true);
    worker_pool_setup = true;
}

bool seize_fire(void)
{
    return workers_must_exit;
}

struct worker_pool *add_worker_pool(void *(*start)(void *)){

    struct worker_pool *new_pool = NULL;
    struct worker_control *new_control;
    int i;

    ovs_mutex_lock(&init_mutex);

    if (!worker_pool_setup) {
         setup_worker_pools();
    }

    new_pool = xmalloc(sizeof(struct worker_pool));
    new_pool->size = pool_size;
    sem_init(&new_pool->done, 0, 0);

    ovs_list_push_back(&worker_pools, &new_pool->list_node);

    new_pool->controls =
        xmalloc(sizeof(struct worker_control) * new_pool->size);

    for (i = 0; i < new_pool->size; i++) {
        new_control = &new_pool->controls[i];
        sem_init(&new_control->fire, 0, 0);
        new_control->id = i;
        new_control->done = &new_pool->done;
        new_control->data = NULL;
        ovs_mutex_init(&new_control->mutex);
        new_control->finished = ATOMIC_VAR_INIT(false);
    }

    for (i = 0; i < pool_size; i++) {
        ovs_thread_create("worker pool helper", start, &new_pool->controls[i]);
    }
    ovs_mutex_unlock(&init_mutex);
    return new_pool;
}


/* Initializes 'hmap' as an empty hash table with mask N. */
void
fast_hmap_init(struct hmap *hmap, ssize_t mask)
{
    size_t i;

    hmap->buckets = xmalloc(sizeof (struct hmap_node *) * (mask + 1));
    hmap->one = NULL;
    hmap->mask = mask;
    hmap->n = 0;
    for (i = 0; i <= hmap->mask; i++) {
        hmap->buckets[i] = NULL;
    }
}

/* Initializes 'hmap' as an empty hash table of size X.
 * Intended for use in parallel processing so that all
 * fragments used to store results in a parallel job
 * are the same size.
 */
void
fast_hmap_size_for(struct hmap *hmap, int size)
{
    size_t mask;
    mask = size / 2;
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask |= mask >> 8;
    mask |= mask >> 16;
#if SIZE_MAX > UINT32_MAX
    mask |= mask >> 32;
#endif

    /* If we need to dynamically allocate buckets we might as well allocate at
     * least 4 of them. */
    mask |= (mask & 1) << 1;

    fast_hmap_init(hmap, mask);
}

/* Run a thread pool - basic, does not do results processing.
 */

void run_pool(struct worker_pool *pool)
{
    int index, completed;

    atomic_thread_fence(memory_order_release);

    for (index = 0; index < pool->size; index++) {
        sem_post(&pool->controls[index].fire);
    }

    completed = 0;

    do {
        bool test;
        sem_wait(&pool->done);
        for (index = 0; index < pool->size; index++) {
            test = true;
            if (atomic_compare_exchange_weak(
                    &pool->controls[index].finished,
                    &test,
                    false)) {
                completed++;
                pool->controls[index].data = NULL;
            }
        }
    } while (completed < pool->size);
}

/* Brute force merge of a hashmap into another hashmap.
 * Intended for use in parallel processing. The destination
 * hashmap MUST be the same size as the one being merged.
 *
 * This can be achieved by pre-allocating them to correct size
 * and using hmap_insert_fast() instead of hmap_insert()
 */

void fast_hmap_merge(struct hmap *dest, struct hmap *inc)
{
    size_t i;

    ovs_assert(inc->mask == dest->mask);

    if (!inc->n) {
        /* Request to merge an empty frag, nothing to do */
        return;
    }

    for (i = 0; i <= dest->mask; i++) {
        struct hmap_node **dest_bucket = &dest->buckets[i];
        struct hmap_node **inc_bucket = &inc->buckets[i];
        if (*inc_bucket != NULL) {
            struct hmap_node *last_node = *inc_bucket;
            while (last_node->next != NULL) {
                last_node = last_node->next;
            }
            last_node->next = *dest_bucket;
            *dest_bucket = *inc_bucket;
            *inc_bucket = NULL;
        }
    }
    dest->n += inc->n;
    inc->n = 0;
}

/* Run a thread pool which gathers results in an array
 * of hashes. Merge results.
 */

void run_pool_hash(
        struct worker_pool *pool,
        struct hmap *result,
        struct hmap *result_frags)
{
    int index, completed;

    atomic_thread_fence(memory_order_release);

    for (index = 0; index < pool->size; index++) {
        sem_post(&pool->controls[index].fire);
    }

    completed = 0;

    do {
        bool test;
        sem_wait(&pool->done);
        for (index = 0; index < pool->size; index++) {
            test = true;
            if (atomic_compare_exchange_weak(
                    &pool->controls[index].finished,
                    &test,
                    false)) {
                fast_hmap_merge(result, &result_frags[index]);
                hmap_destroy(&result_frags[index]);
                completed++;
                pool->controls[index].data = NULL;
            }
        }
    } while (completed < pool->size);
}

void merge_lists(struct ovs_list **dest, struct ovs_list *inc)
{
    struct ovs_list *last, *first;
    if (inc == NULL) {
        return;
    }

    if (* dest == NULL) {
        * dest = inc;
        return;
    }

    if (ovs_list_is_empty(inc)) {
        return;
    }

    if (ovs_list_is_empty(*dest)) {
        * dest = inc;
        return;
    }


    last = inc->prev;
    /* first element is not the list pointer itself, it is the ->next */
    first = inc->next;

    (*dest)->prev->next = first;
    first->prev = (*dest)->prev;

    (*dest)->prev = last;
    last->next = *dest;
}

/* Run a thread pool which gathers results in an array
 * of lists. Merge results
 */

void run_pool_list(
        struct worker_pool *pool,
        struct ovs_list **result,
        struct ovs_list **result_frags)
{
    int index, completed;

    atomic_thread_fence(memory_order_release);


    for (index = 0; index < pool->size; index++) {
        sem_post(&pool->controls[index].fire);
    }

    completed = 0;


    do {
        bool test;
        sem_wait(&pool->done);
        for (index = 0; index < pool->size; index++) {
            test = true;
            if (atomic_compare_exchange_weak(
                    &pool->controls[index].finished,
                    &test,
                    false)) {
                merge_lists(result, result_frags[index]);
                completed++;
                pool->controls[index].data = NULL;
            }
        }
    } while (completed < pool->size);
}

/* Run a thread pool which uses a callback function to process results
 */

void run_pool_callback(
        struct worker_pool *pool,
        void *fin_result,
        void (*helper_func)(
            struct worker_pool *pool, void *fin_result, int index))
{
    int index, completed;

    atomic_thread_fence(memory_order_release);

    for (index = 0; index < pool->size; index++) {
        sem_post(&pool->controls[index].fire);
    }

    completed = 0;

    do {
        bool test;
        sem_wait(&pool->done);
        for (index = 0; index < pool->size; index++) {
            test = true;
            if (atomic_compare_exchange_weak(
                    &pool->controls[index].finished,
                    &test,
                    false)) {
                (helper_func)(pool, fin_result, index);
                completed++;
                pool->controls[index].data = NULL;
            }
        }
    } while (completed < pool->size);
}
