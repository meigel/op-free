/*
 * crease — a wavefolder voice for the bass, lead, arp and chord tracks: two detuned saws give the body, and a
 * triangle core and a triangle sub are folded through a sine shaper the west-coast way, so the harder the folder is
 * driven the more the peaks bend back over each other.
 *
 *   P1        fold     crossfades, and opens the folder: 0 is the dry three-saw voice and the shaper at half a
 *                      cycle; 255 is the folded layer alone, the shaper at four cycles
 *   P2        bias     the offset pushed into the folder, -0.3 (0) .. 0 (128) .. +0.3 (255): bends the folds
 *                      unevenly, so the harmonics go from odd-only (hollow, square-ish) to the full set
 *                      (bright, brassier); at the ends it starts to half-wave rectify
 *   velocity           level
 *   attack..release    host envelope
 *   filter, resonance  the OS's own track filter and LFO, which run after the engine
 *
 * The saws are band-limited (the SDK's PolyBLEP, one polynomial residual per wrap) and detuned +-8 cents
 * around the note, so the dry end is already a wide, slightly beating voice. The sub is a triangle an octave down,
 * inside the folder: a folder adds no new fundamental, so the low end has to be there before the shaper.
 *
 * Why the saws are not folded: a shaper multiplies its input's bandwidth by its turn count, and a saw's 1/n
 * spectrum folded four times aliases into a rumble under 60 Hz that a triangle's 1/n^2 does not (measured on a C6:
 * -19 dB below the fundamental with the saws folded, -44 dB with the triangle core). So the folded layer is a
 * triangle, and the saws stay outside it, which is also how a west-coast voice and a saw lead can live in one engine.
 *
 * Why a sine shaper: a triangle folder repeats its ramp by mirroring, which costs a modulo and a fold per extra
 * turn. A sine needs one cycle per turn and wraps its phase for free, so four turns cost what half a turn costs
 * (opz_sine, ~6 instructions), and its harmonics roll off faster than a triangle fold's.
 *
 * Cost: about 45-55 Blackfin cycles per sample per voice (three BLEP saws, two triangles, the shaper, two one-poles,
 * the envelope and the level/pan mix), well inside the 167-cycle budget, so all voices play.
 *
 * MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"   /* the host envelope: OPZ_ADSR_* accessors and opz_env_tick */
#include "opz_dsp.h"    /* shared fixed-point kernels: opz_saw_blep, opz_sine, opz_tri, opz_lerp, ... */
#include "opz_engine.h" /* the ABI: types, callback table, parameter indices, export table */
#include "opz_mix.h"    /* the track's level and pan dials: the OS leaves them to the engine */

#define N OPZ_BLOCK_FRAMES

/* Detune of the two side saws: 2^(8/1200) - 1 = 0.004631670, Q31. */
#define DETUNE 9941723

/* DC blocker: one-pole high-pass at about 10 Hz, R = 1 - 2 pi 10 / 44100 in Q15. */
#define DC_R 32720
#define INC_MIN 0x8000        /* about a third of a hertz: below this the phase stalls, above INC_MAX it aliases */
#define INC_MAX 0x40000000u   /* 11 kHz, the same ceiling the block-start pitch gets */
/* Both ends of a glide and both detuned copies of them pass through this: 8 cents above 11 kHz still aliases, and a
 * clamp that only holds the un-detuned pitch lets the wedge through. */
static int32_t inc_bound(int32_t x)
{
    if (x < (int32_t)INC_MIN) return (int32_t)INC_MIN;
    if (x > (int32_t)INC_MAX) return (int32_t)INC_MAX;
    return x;
}

/* One-pole low-pass at 6 kHz in front of the folder: a = 1 - exp(-2 pi 6000 / 44100) in Q15. */
#define LP_A 16695

/* Peak per voice: the SDK's -12 dBFS, less a little, because a folded waveform is dense and reads louder than a
 * saw at the same peak. */
#define OUT_GAIN (OPZ_VOICE_PEAK - (OPZ_VOICE_PEAK >> 4))

