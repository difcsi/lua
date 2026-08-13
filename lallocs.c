/*
** $Id: lallocs.c $
** Native support for liballocs-typed C objects (LUA_TFOREIGN)
** See Copyright Notice in lua.h
**
** Port of the pycallocs vmextensions/lua userdata-proxy extension
** (proxy.c, foreign_type.c, foreign_basetype.c, composite_proxy.c,
** address_proxy.c) into the VM: foreign values are first-class
** collectable objects, interned one per (addr, type, length), with
** field/element access dispatched natively from luaV_finishget/set.
*/

#define lallocs_c
#define LUA_CORE

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "lprefix.h"


#include <pthread.h>
#include <stdint.h>
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
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"
#include "lallocs.h"

#include <liballocs.h>
#include <dwarf.h>

#ifndef __STDC_NO_COMPLEX__
#include <complex.h>
#endif

/*
** This file must also link (and run) against only the
** liballocs_dummyweaks stubs -- the configuration used to run the
** upstream test suite, where the full runtime stays out of the
** process. Symbols the stubs do not provide are declared weak and
** null-checked at their call sites; foreign functionality then
** degrades to "not present" errors instead of failing the link.
*/
#pragma weak __liballocs_register_gc_policy
#pragma weak __liballocs_set_alloc_type
#pragma weak __liballocs_get_or_create_address_type
#pragma weak __liballocs_get_or_create_array_type
#pragma weak __liballocs_get_or_create_flexible_array_type
#pragma weak __alaska_heap_allocator

/*
** Translate-then-index glue: ss_translate() re-derives the raw backing
** from an Alaska handle (identity without stackscan); refcount ops
** no-op without it. Never cache a translated pointer across calls that
** may run the Alaska GC.
*/
#if defined(LUA_ALLOCS_HAVE_STACKSCAN)
#include <handle_query.h>
#else
#define ss_translate(p)		(p)
#define ss_is_handle(p)		0
#define ss_inc_refcount(p)	((void)(p))
#define ss_dec_refcount(p)	((void)(p))
#define ss_alloc_get_type(p)	__liballocs_get_alloc_type(p)
#endif

#ifdef SS_HAVE_ALASKA
#define totally_malloc(x)	halloc(x)
#define totally_free(ptr)	hfree(ptr)
#else
#define totally_malloc(x)	malloc(x)
/* (inline over __liballocs_detach_lifetime_policy, which the
** dummyweaks stubs provide as a no-op) */
#define totally_free(ptr)	__liballocs_detach_manual_dealloc_policy(ptr)
#endif

/* true iff the full liballocs runtime is present (not just stubs) */
#define allocs_runtime_present()  (&__liballocs_register_gc_policy != NULL)

/* liballocs lifetime hook, inserted by the trapptrwrites pass in
** instrumented code; declared by hand exactly like the extension. */
void __notify_ptr_write (const void **dest, const void *val);
#pragma weak __notify_ptr_write


LUAI_DDEF Foreign luaA_deadentry_;

/*
** One process-wide GC policy serves every state: liballocs caps the
** number of policies far below "one per state", and the callbacks
** carry no context anyway. They consult a registry of live states
** (linked through AllocsState.next, guarded by a mutex: instrumented
** pointer writes can happen on any thread). Chunk lifetime is shared
** through a process-wide refcount map: the policy attaches when the
** first state adopts a chunk and detaches when the last one lets go,
** so two states viewing the same chunk never double-attach and one
** state's close cannot free memory another still views.
**
** Threading caveat (documented in the README): the registry and the
** refcounts are mutex-guarded, but marking GC objects from a callback
** is only safe when the write happens on the owning state's thread --
** the usual arrangement, since the write is made by C code that state
** called into.
*/
static int g_policy_id = -1;
static AllocsState *g_states = NULL;      /* registry head */
static pthread_mutex_t g_states_mu = PTHREAD_MUTEX_INITIALIZER;
static AllocsMap g_chunkrefs;             /* alloc start -> refcount */
static pthread_mutex_t g_chunkrefs_mu = PTHREAD_MUTEX_INITIALIZER;

/* take one shared reference on 'addr'; attach the policy on 0 -> 1 */
static void chunkref_inc (void *addr) {
  size_t n;
  pthread_mutex_lock(&g_chunkrefs_mu);
  n = (size_t)luaA_mapget(&g_chunkrefs, addr);
  luaA_mapset(NULL, &g_chunkrefs, addr, (void *)(n + 1));
  pthread_mutex_unlock(&g_chunkrefs_mu);
  if (n == 0) {
    if (ss_is_handle(addr))
      ss_inc_refcount(addr);
    else
      __liballocs_attach_lifetime_policy(g_policy_id, ss_translate(addr));
  }
}

/* drop one shared reference; detach (possibly freeing) on 1 -> 0 */
static void chunkref_dec (void *addr) {
  size_t n;
  pthread_mutex_lock(&g_chunkrefs_mu);
  n = (size_t)luaA_mapget(&g_chunkrefs, addr);
  lua_assert(n > 0);
  if (n <= 1)
    luaA_mapremove(&g_chunkrefs, addr);
  else
    luaA_mapset(NULL, &g_chunkrefs, addr, (void *)(n - 1));
  pthread_mutex_unlock(&g_chunkrefs_mu);
  if (n == 1) {
    if (ss_is_handle(addr))
      ss_dec_refcount(addr);
    else  /* may free the chunk; delref callbacks re-enter natively */
      __liballocs_detach_lifetime_policy(g_policy_id, ss_translate(addr));
  }
}


void *luaA_rawaddr (Foreign *f) {
  return ss_translate(f->addr);
}


/*
** {==================================================================
** Generic native pointer-keyed map
** ===================================================================
*/

#define MAP_INITSIZE	16

static size_t maphash (const void *key) {
  size_t h = (size_t)key >> 3;
  h *= cast_sizet(2654435761u);
  return h;
}

static AllocsMapEntry *mapfind (AllocsMap *m, const void *key) {
  size_t mask = m->size - 1;
  size_t i = maphash(key) & mask;
  AllocsMapEntry *tomb = NULL;
  for (;;) {
    AllocsMapEntry *e = &m->tab[i];
    if (e->key == NULL)
      return (tomb != NULL) ? tomb : e;
    else if (e->key == LUAA_DEADKEY) {
      if (tomb == NULL) tomb = e;
    }
    else if (e->key == key)
      return e;
    i = (i + 1) & mask;
  }
}

void *luaA_mapget (AllocsMap *m, const void *key) {
  AllocsMapEntry *e;
  if (m->size == 0) return NULL;
  e = mapfind(m, key);
  return (e->key == key) ? e->val : NULL;
}

static int mapresize (AllocsMap *m, size_t newsize) {
  AllocsMapEntry *newtab =
      cast(AllocsMapEntry *, calloc(newsize, sizeof(AllocsMapEntry)));
  AllocsMapEntry *oldtab = m->tab;
  size_t oldsize = m->size;
  size_t i;
  if (newtab == NULL) return -1;
  m->tab = newtab;
  m->size = newsize;
  m->use = 0;
  for (i = 0; i < oldsize; i++) {
    AllocsMapEntry *e = &oldtab[i];
    if (e->key != NULL && e->key != LUAA_DEADKEY) {
      AllocsMapEntry *ne = mapfind(m, e->key);
      *ne = *e;
      m->use++;
    }
  }
  free(oldtab);
  return 0;
}

/*
** Insert/update. 'L' may be NULL (GC-policy callback context): then an
** out-of-memory failure silently drops the entry -- malloc failure is
** already catastrophic, and callbacks must never raise.
*/
void luaA_mapset (lua_State *L, AllocsMap *m, const void *key, void *val) {
  AllocsMapEntry *e;
  if (m->size == 0) {
    if (mapresize(m, MAP_INITSIZE) != 0) {
      if (L) luaM_error(L);
      return;
    }
  }
  else if (m->use * 4 >= m->size * 3) {
    size_t newsize = (m->live * 4 >= m->size * 3) ? m->size * 2 : m->size;
    if (mapresize(m, newsize) != 0) {
      if (L) luaM_error(L);
      return;
    }
  }
  e = mapfind(m, key);
  if (e->key == key) {  /* update */
    e->val = val;
    return;
  }
  if (e->key == NULL)
    m->use++;
  e->key = key;
  e->val = val;
  m->live++;
}

void luaA_mapremove (AllocsMap *m, const void *key) {
  AllocsMapEntry *e;
  if (m->size == 0) return;
  e = mapfind(m, key);
  if (e->key == key) {
    e->key = LUAA_DEADKEY;
    e->val = NULL;
    m->live--;
  }
}

static void mapfree (AllocsMap *m) {
  free(m->tab);
  m->tab = NULL;
  m->size = m->use = m->live = 0;
}

/* }================================================================== */


/*
** {==================================================================
** Intern table (addr, ftype, length) -> Foreign
** ===================================================================
*/

#define INTERN_INITSIZE		64  /* must be a power of 2 */

static size_t internhash (void *addr, ForeignType *ftype, size_t length) {
  size_t h = (size_t)addr >> 3;
  h ^= (size_t)ftype >> 4;
  h ^= length;
  h *= cast_sizet(2654435761u);
  return h;
}

static int entrymatches (const AllocsInternEntry *e, void *addr,
                         ForeignType *ftype, size_t length) {
  return e->addr == addr && e->ftype == ftype && e->length == length;
}

