#!/usr/bin/env python3
"""
lookup_sym.py - look up a caOS symbol in the API reference of this package.

    tools/lookup_sym.py ca_uart_put          # function: signature, header, doc
    tools/lookup_sym.py CA_WAIT_FOREVER      # define: value(s) and doc
    tools/lookup_sym.py ca_task_handle_t     # type: definition and members
    tools/lookup_sym.py CA_TICK_SPEED_500US  # enum value: the whole enum
    tools/lookup_sym.py --list ca_uart       # every symbol with that prefix
    tools/lookup_sym.py --find uart dma      # symbols whose name or brief has ALL the words

The data is tools/db/reference.json: the same documentation as the reference
chapter of the manual (description and signature, not the source). Part of it
was written with the help of an AI from the code: when it matters, check the
declaration in include/ - the header is the authority.

A symbol that is not found does not exist in caOS: do not guess a name by
convention (ca_task_delay -> the real one is ca_os_sleep). Some functions are
documented for the full edition only (CAN, USB HS, TAMP, FMAC): if a name is
not declared in include/, this FREE edition does not have it.

Exit status: 0 found, 1 not found.
"""
import difflib
import json
import re
import sys
from pathlib import Path

DB = Path(__file__).resolve().parent / "db" / "reference.json"
KINDS = ("functions", "defines", "types", "enums")


def load():
    try:
        return json.loads(DB.read_text())
    except FileNotFoundError:
        sys.exit(f"lookup_sym: {DB} not found")


def in_include(sym: str) -> bool:
    inc = DB.parent.parent.parent / "include"
    if not inc.is_dir():
        return True
    word = re.compile(r"\b" + re.escape(sym) + r"\b")
    return any(word.search(h.read_text(errors="replace")) for h in inc.glob("*.h"))


def block(title: str, text) -> None:
    if text in (None, "", [], {}):
        return
    print(f"{title}:")
    for line in str(text).strip().splitlines():
        print(f"  {line}")


def show_function(sym, r):
    print(f"=== FUNCTION {sym} ===")
    print(f"Signature : {r.get('signature', '')}")
    print(f"Header    : include/{r.get('header', '')}")
    block("Brief", r.get("brief"))
    params = r.get("params") or []
    if params:
        print("Parameters:")
        for p in params:
            if isinstance(p, dict):
                print(f"  {p.get('name', '?')} ({p.get('type', '')}): {p.get('desc', '')}")
            else:
                print(f"  {p}")
    block("Returns", r.get("returns"))
    block("Notes", r.get("notes"))
    block("Usage", r.get("usage"))
    block("Example", r.get("example"))


def show_define(sym, r):
    print(f"=== DEFINE {sym} ===")
    for v in r.get("values", []):
        mcu = f"  [{v['mcu']}]" if v.get("mcu") else ""
        print(f"Value     : {v.get('value', '')}   (include/{v.get('header', '')}){mcu}")
    block("Brief", r.get("brief"))
    block("Notes", r.get("notes"))
    block("Usage", r.get("usage"))


def show_type(sym, r):
    print(f"=== TYPE {sym} ({r.get('kind', '')}) ===")
    print(f"Header    : include/{r.get('header', '')}")
    block("Definition", r.get("body"))
    block("Brief", r.get("brief"))
    members = r.get("members") or []
    if members:
        print("Members:")
        for m in members:
            if isinstance(m, dict):
                print(f"  {m.get('name', '?')} ({m.get('type', '')}): {m.get('desc', '')}")
    block("Notes", r.get("notes"))
    block("Usage", r.get("usage"))
    block("Example", r.get("example"))


def show_enum(sym, r, mark=None):
    print(f"=== ENUM {sym} ===")
    print(f"Header    : include/{r.get('header', '')}")
    block("Brief", r.get("brief"))
    print("Values:")
    for v in r.get("values", []):
        m = ">" if v.get("name") == mark else " "
        desc = f"  {v['desc']}" if v.get("desc") else ""
        print(f" {m} {v.get('name')} = {v.get('value', '')}{desc}")
    block("Notes", r.get("notes"))
    block("Usage", r.get("usage"))


def lookup(db, sym) -> bool:
    found = False
    show = {"functions": show_function, "defines": show_define,
            "types": show_type, "enums": show_enum}
    for k in KINDS:
        if sym in db[k]:
            if found:
                print()
            show[k](sym, db[k][sym])
            found = True
    if not found:                               # an enumerator: show its enum
        for name, r in db["enums"].items():
            if any(v.get("name") == sym for v in r.get("values", [])):
                show_enum(name, r, mark=sym)
                found = True
                break
    if found and not in_include(sym):
        print("\nNOTE: not declared in include/ - not available in this FREE edition.")
    return found


def all_names(db):
    names = set()
    for k in KINDS:
        names.update(db[k])
    for r in db["enums"].values():
        names.update(v.get("name") for v in r.get("values", []) if v.get("name"))
    return names


def kind_of(db, name):
    for k in KINDS:
        if name in db[k]:
            return k[:-1]
    return "enum value"


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    db = load()
    if args[0] == "--list" and len(args) == 2:
        hits = sorted(n for n in all_names(db) if n.startswith(args[1]))
        for n in hits:
            print(f"  {kind_of(db, n):10s}  {n}")
        print(f"\n{len(hits)} symbols")
        return 0 if hits else 1
    if args[0] == "--find" and len(args) >= 2:
        words = [w.lower() for w in args[1:]]
        hits = []
        for k in KINDS:
            for n, r in db[k].items():
                text = (n + " " + str(r.get("brief", ""))).lower()
                if all(w in text for w in words):
                    hits.append((k[:-1], n, str(r.get("brief", ""))[:90]))
        for k, n, b in sorted(hits, key=lambda x: x[1]):
            print(f"  {k:9s} {n:40s} {b}")
        print(f"\n{len(hits)} symbols")
        return 0 if hits else 1
    if len(args) != 1:
        print(__doc__)
        return 1
    sym = args[0]
    if lookup(db, sym):
        return 0
    near = difflib.get_close_matches(sym, all_names(db), n=6, cutoff=0.6)
    print(f"'{sym}' is not a caOS symbol. Do not guess a name by convention.")
    if near:
        print("Closest names: " + ", ".join(near))
    print("Search by meaning: tools/lookup_sym.py --find <words>")
    return 1


if __name__ == "__main__":
    sys.exit(main())
