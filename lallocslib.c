/*
** $Id: lallocslib.c $
** Standard library for liballocs-typed C objects ('allocs')
** See Copyright Notice in lua.h
**
** Port of vmextensions/lua/{luaproxy.c,library_loader.c,foreign_type.c
** (wrapper half)} plus the shared metatable that serves methods,
** tostring and the parity error messages for foreign values. Unlike
** the other standard libraries this one is part of the LUA_TFOREIGN
** core feature and uses internal headers.
*/

#define lallocslib_c
#define LUA_LIB

#include "lprefix.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  /* dladdr */
#endif


#include <stdlib.h>
#include <string.h>

#include "lua.h"

#if defined(LUA_USE_LIBALLOCS)

#include <dlfcn.h>
#include <link.h>
#include <elf.h>

#include "lauxlib.h"
#include "lualib.h"
#include "llimits.h"
#include "lgc.h"
#include "lobject.h"
#include "lstate.h"
#include "ltm.h"
#include "lallocs.h"

#include <liballocs.h>

#if defined(LUA_ALLOCS_HAVE_STACKSCAN)
#include <handle_query.h>
#else
#define ss_translate(p)		(p)
#define ss_alloc_get_type(p)	__liballocs_get_alloc_type(p)
#endif

/* must link against only the dummyweaks stubs too (see lallocs.c) */
#pragma weak __liballocs_get_or_create_address_type
#pragma weak __liballocs_get_or_create_flexible_array_type
#pragma weak __liballocs_get_or_create_subprogram_type

#define checkruntime(L,sym) \
  { if (&sym == NULL) \
      return luaL_error(L, "liballocs runtime not present"); }

#define FTTYPE_MT	"lallocs.ForeignType"
#define WRAPPERS_KEY	"lallocs.wrappers"


static Foreign *checkforeign (lua_State *L, int idx) {
  Foreign *f = luaA_toforeign(L, idx);
  if (f == NULL)
    luaL_error(L, "expected a foreign value, got %s", luaL_typename(L, idx));
  return f;
}

static ForeignType *frntype (Foreign *f) {
  return f->ftype;
}


/*
** {==================================================================
** Lua-visible type wrapper objects
** ===================================================================
*/

static ForeignType *check_fttype (lua_State *L, int idx) {
  return *(ForeignType **)luaL_checkudata(L, idx, FTTYPE_MT);
}

static void pushwrapper (lua_State *L, ForeignType *ft) {
  lua_getfield(L, LUA_REGISTRYINDEX, WRAPPERS_KEY);
  lua_pushlightuserdata(L, ft);
  if (lua_rawget(L, -2) == LUA_TUSERDATA) {
    lua_remove(L, -2);
    return;
  }
  lua_pop(L, 1);
  {
    ForeignType **slot =
        (ForeignType **)lua_newuserdatauv(L, sizeof(ForeignType *), 0);
    *slot = ft;
    luaL_setmetatable(L, FTTYPE_MT);
    lua_pushlightuserdata(L, ft);
    lua_pushvalue(L, -2);
    lua_rawset(L, -4);   /* wrappers[ft] = wrapper */
    lua_remove(L, -2);   /* the wrapper cache table */
  }
}

static int fttype_call (lua_State *L) {
  ForeignType *ft = check_fttype(L, 1);
  if (ft->ft_ctor == NULL)
    return luaL_error(L, "cannot construct values of foreign type '%s'"
                      " from Lua", UNIQTYPE_NAME(ft->ft_type));
  luaC_checkGC(L);  /* constructor churn must still drive the GC */
  if (ft->ft_ctor(L, 2, ft)) return lua_error(L);
  return 1;
}

static int fttype_tostring (lua_State *L) {
  ForeignType *ft = check_fttype(L, 1);
  lua_pushfstring(L, "<foreign type '%s'>", UNIQTYPE_NAME(ft->ft_type));
  return 1;
}

