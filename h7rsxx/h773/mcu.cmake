# NUCLEO-H7S3L8 - the options the caOS FREE libraries of this MCU were built with.
# The firmware runs from the Octo-SPI flash on XSPI2 (0x70000000, XIP): the
# internal flash holds only the trampoline, workspace/app_5.
set(MCU_DEFINES MCU=H7RSxx MCU_ID=773 STM32H7S3xx HSE_VALUE=24000000)
set(MCU_OPTIONS -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard)
set(MCU_LINKER_SCRIPT ${BOARD_DIR}/STM32H7S3L8HX_XSPI2.ld)
set(CMSIS_DEVICE h7rs)
