# crease

A wavefolder voice for the bass, lead, arp and chord tracks, and the answer to "the OP-Z needs something rough".
Three band-limited saws give the body; a triangle core and a triangle sub are folded through a sine shaper, so the
peaks bend back over each other the harder the folder is driven. The OP-Z's own track filter (the filter and
resonance knobs), its LFO page and its FX sends all work on it unchanged, because the folder is the only unusual
thing in it.

| Control | Function |
|---|---|
| P1 **fold** | Two things at once, on purpose: it crossfades the folded layer in over the dry saws, and it opens the folder's drive from half a cycle of the shaper to four. **0**: the dry three-saw voice (wide, slightly beating, ±8 cents) — the folder is out, and the drive setting does not matter. **64**: about a quarter folded, the shaper at 1.5 cycles, a soft buzz on the saws. **128**: half and half, 2.5 cycles: the hollow, reedy middle of the range. **192**: mostly the folder at 3.3 cycles, brass. **255**: the folded layer alone at 4 cycles, dense and screaming. The crossfade carries a matching gain, so sweeping P1 changes the tone without changing the level in the mix (RMS stays inside 3 dB over the whole knob) |
| P2 **bias** | The offset pushed into the folder before the shaper: −0.3 (0), 0 (128), +0.3 (255). Symmetric (128) folds the peaks evenly and gives odd harmonics only — hollow, square-ish, clarinet-like. At either end the folds go uneven, the even harmonics come in and the tone turns bright and brassy; the extremes start to half-wave rectify. Nothing happens at P1 0 (there is no folder to bias) |
| velocity | level, linear (127 = full, 64 ≈ −6 dB, 1 ≈ −40 dB) |
| attack … release | the OP-Z envelope, unchanged; the folder follows it, so a plucked envelope gives the classic folded zap |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it near neutral |
| pan, level | the track's pan and level (yellow and red dial, orange page), applied by the engine as TE's engines do (`opz_mix.h`): level linear (128 = −6 dB, 0 = silent), pan a balance. The FX1/FX2 sends are taken after them, so the sends follow the level dial |
| LFO, portamento, note style | the OS's, as on any track: the LFO can target P1, P2, filter, resonance, pan or level |

**Why the saws are not folded.** A shaper multiplies its input's bandwidth by its turn count, and at 44.1 kHz with no
oversampling whatever leaves Nyquist folds back — for a saw (1/n spectrum) it lands as a rumble under 60 Hz. So the
folded layer is a triangle (1/n²), and the three saws stay outside it. Measured on a C6 at bias 128: the 22–44 Hz band
sits 41–49 dB under the note's own band across P1. Folding the saws instead put it 19 dB under. The two layers in one
engine are also what makes it useful for both jobs: saw leads at low P1, west-coast folded tones at high P1.

**Why a sine shaper.** A triangle folder repeats its ramp by mirroring, which costs a modulo and a fold per extra
turn. A sine needs one cycle per turn and wraps its phase for free, so four turns cost what half a turn costs
(`opz_sine`, ~6 instructions). Its harmonics also roll off faster than a triangle fold's, and it saturates gracefully
when P1 is low and P2 is centred.

