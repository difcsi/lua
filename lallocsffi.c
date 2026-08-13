/*
** $Id: lallocsffi.c $
** Foreign-function call machinery for LUA_TFOREIGN (libffi)
** See Copyright Notice in lua.h
**
** Port of vmextensions/lua/function_proxy.c and the shared
** vmextensions/common/ffitypes.c. A LUA_VFFUNC Foreign is called
** through luaD_precall's tag case, which routes it through the
** ordinary C-function calling convention (precallC) into
** luaA_callforeign below -- so the marshalling code runs in a plain
** lua_CFunction context and the public stack API applies.
*/

#define lallocsffi_c
#define LUA_CORE

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  /* dladdr */
#endif

#include "lprefix.h"


#include <stdlib.h>
#include <string.h>

#include "lua.h"

#if defined(LUA_USE_LIBALLOCS)

#include <dlfcn.h>
#include <ffi.h>

#include "lapi.h"
#include "lauxlib.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltm.h"
#include "lallocs.h"

#include <liballocs.h>
#include <dwarf.h>


/*
** {==================================================================
** uniqtype -> ffi_type (port of vmextensions/common/ffitypes.c)
** ===================================================================
** COMPOSITE builds a real elements[] array (required for AMD64 ABI
** classification) and patches size/alignment for unions. Composite
** results are malloc'd; everything else is a libffi singleton.
*/

static void free_ffi_type_arr (ffi_type **arr);

static void free_ffi_type (ffi_type *typ) {
  if (typ->type == FFI_TYPE_STRUCT) {
    free_ffi_type_arr(typ->elements);
    free(typ);
  }
}

static void free_ffi_type_arr (ffi_type **arr) {
  int i;
  if (arr == NULL) return;
  for (i = 0; arr[i]; i++) free_ffi_type(arr[i]);
  free(arr);
}

