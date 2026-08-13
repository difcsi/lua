/*
** $Id: lallocs.h $
** Native support for liballocs-typed C objects (LUA_TFOREIGN)
** See Copyright Notice in lua.h
**
** Everything in this header (and in lallocs.c, lallocsffi.c,
** lallocsbounds.c, lallocslib.c) is compiled only under
** LUA_USE_LIBALLOCS; the default build is pristine upstream.
**
** Core touch-point checklist (keep in sync when merging upstream):
**   lua.h       - LUA_TFOREIGN tag, LUA_NUMTYPES bump
**   lobject.h   - LUA_VFOREIGN/LUA_VFFUNC, struct Foreign, value macros
**   lstate.h    - GCUnion member, gco2frn, global_State.allocs
**   lstate.c    - g->allocs init, luaA_close in close_state
**   lgc.c       - objsize, getgclist, reallymarkobject, propagatemark,
**                 freeobj cases; luaA_markgcobject helper
**   ltm.c       - luaT_typenames_ entry, luaT_objtypename branch
**   lvm.c       - luaV_finishget/finishset/objlen branches
**   ldo.c       - luaD_precall/pretailcall LUA_VFFUNC cases
**   lapi.c      - lua_rawlen/lua_topointer cases, push/is/to API (M4)
**   ltests.c    - printvalue, checkrefs, checkgraylist, checkobject
**   linit.c/lualib.h - stdlib registration
**   makefile    - LIBALLOCS_CONFIG block, lallocs*.o, dep lines
**
** This header is included by core files compiled with the stock flags,
** so it must not include liballocs (or other foreign) headers; all
** liballocs types appear only as opaque pointers here.
*/

#ifndef lallocs_h
#define lallocs_h

#if defined(LUA_USE_LIBALLOCS)

#include "lobject.h"
#include "lstate.h"


/*
** {==================================================================
** Foreign marshalling types
** ===================================================================
**
** Port of the vmextensions/lua LuaForeignType vtable, retargeted from
** the public stack API to TValues. Conventions:
**  - ft_get/ft_copy write exactly one value into *out (a valid,
**    anchored slot - stack slot or fresh-table cell) on success.
**  - ft_store reads a TValue and writes the C representation to dest.
**  - ft_ctor keeps the STACK protocol of the extension (args at
**    [first_arg..top], pushes the constructed value): constructors
**    only run in C-function contexts where the public API is legal.
**  - ft_dataptr returns a raw pointer to existing C storage of this
**    type held by the value (zero-copy argument path), or NULL.
**    Never errors, never pushes.
**  - All return 0 on success or -1 with an error message pushed on the
**    Lua stack (luaO_pushfstring). They must not raise (longjmp): they
**    get called from libffi frames and GC contexts. Only outermost VM
**    entry points may turn a -1 into an error.
*/

typedef struct ForeignType ForeignType;

struct ForeignType {
  const struct uniqtype *ft_type;
  lu_byte ft_kind;  /* cached UNIQTYPE_KIND */
  lu_byte ft_frn;   /* values of this type have a Foreign representation */
  int (*ft_ctor) (lua_State *L, int first_arg, ForeignType *ft);
  int (*ft_get)  (lua_State *L, void *data, ForeignType *ft, TValue *out);
  int (*ft_copy) (lua_State *L, void *data, ForeignType *ft, TValue *out);
  int (*ft_store)(lua_State *L, const TValue *v, void *dest, ForeignType *ft);
  void *(*ft_dataptr)(lua_State *L, const TValue *v, ForeignType *ft);
};

/* COMPOSITE: per-field info, linear scan + offset-0 chain (see lallocs.c) */
typedef struct ForeignField {
  ForeignType *type;  /* NULL when the field's type is not handled */
  size_t offset;
  const char *name;
} ForeignField;

typedef struct ForeignTypeComposite {
  ForeignType ft;
  unsigned nfields;
  ForeignField *fields;
} ForeignTypeComposite;

