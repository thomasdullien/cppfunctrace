/*
 * symresolve — offline resolver for "@<module>+0x<offset>" names.
 *
 * Each module is mmap'd exactly once and its .symtab (preferred) or
 * .dynsym is indexed in a sorted array so lookups are O(log n). C++
 * symbols are demangled via __cxa_demangle if available at runtime.
 */

#ifndef CPPFT_SYMRESOLVE_H
#define CPPFT_SYMRESOLVE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sym_resolver;
typedef struct sym_resolver sym_resolver;

sym_resolver* sym_resolver_new(void);
void          sym_resolver_free(sym_resolver*);

/* Try to resolve "@<module_path>+0x<offset>" into a pretty name.
 * Writes "<symbol> (<module-basename>)" into `out` on success.
 * Returns 1 on hit, 0 if the input isn't a raw name or cannot be
 * resolved (caller should then use the original name). */
int sym_resolver_lookup(sym_resolver* r,
                         const char* raw, size_t raw_len,
                         char* out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif
