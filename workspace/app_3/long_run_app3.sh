#!/bin/bash
# long_run_app3.sh - soak del router cifrato app_3 su DUE board, per ORE.
#
# Lancia workspace/app_3/test_app3.py con la fase di traffico lunga: UDP 5000
# cifrato A->B e UDP 6000 in chiaro B->A insieme, a blocchi di un'ora; dopo
# ogni blocco controlla le due board (link UP, 0 tag falliti, pool pieno,
# heap integro) e si ferma al primo guasto.
#
# Uso: ./workspace/app_3/long_run_app3.sh ORE [MCU1 MCU2] [--no-flash]
#      ./workspace/app_3/long_run_app3.sh 12                # H563 + F767
#      ./workspace/app_3/long_run_app3.sh 12 H753 H563
#      ./workspace/app_3/long_run_app3.sh 0.25 H563 F767    # 15 min
#
#  ORE        : anche decimali; il traffico dura ORE (le altre fasi ~5 min)
#  MCU1 MCU2  : le due board sul banco (H753 H563 F767), default H563 F767;
#               i ruoli li negoziano le board sul cavo
#  --no-flash : non ricompila e non riflasha (default: build + flash, cosi'
#               l'elf e' sempre quello dei sorgenti attuali)
#
# Banco: le due ETH su un cavo diretto, le due USB utente (NCM) sul PC.
# Log completo in test/hw_test/build/runs/.

set -u
cd "$(dirname "$0")/../.."

usage() { sed -n '2,/^set -u/{/^#/s/^# \{0,1\}//p}' "$0"; exit "${1:-1}"; }

[ $# -ge 1 ] || usage
case "$1" in -h|--help) usage 0 ;; esac

HOURS="$1"; shift
MCUS=()
FLASH=()
for arg in "$@"; do
    case "$arg" in
        --no-flash)          FLASH=(--no-flash) ;;
        H753|H563|F767)      MCUS+=("$arg") ;;
        *) echo "FAIL: argomento sconosciuto '$arg' (MCU: H753 H563 F767)"; exit 1 ;;
    esac
done
[ ${#MCUS[@]} -eq 0 ] && MCUS=(H563 F767)
[ ${#MCUS[@]} -eq 2 ] || { echo "FAIL: servono due MCU, non ${#MCUS[@]}"; exit 1; }
[ "${MCUS[0]}" != "${MCUS[1]}" ] || { echo "FAIL: due MCU diverse"; exit 1; }

[[ "$HOURS" =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo "FAIL: ORE non valide: '$HOURS'"; exit 1; }
SECS=$(awk -v h="$HOURS" 'BEGIN { printf "%d", h * 3600 }')
[ "$SECS" -ge 60 ] || { echo "FAIL: almeno 60 s di traffico"; exit 1; }

mkdir -p test/hw_test/build/runs
LOG="test/hw_test/build/runs/$(date +%Y%m%d-%H%M%S)_app3_${MCUS[0]}_${MCUS[1]}_${HOURS}h.log"

echo "soak app_3 su ${MCUS[0]} + ${MCUS[1]}: traffico $HOURS h ($SECS s, blocchi da 1 h)"
echo "inizio $(date '+%Y-%m-%d %H:%M:%S'), fine prevista $(date -d "+$((SECS + 420)) sec" '+%Y-%m-%d %H:%M')"
echo "log: $LOG"

python3 workspace/app_3/test_app3.py --boards "${MCUS[@]}" --secs "$SECS" "${FLASH[@]}" 2>&1 \
    | tee "$LOG" | grep --line-buffered -E "^\[app3\]|APP3 RESULT|Traceback"
rc=${PIPESTATUS[0]}

echo "fine $(date '+%Y-%m-%d %H:%M:%S'), exit $rc"
exit "$rc"