/* ADDRESS and ARRAY share one shape (element access via pointee) */
typedef struct ForeignTypeAddress {
  ForeignType ft;
  ForeignType *pointee;  /* NULL when the pointee kind is unhandled */
} ForeignTypeAddress;

/* SUBPROGRAM: lazy libffi call interface (see lallocsffi.c) */
typedef struct ForeignTypeFunction {
  ForeignType ft;
  void *ff_cif;                /* ffi_cif*, opaque here (ffi.h stays
                                  private to lallocsffi.c) */
  ForeignType **ff_argtypes;
  ForeignType *ff_rettype;
} ForeignTypeFunction;


/* additional Foreign flags (lobject.h defines FF_BASE, FF_HANDLE) */
#define FF_RELEASED	4  /* release()d: backing chunk may be gone */
#define FF_CLOSURE	8  /* addr is an ffi trampoline (see lallocsffi.c) */

/* }================================================================== */


/*
** {==================================================================
** Per-state integration state
** ===================================================================
*/

/* one entry in the foreign intern table (key: addr, ftype, length) */
typedef struct AllocsInternEntry {
  void *addr;
  ForeignType *ftype;
  size_t length;
  Foreign *f;  /* NULL: free slot; LUAA_DEADENTRY: tombstone */
} AllocsInternEntry;

/* tombstone sentinel: a static never-used object, compared by address */
LUAI_DDEC(Foreign luaA_deadentry_;)
#define LUAA_DEADENTRY	(&luaA_deadentry_)

/* generic native pointer-keyed map (open addressing, tombstones) */
typedef struct AllocsMapEntry {
  const void *key;   /* NULL: free; LUAA_DEADKEY: tombstone */
  void *val;
} AllocsMapEntry;

typedef struct AllocsMap {
  AllocsMapEntry *tab;
  size_t size;  /* capacity, power of 2; 0 until first insert */
  size_t use;   /* live + tombstones */
  size_t live;
} AllocsMap;

#define LUAA_DEADKEY	cast(const void *, LUAA_DEADENTRY)

/*
** Created lazily on first use (see 'luaA_getstate'); allocated with
** plain malloc, NOT luaM_*: it must stay out of the Lua memory
** accounting, and the liballocs GC-policy callbacks read parts of it
** from outside the VM.
*/
typedef struct AllocsState {
  lua_State *mainL;           /* the state's main thread */
  AllocsInternEntry *intern;  /* (addr,ftype,length) -> live Foreign */
  size_t internsize;  /* capacity, always a power of 2 */
  size_t internuse;   /* live entries + tombstones */
  size_t internlive;  /* live entries */
  AllocsMap typemap;  /* memo key -> ForeignType* (immortal) */
  AllocsMap basemap;  /* base addr -> FF_BASE Foreign* */
  AllocsMap roots;    /* C slot addr -> Foreign* it keeps alive (strong) */
  AllocsMap extras;   /* addr -> closure context (lallocsffi.c) */
} AllocsState;


LUAI_FUNC AllocsState *luaA_getstate (lua_State *L);
LUAI_FUNC void luaA_close (lua_State *L);

/* generic native map (shared by lallocs.c / lallocsffi.c) */
LUAI_FUNC void *luaA_mapget (AllocsMap *m, const void *key);
LUAI_FUNC void luaA_mapset (lua_State *L, AllocsMap *m, const void *key,
                            void *val);
LUAI_FUNC void luaA_mapremove (AllocsMap *m, const void *key);

/* }================================================================== */


/*
** {==================================================================
** Internal API
** ===================================================================
*/

/* GC hooks (called from lgc.c) */
LUAI_FUNC l_mem luaA_traverseforeign (global_State *g, Foreign *f);
LUAI_FUNC void luaA_freeforeign (lua_State *L, Foreign *f);
LUAI_FUNC void luaA_markroots (global_State *g);

/* marking entry point implemented in lgc.c */
LUAI_FUNC void luaA_markgcobject (global_State *g, GCObject *o);

/* creation / access */
LUAI_FUNC Foreign *luaA_newforeign (lua_State *L, void *addr,
                                    ForeignType *ftype, size_t length,
                                    lu_byte tag);
