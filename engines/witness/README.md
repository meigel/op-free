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
| 171–255 | noise | `opz_noise` + BLEP saw | blends saw toward white noise | spectral flatness, **−9.4 → −1.8 dB** |

All three are monotone over the full dial, measured through `opzsim` on a held C3 (C6 for the saw mode, where the
alias lands) — see `docs/` for the analysis commands. The resonant peak follows the filter word: **130 Hz at
`filter` 40 to 3,690 Hz at 200**, monotone, which is what makes the noise mode a filter measurement.

The saw mode is worth a sentence: scaling the PolyBLEP residual looks like "less correction" and is not — at zero
the residual becomes a full-scale step once per cycle, which is worse than either end and non-monotone in every
metric tried. Blending the whole waveform against the raw ramp is the honest way to remove a correction.

The filter is the engine's own (`flags 0x04`, see below), and it is **always in circuit**: `filter` 255 is 16 kHz,
not a bypass. A diagnostic wants one disposition — if the cost or the transfer function changed with the knob,
the measurement would be of the knob rather than of the mode.

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
sine        210.4                 213.3
saw         191.7                 195.2
noise       196.6                 200.0
```

That is honest rather than accidental: the state-variable filter runs on every sample, in every mode, because
`flags 0x04` handed it to the engine and a diagnostic wants one disposition. Against the pool it is affordable
at one voice (405 available in the drums-1-voice layout) and at two (200–268), and not beyond. If you need it
polyphonic, that is the knob to measure, not to assume. Render stack: 464 bytes, 24 bytes of voice state.

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

Slot 74, plug id 61204, device image **5,020 of 5,120 B (98 %)** — the next mode must share the per-mode loop
bodies rather than add a fourth copy.

Full detail, including how to add a mode: [`sdk/docs/conformance.md`](../../sdk/docs/conformance.md). See also
`sdk/docs/engine-abi.md`, `sdk/docs/parameter-scheme.md` and `research/crash-safety.md`.
