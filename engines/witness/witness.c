/*
 * witness — a conformance instrument, not a musical voice.
 *
 * It exists to make the platform's own behaviour measurable. Every other engine here is designed to sound
 * good; this one is designed to be checked. Three modes, one kernel path each, so the per-sample cost is
 * that of one kernel rather than all of them, and each mode answers a different question:
 *
 *   P1 0..85     sine      opz_sine.      P2 blends the clean sine toward opz_soft (odd-harmonic drive).
 *   P1 86..170   saw       opz_saw_blep.  P2 blends the band-limited saw toward the naive ramp, i.e. it takes
 *                                         the PolyBLEP correction away, so the alias (inharmonic) energy is what
 *                                         moves. Measure at C6, where a naive saw folds the most.
 *   P1 171..255  noise     opz_noise.     P2 blends a BLEP saw toward white noise, so the filter's resonant
 *                                         peak has a flat excitation and can be measured anywhere in the band.
 *
 *   filter, resonance   the engine's own state-variable filter. The plug sets flags 0x04, so the OS skips its
 *                       track filter on this track and these two words are ours (sdk/docs/plug-format.md);
 *                       that is also what returns the ~17 cycles/sample the OS filter was spending. The filter
 *                       is ALWAYS in circuit: words[8] 255 is 16 kHz, not a bypass, so the cost and the
 *                       transfer function do not change with the knob. A diagnostic wants one disposition.
 *   pan, level          applied by the engine through opz_mix_mono, because the OS applies neither to an
 *                       instrument and takes the FX sends after them (docs/parameter-scheme.md).
 *   attack..release     the host envelope, stepped per sample by opz_env_tick.
 *   portamento          arrives as pitchSlope inside the block.
 *   LFO, note style     host-side; the engine only has to read the pitch every block and tolerate legato.
 *
 * Why a separate plug rather than another mode of crease: a diagnostic is allowed to be ugly. It wants a
 * sine at full scale, a hard-clipped saw and white noise through a resonant bandpass, and those are exactly
 * the settings a musical preset must never ship. Keeping it separate also keeps crease's character and its
 * atlas gate intact.
 *
 * It is meant to be played MONO. The budget is one shared pool, not a per-voice wall (research/cpu-headroom.md:
 * 167 cycles/sample/voice is the worst case at 19 voices; one track alone may spend nearly the whole timed
 * stage), so a single voice can afford every kernel and a real SVF. Do not turn on polyphony and then wonder
 * where the headroom went: measure it with tools/opz_cycles.py before doing that.
 *
 * The point of the modes is that each one is measurable with a different metric and no listening:
 *   sine + soft drive       harmonic distortion rises with P2: measured -62.4 dB at P2 0 to -23.5 dB at 255,
 *                           monotone over the whole dial
 *   saw + BLEP removal      inharmonic energy rises with P2 at C6: -28.6 dB band-limited to -18.2 dB naive,
 *                           monotone; that 10.4 dB is what the PolyBLEP residual buys
 *   noise + BPF             spectral flatness follows P2 (-9.4 dB saw to -1.8 dB noise) and the resonant peak
 *                           follows words[8]: 130 Hz at filter 40 to 3,690 Hz at 200, monotone
 * tools/witness/ carries the bit-signature harness that renders all three on the host and on the Blackfin and
 * compares them sample for sample: that is the gate this engine exists for.
 *
 * MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"
#include "opz_mix.h"

#define N OPZ_BLOCK_FRAMES

/* The same 11 kHz ceiling crease uses: above it the phase increment aliases, and a diagnostic must not
 * report aliasing as if it were the mode's own character. */
#define INC_MIN 0x8000
#define INC_MAX 0x40000000u

/* Peak per voice at full velocity: the SDK's -12 dBFS (opz_engine.h OPZ_VOICE_PEAK), less a little, so three
 * modes with different crest factors all sit under the per-voice target instead of one of them clipping it. */
