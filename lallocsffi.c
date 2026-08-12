/*
** $Id: lallocsffi.c $
** Foreign-function call machinery for LUA_TFOREIGN (libffi)
** See Copyright Notice in lua.h
*/

#define lallocsffi_c
#define LUA_CORE

#include "lprefix.h"


#include "lua.h"

#if defined(LUA_USE_LIBALLOCS)

#include "lallocs.h"

/*
** The libffi call path ('luaA_precallforeign') and the Lua-closure to
** C-function-pointer trampolines arrive with the marshalling
** milestone (M2).
*/

#endif
