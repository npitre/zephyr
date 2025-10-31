/*
 * Copyright (c) 2025 BayLibre SAS
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <stdint.h>

/*
 * FVP Memory Ordering Test Case
 *
 * This test demonstrates a memory ordering issue on FVP where LDAR instructions
 * appear to not trigger expected cache invalidation, causing significant
 * performance degradation in lockfree algorithms.
 *
 * The atomic operations use conditional compilation to select appropriate
 * ARM64 instructions based on architecture:
 * - ARMv8.1-A+: Uses LSE atomics (LDADDAL, SWPAL) when __ARM_FEATURE_ATOMICS defined
 * - ARMv8.0-A:  Uses load-exclusive/store-exclusive (LDAXR, STLXR) otherwise
 *
 * Both instruction sets exhibit similar behavior on FVP, suggesting the issue
 * is related to LDAR and cache coherency rather than specific instruction emulation.
 *
 * Barrier control - set each to 0 or 1 to experiment:
 * BARRIER_AFTER_WRITE:  DMB after atomic writes
 * BARRIER_BEFORE_READ:  DMB before atomic reads
 */

#define BARRIER_AFTER_WRITE 0
#define BARRIER_BEFORE_READ 0

#define FREEQ_SZ 2
#define ITERATIONS 10000
#define STACK_SIZE (512 + CONFIG_TEST_EXTRA_STACK_SIZE)
#define NUM_PRODUCERS 2
#define NUM_THREADS (NUM_PRODUCERS + 1)

/* ========================================================================
 * Local Atomic Operations - ARM64 Assembly
 * ======================================================================== */

typedef struct { unsigned long v; } local_atomic_t;
typedef struct { void *v; } local_atomic_ptr_t;

/* local_atomic_get - Load-Acquire (LDAR) */
static inline unsigned long local_atomic_get(const local_atomic_t *target)
{
	unsigned long ret;
	__asm__ __volatile__(
		"ldar %0, [%1]"
		: "=r" (ret)
		: "r" (&target->v)
		: "memory"
	);
	return ret;
}

/* local_atomic_set - Store-Release via atomic exchange */
static inline unsigned long local_atomic_set(local_atomic_t *target, unsigned long value)
{
	unsigned long ret;
#ifdef __ARM_FEATURE_ATOMICS
	/* ARMv8.1-A: Use SWPAL (Swap with Acquire/Release) */
	__asm__ __volatile__(
		"swpal %0, %0, [%1]"
		: "=r" (ret)
		: "r" (&target->v), "0" (value)
		: "memory"
	);
#else
	/* ARMv8.0-A: Use LDAXR/STLXR (Load-Acquire Exclusive / Store-Release Exclusive) */
	unsigned long tmp;
	__asm__ __volatile__(
		"1:	ldaxr	%0, [%2]\n"
		"	stlxr	%w1, %3, [%2]\n"
		"	cbnz	%w1, 1b\n"
		: "=&r" (ret), "=&r" (tmp)
		: "r" (&target->v), "r" (value)
		: "memory"
	);
#endif
	return ret;
}

/* local_atomic_add - Atomic Add with Acquire/Release */
static inline unsigned long local_atomic_add(local_atomic_t *target, unsigned long value)
{
	unsigned long ret;
#ifdef __ARM_FEATURE_ATOMICS
	/* ARMv8.1-A: Use LDADDAL (Atomic Add with Acquire/Release) */
	__asm__ __volatile__(
		"ldaddal %0, %0, [%1]"
		: "=r" (ret)
		: "r" (&target->v), "0" (value)
		: "memory"
	);
#else
	/* ARMv8.0-A: Use LDAXR/STLXR loop */
	unsigned long tmp;
	__asm__ __volatile__(
		"1:	ldaxr	%0, [%2]\n"
		"	add	%0, %0, %3\n"
		"	stlxr	%w1, %0, [%2]\n"
		"	cbnz	%w1, 1b\n"
		"	sub	%0, %0, %3\n"
		: "=&r" (ret), "=&r" (tmp)
		: "r" (&target->v), "r" (value)
		: "memory"
	);
#endif
	return ret;
}

/* local_atomic_ptr_get - Load-Acquire pointer (LDAR) */
static inline void *local_atomic_ptr_get(const local_atomic_ptr_t *target)
{
	void *ret;
	__asm__ __volatile__(
		"ldar %0, [%1]"
		: "=r" (ret)
		: "r" (&target->v)
		: "memory"
	);
	return ret;
}

