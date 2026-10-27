// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : mpu.h
 * @brief          : RTOS Memory protection for STM32F407 (Cortex-M4)
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Memory protection system for the caOS operating system using the MPU
 * (Memory Protection Unit) available on Cortex-M4 microcontrollers.
 * Provides functionality to protect critical memory areas such as task stacks
 * and heap boundaries, using configurable MPU regions.
 *
 * Typical sequence:
 * 1) MPU initialization -> protection region configuration -> monitoring
 ******************************************************************************
 */


#ifndef _MPU_H_
#define _MPU_H_

#include "common.h"


/* Linker symbols - use &symbol to get the address */
extern u32 _tlb_area_base;           
extern u32 _tlb_area_end;           
extern u32 _tlb_area_size;   

extern u32 _psp_stack_top;     
extern u32 _psp_stack_base;     
extern u32 _psp_stack_size;  

extern u32 _msp_stack_base;  
extern u32 _msp_stack_top;   
extern u32 _msp_stack_size;  



#if __MPU_PRESENT



#define PSP_CHECK_TOP   0
/**
 * @brief Initializes MPU for stack canary protection
 */
void ca_mpu_init(void);

/* Define constants for MPU access permissions */
#define MPU_ACCESS_NONE    0  /* No access (invalid region) */
#define MPU_ACCESS_RDONLY  1  /* Read-only access */
#define MPU_ACCESS_RDWR    2  /* Read-write access */

/**
 * @brief Protects a memory region
 * @param index MPU region index (0-7) or 0xFFFF for automatic allocation
 * @param addr Base address of the region to protect
 * @param size Size of the region to protect
 * @param mode Access mode: MPU_ACCESS_NONE (no access),
 *             MPU_ACCESS_RDONLY (read-only), MPU_ACCESS_RDWR (read-write)
 * @return Assigned region index or -1 in case of error
 */
 int ca_mpu_add_region(u32 addr, u32 size, u16 mode);

/**
 * @brief Updates the protection of an existing MPU region
 * @param region_index MPU region index to update
 * @param addr Base address of the region (must match existing configuration)
 * @param size Size of the region (must match existing configuration)
 * @param mode New access mode
 * @return 0 if successful, -1 in case of error
 */
int ca_mpu_update_region(u16 region_index, u32 addr, u32 size, u16 mode);

/**
 * @brief Configures MPU Region 1 to protect current task PSP stack_end canary
 * @param canary_addr Canary base address (must be 32-byte aligned)
 */
void ca_mpu_protect_canary_psp_start(u32 canary_addr);

#if PSP_CHECK_TOP   
/**
 * @brief Configures MPU Region 1 to protect current task PSP stack_start canary
 * @param canary_addr Canary base address (must be 32-byte aligned)
 */
void ca_mpu_protect_canary_psp_end(u32 canary_addr);
#endif

/**
 * @brief Configures MPU Region 0 to protect main task MSP canary
 * @param canary_addr Canary base address (must be 32-byte aligned)
 */
void ca_mpu_protect_canary_msp(u32 canary_addr);


/* NOTE: ca_mpu_protect_heap_boundaries() was removed (2026-08-20). It spent two
 * MPU regions to guard the 64 bytes of the heap START/END sentinels — the least
 * likely corruption, since the allocator can never walk past them (next-fit
 * restarts at START, g_avail_mem rejects oversized requests), while an overrun
 * INSIDE the heap was never covered anyway. Replaced by the software heap
 * canary in memory.c (ca_memory_check_canary(), checked by the idle task),
 * which freed the two regions for the peripherals. */

/**
 * @brief Prints all active MPU regions (base, size, AP, XN)
 */
void ca_mpu_dump_regions(void);

/**
 * @brief Returns the total number of available MPU regions
 * @return Total number of available MPU regions
 */
u32 ca_mpu_get_total_regions(void);

/**
 * @brief Returns the number of MPU regions available to the user
 * @return Number of MPU regions available to the user (those currently free)
 */
u32 ca_mpu_get_available_regions(void);

