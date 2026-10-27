#!/usr/bin/env python3
"""new_app.py - generate the skeleton of a caOS application.

Asks how many tasks you want, their names and levels, and writes
workspace/app_<N>/ca_os_app_<N>.c, ready for ./do -a <N> and ./prog.sh -s -a <N>.

Works in the FREE package (new_app.py at its root) and in the caOS source
tree (tools/new_app.py), where it first creates the application's own
os_config_app_<N>.h with tools/caos_config.py --new (--from SRC to start
from an existing config) and then writes the .c over its skeleton.

  ./new_app.py                                   interactive
  ./new_app.py -t sensor:4 -t logger:2           two tasks, next free app_N
  ./new_app.py -a 3 -t ctrl:5:large:fpu:assert   options of a task, any order

Task spec   name:level[:small|medium|large][:fpu][:assert][:<period ms>]
  name      C identifier, at most 11 characters (TASK_NAME_LEN 12)
  level     0..6 -> CA_TH_LEVEL0..6, a CFS weight (1, 2, 4 .. 64): a higher
            level gets more CPU, it does not starve the lower ones
  stack     STACK_SMALL (default), STACK_MEDIUM, STACK_LARGE
  fpu       the task uses floating point: JOB_FLOAT_POINT instead of CA_JOB_STD
  assert    on a fault the task is removed (default: restarted)
  period    ms between two loop rounds (default 1000)

Python 3.6+, standard library only.
"""
import argparse
import os
import re
import subprocess
import sys


def find_root():
    """The directory holding cpu.cmake: this one (FREE) or its parent (tools/)."""
    d = os.path.dirname(os.path.abspath(__file__))
    for r in (d, os.path.dirname(d)):
        if os.path.isfile(os.path.join(r, "cpu.cmake")):
            return r
    return d


ROOT = find_root()
NAME_MAX = 11                       # TASK_NAME_LEN 12, NUL included
SYSTEM_TASKS = 6                    # idle, stdout, cli, cli_cmd + margin
STACKS = {"small": "STACK_SMALL", "medium": "STACK_MEDIUM", "large": "STACK_LARGE"}
C_WORDS = {"auto", "break", "case", "char", "const", "continue", "default", "do",
           "double", "else", "enum", "extern", "float", "for", "goto", "if", "int",
           "long", "register", "return", "short", "signed", "sizeof", "static",
           "struct", "switch", "typedef", "union", "unsigned", "void", "volatile",
           "while", "main"}


class Task:
    def __init__(self, name, level, stack="small", fpu=False, fault_assert=False,
                 period=1000):
        self.name, self.level, self.stack = name, level, stack
        self.fpu, self.fault_assert, self.period = fpu, fault_assert, period


def check_name(name, seen):
    if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
        return "'%s': letters, digits and '_', not starting with a digit" % name
    if len(name) > NAME_MAX:
        return "'%s': at most %d characters" % (name, NAME_MAX)
    if name in C_WORDS:
        return "'%s' is a C keyword" % name
    if name in seen:
        return "'%s' is already used" % name
    return None


def parse_spec(spec, seen):
    parts = spec.split(":")
    if len(parts) < 2:
        raise ValueError("'%s': expected name:level[:options]" % spec)
    name = parts[0]
    err = check_name(name, seen)
    if err:
        raise ValueError(err)
    if not parts[1].isdigit() or not 0 <= int(parts[1]) <= 6:
        raise ValueError("'%s': level is 0..6" % spec)
    t = Task(name, int(parts[1]))
    for opt in parts[2:]:
        o = opt.lower()
        if o in STACKS:
            t.stack = o
        elif o == "fpu":
            t.fpu = True
        elif o == "assert":
            t.fault_assert = True
        elif o.isdigit() and int(o) > 0:
            t.period = int(o)
        else:
            raise ValueError("'%s': unknown option '%s'" % (spec, opt))
    return t


def ask(prompt, default=None, check=None):
    while True:
        s = input("%s%s: " % (prompt, " [%s]" % default if default is not None else "")).strip()
        if not s and default is not None:
            s = str(default)
        err = check(s) if check else None
        if not err:
            return s
        print("  " + err)


def ask_int(prompt, default, lo, hi):
    def chk(s):
        if not s.isdigit() or not lo <= int(s) <= hi:
            return "a number %d..%d" % (lo, hi)
        return None
    return int(ask(prompt, default, chk))


def ask_yes(prompt, default=False):
    s = ask(prompt + " (y/n)", "y" if default else "n",
            lambda s: None if s.lower() in ("y", "n", "yes", "no") else "y or n")
    return s.lower().startswith("y")


