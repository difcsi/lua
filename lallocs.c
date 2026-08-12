/*
** $Id: lallocs.c $
** Native support for liballocs-typed C objects (LUA_TFOREIGN)
** See Copyright Notice in lua.h
*/

#define lallocs_c
#define LUA_CORE

#include "lprefix.h"


#include <stdlib.h>
#include <string.h>

#include "lua.h"

#if defined(LUA_USE_LIBALLOCS)

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lallocs.h"


LUAI_DDEF Foreign luaA_deadentry_;


/*
** {==================================================================
** Intern table
** ===================================================================
**
** Open-addressing hash with linear probing and tombstones, keyed by
** (addr, type, length). Entries are weak: a Foreign removes itself in
** 'luaA_freeforeign'. The table is malloc-based so that it can be read
** without touching the Lua heap (GC-policy callbacks, later
** milestones, may consult it from outside the VM thread).
*/

#define INTERN_INITSIZE		64  /* must be a power of 2 */

static size_t internhash (void *addr, const struct uniqtype *type,
                          size_t length) {
  size_t h = cast_sizet((size_t)addr >> 3);
  h ^= cast_sizet((size_t)type >> 4);
  h ^= length;
  h *= cast_sizet(2654435761u);  /* Knuth multiplicative mixing */
  return h;
}


static int entrymatches (const AllocsInternEntry *e, void *addr,
                         const struct uniqtype *type, size_t length) {
  return e->addr == addr && e->type == type && e->length == length;
}


/*
** Find the slot for a key: either the live entry with that key, or
** the slot where it should be inserted (first tombstone on the probe
** path, else the first free slot).
*/
static AllocsInternEntry *internfind (AllocsState *as, void *addr,
                                      const struct uniqtype *type,
                                      size_t length) {
  size_t mask = as->internsize - 1;
  size_t i = internhash(addr, type, length) & mask;
  AllocsInternEntry *tomb = NULL;
  for (;;) {
    AllocsInternEntry *e = &as->intern[i];
    if (e->f == NULL)  /* end of probe path */
      return (tomb != NULL) ? tomb : e;
    else if (e->f == LUAA_DEADENTRY) {
      if (tomb == NULL) tomb = e;  /* remember first tombstone */
    }
    else if (entrymatches(e, addr, type, length))
      return e;
    i = (i + 1) & mask;
  }
}


static void internresize (lua_State *L, AllocsState *as, size_t newsize) {
  AllocsInternEntry *newtab =
      cast(AllocsInternEntry *, calloc(newsize, sizeof(AllocsInternEntry)));
  AllocsInternEntry *oldtab = as->intern;
  size_t oldsize = as->internsize;
  size_t i;
  if (newtab == NULL)
    luaM_error(L);
  as->intern = newtab;
  as->internsize = newsize;
  as->internuse = 0;
  for (i = 0; i < oldsize; i++) {
    AllocsInternEntry *e = &oldtab[i];
    if (e->f != NULL && e->f != LUAA_DEADENTRY) {
      AllocsInternEntry *ne = internfind(as, e->addr, e->type, e->length);
      *ne = *e;
      as->internuse++;
    }
  }
  free(oldtab);
}


static void interninsert (lua_State *L, AllocsState *as, Foreign *f) {
  AllocsInternEntry *e;
  if (as->internuse * 4 >= as->internsize * 3)  /* load factor >= 3/4? */
    internresize(L, as, (as->internlive * 4 >= as->internsize * 3)
                        ? as->internsize * 2  /* genuinely full: grow */
                        : as->internsize);    /* mostly tombstones: rehash */
  e = internfind(as, f->addr, f->type, f->length);
  lua_assert(e->f == NULL || e->f == LUAA_DEADENTRY);
  if (e->f == NULL)
    as->internuse++;  /* consuming a fresh slot (not a tombstone) */
  e->addr = f->addr;
  e->type = f->type;
  e->length = f->length;
  e->f = f;
  as->internlive++;
}