/* T.fun(arg1_t, ...): function type returning T (return type rides in
** upvalue 1 so the Python-like dot spelling works) */
static int fttype_fun (lua_State *L) {
  ForeignType *ret_ft = check_fttype(L, lua_upvalueindex(1));
  int nargs = lua_gettop(L);
  const struct uniqtype *argtypes[nargs > 0 ? nargs : 1];
  const struct uniqtype *funtype;
  ForeignType *ft;
  int i;
  checkruntime(L, __liballocs_get_or_create_subprogram_type);
  for (i = 0; i < nargs; i++)
    argtypes[i] = check_fttype(L, 1 + i)->ft_type;
  funtype = __liballocs_get_or_create_subprogram_type(
      (struct uniqtype *)ret_ft->ft_type, (unsigned)nargs,
      (struct uniqtype **)argtypes);
  if (funtype == NULL)
    return luaL_error(L, "failed to create requested function type");
  ft = luaA_gettype(L, funtype);
  if (ft == NULL) return lua_error(L);
  pushwrapper(L, ft);
  return 1;
}

/* type algebra on wrappers: T.ptr, T.array, T.fun, T.name */
static int fttype_index (lua_State *L) {
  ForeignType *ft = check_fttype(L, 1);
  const char *key = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : NULL;
  if (key == NULL) { lua_pushnil(L); return 1; }

  if (strcmp(key, "ptr") == 0) {
    const struct uniqtype *ptrtype;
    ForeignType *pft;
    checkruntime(L, __liballocs_get_or_create_address_type);
    ptrtype = __liballocs_get_or_create_address_type(ft->ft_type);
    if (ptrtype == NULL)
      return luaL_error(L, "cannot create pointer type to '%s'",
                        UNIQTYPE_NAME(ft->ft_type));
    pft = luaA_gettype(L, ptrtype);
    if (pft == NULL) return lua_error(L);
    pushwrapper(L, pft);
    return 1;
  }
  if (strcmp(key, "array") == 0) {
    const struct uniqtype *arrtype;
    ForeignType *aft;
    checkruntime(L, __liballocs_get_or_create_flexible_array_type);
    arrtype = __liballocs_get_or_create_flexible_array_type(
        (struct uniqtype *)ft->ft_type);
    if (arrtype == NULL)
      return luaL_error(L, "cannot create array of type '%s'",
                        UNIQTYPE_NAME(ft->ft_type));
    aft = luaA_gettype(L, arrtype);
    if (aft == NULL) return lua_error(L);
    pushwrapper(L, aft);
    return 1;
  }
  if (strcmp(key, "fun") == 0) {
    lua_pushvalue(L, 1);  /* the wrapper becomes upvalue 1 */
    lua_pushcclosure(L, fttype_fun, 1);
    return 1;
  }
  if (strcmp(key, "name") == 0) {
    lua_pushstring(L, UNIQTYPE_NAME(ft->ft_type));
    return 1;
  }
  lua_pushnil(L);
  return 1;
}

static void inittypemeta (lua_State *L) {
  if (lua_getfield(L, LUA_REGISTRYINDEX, WRAPPERS_KEY) == LUA_TNIL) {
    lua_pop(L, 1);
    lua_newtable(L);  /* wrapper cache: types are immortal, strong refs */
    lua_setfield(L, LUA_REGISTRYINDEX, WRAPPERS_KEY);
  }
  else lua_pop(L, 1);

  if (luaL_newmetatable(L, FTTYPE_MT)) {
    lua_pushcfunction(L, fttype_call);
    lua_setfield(L, -2, "__call");
    lua_pushcfunction(L, fttype_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, fttype_index);
    lua_setfield(L, -2, "__index");
  }
  lua_pop(L, 1);
}

/* }================================================================== */


/*
** {==================================================================
** Methods and the shared foreign metatable
** ===================================================================
*/

static int frnmethod_release (lua_State *L) {
  Foreign *f = checkforeign(L, 1);
  luaA_release(L, f);
  return 0;
}

