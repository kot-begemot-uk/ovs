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
void hmap_merge(struct hmap *dest, struct hmap *inc);
void run_pool(
    struct worker_pool *pool);
void run_pool_hash(
    struct worker_pool *pool, struct hmap *result, struct hmap *result_frags);
void run_pool_list(
    struct worker_pool *pool, struct ovs_list **result,
    struct ovs_list **result_frags);


/* Returns the first node in 'hmap' in the bucket in which the given 'hash'
 * would land, or a null pointer if that bucket is empty. */
static inline struct hmap_node *
hmap_first_in_bucket_num(const struct hmap *hmap, size_t num)
{
    return hmap->buckets[num];
}

#ifdef  __cplusplus
}
#endif

#endif /* lib/fast-hmap.h */