/* local_atomic_ptr_set - Store-Release pointer via atomic exchange */
static inline void *local_atomic_ptr_set(local_atomic_ptr_t *target, void *value)
{
	void *ret;
#ifdef __ARM_FEATURE_ATOMICS
	/* ARMv8.1-A: Use SWPAL (Swap with Acquire/Release) */
	__asm__ __volatile__(
		"swpal %0, %0, [%1]"
		: "=r" (ret)
		: "r" (&target->v), "0" (value)
		: "memory"
	);
#else
	/* ARMv8.0-A: Use LDAXR/STLXR (Load-Acquire Exclusive / Store-Release Exclusive) */
	unsigned long tmp;
	__asm__ __volatile__(
		"1:	ldaxr	%0, [%2]\n"
		"	stlxr	%w1, %3, [%2]\n"
		"	cbnz	%w1, 1b\n"
		: "=&r" (ret), "=&r" (tmp)
		: "r" (&target->v), "r" (value)
		: "memory"
	);
#endif
	return ret;
}

/* Memory barrier - DMB SY (full system barrier) */
static inline void dmb_sy(void)
{
	__asm__ __volatile__("dmb sy" ::: "memory");
}

/* ========================================================================
 * SPSC Queue (Single Producer Single Consumer)
 * ======================================================================== */

struct spsc_queue {
	unsigned long acquire;  /* Private to producer */
	unsigned long consume;  /* Private to consumer */
	local_atomic_t in;           /* Shared: producer writes, consumer reads */
	local_atomic_t out;          /* Shared: consumer writes, producer reads */
	unsigned long mask;    /* Ring buffer mask */
};

/* Forward declare mpsc_node */
struct mpsc_node {
	local_atomic_ptr_t next;
};

struct test_node {
	uint32_t id;
	struct mpsc_node mpsc_node;
};

static void spsc_init(struct spsc_queue *q)
{
	q->acquire = 0;
	q->consume = 0;
	local_atomic_set(&q->in, 0);
	local_atomic_set(&q->out, 0);
	q->mask = FREEQ_SZ - 1;
}

static struct test_node *spsc_consume(struct spsc_queue *q, struct test_node *buffer)
{
#if BARRIER_BEFORE_READ
	dmb_sy();  /* Ensure we see latest values */
#endif
	unsigned long out = local_atomic_get(&q->out);
	unsigned long in = local_atomic_get(&q->in);
	unsigned long idx = out + q->consume;

	if (idx != in) {
		q->consume += 1;
		return &buffer[idx & q->mask];
	}
	return NULL;
}

static void spsc_release(struct spsc_queue *q)
{
	if (q->consume > 0) {
		q->consume -= 1;
		local_atomic_add(&q->out, 1);
#if BARRIER_AFTER_WRITE
		dmb_sy();  /* Ensure write is visible to other CPUs */
#endif
	}
}

static struct test_node *spsc_acquire(struct spsc_queue *q, struct test_node *buffer)
{
#if BARRIER_BEFORE_READ
	dmb_sy();  /* Ensure we see latest values */
#endif
	unsigned long in = local_atomic_get(&q->in);
	unsigned long out = local_atomic_get(&q->out);
	unsigned long idx = in + q->acquire;

	if ((idx - out) < FREEQ_SZ) {
		q->acquire += 1;
		return &buffer[idx & q->mask];
	}
	return NULL;
}

static void spsc_produce(struct spsc_queue *q)
{
	if (q->acquire > 0) {
		q->acquire -= 1;
		local_atomic_add(&q->in, 1);
#if BARRIER_AFTER_WRITE
		dmb_sy();  /* Ensure write is visible to other CPUs */
#endif
	}
}

/* ========================================================================
 * MPSC Queue (Multiple Producer Single Consumer)
 * ======================================================================== */

struct mpsc_queue {
	local_atomic_ptr_t head;
	struct mpsc_node *tail;
	struct mpsc_node stub;
};

static void mpsc_init(struct mpsc_queue *q)
{
	local_atomic_ptr_set(&q->head, &q->stub);
	q->tail = &q->stub;
	local_atomic_ptr_set(&q->stub.next, NULL);
}

