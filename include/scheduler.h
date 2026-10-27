// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : scheduler.h
 * @brief          : RTOS Scheduler for STM32F407 (Cortex-M4)
 * @author         : Angelo Coppi - Ported from CAOS (Raspberry Pi)
 ******************************************************************************
 * @attention
 *
 * This scheduler has been converted from C++ to C and adapted for Cortex-M4.
 * Supports:
 * - Round-Robin and Priority-based scheduling
 * - Task Sleep/WakeUp
 * - Dynamic priority management
 * - Context switch via PendSV interrupt
 *
 * Differences compared to the original Cortex-A (Raspberry) version:
 * - PCB reduced to 8 registers (R4-R11) instead of 32
 * - No support for processor modes (USR, FIQ, IRQ, SVC, etc.)
 * - Context switch via PendSV instead of custom assembly
 * - PSP stack instead of banked registers
 *
 ******************************************************************************
 */

#ifndef SCHEDULER_H_
#define SCHEDULER_H_

#include "common.h"

/* =============================================================================
 * SCHEDULER CONFIGURATION
 * ============================================================================= */



#define STACK_FILL_PATTERN  0xDEADC0DE  /* Pattern for watermark */

/*STANDARD THREAD CHECK IDLE CHECK ONLY CANARY VALUE*/
#define TASK_CANARY_VALUE_STD         0xCACAFEF8
/*STANDARD THREAD CHECK IDLE CHECK PSP LEN + CANARY VALUE*/
#define TASK_CANARY_VALUE_SAFE        0xCACAFEFC
#if __FPU_PRESENT
/*STANDARD THREAD CHECK IDLE CHECK ONLY CANARY VALUE+SAVE FLOAT POINT REGISTERS*/
#define TASK_CANARY_VALUE_STD_FP      0xCACAFEF9
/*STANDARD THREAD CHECK IDLE CHECK PSP LEN+CANARY VALUE+SAVE FLOAT POINT REGISTERS*/
#define TASK_CANARY_VALUE_SAFE_FP     0xCACAFEFD
#endif


typedef enum ca_job_type_t{
    CA_JOB_STD,
    JOB_SAFE,
#if __FPU_PRESENT    
    JOB_FLOAT_POINT,
    CA_JOB_SAFE_FLOAT_POINT,
#endif
}ca_job_type;

/* =============================================================================
 * CCM ALLOCATOR
 * ============================================================================= */


/**
 * @brief Returns available CCM bytes
 */
u32 ca_ccm_get_free(void);

/**
 * @brief Returns used CCM bytes
 */
u32 ca_ccm_get_used(void);


/* =============================================================================
 * ENUMERATED TYPES
 * ============================================================================= */

/**
 * @brief Task priority (7 levels)
 * @details CPU percentage in Priority mode:
 *  - Level0: ~0.78% CPU
 *  - Level1: ~1.57% CPU
 *  - Level2: ~3.15% CPU
 *  - Level3: ~6.28% CPU
 *  - Level4: ~12.56% CPU
 *  - Level5: ~25.12% CPU
 *  - Level6: ~50.24% CPU
 */
#define        CA_TH_LEVEL0  1
#define        CA_TH_LEVEL1  2
#define        CA_TH_LEVEL2  4
#define        CA_TH_LEVEL3  8
#define        CA_TH_LEVEL4  16
#define        CA_TH_LEVEL5  32
#define        CA_TH_LEVEL6  64

/**
 * @brief Task states
 */
#define     CA_ST_TH_UNKNOW             0x00
#define     CA_ST_TH_BLOCKED            0x01
#define     CA_ST_TH_STOP               0x02
#define     CA_ST_TH_SLEEP              0x04
#define     CA_ST_TH_REMOVE             0x08
#define     CA_ST_TH_STACK_OVERFLOW     0x10
#define     CA_ST_TH_DIGEST_ERROR       0x20
#define     CA_ST_TH_RUN                0x40
#define     CA_ST_TH_INIT               0x80
#define     CA_ST_TH_RUNNING            0xC0
#define     CA_ST_TH_SUSPENDED          0x100
#define     CA_ST_TH_SIG_WDT            0x200