def interactive(max_user):
    print("caOS FREE - new application\n")
    n = ask_int("How many tasks", 1, 1, max_user)
    tasks, seen = [], set()
    for i in range(n):
        print("\nTask %d" % (i + 1))
        name = ask("  name", "task%d" % (i + 1), lambda s: check_name(s, seen))
        seen.add(name)
        t = Task(name, ask_int("  level 0..6 (CPU weight 1..64)", 3, 0, 6))
        t.stack = ask("  stack small/medium/large", "small",
                      lambda s: None if s in STACKS else "small, medium or large")
        t.fpu = ask_yes("  uses floating point", False)
        t.fault_assert = not ask_yes("  restart it on a fault (n = remove it)", True)
        t.period = ask_int("  loop period ms", 1000, 1, 3600000)
        tasks.append(t)
    print()
    cli = ask_yes("CLI on the console", True)
    quiet = not ask_yes("Each task prints a heartbeat line", True)
    return tasks, cli, quiet


def is_dev_tree(root):
    return os.path.isfile(os.path.join(root, "tools", "caos_config.py"))


def cpu_cmake_mcu(root):
    """CURR_MCU of cpu.cmake, None if not readable."""
    try:
        with open(os.path.join(root, "cpu.cmake")) as f:
            return re.search(r'set\(CURR_MCU\s+"([A-Z0-9]+)"', f.read()).group(1)
    except (OSError, AttributeError):
        return None


def board_max_task(root, mcu):
    """MAX_TASK of the MCU, None if not found.
    objmap.h is in <fam>/<mcu>/ (FREE) or hw/<fam>/<mcu>/ (source tree)."""
    if not mcu:
        return None
    try:
        rel = os.path.join(mcu[:2].lower() + "xx", mcu.lower(), "objmap.h")
        for base in (root, os.path.join(root, "hw")):
            p = os.path.join(base, rel)
            if os.path.isfile(p):
                with open(p) as f:
                    v = re.findall(r"#define\s+MAX_TASK\s+(\d+)", f.read())
                return int(v[-1])
    except (OSError, IndexError, ValueError):
        pass
    return None


def task_c(t, quiet):
    body = ["static u32 task_%s(void *arg)" % t.name, "{", "    (void)arg;"]
    if not quiet:
        body += ["    char buf[64];", "    u32 n = 0U;"]
    body += ["    while (1) {", "        /* TODO: the work of %s */" % t.name]
    if not quiet:
        body.append('        ca_printf(STDOUT, buf, sizeof(buf), "%s: %%u\\r\\n", n++);' % t.name)
    body += ["        ca_os_sleep(%uU);" % t.period, "    }",
             "    return 0;                   /* returning ends the task cleanly */", "}"]
    return "\n".join(body)


def build_cmd(app, dev, mcu):
    # bootstrap -m writes cpu.cmake, which ./do and prog.sh -a read: the elf
    # built and the one flashed are for the same board. In the source tree a
    # new target needs the bootstrap anyway.
    return "./bootstrap -m %s && ./do %s %d && ./prog.sh -s -a %d" % (
        mcu or "<MCU>", "-wp" if dev else "-a", app, app)


def generate(app, tasks, cli, quiet, dev):
    if dev:
        cfg = """ * Its configuration is os_config_app_%(n)d.h, next to this file: change it
 * only with tools/caos_config.py (--on/--off/--set ... %(n)d -wp).""" % {"n": app}
    else:
        cfg = """ * The configuration (include/os_config.h) is the one the libraries were
 * built with: it is fixed, changing it does not change the libraries."""
    lines = ["""/*
 * app_%(n)d - caOS application, generated by new_app.py.
 *
 * Every workspace/app_N directory is one firmware: all its .c files, linked
 * against the caOS libraries.
 *     ./bootstrap -m <MCU>   the board, written into cpu.cmake
 *     %(build)-22s ->  elfs/<MCU>/<mcu>_app_%(n)d.elf
 *     %(prog)-22s flash it, for the MCU in cpu.cmake
 *
%(cfg)s
 */
#include "caos.h"
""" % {"n": app, "build": ("./do -wp %d" if dev else "./do -a %d") % app, "prog": "./prog.sh -s -a %d" % app,
       "cfg": cfg}]
    for t in tasks:
        lines.append("/* %s: level %d, %s%s */" % (t.name, t.level, STACKS[t.stack],
                                                  ", floating point" if t.fpu else ""))
        lines.append(task_c(t, quiet) + "\n")
    lines.append("""/* Before the scheduler: drivers and services to set up. */
void ca_os_configure(void)
{
    /* Console output: the stdout task */
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )""")
    if cli:
        lines.append("""    /* CLI on the same console: type 'help' */
    EVAL_CLI(
        ca_cli_init(4096);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
    /* Onboard LEDs: registers the "led" command, so after ca_cli_init() */
    EVAL_LED( ca_led_init(); )""")
    else:
        lines.append("    EVAL_LED( ca_led_init(); )")
    lines.append("""}

/* Still before the scheduler: create the tasks, then start it. It does not
 * return - from here on the tasks run. Keep a handle to address a task
 * later (ca_task_handle_t), never its numeric thid. */
void ca_os_main(void)
{""")
    w = max(len(t.name) for t in tasks)
    for t in tasks:
        lines.append('    ca_os_job(%-*s CA_TH_LEVEL%d, %-*s NULL,\n'
                     '              %s, %s, %s);'
                     % (w + 3, '"%s",' % t.name, t.level, w + 6, "task_%s," % t.name,
                        STACKS[t.stack], "JOB_FLOAT_POINT" if t.fpu else "CA_JOB_STD",
                        "CA_FAULT_ASSERT" if t.fault_assert else "CA_FAULT_RESTART_THREAD"))
    lines.append("    ca_os_run(true);\n}\n")
    return "\n".join(lines)