static void mpsc_push(struct mpsc_queue *q, struct mpsc_node *n)
{
	struct mpsc_node *prev;
	int key;

	local_atomic_ptr_set(&n->next, NULL);

	key = arch_irq_lock();
	prev = (struct mpsc_node *)local_atomic_ptr_set(&q->head, n);
	local_atomic_ptr_set(&prev->next, n);
	arch_irq_unlock(key);
#if BARRIER_AFTER_WRITE
	dmb_sy();  /* Ensure linked list update is visible */
#endif
}

static struct mpsc_node *mpsc_pop(struct mpsc_queue *q)
{
#if BARRIER_BEFORE_READ
	dmb_sy();  /* Ensure we see latest linked list structure */
#endif
	struct mpsc_node *head;
	struct mpsc_node *tail = q->tail;
	struct mpsc_node *next = (struct mpsc_node *)local_atomic_ptr_get(&tail->next);

	/* Skip over stub */
	if (tail == &q->stub) {
		if (next == NULL) {
			return NULL;
		}
		q->tail = next;
		tail = next;
		next = (struct mpsc_node *)local_atomic_ptr_get(&next->next);
	}

	/* If next is non-NULL, return tail */
	if (next != NULL) {
		q->tail = next;
		return tail;
	}

	head = (struct mpsc_node *)local_atomic_ptr_get(&q->head);

	/* If tail != head, updates are pending */
	if (tail != head) {
		return NULL;
	}

	/* Push stub to restart */
	mpsc_push(q, &q->stub);

	next = (struct mpsc_node *)local_atomic_ptr_get(&tail->next);

	if (next != NULL) {
		q->tail = next;
		return tail;
	}

	return NULL;
}

/* ========================================================================
 * Test Setup
 * ======================================================================== */

static struct test_node node_buffers[NUM_PRODUCERS + 1][FREEQ_SZ];
static struct spsc_queue free_queues[NUM_PRODUCERS + 1];
static struct mpsc_queue mpsc_q;

/*
 * Retry counters: Track how many times threads must re-attempt lockfree
 * operations when they cannot proceed (queue appears full/empty). This includes:
 * - Legitimate contention: Queue state genuinely prevents the operation
 * - Cache coherency issues: Stale cached values cause unnecessary retries
 *
 * On properly functioning platforms (QEMU), most retries are legitimate contention.
 * The baseline (~20K retries) represents expected behavior. On FVP, excessive
 * retries (~1M) beyond this baseline indicate cache coherency problems.
 */
static local_atomic_t consumer_mpsc_retries;
static local_atomic_t consumer_spsc_retries;
static local_atomic_t producer_retries[NUM_PRODUCERS + 1];

/* ========================================================================
 * Test Threads
 * ======================================================================== */

static void consumer_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t mpsc_retry_count = 0;
	uint32_t spsc_retry_count = 0;

	for (int i = 0; i < ITERATIONS * NUM_PRODUCERS; i++) {
		struct mpsc_node *n;

		/* Pop from MPSC queue */
		do {
			n = mpsc_pop(&mpsc_q);
			if (n == NULL) {
				mpsc_retry_count++;
				k_yield();
			}
		} while (n == NULL);

		zassert_not_equal(n, &mpsc_q.stub, "Should not get stub");

		struct test_node *node = CONTAINER_OF(n, struct test_node, mpsc_node);
		uint32_t producer_id = node->id;

		/* Return node to producer's free queue */
		struct test_node *acquired;
		do {
			acquired = spsc_acquire(&free_queues[producer_id],
						node_buffers[producer_id]);
			if (acquired == NULL) {
				spsc_retry_count++;
				k_yield();
			}
		} while (acquired == NULL);

		spsc_produce(&free_queues[producer_id]);
	}

	local_atomic_set(&consumer_mpsc_retries, mpsc_retry_count);
	local_atomic_set(&consumer_spsc_retries, spsc_retry_count);
	TC_PRINT("Consumer: %u mpsc retries, %u spsc retries\n",
		 mpsc_retry_count, spsc_retry_count);
}

