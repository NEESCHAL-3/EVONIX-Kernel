// SPDX-License-Identifier: GPL-2.0+
/*
 * EVONIX Kernel V4.6.1 - ADIOS Scheduling & Dispatch C Concurrency Test Harness
 *
 * Direct C validation of ADIOS request dispatch state machine, anti-starvation
 * quota enforcement, counter-reset linearization, forced-dispatch fallback,
 * and multi-threaded concurrency simulating UFS MCQ hardware contexts.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

#define ADIOS_PQ_LEVELS 2
#define ADIOS_STATE_PQ  0
#define ADIOS_STATE_BQ  2
#define ADIOS_STATE_DL  4

#define READ_ONCE(x) (*(const volatile __typeof__(x) *)&(x))

/* Doubly-linked list implementation mirroring Linux kernel list_head */
struct list_head {
	struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *list) {
	list->next = list;
	list->prev = list;
}

static inline bool list_empty(const struct list_head *head) {
	return head->next == head;
}

static inline void __list_add(struct list_head *new_node,
			      struct list_head *prev,
			      struct list_head *next) {
	next->prev = new_node;
	new_node->next = next;
	new_node->prev = prev;
	prev->next = new_node;
}

static inline void list_add_tail(struct list_head *new_node, struct list_head *head) {
	__list_add(new_node, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next) {
	next->prev = prev;
	prev->next = next;
}

static inline void list_del_init(struct list_head *entry) {
	__list_del(entry->prev, entry->next);
	INIT_LIST_HEAD(entry);
}

#define list_entry(ptr, type, member) \
	((type *)((char *)(ptr) - (size_t)(&((type *)0)->member)))

#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)

/* Mock kernel request structure */
struct request {
	struct list_head queuelist;
	uint32_t id;
	uint8_t tier; /* 0: Tier-0 (emergency), 1: Tier-1 (foreground read), 2: Normal (BQ) */
};

/* ADIOS scheduler data structure under test */
struct adios_data {
	pthread_spinlock_t pq_lock;
	struct list_head prio_queue[ADIOS_PQ_LEVELS];
	uint32_t pq1_dispatch_count;

	pthread_spinlock_t bq_lock;
	struct list_head batch_queue;

	atomic_uint state;
	bool bq_throttled;
};

static inline uint32_t eval_adios_state(struct adios_data *ad, uint8_t shift) {
	return (atomic_load_explicit(&ad->state, memory_order_acquire) >> shift) & 0x3;
}

static inline uint32_t get_adios_state(struct adios_data *ad) {
	return atomic_load_explicit(&ad->state, memory_order_acquire);
}

static inline void set_adios_state(struct adios_data *ad, uint8_t shift, uint8_t idx, bool set) {
	uint32_t mask = 1U << (shift + idx);
	if (set)
		atomic_fetch_or_explicit(&ad->state, mask, memory_order_release);
	else
		atomic_fetch_and_explicit(&ad->state, ~mask, memory_order_release);
}

/*
 * Reset Tier-1 dispatch count under pq_lock when a normal (BQ) request is dispatched.
 * Unconditionally holding pq_lock serializes counter reset with concurrent PQ dispatches
 * across multiple hardware contexts (UFS MCQ), eliminating race windows and guaranteeing
 * strict linearization of anti-starvation quota tracking.
 */
static inline void adios_reset_pq1_dispatch_count(struct adios_data *ad) {
	pthread_spin_lock(&ad->pq_lock);
	ad->pq1_dispatch_count = 0;
	pthread_spin_unlock(&ad->pq_lock);
}

/* Dispatch a request from priority queue */
static struct request *dispatch_from_pq(struct adios_data *ad, bool force) {
	struct request *rq = NULL;
	uint32_t pq_state;

	pq_state = eval_adios_state(ad, ADIOS_STATE_PQ);
	if (!pq_state)
		return NULL;

	if (!force && READ_ONCE(ad->pq1_dispatch_count) >= 8 && !(pq_state & 1)) {
		uint32_t state = get_adios_state(ad);
		if (state & ((3U << ADIOS_STATE_BQ) | (3U << ADIOS_STATE_DL)))
			return NULL;
	}

	pthread_spin_lock(&ad->pq_lock);

	pq_state = eval_adios_state(ad, ADIOS_STATE_PQ);
	if (!pq_state) {
		pthread_spin_unlock(&ad->pq_lock);
		return NULL;
	}

	uint8_t pq_idx = !(pq_state & 1);
	if (!force && pq_idx == 1 && ad->pq1_dispatch_count >= 8) {
		uint32_t state = get_adios_state(ad);
		if (state & ((3U << ADIOS_STATE_BQ) | (3U << ADIOS_STATE_DL))) {
			pthread_spin_unlock(&ad->pq_lock);
			return NULL;
		}
	}

	struct list_head *q = &ad->prio_queue[pq_idx];
	if (list_empty(q)) {
		pthread_spin_unlock(&ad->pq_lock);
		return NULL;
	}

	rq = list_first_entry(q, struct request, queuelist);
	list_del_init(&rq->queuelist);
	if (list_empty(q))
		set_adios_state(ad, ADIOS_STATE_PQ, pq_idx, false);

	if (pq_idx == 1) {
		if (list_empty(q))
			ad->pq1_dispatch_count = 0;
		else if (ad->pq1_dispatch_count < 8)
			ad->pq1_dispatch_count++;
	} else {
		ad->pq1_dispatch_count = 0;
	}

	pthread_spin_unlock(&ad->pq_lock);
	return rq;
}

/* Dispatch a request from batch queue */
static struct request *dispatch_from_bq(struct adios_data *ad) {
	struct request *rq = NULL;

	pthread_spin_lock(&ad->bq_lock);

	if (ad->bq_throttled || list_empty(&ad->batch_queue)) {
		pthread_spin_unlock(&ad->bq_lock);
		return NULL;
	}

	rq = list_first_entry(&ad->batch_queue, struct request, queuelist);
	list_del_init(&rq->queuelist);
	if (list_empty(&ad->batch_queue))
		set_adios_state(ad, ADIOS_STATE_BQ, 0, false);

	pthread_spin_unlock(&ad->bq_lock);
	return rq;
}

/* Top-level dispatch request */
static struct request *adios_dispatch_request(struct adios_data *ad) {
	struct request *rq;

	rq = dispatch_from_pq(ad, false);
	if (rq)
		return rq;

	rq = dispatch_from_bq(ad);
	if (rq) {
		adios_reset_pq1_dispatch_count(ad);
		return rq;
	}

	if (READ_ONCE(ad->pq1_dispatch_count) >= 8) {
		rq = dispatch_from_pq(ad, true);
		if (rq)
			return rq;
	}

	return NULL;
}

static void adios_init_test(struct adios_data *ad) {
	memset(ad, 0, sizeof(*ad));
	pthread_spin_init(&ad->pq_lock, PTHREAD_PROCESS_PRIVATE);
	pthread_spin_init(&ad->bq_lock, PTHREAD_PROCESS_PRIVATE);
	INIT_LIST_HEAD(&ad->prio_queue[0]);
	INIT_LIST_HEAD(&ad->prio_queue[1]);
	INIT_LIST_HEAD(&ad->batch_queue);
	atomic_init(&ad->state, 0);
	ad->bq_throttled = false;
	ad->pq1_dispatch_count = 0;
}

static void test_sequential_anti_starvation(void) {
	printf("[TEST 1] Sequential Anti-Starvation Quota & Linearized Reset...\n");
	struct adios_data ad;
	adios_init_test(&ad);

	struct request pq_rqs[20];
	struct request bq_rqs[10];

	// Enqueue 20 Tier-1 requests
	pthread_spin_lock(&ad.pq_lock);
	for (int i = 0; i < 20; i++) {
		INIT_LIST_HEAD(&pq_rqs[i].queuelist);
		pq_rqs[i].id = 100 + i;
		pq_rqs[i].tier = 1;
		list_add_tail(&pq_rqs[i].queuelist, &ad.prio_queue[1]);
	}
	set_adios_state(&ad, ADIOS_STATE_PQ, 1, true);
	pthread_spin_unlock(&ad.pq_lock);

	// Enqueue 10 Normal requests
	pthread_spin_lock(&ad.bq_lock);
	for (int i = 0; i < 10; i++) {
		INIT_LIST_HEAD(&bq_rqs[i].queuelist);
		bq_rqs[i].id = 200 + i;
		bq_rqs[i].tier = 2;
		list_add_tail(&bq_rqs[i].queuelist, &ad.batch_queue);
	}
	set_adios_state(&ad, ADIOS_STATE_BQ, 0, true);
	pthread_spin_unlock(&ad.bq_lock);

	// First 8 dispatches MUST be Tier-1
	for (int i = 0; i < 8; i++) {
		struct request *rq = adios_dispatch_request(&ad);
		assert(rq != NULL);
		assert(rq->tier == 1);
		assert(ad.pq1_dispatch_count == (uint32_t)(i + 1));
	}
	assert(ad.pq1_dispatch_count == 8);

	// 9th dispatch MUST be Normal (BQ) because Tier-1 yields at quota=8
	struct request *rq9 = adios_dispatch_request(&ad);
	assert(rq9 != NULL);
	assert(rq9->tier == 2);
	// Counter MUST be linearized back to 0 under pq_lock
	assert(ad.pq1_dispatch_count == 0);

	// Next 8 dispatches MUST be Tier-1 again
	for (int i = 0; i < 8; i++) {
		struct request *rq = adios_dispatch_request(&ad);
		assert(rq != NULL);
		assert(rq->tier == 1);
		assert(ad.pq1_dispatch_count == (uint32_t)(i + 1));
	}
	assert(ad.pq1_dispatch_count == 8);

	// Next dispatch MUST be Normal (BQ) again
	struct request *rq18 = adios_dispatch_request(&ad);
	assert(rq18 != NULL);
	assert(rq18->tier == 2);
	assert(ad.pq1_dispatch_count == 0);

	printf("[PASS] Test 1 completed successfully.\n");
}

static void test_forced_dispatch_fallback(void) {
	printf("[TEST 2] Forced Dispatch Fallback & Bounded Starvation Recovery...\n");
	struct adios_data ad;
	adios_init_test(&ad);

	struct request pq_rqs[15];
	struct request bq_rqs[5];

	// Enqueue 15 Tier-1 requests
	pthread_spin_lock(&ad.pq_lock);
	for (int i = 0; i < 15; i++) {
		INIT_LIST_HEAD(&pq_rqs[i].queuelist);
		pq_rqs[i].id = 100 + i;
		pq_rqs[i].tier = 1;
		list_add_tail(&pq_rqs[i].queuelist, &ad.prio_queue[1]);
	}
	set_adios_state(&ad, ADIOS_STATE_PQ, 1, true);
	pthread_spin_unlock(&ad.pq_lock);

	// Enqueue 5 Normal requests, but mark BQ as temporarily throttled
	pthread_spin_lock(&ad.bq_lock);
	for (int i = 0; i < 5; i++) {
		INIT_LIST_HEAD(&bq_rqs[i].queuelist);
		bq_rqs[i].id = 200 + i;
		bq_rqs[i].tier = 2;
		list_add_tail(&bq_rqs[i].queuelist, &ad.batch_queue);
	}
	set_adios_state(&ad, ADIOS_STATE_BQ, 0, true);
	ad.bq_throttled = true;
	pthread_spin_unlock(&ad.bq_lock);

	// Dispatches 1 through 8
	for (int i = 0; i < 8; i++) {
		struct request *rq = adios_dispatch_request(&ad);
		assert(rq != NULL && rq->tier == 1);
	}
	assert(ad.pq1_dispatch_count == 8);

	// Dispatch 9: BQ is throttled (returns NULL). Fallback triggers force=true!
	struct request *rq9 = adios_dispatch_request(&ad);
	assert(rq9 != NULL);
	assert(rq9->tier == 1);
	// pq1_dispatch_count must remain clamped at 8 without overflow
	assert(ad.pq1_dispatch_count == 8);

	// Dispatch 10: Still throttled, another forced dispatch succeeds
	struct request *rq10 = adios_dispatch_request(&ad);
	assert(rq10 != NULL && rq10->tier == 1);
	assert(ad.pq1_dispatch_count == 8);

	// Now unthrottle BQ (latency backoff subsided)
	pthread_spin_lock(&ad.bq_lock);
	ad.bq_throttled = false;
	pthread_spin_unlock(&ad.bq_lock);

	// Dispatch 11: Because pq1_dispatch_count is still 8, dispatch_from_pq yields,
	// and dispatch_from_bq IMMEDIATELY succeeds!
	struct request *rq11 = adios_dispatch_request(&ad);
	assert(rq11 != NULL);
	assert(rq11->tier == 2);
	// Counter is reset to 0
	assert(ad.pq1_dispatch_count == 0);

	printf("[PASS] Test 2 completed successfully.\n");
}

static void test_empty_queue_and_tier0(void) {
	printf("[TEST 3] Empty Queue Reset & Tier-0 Emergency Priority...\n");
	struct adios_data ad;
	adios_init_test(&ad);

	struct request pq1_rqs[5];
	struct request pq0_rq;

	// Enqueue 5 Tier-1 requests
	pthread_spin_lock(&ad.pq_lock);
	for (int i = 0; i < 5; i++) {
		INIT_LIST_HEAD(&pq1_rqs[i].queuelist);
		pq1_rqs[i].id = 100 + i;
		pq1_rqs[i].tier = 1;
		list_add_tail(&pq1_rqs[i].queuelist, &ad.prio_queue[1]);
	}
	set_adios_state(&ad, ADIOS_STATE_PQ, 1, true);
	pthread_spin_unlock(&ad.pq_lock);

	// Dispatch 4
	for (int i = 0; i < 4; i++) {
		struct request *rq = adios_dispatch_request(&ad);
		assert(rq != NULL && rq->tier == 1);
		assert(ad.pq1_dispatch_count == (uint32_t)(i + 1));
	}
	assert(ad.pq1_dispatch_count == 4);

	// Enqueue Tier-0 emergency head request
	pthread_spin_lock(&ad.pq_lock);
	INIT_LIST_HEAD(&pq0_rq.queuelist);
	pq0_rq.id = 999;
	pq0_rq.tier = 0;
	list_add_tail(&pq0_rq.queuelist, &ad.prio_queue[0]);
	set_adios_state(&ad, ADIOS_STATE_PQ, 0, true);
	pthread_spin_unlock(&ad.pq_lock);

	// Next dispatch MUST be Tier-0 and MUST reset pq1_dispatch_count to 0
	struct request *rq_t0 = adios_dispatch_request(&ad);
	assert(rq_t0 != NULL);
	assert(rq_t0->tier == 0);
	assert(ad.pq1_dispatch_count == 0);

	// Dispatch final Tier-1 request (drains queue)
	struct request *rq_last = adios_dispatch_request(&ad);
	assert(rq_last != NULL);
	assert(rq_last->tier == 1);
	// Drained queue MUST reset pq1_dispatch_count to 0
	assert(ad.pq1_dispatch_count == 0);

	printf("[PASS] Test 3 completed successfully.\n");
}

#define NUM_WORKERS 8
#define TOTAL_PER_WORKER 10000

struct worker_arg {
	struct adios_data *ad;
	int worker_id;
	uint64_t dispatched_pq1;
	uint64_t dispatched_bq;
};

static void *concurrency_worker(void *data) {
	struct worker_arg *arg = (struct worker_arg *)data;
	struct adios_data *ad = arg->ad;

	for (int i = 0; i < TOTAL_PER_WORKER; i++) {
		struct request *rq = adios_dispatch_request(ad);
		if (rq) {
			if (rq->tier == 1)
				arg->dispatched_pq1++;
			else if (rq->tier == 2)
				arg->dispatched_bq++;
			free(rq);
		} else {
			usleep(10);
		}
	}
	return NULL;
}

static void test_multithreaded_concurrency(void) {
	printf("[TEST 4] 8-Thread UFS MCQ Multi-Context Concurrency Stress Test...\n");
	struct adios_data ad;
	adios_init_test(&ad);

	int total_requests = NUM_WORKERS * TOTAL_PER_WORKER;
	int half = total_requests / 2;

	// Pre-populate queues
	pthread_spin_lock(&ad.pq_lock);
	for (int i = 0; i < half; i++) {
		struct request *rq = malloc(sizeof(struct request));
		INIT_LIST_HEAD(&rq->queuelist);
		rq->id = i;
		rq->tier = 1;
		list_add_tail(&rq->queuelist, &ad.prio_queue[1]);
	}
	set_adios_state(&ad, ADIOS_STATE_PQ, 1, true);
	pthread_spin_unlock(&ad.pq_lock);

	pthread_spin_lock(&ad.bq_lock);
	for (int i = 0; i < half; i++) {
		struct request *rq = malloc(sizeof(struct request));
		INIT_LIST_HEAD(&rq->queuelist);
		rq->id = half + i;
		rq->tier = 2;
		list_add_tail(&rq->queuelist, &ad.batch_queue);
	}
	set_adios_state(&ad, ADIOS_STATE_BQ, 0, true);
	pthread_spin_unlock(&ad.bq_lock);

	pthread_t threads[NUM_WORKERS];
	struct worker_arg args[NUM_WORKERS];

	for (int i = 0; i < NUM_WORKERS; i++) {
		args[i].ad = &ad;
		args[i].worker_id = i;
		args[i].dispatched_pq1 = 0;
		args[i].dispatched_bq = 0;
		pthread_create(&threads[i], NULL, concurrency_worker, &args[i]);
	}

	uint64_t total_pq1 = 0;
	uint64_t total_bq = 0;
	for (int i = 0; i < NUM_WORKERS; i++) {
		pthread_join(threads[i], NULL);
		total_pq1 += args[i].dispatched_pq1;
		total_bq += args[i].dispatched_bq;
	}

	printf("  Total Dispatched: PQ1=%lu, BQ=%lu, Total=%lu (Expected %d)\n",
	       total_pq1, total_bq, total_pq1 + total_bq, total_requests);

	assert(total_pq1 == (uint64_t)half);
	assert(total_bq == (uint64_t)half);
	assert(ad.pq1_dispatch_count == 0);
	assert(list_empty(&ad.prio_queue[1]));
	assert(list_empty(&ad.batch_queue));

	printf("[PASS] Test 4 completed successfully.\n");
}

int main(void) {
	printf("=================================================================\n");
	printf("EVONIX Kernel V4.6.1 - ADIOS Dispatch C Concurrency Test Suite\n");
	printf("=================================================================\n");

	test_sequential_anti_starvation();
	test_forced_dispatch_fallback();
	test_empty_queue_and_tier0();
	test_multithreaded_concurrency();

	printf("=================================================================\n");
	printf("ALL C ADIOS DISPATCH TESTS PASSED PERFECTLY.\n");
	printf("=================================================================\n");
	return 0;
}
