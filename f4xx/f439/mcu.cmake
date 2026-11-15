# NUCLEO-F439ZI - the options the caOS FREE libraries of this MCU were built with
set(MCU_DEFINES MCU=F4xx MCU_ID=439 STM32F439xx HSE_VALUE=8000000UL)
set(MCU_OPTIONS -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
set(MCU_LINKER_SCRIPT ${BOARD_DIR}/STM32F439ZITX_FLASH.ld)
set(CMSIS_DEVICE f4)
