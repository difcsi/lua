/*
** $Id: lallocslib.c $
** Standard library for liballocs-typed C objects ('allocs')
** See Copyright Notice in lua.h
**
** Unlike the other standard libraries, this one is part of the
** LUA_TFOREIGN core feature and uses internal headers: it must create
** and inspect Foreign objects directly.
*/

#define lallocslib_c
#define LUA_LIB

#include "lprefix.h"


#include <stddef.h>

#include "lua.h"

#if defined(LUA_USE_LIBALLOCS)

#include "lauxlib.h"
#include "lualib.h"
#include "llimits.h"
#include "lstate.h"
#include "lallocs.h"


/*
** Test hooks: create a Foreign over a static arena, and inspect the
** intern table. Used by the smoke tests; the real library surface
** (load, type objects, type algebra) arrives with milestone M2.
*/

static char dummyarena[256];

static int a_dummy (lua_State *L) {
  lua_Integer off = luaL_optinteger(L, 1, 0);
  Foreign *f;
  luaL_argcheck(L, 0 <= off && off < cast(lua_Integer, sizeof(dummyarena)),
                1, "offset out of range");
  f = luaA_newforeign(L, dummyarena + off, NULL, 0, LUA_VFOREIGN);
  luaA_pushforeign(L, f);
  return 1;
}


static int a_interncount (lua_State *L) {
  AllocsState *as = G(L)->allocs;
  lua_pushinteger(L, (as != NULL) ? cast(lua_Integer, as->internlive) : 0);
  return 1;
}


static const luaL_Reg allocs_funcs[] = {
  {"_dummy", a_dummy},
  {"_interncount", a_interncount},
  {NULL, NULL}
};


LUAMOD_API int luaopen_allocs (lua_State *L) {
  luaL_newlib(L, allocs_funcs);
  lua_pushliteral(L, "0.1");
  lua_setfield(L, -2, "_VERSION");
  return 1;
}

#endif
