// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : sync.h
 * @brief          : Synchronization Primitives (Mutex, Semaphore, CondVar)
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Synchronization primitives for the caOS operating system.
 * Includes implementations of mutexes, semaphores, condition variables, and other
 * primitives for managing concurrency between tasks. Supports both
 * blocking and non-blocking operations with timeouts.
 *
 * Typical sequence:
 * 1) Primitive initialization -> resource acquisition -> usage -> release
 ******************************************************************************
 */

#ifndef SYNC_H
#define SYNC_H

#include "common.h"
#include "atomic.h"
#include "scheduler.h"



/* ============================================================================
 * WAIT QUEUE (Blocked task list - In-Context Version)
 * ========================================================================== */

typedef struct {
    ca_thread_context_t *head;  // Points to the first task in the chain
    ca_thread_context_t *tail;  // Points to the last task in the chain
} ca_wait_queue_t;

/* ============================================================================
 * MUTEX (Binary Semaphore with Ownership)
 * ========================================================================== */

/* Priority inheritance: how many owners a boost or a drop follows down a
 * chain (T waits M1 owned by U, U waits M2 owned by V...). Every hop runs
 * with IRQs masked, so the walk is capped; a chain deeper than this is a
 * design problem of the application, not something to follow. */
#ifndef CA_PI_MAX_CHAIN
#define CA_PI_MAX_CHAIN  4U
#endif

typedef struct ca_mutex_s {
    volatile u32 owner;          // owner's ca_task_handle_t while locked (not a
                                 // thid: a position moves when a task below ends)
    volatile u8 locked;          // 0 = free, 1 = locked
    ca_wait_queue_t wait_queue;      // Tasks waiting
    ca_spinlock_irq_t internal_lock; // Protects internal structure
    struct ca_mutex_s *pi_next;   // next mutex in the owner's pi_held list
} ca_mutex_t;

/** @brief Re-evaluates a task's effective priority after its prio_base
 *         changed or one of its waiters left; IRQs masked by the caller */
void ca_mutex_pi_refresh(ca_thread_context_t *ctx);

/** @brief A task leaving a mutex wait list from outside the mutex (removed,
 *         faulted): its boost is taken back from the owner; IRQs masked */
void ca_mutex_pi_waiter_gone(ca_thread_context_t *ctx);

/**
 * @brief Initialize mutex
 * @param mtx Pointer to the mutex
 */
void ca_mutex_init(ca_mutex_t *mtx);

/**
 * @brief Acquire mutex (blocks if occupied)
 * @param mtx Pointer to the mutex
 * @note If mutex is occupied, task is blocked and scheduled out
 */
void ca_mutex_lock(ca_mutex_t *mtx);

/**
 * @brief Attempt to acquire mutex without blocking
 * @param mtx Pointer to the mutex
 * @return true if acquired, false if occupied
 */
bool ca_mutex_trylock(ca_mutex_t *mtx);

/**
 * @brief Release mutex
 * @param mtx Pointer to the mutex
 * @note Only the owner can release the mutex
 */
void ca_mutex_unlock(ca_mutex_t *mtx);

/**
 * @brief Check if current task owns the mutex
 * @param mtx Pointer to the mutex
 * @return true if owner
 */
bool ca_mutex_is_owner(ca_mutex_t *mtx);

/**
 * @brief Acquire mutex with timeout
 * @param mtx Pointer to the mutex
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return true if acquired, false if timeout
 */
bool ca_mutex_timedlock(ca_mutex_t *mtx, u32 timeout_ms);

/* ============================================================================
 * RECURSIVE MUTEX (Mutex with reentrancy counter)
 * ========================================================================== */

typedef struct {
    volatile u32 owner;          // owner's ca_task_handle_t while lock_count > 0
    volatile u16 lock_count;     // Reentrancy counter (0 = free)
    ca_wait_queue_t wait_queue;      // Tasks waiting
    ca_spinlock_irq_t internal_lock; // Protects internal structure
} ca_recursive_mutex_t;

/**
 * @brief Initialize recursive mutex
 * @param rmtx Pointer to the recursive mutex
 */
void ca_rmutex_init(ca_recursive_mutex_t *rmtx);

