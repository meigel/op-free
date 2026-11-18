/*
 * crease_sig.c — one FNV-1a digest per (fold, bias, glide) of the crease engine, rendered by the engine itself.
 *
 * witness has had this gate since it was written; crease, which came first, never did. That is the gap that
 * mattered: crease runs three BLEP saws, two triangles, a sine shaper, two one-poles, the DC blocker, the ADSR
 * and opz_mix_mono — and the golden renders in sim/tests only prove what the *host* does. The device is a
 * Blackfin with saturating fixed-point, its own rounding, and hand-scheduled assembly loops in opz_mix.h.
 *
 * The same source is built twice:
 *
 *   host     cc -std=c11 -O2 -I sdk/include            (the arithmetic opzsim's engines get)
 *   device   bfin-elf-gcc, the device flags, run in bfin-elf-run
 *
 * and the two runs must print identical lines. A difference is not a defect in the voice: it localises a kernel
 * whose host build and device build disagree.
 *
 * Every case prints three things, because equality alone is not evidence (witness learned that the hard way —
 * a harness that held its envelope with step = 0 hashed a second of silence for weeks):
 *
 *   <digest>   FNV-1a over both channels, every frame, every block
 *   nz         frames that carried any signal, out of frames rendered
 *   step       largest sample-to-sample jump in 1/1024 of full scale
 *
 * Each (fold, bias) point runs a held note and a glide, because portamento is the ABI word that reaches the
 * engine every block as a slope, and a gate that only ever passes 0 never runs that path.
 *
 * MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"

/* The engine under test, compiled in: both builds run the same source. */
#include "../../engines/crease/crease.c"

#if defined(__bfin__)
static int sig_syscall(int nr, uint32_t *args)
{
    register int r0 __asm__("R0") = (int)args;
    register int p0 __asm__("P0") = nr;
    /* The simulator's handler writes more than R0 (R2 at least: tools/cycles/driver.c). Undeclared clobbers let
     * the compiler keep a live value in a register the call destroys, which would corrupt the digest. */
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

/* A flat envelope at full level: coef = 0 with step = 1.0 holds the level where it is. coef = 0 with step = 0
 * returns zero, collapses the amplitude after the first frame of each block, and hashes silence. */
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

static uint32_t word(int b) { return (uint32_t)b * 0x808080u + ((uint32_t)b >> 1); }

#define BLOCKS 48
/* C3 (MIDI 48) at 44.1 kHz in Q32, written out: opz_note_hz_q16(48) shifts a negative value. */
#define PITCH_C3 0x00c265dbu
#define SLOPE 128

static uint32_t s_hash;
static void mix32(uint32_t v)
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

static uint32_t hexline(char *buf, const char *name)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t n = 0;
    while (*name && n < 40) buf[n++] = *name++;
    buf[n++] = ' ';
    for (int k = 28; k >= 0; k -= 4) buf[n++] = hex[(s_hash >> k) & 15];
    return n;
}

int main(void)
{
    static uint64_t state_mem[(sizeof(State) + 7) / 8];
    static uint64_t env_mem[8];
    static fract32 out[2 * OPZ_BLOCK_FRAMES];
    uint32_t params[OPZ_NUM_PARAMS];

    uint32_t mem_req = 0;
    initModule(1, &s_host_cb, &mem_req);

    /* A 5 x 5 grid over the two dials, plus a glide at every point. Five points is enough to cover both ends of
     * each dial and the detent in the middle of bias; the case count is derived and printed, so the Python test
     * never hard-codes it. */
    static const int fold[] = { 0, 64, 128, 192, 255 };
    static const int bias[] = { 0, 64, 128, 192, 255 };
    static const int glide[] = { 0, 1 };
    const int nfold = (int)(sizeof fold / sizeof fold[0]);
    const int nbias = (int)(sizeof bias / sizeof bias[0]);
    const int nglide = (int)(sizeof glide / sizeof glide[0]);
    const uint32_t frames_per_case = (uint32_t)BLOCKS * OPZ_BLOCK_FRAMES;
    int cases = 0;

    for (int f = 0; f < nfold; f++) {
        for (int b = 0; b < nbias; b++) {
            for (int g = 0; g < nglide; g++) {
                for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = word(128);
                params[OPZ_P_P1] = word(fold[f]);
                params[OPZ_P_P2] = word(bias[b]);
                params[OPZ_P_PAN] = word(128);
                params[OPZ_P_LEVEL] = word(255);
                params[OPZ_P_ATTACK] = word(0);
                params[OPZ_P_DECAY] = word(0);
                params[OPZ_P_SUSTAIN] = word(255);      /* sustain high: the engine's env_fold path is off */
                params[OPZ_P_RELEASE] = word(20);

                State *s = (State *)state_mem;
                ADSR *env = (ADSR *)env_mem;
                init(s, 0, 0, 0, 0);
                OPZ_ADSR_LEVEL(env) = OPZ_Q31_ONE;
                OPZ_ADSR_STAGE(env) = 1;
                OPZ_ADSR_COEF(env) = 0;
                OPZ_ADSR_STEP(env) = OPZ_Q31_ONE;
                onNoteOn(48, 100, s, params);

                const int32_t slope = (int32_t)glide[g] * SLOPE;
                const uint32_t pitch = PITCH_C3;

                uint32_t nz = 0;
                int32_t prev = 0;
                uint32_t step = 0;

                s_hash = 2166136261u;
                for (int blk = 0; blk < BLOCKS; blk++) {
                    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) out[i] = 0;
                    render(out, pitch, slope, s, params, env);
                    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
                        const int32_t l = out[2 * i];
                        mix32((uint32_t)l);
                        mix32((uint32_t)out[2 * i + 1]);
                        if (l != 0 || out[2 * i + 1] != 0) nz++;
                        int64_t diff = (int64_t)l - (int64_t)prev;
                        if (diff < 0) diff = -diff;
                        if ((uint64_t)(diff >> 21) > step) step = (uint32_t)(diff >> 21);
                        prev = l;
                    }
                }

                char name[48], buf[96];
                uint32_t n = 0;
                const char *p = "crease f";
                while (*p) name[n++] = *p++;
                digits(&name[n], fold[f], 3); n += 3;
                name[n++] = ' ';
                name[n++] = 'b';
                digits(&name[n], bias[b], 3); n += 3;
                name[n++] = ' ';
                name[n++] = 'g';
                name[n++] = glide[g] ? '+' : '0';
                name[n] = 0;

                uint32_t k = hexline(buf, name);
                const char *nzs = " nz ";
                while (*nzs) buf[k++] = *nzs++;
                digits(&buf[k], (int)nz, 4); k += 4;
                buf[k++] = '/';
                digits(&buf[k], (int)frames_per_case, 4); k += 4;
                const char *sts = " step ";
                while (*sts) buf[k++] = *sts++;
                digits(&buf[k], (int)step, 5); k += 5;
                buf[k++] = '\n';
                put(buf, k);
                cases++;
            }
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

    {
        const char *d = "done\n";
        uint32_t n = 0, k = 0;
        while (d[k]) n++, k++;
        put(d, n);
    }
    bench_exit(0);
    return 0;
}