/* v:sub(i, j): interior slice [i..j] (1-based, inclusive), anchored to
** its source through the base GC edge */
static int frnmethod_sub (lua_State *L) {
  Foreign *f = checkforeign(L, 1);
  ForeignType *ft = frntype(f);
  ForeignTypeAddress *at;
  lua_Integer i, j;
  size_t esz;
  Foreign *slice;
  if (ft == NULL || (ft->ft_kind != ADDRESS && ft->ft_kind != ARRAY))
    return luaL_error(L, "sub: not an address proxy");
  if (f->flags & FF_RELEASED)
    return luaL_error(L, "access to a released foreign proxy");
  at = cast(ForeignTypeAddress *, ft);
  i = luaL_checkinteger(L, 2);
  j = luaL_optinteger(L, 3, (lua_Integer)f->length);
  if (i < 1) i = 1;
  if (j > (lua_Integer)f->length) j = (lua_Integer)f->length;
  if (j < i) j = i - 1;  /* empty slice */
  esz = at->pointee ? UNIQTYPE_SIZE_IN_BYTES(at->pointee->ft_type) : 0;

  slice = luaA_newforeign(L, (char *)f->addr + (size_t)(i - 1) * esz,
                          ft, (size_t)(j - i + 1), LUA_VFOREIGN);
  if (slice->base == NULL && slice != f) {
    slice->base = f;  /* keep the source (and its chain) alive */
    luaC_objbarrier(L, slice, f);
  }
  luaA_pushforeign(L, slice);
  return 1;
}

static int ischarpointee (ForeignType *ft) {
  ForeignTypeAddress *at;
  if (ft == NULL || (ft->ft_kind != ADDRESS && ft->ft_kind != ARRAY))
    return 0;
  at = cast(ForeignTypeAddress *, ft);
  if (at->pointee == NULL || !UNIQTYPE_IS_BASE_TYPE(at->pointee->ft_type))
    return 0;
  switch (at->pointee->ft_type->un.base.enc) {
    case 0x08: case 0x06:  /* DW_ATE_unsigned_char, DW_ATE_signed_char */
      return 1;
    default:
      return 0;
  }
}

/* v:str(): the pointee bytes as a Lua string (char elements only) */
static int frnmethod_str (lua_State *L) {
  Foreign *f = checkforeign(L, 1);
  const char *s;
  size_t n;
  if (!ischarpointee(frntype(f)))
    return luaL_error(L, "str: only available for char pointees");
  if (f->flags & FF_RELEASED)
    return luaL_error(L, "access to a released foreign proxy");
  s = (const char *)luaA_rawaddr(f);
  n = f->length;
  while (n > 0 && s[n - 1] == '\0') n--;
  lua_pushlstring(L, s, n);
  return 1;
}

/*
** __index fallback: the native path (luaA_foreignget) answered
** LUAA_NOTFOUND. Serve methods, then raise the parity error messages.
*/
static int frn_index (lua_State *L) {
  Foreign *f = checkforeign(L, 1);
  ForeignType *ft = frntype(f);
  const char *key = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : NULL;

  if (key != NULL) {
    if (strcmp(key, "release") == 0) {
      lua_pushcfunction(L, frnmethod_release);
      return 1;
    }
    if (ft != NULL && (ft->ft_kind == ADDRESS || ft->ft_kind == ARRAY)) {
      if (strcmp(key, "sub") == 0) {
        lua_pushcfunction(L, frnmethod_sub);
        return 1;
      }
      if (strcmp(key, "str") == 0) {
        lua_pushcfunction(L, frnmethod_str);
        return 1;
      }
    }
  }
  if (f->flags & FF_RELEASED)
    return luaL_error(L, "access to a released foreign proxy");
  if (ft == NULL)
    return luaL_error(L, "invalid index into foreign value");
  switch (ft->ft_kind) {
    case COMPOSITE:
      if (key == NULL)
        return luaL_error(L, "foreign struct fields are accessed by name");
      return luaL_error(L, "no field '%s' in foreign type '%s'",
                        key, UNIQTYPE_NAME(ft->ft_type));
    case ADDRESS: case ARRAY: {
      ForeignTypeAddress *at = cast(ForeignTypeAddress *, ft);
      if (key != NULL && at->pointee != NULL
          && at->pointee->ft_kind == COMPOSITE)
        return luaL_error(L, "no field '%s' in foreign type '%s'",
                          key, UNIQTYPE_NAME(at->pointee->ft_type));
      return luaL_error(L, "invalid index into address proxy");
    }
    case SUBPROGRAM:
      return luaL_error(L, "invalid index into foreign function");
    default:
      return luaL_error(L, "invalid index into foreign value");
  }
}