/**
 * @brief Acquire recursive mutex (blocks if occupied by another task)
 * @param rmtx Pointer to the recursive mutex
 * @note If already owner, increments counter (no deadlock)
 */
void ca_rmutex_lock(ca_recursive_mutex_t *rmtx);

/**
 * @brief Attempt to acquire recursive mutex without blocking
 * @param rmtx Pointer to the recursive mutex
 * @return true if acquired (or already owner), false if occupied by another
 */
bool ca_rmutex_trylock(ca_recursive_mutex_t *rmtx);

/**
 * @brief Release recursive mutex
 * @param rmtx Pointer to the recursive mutex
 * @note Decrements counter, releases only when count = 0
 */
void ca_rmutex_unlock(ca_recursive_mutex_t *rmtx);

/**
 * @brief Check if current task owns the recursive mutex
 * @param rmtx Pointer to the recursive mutex
 * @return true if owner
 */
bool ca_rmutex_is_owner(ca_recursive_mutex_t *rmtx);

/**
 * @brief Acquire recursive mutex with timeout
 * @param rmtx Pointer to the recursive mutex
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return true if acquired (or already owner), false if timeout
 */
bool ca_rmutex_timedlock(ca_recursive_mutex_t *rmtx, u32 timeout_ms);

/**
 * @brief Get reentrancy counter
 * @param rmtx Pointer to the recursive mutex
 * @return Number of locks acquired (0 if free)
 */
u16 ca_rmutex_get_count(ca_recursive_mutex_t *rmtx);

/* ============================================================================
 * SEMAPHORE (Counting Semaphore)
 * ========================================================================== */

typedef union ca_sem_name_u{
    u8 name_c[8];
    u64 name_l;
} ca_sem_name_t;


typedef struct __attribute__((aligned(4))) {
    volatile u32 count;          // Available resource counter
    u32 max_count;               // Maximum value
    ca_wait_queue_t wait_queue;      // Tasks waiting
    ca_spinlock_irq_t internal_lock; // Protects internal structure
    ca_sem_name_t name;          // added for debug
} ca_semaphore_t;

/**
 * @brief Initialize semaphore
 * @param sem Pointer to the semaphore
 * @param initial_count Initial value
 * @param max_count Maximum value (0 = unlimited)
 */
void ca_sem_init(ca_semaphore_t *sem, const char *name,u32 initial_count, u32 max_count);

/**
 * @brief Decrement semaphore (wait/P operation)
 * @param sem Pointer to the semaphore
 * @note If count = 0, task is blocked
 */
void ca_sem_wait(ca_semaphore_t *sem);

/**
 * @brief Attempt to decrement without blocking
 * @param sem Pointer to the semaphore
 * @return true if decremented, false if count was 0
 */
bool ca_sem_trywait(ca_semaphore_t *sem);

/**
 * @brief Decrement semaphore with timeout
 * @param sem Pointer to the semaphore
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return true if decremented, false if timeout
 */
bool ca_sem_timedwait(ca_semaphore_t *sem, u32 timeout_ms);

/**
 * @brief Increment semaphore (signal/V operation)
 * @param sem Pointer to the semaphore
 * @note Wakes up first waiting task, or increments count
 */
void ca_sem_post(ca_semaphore_t *sem);
void ca_sem_post_capped(ca_semaphore_t *sem); /* ISR notification: silently caps at max */

/**
 * @brief Reads current semaphore value
 * @param sem Pointer to the semaphore
 * @return Current count value
 */
u32 ca_sem_getvalue(ca_semaphore_t *sem);

/* ============================================================================
 * CONDITION VARIABLE (for wait/notify pattern)
 * ========================================================================== */

typedef struct {
    ca_wait_queue_t wait_queue;      // Task waiting
    ca_spinlock_irq_t  internal_lock; // Protects internal structure
} ca_condvar_t;

/**
 * @brief Initializes condition variable
 * @param cv Pointer to the condition variable
 */
void ca_cond_init(ca_condvar_t *cv);

/**
 * @brief Waits on condition variable
 * @param cv Pointer to the condition variable
 * @param mtx Associated mutex (MUST be locked by the caller)
 * @note Atomically releases mutex and blocks task
 *       When awakened, re-acquires mutex before returning
 */
void ca_cond_wait(ca_condvar_t *cv, ca_mutex_t *mtx);

