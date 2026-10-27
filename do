#!/bin/bash
# caOS FREE - build. No argument: everything. Numbers: those examples. -a N: workspace/app_N.
#   ./do            ./do 1 33            ./do -a 1            ./do 1 -a 1 -a 2
cd "$(dirname "$0")" || exit 1
[ -f build/Makefile ] || ./bootstrap || exit 1
# re-read examples/ and workspace/: an app_N added since the last configure
cmake build > /dev/null || exit 1
MCU=$(grep -oP 'CURR_MCU:STRING=\K\S+' build/CMakeCache.txt | tr 'A-Z' 'a-z')
T=()
while [ $# -gt 0 ]; do
    case "$1" in
        -a) [[ "${2:-}" =~ ^[0-9]+$ ]] || { echo "ERROR: -a takes a number: applications are workspace/app_<N>" >&2; exit 1; }
            T+=("${MCU}_app_$2"); shift ;;
        app_[0-9]*) T+=("${MCU}_$1") ;;
        *)  [[ "$1" =~ ^[0-9]+$ ]] || { echo "ERROR: '$1': use N (example_N) or -a N (workspace/app_N)" >&2; exit 1; }
            T+=("${MCU}_example_$1") ;;
    esac
    shift
done
make -C build -j"$(nproc)" "${T[@]}"
rc=$?
echo "firmware in elfs/$(echo "$MCU" | tr 'a-z' 'A-Z')/"
[ "$rc" -eq 0 ] || echo "ERROR: build failed (make exit $rc)" >&2
exit "$rc"