#define TASK_NAME_LEN   12
/**
 * @brief Fault types for emergency handler
 */
/**
 * @brief Fault recovery policy
 */
#define     CA_FAULT_ASSERT              0x00  /* Default: remove task */
#define     CA_FAULT_RESTART_THREAD           0x01  /* Restart task with same params */
#define     CA_FAULT_EXEC_EMERGENCY           0x02  /* Execute emergency handler */
#define     CA_FAULT_CHECK                    0x03  /* per check */
/* =============================================================================
 * DATA STRUCTURES
 * ============================================================================= */

/**
 * @brief Task function type
 * @param arg The argument given to ca_os_job()
 * @return Task return value (exit code, kept in the task context)
 */
typedef u32 (*ca_task_fn_t)(void *arg);


/* ---- Stable task handles --------------------------------------------------
 * A thid is a POSITION in the task list and changes whenever a task below it
 * ends. A handle does not: it names the task's context slot plus a generation,
 * and resolves to the CURRENT thid in constant time (one 32-bit load).
 * Use the resolved thid inside the same critical section (or ISR) it was
 * obtained in: outside it, a task removal can move it again. */
typedef u32 ca_task_handle_t;
#define CA_TASK_HANDLE_NONE   0xFFFFFFFFU

typedef void (*ca_emergency_fn_t)(ca_task_handle_t h, u32 emerg_param);
/**
 * @brief Process Control Block (PCB) - Thread Context
 * @details Adapted for Cortex-M4:
 *  - Only 8 registers R4-R11 (saved manually)
 *  - R0-R3, R12, LR, PC, xPSR saved automatically by hardware
 *  - PSP (Process Stack Pointer) for each thread
 */
struct ca_mutex_s;   /* sync.h: priority inheritance links below */

typedef struct __attribute__((packed, aligned(4))) ca_thread_context {
    /* Hardware context (Cortex-M4) */
    u32 digest;             /* 0 0xCACAFEFE*/
    /* Registers saved automatically by hardware (read from stack) */
    u32 r[13];              /**< 4+52 R0-R12 */
    u32 lr;                 /**< 56+4 Link Register */
    u32 pc;                 /**< 60+4 Program Counter */
    u32 xpsr;               /**< 64+4 xPSR */
    u32 psp;                /**< 68+4 Process Stack Pointer */
    /* Task metadata */
    u32 stack_start;        /**< 72+4 Start of stack area */
    u32 stack_end;          /**< 76+4 End of stack area (with guard) */
    /* Scheduling */
    u16 index;              /**< 80+2 Index in taskList array */
    u16 priority;           /**< 82+2 EFFECTIVE priority (the scheduler's weight):
                                 prio_base raised by priority inheritance */
    u16 num_alloc;          /**< 84+2 Number of allocated heap blocks */
    volatile u16 status;    /**< 86+2 Task status (ISR-shared: volatile
                                 prevents compiler reordering/caching) */
    /* Statistics + Notification */
    volatile u32 notify_value; /**< 88+4 Task notification value (ISR-shared) */
    u32 nswitch;            /**< 92+4 Number of context switches */
    u32 running;            /**< 96+4 Total task lifetime */
    u32 sleep;              /**< 100+4 remaining clocks if in sleep */
    void *wait_list;        /**< 104+4 ca_wait_queue_t the task is linked in, NULL if none
                                 (was 'blocked', a refresh counter no one used) */
    u32 result;             /**< 108+4 Task return value */

    /* Debug / Stack monitoring */
    struct ca_thread_context *next_task_wait;  /**< 112+4 the next task in wait state */    
    /* Emergency handler (fault-safe) */
    ca_task_fn_t thread_exec;     /**< 116+4 the thread entry function */
    u32 p1;                 /**< 120+4 entry parameter */
    u32 emerg_param;        /**< 124+4 parameter for emergency handler */
    ca_emergency_fn_t emergency_exec;  /**< 128+4  the function for emergency management*/
    u32 runtime_enter;      /**< 132+4 last CYCCNT task entry */
    u32 runtime_peak_max;   /**< 136+4 maximum duration (WCET) in clock cycles */
    u32 runtime_total;      /**< 140+4 total cumulative runtime in clock cycles */
    u32 runtime_wdt;        /**< 144+4 WDT threshold in clock cycles (0=disabled) */
    u16 n_assert;           /**< 148+2 number of asserts for this thread  */
    u16 n_restart;          /**< 150+2 number of restarts for this thread */
    u16 n_memfault;         /**< 152+2 number of memfaults for this thread */
    u16 prio_base;          /**< 154+2 priority given at creation / set_priority */
    u16 stack_usage_pct;    /**< 156+2 0-1000 (0.0%-100.0%) */
    u16 fault_type;         /**< 158+2 recovery policy: CA_ONLY_ASSERT/RESTART/EMERGENCY */
    
    /* Priority inheritance (sync.c): effective priority = max(prio_base, the
     * waiters of every mutex in pi_held), propagated along pi_wait owners */
    struct ca_mutex_s *pi_held; /**< 160+4 mutexes this task owns (list) */
    struct ca_mutex_s *pi_wait; /**< 164+4 mutex this task is blocked on */

    char name[TASK_NAME_LEN];         /**< 168+12 thread name*/
    /* 180 size END*/
} ca_thread_context_t;

