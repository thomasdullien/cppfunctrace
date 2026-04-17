/*
 * symresolve — resolve "@<module>+0x<offset>" raw names to symbol names
 * by parsing the referenced ELF file's .symtab/.dynsym.
 *
 * Used by ftrc2perfetto to prettify the raw addresses recorded by the
 * tracer. This deliberately runs offline so tracing itself can stay
 * fast: the library never reads binaries at recording time.
 */

#define _GNU_SOURCE
#include "symresolve.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__LP64__) || defined(_LP64)
# define Elf_Ehdr Elf64_Ehdr
# define Elf_Shdr Elf64_Shdr
# define Elf_Sym  Elf64_Sym
# define ELF_ST_TYPE(x) ELF64_ST_TYPE(x)
#else
# define Elf_Ehdr Elf32_Ehdr
# define Elf_Shdr Elf32_Shdr
# define Elf_Sym  Elf32_Sym
# define ELF_ST_TYPE(x) ELF32_ST_TYPE(x)
#endif

typedef char* (*demangle_fn)(const char*, char*, size_t*, int*);

typedef struct {
    uint64_t    addr;
    uint64_t    size;
    const char* name;   /* interior pointer into mmap'd .strtab */
} SymEntry;

typedef struct Module {
    char*       path;
    SymEntry*   syms;
    size_t      num_syms;
    char*       map;
    size_t      map_size;
    int         missing;    /* 1 if file couldn't be opened/parsed */
    struct Module* next;
} Module;

struct sym_resolver {
    Module*     modules;
    demangle_fn demangle;
};

static int
cmp_sym(const void* a, const void* b)
{
    const SymEntry* sa = (const SymEntry*)a;
    const SymEntry* sb = (const SymEntry*)b;
    if (sa->addr < sb->addr) return -1;
    if (sa->addr > sb->addr) return  1;
    if (sa->size < sb->size) return -1;
    if (sa->size > sb->size) return  1;
    return 0;
}

static Module*
load_module(const char* path)
{
    Module* m = (Module*)calloc(1, sizeof(Module));
    if (!m) return NULL;
    m->path = strdup(path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) { m->missing = 1; return m; }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(Elf_Ehdr)) {
        close(fd); m->missing = 1; return m;
    }
    size_t map_size = (size_t)st.st_size;
    char* map = (char*)mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { m->missing = 1; return m; }

    Elf_Ehdr* eh = (Elf_Ehdr*)map;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_shoff == 0 ||
        eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf_Shdr) > map_size) {
        munmap(map, map_size); m->missing = 1; return m;
    }

    Elf_Shdr* shs = (Elf_Shdr*)(map + eh->e_shoff);
    const char* shstr = map + shs[eh->e_shstrndx].sh_offset;

    Elf_Shdr *symtab = NULL, *strtab_sh = NULL;
    Elf_Shdr *dynsym = NULL, *dynstr_sh = NULL;
    for (int i = 0; i < eh->e_shnum; i++) {
        const char* n = shstr + shs[i].sh_name;
        if (shs[i].sh_type == SHT_SYMTAB && strcmp(n, ".symtab") == 0) {
            symtab = &shs[i];
            if (symtab->sh_link < eh->e_shnum) strtab_sh = &shs[symtab->sh_link];
        } else if (shs[i].sh_type == SHT_DYNSYM && strcmp(n, ".dynsym") == 0) {
            dynsym = &shs[i];
            if (dynsym->sh_link < eh->e_shnum) dynstr_sh = &shs[dynsym->sh_link];
        }
    }
    Elf_Shdr* sh = symtab ? symtab : dynsym;
    Elf_Shdr* str_sh = symtab ? strtab_sh : dynstr_sh;
    if (!sh || !str_sh || sh->sh_entsize < sizeof(Elf_Sym)) {
        munmap(map, map_size); m->missing = 1; return m;
    }

    size_t n = sh->sh_size / sh->sh_entsize;
    const char* strs = map + str_sh->sh_offset;
    Elf_Sym* base = (Elf_Sym*)(map + sh->sh_offset);

    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (ELF_ST_TYPE(base[i].st_info) == STT_FUNC && base[i].st_value)
            count++;
    }
    if (count == 0) { munmap(map, map_size); m->missing = 1; return m; }

    m->map = map;
    m->map_size = map_size;
    m->syms = (SymEntry*)malloc(count * sizeof(SymEntry));
    if (!m->syms) { munmap(map, map_size); m->missing = 1; return m; }
    m->num_syms = 0;
    for (size_t i = 0; i < n; i++) {
        if (ELF_ST_TYPE(base[i].st_info) != STT_FUNC) continue;
        if (base[i].st_value == 0) continue;
        m->syms[m->num_syms].addr = base[i].st_value;
        m->syms[m->num_syms].size = base[i].st_size;
        m->syms[m->num_syms].name = strs + base[i].st_name;
        m->num_syms++;
    }
    qsort(m->syms, m->num_syms, sizeof(SymEntry), cmp_sym);

    /* For ET_EXEC binaries symbol values are absolute; for ET_DYN they
     * are file-offsets from the module base. We stored them verbatim —
     * the caller passes the same kind of offset. The tracer records
     * `addr - dlpi_addr`, so:
     *   - ET_DYN (PIE, shared libs): dlpi_addr = load base → st_value is
     *     already the module-relative offset we want. ✓
     *   - ET_EXEC:                   dlpi_addr = 0 on most distros →
     *     st_value is also absolute, so `addr - 0 = addr` matches. ✓
     * Both cases work without additional conversion.
     */
    (void)eh->e_type;
    return m;
}

