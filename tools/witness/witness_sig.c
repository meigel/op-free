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
 * What it deliberately does NOT cover: the host's ADSR model (the callback here holds the envelope flat, so
 * the engine's own amplitude law is what is under test) and voice allocation. Those are the host's, and
 * sim/tests already covers them.
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
    __asm__ volatile("EXCPT 0;" : "+r"(r0) : "r"(p0) : "memory");
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
static void bench_exit(int code)   /* crt0.S calls this */ { (void)code; }
#endif

/* A flat sustain: attack/decay/sustain/release are the host's business, and a moving envelope would put the
 * host's model into a comparison that is about the engine's arithmetic. */
static void hold_adsr(ADSR *env, uint32_t a, uint32_t d, uint32_t s, uint32_t r)
{
    (void)a; (void)d; (void)s; (void)r;
    OPZ_ADSR_LEVEL(env) = 0x7fffffff;
    OPZ_ADSR_STAGE(env) = 1;
    OPZ_ADSR_COEF(env) = 0;
    OPZ_ADSR_STEP(env) = 0;
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
#define NOTES 3   /* three notes per case: a held one, so pitch and the filter settle */

static uint32_t s_hash;
static void mix(uint32_t v)
{
    for (int k = 0; k < 4; k++) {
        s_hash ^= (v >> (8 * k)) & 0xffu;
        s_hash *= 16777619u;
    }
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

    const uint32_t inc = opz_hz_to_inc(opz_note_hz_q16(48));   /* C3 */

    static const int p1[3] = { 0, 128, 240 };                 /* the three mode zones */
    static const int filter[3] = { 255, 200, 96 };             /* open, mid, and low enough for a peak */
    static const int reso[3] = { 0, 64, 200 };

    for (int m = 0; m < 3; m++) {
        for (int depth = 0; depth <= 255; depth += 85) {
            for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = word(128);
            params[OPZ_P_P1] = word(p1[m]);
            params[OPZ_P_P2] = word(depth);
            params[OPZ_P_FILTER] = word(filter[m]);
            params[OPZ_P_RESONANCE] = word(reso[m]);
            params[OPZ_P_PAN] = word(128);
            params[OPZ_P_LEVEL] = word(255);

            State *s = (State *)state_mem;
            ADSR *env = (ADSR *)env_mem;
            init(s, 0, 0, 0, 0);
            OPZ_ADSR_LEVEL(env) = 0x7fffffff;
            OPZ_ADSR_STAGE(env) = 1;
            OPZ_ADSR_COEF(env) = 0;
            OPZ_ADSR_STEP(env) = 0;
            onNoteOn(48, 100, s, params);

            s_hash = 2166136261u;
            for (int b = 0; b < BLOCKS * NOTES; b++) {
                for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) out[i] = 0;
                render(out, inc, 0, s, params, env);
                for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) mix((uint32_t)out[2 * i]);   /* left channel */
            }

            char name[64];
            uint32_t n = 0;
            const char *p = "mode ";
            while (*p) name[n++] = *p++;
            name[n++] = (char)('0' + m);
            name[n++] = ' ';
            name[n++] = 'd';
            name[n++] = (char)('0' + depth / 100);
            name[n++] = (char)('0' + (depth / 10) % 10);
            name[n++] = (char)('0' + depth % 10);
            name[n] = 0;
            line(name);
        }
    }

    line("done");
    bench_exit(0);
    return 0;
}