/* MAX_TASK in every objmap.h = TLB size / 180: 4 KB -> 22, 8 KB -> 45,
 * 16 KB -> 91. ca_scheduler_init() checks it against _tlb_area_size. */
_Static_assert(sizeof(ca_thread_context_t) == 180, "ca_thread_context_t size mismatch");



/* In thread.h or sync.h */

/** @brief Sets the next task in the wait queue */
static inline void ca_scheduler_set_next_wait(ca_thread_context_t *task, ca_thread_context_t *next) {
    if (task) {
        task->next_task_wait = next;
    }
}

/** @brief Gets the next task in the wait queue */
static inline ca_thread_context_t* ca_scheduler_get_next_wait(ca_thread_context_t *task) {
    return task ? task->next_task_wait : NULL;
}






/**
 * @brief Dynamic array of pointers to ca_thread_context_t
 */
typedef struct {
    ca_thread_context_t **items;  /**< Pointer buffer */
    u32 size;                 /**< Number of active elements */
    u32 capacity;             /**< Maximum capacity */
    bool good;                /**< Last operation validity flag */
} ca_task_list_t;



/* =============================================================================
 * SCHEDULER (main interface)
 * ============================================================================= */

/* Operations on ANOTHER task named by thid. A thid kept across time can name a
 * different task (swap-with-last on removal): hold a ca_task_handle_t and use
 * the _handle form. Deprecated, and with -Werror a use does not build.
 * Kernel sources that call them with a thid read inside the same critical
 * section define CA_THID_API_INTERNAL before including caos.h. */
#ifdef CA_THID_API_INTERNAL
#define CA_THID_API
#else
#define CA_THID_API __attribute__((deprecated("a thid is a position: hold a ca_task_handle_t and use the _handle form")))
#endif



/**
 * @brief Wakes up a task from BLOCKED (signal)
 * @param thid Thread ID
 */
void ca_scheduler_signal_wake_up(u32 thid);