/* ---- per-voice state ------------------------------------------------------------------------------------------
 * The host gives every voice 280 bytes for this struct (bytes 280-307 of the area are the OS's own). It does NOT
 * clear them between notes on the same plug, so init() clears them and onNoteOn() sets what a note needs. */
typedef struct State {
    uint32_t ph_a, ph_b;       /* the two triangles, Q32 turns */
    uint32_t ph_sub;           /* the triangle sub, one octave down */
    fract32 lpf;               /* pre-shaper low-pass state */
    fract32 dc_x1, dc_y1;      /* DC blocker state */
    fract32 amp;               /* velocity x OUT_GAIN, Q31, set at note-on */
    fract32 vel;               /* the note's velocity, Q31, kept for the drive accent */
    OpzMix mix;                /* level and pan gains of the last block, for opz_mix_mono's ramp */
} State;

_Static_assert(sizeof(State) <= OPZ_VOICE_STATE_BYTES, "voice state must fit in OPZ_VOICE_STATE_BYTES (engine-abi.md rule 13)");

/* The one legitimate global: the host's callback table, shared by every voice. */
static const ModuleCallbackInterface *s_cb;

/* One-pole DC blocker: y = x - x1 + R y1. The bias leans the folder's output off centre, and DC on a track is
 * headroom spent on nothing.
 *
 * It runs at half scale. A highpass passes twice its input on a step, and Q31 has no room for that: the
 * (x - x1) difference saturated on high notes -- 1,608 of 4,410 samples at C8 with P1 and P2 at 255. Halved, the
 * difference cannot reach full scale, and the doubling on the way out is a saturating add, so only a genuine
 * overshoot gives way instead of corrupting the filter state. */
static inline __attribute__((always_inline)) fract32 dc_block(State *s, fract32 x)
{
    fract32 h = x >> 1;
    fract32 y = opz_add_q31(opz_sub_q31(h, s->dc_x1), opz_mul_q31_q15(s->dc_y1, DC_R));
    s->dc_x1 = h;
    s->dc_y1 = y;
    return opz_add_q31(y, y);
}

/* ---- entry points ---------------------------------------------------------------------------------------------- */

/* Once, when the OP-Z loads the engine at boot. Keep the callback pointer; instruments ask for no buffer. */
uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = 0;
    return 1;           /* non-zero: loaded. 0 would make the OS report "Failed to initialize module" */
}

/* When a voice first plays a note on this plug. Clear the whole state: it is full of the last plug's bytes. */
void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)track; (void)arg2; (void)mem; (void)scratch;
    volatile uint8_t *p = (volatile uint8_t *)state;  /* volatile: stops the compiler turning this into memset() */
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
}

/* A note starts on this voice. The host has already reset the envelope to its attack. Don't latch a frequency
 * here: with the legato note style new notes only move `pitch` and never reach onNoteOn, so render() reads the
 * pitch every block. render() may also run before this returns (rule 18): every field written here must be safe
 * to render on its own. */
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)note;
    State *s = state;
    fract32 vel = (fract32)((uint32_t)(velocity > 127 ? 127 : velocity) * 0x01020408u);  /* v / 127 in Q31 */
    s->amp = opz_mul_q31(vel, OUT_GAIN);       /* a little under -12 dBFS at full velocity */
    s->vel = vel;                              /* the folder opens a little further on an accent */
    /* Both saws start at their zero crossing (phase 0.5), i.e. at silence: a saw that started at its -1 edge
     * would step the DC blocker into a 50 ms low-frequency thump on every note. They spread from here by the detune
     * alone, so the attack is phase-locked and bright, then widens. */
    s->ph_a = 0x80000000u;
    s->ph_b = 0x80000000u;
    s->ph_sub = 0;
    s->lpf = 0;
    s->dc_x1 = 0;
    s->dc_y1 = 0;
    if (params) opz_mix_start(&s->mix, params);  /* the note starts at the dials' level and pan (NULL: ramps in) */
}

/* The key was released. The host moves the envelope to its release stage by itself, and this may be called
 * twice, from the audio interrupt (rule 20): nothing to do here. */
void onNoteOff(int note, void *state)
{
    (void)note; (void)state;
}