/**
 * @brief Returns the number of currently occupied MPU regions
 * @return Number of occupied MPU regions (total - available)
 */
u32 ca_mpu_get_busy_regions(void);

/**
 * @brief Index of the first MPU region that is not enabled
 * @return Region index, or ca_mpu_get_total_regions() if none is free
 * @note NOT the same as ca_mpu_get_busy_regions(): the fixed regions are not
 *       contiguous (kernel 0-2, then USB/NET/ETH only if enabled), so the count
 *       of the enabled ones can point at a region already in use.
 */
u32 ca_mpu_first_free_region(void);

/**
 * @brief Checks whether an MPU region is currently armed AND covers [addr, addr+size)
 * @param region MPU region index
 * @param addr Base address of the range to check
 * @param size Size in bytes of the range to check
 * @return true if the region is enabled and overlaps the given range, false otherwise
 * @note Encoding differs per core (ARMv7-M RBAR/RASR size-field vs ARMv8-M
 * RBAR/RLAR base+limit) — implemented per-family in hw/<family>/common/mpu.c.
 */
bool ca_mpu_region_covers(u16 region, u32 addr, u32 size);

/* defined in the linker */




#define STACK_CANARY_SIZE   32  /* MPU minimum region = 32 bytes */
#define STACK_CANARY_VALUE  0xCAFEBABEDEADBEEFULL




/* Kernel-reserved regions. Regions 3 and 4 used to guard the heap sentinels;
 * they were freed on 2026-08-20 (software canary in memory.c) and the
 * peripherals moved down by two, so the dynamic allocator now starts at 6
 * instead of 8. Peripheral indices live in each objmap.h (USB/NET/ETH). */
#define MPU_CANARY_REGION_MSP         0  /* canary MSP */

#define MPU_CANARY_REGION_PSP_START   1  /* stack guard of the running task */
#define MPU_CANARY_REGION_PSP_END     2  /* deep guard (v7) / psp_end canary if PSP_CHECK_TOP */

#define OS_REGIONS 3

/* Region indices the dynamic allocator (ca_mpu_add_region) must never hand out.
 * Their RASR/RLAR enable bit is NOT a reliable "in use" flag: the kernel ones
 * are armed only once the scheduler runs (and reprogrammed at every context
 * switch), the peripheral ones only when their driver configures them. Before
 * that they read as free - a region taken from ca_os_configure() landed on
 * PSP_START and vanished at the first context switch, silently (ex26/46/47 arm
 * the key store exactly there). Peripheral indices are reserved whenever the
 * objmap defines them, whether or not the part ends up arming them. */
static inline bool ca_mpu_region_is_reserved(u32 idx) {
    if (idx < OS_REGIONS) return true;
#ifdef USB_MPU_REGION
    if (idx == USB_MPU_REGION) return true;
#endif
#ifdef NET_MPU_REGION
    if (idx == NET_MPU_REGION) return true;
#endif
#ifdef ETH_MPU_REGION
    if (idx == ETH_MPU_REGION) return true;
#endif
#ifdef RO_MPU_REGION
    if (idx == RO_MPU_REGION) return true;
#endif
    return false;
}


#else
/* No MPU: define no-op constants and macros */
#define MPU_ACCESS_NONE    0
#define MPU_ACCESS_RDONLY  1
#define MPU_ACCESS_RDWR    2

#define ca_mpu_init()
#define ca_mpu_add_region(addr, size, mode)                      0
#define ca_mpu_protect_canary_psp_start(canary_addr)
#define ca_mpu_protect_canary_psp_end(canary_addr)
#define ca_mpu_protect_canary_msp(canary_addr)
#define ca_mpu_dump_regions()
#define ca_mpu_get_total_regions()                               0
#define ca_mpu_get_available_regions()                           0
#define ca_mpu_get_system_regions()                              0
#define ca_mpu_get_busy_regions()                                0
#define ca_mpu_first_free_region()                               0
#define ca_mpu_update_region(region_index, addr, size, mode)     ((void)(mode), 0)
#define ca_mpu_region_covers(region, addr, size)                 false
#endif

#endif //_MPU_H_