static AllocsInternEntry *internfind (AllocsState *as, void *addr,
                                      ForeignType *ftype, size_t length) {
  size_t mask = as->internsize - 1;
  size_t i = internhash(addr, ftype, length) & mask;
  AllocsInternEntry *tomb = NULL;
  for (;;) {
    AllocsInternEntry *e = &as->intern[i];
    if (e->f == NULL)
      return (tomb != NULL) ? tomb : e;
    else if (e->f == LUAA_DEADENTRY) {
      if (tomb == NULL) tomb = e;
    }
    else if (entrymatches(e, addr, ftype, length))
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
      AllocsInternEntry *ne = internfind(as, e->addr, e->ftype, e->length);
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
  e = internfind(as, f->addr, f->ftype, f->length);
  lua_assert(e->f == NULL || e->f == LUAA_DEADENTRY);
  if (e->f == NULL)
    as->internuse++;
  e->addr = f->addr;
  e->ftype = f->ftype;
  e->length = f->length;
  e->f = f;
  as->internlive++;
}

Foreign *luaA_internlookup (AllocsState *as, void *addr,
                            ForeignType *ftype, size_t length) {
  AllocsInternEntry *e;
  if (as == NULL || as->intern == NULL)
    return NULL;
  e = internfind(as, addr, ftype, length);
  return (e->f != NULL && e->f != LUAA_DEADENTRY) ? e->f : NULL;
}

/* }================================================================== */


/*
** {==================================================================
** GC-policy callbacks and the external-root set
** ===================================================================
**
** M2 discipline (SLOT_REFS parity): every tracked pointer write into
** policy-attached memory roots the target's base Foreign, strongly,
** keyed by the written slot. Cycles through C memory therefore leak,
** exactly like the extension; the classifying root set that fixes them
** arrives with milestone M3. Callbacks touch only native structures.
*/

/* does this C slot lie inside an allocation that Lua already views
** (i.e. one with a base Foreign)? Such slots are internal edges: the
** traversal discovers them, and rooting them is precisely the
** SLOT_REFS cycle leak. */
static int slot_is_lua_known (AllocsState *as, const void **from) {
  struct allocator *a = NULL;
  const void *alloc_start = NULL;
  struct liballocs_err *err;
  if (luaA_mapget(&as->basemap, from) != NULL)
    return 1;  /* slot at the very start of a known chunk */
  if (!allocs_runtime_present())
    return 0;
  err = __liballocs_get_alloc_info(from, &a, &alloc_start, NULL, NULL, NULL);
  if (err || alloc_start == NULL)
    return 0;
  return luaA_mapget(&as->basemap, alloc_start) != NULL;
}

static void lp_addref (const void *target, const void **from) {
  AllocsState *as;
  pthread_mutex_lock(&g_states_mu);
  for (as = g_states; as != NULL; as = as->next) {
    Foreign *f = cast(Foreign *, luaA_mapget(&as->basemap, target));
    if (f == NULL) continue;
    if (slot_is_lua_known(as, from))
      continue;  /* internal edge: the GC traversal covers it */
    luaA_mapset(NULL, &as->roots, from, f);
    /* Keep the rooted object safe against an in-progress collection:
       it may already have been overlooked (root added after 'atomic')
       or even dead-marked but not yet swept. */
    {
      global_State *g = G(as->mainL);
      GCObject *o = obj2gco(f);
      if (isdead(g, o))
        changewhite(o);  /* resurrect: sweep will keep it */
      else if (keepinvariant(g))
        luaA_markgcobject(g, o);
    }
  }
  pthread_mutex_unlock(&g_states_mu);
}

static void lp_delref (const void *target, const void **from) {
  AllocsState *as;
  (void)target;  /* spurious calls from inexistent locations expected */
  pthread_mutex_lock(&g_states_mu);
  for (as = g_states; as != NULL; as = as->next)
    luaA_mapremove(&as->roots, from);
  pthread_mutex_unlock(&g_states_mu);
}

/* mark every rooted Foreign (called from restartcollection/atomic) */
void luaA_markroots (global_State *g) {
  AllocsState *as = g->allocs;
  size_t i;
  if (as == NULL) return;
  for (i = 0; i < as->roots.size; i++) {
    AllocsMapEntry *e = &as->roots.tab[i];
    if (e->key != NULL && e->key != LUAA_DEADKEY)
      luaA_markgcobject(g, obj2gco(cast(Foreign *, e->val)));
  }
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
    as->mainL = mainthread(g);
    g->allocs = as;
    pthread_mutex_lock(&g_states_mu);
    as->next = g_states;  /* enter the process registry */
    g_states = as;
    pthread_mutex_unlock(&g_states_mu);
    if (g_policy_id < 0 && allocs_runtime_present())
      g_policy_id = __liballocs_register_gc_policy(lp_addref, lp_delref);
  }
  return as;
}


