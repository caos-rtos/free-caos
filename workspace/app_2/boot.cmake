# This application is the bootloader (workspace/app_2/doc): it is linked inside
# the bootloader area, CA_BOOT_LOADER_LEN of caos/inc/boot_image.h, so a build
# that outgrows it fails at link time instead of overwriting slot A.
set(CAOS_BOOTLOADER ON)
