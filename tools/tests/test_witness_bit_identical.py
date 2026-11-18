"""The differential gate for the witness engine: the host build and the device build must render the same bits.

Run from the repository root:

    python3 -m unittest discover -s tools/tests -v

`tools/witness/witness_sig.c` compiles the engine in, renders every (mode, depth, glide) deterministically and
prints one FNV-1a digest per case, plus two observability numbers: `nz`, the number of frames that carried any
signal at all, and `step`, the largest sample-to-sample jump. It is built twice -- with `cc` for the host, and
with bfin-elf-gcc for the device, run in bfin-elf-run -- and the two outputs must match line for line.

Three assertions, because equality alone is not evidence:

  * host and device print the same lines (the gate itself);
  * every case carries signal in (almost) every frame. An earlier version of this harness held the envelope
    with step = 0, so amplitude collapsed after the first frame of each block and both builds agreed on a
    second of silence -- a comparison that could never fail. `nz` is what would have caught it.
  * the glide cases actually glide: for the sine and saw modes the held, rising and falling variants must have
    different digests, so a renderer that ignored pitchSlope could not pass. Pure noise at full depth is the
    one legitimate exception -- its pitch does not reach the output.

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


def parse(lines):
    """'mode 1 d085 g+ <digest> nz 3072/3072 step 00103' -> {"mode 1 d085 g+": (digest, nz, frames, step)}."""
    cases = {}
    for line in lines:
        if not line.startswith("mode "):
            continue
        f = line.split()
        nz, frames = f[6].split("/")
        cases[" ".join(f[0:4])] = (f[4], int(nz), int(frames), int(f[8]))
    return cases


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
            run = subprocess.run([opz_cycles.tool(TOOLCHAIN, "run"),
                                  "--memory-region", "0x80000000,0x100000", elf],
                                 capture_output=True, text=True, timeout=300)
            got = run.stdout
        # The digest lines are printed before bench_exit, so matching output alone does not prove the run
        # finished: a crash during shutdown would still leave it. The exit status is part of the evidence.
        self.assertEqual(run.returncode, 0, "bfin-elf-run exited %d: %r" % (run.returncode, run.stderr[-300:]))

        self.assertTrue(want.splitlines() and want.splitlines()[-1] == "done",
                        "host run: %r" % want[-200:])
        host_lines, dev_lines = want.splitlines(), got.splitlines()

        # The harness prints its own case count so this test never encodes "3 modes x 4 depths x 3 glides":
        # adding a mode is an edit to witness_sig.c alone, and a stale expectation here cannot silently
        # shrink the coverage.
        counts = [l for l in host_lines if l.startswith("cases ")]
        self.assertEqual(len(counts), 1, "the harness must print one 'cases N' line")
        n = int(counts[0].split()[1])
        self.assertEqual(len(host_lines), n + 2, "expected %d cases + 'cases' + 'done'" % n)

        # Report the first differing case by name: the point of the gate is localisation, not a boolean.
        for h, d in zip(host_lines, dev_lines):
            if h != d:
                self.fail("witness renders different bits on host and device:\n  host   %s\n  device %s\n"
                          "A difference here is a kernel whose two builds disagree, not a voice defect."
                          % (h, d))
        self.assertEqual(dev_lines, host_lines)

    def test_every_case_carries_signal(self):
        """A gate that hashes silence passes trivially; this is the assertion that the signal is there.

        `step = 0` in the harness's envelope callback zeroed the amplitude after the first frame of each block,
        which left 63 of every 64 samples silent and both builds agreeing about it. One genuinely zero frame is
        allowed, because a sine starts at phase 0.
        """
        inc = os.path.join(REPO, "sdk", "include")
        with tempfile.TemporaryDirectory() as tmp:
            host = os.path.join(tmp, "wsig")
            subprocess.run(["cc", "-std=c11", "-O2", "-I", inc, SRC, "-o", host], check=True)
            out = subprocess.run([host], capture_output=True, text=True, check=True).stdout
        cases = parse(out.splitlines())
        self.assertGreaterEqual(len(cases), 12, "the harness reported too few cases: %d" % len(cases))
        for name, (digest, nz, frames, step) in cases.items():
            self.assertGreaterEqual(nz, frames - 1,
                                    "%s: only %d of %d frames carried signal -- the envelope is not holding %s"
                                    % (name, nz, frames, digest))
            # Not a stale-reciprocal detector (that defect is ~1e-3 of full scale and this metric cannot see
            # it); a gross-discontinuity limit. The band-limited saw's edge measures 101-103 at C3, and 206 in
            # the over-ceiling case, where the increment is 2^30 and the waveform genuinely moves that fast. A
            # full-scale jump is 1024.
            self.assertLess(step, 400, "%s: a sample-to-sample jump of %d/1024 is a discontinuity, not a "
                                       "band-limited waveform" % (name, step))

    def test_the_glide_cases_actually_glide(self):
        """`render` rebuilds the BLEP reciprocal every sample while pitchSlope != 0; a gate that only ever
        passes 0 never runs that code. Distinct digests per glide direction are what proves it runs."""
        inc = os.path.join(REPO, "sdk", "include")
        with tempfile.TemporaryDirectory() as tmp:
            host = os.path.join(tmp, "wsig")
            subprocess.run(["cc", "-std=c11", "-O2", "-I", inc, SRC, "-o", host], check=True)
            out = subprocess.run([host], capture_output=True, text=True, check=True).stdout
        cases = parse(out.splitlines())
        checked = 0
        for name in cases:
            word, m, depth, glide = name.split()
            if glide != "g0":
                continue
            held = cases[name][0]
            rising = cases["%s %s %s g+" % (word, m, depth)][0]
            falling = cases["%s %s %s g-" % (word, m, depth)][0]
            over = cases["%s %s %s g^" % (word, m, depth)][0]
            # Depth 255 of the noise mode is pure white noise: its pitch never reaches the output, so all three
            # variants agreeing is the correct result and not a missed glide.
            if m == "2" and depth == "d255":
                self.assertEqual(len({held, rising, falling, over}), 1,
                                 "%s: pure noise should not depend on pitch" % name)
                continue
            self.assertNotEqual(held, rising, "%s: the rising glide rendered identically to the held note" % name)
            self.assertNotEqual(held, falling, "%s: the falling glide rendered identically to the held note" % name)
            self.assertNotEqual(rising, falling, "%s: rising and falling glides are indistinguishable" % name)
            # The over-ceiling case: the increment is clamped while the raw pitch is above it, so it must part
            # from the held note once the glide comes back down.
            self.assertNotEqual(held, over, "%s: the over-ceiling glide rendered like a held note" % name)
            checked += 1
        self.assertGreater(checked, 0, "no glide cases were checked")


if __name__ == "__main__":
    unittest.main()