void luaA_close (lua_State *L) {
  global_State *g = G(L);
  AllocsState *as = g->allocs;
  if (as != NULL) {
    AllocsState **p;
    pthread_mutex_lock(&g_states_mu);
    for (p = &g_states; *p != NULL; p = &(*p)->next) {
      if (*p == as) {  /* leave the registry: callbacks must not see us */
        *p = as->next;
        break;
      }
    }
    pthread_mutex_unlock(&g_states_mu);
    lua_assert(as->internlive == 0);  /* all Foreigns already freed */
    free(as->intern);
    mapfree(&as->typemap);
    mapfree(&as->basemap);
    mapfree(&as->roots);
    mapfree(&as->extras);
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

Foreign *luaA_newforeign (lua_State *L, void *addr, ForeignType *ftype,
                          size_t length, lu_byte tag) {
  global_State *g = G(L);
  AllocsState *as = luaA_getstate(L);
  Foreign *f = luaA_internlookup(as, addr, ftype, length);
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
  f->type = (ftype != NULL) ? ftype->ft_type : NULL;
  f->length = length;
  f->base = NULL;
  f->ftype = ftype;
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


Foreign *luaA_toforeign (lua_State *L, int idx) {
  const TValue *v;
  idx = lua_absindex(L, idx);
  if (idx <= 0 || L->ci->func.p + idx >= L->top.p) return NULL;
  v = s2v(L->ci->func.p + idx);
  return ttisforeign(v) ? frnvalue(v) : NULL;
}


/* mark the base Foreign viewing the allocation 'target' points into
** (exact basemap hit for base pointers, else resolve the allocation
** start); interior pointers to unknown chunks are invisible edges */
static l_mem marktarget (global_State *g, AllocsState *as, void *target) {
  Foreign *tf = cast(Foreign *, luaA_mapget(&as->basemap, target));
  if (tf == NULL && allocs_runtime_present()) {
    struct allocator *a = NULL;
    const void *alloc_start = NULL;
    struct liballocs_err *err =
        __liballocs_get_alloc_info(target, &a, &alloc_start, NULL, NULL,
                                   NULL);
    if (!err && alloc_start != NULL)
      tf = cast(Foreign *, luaA_mapget(&as->basemap, alloc_start));
  }
  if (tf != NULL)
    luaA_markgcobject(g, obj2gco(tf));
  return 1;
}

/*
** Walk the pointer slots of a C region via its uniqtype layout,
** marking the base Foreigns of pointed-to allocations: C->C edges
** become ordinary GC edges, which is what makes host<->C reference
** cycles collectable. 'nelems_hint' bounds arrays whose static type
** has no known length (flexible arrays: the view's element count).
*/
static l_mem traverse_region (global_State *g, AllocsState *as,
                              char *region, const struct uniqtype *t,
                              size_t nelems_hint) {
  l_mem work = 0;
  if (t == NULL) return 0;
  if (UNIQTYPE_IS_POINTER_TYPE(t)) {
    void *target = *(void **)region;
    if (target != NULL)
      work += marktarget(g, as, target);
  }
  else if (UNIQTYPE_IS_ARRAY_TYPE(t)) {
    const struct uniqtype *et = UNIQTYPE_ARRAY_ELEMENT_TYPE(t);
    size_t esz, n, i;
    if (et == NULL) return 0;
    esz = UNIQTYPE_SIZE_IN_BYTES(et);
    if (esz == 0) return 0;
    n = UNIQTYPE_HAS_KNOWN_LENGTH(t) ? UNIQTYPE_ARRAY_LENGTH(t)
                                     : nelems_hint;
    for (i = 0; i < n; i++)
      work += traverse_region(g, as, region + i * esz, et, 0);
  }
  else if (UNIQTYPE_IS_COMPOSITE_TYPE(t)) {
    unsigned nmemb = UNIQTYPE_COMPOSITE_MEMBER_COUNT(t);
    unsigned i;
    for (i = 0; i < nmemb; i++)
      work += traverse_region(g, as, region + t->related[i].un.memb.off,
                              t->related[i].un.memb.ptr, 0);
  }
  return work;
}

l_mem luaA_traverseforeign (global_State *g, Foreign *f) {
  l_mem work = 1;
  if (f->base != NULL)
    luaA_markgcobject(g, obj2gco(f->base));
  if (f->flags & FF_CLOSURE)
    luaA_traverseclosure(g, f);
  if ((f->flags & FF_BASE) && !(f->flags & FF_RELEASED)
      && f->ftype != NULL && g->allocs != NULL)
    work += traverse_region(g, g->allocs, cast(char *, luaA_rawaddr(f)),
                            f->ftype->ft_type, f->length);
  return work;
}


void luaA_freeforeign (lua_State *L, Foreign *f) {
  AllocsState *as = G(L)->allocs;
  lua_assert(as != NULL);
  if ((f->flags & (FF_BASE | FF_CLOSURE)) && !(f->flags & FF_RELEASED)) {
    if (cast(Foreign *, luaA_mapget(&as->basemap, f->addr)) == f)
      luaA_mapremove(&as->basemap, f->addr);
  }
  if ((f->flags & FF_BASE) && !(f->flags & FF_RELEASED))
    chunkref_dec(f->addr);
  if (f->flags & FF_CLOSURE)
    luaA_freeclosure(as, f);
  {
    AllocsInternEntry *e = internfind(as, f->addr, f->ftype, f->length);
    if (e->f == f) {  /* still our entry? */
      e->f = LUAA_DEADENTRY;
      as->internlive--;
    }
  }
  luaM_freemem(L, f, sizeof(Foreign));
}

/* }================================================================== */


/*
** {==================================================================
** Scalar (BASE/ENUMERATION) marshalling
** ===================================================================
** Port of foreign_basetype.c onto TValues. Same numeric fidelity notes:
** uint64 above LUA_MAXINTEGER degrades to float; long double through
** double; _Complex as {re=,im=} tables; bit fields rejected.
*/

#define pusherr(L,fmt)		(luaO_pushfstring(L, "%s", fmt), -1)

static int tointeger_arg (lua_State *L, const TValue *v, int accept_char,
                          lua_Integer *out) {
  (void)L;
  if (luaV_tointeger(v, out, F2Ieq))  /* accepts numeric strings too */
    return 1;
  if (accept_char && ttisstring(v)) {
    size_t len;
    const char *s = getlstr(tsvalue(v), len);
    if (len == 1) { *out = (unsigned char)s[0]; return 1; }
  }
  return 0;
}

static int bool_get (lua_State *L, void *data, ForeignType *ft, TValue *out) {
  (void)L; (void)ft;
  if (*(uint8_t *)data != 0) setbtvalue(out); else setbfvalue(out);
  return 0;
}
static int bool_store (lua_State *L, const TValue *v, void *dest,
                       ForeignType *ft) {
  (void)L; (void)ft;
  *(uint8_t *)dest = l_isfalse(v) ? 0 : 1;
  return 0;
}

#define DEFINE_UINT_GETSTORE(size) \
static int uint##size##_get (lua_State *L, void *data, ForeignType *ft, \
                             TValue *out) { \
  uint##size##_t u = *(uint##size##_t *)data; \
  (void)L; (void)ft; \
  if ((uint64_t)u > (uint64_t)LUA_MAXINTEGER) \
    { setfltvalue(out, (lua_Number)u); } \
  else \
    { setivalue(out, (lua_Integer)u); } \
  return 0; \
} \
static int uint##size##_store (lua_State *L, const TValue *v, void *dest, \
                               ForeignType *ft) { \
  lua_Integer i; \
  (void)ft; \
  if (!tointeger_arg(L, v, 0, &i)) { \
    luaO_pushfstring(L, "expected an integer, got %s", \
                     luaT_objtypename(L, v)); \
    return -1; \
  } \
  if (i < 0 || (lua_Integer)(uint##size##_t)i != i) \
    return pusherr(L, "argument does not fit into a " #size \
                      " bit unsigned integer"); \
  *(uint##size##_t *)dest = (uint##size##_t)i; \
  return 0; \
}
DEFINE_UINT_GETSTORE(8)
DEFINE_UINT_GETSTORE(16)
DEFINE_UINT_GETSTORE(32)
DEFINE_UINT_GETSTORE(64)

#define DEFINE_INT_GETSTORE(size) \
static int int##size##_get (lua_State *L, void *data, ForeignType *ft, \
                            TValue *out) { \
  (void)L; (void)ft; \
  setivalue(out, (lua_Integer)*(int##size##_t *)data); \
  return 0; \
} \
static int int##size##_store (lua_State *L, const TValue *v, void *dest, \
                              ForeignType *ft) { \
  lua_Integer i; \
  (void)ft; \
  if (!tointeger_arg(L, v, 0, &i)) { \
    luaO_pushfstring(L, "expected an integer, got %s", \
                     luaT_objtypename(L, v)); \
    return -1; \
  } \
  if ((lua_Integer)(int##size##_t)i != i) \
    return pusherr(L, "argument does not fit into a " #size \
                      " bit signed integer"); \
  *(int##size##_t *)dest = (int##size##_t)i; \
  return 0; \
}
DEFINE_INT_GETSTORE(8)
DEFINE_INT_GETSTORE(16)
DEFINE_INT_GETSTORE(32)
DEFINE_INT_GETSTORE(64)

/* chars read as Lua strings of the char's byte width; stores accept a
** string of that exact length, or an integer code point */
#define DEFINE_CHAR_GETSTORE(size) \
static int char##size##_get (lua_State *L, void *data, ForeignType *ft, \
                             TValue *out) { \
  (void)ft; \
  setsvalue(L, out, luaS_newlstr(L, (const char *)data, size / 8)); \
  return 0; \
} \
static int uchar##size##_store (lua_State *L, const TValue *v, void *dest, \
                                ForeignType *ft) { \
  if (ttisstring(v)) { \
    size_t len; \
    const char *s = getlstr(tsvalue(v), len); \
    if (len == size / 8) { memcpy(dest, s, size / 8); return 0; } \
    if (size != 8 || len != 1) { \
      luaO_pushfstring(L, "expected a string of length %d, got length %d", \
                       (int)(size / 8), (int)len); \
      return -1; \
    } \
  } \
  return uint##size##_store(L, v, dest, ft); \
} \
static int schar##size##_store (lua_State *L, const TValue *v, void *dest, \
                                ForeignType *ft) { \
  if (ttisstring(v)) \
    return uchar##size##_store(L, v, dest, ft); \
  return int##size##_store(L, v, dest, ft); \
}
DEFINE_CHAR_GETSTORE(8)
DEFINE_CHAR_GETSTORE(16)
DEFINE_CHAR_GETSTORE(32)
DEFINE_CHAR_GETSTORE(64)

#define DEFINE_FLOAT_GETSTORE(t) \
static int t##_get (lua_State *L, void *data, ForeignType *ft, TValue *out) { \
  (void)L; (void)ft; \
  setfltvalue(out, (lua_Number)*(t *)data); \
  return 0; \
} \
static int t##_store (lua_State *L, const TValue *v, void *dest, \
                      ForeignType *ft) { \
  lua_Number n; \
  (void)ft; \
  if (!tonumber(v, &n)) { \
    luaO_pushfstring(L, "expected a number, got %s", \
                     luaT_objtypename(L, v)); \
    return -1; \
  } \
  *(t *)dest = (t)n; \
  return 0; \
}
DEFINE_FLOAT_GETSTORE(float)
DEFINE_FLOAT_GETSTORE(double)
typedef long double long_double;
DEFINE_FLOAT_GETSTORE(long_double)

#ifndef __STDC_NO_COMPLEX__
static int tocomplex_arg (lua_State *L, const TValue *v, double *re,
                          double *im) {
  lua_Number n;
  (void)L;
  if (tonumber(v, &n)) { *re = (double)n; *im = 0; return 1; }
  if (!ttistable(v)) return 0;
  {
    Table *t = hvalue(v);
    TValue rv, iv;
    lua_Number nr, ni;
    luaH_getstr(t, luaS_new(L, "re"), &rv);
    luaH_getstr(t, luaS_new(L, "im"), &iv);
    if (!tonumber(&rv, &nr) || !tonumber(&iv, &ni)) return 0;
    *re = (double)nr;
    *im = (double)ni;
    return 1;
  }
}

/* build the {re=,im=} result table; 'out' anchors it across the sets */
static void pushcomplex (lua_State *L, TValue *out, double re, double im) {
  Table *t = luaH_new(L);
  TValue k, v;
  sethvalue(L, out, t);
  setsvalue(L, &k, luaS_new(L, "re"));
  setfltvalue(&v, (lua_Number)re);
  luaH_set(L, t, &k, &v);
  setsvalue(L, &k, luaS_new(L, "im"));
  setfltvalue(&v, (lua_Number)im);
  luaH_set(L, t, &k, &v);
}

#define DEFINE_COMPLEX_GETSTORE(t) \
static int t##_get (lua_State *L, void *data, ForeignType *ft, TValue *out) { \
  complex double z = (complex double)*(t *)data; \
  (void)ft; \
  pushcomplex(L, out, creal(z), cimag(z)); \
  return 0; \
} \
static int t##_store (lua_State *L, const TValue *v, void *dest, \
                      ForeignType *ft) { \
  double re, im; \
  (void)ft; \
  if (!tocomplex_arg(L, v, &re, &im)) { \
    luaO_pushfstring(L, "expected a number or {re=,im=} table, got %s", \
                     luaT_objtypename(L, v)); \
    return -1; \
  } \
  *(t *)dest = (t)CMPLX(re, im); \
  return 0; \
}
typedef complex float cmplx_float;
DEFINE_COMPLEX_GETSTORE(cmplx_float)
typedef complex double cmplx_double;
DEFINE_COMPLEX_GETSTORE(cmplx_double)
typedef complex long double cmplx_long_double;
DEFINE_COMPLEX_GETSTORE(cmplx_long_double)
#endif

#define CHECK_SIZE(sz, get, store) \
  if (size == sz) { \
    ft->ft_get = get; \
    ft->ft_copy = get; \
    ft->ft_store = store; \
    return 0; \
  }

static int basetype_init (lua_State *L, ForeignType *ft) {
  const struct uniqtype *type = ft->ft_type;
  unsigned size = UNIQTYPE_SIZE_IN_BYTES(type);

  if (UNIQTYPE_BASE_TYPE_BIT_SIZE(type) != 8 * size)
    return pusherr(L, "unsupported foreign bit field type");

  switch (type->un.base.enc) {
    case 0:
      /* liballocs' synthetic __uninterpreted_byte: treat like unsigned
         char so untyped heap buffers still get working indexing */
      CHECK_SIZE(1, char8_get, uchar8_store)
      break;
    case DW_ATE_boolean:
      ft->ft_get = bool_get;
      ft->ft_copy = bool_get;
      ft->ft_store = bool_store;
      return 0;
    case DW_ATE_address:
    case DW_ATE_unsigned:
      CHECK_SIZE(1, uint8_get, uint8_store)
      CHECK_SIZE(2, uint16_get, uint16_store)
      CHECK_SIZE(4, uint32_get, uint32_store)
      CHECK_SIZE(8, uint64_get, uint64_store)
      break;
    case DW_ATE_signed:
      CHECK_SIZE(1, int8_get, int8_store)
      CHECK_SIZE(2, int16_get, int16_store)
      CHECK_SIZE(4, int32_get, int32_store)
      CHECK_SIZE(8, int64_get, int64_store)
      break;
    case DW_ATE_unsigned_char:
      CHECK_SIZE(1, char8_get, uchar8_store)
      CHECK_SIZE(2, char16_get, uchar16_store)
      CHECK_SIZE(4, char32_get, uchar32_store)
      CHECK_SIZE(8, char64_get, uchar64_store)
      break;
    case DW_ATE_signed_char:
      CHECK_SIZE(1, char8_get, schar8_store)
      CHECK_SIZE(2, char16_get, schar16_store)
      CHECK_SIZE(4, char32_get, schar32_store)
      CHECK_SIZE(8, char64_get, schar64_store)
      break;
    case DW_ATE_float:
      CHECK_SIZE(4, float_get, float_store)
      CHECK_SIZE(8, double_get, double_store)
      CHECK_SIZE(16, long_double_get, long_double_store)
      break;
#ifndef __STDC_NO_COMPLEX__
    case DW_ATE_complex_float:
      CHECK_SIZE(8, cmplx_float_get, cmplx_float_store)
      CHECK_SIZE(16, cmplx_double_get, cmplx_double_store)
      CHECK_SIZE(32, cmplx_long_double_get, cmplx_long_double_store)
      break;
#endif
    default:
      break;
  }
  luaO_pushfstring(L, "unsupported foreign base type '%s'",
                   UNIQTYPE_NAME(type));
  return -1;
}