static int frn_newindex (lua_State *L) {
  Foreign *f = checkforeign(L, 1);
  ForeignType *ft = frntype(f);
  if (f->flags & FF_RELEASED)
    return luaL_error(L, "access to a released foreign proxy");
  if (ft == NULL)
    return luaL_error(L, "invalid index into foreign value");
  switch (ft->ft_kind) {
    case ADDRESS: case ARRAY:
      return luaL_error(L, "invalid index into address proxy");
    case SUBPROGRAM:
      return luaL_error(L, "cannot index a foreign function");
    default:
      return luaL_error(L, "invalid index into foreign value");
  }
}

/* renders the value that 'luaA_pushfromdata' pushed, appending it to b */
static void addfieldvalue (lua_State *L, luaL_Buffer *b) {
  luaL_tolstring(L, -1, NULL);  /* value -> its display string (on top) */
  lua_remove(L, -2);            /* drop the raw value */
  luaL_addvalue(b);
}

static int frn_tostring (lua_State *L) {
  Foreign *f = checkforeign(L, 1);
  ForeignType *ft = frntype(f);

  if (f->flags & FF_RELEASED) {
    lua_pushstring(L, "<released foreign proxy>");
    return 1;
  }
  if (ft == NULL) {
    lua_pushfstring(L, "foreign: %p", f->addr);
    return 1;
  }

  switch (ft->ft_kind) {
    case COMPOSITE: {
      ForeignTypeComposite *ct = cast(ForeignTypeComposite *, ft);
      luaL_Buffer b;
      unsigned i;
      int first = 1;
      luaL_checkstack(L, 8, "nested struct tostring");
      luaL_buffinit(L, &b);
      luaL_addchar(&b, '(');
      luaL_addstring(&b, UNIQTYPE_NAME(ft->ft_type));
      luaL_addstring(&b, "){");
      for (i = 0; i < ct->nfields; i++) {
        ForeignField *fld = &ct->fields[i];
        if (fld->type == NULL) continue;  /* skip unhandled fields */
        if (!first) luaL_addstring(&b, ", ");
        first = 0;
        luaL_addstring(&b, fld->name);
        luaL_addstring(&b, ": ");
        if (luaA_pushfromdata(L, (char *)luaA_rawaddr(f) + fld->offset,
                              fld->type, 0))
          return lua_error(L);
        addfieldvalue(L, &b);
      }
      luaL_addchar(&b, '}');
      luaL_pushresult(&b);
      return 1;
    }
    case ADDRESS: case ARRAY: {
      ForeignTypeAddress *at = cast(ForeignTypeAddress *, ft);
      size_t esz = at->pointee
          ? UNIQTYPE_SIZE_IN_BYTES(at->pointee->ft_type) : 0;
      if (ischarpointee(ft) && esz == 1) {
        const char *s = (const char *)luaA_rawaddr(f);
        size_t n = f->length;
        luaL_Buffer b;
        while (n > 0 && s[n - 1] == '\0') n--;
        luaL_buffinit(L, &b);
        luaL_addstring(&b, "<'");
        luaL_addlstring(&b, s, n);
        luaL_addstring(&b, "'>");
        luaL_pushresult(&b);
        return 1;
      }
      {
        luaL_Buffer b;
        size_t i;
        luaL_checkstack(L, 8, "address proxy tostring");
        luaL_buffinit(L, &b);
        luaL_addstring(&b, "<[");
        for (i = 0; i < f->length; i++) {
          if (i > 0) luaL_addstring(&b, ", ");
          if (luaA_pushfromdata(L, (char *)luaA_rawaddr(f) + i * esz,
                                at->pointee, 0))
            return lua_error(L);
          addfieldvalue(L, &b);
        }
        luaL_addstring(&b, "]>");
        luaL_pushresult(&b);
        return 1;
      }
    }
    case SUBPROGRAM: {
      const struct uniqtype *type = ft->ft_type;
      const char *symname = "<unknown>";
      Dl_info dlinfo;
      unsigned nret = type->un.subprogram.nret;
      unsigned narg = type->un.subprogram.narg;
      unsigned a;
      luaL_Buffer b;
      if (dladdr(f->addr, &dlinfo) && dlinfo.dli_sname)
        symname = dlinfo.dli_sname;
      luaL_buffinit(L, &b);
      luaL_addstring(&b, "<foreign function '");
      for (a = 0; a < nret; a++) {
        struct uniqtype *t = type->related[a].un.t.ptr;
        if (a > 0) luaL_addstring(&b, ", ");
        luaL_addstring(&b, t ? UNIQTYPE_NAME(t) : "<unknown>");
      }
      luaL_addchar(&b, ' ');
      luaL_addstring(&b, symname);
      luaL_addchar(&b, '(');
      for (a = 0; a < narg; a++) {
        struct uniqtype *t = type->related[nret + a].un.t.ptr;
        if (a > 0) luaL_addstring(&b, ", ");
        luaL_addstring(&b, t ? UNIQTYPE_NAME(t) : "<unknown>");
      }
      luaL_addstring(&b, ")'>");
      luaL_pushresult(&b);
      return 1;
    }
    default:
      lua_pushfstring(L, "foreign: %p", f->addr);
      return 1;
  }
}

