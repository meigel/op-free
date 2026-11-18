# The conformance instrument and the bit gate

Two things that exist so the rest of the platform can be *checked* rather than trusted: an engine whose job is
measurement (`engines/witness`) and a test that renders it on the host and on the device and compares the bits.

## Why a separate engine

Every other engine in `engines/` is designed to be played. `witness` is designed to be measured, and it wants
signals no musical preset should ship: a full-scale sine, a naive (aliased) saw, white noise through a resonant
bandpass. Keeping it separate keeps the musical engines' character and their atlas gates intact.

## The three modes

`P1` picks the mode, `P2` the depth. The branch is taken once per block, so the per-sample cost is that of one
kernel rather than all of them.

| `P1` | mode | core | `P2` does | metric, measured through `opzsim` |
|---|---|---|---|---|
| 0–85 | sine | `opz_sine` | blends a clean sine toward `opz_soft` drive | harmonic distortion **−62.4 → −23.5 dB** |
| 86–170 | saw | `opz_saw_blep` | blends the band-limited saw toward the naive ramp | inharmonic energy at C6 **−28.6 → −18.2 dB** |
| 171–255 | noise | `opz_noise` + BLEP saw | blends saw toward white noise | spectral flatness **−9.4 → −1.7 dB** |

All three are monotone over the whole dial, and the resonant peak follows the filter word (65 Hz at `filter` 40,
160 at 80, 440 at 120, 1,290 at 160, 3,505 at 200). A parameter that does not move its mode's metric is a bug in
the engine, not in the metric.

Every one of those is measured with `resonance` at 0, because `opz_svf`'s adds saturate: on the clean sine at
`filter` 92 the third harmonic sits at **−80.7 dB at resonance 0 and −16.7 dB at 32**, so from about 32 upward the
filter is a saturating resonant filter rather than a linear one. That is the platform's kernel, and it is why the
metrics — and the presets — stay in the linear region.

The saw mode is worth understanding before extending it, because the obvious implementation is wrong. Scaling the
`recip` argument — the reciprocal of the increment that the PolyBLEP residual is normalised against — is not
"scaling the correction down": it changes the width of the polynomial's window, and at zero the residual collapses
to a full-scale step once per cycle. Measured that way the inharmonic energy went *down* from depth 0 to 192 and
then jumped back up at 255 — non-monotone, and worse than either end. What removes a correction is blending the
whole waveform against the raw phase ramp, with two Q15 weights so the difference never saturates; then the metric
is monotone and it exposes what the kernel buys: 10.4 dB of inharmonic energy at C6.

## Why `flags 0x04`

The plug sets it, so the OS skips its track filter on this track and hands `filter`/`resonance` to the engine,
returning roughly 17 cycles/sample ([cpu-headroom.md](../../research/cpu-headroom.md), 167 → 184). For a
diagnostic that is doubly right: two more of the eighteen parameter words become the engine's own, and the SVF
they drive is `opz_svf`, one of the kernels the gate below compares.

The filter is **always in circuit** — `filter` 255 is 16 kHz, not a bypass — because a measurement wants one
disposition: if the cost and the transfer function changed with the knob, the measurement would be of the knob.

## Cost

```
mode     cycles/sample/voice   max (knobs moving)
sine     210.8                 211.0
saw      170.3                 172.2
noise    182.3                 184.1
```

Over the 167 line on purpose. That line is the worst case for 19 sounding voices across all eight tracks, and the
timed stage is one shared pool; the per-voice allowance rises as fewer voices sound (16 → 200, 12 → 268, 8 → 405,
per the table in [cpu-headroom.md](../../research/cpu-headroom.md) §1). So this engine is affordable when it is the
only thing playing and its tracks are mono — the 405 figure is for eight sounding voices with *every* track mono,
not for one instrument on its own.

Render stack: 412 B, voice state 28 B of the 280 allowed, device image 4,060 of 5,120 B — 79 % of slot 74, with room for a fourth
mode.

The image is worth a note because it nearly cost the slot: the filter's reciprocal was written as a 64-bit divide,
which pulls `___divdi3` into the image — 1,206 bytes, a fifth of a 5 KB window, for one division per block. The
same value in 32 bits (`0x80000000u / q_q16`, then shifted) removes it and the image by 1.5 KB. Check
`bfin-elf-nm --size-sort` before adding a mode: the library helpers are bigger than the DSP.

## The gate

`tools/witness/witness_sig.c` compiles the engine in, renders every `(mode, depth)` deterministically and prints
one FNV-1a digest per case. `tools/tests/test_witness_bit_identical.py` builds it twice — `cc` for the host,
`bfin-elf-gcc` for the device, run in `bfin-elf-run` — and requires the two outputs to match line for line. A
mismatch names the mode and the depth.

```bash
python3 -m unittest tools.tests.test_witness_bit_identical -v
```

Run it whenever you touch an engine that runs a shared kernel. It is the only check here that compares what the
simulator renders with what the device renders.

### What it currently says

All twelve cases are bit-identical today, across `opz_sine`, `opz_saw_blep`, `opz_noise`, `opz_soft`, `opz_lerp`,
`opz_blep_recip`, `opz_exp2_q16`, `opz_svf_coef`, `opz_svf` and the `opz_mix_mono` Blackfin loops. That is
informative rather than empty, because `test_opz_cycles` reports three helpers whose two builds disagree:

```
opz_mul_q31              host bc9882ce   device 256d1885
opz_mul_q31_q15          host 6ed4596f   device 849f7e48
opz_svf_coef opz_svf     host b2e95b4f   device c3994a41
```

That is evidence, not a proof of where the helper divergence lives. Twelve deterministic renders say the kernels
this engine exercises agree on *these* inputs; they say nothing about `sdk_check.c`'s own sequence, which mixes
edge values with `pick()`-drawn ones. The working hypothesis stays what it was — the disagreement is in the edge
inputs (`0x80000000`, `0x7fffffff`, ±`0x40000000`, …) — and the open failure stays visible in `test_opz_cycles`
with its scope stated, rather than being papered over by a gate that happens to pass.

## Adding a mode

1. Pick a `P1` zone and give it one kernel path; keep the branch outside the per-sample loop.
2. Name the metric that must move monotonically with `P2`, and measure it before writing it down.
3. Add a case to `engines/witness/tests/modes.scn` and regenerate the golden
   (`python3 sim/tests/golden.py --build sim/build --update --engine witness`).
4. Add the mode to the differential gate: one entry in each of the `p1`, `filter` and `reso` arrays in
   `tools/witness/witness_sig.c`. The case count is derived from those arrays and printed as `cases N`, so the
   Python test needs no edit and cannot silently keep testing three modes.
5. Re-measure cost with `tools/opz_cycles.py engines/witness --quick` and update this page if a number moved.

## What it does not cover

The host's ADSR model (the gate holds the envelope flat on purpose, so the comparison is about the engine's
arithmetic), voice allocation and stealing, and parameter smoothing — those are the host's, and `sim/tests`
covers them directly.
