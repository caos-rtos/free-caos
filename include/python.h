// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : python.h
 * @brief          : MicroPython REPL as a caOS task (EN_PYTHON)
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * The 'py' command starts the master task and returns at once (no cli_cmd
 * slot stays busy). The master takes the console (console.h), allocates its
 * GC heap of UPY_HEAP_SIZE bytes from its own task heap and runs the REPL;
 * Ctrl-D on an empty line or sys.exit() ends it, the console goes back to the
 * CLI. A master that faults is removed by the kernel: heap freed, console
 * reclaimed by the next byte. Port: external/caos_micropython (upy_add.md).
 ******************************************************************************
 */

#ifndef _PYTHON_H_
#define _PYTHON_H_

#include "def_mcu.h"
#include "os_config.h"


#endif /* _PYTHON_H_ */