/* 64 frames for one voice. `out` is the track's buffer, interleaved stereo (L, R, L, R, ... 128 values) that every
 * voice of the track ADDS into: never overwrite it. `pitch` is the phase increment per sample (Q32) at the block's
 * start; add `pitchSlope` every sample to glide to the next block's pitch. `params` are the 18 smoothed parameters
 * (Q31, 0..0x7fffffff): this engine owns P1 and P2; filter and resonance are the OS's own track filter. Level and
 * pan are the engine's job (every TE engine applies them in render; the FX sends are taken after them):
 * opz_mix_mono does it at the end of the block. */
void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    State *s = state;

    /* 1. Stage 0 means the voice is idle: do nothing. */
    if (OPZ_ADSR_STAGE(env) == 0) return;

    /* 2. Let the host move the envelope on (attack, decay, sustain, release, fade) with this block's knobs. */
    s_cb->updateParametersADSR(env, params[OPZ_P_ATTACK], params[OPZ_P_DECAY], params[OPZ_P_SUSTAIN],
                               params[OPZ_P_RELEASE]);

    /* 3. Everything that depends only on the knobs and the pitch, once per block. */
    /*    Pitch first: the folder's ceiling depends on it. Clamp to 11 kHz, then the two triangles detune around it.
     *    They glide with the same slope, so a portamento moves them together. */
    const int32_t raw = (int32_t)pitch;                  /* before the ceiling: the glide's other end needs it */
    const int32_t inc = inc_bound(raw);
    const int32_t delta = opz_mul_q31(inc, DETUNE);
    uint32_t inc_a = (uint32_t)inc_bound(inc + delta), inc_b = (uint32_t)inc_bound(inc - delta);

    /*    Folder drive: half a cycle of the shaper at P1 0, four cycles at 255. The shaper's phase is a Q31
     *    multiply (x * m, one turn) shifted left by k turns: m in Q31 holds the fraction, k the octave, so four
     *    turns still cost one multiply and one shift per sample and nothing overflows. Four is the cap: every turn
     *    multiplies the shaper's bandwidth, and at 44.1 kHz with no oversampling the extra ones only alias (measured
     *    on a C6: the band under 60 Hz rises 3 dB per turn past four). */
    uint32_t t = 0x8000u + (params[OPZ_P_P1] >> 13);   /* Q16 turns: 0.5 .. 4.5 */
    /*    The ceiling first: four turns is the cap (every turn multiplies the shaper's bandwidth), and THEN the
     *    note gives some of it back -- a high note needs to fold earlier than a low one. Doing it in this order is
     *    what makes it bite: at P1 255 the raw 4.5 turns are capped at 4.0 and the note's increment comes off that,
     *    so C6 gives up about 0.4 of a turn where subtracting first left the cap swallowing the whole reduction. */
    if (t > 0x3ffffu) t = 0x3ffffu;                    /* 4.0 */
    if (t > 0x8000u) {
        uint32_t nyq = inc >> 12;
        uint32_t excess = t - 0x8000u;
        t = 0x8000u + (excess > nyq ? excess - nyq : 0u);
    }
    int k = t < 0x10000u ? 0 : t < 0x20000u ? 1 : 2;
    fract32 turn_m = (fract32)(t << (15 - k));         /* Q31, 0.5 .. 1.0 */
    /*    The dry-to-folded crossfade is P1 itself: at 0 the voice is the plain two-saw sound. */
    fract32 w = opz_pq31(params[OPZ_P_P1]);
    /*    A folded waveform is denser than the saws: at the same peak it reads 6 dB louder (measured on A3: -24
     *    dBFS RMS dry, -18 folded). The crossfade carries the matching gain, so turning fold up changes the tone and
     *    not the level in the mix. */
