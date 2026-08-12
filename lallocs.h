/*
** $Id: lallocs.h $
** Native support for liballocs-typed C objects (LUA_TFOREIGN)
** See Copyright Notice in lua.h
**
** Everything in this header (and in lallocs.c, lallocsffi.c,
** lallocslib.c) is compiled only under LUA_USE_LIBALLOCS; the default
** build is pristine upstream Lua.
**
** Core touch-point checklist (keep in sync when merging upstream):
**   lua.h       - LUA_TFOREIGN tag, LUA_NUMTYPES bump
**   lobject.h   - LUA_VFOREIGN/LUA_VFFUNC, struct Foreign, value macros
**   lstate.h    - GCUnion member, gco2frn, global_State.allocs
**   lstate.c    - g->allocs init, luaA_close in close_state
**   lgc.c       - objsize, getgclist, reallymarkobject, propagatemark,
**                 freeobj cases; luaA_markgcobject helper
**   ltm.c       - luaT_typenames_ entry, luaT_objtypename branch
**   lvm.c       - luaV_finishget/finishset/objlen branches (M2)
**   ldo.c       - luaD_precall/pretailcall LUA_VFFUNC cases (M2)
**   lapi.c      - lua_rawlen/lua_topointer cases, push/is/to API (M4)
**   ltests.c    - printvalue, checkrefs, checkgraylist, checkobject
**   linit.c/lualib.h - stdlib registration
**   makefile    - LIBALLOCS_CONFIG block, lallocs*.o, dep lines
*/

#ifndef lallocs_h
#define lallocs_h

#if defined(LUA_USE_LIBALLOCS)

#include "lobject.h"
#include "lstate.h"


/*
** One entry in the foreign intern table: at most one live Foreign per
** (addr, type, length) triple per state, so equality, table-key
** hashing and 'rawequal' work by GC-pointer identity with no changes
** to ltable.c/lvm.c.
*/
typedef struct AllocsInternEntry {
  void *addr;
  const struct uniqtype *type;
  size_t length;
  Foreign *f;  /* NULL: free slot; LUAA_DEADENTRY: tombstone */
} AllocsInternEntry;

/* tombstone sentinel: a static never-used object, compared by address */
LUAI_DDEC(Foreign luaA_deadentry_;)
#define LUAA_DEADENTRY	(&luaA_deadentry_)


/*
** Per-global_State liballocs integration state. Created lazily on
** first use (see 'luaA_getstate'); allocated with plain malloc, NOT
** luaM_*: it must stay out of the Lua memory accounting, and later
** milestones read parts of it from liballocs GC-policy callbacks that
** may run outside the VM thread.
*/
typedef struct AllocsState {
  AllocsInternEntry *intern;  /* open-addressing hash of live Foreigns */
  size_t internsize;  /* capacity, always a power of 2 */
  size_t internuse;   /* live entries + tombstones */
  size_t internlive;  /* live entries */
} AllocsState;


LUAI_FUNC AllocsState *luaA_getstate (lua_State *L);
LUAI_FUNC void luaA_close (lua_State *L);

/* GC hooks (called from lgc.c) */
LUAI_FUNC l_mem luaA_traverseforeign (global_State *g, Foreign *f);
LUAI_FUNC void luaA_freeforeign (lua_State *L, Foreign *f);

/* marking entry point implemented in lgc.c */
LUAI_FUNC void luaA_markgcobject (global_State *g, GCObject *o);

/* creation / access */
LUAI_FUNC Foreign *luaA_newforeign (lua_State *L, void *addr,
                                    const struct uniqtype *type,
                                    size_t length, lu_byte tag);
LUAI_FUNC Foreign *luaA_internlookup (AllocsState *as, void *addr,
                                      const struct uniqtype *type,
                                      size_t length);
LUAI_FUNC void luaA_pushforeign (lua_State *L, Foreign *f);

/* naming (for luaT_objtypename; may return NULL) */
LUAI_FUNC const char *luaA_foreigntypename (lua_State *L, Foreign *f);

#endif

#endif
