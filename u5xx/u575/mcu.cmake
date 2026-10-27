# NUCLEO-U575ZI-Q - the options the caOS FREE libraries of this MCU were built with
set(MCU_DEFINES MCU=U5xx MCU_ID=575 STM32U575xx HSE_VALUE=8000000UL)
set(MCU_OPTIONS -mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard)
set(MCU_LINKER_SCRIPT ${BOARD_DIR}/STM32U575XX_FLASH.ld)
set(CMSIS_DEVICE u5)