/* install the shared metatable for all foreign values */
static void installforeignmt (lua_State *L) {
  lua_createtable(L, 0, 4);
  lua_pushcfunction(L, frn_index);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, frn_newindex);
  lua_setfield(L, -2, "__newindex");
  lua_pushcfunction(L, frn_tostring);
  lua_setfield(L, -2, "__tostring");
  G(L)->mt[LUA_TFOREIGN] = hvalue(s2v(L->top.p - 1));
  lua_pop(L, 1);
}

/* }================================================================== */


/*
** {==================================================================
** Library loading (port of library_loader.c + dlsym_iter.c)
** ===================================================================
*/

/* iterate a freshly dlopen'ed library's own .dynsym */
static int dl_iterate_syms (struct link_map *handle,
    int (*callback)(const ElfW(Sym) *, ElfW(Addr), char *, void *),
    void *arg) {
  const ElfW(Dyn) *p_dyn = handle->l_ld;
  const ElfW(Sym) *p_dynsym = 0;
  char *p_dynstr = 0;
  const ElfW(Sym) *sym;
  int ret = 0;
  while (p_dyn->d_tag != DT_NULL) {
    switch (p_dyn->d_tag) {
      case DT_SYMTAB:
        p_dynsym = (const ElfW(Sym) *)p_dyn->d_un.d_ptr;
        break;
      case DT_STRTAB:
        p_dynstr = (char *)p_dyn->d_un.d_ptr;
        break;
      default: break;
    }
    ++p_dyn;
  }
  if (!p_dynsym || !p_dynstr) return -1;
  lua_assert((char *)p_dynstr > (char *)p_dynsym);
  for (sym = p_dynsym; (char *)sym != p_dynstr; ++sym) {
    ret = callback(sym, handle->l_addr, p_dynstr, arg);
    if (ret != 0) break;
  }
  return ret;
}

