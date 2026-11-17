/*
 * witness_sig.c — one FNV-1a digest per (mode, depth) of the witness engine, rendered by the engine itself.
 *
 * This is the differential gate the engine exists for. The same source is built twice:
 *
 *   host     cc -std=c11 -O2 -I sdk/include     (the arithmetic opzsim's engines get)
 *   device   bfin-elf-gcc with the device flags  (run in bfin-elf-run)
 *
 * and the two runs must print identical lines. A difference is not a defect in the voice: it localises a
 * kernel whose host build and device build disagree — which the simulator would otherwise hide, because the
 * host renders are the only renders anyone listens to here.
 *
 * What it deliberately does NOT cover: the host's ADSR model (the callback below holds the envelope flat, so
 * the engine's own amplitude law is what is under test) and voice allocation. Those are the host's, and
 * sim/tests already covers them.
 *
 * Output: one "mode <m> d<ddd> <digest>" per case, then "cases <n>" so the test never hard-codes how many
 * there are, then "done". Both channels are hashed: opz_mix_mono computes its left and right gains on separate
 * paths, so a device-only error in the right one has to show up.
 *
 * MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"

/* The engine under test, compiled in: both builds run the same source, so the comparison cannot be fooled
 * by two different objects of the same name. */
#include "../../engines/witness/witness.c"

#if defined(__bfin__)
static int sig_syscall(int nr, uint32_t *args)
{
    register int r0 __asm__("R0") = (int)args;
    register int p0 __asm__("P0") = nr;
    /* The simulator's handler writes more than R0 — R2 at least (tools/cycles/driver.c documents how that was
     * found: every word after a mid-line flush printed as zeros). Undeclared clobbers let the compiler keep a
     * live value in a register the call destroys, which would corrupt the digest it is meant to protect. */
    __asm__ volatile("EXCPT 0;" : "+r"(r0), "+r"(p0) : : "R1", "R2", "R3", "P1", "P2", "cc", "memory");
    return r0;
}
static void put(const char *s, uint32_t n)
{
    uint32_t args[3] = { 1, (uint32_t)s, n };
    sig_syscall(5 /* write */, args);
}
void bench_exit(int code)   /* crt0.S calls this */
{
    uint32_t args[1] = { (uint32_t)code };
    sig_syscall(1 /* exit */, args);
}
#else
#include <stdio.h>
static void put(const char *s, uint32_t n) { fwrite(s, 1, n, stdout); }
static void bench_exit(int code) { (void)code; }   /* crt0.S calls this */
#endif

/* A flat envelope, at full level.
 *
 * opz_env_tick(level, coef, step) returns coef*level + (1 - (coef*level)^3) * step, so coef = 0 with step = 1.0
 * holds the level where it is. This is not a detail: with coef = 0 and step = 0 the tick returns zero, the
 * amplitude collapses after the first frame of every block, and the gate hashes 63/64 of a second of silence —
 * a comparison that can never fail. */
static void hold_adsr(ADSR *env, uint32_t a, uint32_t d, uint32_t s, uint32_t r)
{
    (void)a; (void)d; (void)s; (void)r;
    OPZ_ADSR_LEVEL(env) = OPZ_Q31_ONE;
    OPZ_ADSR_STAGE(env) = 1;
    OPZ_ADSR_COEF(env) = 0;
    OPZ_ADSR_STEP(env) = OPZ_Q31_ONE;
}

static const void *no_sample(int pack, uint8_t note) { (void)pack; (void)note; return 0; }

static const ModuleCallbackInterface s_host_cb = {
    .sequencerSetValue = 0,
    .sequencerGetValue = 0,
    .updateParametersADSR = hold_adsr,
    .getSample = no_sample,
    .getPresetData = 0,
};

/* The device expands a byte to a Q31 word this way (docs/parameter-scheme.md, engine-abi.md); both builds use
 * the same expression, so the comparison covers the engine, not the conversion. */
static uint32_t word(int b) { return (uint32_t)b * 0x808080u + ((uint32_t)b >> 1); }

#define BLOCKS 48