#define OUT_GAIN 0x1c000000

/* Filter mapping, once per block: words[8] 0..255 -> 20 Hz .. 16 kHz in equal octaves, words[9] 0..255 -> Q
 * 0.7 .. 15.5. Equal octaves is the sweep a measurement wants; a linear-in-hertz sweep spends most of the
 * dial in the top octave. */
#define FC_OCT_Q16   2477   /* 9.64 octaves over 255 steps, in Q16 */
#define Q_BASE_Q16   45875  /* 0.7 */
#define Q_STEP_Q16   3804   /* 14.8 / 255 */

/* Mode boundaries in the P1 byte. Written as constants rather than as a division so the zone edges are
 * visible in the source and in the README. */
#define MODE1_FROM 86
#define MODE2_FROM 171

static const ModuleCallbackInterface *s_cb;

/* (The noise seed is built from note, velocity and the voice's own track -- see onNoteOn. A counter shared by
 * the whole module would be per-run state: the render would then depend on how many notes came before it in
 * that process, which the golden tests noticed immediately (identical spectrum, different hash).) */

/* One voice's state, and all of it: 24 bytes of the 280 the ABI allows. */
typedef struct {
    uint32_t phase;   /* Q32 phase, wraps */
    uint32_t seed;    /* opz_noise's LCG state, per voice so two voices do not correlate */
    OpzSvf   svf;     /* the engine's own filter state (flags 0x04) */
    fract32  amp;     /* velocity, latched at note-on */
    uint32_t track;   /* the track this voice belongs to: part of the noise seed */
    OpzMix   mix;     /* level and pan, ramped per block by opz_mix_mono */
} State;

_Static_assert(sizeof(State) <= OPZ_VOICE_STATE_BYTES, "witness state fits the device's voice record");

/* A crossfade as two weighted multiplies. opz_lerp(a, b, w) computes a + w*(b - a), and that difference
 * saturates when a and b are far apart -- at full depth a saw of -0.75 against noise of +0.75 lands at +0.25
 * instead of +0.75, so the endpoint of the blend is not the signal it claims to be. Two Q15 weights cost one
 * extra multiply and are exact. */
static fract32 blend_q15(fract32 a, fract32 b, int32_t wa, int32_t wb)
{
    return opz_add_q31(opz_mul_q31_q15(a, wa), opz_mul_q31_q15(b, wb));
}

/* ---- entry points ---------------------------------------------------------------------------------------------- */

uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = 0;
    return 1;
}

