/*
 * engine.c — load an engine built as a host shared object (macOS dylib or ELF .so), resolve the five exports,
 * estimate its slot usage. In the wasm build (sim/wasm) the engine is linked into the same module instead; see the
 * __wasm__ branch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#ifndef __wasm__
#include <dlfcn.h>
#endif

int engine_slot_bytes(int slot)
{
    if (slot >= 0 && slot <= 7) return 0xC800;
    if (slot >= 8 && slot <= 57) return 0x2800;
    if (slot >= 58 && slot <= 89) return 0x1400;
    return -1;
}

static void base_name(const char *path, char *out, size_t n)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    snprintf(out, n, "%s", b);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
    if (!strncmp(out, "lib", 3)) memmove(out, out + 3, strlen(out + 3) + 1);
}

#ifdef __wasm__
/* sim/wasm: one module per engine, host and engine linked together. The exports are weak so that a missing one
 * reads as NULL and is refused below, as dlsym's NULL is. */
#pragma weak initModule
#pragma weak init
#pragma weak onNoteOn
#pragma weak onNoteOff
#pragma weak render
#pragma weak renderFX

static int resolve(Engine *e, const char *path)
{
    snprintf(e->path, sizeof e->path, "%s", path);
    base_name(path, e->name, sizeof e->name);
    e->initModule = initModule;
    e->init = init;
    e->onNoteOn = onNoteOn;
    e->onNoteOff = onNoteOff;
    e->render = render;
    e->renderFX = renderFX;
    return 0;
}
#else
static int resolve(Engine *e, const char *path)
{
    snprintf(e->path, sizeof e->path, "%s", path);
    base_name(path, e->name, sizeof e->name);
    e->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!e->handle) {
        fprintf(stderr, "error   %s\n", dlerror());
        return -1;
    }
    *(void **)&e->initModule = dlsym(e->handle, "initModule");
    *(void **)&e->init = dlsym(e->handle, "init");
    *(void **)&e->onNoteOn = dlsym(e->handle, "onNoteOn");
    *(void **)&e->onNoteOff = dlsym(e->handle, "onNoteOff");
    *(void **)&e->render = dlsym(e->handle, "render");
    *(void **)&e->renderFX = dlsym(e->handle, "renderFX");
    return 0;
}
#endif

int engine_load(Engine *e, const char *path, const ModuleCallbackInterface *cb)
{
    if (resolve(e, path)) return -1;

    int bad = 0;
    const char *names[] = { "initModule", "init", "onNoteOn", "onNoteOff" };
    void *fns[] = { (void *)e->initModule, (void *)e->init, (void *)e->onNoteOn, (void *)e->onNoteOff };
    for (int i = 0; i < 4; i++)
        if (!fns[i]) {
            fprintf(stderr, "error   %s: missing export %s (Could not lookup)\n", e->name, names[i]);
            bad = 1;
        }
    if (!!e->render == !!e->renderFX) {
        fprintf(stderr, "error   %s: export exactly one of render (instrument) or renderFX (effect)\n", e->name);
        bad = 1;
    }
    if (bad) return -1;
    e->is_fx = e->renderFX != NULL;

    e->mem_request = 0;
    if (!e->initModule(0, cb, &e->mem_request)) {
        fprintf(stderr, "error   %s: Failed to initialize module (initModule returned 0)\n", e->name);
        return -1;
    }
    e->size_ok = engine_inspect_size(path, &e->size) == 0;
    return 0;
}

#ifdef __wasm__
int engine_inspect_size(const char *path, SlotSize *s)
{
    (void)path;
    memset(s, 0, sizeof *s);
    s->not_measured = 1;
    return 0;
}
#elif !defined(__APPLE__)
/* ---- ELF size estimate (Linux and other non-Darwin hosts) ------------------------------------------
 * The same reading of the linked image that the Mach-O path takes: the sections an engine actually occupies in a
 * slot, and the names it imports (there is no libc on the device). Host linker overhead (.got, .dynamic, .plt, unwind
 * tables) is left out, as the Mach-O path leaves out stubs and GOT. */