/**
 * @brief Wakes a waiter taken out of a wait list, BLOCKED or SLEEP alike
 * @param ctx Context dequeued from the wait list (not an index: a position
 *            can name another task by the time it is used)
 * @return false only if the task is ending (REMOVE, overflow, digest, WDT):
 *         it will never run again, and whatever the waker handed it - an
 *         item's wake, a semaphore count, a mutex - must go to the next
 *         waiter. true otherwise, also when it was already runnable (a timed
 *         waiter past its deadline): it will run and see what it was given.
 * @details An untimed waiter is BLOCKED, a timed one SLEEPs until its
 *          deadline; both sit in the same wait list. Waking only BLOCKED
 *          spent the wake on a timed waiter at the head and left the untimed
 *          one behind it asleep with work pending (ex87 'mixed').
 */
bool ca_scheduler_wake_waiter(ca_thread_context_t *ctx);

/**
 * @brief Sets the effective priority keeping the task's CFS virtual time
 * @note Priority inheritance (sync.c). IRQs masked by the caller.
 */
void ca_scheduler_set_eff_priority(ca_thread_context_t *t, u16 prio);

/**
 * @brief Tick hook: pends the PendSV only when the running task's slice is over
 * @note Called by SysTick_Handler. A slice is the run of passes the CFS would
 *       give the same task in a row (scheduler.c, SLICES).
 */
void ca_scheduler_tick(void);

/** @brief ca_scheduler_wake_waiter() on the task a handle names; false if
 *         the handle is stale or the task is ending */
bool ca_scheduler_wake_waiter_handle(ca_task_handle_t h);


/**
 * @brief Changes a task's priority
 * @param thIdx Task index
 * @param newPrio New priority
 * @return true if successful
 * @deprecated use ca_os_set_priority_handle()
 */
CA_THID_API bool ca_os_set_priority(u32 thIdx, u16 newPrio);

/**
 * @brief Gets the current task's context
 * @return Pointer to context or NULL
 */
const ca_thread_context_t* ca_scheduler_get_current_context(void);

/**
 * @brief Get context by index
 * @param idx Task index
 * @return Pointer to context or NULL
 */
const ca_thread_context_t* ca_scheduler_get_context_from_idx(u32 idx);

/**
 * @brief Get current task ID
 * @return Thread ID or 0xFFFFFFFF if no task
 */
u32 ca_scheduler_get_current_task_id(void);

/**
 * @brief Get number of active tasks
 * @return Number of tasks
 */
u32 ca_scheduler_size(void);

/**
 * @brief Verify if a task index is valid
 * @param thid Thread ID
 * @return true if valid
 */
bool ca_scheduler_is_valid_context(u32 thid);


/**
 * @brief Get task name by index
 * @param idx Task index
 * @return Pointer to name (string in static table) or NULL
 */
const char* ca_scheduler_get_task_name(u32 idx);

/**
 * @brief Set emergency handler for a task
 * @param h Task handle
 * @param func Callback function (NULL to disable), called with the same handle
 * @param emerg_param Parameter passed to the emergency function
 * @return true if successful, false if that task has ended
 */
bool ca_task_set_emergency_exec(ca_task_handle_t h, ca_emergency_fn_t func, u32 emerg_param);


/**
 * @brief swap input parameter : in case of emergency function, swaps 
 * the original input parameter with the emergency parameter. A subsequent swap
 * restores the situation to the original condition
 * @param none
 * @return none
 */
void ca_task_swap_param(void);


/* =============================================================================
 * CONTEXT SWITCH (assembly for Cortex-M4)
 * ============================================================================= */



/**
 * @brief PendSV handler for context switch
 * @details This function goes into the vector table as PendSV_Handler
 */
void PendSV_Handler(void);




/**
 * @brief Flag for output selection in DumpTask
 */
#define DUMP_TASK_HDR       0x01  /* Header: name, index, status */
#define DUMP_TASK_ADDR      0x02  /* Addresses: thid, psp, stack range */
#define DUMP_TASK_REG       0x04  /* Registers R0-R12, LR, PC, xPSR */
#define DUMP_TASK_STACK     0x08  /* Stack: usage%, integrity check */
#define DUMP_TASK_STATS     0x10  /* Statistics: time, sleep, nswitch */
#define DUMP_TASK_ALL       0xFF  /* All */

