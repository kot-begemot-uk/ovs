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

#ifndef OVS_PARALLEL_HMAP
#define OVS_PARALLEL_HMAP 1

/* if the parallel macros are defined by hmap.h or any other ovs define
 * we skip over the ovn specific definitions.
 */

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

/* A version of the HMAP_FOR_EACH macro intended for iterating as part
 * of parallel processing.
 * Each worker thread has a different ThreadID in the range of 0..POOL_SIZE
 * and will iterate hash buckets ThreadID, ThreadID + step,
 * ThreadID + step * 2, etc. The actual macro accepts
 * ThreadID + step * i as the JOBID parameter.
 */

#define HMAP_FOR_EACH_IN_PARALLEL(NODE, MEMBER, JOBID, HMAP) \
   for (INIT_CONTAINER(NODE, hmap_first_in_bucket_num(HMAP, JOBID), MEMBER); \
        (NODE != OBJECT_CONTAINING(NULL, NODE, MEMBER)) \
       || ((NODE = NULL), false); \
       ASSIGN_CONTAINER(NODE, hmap_next_in_bucket(&(NODE)->MEMBER), MEMBER))

/* We do not have a SAFE version of the macro, because the hash size is not
 * atomic and hash removal operations would need to be wrapped with
 * locks. This will defeat most of the benefits from doing anything in
 * parallel.
 * If the code block inside FOR_EACH_IN_PARALLEL needs to remove elements,
 * each thread should store them in a temporary list result instead, merging
 * the lists into a combined result at the end */

/* Work "Handle" */

struct worker_control {
    int id; /* Used as a modulo when iterating over a hash. */
    atomic_bool finished; /* Set to true after achunk of work is complete. */
    sem_t *fire; /* Work start semaphore - sem_post starts the worker. */
    sem_t *done; /* Work completion semaphore - sem_post on completion. */
    struct ovs_mutex mutex; /* Guards the data. */
    void *data; /* Pointer to data to be processed. */
    void *workload; /* back-pointer to the worker pool structure. */
};

struct worker_pool {
    int size;   /* Number of threads in the pool. */
    struct ovs_list list_node; /* List of pools - used in cleanup/exit. */
    struct worker_control *controls; /* "Handles" in this pool. */
    sem_t *done; /* Work completion semaphorew. */
};

/* Add a worker pool for thread function start() which expects a pointer to
 * a worker_control structure as an argument. */

struct worker_pool *add_worker_pool(void *(*start)(void *));

/* Setting this to true will make all processing threads exit */

bool stop_parallel_processing(void);

/* Build a hmap pre-sized for size elements */

void fast_hmap_size_for(struct hmap *hmap, int size);

/* Build a hmap with a mask equals to size */

void fast_hmap_init(struct hmap *hmap, ssize_t size);

/* Brute-force merge a hmap into hmap.
 * Dest and inc have to have the same mask. The merge is performed
 * by extending the element list for bucket N in the dest hmap with the list
 * from bucket N in inc.
 */

void fast_hmap_merge(struct hmap *dest, struct hmap *inc);

/* Run a pool, without any default processing of results.
 */

void run_pool(struct worker_pool *pool);

/* Run a pool, merge results from hash frags into a final hash result.
 * The hash frags must be pre-sized to the same size.
 */

void run_pool_hash(struct worker_pool *pool,
                   struct hmap *result, struct hmap *result_frags);

/* Run a pool, merge results from list frags into a final list result.
 */

void run_pool_list(struct worker_pool *pool,
                    struct ovs_list *result, struct ovs_list *result_frags);

/* Run a pool, call a callback function to perform processing of results.
 */

void run_pool_callback(struct worker_pool *pool, void *fin_result,
                       void *result_frags,
                       void (*helper_func)(struct worker_pool *pool,
                                           void *fin_result, void *result_frags,
                                           int index));


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
parallel_hmap_next(const struct hmap *hmap,
                   const struct hmap_node *node, ssize_t pool_size)
{
    return (node->next
            ? node->next
            : parallel_hmap_next__(hmap,
                (node->hash & hmap->mask) + pool_size, pool_size));
}

static inline void post_completed_work(struct worker_control *control)
{
    atomic_store_relaxed(&control->finished, true);
    atomic_thread_fence(memory_order_acq_rel);
    sem_post(control->done);
}

bool can_parallelize_hashes(void);

#ifdef  __cplusplus
}
#endif


#endif /* lib/ovs-fasthmap.h */