def next_free_app(ws):
    n = 1
    while os.path.exists(os.path.join(ws, "app_%d" % n)):
        n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 epilog=__doc__.split("\n\n", 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-a", "--app", type=int, help="application number N (default: first free)")
    ap.add_argument("-t", "--task", action="append", default=[], metavar="SPEC",
                    help="a task, name:level[:options]; repeat for more (none: interactive)")
    ap.add_argument("--no-cli", action="store_true", help="no CLI on the console")
    ap.add_argument("--quiet", action="store_true", help="tasks print no heartbeat line")
    ap.add_argument("--force", action="store_true", help="overwrite an existing app_N")
    ap.add_argument("--from", dest="src", metavar="SRC",
                    help="source tree only: config to start from (exNN, app_N, file)")
    ap.add_argument("-m", "--mcu", help="board MCU (F446 ... U575; default: cpu.cmake)")
    ap.add_argument("--root", default=ROOT, help=argparse.SUPPRESS)
    a = ap.parse_args()
    dev = is_dev_tree(a.root)
    if a.src and not dev:
        sys.exit("ERROR: --from is for the source tree: the FREE config is fixed")

    mcu = a.mcu.upper() if a.mcu else cpu_cmake_mcu(a.root)
    max_task = board_max_task(a.root, mcu)
    if a.mcu and max_task is None:
        sys.exit("ERROR: no board %s here" % mcu)
    max_user = max_task - SYSTEM_TASKS if max_task else 32

    if a.task:
        tasks, seen = [], set()
        try:
            for spec in a.task:
                t = parse_spec(spec, seen)
                seen.add(t.name)
                tasks.append(t)
        except ValueError as e:
            sys.exit("ERROR: %s" % e)
        cli, quiet = not a.no_cli, a.quiet
    else:
        try:
            tasks, cli, quiet = interactive(max_user)
        except (EOFError, KeyboardInterrupt):
            sys.exit("\naborted")

    if max_task and len(tasks) > max_user:
        print("WARNING: %d tasks; %s has MAX_TASK %d, and caOS uses some for itself"
              % (len(tasks), mcu, max_task))

    ws = os.path.join(a.root, "workspace")
    app = a.app if a.app else next_free_app(ws)
    if app < 1:
        sys.exit("ERROR: the application number starts at 1")
    d = os.path.join(ws, "app_%d" % app)
    if os.path.exists(d) and not a.force:
        sys.exit("ERROR: %s exists (--force to overwrite)" % os.path.relpath(d, a.root))
    if dev and not os.path.exists(d):
        # the config only through caos_config.py: it also writes a skeleton .c,
        # replaced below
        cmd = [sys.executable, os.path.join(a.root, "tools", "caos_config.py"),
               "--new", str(app), "-wp"] + (["--from", a.src] if a.src else [])
        if subprocess.call(cmd, cwd=a.root) != 0:
            sys.exit("ERROR: caos_config.py --new failed")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "ca_os_app_%d.c" % app)
    with open(path, "w") as f:
        f.write(generate(app, tasks, cli, quiet, dev))

    print("written %s (%d task%s)" % (os.path.relpath(path, a.root), len(tasks),
                                       "" if len(tasks) == 1 else "s"))
    print("  %s" % build_cmd(app, dev, mcu))


if __name__ == "__main__":
    main()