/**
 * @brief Waits on condition variable with timeout
 * @param cv Pointer to the condition variable
 * @param mtx Associated mutex
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return true if awakened by signal, false if timeout
 */
bool ca_cond_timedwait(ca_condvar_t *cv, ca_mutex_t *mtx, u32 timeout_ms);

/**
 * @brief Wakes up a waiting task
 * @param cv Pointer to the condition variable
 * @note Wakes up ONLY ONE task (FIFO)
 */
void ca_cond_signal(ca_condvar_t *cv);

/**
 * @brief Wakes up all waiting tasks
 * @param cv Pointer to the condition variable
 * @note Wakes up ALL waiting tasks
 */
void ca_cond_broadcast(ca_condvar_t *cv);

/* ============================================================================
 * CRITICAL SECTION (IRQ disable with nesting counter)
 * ========================================================================== */

/**
 * @brief Enters critical section (disables IRQ)
 * @note Supports nesting - only first call disables IRQ
 * @note Lighter than spinlock for short single-core sections
 */
void ca_enter_critical(void);

/**
 * @brief Exits critical section
 * @note Re-enables IRQ only when nesting count returns to 0
 */
void ca_exit_critical(void);

/**
 * @brief Returns current nesting level
 * @return Number of pending enter_critical
 */
u32 ca_critical_nesting_level(void);

/* ============================================================================
 * READ-WRITE LOCK (N simultaneous readers, exclusive writer)
 * ========================================================================== */

#if EN_RWLOCK == 1

typedef struct {
    volatile u16  readers;       /* active readers */
    volatile u8   writer_active; /* writer in progress */
    ca_wait_queue_t   read_queue;    /* waiting readers */
    ca_wait_queue_t   write_queue;   /* waiting writers */
    ca_spinlock_irq_t internal_lock;
} ca_rwlock_t;

/**
 * @brief Initializes read-write lock
 */
void ca_rwlock_init(ca_rwlock_t *rw);

/**
 * @brief Acquires read lock (blocks if writer is active or waiting)
 * @note Write-preferring strategy: waiting writers block new readers
 */
void ca_rwlock_read_lock(ca_rwlock_t *rw);

/**
 * @brief Releases read lock
 */
void ca_rwlock_read_unlock(ca_rwlock_t *rw);

/**
 * @brief Acquires write lock (blocks until readers > 0 or another writer)
 */
void ca_rwlock_write_lock(ca_rwlock_t *rw);

/**
 * @brief Releases write lock
 * @note Wakes up waiting writers (priority) or all waiting readers
 */
void ca_rwlock_write_unlock(ca_rwlock_t *rw);

#endif /* EN_RWLOCK */

/* ============================================================================
 * BARRIER (synchronization point for N tasks)
 * ========================================================================== */

#if EN_BARRIER == 1

typedef struct {
    volatile u32  count;         /* number of tasks to synchronize */
    volatile u32  arrived;       /* tasks arrived at the barrier in this phase */
    volatile u32  phase;         /* current phase (auto-incremented) */
    ca_wait_queue_t   wait_queue;
    ca_spinlock_irq_t internal_lock;
} ca_barrier_t;

/**
 * @brief Initializes barrier
 * @param b     Pointer to the barrier
 * @param count Number of tasks that must arrive before proceeding
 */
void ca_barrier_init(ca_barrier_t *b, u32 count);

/**
 * @brief Waits at the barrier until all tasks have arrived
 * @return true only for the last task (the "trigger")
 * @note The barrier resets automatically (reusable in loops)
 */
bool ca_barrier_wait(ca_barrier_t *b);

#endif /* EN_BARRIER */

/* ============================================================================
 * WAIT QUEUE OPERATIONS (internal helper functions)
 * ========================================================================== */

void ca_wait_queue_init(ca_wait_queue_t *wq);
void ca_wait_queue_enqueue(ca_wait_queue_t *wq, ca_thread_context_t *task);
ca_thread_context_t* ca_wait_queue_dequeue(ca_wait_queue_t *wq);
bool ca_wait_queue_remove(ca_wait_queue_t *wq, ca_thread_context_t *task);
bool ca_wait_queue_is_empty(ca_wait_queue_t *wq);



#endif /* SYNC_H */