static int enumtype_init (lua_State *L, ForeignType *ft) {
  const struct uniqtype *type = ft->ft_type;
  const struct uniqtype *base = UNIQTYPE_ENUM_BASE_TYPE(type);
  unsigned size;
  if (base && UNIQTYPE_IS_BASE_TYPE(base)) {
    /* the scalar handlers are size-specialised and never read ft_type,
       so borrowing them while keeping ft_type = the enum is safe */
    ForeignType tmp = *ft;
    tmp.ft_type = base;
    if (basetype_init(L, &tmp) < 0) return -1;
    ft->ft_get = tmp.ft_get;
    ft->ft_copy = tmp.ft_copy;
    ft->ft_store = tmp.ft_store;
    return 0;
  }
  size = UNIQTYPE_SIZE_IN_BYTES(type);
  CHECK_SIZE(1, int8_get, int8_store)
  CHECK_SIZE(2, int16_get, int16_store)
  CHECK_SIZE(4, int32_get, int32_store)
  CHECK_SIZE(8, int64_get, int64_store)
  return pusherr(L, "unsupported foreign enumeration size");
}

static int ischartype (const struct uniqtype *type) {
  if (!UNIQTYPE_IS_BASE_TYPE(type)) return 0;
  switch (type->un.base.enc) {
    case DW_ATE_unsigned_char:
    case DW_ATE_signed_char:
      return 1;
    default:
      return 0;
  }
}

/* ---- VOID ---- */

static int void_get (lua_State *L, void *data, ForeignType *ft, TValue *out) {
  (void)L; (void)data; (void)ft;
  setnilvalue(out);
  return 0;
}