sym_resolver*
sym_resolver_new(void)
{
    sym_resolver* r = (sym_resolver*)calloc(1, sizeof(sym_resolver));
    if (!r) return NULL;
    r->demangle = (demangle_fn)dlsym(RTLD_DEFAULT, "__cxa_demangle");
    return r;
}

void
sym_resolver_free(sym_resolver* r)
{
    if (!r) return;
    Module* m = r->modules;
    while (m) {
        Module* n = m->next;
        free(m->path);
        free(m->syms);
        if (m->map) munmap(m->map, m->map_size);
        free(m);
        m = n;
    }
    free(r);
}

static Module*
get_module(sym_resolver* r, const char* path, size_t path_len)
{
    char tmp[1024];
    if (path_len >= sizeof(tmp)) path_len = sizeof(tmp) - 1;
    memcpy(tmp, path, path_len);
    tmp[path_len] = 0;

    for (Module* m = r->modules; m; m = m->next) {
        if (strcmp(m->path, tmp) == 0) return m;
    }
    Module* m = load_module(tmp);
    if (m) { m->next = r->modules; r->modules = m; }
    return m;
}

static const SymEntry*
find_sym(const Module* m, uint64_t off)
{
    if (!m->num_syms) return NULL;
    size_t lo = 0, hi = m->num_syms;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (m->syms[mid].addr <= off) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return NULL;
    const SymEntry* s = &m->syms[lo - 1];
    uint64_t gap = off - s->addr;
    uint64_t cutoff = s->size ? s->size : (uint64_t)(64 * 1024);
    if (gap >= cutoff) return NULL;
    return s;
}

int
sym_resolver_lookup(sym_resolver* r,
                     const char* raw, size_t raw_len,
                     char* out, size_t out_len)
{
    if (!r || raw_len < 2 || raw[0] != '@') return 0;

    /* Split at "+0x" */
    const char* plus = memchr(raw, '+', raw_len);
    if (!plus || plus + 3 > raw + raw_len ||
        plus[1] != '0' || plus[2] != 'x') return 0;

    const char* path = raw + 1;
    size_t      plen = (size_t)(plus - path);

    uint64_t off = 0;
    const char* p = plus + 3;
    const char* e = raw + raw_len;
    for (; p < e; p++) {
        char c = *p;
        uint64_t d;
        if (c >= '0' && c <= '9') d = (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint64_t)(c - 'A' + 10);
        else break;
        off = (off << 4) | d;
    }

    Module* m = get_module(r, path, plen);
    if (!m || m->missing) return 0;

    const SymEntry* s = find_sym(m, off);
    if (!s || !s->name || !s->name[0]) return 0;

    const char* sym = s->name;
    char* demangled = NULL;
    if (r->demangle) {
        int status = 0;
        demangled = r->demangle(sym, NULL, NULL, &status);
        if (status == 0 && demangled) sym = demangled;
    }

    /* Trim path to basename for display. */
    const char* modbase = m->path;
    const char* slash = strrchr(modbase, '/');
    if (slash) modbase = slash + 1;

    snprintf(out, out_len, "%s (%s)", sym, modbase);
    free(demangled);
    return 1;
}
