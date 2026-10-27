# CAOS_STLINK_SN picks the probe when more than one ST-LINK is connected
# (prog.sh passes the one it has just programmed); without it, the first found.
. "$(dirname "$0")/tools/stm32_prg.sh"
PRG_CLI="$(stm32_prg_cli)" || exit 1
"$PRG_CLI" -c port=SWD mode=UR ${CAOS_STLINK_SN:+sn=$CAOS_STLINK_SN} -rst
