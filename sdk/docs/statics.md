# File-scope statics: which engines assume one instance, and why

Every engine is loaded into one address space and renders one voice at a time. That makes a `static` buffer a cheap
way to get scratch space — and it silently encodes a set of assumptions that no test states: **one instance per
process, one voice rendering at a time, and a fixed note order.** This is the register of those assumptions,
classified, with the one that has an audible consequence measured rather than asserted.

Fifteen of the thirty-three engines keep file-scope mutable state. The rest keep everything in the voice record and
are instance-safe by construction.

Enforced by `tools/singleton_audit.py --check`, which reads the table below and fails when an engine grows a static
that is not classified here. To add one: classify it in this file, in the same change. CI runs it through
`tools/tests/test_singleton_audit.py`.

## The classes

| class | what it is | safe because | breaks when |
|---|---|---|---|
| `scratch` | a per-block temporary: one voice fills it, the same voice reads it back inside the same render call | `render` is called for one voice at a time and returns before the next starts | two voices render concurrently, or a render is re-entered |
| `seed` | a PRNG advanced at note-on, so each note gets a different noise or detune draw | the draw order is fixed | **notes arrive in a different order** — measured below |
| `shared-track` | state keyed by track and deliberately outliving the note: the choke counters, the per-track LFO | one engine owns all eight tracks, and the sharing is the feature | two instances share a process (they would choke each other) |
| `cross-note` | a voice pool that outlives notes: pluck's string lines and slot clock | allocation is deterministic in note order | notes are reordered, or two instances share the pool |
| `cache` | coefficients or tables derived from the current parameters, kept to avoid recomputing them per block | they are recomputed when the input changes | never, as long as the key covers every input |
| `built-once` | a derived table built in the first render or in `init` and only read afterwards | nothing writes it again | a second instance wants different contents |
| `singleton-bank` | one patch bank per address space (DX7 patches) | the device has one engine per slot | two instances want independent patch banks |

## The register

| engine | static | class | note |
|---|---|---|---|
| 8bit | s_choke | shared-track | closed hat bumps it, open hat compares; per track |
| 8bit | s_tone | scratch | the tone channel of the voice being rendered |
| chip | s_voices | seed | note-on noise register, advanced per voice in init order |
| chip | s_c | built-once | running integral of each wave table, built at init |
| chip | s_metal | seed | the metal-noise shift register, advanced per note |
| dx | s_bank | singleton-bank | 32 DX7 patches in packed format, one bank per process |
| dx | s_bus | scratch | three algorithm buses for the current block |
| ensemble | s_em | scratch | partial integrator for the current block |
| ensemble | s_es | scratch | partial integrator for the current block |
| ensemble | s_lfo | shared-track | one LFO pair per track, deliberately across notes |
| ensemble | s_seed | seed | one draw per note-on for detune noise |
| fmperc | s_choke | shared-track | closed hat bumps it, open hat compares; per track |
| fmperc | s_mod | scratch | pass 1 writes the modulator, pass 2 reads it |
| hats | s_choke | shared-track | closed hat bumps it, open hat compares; per track |
| hats | s_d | scratch | the six squares' difference buffer for the block |
| hats | s_cache | cache | two alternating filter-coefficient slots |
| hats | s_cache_next | cache | which slot the next block builds into |
| kit | s_choke | shared-track | closed hat bumps it, open hat compares; per track |
| kit | s_svf_cache | cache | noise-filter coefficients per model at the last tune |
| kit | s_tone | scratch | the tone channel of the voice being rendered |
| kit | s_noise | scratch | the noise channel of the voice being rendered |
| kit | s_click | scratch | the click channel of the voice being rendered |
| mallet | s_seed | seed | one draw per hit for the click's noise |
| mallet | s_acc | scratch | the partials summed for the current block |
| modal | s_seed | seed | one draw per note for the exciter noise |
| modal | s_acc | scratch | the partials summed for the current block |
| pd | s_y | scratch | the block's waves: two, and the resonance waves' sine, rewritten every render |
| pluck | s_line | cross-note | the string lines; a note's line rings past its note |
| pluck | s_seen | cross-note | when each slot was last taken, for stealing |
| pluck | s_clock | cross-note | monotonic slot clock, advanced per note-on |
| pluck | s_seed | seed | one draw per note for the exciter burst |
| pluck | s_exc | scratch | the exciter burst for the current string step |
| pluck | s_str | scratch | the string output for the current step |
| pluck | s_buf | scratch | the loop signal between passes |
| super | s_em | scratch | partial integrator for the current block |
| super | s_es | scratch | partial integrator for the current block |
| super | s_seed | seed | one draw per note for detune noise |
| swarm | s_el | scratch | partial integrator for the current block |
| swarm | s_er | scratch | partial integrator for the current block |
| swarm | s_pa | scratch | the left group's partials |
| swarm | s_pb | scratch | the right group's partials |
| swarm | s_seed | seed | one draw per note for detune noise |
| sync | s_e | scratch | the band-limited edge accumulator for the block |
| sync | s_k0 | scratch | residual on sample N for the state's c0 |
| sync | s_k1 | scratch | residual on sample N + 1 for the state's c1 |
| sync | s_seed | seed | one draw per note for the noise source |
| table | s_line | scratch | the blended table stretch for the block |
| table | s_pos | scratch | read positions into those stretches |
| table | s_gain | scratch | the envelope times the crossfade weight |
| table | s_co | scratch | additive-path coefficients at the block's end |
| table | s_recip | built-once | reciprocal ramp, built at init |
| vowel | s_x | scratch | source buffer, zeros between renders |
| vowel | s_y | scratch | formant sum for the block |
| vowel | s_run | scratch | one singer's glottal pulse edge list |
| vowel | s_runs | scratch | the two singers' edges merged |
| vowel | s_seed | seed | one draw per note for the breath noise |

