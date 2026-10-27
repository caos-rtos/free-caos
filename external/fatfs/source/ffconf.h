/*
 * Redirect: la configurazione FatFS di caOS vive fuori dal submodule fatfs.
 * Chiunque includa ff.h (libfatfs.a E gli esempi/consumer) vede la STESSA
 * configurazione -> stesso sizeof(FATFS)/FIL. Evita ODR mismatch che causa
 * overflow di s_fs.win[] sulla memoria adiacente.
 */
#include "../../caos_fatfs/caos_ffconf.h"