void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)arg2; (void)mem; (void)scratch;
    volatile uint8_t *p = (volatile uint8_t *)state;
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    ((State *)state)->track = track;   /* remembered for the seed: two mono tracks on one note must differ */
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)params;
    State *s = state;
    const fract32 vel = (fract32)((uint32_t)(velocity > 127 ? 127 : velocity) * 0x01020408u); /* v/127 in Q31 */
    s->amp = opz_mul_q31(vel, OUT_GAIN);
    s->phase = 0;
    /* A per-note seed: two voices of noise that shared a seed would sum coherently, and a diagnostic that
     * cannot tell correlation from addition is not one. */
    /* Per voice, deterministically: the host passes no voice index, so the track is what separates two voices
     * playing the same note. Without it their noise streams are identical and sum coherently. */
    s->seed = 0x9e3779b9u ^ ((uint32_t)note << 16) ^ ((uint32_t)velocity << 8) ^ (s->track * 2654435761u);
    s->svf.ic1 = 0;
    s->svf.ic2 = 0;
    /* A fresh voice's mix gains are zero, and a reused one still holds the previous note's: opz_mix_mono would
     * ramp from either, so the first block of every note would swell from silence or slide from the last note's
     * pan. Start them at what the dials say. */
    opz_mix_start(&s->mix, params);
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;   /* the host envelope owns the release; nothing to latch */
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    State *s = state;

    /* Stage 0 is an idle voice: the host has freed it, and touching the state would resurrect it. */
    if (OPZ_ADSR_STAGE(env) == 0) return;

    s_cb->updateParametersADSR(env, params[OPZ_P_ATTACK], params[OPZ_P_DECAY], params[OPZ_P_SUSTAIN],
                               params[OPZ_P_RELEASE]);

    /* Everything that depends only on the knobs and the pitch, once per block. */
    const int p1b = (int)(params[OPZ_P_P1] >> 23);                 /* the byte, from the expanded word */
    const int mode = p1b >= MODE2_FROM ? 2 : p1b >= MODE1_FROM ? 1 : 0;
    const fract32 w = opz_pq31(params[OPZ_P_P2]);                  /* depth within the mode, 0..1 */

    const int32_t oct = (int32_t)((uint32_t)params[OPZ_P_FILTER] >> 23) * FC_OCT_Q16;
    const int32_t fc_q16 = 20 * opz_exp2_q16(oct);                 /* 20 Hz x 2^oct, in Q16 Hz */
    const int32_t q_q16 = Q_BASE_Q16 + (int32_t)((uint32_t)params[OPZ_P_RESONANCE] >> 23) * Q_STEP_Q16;
    /* 1/Q in Q30, computed in 32 bits: k_q15 = 2^31 / q_q16 then shifted. Writing the same thing as
     * (1 << 46) / q_q16 divides a 64-bit value and pulls ___divdi3 into the image -- 1,206 bytes, a fifth of the
     * slot window, for one division per block. */
    const uint32_t k_q15 = 0x80000000u / (uint32_t)q_q16;
    const int32_t k_q30 = (int32_t)(k_q15 << 15);
    const OpzSvfCoef coef = opz_svf_coef(fc_q16, k_q30);

    /*    The host's pitch is an unsigned Q32 increment. Casting it to int32_t first makes every value above
     *    2^31 negative, which the clamp below would then send to INC_MIN -- a note near the ceiling would
     *    arrive as a sub-audio drone instead of at 11 kHz. Clamp the unsigned value.
     *
     *    The glide then advances *that* raw pitch and re-derives the increment from it, rather than advancing
     *    the clamped increment. Advancing the clamped one loses information above the ceiling: a downward glide
     *    started from an over-ceiling pitch would begin at INC_MAX and fall immediately, and a glide that
     *    wandered above the ceiling would come back at the wrong value on the next block. The raw pitch is a
     *    wrap-around accumulator by definition, so adding the slope to it unsigned is exact. */
    uint32_t raw_pitch = pitch;
    uint32_t inc = raw_pitch > INC_MAX ? INC_MAX : (raw_pitch < INC_MIN ? (uint32_t)INC_MIN : raw_pitch);
    uint32_t recip = opz_blep_recip(inc);
    /*    And the reciprocal must follow the increment: opz_saw_blep's residual is computed against 1/dt of the
     *    increment passed with it, so a glide that leaves the reciprocal at the block's first value applies a
     *    correction of the wrong width for every sample after it. Recomputing it costs a normalise and a
     *    reciprocal, so it happens only while the pitch is actually moving. */
    const int gliding = pitchSlope != 0;
    /* Mode 1 blends the band-limited saw against the naive one, which is the only honest way to take a
     * correction away: scaling `recip` is not "less correction" -- at zero the residual becomes a full-scale
     * step once per cycle, which is worse than either end and not monotone in any metric (measured). The dial
     * therefore runs from opz_saw_blep to the raw phase ramp, and the metric it exposes is the inharmonic
     * energy, i.e. exactly what the kernel exists to suppress. Measure it at C6, where a naive saw folds the
     * most. */
    const int32_t w15 = (int32_t)(w >> 16), iw15 = 32767 - w15;   /* depth, and its complement, in Q15 */

    int32_t level = OPZ_ADSR_LEVEL(env), env_coef = OPZ_ADSR_COEF(env), step = OPZ_ADSR_STEP(env);
    const fract32 amp = s->amp;   /* velocity and the peak law; the envelope is applied per sample below */

    fract32 mono[N];
    uint32_t ph = s->phase, seed = s->seed;
    OpzSvf svf = s->svf;
    fract32 lp, bp;

    /* One loop per mode: the branch is taken once per block, not once per sample. A diagnostic that spent a
     * third of its budget on a branch inside the loop would be measuring itself. */
    if (mode == 0) {
        for (int i = 0; i < N; i++) {
            const fract32 clean = opz_sine(ph);
            const fract32 x = blend_q15(clean, opz_soft(clean), iw15, w15);   /* clean (P2 0) -> drive (255) */
            opz_svf(&svf, &coef, x, &lp, 0, 0);          /* only the lowpass: the other outputs are dead code */
            mono[i] = opz_mul_q31_q15(lp, (int32_t)(opz_mul_q31_q15(amp, level >> 16) >> 16));
            level = opz_env_tick(level, env_coef, step);
            ph += inc;
            if (gliding) {
                raw_pitch += (uint32_t)pitchSlope;      /* the pitch is a wrap-around accumulator */
                inc = raw_pitch > INC_MAX ? INC_MAX : (raw_pitch < INC_MIN ? (uint32_t)INC_MIN : raw_pitch);
                recip = opz_blep_recip(inc);            /* the next sample's residual needs its own 1/dt */
            }
        }
    } else if (mode == 1) {
        for (int i = 0; i < N; i++) {
            const fract32 naive = (fract32)(ph ^ 0x80000000u);           /* the ramp without its residual */
            const fract32 x = blend_q15(opz_saw_blep(ph, inc, recip), naive, iw15, w15);
            opz_svf(&svf, &coef, x, &lp, 0, 0);          /* only the lowpass: the other outputs are dead code */
            mono[i] = opz_mul_q31_q15(lp, (int32_t)(opz_mul_q31_q15(amp, level >> 16) >> 16));
            level = opz_env_tick(level, env_coef, step);
            ph += inc;
            if (gliding) {
                raw_pitch += (uint32_t)pitchSlope;      /* the pitch is a wrap-around accumulator */
                inc = raw_pitch > INC_MAX ? INC_MAX : (raw_pitch < INC_MIN ? (uint32_t)INC_MIN : raw_pitch);
                recip = opz_blep_recip(inc);            /* the next sample's residual needs its own 1/dt */
            }
        }
    } else {
        for (int i = 0; i < N; i++) {
            const fract32 n = opz_noise(&seed);
            const fract32 sw = opz_saw_blep(ph, inc, recip);
            const fract32 x = blend_q15(sw, n, iw15, w15);   /* flat excitation for the resonant peak */
            opz_svf(&svf, &coef, x, 0, &bp, 0);          /* only the bandpass, for the resonant peak */
            mono[i] = opz_mul_q31_q15(bp, (int32_t)(opz_mul_q31_q15(amp, level >> 16) >> 16));       /* the peak, not the passband: measurable anywhere */
            level = opz_env_tick(level, env_coef, step);
            ph += inc;
            if (gliding) {
                raw_pitch += (uint32_t)pitchSlope;      /* the pitch is a wrap-around accumulator */
                inc = raw_pitch > INC_MAX ? INC_MAX : (raw_pitch < INC_MIN ? (uint32_t)INC_MIN : raw_pitch);
                recip = opz_blep_recip(inc);            /* the next sample's residual needs its own 1/dt */
            }
        }
    }

    /* The track's level and pan, added into both channels of the interleaved buffer, saturating. */
    opz_mix_mono(out, mono, &s->mix, params);

    OPZ_ADSR_LEVEL(env) = level;
    s->phase = ph;
    s->seed = seed;
    s->svf = svf;
}

OPZ_EXPORTS_INSTRUMENT();