/* C3, written out rather than computed: opz_note_hz_q16(48) shifts the negative `48 - 69` left, which is
 * undefined in C11, and an input the two compilers may evaluate differently has no business inside a bit
 * comparison. 130.812783 Hz at 44.1 kHz in Q32 = 0x00c265db. */
#define PITCH_C3 0x00c265dbu

static uint32_t s_hash;
static void mix(uint32_t v)
{
    for (int k = 0; k < 4; k++) {
        s_hash ^= (v >> (8 * k)) & 0xffu;
        s_hash *= 16777619u;
    }
}

static void digits(char *d, int v, int width)
{
    for (int i = width - 1; i >= 0; i--) { d[i] = (char)('0' + v % 10); v /= 10; }
}

static void line(const char *name)
{
    static const char hex[] = "0123456789abcdef";
    char buf[80];
    uint32_t n = 0;
    while (*name && n < 56) buf[n++] = *name++;
    buf[n++] = ' ';
    for (int k = 28; k >= 0; k -= 4) buf[n++] = hex[(s_hash >> k) & 15];
    buf[n++] = '\n';
    put(buf, n);
}

int main(void)
{
    static uint64_t state_mem[(sizeof(State) + 7) / 8];
    static uint64_t env_mem[8];                       /* ADSR is opaque here: the macros address its bytes */
    static fract32 out[2 * OPZ_BLOCK_FRAMES];
    uint32_t params[OPZ_NUM_PARAMS];

    uint32_t mem_req = 0;
    initModule(1, &s_host_cb, &mem_req);

    /* One triple per mode: the P1 that selects it and the filter settings that make its own kernel the thing
     * under test. The depth list applies to every mode, so adding a mode is one entry in each array here and
     * nothing else — the case count is derived, and printed. */
    static const int p1[] = { 0, 128, 240 };
    static const int filter[] = { 255, 200, 96 };     /* open, mid, low enough for a peak */
    static const int reso[] = { 0, 64, 200 };
    static const int depth[] = { 0, 85, 170, 255 };
    const int modes = (int)(sizeof p1 / sizeof p1[0]);
    const int depths = (int)(sizeof depth / sizeof depth[0]);
    int cases = 0;

    for (int m = 0; m < modes; m++) {
        for (int d = 0; d < depths; d++) {
            for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = word(128);
            params[OPZ_P_P1] = word(p1[m]);
            params[OPZ_P_P2] = word(depth[d]);
            params[OPZ_P_FILTER] = word(filter[m]);
            params[OPZ_P_RESONANCE] = word(reso[m]);
            params[OPZ_P_PAN] = word(128);
            params[OPZ_P_LEVEL] = word(255);

            State *s = (State *)state_mem;
            ADSR *env = (ADSR *)env_mem;
            init(s, 0, 0, 0, 0);
            OPZ_ADSR_LEVEL(env) = OPZ_Q31_ONE;
            OPZ_ADSR_STAGE(env) = 1;
            OPZ_ADSR_COEF(env) = 0;
            OPZ_ADSR_STEP(env) = OPZ_Q31_ONE;
            onNoteOn(48, 100, s, params);

            s_hash = 2166136261u;
            for (int b = 0; b < BLOCKS; b++) {
                for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) out[i] = 0;
                render(out, PITCH_C3, 0, s, params, env);
                for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
                    mix((uint32_t)out[2 * i]);          /* left */
                    mix((uint32_t)out[2 * i + 1]);      /* right: separate gain path in opz_mix_mono */
                }
            }

            char name[64];
            uint32_t n = 0;
            const char *p = "mode ";
            while (*p) name[n++] = *p++;
            digits(&name[n], m, 1); n += 1;
            name[n++] = ' ';
            name[n++] = 'd';
            digits(&name[n], depth[d], 3); n += 3;
            name[n] = 0;
            line(name);
            cases++;
        }
    }

    {
        char buf[32];
        uint32_t n = 0;
        const char *c = "cases ";
        while (*c) buf[n++] = *c++;
        digits(&buf[n], cases, 3); n += 3;
        buf[n++] = '\n';
        put(buf, n);
    }

    line("done");
    bench_exit(0);
    return 0;
}
