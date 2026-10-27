// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : memory.h
 * @brief          : Memory Allocator - Porting from CAOS
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Dynamic memory allocation system for the caOS operating system.
 * Implements a memory allocator with owner tracking,
 * allowing to identify which task allocated a memory block.
 * Includes functions to allocate, free, and manage heap memory.
 *
 * Typical sequence:
 * 1) Allocator initialization -> memory allocation -> usage -> deallocation
 ******************************************************************************
 */

#ifndef MEMORY_H
#define MEMORY_H

#include "common.h"

/*from linker*/
extern u32 _heap_start;
extern u32 _heap_end;


/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief Initializes the memory allocator
 * @param heap_start Heap start address
 * @param heap_end Heap end address
 */
void ca_memory_init(u32 *heap_start, u32 *heap_end);



/**
 * @brief Allocates memory
 * @param size Requested size in bytes
 * @return Pointer to the allocated memory, NULL if it fails
 */
void *ca_memory_allocate(u32 size);

/**
 * @brief Allocates memory that belongs to no task
 * @details Same as ca_memory_allocate(), but the block is not tied to the
 *          calling task: it survives the task's exit and is released only by
 *          ca_memory_free() (common to both). For objects created by one task
 *          and used by others (queues, buffers handed over).
 * @param size Requested size in bytes
 * @return Pointer to the allocated memory, NULL if it fails
 */
void *ca_memory_allocate_shared(u32 size);

/**
 * @brief Makes the calling task the owner of a block
 * @details For a block handed over between tasks: the producer allocates it
 *          shared (it survives the producer), the consumer adopts it on
 *          receipt, so it is freed if the consumer dies before releasing it.
 *          Called outside a task the block becomes shared (no owner).
 * @param p Pointer returned by an allocation
 * @return true if p is a valid allocated block
 */
bool ca_memory_adopt(void *p);

/**
 * @brief Frees allocated memory
 * @param p Pointer to the memory to be freed
 * @param freed_size If not NULL, returns the freed size
 * @return true if successfully freed
 */
bool ca_memory_free(void *p, u32 *freed_size, bool wiped);

/**
 * @brief Frees all blocks owned by a task
 * @param task_slot Owner task SLOT (ca_os_task_slot), not its thid: a thid
 *        moves when another task ends, the slot is fixed for the task's life
 * @return Number of freed blocks
 */
u32 ca_memory_free_all_owned_by(u16 task_slot);


/**
 * @brief Gets information about a memory block
 * @param p Pointer to the allocated memory
 */
void ca_memory_info(void *p);

/**
 * @brief Prints block list on ITM
 */
void ca_memory_list(void);

/**
 * @brief Prints memory in hexdump -C format (HEX + ASCII)
 * @param p Start address
 * @param lines Number of 16-byte lines
 */
void ca_memory_dump(void *p, u32 lines);



/**
 * @brief Prints free memory and the result of ca_memory_check() (CLI 'free')
 */
void ca_memory_dump_avail(void);

/* ============================================================================
 * INLINE GETTERS
 * ========================================================================== */

/**
 * @brief Returns heap start address
 */
u32 *ca_memory_get_start_address(void);

/**
 * @brief Returns heap end address
 */
u32 *ca_memory_get_end_address(void);

/**
 * @brief Returns total heap size
 */
u32 ca_memory_get_total_size(void);

/**
 * @brief Returns available memory: the sum of the FREE payloads
 * @note Fragmented: the largest single allocation that can succeed is
 *       ca_memory_check_t.largest_free, not this
 */
u32 ca_memory_get_avail_memory(void);

/**
 * @brief Returns total memory
 */
u32 ca_memory_get_total_memory(void);



/**
 * @brief Returns total memory allocated by a task
 * @param task_slot Task SLOT (ca_os_task_slot), not its thid
 * @return Bytes allocated by the task (payload only, without header)
 */
u32 ca_memory_get_task_usage(u16 task_slot);