static int void_store (lua_State *L, const TValue *v, void *dest,
                       ForeignType *ft) {
  (void)dest; (void)ft;
  if (!ttisnil(v))
    return pusherr(L, "the only accepted value for the void foreign type"
                      " is nil");
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Stack adapters (C-function contexts only)
** ===================================================================
*/

static TValue *cfslot (lua_State *L, int idx) {
  idx = lua_absindex(L, idx);
  lua_assert(idx > 0 && L->ci->func.p + idx < L->top.p);
  return s2v(L->ci->func.p + idx);
}

int luaA_pushfromdata (lua_State *L, void *data, ForeignType *ft, int copy) {
  int r;
  int (*op)(lua_State *, void *, ForeignType *, TValue *) =
      (copy && ft->ft_copy != NULL) ? ft->ft_copy : ft->ft_get;
  if (op == NULL)
    return pusherr(L, "foreign type has no read operation");
  lua_pushnil(L);  /* anchored placeholder slot for the result */
  r = op(L, data, ft, s2v(L->top.p - 1));
  if (r != 0)
    lua_remove(L, -2);  /* drop the placeholder below the message */
  return r;
}

int luaA_storestack (lua_State *L, int idx, void *dest, ForeignType *ft) {
  return ft->ft_store(L, cfslot(L, idx), dest, ft);
}

/* }================================================================== */


/*
** {==================================================================
** Foreign-representation (proxy-kind) generic vtable
** ===================================================================
*/

int luaA_istriviallycopiable (ForeignType *ft) {
  switch (ft->ft_kind) {
    case COMPOSITE:
    case ARRAY:
    case SUBPROGRAM:
      return 0;
    default:
      return 1;
  }
}

/* a value of type 'have' is acceptable where 'want' is expected: same
** type, or 'want' reached through the offset-0 first-member chain */
int luaA_typeacceptable (ForeignType *have, ForeignType *want) {
  const struct uniqtype *t;
  if (have == want) return 1;
  t = have->ft_type;
  while (UNIQTYPE_IS_COMPOSITE_TYPE(t)
         && UNIQTYPE_COMPOSITE_MEMBER_COUNT(t) > 0
         && t->related[0].un.memb.off == 0) {
    t = t->related[0].un.memb.ptr;
    if (t == want->ft_type) return 1;
  }
  return 0;
}

static size_t view_length (ForeignType *ft, void *addr);

/*
** The result of reading a foreign-representation value at 'data':
** the base Foreign itself when it is the exact answer, else an
** interior/derived view anchored to it. Writes the result to *out.
*/
static int frn_get (lua_State *L, void *data, ForeignType *ft, TValue *out) {
  Foreign *base = luaA_getorcreatebase(L, data);
  Foreign *f;
  if (base != NULL && base->ftype == ft && base->addr == data) {
    setfrnvalue(L, out, base);
    return 0;
  }
  f = luaA_newforeign(L, data, ft, view_length(ft, data),
                      (ft->ft_kind == SUBPROGRAM) ? LUA_VFFUNC : LUA_VFOREIGN);
  if (f->base == NULL && base != NULL) {
    f->base = base;
    luaC_objbarrier(L, f, base);
  }
  setfrnvalue(L, out, f);
  return 0;
}

static int frn_copy (lua_State *L, void *data, ForeignType *ft, TValue *out) {
  size_t size = UNIQTYPE_SIZE_IN_BYTES(ft->ft_type);
  void *chunk = totally_malloc(size);
  Foreign *f;
  if (chunk == NULL)
    return pusherr(L, "out of memory copying foreign value");
  if (&__liballocs_set_alloc_type != NULL)
    __liballocs_set_alloc_type(ss_translate(chunk), ft->ft_type);
  memcpy(ss_translate(chunk), ss_translate(data), size);
  f = luaA_getorcreatebase(L, chunk);  /* attaches the lifetime policy */
  totally_free(chunk);  /* the policy reference keeps the chunk alive */
  if (f == NULL)
    return pusherr(L, "cannot register copied foreign value");
  setfrnvalue(L, out, f);
  return 0;
}

static int frn_store (lua_State *L, const TValue *v, void *dest,
                      ForeignType *ft) {
  if (ttisforeign(v)) {
    Foreign *f = frnvalue(v);
    if (luaA_typeacceptable(f->ftype, ft)) {
      memcpy(ss_translate(dest), luaA_rawaddr(f),
             UNIQTYPE_SIZE_IN_BYTES(ft->ft_type));
      return 0;
    }
  }
  /* C-function-context fallback: convert through the constructor */
  if (ft->ft_ctor != NULL) {
    int top = lua_gettop(L);
    Foreign *tmp;
    setobj2s(L, L->top.p, v);
    api_incr_top(L);
    if (ft->ft_ctor(L, top + 1, ft) == 0) {
      tmp = frnvalue(s2v(L->top.p - 1));
      memcpy(ss_translate(dest), luaA_rawaddr(tmp),
             UNIQTYPE_SIZE_IN_BYTES(ft->ft_type));
      lua_settop(L, top);
      return 0;
    }
    lua_remove(L, top + 1);  /* the copied value below the message */
    return -1;
  }
  luaO_pushfstring(L, "expected value of foreign type '%s', got %s",
                   UNIQTYPE_NAME(ft->ft_type), luaT_objtypename(L, v));
  return -1;
}

static void *frn_dataptr (lua_State *L, const TValue *v, ForeignType *ft) {
  Foreign *f;
  (void)L;
  if (!ttisforeign(v)) return NULL;
  f = frnvalue(v);
  if (!luaA_typeacceptable(f->ftype, ft)) return NULL;
  return luaA_rawaddr(f);
}

/* }================================================================== */


/*
** {==================================================================
** COMPOSITE types
** ===================================================================
*/

/* find 'key' in ct's fields, walking the offset-0 first-member chain */
static ForeignField *find_field (ForeignTypeComposite *ct, const char *key,
                                 size_t *extra_off) {
  *extra_off = 0;
  while (ct != NULL) {
    unsigned i;
    for (i = 0; i < ct->nfields; i++)
      if (ct->fields[i].name && strcmp(ct->fields[i].name, key) == 0)
        return &ct->fields[i];
    if (ct->nfields > 0 && ct->fields[0].offset == 0 && ct->fields[0].type
        && ct->fields[0].type->ft_kind == COMPOSITE)
      ct = cast(ForeignTypeComposite *, ct->fields[0].type);
    else
      ct = NULL;
  }
  return NULL;
}

int luaA_findfield (ForeignType *composite_ft, const char *key,
                    ForeignType **out_ft, size_t *out_off) {
  size_t extra_off;
  ForeignField *f =
      find_field(cast(ForeignTypeComposite *, composite_ft), key, &extra_off);
  if (f == NULL) return 0;
  *out_ft = f->type;
  *out_off = extra_off + f->offset;
  return 1;
}

/* Constructor: T() zero-initialises, T(foreign) copies, T{f=...} by
** name, T(v1, v2, ...) in declaration order. Pushes the new value. */
static int composite_ctor (lua_State *L, int first_arg, ForeignType *ft) {
  ForeignTypeComposite *ct = cast(ForeignTypeComposite *, ft);
  size_t size = UNIQTYPE_SIZE_IN_BYTES(ft->ft_type);
  int nargs = lua_gettop(L) - first_arg + 1;
  void *chunk;
  Foreign *f;
  void *raw;
  int result_idx;
  if (nargs < 0) nargs = 0;

  chunk = totally_malloc(size);
  if (chunk == NULL)
    return pusherr(L, "out of memory constructing foreign struct");
  if (&__liballocs_set_alloc_type != NULL)
    __liballocs_set_alloc_type(ss_translate(chunk), ft->ft_type);
  f = luaA_getorcreatebase(L, chunk);
  totally_free(chunk);  /* kept alive by the policy reference */
  if (f == NULL)
    return pusherr(L, "cannot register constructed foreign struct");
  luaA_pushforeign(L, f);
  result_idx = lua_gettop(L);
  raw = luaA_rawaddr(f);

  if (nargs == 0) {
    memset(raw, 0, size);
    return 0;
  }

  if (nargs == 1) {
    Foreign *other = luaA_toforeign(L, first_arg);
    if (other != NULL && other->ftype == ft) {
      if (other->addr == f->addr) {
        lua_remove(L, result_idx);
        return pusherr(L, "trying to copy inside yourself");
      }
      memcpy(raw, luaA_rawaddr(other), size);
      return 0;
    }
    if (lua_type(L, first_arg) == LUA_TTABLE) {
      unsigned i;
      for (i = 0; i < ct->nfields; i++) {
        ForeignField *fld = &ct->fields[i];
        if (fld->type == NULL) continue;
        lua_getfield(L, first_arg, fld->name);
        if (!lua_isnil(L, -1)) {
          if (fld->type->ft_store(L, cfslot(L, -1),
                                  (char *)raw + fld->offset, fld->type)) {
            lua_remove(L, -2);         /* the fetched value */
            lua_remove(L, result_idx); /* the half-built value */
            return -1;
          }
        }
        else
          memset((char *)raw + fld->offset, 0,
                 UNIQTYPE_SIZE_IN_BYTES(fld->type->ft_type));
        lua_pop(L, 1);
      }
      return 0;
    }
  }

  /* positional arguments: one per field in declaration order */
  if ((unsigned)nargs > ct->nfields) {
    lua_remove(L, result_idx);
    luaO_pushfstring(L, "too many arguments (foreign type '%s' has %d fields)",
                     UNIQTYPE_NAME(ft->ft_type), (int)ct->nfields);
    return -1;
  }
  {
    unsigned i;
    for (i = 0; i < ct->nfields; i++) {
      ForeignField *fld = &ct->fields[i];
      if (fld->type == NULL) {
        if ((int)i < nargs) {
          lua_remove(L, result_idx);
          luaO_pushfstring(L, "field '%s' has an unhandled foreign datatype",
                           ct->fields[i].name);
          return -1;
        }
        continue;
      }
      if ((int)i < nargs) {
        if (fld->type->ft_store(L, cfslot(L, first_arg + (int)i),
                                (char *)raw + fld->offset, fld->type)) {
          lua_remove(L, result_idx);
          return -1;
        }
      }
      else
        memset((char *)raw + fld->offset, 0,
               UNIQTYPE_SIZE_IN_BYTES(fld->type->ft_type));
    }
  }
  return 0;
}

static ForeignType *composite_newtype (lua_State *L,
                                       const struct uniqtype *type) {
  unsigned nfields = UNIQTYPE_COMPOSITE_MEMBER_COUNT(type);
  const char **field_names = UNIQTYPE_COMPOSITE_SUBOBJ_NAMES(type);
  ForeignTypeComposite *ct;
  unsigned i;
  (void)L;
  if (field_names == NULL) nfields = 0;  /* incomplete struct */
  ct = cast(ForeignTypeComposite *, calloc(1, sizeof(*ct)));
  ct->ft.ft_type = type;
  ct->ft.ft_kind = COMPOSITE;
  ct->ft.ft_frn = 1;
  ct->ft.ft_ctor = composite_ctor;
  ct->ft.ft_get = frn_get;
  ct->ft.ft_copy = frn_copy;
  ct->ft.ft_store = frn_store;
  ct->ft.ft_dataptr = frn_dataptr;
  ct->nfields = nfields;
  ct->fields = cast(ForeignField *,
                    calloc(nfields ? nfields : 1, sizeof(ForeignField)));
  for (i = 0; i < nfields; i++)
    ct->fields[i].name = field_names[i];
  return &ct->ft;
}

static void composite_inittype (lua_State *L, ForeignType *ft) {
  ForeignTypeComposite *ct = cast(ForeignTypeComposite *, ft);
  const struct uniqtype *type = ft->ft_type;
  unsigned i;
  for (i = 0; i < ct->nfields; i++) {
    const struct uniqtype_rel_info *rel = &type->related[i];
    ct->fields[i].offset = rel->un.memb.off;
    ct->fields[i].type = luaA_gettype(L, rel->un.memb.ptr);
    if (ct->fields[i].type == NULL)
      lua_pop(L, 1);  /* drop error msg; field stays invalid */
  }
}

/* }================================================================== */


/*
** {==================================================================
** ADDRESS and ARRAY types
** ===================================================================
*/

static size_t addr_elemsize (ForeignTypeAddress *at) {
  return at->pointee ? UNIQTYPE_SIZE_IN_BYTES(at->pointee->ft_type) : 0;
}

/*
** Length inference for a view of address/array class at 'addr' (the
** target address, already the element pointer): liballocs bounds, then
** a NUL-scan for char pointees capped by whatever bound exists, then 1.
** Port of addrproxy_initlength; deterministic per (addr, ft), which is
** what makes it sound as part of the intern key.
*/
static size_t addr_viewlength (ForeignTypeAddress *at, void *addr) {
  const struct uniqtype *ptyp;
  void *ptr;
  AllocsBounds bounds;
  unsigned long esz;
  if (at->pointee == NULL)
    return 0;
  ptyp = at->pointee->ft_type;
  if (UNIQTYPE_SIZE_IN_BYTES(ptyp) == 0 || !UNIQTYPE_HAS_KNOWN_LENGTH(ptyp))
    return 0;
  ptr = ss_translate(addr);
  bounds = luaA_fetchbounds(ptr, ptyp);
  esz = UNIQTYPE_SIZE_IN_BYTES(ptyp);
  if ((void *)bounds.base == ptr && bounds.size != 1)
    return bounds.size / esz;
  else if (ischartype(ptyp) && esz == 1) {
    unsigned long cap = 0;
    if (bounds.base && bounds.size > 1
        && (char *)ptr >= (char *)bounds.base
        && (char *)ptr < (char *)bounds.base + bounds.size)
      cap = bounds.size - (unsigned long)((char *)ptr - (char *)bounds.base);
    if (cap) {
      unsigned long n = 0;
      while (n < cap && ((const char *)ptr)[n] != '\0') n++;
      return (size_t)n;
    }
    return 1;
  }
  return 1;  /* interior pointer to a single element */
}

/* length used when interning a view of type 'ft' at 'addr' */
static size_t view_length (ForeignType *ft, void *addr) {
  switch (ft->ft_kind) {
    case ADDRESS:
      return addr_viewlength(cast(ForeignTypeAddress *, ft), addr);
    case ARRAY:
      if (UNIQTYPE_HAS_KNOWN_LENGTH(ft->ft_type))
        return UNIQTYPE_ARRAY_LENGTH(ft->ft_type);
      return addr_viewlength(cast(ForeignTypeAddress *, ft), addr);
    default:
      return 0;
  }
}

/* is 'f' of the same address class as 'at' (pointer/array/slice over
** the same element type)? The native equivalent of the extension's
** shared-metatable-name test. */
static int addrclass_matches (Foreign *f, ForeignTypeAddress *at) {
  ForeignTypeAddress *fat;
  if (f->ftype->ft_kind != ADDRESS && f->ftype->ft_kind != ARRAY)
    return 0;
  fat = cast(ForeignTypeAddress *, f->ftype);
  return fat->pointee != NULL && fat->pointee == at->pointee;
}

static int addr_typecheck (Foreign *f, ForeignTypeAddress *at) {
  if (f == NULL) return 0;
  if (addrclass_matches(f, at)) return 1;
  if (at->pointee == NULL) return 0;
  if (at->pointee->ft_kind == VOID) return 1;  /* void* accepts anything */
  return luaA_typeacceptable(f->ftype, at->pointee);
}

/* dereference: 'data' is the address of a pointer */
static int addr_get (lua_State *L, void *data, ForeignType *ft, TValue *out) {
  ForeignTypeAddress *at = cast(ForeignTypeAddress *, ft);
  void *ptr = *(void **)data;
  Foreign *base;
  Foreign *f;
  if (ptr == NULL) {
    setnilvalue(out);
    return 0;
  }
  base = luaA_getorcreatebase(L, ptr);
  if (base != NULL) {
    /* prefer the base when that does not break semantics */
    if (at->pointee != NULL && at->pointee->ft_kind == VOID) {
      setfrnvalue(L, out, base);
      return 0;
    }
    if (base->addr == ptr && at->pointee != NULL
        && (addrclass_matches(base, at)
            || luaA_typeacceptable(base->ftype, at->pointee))) {
      setfrnvalue(L, out, base);
      return 0;
    }
  }
  f = luaA_newforeign(L, ptr, ft, view_length(ft, ptr), LUA_VFOREIGN);
  if (f->base == NULL && base != NULL) {
    f->base = base;
    luaC_objbarrier(L, f, base);
  }
  setfrnvalue(L, out, f);
  return 0;
}

static int addr_store (lua_State *L, const TValue *v, void *dest,
                       ForeignType *ft) {
  ForeignTypeAddress *at = cast(ForeignTypeAddress *, ft);
  Foreign *f;
  void *val;

  if (ttisnil(v)) {  /* nil -> NULL */
    ss_dec_refcount(*(void **)dest);
    __notify_ptr_write((const void **)dest, NULL);
    *(void **)dest = NULL;
    return 0;
  }

  f = ttisforeign(v) ? frnvalue(v) : NULL;
  if (!addr_typecheck(f, at)) {
    luaO_pushfstring(L, "expected reference to object of type %s, got %s",
                     at->pointee ? UNIQTYPE_NAME(at->pointee->ft_type) : "?",
                     luaT_objtypename(L, v));
    return -1;
  }

  val = f->addr;
  /* the exact barrier ordering of the extension: drop the old
     pointee's refs, take the new one's, tell liballocs, then store */
  ss_dec_refcount(*(void **)dest);
  ss_inc_refcount(val);
  __notify_ptr_write((const void **)dest, val);
  *(void **)dest = val;
  if (!ss_is_handle(val))
    luaA_retainstoredptr(L, f, (const void **)dest, val);
  return 0;
}

static void *addr_dataptr (lua_State *L, const TValue *v, ForeignType *ft) {
  ForeignTypeAddress *at = cast(ForeignTypeAddress *, ft);
  Foreign *f;
  (void)L;

  if (ttisnil(v)) {
    static void *np = NULL;
    return &np;
  }
  if (!ttisforeign(v)) return NULL;
  f = frnvalue(v);
  if (addr_typecheck(f, at))
    return &f->addr;

  /* one-element pointer cell passed where T* is expected: hand over
     the pointer stored IN the cell, so a cell can be a T** out-param
     first and the resulting T* handle afterwards */
  if (f->length == 1
      && (f->ftype->ft_kind == ADDRESS || f->ftype->ft_kind == ARRAY)) {
    ForeignTypeAddress *fat = cast(ForeignTypeAddress *, f->ftype);
    if (fat->pointee == &at->ft
        || (fat->pointee != NULL && fat->pointee->ft_type == ft->ft_type))
      return f->addr;
  }
  return NULL;
}

static ForeignType *address_newtype (lua_State *L,
                                     const struct uniqtype *type) {
  const struct uniqtype *pointee = type->related[0].un.t.ptr;
  ForeignTypeAddress *at;
  if (pointee == NULL) {
    pusherr(L, "pointer type with no pointee");
    return NULL;
  }
  at = cast(ForeignTypeAddress *, calloc(1, sizeof(*at)));
  at->ft.ft_type = type;
  at->ft.ft_kind = ADDRESS;
  at->ft.ft_frn = 1;
  at->ft.ft_get = addr_get;
  at->ft.ft_copy = addr_get;
  at->ft.ft_store = addr_store;
  at->ft.ft_dataptr = addr_dataptr;
  at->ft.ft_ctor = NULL;  /* set to cell ctor in inittype */
  return &at->ft;
}

/* forward declarations of ctors defined below */
static int cell_ctor (lua_State *L, int first_arg, ForeignType *ft);
static int array_ctor (lua_State *L, int first_arg, ForeignType *ft);

static void address_inittype (lua_State *L, ForeignType *ft) {
  ForeignTypeAddress *at = cast(ForeignTypeAddress *, ft);
  const struct uniqtype *pointee = ft->ft_type->related[0].un.t.ptr;
  at->pointee = luaA_gettype(L, pointee);
  if (at->pointee == NULL)
    lua_pop(L, 1);  /* degraded (opaque, length-0) class */
  /* T_ptr(): a fresh NULL-initialised one-element pointer cell (a T*
     slot), passable wherever C expects T** -- the out-param idiom */
  at->ft.ft_ctor = cell_ctor;
}

/* fill [1..len] from a Lua sequence (or zero-init when tbl_idx == 0) */
static int array_fill (lua_State *L, Foreign *f, int tbl_idx) {
  ForeignTypeAddress *at = cast(ForeignTypeAddress *, f->ftype);
  size_t esz = addr_elemsize(at);
  void *raw = luaA_rawaddr(f);
  lua_Integer arglen;
  lua_Integer i;

  if (!tbl_idx) {
    memset(raw, 0, f->length * esz);
    return 0;
  }
  arglen = (lua_Integer)lua_rawlen(L, tbl_idx);
  if (arglen > (lua_Integer)f->length) {
    luaO_pushfstring(L, "sequence too long to be stored at this address"
                        " (only %d items)", (int)f->length);
    return -1;
  }
  for (i = 1; i <= arglen; i++) {
    int r;
    lua_geti(L, tbl_idx, i);
    r = at->pointee->ft_store(L, cfslot(L, -1),
                              (char *)raw + (size_t)(i - 1) * esz,
                              at->pointee);
    if (r) { lua_remove(L, -2); return -1; }
    lua_pop(L, 1);
  }
  if ((size_t)arglen < f->length)
    memset((char *)raw + (size_t)arglen * esz, 0,
           (f->length - (size_t)arglen) * esz);
  return 0;
}

/* array construction from Lua: T_array(n) or T_array{v1, v2, ...} */
static int array_ctor (lua_State *L, int first_arg, ForeignType *ft) {
  ForeignTypeAddress *at = cast(ForeignTypeAddress *, ft);
  int nargs = lua_gettop(L) - first_arg + 1;
  lua_Integer len;
  int tbl_idx = 0;
  size_t esz;
  int is_char;
  void *chunk;
  Foreign *f;

  if (at->pointee == NULL)
    return pusherr(L, "array element foreign datatype is not handled");
  if (nargs != 1)
    return pusherr(L, "array constructor takes exactly one argument"
                      " (length or sequence)");

  if (lua_type(L, first_arg) == LUA_TTABLE) {
    len = (lua_Integer)lua_rawlen(L, first_arg);
    tbl_idx = first_arg;
  }
  else if (lua_isinteger(L, first_arg))
    len = lua_tointeger(L, first_arg);
  else
    return pusherr(L, "array constructor argument must be a table or an"
                      " integer length");
  if (len < 0)
    return pusherr(L, "array length cannot be negative");

  esz = addr_elemsize(at);
  is_char = ischartype(at->pointee->ft_type);
  /* char arrays get one extra NUL element so C string readers stay in
     bounds (mirror of the extension's hack) */
  chunk = totally_malloc(((size_t)len + (is_char ? 1 : 0)) * esz);
  if (chunk == NULL)
    return pusherr(L, "out of memory constructing foreign array");
  if (is_char) memset((char *)chunk + (size_t)len * esz, 0, esz);
  if (&__liballocs_set_alloc_type != NULL
      && &__liballocs_get_or_create_array_type != NULL)
    __liballocs_set_alloc_type(ss_translate(chunk),
        __liballocs_get_or_create_array_type(
            (struct uniqtype *)at->pointee->ft_type, (unsigned)len));

  f = luaA_getorcreatebase(L, chunk);
  totally_free(chunk);  /* kept alive by the policy reference */
  if (f == NULL)
    return pusherr(L, "cannot register constructed foreign array");
  luaA_pushforeign(L, f);

  if (array_fill(L, f, tbl_idx)) {
    lua_remove(L, -2);  /* the half-built value below the message */
    return -1;
  }
  return 0;
}

static int array_storeinto (lua_State *L, const TValue *v, void *dest,
                            ForeignType *ft) {
  (void)v; (void)dest; (void)ft;
  return pusherr(L, "cannot copy full arrays (for now)");
}

static ForeignType *array_newtype (lua_State *L, const struct uniqtype *type) {
  ForeignTypeAddress *at = cast(ForeignTypeAddress *, calloc(1, sizeof(*at)));
  (void)L;
  at->ft.ft_type = type;
  at->ft.ft_kind = ARRAY;
  at->ft.ft_frn = 1;
  at->ft.ft_ctor = array_ctor;
  at->ft.ft_get = frn_get;
  at->ft.ft_copy = NULL;
  at->ft.ft_store = array_storeinto;
  at->ft.ft_dataptr = frn_dataptr;
  return &at->ft;
}

static void array_inittype (lua_State *L, ForeignType *ft) {
  ForeignTypeAddress *at = cast(ForeignTypeAddress *, ft);
  const struct uniqtype *elem = ft->ft_type->related[0].un.t.ptr;
  const struct uniqtype *addr_uniqtype;
  ForeignType *addr_ft;
  if (elem == NULL) return;
  /* arrays share the pointer-to-element class: same pointee, so cells
     pass where C expects a pointer to the element */
  addr_uniqtype = (&__liballocs_get_or_create_address_type != NULL)
      ? __liballocs_get_or_create_address_type(elem) : NULL;
  addr_ft = addr_uniqtype ? luaA_gettype(L, addr_uniqtype) : NULL;
  if (addr_ft == NULL) {
    if (addr_uniqtype) lua_pop(L, 1);
    at->pointee = NULL;
    return;
  }
  at->pointee = cast(ForeignTypeAddress *, addr_ft)->pointee;
}

/* one-element out-parameter cells */
int luaA_newcell (lua_State *L, ForeignType *elem_ft, int init_idx) {
  struct uniqtype *arrtype;
  ForeignType *arr_ft;
  if (init_idx) init_idx = lua_absindex(L, init_idx);
  arrtype = (&__liballocs_get_or_create_flexible_array_type != NULL)
      ? __liballocs_get_or_create_flexible_array_type(
            (struct uniqtype *)elem_ft->ft_type)
      : NULL;
  if (arrtype == NULL) {
    luaO_pushfstring(L, "cannot create a cell of type '%s'",
                     UNIQTYPE_NAME(elem_ft->ft_type));
    return -1;
  }
  arr_ft = luaA_gettype(L, arrtype);
  if (arr_ft == NULL) return -1;  /* error message already pushed */
  if (arr_ft->ft_ctor == NULL) {
    luaO_pushfstring(L, "cell type '%s' is not constructible",
                     UNIQTYPE_NAME(arrtype));
    return -1;
  }

  lua_pushinteger(L, 1);  /* a zero-initialised one-element array... */
  if (arr_ft->ft_ctor(L, lua_gettop(L), arr_ft)) {
    lua_remove(L, -2);  /* the pushed length below the error message */
    return -1;
  }
  lua_remove(L, -2);  /* the pushed length */

  if (init_idx) {  /* ...optionally initialised from the given value */
    Foreign *f = frnvalue(s2v(L->top.p - 1));
    if (elem_ft->ft_store(L, cfslot(L, init_idx), luaA_rawaddr(f), elem_ft)) {
      lua_remove(L, -2);  /* the half-built cell */
      return -1;
    }
  }
  return 0;
}

static int cell_ctor (lua_State *L, int first_arg, ForeignType *ft) {
  int nargs = lua_gettop(L) - first_arg + 1;
  ForeignType *elem;
  if (nargs < 0) nargs = 0;
  if (nargs > 1)
    return pusherr(L, "cell constructor takes at most one argument");
  /* for scalar types the cell element is the type itself; for pointer
     types likewise (a T* cell = T** storage) */
  elem = ft;
  return luaA_newcell(L, elem, nargs == 1 ? first_arg : 0);
}

/* }================================================================== */


/*
** {==================================================================
** uniqtype -> ForeignType dispatch and memoisation
** ===================================================================
** Memo policy (copied from the extension): structural kinds (BASE,
** ADDRESS, ARRAY) can exist as several identical uniqtype instances in
** one process, so they are interned by UNIQTYPE_NAME string; nominal
** kinds (COMPOSITE, SUBPROGRAM) keep pointer identity. Types are
** immortal (the memo never evicts).
**
** Two-phase creation: the memo entry is stored BEFORE Init recurses
** into member/pointee types, so recursive types terminate.
*/

/* string-keyed memo for structural kinds (never evicts, no tombstones) */
typedef struct StrMapEntry {
  const char *name;  /* strdup'd */
  ForeignType *ft;
} StrMapEntry;

static struct {
  StrMapEntry *tab;
  size_t size;
  size_t live;
} g_strmemo;

static size_t strhash (const char *s) {
  size_t h = 5381;
  while (*s) h = h * 33 + (unsigned char)*s++;
  return h;
}

static StrMapEntry *strmemofind (const char *name) {
  size_t mask = g_strmemo.size - 1;
  size_t i = strhash(name) & mask;
  for (;;) {
    StrMapEntry *e = &g_strmemo.tab[i];
    if (e->name == NULL || strcmp(e->name, name) == 0)
      return e;
    i = (i + 1) & mask;
  }
}

static void strmemoinsert (lua_State *L, const char *name, ForeignType *ft) {
  StrMapEntry *e;
  if (g_strmemo.size == 0 || g_strmemo.live * 4 >= g_strmemo.size * 3) {
    size_t newsize = g_strmemo.size ? g_strmemo.size * 2 : 64;
    StrMapEntry *newtab =
        cast(StrMapEntry *, calloc(newsize, sizeof(StrMapEntry)));
    StrMapEntry *oldtab = g_strmemo.tab;
    size_t oldsize = g_strmemo.size;
    size_t i;
    if (newtab == NULL) luaM_error(L);
    g_strmemo.tab = newtab;
    g_strmemo.size = newsize;
    for (i = 0; i < oldsize; i++)
      if (oldtab[i].name != NULL)
        *strmemofind(oldtab[i].name) = oldtab[i];
    free(oldtab);
  }
  e = strmemofind(name);
  if (e->name == NULL) {
    e->name = strdup(name);
    e->ft = ft;
    g_strmemo.live++;
  }
}

static int is_structural_kind (const struct uniqtype *type) {
  switch (UNIQTYPE_KIND(type)) {
    case BASE: case ADDRESS: case ARRAY:
      return UNIQTYPE_NAME(type) != NULL;
    default:
      return 0;
  }
}

static ForeignType *type_new (lua_State *L, const struct uniqtype *type) {
  switch (UNIQTYPE_KIND(type)) {
    case VOID: {
      ForeignType *ft = cast(ForeignType *, calloc(1, sizeof(*ft)));
      ft->ft_type = type;
      ft->ft_kind = VOID;
      ft->ft_get = void_get;
      ft->ft_copy = void_get;
      ft->ft_store = void_store;
      return ft;
    }
    case BASE: {
      ForeignType *ft = cast(ForeignType *, calloc(1, sizeof(*ft)));
      ft->ft_type = type;
      ft->ft_kind = BASE;
      if (basetype_init(L, ft) < 0) { free(ft); return NULL; }
      /* allocs.int() / allocs.int(5): a one-element out-param cell */
      ft->ft_ctor = cell_ctor;
      return ft;
    }
    case ENUMERATION: {
      ForeignType *ft = cast(ForeignType *, calloc(1, sizeof(*ft)));
      ft->ft_type = type;
      ft->ft_kind = ENUMERATION;
      if (enumtype_init(L, ft) < 0) { free(ft); return NULL; }
      return ft;
    }
    case COMPOSITE:
      return composite_newtype(L, type);
    case ADDRESS:
      return address_newtype(L, type);
    case ARRAY:
      return array_newtype(L, type);
    case SUBPROGRAM: {
      ForeignTypeFunction *ft =
          cast(ForeignTypeFunction *, calloc(1, sizeof(*ft)));
      ft->ft.ft_type = type;
      ft->ft.ft_kind = SUBPROGRAM;
      ft->ft.ft_frn = 1;
      ft->ft.ft_ctor = luaA_closurector;
      ft->ft.ft_get = frn_get;
      ft->ft.ft_copy = NULL;  /* a function has no by-value copy */
      ft->ft.ft_store = frn_store;
      ft->ft.ft_dataptr = frn_dataptr;
      return &ft->ft;
    }
    default:
      luaO_pushfstring(L, "cannot create foreign type for '%s'",
                       UNIQTYPE_NAME(type));
      return NULL;
  }
}

static void type_init (lua_State *L, ForeignType *ft) {
  switch (ft->ft_kind) {
    case COMPOSITE:
      composite_inittype(L, ft);
      break;
    case ADDRESS:
      address_inittype(L, ft);
      break;
    case ARRAY:
      array_inittype(L, ft);
      break;
    default:
      break;
  }
}

ForeignType *luaA_gettype (lua_State *L, const struct uniqtype *type) {
  AllocsState *as = luaA_getstate(L);
  ForeignType *ft;
  int structural;
  const char *name = NULL;

  if (type == NULL)
    return (pusherr(L, "luaA_gettype called with NULL uniqtype"), NULL);

  structural = is_structural_kind(type);
  if (structural) {
    name = UNIQTYPE_NAME(type);
    if (g_strmemo.size != 0) {
      StrMapEntry *e = strmemofind(name);
      if (e->name != NULL) return e->ft;
    }
  }
  else {
    ft = cast(ForeignType *, luaA_mapget(&as->typemap, type));
    if (ft != NULL) return ft;
  }

  ft = type_new(L, type);
  if (ft == NULL) return NULL;  /* error message pushed */
  /* memo BEFORE init recurses (recursion termination) */
  if (structural)
    strmemoinsert(L, name, ft);
  else
    luaA_mapset(L, &as->typemap, type, ft);
  type_init(L, ft);
  return ft;
}

/* }================================================================== */


/*
** {==================================================================
** Base Foreigns, lifetime, release
** ===================================================================
*/

/*
** When a chunk first becomes Lua-known, drop any external-root
** entries whose slot lies inside it: they were classified "external"
** before the chunk had a base Foreign, and leaving them would pin
** cycles (their edges are now discovered by the traversal instead).
*/
static void purge_roots_in (AllocsState *as, const char *start,
                            size_t size) {
  size_t i;
  if (size == 0 || as->roots.size == 0) return;
  for (i = 0; i < as->roots.size; i++) {
    AllocsMapEntry *e = &as->roots.tab[i];
    if (e->key != NULL && e->key != LUAA_DEADKEY) {
      const char *k = cast(const char *, e->key);
      if (k >= start && k < start + size) {
        e->key = LUAA_DEADKEY;
        e->val = NULL;
        as->roots.live--;
      }
    }
  }
}

Foreign *luaA_getorcreatebase (lua_State *L, void *addr) {
  AllocsState *as = luaA_getstate(L);
  struct allocator *allocator = NULL;
  const void *alloc_start = NULL;
  unsigned long alloc_size = 0;
  struct uniqtype *alloc_type = NULL;
  void *raw = ss_translate(addr);
  void *base_addr;
  ForeignType *ft;
  Foreign *f;
  struct liballocs_err *err;

  f = cast(Foreign *, luaA_mapget(&as->basemap, addr));
  if (f != NULL) {  /* fast path: exact hit on a known base */
    if (isdead(G(L), obj2gco(f)))
      changewhite(obj2gco(f));
    return f;
  }

  err = __liballocs_get_alloc_info(raw, &allocator, &alloc_start,
                                   &alloc_size, &alloc_type, NULL);
  if (err || !allocator || !ALLOCATOR_HANDLE_LIFETIME_INSERT(allocator)
      || !alloc_type)
    return NULL;

  /* liballocs answers in raw address space; recover the addresslike */
  base_addr = (char *)addr - ((char *)raw - (char *)alloc_start);

  f = cast(Foreign *, luaA_mapget(&as->basemap, base_addr));
  if (f != NULL) {
    if (isdead(G(L), obj2gco(f)))
      changewhite(obj2gco(f));
    return f;
  }

  ft = luaA_gettype(L, alloc_type);
  if (ft == NULL) { lua_pop(L, 1); return NULL; }  /* pop error message */
  if (!ft->ft_frn) return NULL;  /* no foreign representation (scalars) */

  f = luaA_newforeign(L, base_addr, ft, view_length(ft, base_addr),
                      (ft->ft_kind == SUBPROGRAM) ? LUA_VFFUNC : LUA_VFOREIGN);
  if (!(f->flags & FF_BASE)) {
    f->flags |= FF_BASE;
    chunkref_inc(base_addr);
    luaA_mapset(L, &as->basemap, base_addr, f);
    purge_roots_in(as, cast(const char *, base_addr), (size_t)alloc_size);
    /* make the GC pacer see the adopted C memory (the debt resets at
       the end of each cycle, so this only accelerates the next one) */
    if (alloc_size > 0)
      luaE_setdebt(G(L), G(L)->GCdebt + (l_mem)alloc_size);
  }
  return f;
}


/* after a pointer store, keep the stored-to base Foreign alive while
** the slot references it, covering values liballocs cannot track (ffi
** closure trampolines); skip when the GC policy already roots it */
void luaA_retainstoredptr (lua_State *L, Foreign *f, const void **dest,
                           const void *val) {
  AllocsState *as = luaA_getstate(L);
  Foreign *basef;
  if (val == NULL || f->addr != (void *)val) return;
  if (luaA_mapget(&as->roots, dest) != NULL)
    return;  /* the GC policy already manages this slot */
  if (slot_is_lua_known(as, dest))
    return;  /* internal edge: the GC traversal covers it */
  basef = cast(Foreign *, luaA_mapget(&as->basemap, val));
  if (basef == f)
    luaA_mapset(L, &as->roots, dest, f);
}


/* recursive walk clearing every tracked pointer slot in a region */
static void clear_tracked_slots (AllocsState *as, void *region,
                                 struct uniqtype *t) {
  if (t == NULL) return;
  if (UNIQTYPE_IS_POINTER_TYPE(t)) {
    const void **slot = (const void **)region;
    if (*slot) {
      lp_delref(NULL, slot);
      *slot = NULL;
    }
  }
  else if (UNIQTYPE_IS_ARRAY_TYPE(t)) {
    struct uniqtype *et = UNIQTYPE_ARRAY_ELEMENT_TYPE(t);
    unsigned long esz;
    unsigned n, i;
    if (et == NULL) return;
    esz = UNIQTYPE_SIZE_IN_BYTES(et);
    n = UNIQTYPE_ARRAY_LENGTH(t);
    for (i = 0; i < n; i++)
      clear_tracked_slots(as, (char *)region + i * esz, et);
  }
  else if (UNIQTYPE_IS_COMPOSITE_TYPE(t)) {
    unsigned nmemb = UNIQTYPE_COMPOSITE_MEMBER_COUNT(t);
    unsigned i;
    for (i = 0; i < nmemb; i++)
      clear_tracked_slots(as, (char *)region + t->related[i].un.memb.off,
                          t->related[i].un.memb.ptr);
  }
}

void luaA_release (lua_State *L, Foreign *f) {
  AllocsState *as = luaA_getstate(L);
  void *raw;
  struct uniqtype *t;
  if (f->flags & FF_RELEASED) return;  /* idempotent */

  raw = ss_translate(f->addr);
  t = ss_alloc_get_type(f->addr);
  if (t == NULL) t = (struct uniqtype *)f->ftype->ft_type;
  clear_tracked_slots(as, raw, t);

  if (f->flags & FF_BASE) {
    luaA_mapremove(&as->basemap, f->addr);
    chunkref_dec(f->addr);
  }
  /* drop the intern entry so a fresh Foreign can emerge for this
     address if it is re-typed later */
  {
    AllocsInternEntry *e = internfind(as, f->addr, f->ftype, f->length);
    if (e->f == f) {
      e->f = LUAA_DEADENTRY;
      as->internlive--;
    }
  }
  /* the backing chunk may be gone now; poison so later access raises */
  f->flags |= FF_RELEASED;
}

/* }================================================================== */


/*
** {==================================================================
** VM integration: native get/set/len
** ===================================================================
*/

const char *luaA_foreigntypename (lua_State *L, Foreign *f) {
  (void)L;
  if (f->ftype == NULL || f->ftype->ft_type == NULL) return NULL;
  return UNIQTYPE_NAME(f->ftype->ft_type);
}

#define checkreleased(L,f) \
  { if ((f)->flags & FF_RELEASED) \
      luaG_runerror(L, "access to a released foreign proxy"); }

/*
** t[key] for a foreign 't'. Writes the result to 'val' and returns its
** tag, or LUAA_NOTFOUND to fall through to the __index metamethod path
** (which serves methods and raises the parity error messages).
*/
int luaA_foreignget (lua_State *L, const TValue *t, const TValue *key,
                     StkId val) {
  Foreign *f = frnvalue(t);
  ForeignType *ft = f->ftype;
  if (ft == NULL) return LUAA_NOTFOUND;

  switch (ft->ft_kind) {
    case COMPOSITE: {
      const char *k;
      ForeignType *fft;
      size_t off;
      if (!ttisstring(key)) {
        if (f->flags & FF_RELEASED) return LUAA_NOTFOUND;
        luaG_runerror(L, "foreign struct fields are accessed by name");
      }
      k = getstr(tsvalue(key));
      if (!luaA_findfield(ft, k, &fft, &off))
        return LUAA_NOTFOUND;  /* maybe a method; else __index raises */
      checkreleased(L, f);
      if (fft == NULL)
        luaG_runerror(L, "field '%s': foreign datatype is not handled", k);
      if (fft->ft_get(L, (char *)luaA_rawaddr(f) + off, fft, s2v(val)))
        luaG_errormsg(L);  /* error message pushed by the vtable op */
      return ttypetag(s2v(val));
    }
    case ADDRESS:
    case ARRAY: {
      ForeignTypeAddress *at = cast(ForeignTypeAddress *, ft);
      if (ttisinteger(key)) {
        lua_Integer i = ivalue(key);
        checkreleased(L, f);
        if (at->pointee == NULL)
          luaG_runerror(L, "pointee foreign datatype is not handled");
        if (i < 1 || (size_t)i > f->length) {
          /* out-of-range reads yield nil so ipairs terminates */
          setnilvalue(s2v(val));
          return LUA_VNIL;
        }
        if (at->pointee->ft_get(L,
                (char *)luaA_rawaddr(f) + (size_t)(i - 1) * addr_elemsize(at),
                at->pointee, s2v(val)))
          luaG_errormsg(L);
        return ttypetag(s2v(val));
      }
      if (ttisstring(key) && at->pointee != NULL
          && at->pointee->ft_kind == COMPOSITE) {
        /* direct field access on a composite pointee */
        const char *k = getstr(tsvalue(key));
        ForeignType *fft;
        size_t off;
        if (!luaA_findfield(at->pointee, k, &fft, &off))
          return LUAA_NOTFOUND;
        checkreleased(L, f);
        if (fft == NULL)
          luaG_runerror(L, "field '%s': foreign datatype is not handled", k);
        if (fft->ft_get(L, (char *)luaA_rawaddr(f) + off, fft, s2v(val)))
          luaG_errormsg(L);
        return ttypetag(s2v(val));
      }
      return LUAA_NOTFOUND;
    }
    default:
      return LUAA_NOTFOUND;
  }
}

/*
** t[key] = val for a foreign 't'. Returns 0 on success or
** LUAA_NOTFOUND to fall through to __newindex (which raises the parity
** error messages).
*/
int luaA_foreignset (lua_State *L, const TValue *t, const TValue *key,
                     TValue *val) {
  Foreign *f = frnvalue(t);
  ForeignType *ft = f->ftype;
  if (ft == NULL) return LUAA_NOTFOUND;

  switch (ft->ft_kind) {
    case COMPOSITE: {
      const char *k;
      ForeignType *fft;
      size_t off;
      if (!ttisstring(key))
        luaG_runerror(L, "foreign struct fields are accessed by name");
      checkreleased(L, f);
      k = getstr(tsvalue(key));
      if (!luaA_findfield(ft, k, &fft, &off))
        luaG_runerror(L, "no field '%s' in foreign type '%s'",
                      k, UNIQTYPE_NAME(ft->ft_type));
      if (fft == NULL)
        luaG_runerror(L, "field '%s': foreign datatype is not handled", k);
      if (!luaA_istriviallycopiable(fft))
        luaG_runerror(L, "cannot assign to field '%s' (not trivially"
                         " copiable)", k);
      if (fft->ft_store(L, val, (char *)luaA_rawaddr(f) + off, fft))
        luaG_errormsg(L);
      return 0;
    }
    case ADDRESS:
    case ARRAY: {
      ForeignTypeAddress *at = cast(ForeignTypeAddress *, ft);
      if (ttisinteger(key)) {
        lua_Integer i = ivalue(key);
        checkreleased(L, f);
        if (at->pointee == NULL)
          luaG_runerror(L, "pointee foreign datatype is not handled");
        if (!luaA_istriviallycopiable(at->pointee))
          luaG_runerror(L, "cannot assign elements of this type (not"
                           " trivially copiable)");
        if (i < 1 || (size_t)i > f->length)
          luaG_runerror(L, "index %I out of range (length %I)",
                        (LUAI_UACINT)i, (LUAI_UACINT)f->length);
        if (at->pointee->ft_store(L, val,
                (char *)luaA_rawaddr(f) + (size_t)(i - 1) * addr_elemsize(at),
                at->pointee))
          luaG_errormsg(L);
        return 0;
      }
      if (ttisstring(key) && at->pointee != NULL
          && at->pointee->ft_kind == COMPOSITE) {
        const char *k = getstr(tsvalue(key));
        ForeignType *fft;
        size_t off;
        checkreleased(L, f);
        if (!luaA_findfield(at->pointee, k, &fft, &off))
          luaG_runerror(L, "no field '%s' in foreign type '%s'",
                        k, UNIQTYPE_NAME(at->pointee->ft_type));
        if (fft == NULL)
          luaG_runerror(L, "field '%s': foreign datatype is not handled", k);
        if (!luaA_istriviallycopiable(fft))
          luaG_runerror(L, "cannot assign to field '%s'", k);
        if (fft->ft_store(L, val, (char *)luaA_rawaddr(f) + off, fft))
          luaG_errormsg(L);
        return 0;
      }
      return LUAA_NOTFOUND;
    }
    default:
      return LUAA_NOTFOUND;
  }
}

/* #t for a foreign 't': element count for address/array views; other
** kinds fall through to metamethods (returns 0) */
int luaA_foreignlen (lua_State *L, const TValue *t, lua_Integer *out) {
  Foreign *f = frnvalue(t);
  (void)L;
  if (f->ftype == NULL) return 0;
  switch (f->ftype->ft_kind) {
    case ADDRESS: case ARRAY:
      *out = (lua_Integer)f->length;
      return 1;
    default:
      return 0;
  }
}

/* }================================================================== */

#endif