static void producer_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t id = (uint32_t)(uintptr_t)p1;
	uint32_t retry_count = 0;

	for (int i = 0; i < ITERATIONS; i++) {
		struct test_node *node;

		/* Get node from free queue */
		do {
			node = spsc_consume(&free_queues[id], node_buffers[id]);
			if (node == NULL) {
				retry_count++;
				k_yield();
			}
		} while (node == NULL);

		spsc_release(&free_queues[id]);

		/* Fill in node data */
		node->id = id;

		/* Push to MPSC queue */
		mpsc_push(&mpsc_q, &node->mpsc_node);
	}

	local_atomic_set(&producer_retries[id], retry_count);
	TC_PRINT("Producer %u: %u spsc retries\n", id, retry_count);
}

/* ========================================================================
 * Test Entry Point
 * ======================================================================== */

static struct k_thread threads[NUM_THREADS];
static K_THREAD_STACK_ARRAY_DEFINE(thread_stacks, NUM_THREADS, STACK_SIZE);
static k_tid_t tids[NUM_THREADS];

ZTEST(fvp_ordering_bug, test_lockfree)
{
	TC_PRINT("=== BARRIER CONFIGURATION ===\n");
	TC_PRINT("BARRIER_AFTER_WRITE:  %d\n", BARRIER_AFTER_WRITE);
	TC_PRINT("BARRIER_BEFORE_READ:  %d\n", BARRIER_BEFORE_READ);
	TC_PRINT("=============================\n");
	TC_PRINT("Initializing queues\n");

	/* Initialize MPSC queue */
	mpsc_init(&mpsc_q);

	/* Initialize SPSC free queues and populate with nodes */
	for (int i = 1; i <= NUM_PRODUCERS; i++) {
		spsc_init(&free_queues[i]);

		/* Pre-populate free queue with all nodes */
		for (int j = 0; j < FREEQ_SZ; j++) {
			spsc_acquire(&free_queues[i], node_buffers[i]);
		}
		spsc_produce(&free_queues[i]);
		spsc_produce(&free_queues[i]);
	}

	TC_PRINT("Starting consumer\n");
	tids[0] = k_thread_create(&threads[0], thread_stacks[0], STACK_SIZE,
				   consumer_thread, NULL, NULL, NULL,
				   K_PRIO_PREEMPT(5), 0, K_NO_WAIT);

	for (int i = 1; i <= NUM_PRODUCERS; i++) {
		TC_PRINT("Starting producer %d\n", i);
		tids[i] = k_thread_create(&threads[i], thread_stacks[i], STACK_SIZE,
					   producer_thread, (void *)(uintptr_t)i, NULL, NULL,
					   K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	}

	for (int i = 0; i < NUM_THREADS; i++) {
		TC_PRINT("Joining thread %d\n", i);
		k_thread_join(tids[i], K_FOREVER);
	}

	/* Print statistics */
	TC_PRINT("\n=== RETRY STATISTICS ===\n");
	TC_PRINT("Consumer MPSC retries: %lu\n", local_atomic_get(&consumer_mpsc_retries));
	TC_PRINT("Consumer SPSC retries: %lu\n", local_atomic_get(&consumer_spsc_retries));
	for (int i = 1; i <= NUM_PRODUCERS; i++) {
		TC_PRINT("Producer %d SPSC retries: %lu\n", i, local_atomic_get(&producer_retries[i]));
	}

	uint32_t total = (uint32_t)local_atomic_get(&consumer_mpsc_retries) +
			 (uint32_t)local_atomic_get(&consumer_spsc_retries);
	for (int i = 1; i <= NUM_PRODUCERS; i++) {
		total += (uint32_t)local_atomic_get(&producer_retries[i]);
	}
	uint32_t operations = ITERATIONS * NUM_PRODUCERS * 2;
	float ratio = (float)total / operations;

	TC_PRINT("Total retries: %u for %u operations (%.2f per operation)\n",
		 total, operations, ratio);
	TC_PRINT("========================\n");

	/* Check for excessive retries indicating memory ordering issues.
	 * Expected behavior: ~0.2-0.5 retries per operation (QEMU/FVP with barriers)
	 * Memory ordering issue: ~27 retries per operation (FVP without barriers)
	 * Threshold: Fail if ratio > 2.0
	 */
	zassert_true(ratio < 2.0f,
		     "Excessive retries detected: %.2f per operation (expected < 1.0). "
		     "This suggests a memory ordering issue. "
		     "Try enabling BARRIER_BEFORE_READ=1", ratio);
}

ZTEST_SUITE(fvp_ordering_bug, NULL, NULL, NULL, NULL, NULL);