static ffi_type *ffi_type_for_uniqtype (const struct uniqtype *type) {
  switch (UNIQTYPE_KIND(type)) {
    case VOID:
      return &ffi_type_void;
    case ARRAY:
    case ADDRESS:
    case SUBPROGRAM:
      return &ffi_type_pointer;
    case ENUMERATION:
      if (type->related[0].un.t.ptr)
        type = type->related[0].un.t.ptr;  /* fall through as BASE */
      else {
        unsigned size = UNIQTYPE_SIZE_IN_BYTES(type);
        if (size == 1) return &ffi_type_sint8;
        if (size == 2) return &ffi_type_sint16;
        if (size == 4) return &ffi_type_sint32;
        if (size == 8) return &ffi_type_sint64;
        return NULL;
      }
      /* FALLTHROUGH */
    case BASE: {
      unsigned size = UNIQTYPE_SIZE_IN_BYTES(type);
      switch (type->un.base.enc) {
        case DW_ATE_boolean:
          return &ffi_type_uint8;
        case DW_ATE_address:
        case DW_ATE_unsigned:
        case DW_ATE_unsigned_char:
          if (size == 1) return &ffi_type_uint8;
          if (size == 2) return &ffi_type_uint16;
          if (size == 4) return &ffi_type_uint32;
          if (size == 8) return &ffi_type_uint64;
          return NULL;
        case DW_ATE_signed:
        case DW_ATE_signed_char:
          if (size == 1) return &ffi_type_sint8;
          if (size == 2) return &ffi_type_sint16;
          if (size == 4) return &ffi_type_sint32;
          if (size == 8) return &ffi_type_sint64;
          return NULL;
        case DW_ATE_float:
          if (size == 4) return &ffi_type_float;
          if (size == 8) return &ffi_type_double;
          if (size == 16) return &ffi_type_longdouble;
          return NULL;
#ifdef FFI_TARGET_HAS_COMPLEX_TYPE
        case DW_ATE_complex_float:
          if (size == 8) return &ffi_type_complex_float;
          if (size == 16) return &ffi_type_complex_double;
          if (size == 32) return &ffi_type_complex_longdouble;
          return NULL;
#endif
        default:
          return NULL;
      }
    }
    case COMPOSITE: {
      int nb_fields = (int)type->un.composite.nmemb;
      ffi_type **field_ffi_types =
          cast(ffi_type **, malloc((1 + (size_t)nb_fields) * sizeof(ffi_type *)));
      ffi_type *ffityp;
      ffi_cif dummy;
      int i;
      for (i = 0; i < nb_fields; i++) {
        field_ffi_types[i] = ffi_type_for_uniqtype(type->related[i].un.t.ptr);
        if (field_ffi_types[i] == NULL) {
          field_ffi_types[i] = NULL;
          free_ffi_type_arr(field_ffi_types);
          return NULL;
        }
      }
      field_ffi_types[nb_fields] = NULL;

      ffityp = cast(ffi_type *, malloc(sizeof(ffi_type)));
      memset(ffityp, 0, sizeof(ffi_type));
      ffityp->type = FFI_TYPE_STRUCT;
      ffityp->elements = field_ffi_types;

      /* fills size/alignment (a dummy cif prep works on old libffi) */
      if (ffi_prep_cif(&dummy, FFI_DEFAULT_ABI, 0, ffityp, NULL) != FFI_OK) {
        free_ffi_type(ffityp);
        return NULL;
      }
      /* patch size and alignment to handle unions */
      if (ffityp->size != UNIQTYPE_SIZE_IN_BYTES(type)) {
        ffityp->size = UNIQTYPE_SIZE_IN_BYTES(type);
        for (i = 0; i < nb_fields; i++)
          if (field_ffi_types[i]->alignment > ffityp->alignment)
            ffityp->alignment = field_ffi_types[i]->alignment;
      }
      return ffityp;
    }
    case SUBRANGE:
    default:
      return NULL;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Function types and the call path
** ===================================================================
*/

/* lazy call-interface preparation; 0 ok, -1 with error message pushed */
int luaA_functionsetup (lua_State *L, ForeignTypeFunction *self) {
  const struct uniqtype *type = self->ft.ft_type;
  const struct uniqtype *ret_type;
  ffi_type *ffi_ret_type;
  ffi_type **ffi_arg_types = NULL;
  unsigned narg;
  unsigned i;
  ffi_cif *cif;

  if (self->ff_cif != NULL) return 0;

  if (type->un.subprogram.nret != 1) {
    luaO_pushfstring(L, "%s", "foreign functions not having exactly one"
                     " return value are not supported");
    return -1;
  }
  ret_type = type->related[0].un.t.ptr;
  self->ff_rettype = luaA_gettype(L, ret_type);
  if (self->ff_rettype == NULL) return -1;  /* message already pushed */
  ffi_ret_type = ffi_type_for_uniqtype(ret_type);
  if (ffi_ret_type == NULL) {
    luaO_pushfstring(L, "%s", "cannot get ABI encoding for return value");
    return -1;
  }

  narg = type->un.subprogram.narg;
  if (narg > 0) {
    ffi_arg_types = cast(ffi_type **, malloc((1 + narg) * sizeof(ffi_type *)));
    self->ff_argtypes =
        cast(ForeignType **, malloc((1 + narg) * sizeof(ForeignType *)));
    ffi_arg_types[narg] = NULL;
    self->ff_argtypes[narg] = NULL;
  }
  for (i = 0; i < narg; i++) {
    const struct uniqtype *arg_type = type->related[i + 1].un.t.ptr;
    ffi_arg_types[i] = ffi_type_for_uniqtype(arg_type);
    self->ff_argtypes[i] = arg_type ? luaA_gettype(L, arg_type) : NULL;
    if (self->ff_argtypes[i] == NULL && arg_type)
      lua_pop(L, 1);  /* its error message */
    if (ffi_arg_types[i] == NULL || self->ff_argtypes[i] == NULL) {
      luaO_pushfstring(L, "cannot get ABI encoding for argument %d",
                       (int)i + 1);
      ffi_arg_types[i + 1] = NULL;
      self->ff_argtypes[i + 1] = NULL;
      free_ffi_type_arr(ffi_arg_types);
      free(self->ff_argtypes);
      self->ff_argtypes = NULL;
      free_ffi_type(ffi_ret_type);
      return -1;
    }
  }

  cif = cast(ffi_cif *, malloc(sizeof(ffi_cif)));
  if (ffi_prep_cif(cif, FFI_DEFAULT_ABI, narg, ffi_ret_type,
                   ffi_arg_types) == FFI_OK) {
    self->ff_cif = cif;
    return 0;
  }

  luaO_pushfstring(L, "%s", "failure in call information initialization");
  free(cif);
  free_ffi_type_arr(ffi_arg_types);
  free(self->ff_argtypes);
  self->ff_argtypes = NULL;
  free_ffi_type(ffi_ret_type);
  return -1;
}


/*
** The call itself. Runs as an ordinary C function (via precallC from
** luaD_precall's LUA_VFFUNC case): the called Foreign sits in the
** function slot of the current CallInfo, arguments at 1..top.
**
** Marshalling per call: pass 1 takes zero-copy data pointers for
** arguments that already have C storage; pass 2 stores the rest into
** a stack scratch buffer. No defer-free window is needed (unlike the
** extension): a chunk whose last root the callee drops survives until
** the next GC, which cannot run inside ffi_call, and its Foreign is
** resurrected by the intern lookup when the return value is wrapped.
*/
int luaA_callforeign (lua_State *L) {
  Foreign *self = frnvalue(s2v(L->ci->func.p));
  ForeignTypeFunction *type = cast(ForeignTypeFunction *, self->ftype);
  const struct uniqtype *ret_type;
  ForeignType *ret_ftype;
  unsigned narg, given, i;
  int r;

  luaC_checkGC(L);  /* foreign-call churn must still drive the GC */
  if (luaA_functionsetup(L, type)) return lua_error(L);

  narg = type->ft.ft_type->un.subprogram.narg;
  given = (unsigned)lua_gettop(L);
  if (given != narg)
    return luaL_error(L, "this function takes exactly %d argument%s"
                      " (%d given)", narg, narg == 1 ? "" : "s", given);

  {
    void *ff_args[narg > 0 ? narg : 1];
    unsigned argsize = 0;
    for (i = 0; i < narg; i++) {
      ForeignType *aft = type->ff_argtypes[i];
      void *data_ptr = NULL;
      if (aft->ft_dataptr) {
        data_ptr = aft->ft_dataptr(L, s2v(L->ci->func.p + 1 + i), aft);
        ff_args[i] = data_ptr;
      }
      if (data_ptr == NULL)
        argsize += UNIQTYPE_SIZE_IN_BYTES(aft->ft_type);
    }

    {
      char argvals[argsize > 0 ? argsize : 1];
      char *cur_arg = argvals;
      for (i = 0; i < narg; i++) {
        ForeignType *aft = type->ff_argtypes[i];
        if (aft->ft_dataptr && ff_args[i]) continue;
        if (aft->ft_store(L, s2v(L->ci->func.p + 1 + i), cur_arg, aft))
          return lua_error(L);
        ff_args[i] = cur_arg;
        cur_arg += UNIQTYPE_SIZE_IN_BYTES(aft->ft_type);
      }

      ret_type = type->ft.ft_type->related[0].un.t.ptr;
      ret_ftype = type->ff_rettype;
      {
        unsigned natret = UNIQTYPE_SIZE_IN_BYTES(ret_type);
        unsigned retsize = natret;
        if (sizeof(ffi_arg) > retsize) retsize = (unsigned)sizeof(ffi_arg);
        {
          char retval[retsize];
          char *retp = retval;

          ffi_call(cast(ffi_cif *, type->ff_cif),
                   (void (*)(void))self->addr, retval, ff_args);

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
          if (natret < sizeof(ffi_arg) &&
              (UNIQTYPE_IS_ENUM_TYPE(ret_type)
               || UNIQTYPE_IS_POINTER_TYPE(ret_type)
               || (UNIQTYPE_IS_BASE_TYPE(ret_type)
                   && ret_type->un.base.enc != DW_ATE_float)))
            retp += sizeof(ffi_arg) - natret;
#endif
          r = luaA_pushfromdata(L, retp, ret_ftype, 1);
        }
      }
    }
  }
  if (r) return lua_error(L);
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** Closures: Lua function -> C function pointer
** ===================================================================
** The closure context is a native structure keyed by the trampoline
** code address in AllocsState.extras. The Lua callable is anchored as
** a GC edge of the closure's Foreign (marked by luaA_traverseclosure),
** NOT via the registry: luaA_freeclosure runs inside the GC sweep and
** must not touch the Lua heap.
*/

typedef struct AllocsClosureCtx {
  ForeignTypeFunction *type;
  ffi_closure *closure;
  lua_State *L;   /* the creating state; the trampoline is bound to it */
  TValue fn;      /* the Lua callable (GC edge via traverseclosure) */
} AllocsClosureCtx;

static AllocsClosureCtx *closurectx (AllocsState *as, Foreign *f) {
  return cast(AllocsClosureCtx *, luaA_mapget(&as->extras, f->addr));
}

void luaA_traverseclosure (global_State *g, Foreign *f) {
  AllocsState *as = g->allocs;
  AllocsClosureCtx *ctx;
  if (as == NULL) return;
  ctx = closurectx(as, f);
  if (ctx != NULL && iscollectable(&ctx->fn))
    luaA_markgcobject(g, gcvalue(&ctx->fn));
}

void luaA_freeclosure (AllocsState *as, Foreign *f) {
  AllocsClosureCtx *ctx = closurectx(as, f);
  if (ctx != NULL) {
    luaA_mapremove(&as->extras, f->addr);
    ffi_closure_free(ctx->closure);
    free(ctx);
  }
}

/*
** The ffi trampoline target. Runs with no Lua frame beneath it, so it
** must never longjmp: the Lua callable is invoked under lua_pcall, and
** any failure leaves a zeroed return value and a warning.
*/
static void closure_trampoline (ffi_cif *cif, void *ret, void **args,
                                void *ud) {
  AllocsClosureCtx *ctx = cast(AllocsClosureCtx *, ud);
  ForeignTypeFunction *ft = ctx->type;
  lua_State *L = ctx->L;
  unsigned nargs = cif->nargs;
  int base;
  unsigned i;

  /* zero the whole return slot first (integral returns are widened to
     a full ffi_arg by libffi's contract; error paths return 0) */
  if (cif->rtype != &ffi_type_void) {
    size_t retsz = cif->rtype->size;
    if (cif->rtype->type != FFI_TYPE_STRUCT && retsz < sizeof(ffi_arg))
      retsz = sizeof(ffi_arg);
    memset(ret, 0, retsz);
  }

  if (L == NULL || !lua_checkstack(L, (int)nargs + 4)) return;
  base = lua_gettop(L);

  setobj2s(L, L->top.p, &ctx->fn);
  api_incr_top(L);
  for (i = 0; i < nargs; i++) {
    ForeignType *aft = ft->ff_argtypes[i];
    /* copy by value so the argument outlives the C caller's frame */
    if (luaA_pushfromdata(L, args[i], aft, 1)) {
      lua_warning(L, "lallocs: closure argument conversion failed", 0);
      lua_settop(L, base);
      return;
    }
  }
  if (lua_pcall(L, (int)nargs, 1, 0) != LUA_OK) {
    lua_warning(L, "lallocs: error in foreign closure: ", 1);
    lua_warning(L, lua_isstring(L, -1) ? lua_tostring(L, -1) : "?", 0);
    lua_settop(L, base);
    return;
  }
  if (ft->ff_rettype->ft_store(L, s2v(L->top.p - 1), ret, ft->ff_rettype)) {
    lua_warning(L, "lallocs: closure return conversion failed", 0);
    lua_pop(L, 1);  /* the pushed error message */
  }
  lua_settop(L, base);
}

/* ft_ctor of SUBPROGRAM types: allocs.int.fun(allocs.int)(add_one)
** builds a C function pointer calling the Lua function */
int luaA_closurector (lua_State *L, int first_arg, ForeignType *ftt) {
  ForeignTypeFunction *ft = cast(ForeignTypeFunction *, ftt);
  AllocsState *as = luaA_getstate(L);
  int nargs = lua_gettop(L) - first_arg + 1;
  AllocsClosureCtx *ctx;
  void *code = NULL;
  Foreign *f;

  if (nargs != 1) {
    luaO_pushfstring(L, "%s", "closure constructor takes exactly one"
                     " callable argument");
    return -1;
  }
  if (lua_type(L, first_arg) != LUA_TFUNCTION
      && !luaL_getmetafield(L, first_arg, "__call")) {
    luaO_pushfstring(L, "%s", "cannot create a foreign closure over a"
                     " non-callable value");
    return -1;
  }
  if (lua_type(L, first_arg) != LUA_TFUNCTION)
    lua_pop(L, 1);  /* the __call field */
  if (luaA_functionsetup(L, ft)) return -1;

  ctx = cast(AllocsClosureCtx *, calloc(1, sizeof(*ctx)));
  ctx->closure = cast(ffi_closure *,
                      ffi_closure_alloc(sizeof(ffi_closure), &code));
  if (ctx->closure == NULL) {
    free(ctx);
    luaO_pushfstring(L, "%s", "failed to allocate ffi closure");
    return -1;
  }
  ctx->type = ft;
  ctx->L = as->mainL;
  setobj(L, &ctx->fn, s2v(L->ci->func.p + first_arg));

  if (ffi_prep_closure_loc(ctx->closure, cast(ffi_cif *, ft->ff_cif),
                           closure_trampoline, ctx, code) != FFI_OK) {
    ffi_closure_free(ctx->closure);
    free(ctx);
    luaO_pushfstring(L, "%s", "failed to create closure for callable");
    return -1;
  }

  luaA_mapset(L, &as->extras, code, ctx);
  f = luaA_newforeign(L, code, &ft->ft, 0, LUA_VFFUNC);
  f->flags |= FF_CLOSURE;
  /* register like a base (the extension put closures in its identity
     cache): this is what lets a C slot holding the trampoline pointer
     retain the closure via luaA_retainstoredptr */
  luaA_mapset(L, &as->basemap, code, f);
  /* the ctx->fn edge is traversed via the Foreign; make the write
     visible to an in-progress collection */
  if (iscollectable(&ctx->fn))
    luaC_objbarrier(L, f, gcvalue(&ctx->fn));
  luaA_pushforeign(L, f);
  return 0;
}

/* }================================================================== */

#endif
