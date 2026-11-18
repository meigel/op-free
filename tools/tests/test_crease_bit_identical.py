"""The differential gate for the crease engine: the host build and the device build must render the same bits.

    python3 -m unittest discover -s tools/tests -v

`tools/crease/crease_sig.c` compiles the engine in, renders a 5 x 5 grid over (fold, bias) twice — held and
gliding — and prints one FNV-1a digest per case plus `nz` (frames that carried signal) and `step` (largest
sample-to-sample jump, 1/1024 of full scale). It is built twice, with `cc` for the host and with bfin-elf-gcc
for the device, and the two outputs must match line for line.

Why crease needs it as much as witness does: the golden renders in sim/tests prove what the *host* does. The
device is a Blackfin with saturating fixed-point arithmetic, its own rounding, and hand-scheduled assembly loops
in opz_mix.h that the host never executes. Three SDK helpers are known to disagree between the two builds on edge
inputs (test_opz_cycles), so "it compiles and fits" says nothing about whether it computes the same sound.

Three assertions, because equality alone is not evidence:
  * host and device print the same lines;
  * every case carries signal in (almost) every frame — a sine/triangle starts at phase 0, so one zero frame is
    legitimate; 47 of 3072 is the signature of a collapsed amplitude;
  * the gliding case of every point differs from the held one, so a renderer that ignored `pitchSlope` fails.

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
SRC = os.path.join(TOOLS, "crease", "crease_sig.c")

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


def parse(lines):
    """'crease f128 b192 g0 <digest> nz 3071/3072 step 00002' -> {name: (digest, nz, frames, step)}."""
    cases = {}
    for line in lines:
        if not line.startswith("crease "):
            continue
        f = line.split()
        nz, frames = f[6].split("/")
        cases[" ".join(f[0:4])] = (f[4], int(nz), int(frames), int(f[8]))
    return cases


@unittest.skipUnless(opz_cycles.available(TOOLCHAIN), "bfin-elf-gcc / bfin-elf-run not installed in %s"
                     % TOOLCHAIN)
class CreaseHostMatchesDevice(unittest.TestCase):
    def test_every_case_renders_the_same_bits(self):
        inc = os.path.join(REPO, "sdk", "include")
        with tempfile.TemporaryDirectory() as tmp:
            host = os.path.join(tmp, "csig")
            subprocess.run(["cc", "-std=c11", "-O2", "-I", inc, SRC, "-o", host], check=True)
            want = subprocess.run([host], capture_output=True, text=True, check=True).stdout

            ld = os.path.join(tmp, "csig.ld")
            with open(ld, "w") as f:
                f.write(LD)
            elf = os.path.join(tmp, "csig.elf")
            subprocess.run([opz_cycles.tool(TOOLCHAIN, "gcc")] + opz_cycles.opz_build.CFLAGS
                           + ["-I", inc, os.path.join(TOOLS, "cycles", "crt0.S"), SRC,
                              "-nostdlib", "-nostartfiles", "-T", ld, "-o", elf, "-lgcc"],
                           check=True, capture_output=True)
            run = subprocess.run([opz_cycles.tool(TOOLCHAIN, "run"),
                                  "--memory-region", "0x80000000,0x100000", elf],
                                 capture_output=True, text=True, timeout=300)
            got = run.stdout

        # The digests print before bench_exit: matching output alone does not prove the run finished.
        self.assertEqual(run.returncode, 0, "bfin-elf-run exited %d: %r" % (run.returncode, run.stderr[-300:]))
        self.assertTrue(want.splitlines() and want.splitlines()[-1] == "done", "host run: %r" % want[-200:])

        host_lines, dev_lines = want.splitlines(), got.splitlines()
        counts = [l for l in host_lines if l.startswith("cases ")]
        self.assertEqual(len(counts), 1, "the harness must print one 'cases N' line")
        n = int(counts[0].split()[1])
        self.assertEqual(len(host_lines), n + 2, "expected %d cases + 'cases' + 'done'" % n)

        for h, d in zip(host_lines, dev_lines):
            if h != d:
                self.fail("crease renders different bits on host and device:\n  host   %s\n  device %s\n"
                          "A difference here is a kernel whose two builds disagree, not a voice defect."
                          % (h, d))
        self.assertEqual(dev_lines, host_lines)

    def test_every_case_carries_signal(self):
        inc = os.path.join(REPO, "sdk", "include")
        with tempfile.TemporaryDirectory() as tmp:
            host = os.path.join(tmp, "csig")
            subprocess.run(["cc", "-std=c11", "-O2", "-I", inc, SRC, "-o", host], check=True)
            out = subprocess.run([host], capture_output=True, text=True, check=True).stdout
        cases = parse(out.splitlines())
        self.assertGreaterEqual(len(cases), 40, "the harness reported too few cases: %d" % len(cases))
        for name, (digest, nz, frames, step) in cases.items():
            self.assertGreaterEqual(nz, frames - 1,
                                    "%s: only %d of %d frames carried signal (digest %s)"
                                    % (name, nz, frames, digest))
            # The folded layer is dense and the saws are band-limited: 85/1024 is the largest measured edge,
            # a full-scale step would be 1024. Not a discontinuity detector beyond that.
            self.assertLess(step, 400, "%s: a sample-to-sample jump of %d/1024 is a discontinuity" % (name, step))

    def test_the_glide_cases_actually_glide(self):
        inc = os.path.join(REPO, "sdk", "include")
        with tempfile.TemporaryDirectory() as tmp:
            host = os.path.join(tmp, "csig")
            subprocess.run(["cc", "-std=c11", "-O2", "-I", inc, SRC, "-o", host], check=True)
            out = subprocess.run([host], capture_output=True, text=True, check=True).stdout
        cases = parse(out.splitlines())
        checked = 0
        for name in cases:
            head, g = name.rsplit(" ", 1)
            if g != "g0":
                continue
            self.assertNotEqual(cases[name][0], cases[head + " g+"][0],
                                "%s: the gliding case rendered identically to the held note" % name)
            checked += 1
        self.assertGreater(checked, 0, "no glide cases were checked")


if __name__ == "__main__":
    unittest.main()