/**
 * @brief Verifies the heap canaries and the two sentinel blocks
 * @return true if intact, false if the heap was corrupted from the outside
 * @note Cheap (a few word compares): meant to be called periodically from the
 *       idle task. Detects corruption AFTER the fact — it does not prevent the
 *       write, unlike the MPU regions this replaces.
 */
bool ca_memory_check_canary(void);

/** Result of ca_memory_check(): one walk of the whole block chain */
typedef struct {
    u32  blocks;        /**< blocks walked, START and END included */
    u32  busy;          /**< BUSY blocks */
    u32  busy_bytes;    /**< payload of the BUSY blocks */
    u32  shared;        /**< BUSY blocks with no owner (shared or pre-scheduler) */
    u32  shared_bytes;  /**< their payload: a steady growth is a leak */
    u32  free;          /**< FREE blocks */
    u32  free_bytes;    /**< payload of the FREE blocks */
    u32  largest_free;  /**< largest FREE payload: the biggest allocation that can succeed */
    u32  unmerged;      /**< FREE blocks followed by another FREE one (missed merge) */
    u32  avail;         /**< g_avail_mem as the allocator holds it */
    u32  avail_expect;  /**< what the chain says it should be */
    u32  bad_addr;      /**< first block that failed a check, 0 if none */
    cstr bad_what;      /**< which check it failed, NULL if none */
    bool chain_ok;      /**< every header valid, linked both ways, contiguous */
    bool canary_ok;     /**< ca_memory_check_canary() */
} ca_memory_check_t;

/**
 * @brief Walks the whole heap chain and validates it, without changing it
 * @param r [out] counts and the first defect found (may be NULL)
 * @return true if chain, accounting and canaries are all intact
 * @note Under the allocator lock (IRQs off) for the whole walk: a diagnostic,
 *       not something to call from a hot path.
 */
bool ca_memory_check(ca_memory_check_t *r);

/**
 * @brief Rewrites both heap canaries with the expected pattern
 * @note Repairs the MARKER, not the heap: whatever overwrote it may well have
 *       damaged real data too. Called right after reporting corruption, so the
 *       trap is armed again for the next occurrence.
 */
void ca_memory_restore_canary(void);

/**
 * @brief Address of the low heap canary (below the START block)
 * @return Pointer to HEAP_CANARY_SIZE bytes, or NULL before ca_memory_init()
 * @note For dumps/diagnostics: the pattern is deliberately distinct from the
 *       stack canaries so a hexdump tells them apart.
 */
const u32 *ca_memory_canary_low(void);

/**
 * @brief Address of the high heap canary (above the END block)
 * @return Pointer to HEAP_CANARY_SIZE bytes, or NULL before ca_memory_init()
 */
const u32 *ca_memory_canary_high(void);

/* ============================================================================
 * MEMORY MAP
 * ========================================================================== */

/** Max entries a caller must provide to ca_memmap_get(). The widest part
 *  today (H753) declares 5 areas; 16 leaves room without forcing the caller
 *  to size the array per MCU. */
#define CA_MEMMAP_MAX   16U

/** One physical memory area, as the part declares it. */
typedef struct {
    u32  addr;              /**< base address */
    u32  size;              /**< size in BYTES */
    char name[12];          /**< "RAM1".."RAMn" by ascending address, "FLASH" */
} ca_mem_range;

/**
 * @brief Fills in the physical memory areas of this MCU
 * @param[out] out Array of at least CA_MEMMAP_MAX entries
 * @return Number of valid entries written, 0 if out is NULL
 * @note Physical areas only, with no regard for what they are used for: heap,
 *       MSP, PSP and the driver pools all live inside these and are not told
 *       apart. A part with a single RAM block that does everything reports one
 *       area. Naming is positional - RAM areas are numbered by ascending
 *       address, so the same index can mean a different memory on another
 *       part; knowing what each area is, is up to the caller.
 */
u32 ca_memmap_get(ca_mem_range *out);

#endif /* MEMORY_H */
