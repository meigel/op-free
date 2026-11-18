# witness

A conformance instrument. Every other engine in this tree is written to sound good; this one is written to be
**checked**. It is not a preset bank you would put in a song.

## The three modes

`P1` picks the mode, `P2` sets the depth inside it. Each mode routes the whole block through exactly one kernel,
so the per-sample cost is that of one kernel and not of three, and each answers a different question.

| P1 | mode | core | P2 does | metric that moves with it |
|---|---|---|---|---|
| 0–85 | sine | `opz_sine` | blends a clean sine toward `opz_soft` drive | harmonic distortion, **−62.4 → −23.5 dB** |
| 86–170 | saw | `opz_saw_blep` | blends the band-limited saw toward the naive ramp, removing the correction | inharmonic energy at C6, **−28.6 → −18.2 dB** |
| 171–255 | noise | `opz_noise` + BLEP saw | blends saw toward white noise | spectral flatness, **−9.4 → −1.7 dB** |

All three are monotone over the full dial, measured through `opzsim` on a held C3 (C6 for the saw mode, where the
alias lands) — see `docs/` for the analysis commands. The resonant peak follows the filter word: **65 Hz at `filter` 40, 160 Hz at 80, 440 Hz at 120,
1,290 Hz at 160, 3,505 Hz at 200**, monotone, which is what makes the noise mode a filter measurement.

The saw mode is worth a sentence: scaling the PolyBLEP residual looks like "less correction" and is not — at zero
the residual becomes a full-scale step once per cycle, which is worse than either end and non-monotone in every
metric tried. Blending the whole waveform against the raw ramp is the honest way to remove a correction.

The filter is the engine's own (`flags 0x04`, see below), and it is **always in circuit**: `filter` 255 is 16 kHz,
not a bypass. A diagnostic wants one disposition — if the cost or the transfer function changed with the knob,
the measurement would be of the knob rather than of the mode.

## The filter saturates, and from where

`opz_svf`'s adds saturate, and a resonant peak of gain ~Q drives them past full scale long before the dial reaches
255. Measured on the clean sine (`P2` 0) with the cutoff near the note (`filter` 92), third-harmonic distortion:

```
resonance    0     32     64    128    255
THD      -80.7  -16.7  -15.9  -15.6  -15.6 dB
```

So the filter is linear at `resonance` 0 and effectively a saturating resonant filter from about 32 upward. That
is the platform's SVF, not this engine's choice, and it is why every metric in the table above is measured with
`resonance` at 0. That is also the honest reading of the presets: the sine and saw banks sit at `resonance` 0,
where the filter is linear, and the two noise banks sit at 20 and 40, where it is deliberately not — a resonant
peak is the whole point of them. Up to Q ~1 is where the "clean reference" claim actually holds.

## The parameter words

The ABI hands an engine 18 words. Only four are the engine's to apply; the rest are the OS's, and this engine's
job is to *respond* to them correctly rather than to take them over.

| words | owner | how witness uses them |
|---|---|---|
| `P1`, `P2` | engine | mode and depth, above |
| `filter`, `resonance` | **engine here** | own SVF, because `flags 0x04` skips the OS track filter; 20 Hz–16 kHz in equal octaves, Q 0.7–15.5 |
| `pan`, `level` | engine | `opz_mix_mono`: the OS applies neither, and takes the FX sends after both |
| `attack`…`release` | host | stepped per sample with `opz_env_tick` |
| `fx1`, `fx2` sends | host | taken after level and pan |
| `portamento` | host | arrives as `pitchSlope`; the increment is re-bounded every sample |
| LFO depth/speed/target/shape | host | routed by the OS to `P1`/`P2` |
| `note_style` | host | legato moves pitch without a note-on, so `render` reads pitch every block |

## Why `flags 0x04`

It skips the OS track filter on this track and hands `filter`/`resonance` to the engine, which returns roughly
17 cycles/sample that the OS filter was spending (`research/cpu-headroom.md`, 167 → 184). For a diagnostic that
is doubly right: the words become the engine's own, and the SVF those words drive is `opz_svf` — one of the
kernels whose host and device builds are compared below.

## Playing it

Play it **mono**. The cycle budget is a shared pool, not a per-voice wall (`research/cpu-headroom.md` §1: 167
cycles/sample/voice is only the worst case at 19 voices; a synth alone on its tracks may spend most of the timed
stage).

Measured on the device model (`tools/opz_cycles.py engines/witness --quick`), and it is **over the 167 line**:

```
mode        cycles/sample/voice   max (knobs moving)
sine        210.8                 211.0
saw         170.3                 172.2
noise       182.3                 184.1
```

That is honest rather than accidental: the state-variable filter runs on every sample, in every mode, because
`flags 0x04` handed it to the engine and a diagnostic wants one disposition. Against the pool it is affordable
at one voice (405 available in the drums-1-voice layout) and at two (200–268), and not beyond. If you need it
polyphonic, that is the knob to measure, not to assume. Render stack: 412 bytes, 28 bytes of voice state (the track, for the noise seed).

## The gate it exists for

```bash
python3 -m unittest tools.tests.test_witness_bit_identical -v
```

`tools/witness/witness_sig.c` compiles this engine in, renders every `(mode, depth)` deterministically and prints
one FNV-1a digest per case. The test builds it twice — `cc` for the host, `bfin-elf-gcc` run in `bfin-elf-run`
for the device — and requires the outputs to match line for line. A difference is not a voice defect: it
localises a kernel whose two builds disagree, which is a platform bug the simulator would otherwise hide.

**Currently: all twelve cases are bit-identical**, across `opz_sine`, `opz_saw_blep`, `opz_noise`, `opz_soft`,
`opz_lerp`, `opz_blep_recip`, `opz_exp2_q16`, `opz_svf_coef`, `opz_svf` and the `opz_mix_mono` Blackfin loops.
That is what confines the known `test_opz_cycles` divergence (`opz_mul_q31`, `opz_mul_q31_q15`, `opz_svf`) to the
edge inputs that check folds in.

Slot 74, plug id 61204, device image **4,060 of 5,120 B (79 %)**. It was 5,544 before the review: the 64-bit divide
that computed the filter's reciprocal pulled `___divdi3` into the image — 1,206 bytes, a fifth of the window, for
one division per block. Done in 32 bits it is gone, and the image came down by 1.5 KB.

Full detail, including how to add a mode: [`sdk/docs/conformance.md`](../../sdk/docs/conformance.md). See also
`sdk/docs/engine-abi.md`, `sdk/docs/parameter-scheme.md` and `research/crash-safety.md`.
