# NUCLEO-H563ZI - the options the caOS FREE libraries of this MCU were built with
set(MCU_DEFINES MCU=H5xx MCU_ID=563 STM32H563xx HSE_VALUE=8000000UL)
set(MCU_OPTIONS -mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard)
set(MCU_LINKER_SCRIPT ${BOARD_DIR}/STM32H563ZITX_FLASH.ld)
set(CMSIS_DEVICE h5)