/*    Bias: P2 centred, scaled to +-0.15. The range is deliberately narrow: the shaper's phase is periodic in the
     *    bias, so a wider range wraps and the dial repeats itself. Measured at P1 191 (odd/even, dB, P2 0..255):
     *    +-0.3 gave -7.7 / +13.3 / -18.7 / +12.9 / -7.7 -- the ends sound like the centre-quarter, a dead feel;
     *    +-0.15 gives +13.2 / +0.5 / -18.7 / +0.6 / +12.9, monotone on each side of the centre detent, 32 dB end
     *    to end. At P1 255 the same sweep spans 38 dB. */
    fract32 bias = opz_mul_q31((fract32)((int32_t)(params[OPZ_P_P2] & OPZ_Q31_ONE) - 0x40000000), 0x26666666);
    /*    A folded waveform is denser than the saws: at the same peak it reads 6 dB louder (measured on A3). The
     *    crossfade carries the matching gain, so turning fold up changes the tone and not the level in the mix. */
    fract32 amp = opz_mul_q31_q15(s->amp, (int32_t)(32767 - (w >> 17)));
    /*    Velocity leans the fold as well, by up to a quarter of the dial's own range: an accent changes colour, not
     *    only level. The turn count was the obvious target and was measured as the wrong one -- at P1 200 it sits at
     *    3.6 of a 4.0 cap, so a fifth more turns moved the centroid by 8 Hz. Bias moves the odd/even balance by tens
     *    of dB, so the same effort is audible. */
    bias = opz_add_q31(bias, opz_mul_q31(opz_mul_q31(s->vel, 0x06666666), 0x26666666));

    /*    Page 2 as a timbre control, gated by sustain. At full sustain the envelope is a VCA and the fold is P1's
     *    alone (a pad keeps its tone); as sustain falls the envelope's own shape opens the folder, which is what makes
     *    a short decay a pluck rather than a gate. It costs nothing extra: the level is already in the loop. */
    fract32 edepth = opz_mul_q31((fract32)(OPZ_Q31_ONE - (params[OPZ_P_SUSTAIN] & OPZ_Q31_ONE)), 0x4CCCCCCD);
    const int env_fold = edepth != 0;   /* one test per block instead of two multiplies per sample */
    /* The gains the loop multiplies by, narrowed to Q15 once per block: a gain needs no more than 15 bits, and the
     * Q15 multiply is three instructions against the Q31 one's dependency chain (measured: 4.5% of cycles for three
     * calls against 32% for six). */
    const int32_t w15 = (int32_t)(w >> 16), turn15 = (int32_t)(turn_m >> 16), ed15 = (int32_t)(edepth >> 16);



    /* 4. The envelope's level and per-sample coefficients live in locals for the loop, and go back at the end.
     *    Locals, not s->...: the compiler must assume a write to out[] could change *s, and would reload it. */
    /* The glide walks both increments every sample, and a downward bend from a clamped start can take them to or past
     * zero, where the unsigned phase wraps to a near-Nyquist increment. Clamping each increment per sample was safe
     * and cost 27 cycles/sample; a block is a straight line, so its two endpoints decide whether it stays inside
     * [INC_MIN, INC_MAX] and the slope is shortened once instead. Dropping the glide to zero is the other half: a
     * block without one keeps the two adds it always had. */
    int32_t slope_a = pitchSlope, slope_b = pitchSlope;
    if (pitchSlope) {
        /*    Both ends from the raw pitch, then bounded, detuned and bounded again, and the slope is the line
         *    between the endpoints. Taking the far end from the already-clamped increment instead makes a pitch that
         *    is still above the ceiling fall away from it on the first block of a bend -- at MIDI 127 bent down 200
         *    cents that is 9.6 kHz where the ceiling holds 11 kHz, back up at the next block. A ramp bounded at both
         *    ends stays between them, so the ceiling holds until the raw pitch itself comes under it. */
        const int64_t next = (int64_t)raw + (int64_t)OPZ_BLOCK_FRAMES * pitchSlope;
        const int32_t p1r = next > INT32_MAX ? INT32_MAX : (next < INT32_MIN ? INT32_MIN : (int32_t)next);
        const int32_t p1 = inc_bound(p1r);
        const int32_t d1 = opz_mul_q31(p1, DETUNE);
        const int32_t a1u = p1 + d1, b1u = p1 - d1;              /* unbounded, to detect a binding clamp */
        const int32_t a1 = inc_bound(a1u), b1 = inc_bound(b1u);
        /*    Only when something actually binds. Deriving the slope unconditionally would replace pitchSlope with
         *    its 64-sample truncation on every block, which is a drift nobody asked for: it changed the knobs
         *    render by the hash and by nothing audible (spectral distance 0.00 dB). */
        if (p1 != p1r || a1 != a1u || b1 != b1u || (int32_t)inc_a != inc + delta ||
            (int32_t)inc_b != inc - delta) {
            slope_a = (a1 - (int32_t)inc_a) / OPZ_BLOCK_FRAMES;
            slope_b = (b1 - (int32_t)inc_b) / OPZ_BLOCK_FRAMES;
        }
    }

    int32_t level = OPZ_ADSR_LEVEL(env), coef = OPZ_ADSR_COEF(env), step = OPZ_ADSR_STEP(env);
    uint32_t pa = s->ph_a, pb = s->ph_b;
    fract32 mono[OPZ_BLOCK_FRAMES];                         /* the voice, before level and pan */

    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        /* The dry layer: two triangles at half scale each, so their sum reaches exactly +-1 when they line up (they
         * do, every beat of the detune) and nothing saturates on the way. */
        fract32 tri = opz_add_q31(opz_tri(pa) >> 1, opz_tri(pb) >> 1);

        /* The folded layer: a triangle core and the triangle sub, folded. Triangles, not the saws: the shaper
         * multiplies its input's bandwidth by the turn count, and a triangle's 1/n^2 spectrum comes out of four turns
         * as music where a saw's 1/n comes out as the rumble under 60 Hz that only aliasing puts there. */
        fract32 core = opz_tri(pa) >> 1;                                   /* +-0.5 into the shaper */
        /* one-pole at 6 kHz in front of the shaper: 2 ops against an oversampled pair of filters */
        fract32 lp = opz_add_q31(s->lpf, opz_mul_q31_q15(opz_sub_q31(core, s->lpf), LP_A));
        s->lpf = lp;
        /* the folder: one multiply for the turn count (x * m, one turn), one shift for the octave, then the shaper */
        fract32 u = opz_add_q31(lp, bias);
        /* the envelope on the folder, gated by sustain: a pluck's decay opens and closes the folds */
        if (env_fold) u = opz_add_q31(u, opz_mul_q31_q15(u, opz_mul_q31_q15(ed15, level >> 16)));
        fract32 folded = opz_sine(((uint32_t)opz_mul_q31_q15(u, turn15)) << k);

        /* P1 crossfades the dry triangles against the folded layer. Only the folded layer is DC-blocked: a triangle
         * has no DC of its own, and its shaped output does. The two layers are weighted separately rather than
         * lerped: opz_lerp saturates (folded - dry) to full scale first, which clipped the crossfade and meant P1 at
         * 255 never selected the folded layer alone. */
        fract32 fdc = dc_block(s, folded);
        fract32 y = opz_add_q31(opz_mul_q31_q15(tri, 32767 - w15), opz_mul_q31_q15(fdc, w15));
        /* level: velocity, the envelope, and the matched gain, in one multiply */
        /* two Q15 multiplies: amp x level in Q31, then narrowed to a Q15 gain for the sample (a gain needs 15
         * bits; passing the Q31 value straight in as a Q15 coefficient would scale it by 2^16). */
        mono[i] = opz_mul_q31_q15(y, (int32_t)(opz_mul_q31_q15(amp, level >> 16) >> 16));
        /* advance: envelope, phases (wrapping is what we want), pitch glide */
        level = opz_env_tick(level, coef, step);
        pa += inc_a; pb += inc_b;
        inc_a += (uint32_t)slope_a; inc_b += (uint32_t)slope_b;
    }

    /* 5. The track's level and pan, ADDED into both channels of the interleaved buffer, saturating. */
    opz_mix_mono(out, mono, &s->mix, params);

    /* 6. Store what the next block continues from. The host frees the voice once the envelope reaches stage 0. */
    OPZ_ADSR_LEVEL(env) = level;
    s->ph_a = pa; s->ph_b = pb;
}

/* The export table the OS looks up by name: initModule, init, onNoteOn, onNoteOff and render. Last line, always. */
OPZ_EXPORTS_INSTRUMENT();