#include <elf.h>

/* A range inside the mapped file, without wrapping: off <= n and len <= n - off. */
static int elf_range(uint64_t off, uint64_t len, size_t n)
{
    return off <= (uint64_t)n && len <= (uint64_t)n - off;
}

static void add_section_elf(SlotSize *s, const char *name, uint32_t type, uint64_t size)
{
    if (!strcmp(name, ".init_array")) s->has_constructors = 1;
    else if (type == SHT_NOBITS) s->bss += size;                        /* .bss, .tbss */
    else if (!strcmp(name, ".text")) s->code += size;
    else if (!strncmp(name, ".rodata", 7)) s->rodata += size;
    else if (!strncmp(name, ".data", 5)) s->data += size;               /* .data, .data.rel.ro */
    else if (!strcmp(name, ".init_array")) s->has_constructors = 1;
    /* .got, .dynamic, .plt, .eh_frame, .comment, .symtab: host linker overhead, not counted */
}

int engine_inspect_size(const char *path, SlotSize *s)
{
    memset(s, 0, sizeof *s);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < (long)sizeof(Elf64_Ehdr)) { fclose(f); return -1; }
    uint8_t *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return -1;
    }
    fclose(f);

    const Elf64_Ehdr *eh = (const void *)p;
    int rc = -1;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB || eh->e_shentsize != sizeof(Elf64_Shdr) || !eh->e_shnum ||
        eh->e_shstrndx >= eh->e_shnum ||
        !elf_range(eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr), (size_t)n))
        goto out;

    const Elf64_Shdr *sh = (const void *)(p + eh->e_shoff);
    const Elf64_Shdr *strsh = &sh[eh->e_shstrndx];
    if (!elf_range(strsh->sh_offset, strsh->sh_size, (size_t)n)) goto out;
    const char *shstr = (const char *)(p + strsh->sh_offset);
    const Elf64_Shdr *dynsym = NULL;

    for (uint32_t i = 0; i < eh->e_shnum; i++) {
        const char *name = "";
        if (sh[i].sh_name < strsh->sh_size) {
            const char *c = shstr + sh[i].sh_name;
            /* a name that does not terminate inside its table is not a name */
            if (memchr(c, 0, (size_t)(strsh->sh_size - sh[i].sh_name))) name = c;
        }
        add_section_elf(s, name, sh[i].sh_type, sh[i].sh_size);
        if (sh[i].sh_type == SHT_DYNSYM && sh[i].sh_link < eh->e_shnum) dynsym = &sh[i];
    }

    const Elf64_Shdr *strtab = dynsym ? &sh[dynsym->sh_link] : NULL;
    if (dynsym && dynsym->sh_entsize == sizeof(Elf64_Sym) &&
        elf_range(dynsym->sh_offset, dynsym->sh_size, (size_t)n) &&
        elf_range(strtab->sh_offset, strtab->sh_size, (size_t)n)) {
        const Elf64_Sym *sym = (const void *)(p + dynsym->sh_offset);
        const char *str = (const char *)(p + strtab->sh_offset);
        uint32_t count = (uint32_t)(dynsym->sh_size / sizeof(Elf64_Sym));
        for (uint32_t k = 0; k < count; k++) {
            if (sym[k].st_shndx != SHN_UNDEF || ELF64_ST_BIND(sym[k].st_info) == STB_LOCAL) continue;
            if (sym[k].st_name >= strtab->sh_size) continue;                /* outside the string table */
            if (!memchr(str + sym[k].st_name, 0, (size_t)(strtab->sh_size - sym[k].st_name))) continue;
            const char *nm = str + sym[k].st_name;
            size_t used = strlen(s->undefined);
            snprintf(s->undefined + used, sizeof s->undefined - used, "%s%s", used ? ", " : "", nm);
        }
    }
    rc = 0;
out:
    free(p);
    return rc;
}
#else
/* ---- Mach-O size estimate ---------------------------------------------------------------------------- */

