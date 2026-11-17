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
| 171–255 | noise | `opz_noise` + BLEP saw | blends saw toward white noise | spectral flatness **−9.4 → −1.8 dB** |

All three are monotone over the whole dial, and the resonant peak follows the filter word (130 Hz at `filter` 40
to 3,690 Hz at 200). A parameter that does not move its mode's metric is a bug in the engine, not in the metric.

The saw mode is worth understanding before extending it. Scaling the PolyBLEP residual *looks* like "less
correction" and is not: with the residual scaled to zero it becomes a full-scale step once per cycle — worse than
either end and non-monotone in every metric tried (measured, then discarded). Blending the whole waveform against
the raw phase ramp is the honest way to remove a correction, and the metric it then exposes is what the kernel
buys: 10.4 dB of inharmonic energy at C6.

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
sine     210.4                 213.3
saw      191.7                 195.2
noise    196.6                 200.0
```

Over the 167 line on purpose, and affordable because the budget is one shared pool, not a per-voice wall
([cpu-headroom.md](../../research/cpu-headroom.md) §1): a mono instrument has 405 at one voice. Render stack
464 B, voice state 24 B of the 280 allowed, device image 5,020 of 5,120 B — 98 % of slot 74, so the next mode
must first share the per-mode loop bodies rather than add a fourth copy.

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

All twelve cases are bit-identical, across `opz_sine`, `opz_saw_blep`, `opz_noise`, `opz_soft`, `opz_lerp`,
`opz_blep_recip`, `opz_exp2_q16`, `opz_svf_coef`, `opz_svf` and the `opz_mix_mono` Blackfin loops. That is
informative rather than empty, because `test_opz_cycles` reports three helpers whose two builds disagree:

```
opz_mul_q31              host bc9882ce   device 256d1885
opz_mul_q31_q15          host 6ed4596f   device 849f7e48
opz_svf_coef opz_svf     host b2e95b4f   device c3994a41
```

An engine that runs `opz_svf` on every sample agrees to the bit, and a probe over 400 random inputs per multiply
found no disagreement either. The divergence is therefore confined to the **edge inputs** `sdk_check.c`
deliberately folds in (`0x80000000`, `0x7fffffff`, ±`0x40000000`, …) — a saturation path, not ordinary
arithmetic. The open failure stays visible in `test_opz_cycles` with its scope stated, rather than being papered
over by a gate that happens to pass.

## Adding a mode

1. Pick a `P1` zone and give it one kernel path; keep the branch outside the per-sample loop.
2. Name the metric that must move monotonically with `P2`, and measure it before writing it down.
3. Add a case to `engines/witness/tests/modes.scn` and regenerate the golden
   (`python3 sim/tests/golden.py --build sim/build --update --engine witness`).
4. Add the mode to the differential gate: `tools/witness/witness_sig.c` loops over `p1[3]` — extend the arrays.
5. Re-measure cost with `tools/opz_cycles.py engines/witness --quick` and update this page if a number moved.

## What it does not cover

The host's ADSR model (the gate holds the envelope flat on purpose, so the comparison is about the engine's
arithmetic), voice allocation and stealing, and parameter smoothing — those are the host's, and `sim/tests`
covers them directly.
