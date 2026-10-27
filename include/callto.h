// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : callto.h
 * @brief          : 'callto' console command — DEBUG ONLY
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Calls an arbitrary address from a one-shot task, with an optional u32
 * argument, and reports what it returned.
 *
 * ⚠️ This is an arbitrary-execution primitive on the console. EN_CLI_CALLTO is
 * off by default and must stay off in anything that ships: with keys_store
 * holding keys in RAM, a build that carries this command is a build that hands
 * them out. 'config' reports the flag so a firmware can be asked.
 ******************************************************************************
 */

#ifndef _CALLTO_H_
#define _CALLTO_H_

#include "def_mcu.h"
#include "os_config.h"

#if EN_CLI_CALLTO && ENABLE_CLI

/**
 * @brief Registers the 'callto' node on the CLI chain.
 *
 * Called by ca_cli_init(): the command exists wherever EN_CLI_CALLTO is on,
 * with nothing for the example to remember. A flag that switches a feature on
 * while the node stays unregistered is a defect this project has already paid
 * for once - the CLI answers 'huh!' and the feature looks broken rather than
 * absent.
 */
void ca_cli_callto_init(void);

#endif /* EN_CLI_CALLTO && ENABLE_CLI */

#endif /* _CALLTO_H_ */