struct add_sym_ctxt {
  lua_State *L;
  int module_idx;  /* absolute stack index of the module table */
  int unsup_idx;   /* absolute stack index of the __unsupported table */
};

/* module[type_name] = wrapper ($ -> _ in names, first writer wins);
** 0 added, 1 already present, -1 unhandled type */
static int add_type_to_module (const struct uniqtype *type,
                               struct add_sym_ctxt *ctxt) {
  lua_State *L = ctxt->L;
  char type_name[256];
  ForeignType *ft;
  char *c;
  strncpy(type_name, UNIQTYPE_NAME(type), sizeof type_name - 1);
  type_name[sizeof type_name - 1] = '\0';
  for (c = type_name; *c; ++c)
    if (*c == '$') *c = '_';

  if (lua_getfield(L, ctxt->module_idx, type_name) != LUA_TNIL) {
    lua_pop(L, 1);
    return 1;
  }
  lua_pop(L, 1);

  ft = luaA_gettype(L, type);
  if (ft == NULL) {
    lua_pop(L, 1);  /* the error message */
    return -1;
  }
  pushwrapper(L, ft);
  lua_setfield(L, ctxt->module_idx, type_name);
  return 0;
}

static void recursively_add_useful_types (const struct uniqtype *type,
                                          struct add_sym_ctxt *ctxt) {
  if (type == NULL) return;
  switch (UNIQTYPE_KIND(type)) {
    case BASE:
      /* ignore bit field types */
      if (UNIQTYPE_BASE_TYPE_BIT_SIZE(type)
          != 8 * UNIQTYPE_SIZE_IN_BYTES(type))
        return;
      /* FALLTHROUGH */
    case VOID:
      add_type_to_module(type, ctxt);
      return;
    case COMPOSITE:
      if (add_type_to_module(type, ctxt) == 0) {
        unsigned nmemb = UNIQTYPE_COMPOSITE_MEMBER_COUNT(type);
        unsigned i;
        for (i = 0; i < nmemb; i++)
          recursively_add_useful_types(type->related[i].un.t.ptr, ctxt);
      }
      return;
    case ENUMERATION:
      if (add_type_to_module(type, ctxt) == 0)
        recursively_add_useful_types(UNIQTYPE_ENUM_BASE_TYPE(type), ctxt);
      return;
    case ARRAY:
    case SUBRANGE:
      recursively_add_useful_types(UNIQTYPE_ARRAY_ELEMENT_TYPE(type), ctxt);
      return;
    case ADDRESS:
      recursively_add_useful_types(UNIQTYPE_ULTIMATE_POINTEE_TYPE(type),
                                   ctxt);
      return;
    case SUBPROGRAM: {
      unsigned nb = type->un.subprogram.narg + type->un.subprogram.nret;
      unsigned i;
      for (i = 0; i < nb; i++)
        recursively_add_useful_types(type->related[i].un.t.ptr, ctxt);
      return;
    }
    default:
      return;
  }
}