**The signal path**, per sample: three PolyBLEP saws (a third of full scale each, so their sum reaches exactly ±1 when
they line up every beat) → the folded layer in parallel: triangle core + triangle sub at −12 dB → one-pole low-pass at
6 kHz → + bias → sine shaper (turns = 0.5…4, as one Q31 multiply and one shift) → one-pole DC blocker at 10 Hz →
crossfade by P1 → velocity, envelope → `opz_mix_mono` (the track's level and pan, added into both channels).

Two details that took measuring rather than reasoning:

- **The DC blocker sits on the folded layer only.** A saw has no DC of its own, and its −2 wrap step saturates a Q31
  blocker: put through one, a plain 1046 Hz saw came out with a mean of 0.3 and full-scale peaks. The shaper's own
  output is smooth, so the blocker sees only the bias's offset there.
- **The saws start at their zero crossing** (phase 0.5, i.e. at silence) on every note. A saw starting at its −1 edge
  steps the blocker into a 50 ms low-frequency thump; starting at zero, the attack is phase-locked and clean.

## Presets

Five per synth track. `slot` in `presets.json` is the track (0 bass, 1 lead, 2 arp, 3 chord); index 0 of each is the
sound a track gets when it switches to crease. Peaks and RMS are opzsim on a single note at velocity 100 (A1 for the
bass presets, A3 otherwise), track level as in the table.

| Track | Preset | Octave | P1 | P2 | A | D | S | R | Level | Peak / RMS | Sound |
|---|---|---|---|---|---|---|---|---|---|---|---|
| bass | **crease bass** | −2 | 150 | 96 | 0 | 70 | 210 | 30 | 210 | −19.7 / −31.1 | the folded layer over the saws, a broad growling bass |
| bass | growl | −2 | 205 | 40 | 0 | 60 | 200 | 25 | 215 | −22.0 / −31.4 | biased down: even harmonics, angry |
| bass | clean sub | −2 | 0 | 128 | 0 | 80 | 230 | 40 | 205 | −16.9 / −28.2 | no folder: three saws with the triangle sub, round |
| bass | rubber | −2 | 96 | 170 | 5 | 90 | 190 | 45 | 210 | −18.7 / −33.7 | light fold, soft attack, elastic |
| bass | acid crease | −2 | 235 | 20 | 0 | 50 | 170 | 20 | 215 | −22.0 / −32.6 | heavily folded and biased: the acid squelch |
| lead | **crease lead** | 0 | 200 | 128 | 5 | 80 | 200 | 60 | 200 | −21.4 / −31.4 | folded saw lead, symmetric |
| lead | screamer | 0 | 255 | 200 | 0 | 70 | 190 | 50 | 205 | −22.6 / −31.6 | full fold, bright and cutting |
| lead | brass | 0 | 140 | 210 | 20 | 90 | 220 | 70 | 200 | −20.3 / −31.4 | soft attack, even harmonics: brass |
| lead | saw lead | 0 | 0 | 128 | 5 | 80 | 210 | 60 | 195 | −17.5 / −30.5 | the dry side of the engine |
| lead | whistle | 0 | 168 | 60 | 10 | 100 | 215 | 80 | 190 | −21.1 / −31.3 | thin, high fold |
| arp | **crease arp** | 0 | 176 | 128 | 0 | 45 | 90 | 25 | 200 | −20.6 / −37.6 | short folded pluck |
| arp | bell arp | 0 | 72 | 40 | 0 | 60 | 40 | 40 | 200 | −18.9 / −41.0 | small fold: a struck, singing bell |
| arp | pluck arp | 0 | 128 | 150 | 0 | 40 | 20 | 30 | 205 | −19.4 / −42.1 | tight folded pluck |
| arp | stab arp | 0 | 224 | 90 | 0 | 35 | 0 | 20 | 205 | −21.8 / −42.5 | one-shot stab |
| arp | spark arp | 0 | 200 | 24 | 0 | 30 | 0 | 15 | 195 | −22.2 / −44.5 | very short, biased, brittle |
| chord | **crease pad** | 0 | 110 | 150 | 90 | 120 | 220 | 150 | 175 | −21.6 / −33.3 | slow swell, half folded |
| chord | wide pad | 0 | 170 | 128 | 120 | 130 | 225 | 170 | 170 | −22.5 / −32.9 | wider, slower |
| chord | hollow | 0 | 80 | 0 | 70 | 110 | 210 | 140 | 180 | −21.5 / −32.8 | odd harmonics only: hollow, woody |
| chord | dark swell | 0 | 40 | 220 | 160 | 140 | 200 | 190 | 185 | −31.2 / −45.6 | long attack, gentle fold (measured over the first two seconds, before the swell opens) |
| chord | drift | 0 | 140 | 60 | 140 | 150 | 215 | 200 | 170 | −27.0 / −38.0 | very slow attack and release, biased, moving |

Every preset: sends 0, pan 128, no portamento, the LFO untouched. Chord presets sit lower in level because four
voices add up. `python3 tools/opz.py presets check engines/crease` reports 20 presets, no warnings. Not yet tuned by
ear on the device.

## Facts

| | |
|---|---|
| Plug | 60906 (test id; a released id from 61000 up is assigned before publishing), `trackmask` 0xF0 (bass, lead, arp, chord), version 1 |
| Memory slot | 31 (10 KB class); the host build measures ~3.8 KB of code and data, so it has room to grow in the slot |
| State | 32 B of 280 per voice (3 phases, sub phase, low-pass, DC blocker ×2, velocity gain, level/pan); `memRequest` 0 |
| Voices | all of them: no voice pool, the cost is per voice and independent of the knob positions |
| CPU | **not measured on Blackfin in this checkout** (no Blackfin toolchain here): run `python3 tools/opz_cycles.py engines/crease`, or `make -C sim cycles`. The case list in `tools/opz_cycles.py` covers the P1/P2 ends, a pluck, a release and 19 voices. Per sample the loop is 3 BLEP saws, 2 triangles, 2 one-poles, the shaper and 6 fractional multiplies — on the host it measures 22 ns/sample/voice against `super`'s 24, and `super` is known at 118 cycles/sample per voice, so ~110–130 cycles looks likely against the 167-cycle budget |
| Level | at track level 255: −12.7 dBFS peak at P1 0, −18.6 at P1 255 (a folded waveform is dense: its crest factor drops, while the RMS stays inside −24.9…−28.1 dBFS across the whole knob). A 4-note chord never clips (worst peak −3.7 dBFS at P1 0) |

Tests ([tests/](tests/)): `basic` (notes across the range, dry and folded, gate, release, voices freed, level), `knobs`
(P1 and P2 swept under a chord with pitch bends), `extremes` (the knob ends on C0 and C8, attack 0, release 0, notes
retriggered on one voice), `chords` (four voices, then six notes into them so the host steals and fades, then full
bias).

## What to listen for on the device

Install it on the lead track (it starts on that track's index-0 preset), or start from P1 96, P2 128 with sustain up:

- **P1:** a slow sweep from 0 to 255 should go from three clean saws to a dense, screaming fold, and the **loudness
  should not change** — if it does, the compensation is wrong and the numbers above say by how much.
- **P2:** at P1 ≥ 128, sweeping P2 from 128 to either end should turn a hollow, square-ish tone into a bright brass
  one, without a jump in level at the ends.
- **No click and no thump on the attack:** single short notes at attack 0 should start clean, with no low-frequency
  bump under them (that is what the phase-0.5 start and the blocker are for).
- **High notes:** C6 and up at P1 255 should stay musical, not fuzzy — there is no oversampling, and the folded layer
  is a triangle for exactly this reason. If a high note sounds dirty, note the pitch and the knob positions.
- **Bass:** "crease bass" and "growl" at C1–C2 should hold up next to volt and the OP-Z's own bass, with the track
  filter around 90–110 for the growl.
- **Chords:** four notes at P1 128–200 should not choke or clip; the chord track's own voice stealing must fade
  smoothly (the host does that; `super` and `swarm` behave the same).
- **Level dial and pan:** both must work on every note (they are applied by the engine, not the OS). A send to FX1
  must follow the level dial down to silence.

## Credits

Original code, MIT licence. No code or data from any other instrument or library: the BLEP polynomial, the sine, the
triangle and the level/pan law are the SDK's (`sdk/include/opz_dsp.h`, `opz_mix.h`). The design is the classic
west-coast signal path — a triangle folded through a sine shaper — applied to a saw-based engine.