Foreign *luaA_internlookup (AllocsState *as, void *addr,
                            const struct uniqtype *type, size_t length) {
  AllocsInternEntry *e;
  if (as == NULL || as->intern == NULL)
    return NULL;
  e = internfind(as, addr, type, length);
  return (e->f != NULL && e->f != LUAA_DEADENTRY) ? e->f : NULL;
}

/* }================================================================== */


/*
** {==================================================================
** State lifecycle
** ===================================================================
*/

AllocsState *luaA_getstate (lua_State *L) {
  global_State *g = G(L);
  AllocsState *as = g->allocs;
  if (as == NULL) {  /* first use: create it */
    as = cast(AllocsState *, calloc(1, sizeof(AllocsState)));
    if (as == NULL)
      luaM_error(L);
    as->intern = cast(AllocsInternEntry *,
                      calloc(INTERN_INITSIZE, sizeof(AllocsInternEntry)));
    if (as->intern == NULL) {
      free(as);
      luaM_error(L);
    }
    as->internsize = INTERN_INITSIZE;
    g->allocs = as;
  }
  return as;
}


void luaA_close (lua_State *L) {
  global_State *g = G(L);
  AllocsState *as = g->allocs;
  if (as != NULL) {
    lua_assert(as->internlive == 0);  /* all Foreigns already freed */
    free(as->intern);
    free(as);
    g->allocs = NULL;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Creation and GC hooks
** ===================================================================
*/

Foreign *luaA_newforeign (lua_State *L, void *addr,
                          const struct uniqtype *type, size_t length,
                          lu_byte tag) {
  global_State *g = G(L);
  AllocsState *as = luaA_getstate(L);
  Foreign *f = luaA_internlookup(as, addr, type, length);
  GCObject *o;
  lua_assert(tag == LUA_VFOREIGN || tag == LUA_VFFUNC);
  if (f != NULL) {  /* already interned? */
    lua_assert(f->tt == tag);
    if (isdead(g, obj2gco(f)))  /* dead but not yet swept? */
      changewhite(obj2gco(f));  /* resurrect it */
    return f;
  }
  o = luaC_newobj(L, tag, sizeof(Foreign));
  f = gco2frn(o);
  f->flags = 0;
  f->addr = addr;
  f->type = type;
  f->length = length;
  f->base = NULL;
  f->ftype = NULL;
  f->gclist = NULL;
  interninsert(L, as, f);
  return f;
}


void luaA_pushforeign (lua_State *L, Foreign *f) {
  lua_lock(L);
  setfrnvalue2s(L, L->top.p, f);
  api_incr_top(L);
  lua_unlock(L);
}


/*
** Traverse a foreign object during GC propagation. In this milestone
** only the base anchor of interior views is a GC edge; the traversal
** of the underlying C memory via its uniqtype layout arrives with the
** native-GC milestone.
*/
l_mem luaA_traverseforeign (global_State *g, Foreign *f) {
  if (f->base != NULL)
    luaA_markgcobject(g, obj2gco(f->base));
  return 1;
}


void luaA_freeforeign (lua_State *L, Foreign *f) {
  AllocsState *as = G(L)->allocs;
  lua_assert(as != NULL);
  {
    AllocsInternEntry *e = internfind(as, f->addr, f->type, f->length);
    if (e->f == f) {  /* still our entry? */
      e->f = LUAA_DEADENTRY;  /* tombstone it */
      as->internlive--;
    }
  }
  luaM_freemem(L, f, sizeof(Foreign));
}

/* }================================================================== */


/*
** Type name for error messages and 'luaT_objtypename'. Returns NULL
** when nothing better than the generic "foreign" is known. (Real
** uniqtype names arrive with the marshalling milestone.)
*/
const char *luaA_foreigntypename (lua_State *L, Foreign *f) {
  UNUSED(L);
  UNUSED(f);
  return NULL;
}

#endif
