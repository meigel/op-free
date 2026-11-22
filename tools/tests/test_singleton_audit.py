"""The static audit finds every shape of file-scope static, and fails in both directions.

The audit is only worth having if a new static cannot slip through and an old register entry cannot linger. Both
directions are exercised here against a fixture engine, and the shapes that a naive regex gets wrong — comma
declarations, array extents, anonymous structs, attributes, initialisers — are asserted one by one.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "singleton_audit.py")

FIXTURE = textwrap.dedent("""\
    #include <stdint.h>
    #define BLK 64
    #define N 8

    static uint32_t s_comma[N + 1], s_comma2[N + 1];      /* two names, one statement */
    static int16_t s_arr[BLK];                            /* the extent must not become the name */
    static struct { int32_t a, b; uint16_t c; } s_named[2]; /* anonymous struct body */
    static const int32_t k_tab[4] = {1, 2, 3, 4};         /* const: not state */
    static inline int32_t helper(int32_t x) { return x; } /* function: not state */
    static uint32_t s_seeded = 0x1234u;                   /* initialiser */
    static int32_t s_attr[8] __attribute__((aligned(4))); /* attribute between name and ';' */

    void render(void) {
        static int local;                                 /* function-scope static: not ours */
        local++;
    }
    """)

EXPECTED = {"s_comma", "s_comma2", "s_arr", "s_named", "s_seeded", "s_attr"}


class StaticShapes(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.engines = os.path.join(self.dir, "engines")
        os.makedirs(os.path.join(self.engines, "fixture"))
        self.c = os.path.join(self.engines, "fixture", "fixture.c")
        with open(self.c, "w") as f:
            f.write(FIXTURE)
        self.register = os.path.join(self.dir, "register.md")
        self.write_register("")

    def tearDown(self):
        self.tmp.cleanup()

    def write_register(self, rows: str) -> None:
        with open(self.register, "w") as f:
            f.write("# register\n\n| engine | static | class | note |\n|---|---|---|---|\n" + rows)

    def run_tool(self, *extra):
        return subprocess.run([sys.executable, TOOL, "--engines", self.engines, "--register", self.register, *extra],
                              capture_output=True, text=True)

    def test_every_shape_is_found(self):
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import singleton_audit
        self.assertEqual(set(singleton_audit.statics(self.c)), EXPECTED)

    def test_an_unregistered_static_fails(self):
        r = self.run_tool()
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("not classified", r.stdout)
        for name in EXPECTED:
            self.assertIn(name, r.stdout)

    def test_a_register_entry_with_no_static_fails(self):
        rows = "".join("| fixture | %s | scratch | |\n" % n for n in EXPECTED)
        rows += "| fixture | s_long_gone | scratch | |\n"
        self.write_register(rows)
        r = self.run_tool()
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("rotted", r.stdout)
        self.assertIn("s_long_gone", r.stdout)
        self.assertNotIn("not classified", r.stdout)

    def test_a_complete_register_passes(self):
        self.write_register("".join("| fixture | %s | scratch | |\n" % n for n in EXPECTED))
        r = self.run_tool()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("6 classified", r.stdout)


class TheRepository(unittest.TestCase):
    def test_every_engine_static_is_classified(self):
        r = subprocess.run([sys.executable, TOOL], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("none unclassified", r.stdout)


if __name__ == "__main__":
    unittest.main()
