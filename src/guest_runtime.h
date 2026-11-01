#ifndef GEX_GUEST_RUNTIME_H
#define GEX_GUEST_RUNTIME_H

#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include "recomp_funcs.h"
#include "graphics_guest.h"

recomp_func_t gex_lookup_import(uint32_t va);
void gex_guest_wait(void);
#define recomp_lookup_import(va) gex_lookup_import(va)

#endif