## The measured consequence: a chord depends on the order its notes arrive

The `seed` class is the one with a musical consequence, so it was measured instead of argued. Two scenarios render the
**same two-note chord** (C4 and E4, both at step 0, length 2, velocity 110, `track lead`), differing only in the order
the two `note` lines are written. The difference is reported relative to the signal's own RMS.

| engine | hash | signal | relative difference | reading |
|---|---|---|---|---|
| pluck | differs | −42.7 dBFS | **+0.7 dB** | a different sound |
| swarm | differs | −26.6 dBFS | **+1.0 dB** | a different sound |
| ensemble | differs | −27.9 dBFS | **+4.7 dB** | a different sound |
| vowel | differs | −25.0 dBFS | **−3.3 dB** | a different sound |
| super | differs | −27.3 dBFS | −6.4 dB | audibly different |
| sync | differs | −22.6 dBFS | −12.3 dB | audibly different |
| mallet | differs | −43.5 dBFS | −79.9 dBFS | one or two LSBs: identical to the ear |
| modal | differs | −49.7 dBFS | −74.8 dB | one or two LSBs: identical to the ear |
| chip | identical | −26.9 dBFS | — | the seeded register is not reached at this patch |
| 8bit, kit | identical | — | — | choke state only, as designed |
| crease, table | identical | — | — | control: no seed, so order cannot matter |

Two lessons from that table, both of which argue against trusting the hash:

- **A differing hash is not a defect.** mallet and modal differ in the file and not in the ear. Any audit that stopped
  at "hash differs" would have reported two engines as broken on one or two least-significant bits.
- **A differing sound is not automatically a defect either** — but it is a property a golden cannot pin. Four engines
  render a materially different chord from the same notes in a different order. Because the goldens fix one order, they
  pass either way, and a host that delivers a chord in another order gets another sound.

`chip` is worth keeping as it is: the seed exists and advances, but this patch's oscillator does not consume it, so the
measured difference is zero. That is a fact about the patch, not about the code — do not read "identical" as "no
seed".

## What this means for a new engine

- **Prefer the voice record.** Scratch in the `State` costs nothing a static does not: both are one address, and the
  state is already sized and reported by `opz_build.py`.
- **If a static is unavoidable, classify it here in the same change.** The check fails otherwise, which is the point.
- **A noise seed derived from the note, not from a counter, is order-independent** — for example from the pitch,
  velocity and sample position of the note-on. That is the fix if the order-dependence in the four loud engines is
  ever judged not worth keeping; it changes every golden of those engines, so it is a deliberate decision rather than
  a cleanup.
- **Concurrency is not a device concern.** The OP-Z renders one voice at a time, so `scratch` is safe there. It becomes
  unsafe the moment two voices render in parallel, which is a desktop-host question, not a firmware one.

## Evidence boundary

The table above was produced by static analysis of `engines/*/*.c` at this commit, and the order-dependence by
rendering through `sim/build/opzsim`. Not measured: whether the four loud engines sound *worse* for their
order-dependence (pluck's exciter noise and vowel's breath differ per note by design — the question is whether the
difference should instead follow the note rather than the arrival order), and whether any of these engines renders
correctly under concurrent voices. The cross-track `s_choke` sharing was read from the code and its behaviour on
hardware was not exercised.