/* __unsupported[symname] = "stage" or "stage: <message on stack top>" */
static void record_unsupported (struct add_sym_ctxt *ctxt,
                                const char *symname, const char *stage,
                                int msg_on_stack) {
  lua_State *L = ctxt->L;
  if (msg_on_stack) {
    lua_pushfstring(L, "%s: %s", stage,
                    lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    lua_remove(L, -2);
  }
  else
    lua_pushstring(L, stage);
  lua_setfield(L, ctxt->unsup_idx, symname);
}

static int add_sym_to_module (const ElfW(Sym) *sym, ElfW(Addr) loadAddress,
                              char *strtab, void *arg) {
  struct add_sym_ctxt *ctxt = cast(struct add_sym_ctxt *, arg);
  lua_State *L = ctxt->L;

  if ((ELF64_ST_TYPE(sym->st_info) == STT_FUNC
       || ELF64_ST_TYPE(sym->st_info) == STT_OBJECT)
      && (ELF64_ST_BIND(sym->st_info) == STB_GLOBAL
          || ELF64_ST_BIND(sym->st_info) == STB_WEAK)
      && sym->st_shndx != SHN_UNDEF
      && sym->st_shndx != SHN_ABS) {
    const char *symname = strtab + sym->st_name;
    void *data;
    const struct uniqtype *type;
    ForeignType *ftype;
    /* ignore unnamed symbols and reserved names */
    if (symname[0] == '\0' || symname[0] == '_') return 0;

    data = (void *)(loadAddress + sym->st_value);

    type = ss_alloc_get_type(data);
    if (type == NULL) {
      record_unsupported(ctxt, symname, "no liballocs type at symbol", 0);
      return 0;
    }
    recursively_add_useful_types(type, ctxt);

    ftype = luaA_gettype(L, type);
    if (ftype == NULL) {
      record_unsupported(ctxt, symname,
                         "no foreign type for symbol's uniqtype", 1);
      return 0;
    }

    if (luaA_pushfromdata(L, data, ftype, 0)) {
      record_unsupported(ctxt, symname, "proxy creation failed", 1);
      return 0;
    }
    lua_setfield(L, ctxt->module_idx, symname);
  }
  return 0;
}

/* the C scalar base types come from the preloaded base-types provider
** meta-DSO; weak so a missing provider degrades to fewer entries */
#define WEAK_UNIQTYPE(t) \
  extern struct uniqtype __uniqtype__##t __attribute__((weak))
WEAK_UNIQTYPE(void);
WEAK_UNIQTYPE(int);
WEAK_UNIQTYPE(unsigned_int);
WEAK_UNIQTYPE(signed_char);
WEAK_UNIQTYPE(unsigned_char);
WEAK_UNIQTYPE(long_int);
WEAK_UNIQTYPE(unsigned_long_int);
WEAK_UNIQTYPE(short_int);
WEAK_UNIQTYPE(short_unsigned_int);
WEAK_UNIQTYPE(float);
WEAK_UNIQTYPE(double);

static void add_base_types_to_module (struct add_sym_ctxt *ctxt) {
#define ADD_BASE_TYPE(t) \
  do { if (&__uniqtype__##t) add_type_to_module(&__uniqtype__##t, ctxt); } \
  while (0)
  ADD_BASE_TYPE(void);
  ADD_BASE_TYPE(int);
  ADD_BASE_TYPE(unsigned_int);
  ADD_BASE_TYPE(signed_char);
  ADD_BASE_TYPE(unsigned_char);
  ADD_BASE_TYPE(long_int);
  ADD_BASE_TYPE(unsigned_long_int);
  ADD_BASE_TYPE(short_int);
  ADD_BASE_TYPE(short_unsigned_int);
  ADD_BASE_TYPE(float);
  ADD_BASE_TYPE(double);
#undef ADD_BASE_TYPE
}

/* allocs.load(path) -> module table */
static int allocs_load (lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  struct link_map *handle;
  int module_idx, unsup_idx;

  /* the dlopen handle is deliberately never dlclose'd: foreign values
     into the library may outlive the module table */
  handle = cast(struct link_map *, dlopen(path, RTLD_NOW | RTLD_GLOBAL));
  if (handle == NULL)
    return luaL_error(L, "loading of native shared library '%s' failed: %s",
                      path, dlerror());

  lua_newtable(L);  /* the module */
  module_idx = lua_gettop(L);
  lua_newtable(L);  /* __unsupported */
  unsup_idx = lua_gettop(L);

  {
    struct add_sym_ctxt ctxt;
    ctxt.L = L;
    ctxt.module_idx = module_idx;
    ctxt.unsup_idx = unsup_idx;
    dl_iterate_syms(handle, add_sym_to_module, &ctxt);
    add_base_types_to_module(&ctxt);
  }

  lua_setfield(L, module_idx, "__unsupported");
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** Module surface
** ===================================================================
*/

static int allocs_from_address (lua_State *L) {
  lua_Integer addr = luaL_checkinteger(L, 1);
  Foreign *f = luaA_getorcreatebase(L, (void *)(uintptr_t)addr);
  if (f == NULL)
    return luaL_error(L,
        "proxy_from_address: no liballocs proxy type for address %p",
        (void *)(uintptr_t)addr);
  luaA_pushforeign(L, f);
  return 1;
}

/* the inverse: a foreign value's underlying addresslike as an integer */
static int allocs_address_of (lua_State *L) {
  Foreign *f = luaA_toforeign(L, 1);
  if (f == NULL)
    return luaL_error(L, "address_of: expected a foreign value");
  lua_pushinteger(L, (lua_Integer)(uintptr_t)f->addr);
  return 1;
}

static int allocs_release (lua_State *L) {
  return frnmethod_release(L);
}

static int allocs_interncount (lua_State *L) {
  AllocsState *as = G(L)->allocs;
  lua_pushinteger(L, (as != NULL) ? cast(lua_Integer, as->internlive) : 0);
  return 1;
}

/* test hook: a Foreign over a static arena (see M1 smoke tests) */
static char dummyarena[256];

static int allocs_dummy (lua_State *L) {
  lua_Integer off = luaL_optinteger(L, 1, 0);
  Foreign *f;
  luaL_argcheck(L, 0 <= off && off < cast(lua_Integer, sizeof(dummyarena)),
                1, "offset out of range");
  f = luaA_newforeign(L, dummyarena + off, NULL, 0, LUA_VFOREIGN);
  luaA_pushforeign(L, f);
  return 1;
}

static const luaL_Reg allocs_funcs[] = {
  {"load", allocs_load},
  {"from_address", allocs_from_address},
  {"proxy_from_address", allocs_from_address},  /* compat alias */
  {"address_of", allocs_address_of},
  {"release", allocs_release},
  {"_interncount", allocs_interncount},
  {"_dummy", allocs_dummy},
  {NULL, NULL}
};

static void add_type_field (lua_State *L, const char *name,
                            struct uniqtype *u) {
  ForeignType *ft;
  if (u == NULL) return;  /* base-types provider not preloaded */
  ft = luaA_gettype(L, u);
  if (ft == NULL) { lua_pop(L, 1); return; }
  pushwrapper(L, ft);
  lua_setfield(L, -2, name);
}

LUAMOD_API int luaopen_allocs (lua_State *L) {
  luaA_getstate(L);  /* create state, register the GC policy, and
                        enforce the one-state-per-process limit */
  inittypemeta(L);
  installforeignmt(L);

  luaL_newlib(L, allocs_funcs);

#define ADD_TYPE_WITH_NAME(name, t) add_type_field(L, name, &__uniqtype__##t)
#define ADD_TYPE(t) ADD_TYPE_WITH_NAME(#t, t)
  ADD_TYPE(void);
  ADD_TYPE(int);
  ADD_TYPE(unsigned_int);
  ADD_TYPE(signed_char);
  ADD_TYPE(unsigned_char);
  if ((char)-1 < 0) ADD_TYPE_WITH_NAME("char", signed_char);
  else ADD_TYPE_WITH_NAME("char", unsigned_char);
  ADD_TYPE(long_int);
  ADD_TYPE(unsigned_long_int);
  ADD_TYPE(short_int);
  ADD_TYPE_WITH_NAME("unsigned_short_int", short_unsigned_int);
  ADD_TYPE(float);
  ADD_TYPE(double);
#undef ADD_TYPE
#undef ADD_TYPE_WITH_NAME

  lua_pushstring(L, "0.1");
  lua_setfield(L, -2, "_VERSION");
  return 1;
}

/* }================================================================== */

#endif
