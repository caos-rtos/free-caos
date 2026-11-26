/*
 * caOS <-> LVGL: what lv_conf.h needs from caOS before lvgl.h is usable.
 * Kept free of caos.h on purpose: it is included by every LVGL source.
 */
#ifndef CAOS_LVGL_PORT_H
#define CAOS_LVGL_PORT_H

#include <stddef.h>

/* LV_MEM_POOL_ALLOC: LVGL's first pool, from the caOS heap of the caller
 * (the GUI task). NULL if the heap cannot give it: lv_init() then fails its
 * malloc assert. */
void *caos_lvgl_pool_alloc(size_t bytes);

#endif /* CAOS_LVGL_PORT_H */