/**
 * @brief Print task information (selectable via mode)
 * @param index Index of the task to print
 * @param mode DUMP_TASK_* flags combined with OR
 * @return true if task found and printed, false otherwise
 */
bool ca_scheduler_dump_task(u16 index,  u16 mode);

/**
 * @brief Print information of all active tasks
 * @param mode DUMP_TASK_* flags combined with OR
 */
void ca_scheduler_dump_all_tasks( u16 mode);

/**
 * @brief Print compact info of all tasks (CPU% and Stack% on one line)
 * @note Format: task 'name' CPU=X.Y% Stack=Z.W%
 * @note -m : memory status per thread =3
 * @note -r : run time status per thread =2
 * @note -f : flag status per thread =1
 * @note no param = 0 = default
 */
typedef enum ca_scheduler_tasks_info_mode_tag{
    CA_TH_INFO_DEFAULT=0,
    CA_TH_INFO_FLAG,
    TH_INFO_RUNTIME,
    CA_TH_INFO_MEM,
    TH_INFO_ASSERT
} ca_scheduler_tasks_info_mode;

void ca_scheduler_tasks_info(ca_scheduler_tasks_info_mode mode);
/** @brief CLI 'sched': CPU split since the previous call - context switch,
 *  tasks, idle, and free time (WFI) against wall time from the tick. */
void ca_scheduler_sched_info(void);

/**
 * @brief fault management based on the type chosen during task creation
 * 
 * @param ctx the context of the thread that caused the fault
 */

void ca_scheduler_manage_fault(void);

/**
 * @brief scheduler init: task list, CCM allocator, idle task
 * @details One policy, CFS-like on the task weights (CA_TH_LEVELn).
 * @return true on success
 */
bool ca_scheduler_init(void);


/**
 * @brief Update stack_usage_pct for all tasks
 */
void ca_scheduler_update_all_stack_usage(void);

/**
 * @brief Verify canary of all tasks and print result
 */
void ca_scheduler_check_all_canaries(void);
/************************************************************************************ */



/**
 * @brief Destroy the scheduler and all tasks
 * @return true if successful
 */
bool ca_os_destroy(void);


/**
 * @brief Reboot the MCU. Does not return.
 *
 * Three routes, tried in order:
 *   1. NVIC_SystemReset() — the Cortex-M core reset, available everywhere;
 *   2. with EN_WDT, a watchdog armed at its shortest timeout and never
 *      reloaded, which resets even if the core request is somehow blocked;
 *   3. as a last resort, a jump through the reset vector at 0x00000000 —
 *      NOTE this restarts the firmware WITHOUT resetting the peripherals,
 *      so it is a weaker guarantee than a real reset.
 *
 * Peripherals keep their state until the reset actually lands, so callers
 * that must leave a pin in a safe state should do so before calling.
 */
void ca_os_reset(void);


/**
 * @brief start the scheduler
 */
void ca_os_run(bool active);

/**
 * @brief return the scheduler status
 */
bool ca_os_running(void);


/**
 * @brief Remove a task
 * @param idx Task index
 * @return true if successful
 * @deprecated use ca_os_remove_job_handle()
 */
CA_THID_API bool ca_os_remove_job(u32 idx);

/**
 * @brief Remove all tasks
 * @return true if successful
 */
bool ca_os_remove_all_jobs(void);


/**
 * @brief Create and add a new task
 * @param name Task name (max 10 char)
 * @param priority Task priority
 * @param func Task function
 * @param arg Argument passed to func
 * @param stack_size Stack size (use enum ca_stack_size_t)
 * @return Handle of the new task, CA_TASK_HANDLE_NONE if error. It is not a
 *         thid: never pass it where a thid is expected
 */
ca_task_handle_t ca_os_job(const char *name, u16 priority,
                       ca_task_fn_t func, void *arg,
                       ca_stack_size_t stack_size, ca_job_type type,u8 fault_type);