#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

static void add_section(SlotSize *s, const char *seg, const char *sect, uint64_t size)
{
    if (!strcmp(sect, "__text"))
        s->code += size;
    else if (!strcmp(sect, "__bss") || !strcmp(sect, "__common"))
        s->bss += size;
    else if (!strcmp(sect, "__data"))
        s->data += size;
    else if (!strcmp(sect, "__const") || !strcmp(sect, "__cstring") || !strncmp(sect, "__literal", 9))
        s->rodata += size;
    else if (!strcmp(sect, "__mod_init_func") || !strcmp(sect, "__init_offsets"))
        s->has_constructors = 1;
    else if (strcmp(seg, "__TEXT") && strcmp(seg, "__DATA_CONST") && strcmp(seg, "__DATA"))
        s->other += size;
    /* stubs, GOT, unwind info, eh_frame: host linker overhead, not counted */
}

static int inspect_thin(const uint8_t *p, size_t n, SlotSize *s)
{
    if (n < sizeof(struct mach_header_64)) return -1;
    const struct mach_header_64 *mh = (const void *)p;
    if (mh->magic != MH_MAGIC_64) return -1;
    size_t off = sizeof *mh;
    for (uint32_t i = 0; i < mh->ncmds && off + sizeof(struct load_command) <= n; i++) {
        const struct load_command *lc = (const void *)(p + off);
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const void *)lc;
            const struct section_64 *sec = (const void *)(sg + 1);
            for (uint32_t k = 0; k < sg->nsects; k++) {
                char seg[17] = { 0 }, name[17] = { 0 };
                memcpy(seg, sec[k].segname, 16);
                memcpy(name, sec[k].sectname, 16);
                add_section(s, seg, name, sec[k].size);
            }
        } else if (lc->cmd == LC_SYMTAB) {
            const struct symtab_command *st = (const void *)lc;
            if ((size_t)st->symoff + (size_t)st->nsyms * sizeof(struct nlist_64) > n || st->stroff >= n)
                break;
            const struct nlist_64 *sym = (const void *)(p + st->symoff);
            for (uint32_t k = 0; k < st->nsyms; k++) {
                if ((sym[k].n_type & N_STAB) || (sym[k].n_type & N_TYPE) != N_UNDF || !(sym[k].n_type & N_EXT))
                    continue;
                const char *nm = (const char *)p + st->stroff + sym[k].n_un.n_strx;
                if (!strcmp(nm, "dyld_stub_binder")) continue;
                size_t used = strlen(s->undefined);
                snprintf(s->undefined + used, sizeof s->undefined - used, "%s%s", used ? ", " : "",
                         nm[0] == '_' ? nm + 1 : nm);
            }
        }
        off += lc->cmdsize;
    }
    return 0;
}

int engine_inspect_size(const char *path, SlotSize *s)
{
    memset(s, 0, sizeof *s);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return -1;
    }
    fclose(f);
    int rc = -1;
    uint32_t magic = *(uint32_t *)p;
    if (magic == FAT_CIGAM || magic == FAT_MAGIC) {
        const struct fat_header *fh = (const void *)p;
        const struct fat_arch *fa = (const void *)(fh + 1);
        uint32_t na = OSSwapBigToHostInt32(fh->nfat_arch);
#if defined(__arm64__)
        cpu_type_t want = CPU_TYPE_ARM64;
#else
        cpu_type_t want = CPU_TYPE_X86_64;
#endif
        for (uint32_t i = 0; i < na; i++)
            if ((cpu_type_t)OSSwapBigToHostInt32(fa[i].cputype) == want) {
                uint32_t o = OSSwapBigToHostInt32(fa[i].offset), sz = OSSwapBigToHostInt32(fa[i].size);
                if ((long)o + (long)sz <= n) rc = inspect_thin(p + o, sz, s);
                break;
            }
    } else {
        rc = inspect_thin(p, (size_t)n, s);
    }
    free(p);
    return rc;
}
#endif
