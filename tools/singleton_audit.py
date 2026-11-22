#!/usr/bin/env python3
"""Fail when an engine grows a file-scope mutable static that nobody classified.

Every engine is loaded into one address space and renders one voice at a time, so a `static` scratch buffer is a cheap
way to get per-block space — and an unstated assumption that there is exactly one instance, that voices render one at
a time, and that notes arrive in a fixed order. `sdk/docs/statics.md` is the register of those assumptions; this tool
makes it binding: a static that is not in the register fails the check, and a register entry whose static is gone also
fails (so the register cannot rot in the other direction).

The register is parsed from the markdown, so there is one source of truth — classify the static in the doc, in the
same change that adds it.

    python3 tools/singleton_audit.py            report what is registered and what is not, exit 1 on any mismatch
    python3 tools/singleton_audit.py --check     same, quieter (what CI and the unit test run)
    python3 tools/singleton_audit.py --report    print the register, no comparison
"""

from __future__ import annotations

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINES = os.path.join(ROOT, "engines")
REGISTER = os.path.join(ROOT, "sdk", "docs", "statics.md")

CLASSES = {"scratch", "seed", "shared-track", "cross-note", "cache", "built-once", "singleton-bank"}

# words that are never a declarator name
TYPES = {
    "static", "const", "volatile", "unsigned", "signed", "char", "short", "int", "long", "float", "double",
    "void", "struct", "union", "enum", "inline", "register", "extern",
}
TYPE_RE = re.compile(r"\b(?:u?int(?:8|16|32|64)_t|fract32|Line|OpzSvfCoef|State|bool|size_t)\b")

STMT = re.compile(r"^static\s([^;]*);", re.M | re.S)
ATTR = re.compile(r"__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)", re.S)
ROW = re.compile(r"^\|\s*([A-Za-z0-9_]+)\s*\|\s*([A-Za-z_]\w*)\s*\|\s*([a-z-]+)\s*\|", re.M)


def _split_declarators(body: str) -> list[str]:
    """Split `uint32_t a[4], b[8]` into the declarator parts, ignoring commas inside [] {} and ()."""
    depth, part, parts = 0, [], []
    for ch in body:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(part))
            part = []
        else:
            part.append(ch)
    parts.append("".join(part))
    return parts


def _statement(src: str, i: int) -> tuple[str, int]:
    """From just after `static`, return the statement text and the index after its terminator.

    Depth-aware, so the semicolons inside a function body, an anonymous struct or an initialiser do not end the
    statement early. A file-scope static ends at the first depth-0 `;`.
    """
    depth, j = 0, i
    while j < len(src):
        ch = src[j]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == ";" and depth == 0:
            return src[i:j], j + 1
        j += 1
    return src[i:], len(src)


def statics(path: str) -> dict[str, str]:
    """engine -> {static name: declaration}, for file-scope mutable statics only."""
    with open(path, encoding="utf-8", errors="ignore") as f:
        src = f.read()
    src = ATTR.sub("", src)                       # __attribute__((aligned(4))) must not confuse the scan
    found: dict[str, str] = {}
    for m in re.finditer(r"^static\s+(?!const\b)(?!inline\b)", src, re.M):
        body, _ = _statement(src, m.end())
        body = " ".join(body.split())
        if not body:
            continue
        # a function definition has its '(' before any '{'; an anonymous struct has the '{' first
        first_paren, first_brace = body.find("("), body.find("{")
        if first_paren != -1 and (first_brace == -1 or first_paren < first_brace):
            continue
        for part in _split_declarators(body):
            # drop the array extents first, or `s_tone[OPZ_BLOCK_FRAMES]` reads as a variable named OPZ_BLOCK_FRAMES
            head = re.sub(r"\[[^\]]*\]", "", part).split("=")[0].strip()
            if not head or "(" in head:
                continue                               # function pointers, prototypes
            if "}" in head:                            # anonymous struct/union: drop the body, keep the name
                head = re.sub(r"\{[^{}]*\}", "", head)
            words = [w for w in re.findall(r"[A-Za-z_]\w*", head) if w not in TYPES and not TYPE_RE.fullmatch(w)]
            name = words[-1] if words else ""
            if name and name not in TYPES:
                found[name] = head.strip()
    return found


def register(path: str | None = None) -> dict[str, dict[str, str]]:
    """engine -> {static: class}, from the markdown table (rows whose third cell is a known class)."""
    with open(path or REGISTER, encoding="utf-8") as f:
        text = f.read()
    out: dict[str, dict[str, str]] = {}
    for engine, name, cls in ROW.findall(text):
        if cls in CLASSES:
            out.setdefault(engine, {})[name] = cls
    return out


def scan(engines: str | None = None) -> tuple[dict[str, dict[str, str]], dict[str, dict[str, str]]]:
    engines = engines or ENGINES
    actual: dict[str, dict[str, str]] = {}
    if not os.path.isdir(engines):
        return register(), actual
    for entry in sorted(os.listdir(engines)):
        c = os.path.join(engines, entry, entry + ".c")
        if os.path.exists(c):
            actual[entry] = statics(c)
    return register(), actual


def compare(engines: str | None = None) -> tuple[list, list, int]:
    reg, actual = scan(engines)
    unclassified: list[tuple[str, str, str]] = []
    stale: list[tuple[str, str, str]] = []
    ok = 0
    for engine, found in actual.items():
        for name, decl in found.items():
            if name in reg.get(engine, {}):
                ok += 1
            else:
                unclassified.append((engine, name, decl))
    for engine, found in reg.items():
        for name, cls in found.items():
            if name not in actual.get(engine, {}):
                stale.append((engine, name, cls))
    return unclassified, stale, ok


def main() -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    ap.add_argument("--check", action="store_true", help="only the verdict (what CI runs)")
    ap.add_argument("--report", action="store_true", help="print the register")
    ap.add_argument("--engines", help="scan another engines directory (tests point this at a fixture)")
    ap.add_argument("--register", help="read another register file")
    a = ap.parse_args()

    global REGISTER
    if a.register:
        REGISTER = a.register

    if a.report:
        for engine, found in sorted(register(REGISTER).items()):
            for name, cls in sorted(found.items()):
                print("%-10s %-14s %s" % (engine, name, cls))
        return 0

    unclassified, stale, ok = compare(a.engines)
    reg, actual = scan(a.engines)
    mutable = len([e for e in actual if actual[e]])
    if unclassified:
        print("error    %d file-scope static(s) not classified in sdk/docs/statics.md:" % len(unclassified))
        for engine, name, decl in unclassified:
            print("           %s: %s (%s)" % (engine, name, decl))
        print("         classify it there — scratch, seed, shared-track, cross-note, cache, built-once,")
        print("         singleton-bank — or move it into the voice record.")
    if stale:
        print("error    %d register entr(y/ies) with no matching static (the register has rotted):" % len(stale))
        for engine, name, cls in stale:
            print("           %s: %s (%s)" % (engine, name, cls))
    if unclassified or stale:
        return 1
    print("ok       statics   %d classified across %d engine(s), %d with file-scope state, none unclassified"
          % (ok, len(actual), mutable))
    return 0


if __name__ == "__main__":
    sys.exit(main())
