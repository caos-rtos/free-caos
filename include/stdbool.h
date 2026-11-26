// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/* caOS stdbool.h stub.
 * bool is already defined as u16 in def_mcu.h.
 * Do not redefine with _Bool to avoid type conflicts. */

#ifndef _STDBOOL_H
#define _STDBOOL_H

/* Prevents any external stdbool.h from redefining bool */
#define __bool_true_false_are_defined 1

#ifndef true
#define true  1
#endif
#ifndef false
#define false 0
#endif

#endif /* _STDBOOL_H */
