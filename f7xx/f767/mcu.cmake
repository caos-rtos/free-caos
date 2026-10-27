# NUCLEO-F767ZI - the options the caOS FREE libraries of this MCU were built with
set(MCU_DEFINES MCU=F7xx MCU_ID=767 STM32F767xx HSE_VALUE=8000000UL)
set(MCU_OPTIONS -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard)
set(MCU_LINKER_SCRIPT ${BOARD_DIR}/STM32F767ZITX_FLASH.ld)
set(CMSIS_DEVICE f7)
