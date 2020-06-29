/*
 * Copyright (c) 2020 Red Hat, Inc.
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

#ifndef FAST_HMAP_H
#define FAST_HMAP_H 1


#ifdef  __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdlib.h>
#include <semaphore.h>
#include "openvswitch/util.h"
#include "openvswitch/hmap.h"
#include "openvswitch/thread.h"
#include "ovs-atomic.h"

#define HMAP_FOR_EACH_IN_PARALLEL(NODE, MEMBER, JOBID, HMAP) \
   for (INIT_CONTAINER(NODE, hmap_first_in_bucket_num(HMAP, JOBID), MEMBER); \
        (NODE != OBJECT_CONTAINING(NULL, NODE, MEMBER)) \
       || ((NODE = NULL), false); \
       ASSIGN_CONTAINER(NODE, hmap_next_in_bucket(&(NODE)->MEMBER), MEMBER))

/* Safe when NODE may be freed (not needed when NODE may be removed from the
 * hash map but its members remain accessible and intact). */
#define HMAP_FOR_EACH_IN_PARALLEL_SAFE(NODE, NEXT, MEMBER, JOBID, HMAP) \
    HMAP_FOR_EACH_SAFE_PARALLEL_INIT(NODE, NEXT, MEMBER, JOBID, HMAP, (void) 0)

#define HMAP_FOR_EACH_SAFE_PARALLEL_INIT(NODE, NEXT, \
    MEMBER, JOBID, HMAP, ...)\
    for (INIT_CONTAINER(NODE, hmap_first_in_bucket_num(HMAP, JOBID), MEMBER), \
           __VA_ARGS__;   \
         ((NODE != OBJECT_CONTAINING(NULL, NODE, MEMBER))               \
          || ((NODE = NULL), false)                                     \
          ? INIT_CONTAINER(NEXT, hmap_next_in_bucket(&(NODE)->MEMBER),  \
          MEMBER), 1 : 0);                                              \
         (NODE) = (NEXT))

struct worker_control {
    int id;
    int size;
    atomic_bool finished;
    sem_t fire;
    sem_t *done;
    struct ovs_mutex mutex;
    void *data;
    void *workload;
};

struct worker_pool {
    int size;
    struct ovs_list list_node;
    struct worker_control *controls;
    sem_t done;
};

struct worker_pool *add_worker_pool(void *(*start)(void *));

bool seize_fire(void);
void fast_hmap_size_for(struct hmap *hmap, int size);
void fast_hmap_init(struct hmap *hmap, ssize_t size);
void fast_hmap_merge(struct hmap *dest, struct hmap *inc);
void hmap_merge(struct hmap *dest, struct hmap *inc);
void merge_lists(struct ovs_list **dest, struct ovs_list *inc);

void run_pool(
    struct worker_pool *pool);
void run_pool_hash(
    struct worker_pool *pool, struct hmap *result, struct hmap *result_frags);
void run_pool_list(
    struct worker_pool *pool, struct ovs_list **result,
    struct ovs_list **result_frags);
void run_pool_callback(
        struct worker_pool *pool,
        void *fin_result,
        void (*helper_func)(
            struct worker_pool *pool, void *fin_result, int index));


/* Returns the first node in 'hmap' in the bucket in which the given 'hash'
 * would land, or a null pointer if that bucket is empty. */
static inline struct hmap_node *
hmap_first_in_bucket_num(const struct hmap *hmap, size_t num)
{
    return hmap->buckets[num];
}

static inline struct hmap_node *
parallel_hmap_next__(const struct hmap *hmap, size_t start, size_t pool_size)
{
    size_t i;
    for (i = start; i <= hmap->mask; i+= pool_size) {
        struct hmap_node *node = hmap->buckets[i];
        if (node) {
            return node;
        }
    }
    return NULL;
}

/* Returns the first node in 'hmap', as expected by thread with job_id
 * for parallel processing in arbitrary order, or a null pointer if
 * the slice of 'hmap' for that job_id is empty. */
static inline struct hmap_node *
parallel_hmap_first(const struct hmap *hmap, size_t job_id, size_t pool_size)
{
    return parallel_hmap_next__(hmap, job_id, pool_size);
}

/* Returns the next node in the slice of 'hmap' following 'node',
 * in arbitrary order, or a * null pointer if 'node' is the last node in
 * the 'hmap' slice.
 *
 */
static inline struct hmap_node *
parallel_hmap_next(
        const struct hmap *hmap,
        const struct hmap_node *node,
        ssize_t pool_size)
{
    return (node->next
            ? node->next
            : parallel_hmap_next__(hmap,
                (node->hash & hmap->mask) + pool_size, pool_size));
}

#ifdef  __cplusplus
}
#endif

#endif /* lib/fast-hmap.h */