/**
 * @brief Adds a task in the SUSPENDED state.
 * @details Same signature as ca_os_job, but the task is not schedulable until
 *          ca_os_resume_handle() is called with the returned handle. Use it when the
 *          caller decides the order: a task created with ca_os_job is eligible
 *          immediately (CA_ST_TH_RUNNING is RUN|INIT, and a new task is INIT),
 *          so a batch of them starts racing before the code that created them
 *          has finished setting up. The task enters the list already
 *          suspended: there is no window in which it can run.
 * @param name Task name
 * @param priority Priority level
 * @param func Task function
 * @param arg Argument passed to func
 * @param stack_size Stack size (use enum ca_stack_size_t)
 * @return Handle to pass to ca_os_resume_handle(), CA_TASK_HANDLE_NONE if error
 */
ca_task_handle_t ca_os_job_suspend(const char *name, u16 priority,
                       ca_task_fn_t func, void *arg,
                       ca_stack_size_t stack_size, ca_job_type type,u8 fault_type);

/**
 * @brief Stack base of a task.
 * @details NOT an identity: when the task ends its PSP area is reused by the
 *          next task created, so the same base can name a newcomer. To keep
 *          hold of a task across removals use the handle itself.
 * @param h Handle of the task
 * @return Stack base address, or 0 if that task has ended
 */
u32 ca_os_stack_from_handle(ca_task_handle_t h);

/* ---- Stable task handles (type above) ------------------------------------ */
/** @brief Handle of a task, CA_TASK_HANDLE_NONE if it is not in the list */
ca_task_handle_t ca_os_task_handle(const ca_thread_context_t *ctx);
/** @brief Handle of the calling task */
ca_task_handle_t ca_os_self_handle(void);
/** @brief Handle of the task at a thid NOW - e.g. a number typed at the CLI,
 *  which is only a position and must be converted at once */
ca_task_handle_t ca_os_handle_from_thid(u32 thid);
/** @brief Current thid of a handle, (u32)-1 if that task has ended */
u32 ca_os_thid_from_handle(ca_task_handle_t h);
/** @brief By-handle forms of ca_os_remove_job/suspend/resume/wake_up/signal/
 *  set_priority: resolve and act atomically. Return false (or do nothing) if
 *  the task has ended. Prefer them to the thid forms whenever the task was
 *  named some time before acting on it. */
bool ca_os_remove_job_handle(ca_task_handle_t h);
bool ca_os_suspend_handle(ca_task_handle_t h);
bool ca_os_resume_handle(ca_task_handle_t h);
void ca_os_wake_up_handle(ca_task_handle_t h);
bool ca_os_signal_handle(ca_task_handle_t h);
bool ca_os_set_priority_handle(ca_task_handle_t h, u16 newPrio);
/** @brief Consistent copy of the task at thid (and its slot, if slot != NULL):
 *  false if there is none, or if it was removed or replaced while being copied.
 *  For readers that print or take their time - never hold a context pointer. */
bool ca_os_task_snapshot(u32 thid, ca_thread_context_t *out, u32 *slot);
/** @brief Context slot of a task: < MAX_TASK and fixed for the task's life */
u32 ca_os_task_slot(const ca_thread_context_t *ctx);
/** @brief Context in a slot, NULL if no task holds it. Stable only while the
 *  owner of the slot is known to be alive (e.g. it owns a heap block) */
ca_thread_context_t *ca_os_context_from_slot(u32 slot);


/**
 * @brief Put the current task to sleep for N milliseconds
 * @param ms Milliseconds
 * @return 0 if successful
 */
u32 ca_os_sleep(u32 ms);

/** @brief Put the current task to sleep for N scheduler ticks */
u32 ca_os_sleep_ticks(u32 ticks);


/**
 * @brief Wake up a task from SLEEP
 * @param thid Thread ID
 * @deprecated use ca_os_wake_up_handle()
 */
CA_THID_API void ca_os_wake_up(u32 thid);

