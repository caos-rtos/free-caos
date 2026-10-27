# NUCLEO-H753ZI - the options the caOS FREE libraries of this MCU were built with
set(MCU_DEFINES MCU=H7xx MCU_ID=753 STM32H753xx USE_PWR_LDO_SUPPLY HSE_VALUE=8000000)
set(MCU_OPTIONS -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard)
set(MCU_LINKER_SCRIPT ${BOARD_DIR}/STM32H753ZITX_FLASH.ld)
set(CMSIS_DEVICE h7)
