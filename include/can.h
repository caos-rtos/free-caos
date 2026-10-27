// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    can.h
 * @brief   CAN driver — bxCAN (F1/F4/F7) and FDCAN (H7/H5)
 *
 * @note    num=1 → CAN1/FDCAN1, num=2 → CAN2/FDCAN2
 *          Only CAN 2.0 classic (8 byte payload), polling.
 *          Accepts all messages by default (no active filters).
 *          GPIO/AF: configure separately with ca_gpio_configure().
 ******************************************************************************
 */

#ifndef CAN_H_
#define CAN_H_

#include "common.h"
#include "os_config.h"


#endif /* CAN_H_ */