/**
 * @brief Suspend a task indefinitely
 * @param thid Thread ID
 * @return true if successful
 * @note Task goes into CA_ST_TH_SUSPENDED state, requires ca_os_resume to resume
 * @deprecated use ca_os_suspend_handle()
 */
CA_THID_API bool ca_os_suspend(u32 thid);

/**
 * @brief Resume a suspended task
 * @param thid Thread ID
 * @return true if successful
 * @deprecated use ca_os_resume_handle()
 */
CA_THID_API bool ca_os_resume(u32 thid);


/**
 * @brief Wake up task from BLOCKED state
 * @param thid Thread ID
 * @note Alias of ca_scheduler_signal_wake_up
 * @deprecated use ca_os_signal_handle()
 */
CA_THID_API bool ca_os_signal(u32 thid);


/**
 * @brief Put the current task in wait for signal
 * @return 0 if successful
 * @warning Only safe when nothing can signal the task before this call. A
 *          waiter that checks a condition or registers under a lock must use
 *          ca_os_wait_prepare() inside the lock and ca_os_yield() after it:
 *          called after the unlock, this loses a wake that lands in between.
 *          No kernel primitive uses it any more (ex87).
 */
u32 ca_os_wait(void);

/**
 * @brief First half of ca_os_wait(): mark the current task BLOCKED, no yield
 * @return 0 if marked (the caller must then ca_os_yield()), 1 otherwise
 * @details For a waiter that registers in a wait list under a lock and can be
 *          woken from an ISR. With ca_os_wait() called AFTER the unlock, a wake
 *          landing in between finds the task still RUNNING, is ignored
 *          (signal_wake_up acts only on BLOCKED), and the task then blocks
 *          with nobody left to wake it. Calling this INSIDE the critical
 *          section closes that window: a later wake finds it BLOCKED.
 */
u32 ca_os_wait_prepare(void);

/**
 * @brief Timed twin of ca_os_wait_prepare(): mark the current task SLEEP for
 *        @p ticks, no yield
 * @return 0 if marked (the caller must then ca_os_yield()), 1 otherwise
 * @details For a timed waiter in a wait list: called inside the lock, a peer
 *          waking it (ca_scheduler_wake_waiter) finds it already SLEEP; if
 *          nobody does, the scheduler refresh wakes it at the deadline.
 */
u32 ca_os_sleep_prepare(u32 ticks);



/**
 * @brief Put a task in BLOCKED (without trigger switch)
 * @param thid Thread ID
 * @return 0 if successful
 * @deprecated a task blocks itself: ca_os_wait_prepare() + ca_os_yield()
 */
CA_THID_API u32 ca_os_block(u32 thid);


/**
 * @brief Scheduler lock: no context switch until the matching unlock
 * @details Interrupts stay enabled and are served; only the switch away from
 *          the calling task is deferred, and done at the last unlock if one
 *          was asked meanwhile. Nests. The caller must not block (sleep, wait)
 *          while it holds the lock. A task removed on a fault drops it.
 *          Not for ISRs.
 */
void ca_os_sched_lock(void);
void ca_os_sched_unlock(void);

/**
 * @brief Start the context switch (trigger PendSV) equ. YIELD
 * @details Sets the PENDSVSET bit in ICSR to trigger PendSV interrupt
 */
void ca_os_yield(void);

/**
 * @brief return the total time of the thread in use
 * @return runtime total from thread context
 */
u32 ca_scheduler_get_runtime_total(void);

/**
 * @brief set a watchdog per thread
 * @note 32 bit compares with runtime total
 */
void ca_os_thread_set_watchdog(u32 clocks_cycles);
/**
 * @brief resets the total runtime counter
 * @note  if watchdog !=0 and watchdog<runttime_total
 * then thread assert, emergency and/or restart recovery policy 
 * 
 */
void ca_os_thread_reset_watchdog();

#endif /* SCHEDULER_H_ */
