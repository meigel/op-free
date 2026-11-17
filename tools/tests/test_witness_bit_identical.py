"""The differential gate for the witness engine: the host build and the device build must render the same bits.

Run from the repository root:

    python3 -m unittest discover -s tools/tests -v

`tools/witness/witness_sig.c` compiles the engine in, renders every (mode, depth) deterministically and prints
one FNV-1a digest per case. It is built twice -- with `cc` for the host, and with bfin-elf-gcc for the device,
run in bfin-elf-run -- and the two outputs must match line for line.

Why this engine carries the gate: the golden renders in sim/tests prove what the *host* renders, and the host
is not the device. The kernels are hand-written fixed-point in sdk/include, and three of them already disagree
between the two builds somewhere (see test_opz_cycles). A voice that runs every kernel in a real render path
turns that from a helper-level question into "which mode, which depth".

Skipped when the Blackfin toolchain is not installed (research/toolchain-setup.md).
"""

import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import opz_cycles  # noqa: E402

TOOLCHAIN = os.path.expanduser("~/opt/bfin-elf")
SRC = os.path.join(TOOLS, "witness", "witness_sig.c")
ENGINE = os.path.join(REPO, "engines", "witness", "witness.c")

# The same bare-metal layout the SDK check uses: one flat region, the stack at its top.
LD = """OUTPUT_FORMAT("elf32-bfin")
OUTPUT_ARCH(bfin)
ENTRY(__start)
MEMORY { HOST (rwx) : ORIGIN = 0x80000000, LENGTH = 0x100000 }
SECTIONS
{
  .text   : { *(.text.start) *(.text .text.*) } > HOST
  .rodata : { *(.rodata .rodata.*) } > HOST
  .data   : { *(.data .data.* .sdata .sdata.*) } > HOST
  .bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.* COMMON) } > HOST
  __stack_top = ORIGIN(HOST) + LENGTH(HOST) - 256;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.debug*) }
}
"""


@unittest.skipUnless(opz_cycles.available(TOOLCHAIN), "bfin-elf-gcc / bfin-elf-run not installed in %s"
                     % TOOLCHAIN)
class WitnessHostMatchesDevice(unittest.TestCase):
    def test_every_mode_renders_the_same_bits(self):
        inc = os.path.join(REPO, "sdk", "include")
        with tempfile.TemporaryDirectory() as tmp:
            host = os.path.join(tmp, "wsig")
            subprocess.run(["cc", "-std=c11", "-O2", "-I", inc, SRC, "-o", host], check=True)
            want = subprocess.run([host], capture_output=True, text=True, check=True).stdout

            ld = os.path.join(tmp, "wsig.ld")
            with open(ld, "w") as f:
                f.write(LD)
            elf = os.path.join(tmp, "wsig.elf")
            subprocess.run([opz_cycles.tool(TOOLCHAIN, "gcc")] + opz_cycles.opz_build.CFLAGS
                           + ["-I", inc, os.path.join(TOOLS, "cycles", "crt0.S"), SRC,
                              "-nostdlib", "-nostartfiles", "-T", ld, "-o", elf, "-lgcc"],
                           check=True, capture_output=True)
            got = subprocess.run([opz_cycles.tool(TOOLCHAIN, "run"),
                                  "--memory-region", "0x80000000,0x100000", elf],
                                 capture_output=True, text=True, timeout=300).stdout

        self.assertTrue(want.splitlines() and want.splitlines()[-1].startswith("done"),
                        "host run: %r" % want[-200:])
        host_lines, dev_lines = want.splitlines(), got.splitlines()
        self.assertEqual(len(host_lines), 3 * 4 + 1, "expected 3 modes x 4 depths + done")

        # Report the first differing case by name: the point of the gate is localisation, not a boolean.
        for h, d in zip(host_lines, dev_lines):
            if h != d:
                self.fail("witness renders different bits on host and device:\n  host   %s\n  device %s\n"
                          "A difference here is a kernel whose two builds disagree, not a voice defect."
                          % (h, d))
        self.assertEqual(dev_lines, host_lines)


if __name__ == "__main__":
    unittest.main()