LUAI_FUNC Foreign *luaA_internlookup (AllocsState *as, void *addr,
                                      ForeignType *ftype, size_t length);
LUAI_FUNC void luaA_pushforeign (lua_State *L, Foreign *f);

/* the registered base Foreign for the allocation containing addr,
** creating it if liballocs can type the allocation; NULL otherwise */
LUAI_FUNC Foreign *luaA_getorcreatebase (lua_State *L, void *addr);

/* uniqtype -> ForeignType (memoized; NULL with message pushed) */
LUAI_FUNC ForeignType *luaA_gettype (lua_State *L,
                                     const struct uniqtype *type);

/* VM integration: return the tag of the result written to 'val', or
** LUAA_NOTFOUND to fall through to the metamethod path; raise on real
** errors. luaA_foreignset returns 0 on success / LUAA_NOTFOUND. */
#define LUAA_NOTFOUND	(-1)
LUAI_FUNC int luaA_foreignget (lua_State *L, const TValue *t,
                               const TValue *key, StkId val);
LUAI_FUNC int luaA_foreignset (lua_State *L, const TValue *t,
                               const TValue *key, TValue *val);
/* 1 with *out = element count if '#t' is handled natively, else 0 */
LUAI_FUNC int luaA_foreignlen (lua_State *L, const TValue *t,
                               lua_Integer *out);

/* composite field lookup (offset-0 first-member chain); 1 if found */
LUAI_FUNC int luaA_findfield (ForeignType *composite_ft, const char *key,
                              ForeignType **out_ft, size_t *out_off);

/* SUBPROGRAM pieces implemented in lallocsffi.c */
LUAI_FUNC void luaA_traverseclosure (global_State *g, Foreign *f);
LUAI_FUNC void luaA_freeclosure (AllocsState *as, Foreign *f);
LUAI_FUNC int luaA_functionsetup (lua_State *L, ForeignTypeFunction *ft);
LUAI_FUNC int luaA_closurector (lua_State *L, int first_arg,
                                ForeignType *ft);

/* foreign-function call (lallocsffi.c); lua_CFunction shape, invoked
** through precallC from luaD_precall's LUA_VFFUNC case */
LUAI_FUNC int luaA_callforeign (lua_State *L);

/* stack adapters over the TValue vtable (C-function contexts only) */
LUAI_FUNC int luaA_pushfromdata (lua_State *L, void *data, ForeignType *ft,
                                 int copy);
LUAI_FUNC int luaA_storestack (lua_State *L, int idx, void *dest,
                               ForeignType *ft);

/* Foreign value of the TValue at a C-function stack index (or NULL) */
LUAI_FUNC Foreign *luaA_toforeign (lua_State *L, int idx);

/* naming (for luaT_objtypename; may return NULL) */
LUAI_FUNC const char *luaA_foreigntypename (lua_State *L, Foreign *f);

/* raw dereferenceable pointer for f->addr (identity without stackscan) */
LUAI_FUNC void *luaA_rawaddr (Foreign *f);

/* release() support: clear tracked slots, detach, poison */
LUAI_FUNC void luaA_release (lua_State *L, Foreign *f);

/* helpers shared with lallocsffi.c / lallocslib.c */
LUAI_FUNC int luaA_typeacceptable (ForeignType *have, ForeignType *want);
LUAI_FUNC int luaA_istriviallycopiable (ForeignType *ft);
LUAI_FUNC int luaA_newcell (lua_State *L, ForeignType *elem_ft, int init_idx);
LUAI_FUNC void luaA_retainstoredptr (lua_State *L, Foreign *f,
                                     const void **dest, const void *val);

/* bounds query (lallocsbounds.c, minicrunch port) */
typedef struct AllocsBounds {
  unsigned long base;
  unsigned size;
} AllocsBounds;
LUAI_FUNC AllocsBounds luaA_fetchbounds (const void *obj,
                                         const struct uniqtype *t);

/* }================================================================== */

#endif

#endif
