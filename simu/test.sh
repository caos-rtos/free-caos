#!/bin/bash
# Run the Renode robot suites in simu/tests (default: all of them). Results in simu/out/robot.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$ROOT/simu/out/robot"
# renode-test needs robotframework etc.: venv in simu/.venv
# (see QUICKSTART.md: python3 -m venv simu/.venv, pip install -r <renode>/tests/requirements.txt)
[ -d "$ROOT/simu/.venv" ] && PATH="$ROOT/simu/.venv/bin:$PATH"
"$ROOT/simu/gen_platforms.py" || exit 1   # Renode models patched for caOS -> simu/out/platforms
# boards whose ELFs are not built (elfs/<MCU>/) are skipped by tag
EXCL=()
for b in h753 f767 f439 f446 u575 h563 h773; do
    ls "$ROOT/elfs/${b^^}/${b}_example_"*.elf >/dev/null 2>&1 || EXCL+=(--exclude "$b")
done
cd "$ROOT/simu/out/robot"   # robot writes log/report in cwd
[ $# -gt 0 ] || set -- "$ROOT"/simu/tests/*.robot
exec renode-test "${EXCL[@]}" --results-dir "$ROOT/simu/out/robot" "$@"
