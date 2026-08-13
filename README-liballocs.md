# Lua + liballocs: native foreign objects (`LUA_TFOREIGN`)

This branch (`liballocs`) extends the Lua VM with **first-class,
garbage-collected views of liballocs-typed C objects**. It replaces the
out-of-tree userdata-proxy extension (`pycallocs/vmextensions/lua`) with
native machinery: C structs, arrays, pointers and functions surface as
values of a new collectable type, known to the interpreter, the GC and
the equality/hashing machinery.

Everything is guarded by `LUA_USE_LIBALLOCS`. **The default build is
pristine upstream Lua** — with the flag off, all `lallocs*.c` files
compile to empty translation units and no core behaviour changes.

## What "native" buys over the proxy extension

- **Host↔C reference cycles are collected.** The GC traverses foreign
  memory via uniqtype layouts, so C→C pointer edges are ordinary GC
  edges (`gc_struct_cycle`'s documented leak is gone; see
  `tests/lua/native_cycle`, `native_gengc`). External C slots (globals,
  untracked allocations) act as GC roots, maintained by the liballocs
  GC-policy callbacks; slots *inside* Lua-known chunks are classified as
  internal edges and never pin cycles.
- **Identity by C address.** One interned `Foreign` per
  `(address, type, length)`: `==`, `rawequal` and table keys work with
  no metamethods (`native_identity`).
- **Native dispatch.** Field/element access runs in
  `luaV_finishget/finishset`; foreign functions are called through
  `luaD_precall` like ordinary functions (libffi underneath).
- **Multiple `lua_State`s per process.** A process registry plus shared
  chunk refcounts: two states can view the same chunk, and one state's
  close never frees memory another still views (`native_multistate`).
- `pairs()` over structs and arrays, pointer arithmetic (`p+1`, `p-q`)
  and same-allocation ordering (`native_pairs`, `native_ptr_arith`).

The `allocs` stdlib carries the module surface (`allocs.load`,
type objects, `.ptr`/`.array`/`.fun` algebra, `from_address`,
`address_of`, `release`), source-compatible with the extension's
`luaproxy` module for the pycallocs test suite.

## Building

```sh
# pristine upstream (default)
make -j

# with native liballocs support, full runtime linked in (deliverable)
make -j LIBALLOCS_CONFIG=1 \
     LIBALLOCS_HOME=/path/to/liballocs \
     BASETYPES_PROVIDER=/path/to/libbasetypes_provider.so

# with native support compiled in but only the no-op stubs linked
# (used to run the upstream test suite; foreign features degrade
# gracefully to "not present" errors)
make -j LIBALLOCS_CONFIG=1 LIBALLOCS_LINK=dummyweaks \
     LIBALLOCS_HOME=/path/to/liballocs
```

Optional: `STACKSCAN_HOME=...` enables the Alaska handle
(`ss_translate`) paths. `lallocs*.o` build with `-std=gnu11 -fPIC`
(liballocs headers need GNU C and reference protected DSO data);
the rest of the tree keeps the stock flags.

Run recipe (same as the extension): `LD_PRELOAD` the liballocs preload
and the base-types provider, set `META_BASE`, and
`GLIBC_TUNABLES=glibc.rtld.optional_static_tls=524288`. See
`pycallocs/tests/vmextensions/lua/run_tests.sh --native`, which runs the
whole suite (11 parity tests + 6 native-capability tests).

## Core diff surface (checklist for upstream merges)

Kept deliberately small: heavy logic lives in the new files
(`lallocs.h`, `lallocs.c`, `lallocsffi.c`, `lallocsbounds.c`,
`lallocslib.c`); core files get guarded case labels only.

| File | What |
|---|---|
| `lua.h` | `LUA_TFOREIGN`, `LUA_NUMTYPES` 9→10; `lua_isforeign`/`lua_toforeignaddr`/`lua_pushforeign` |
| `lobject.h` | `LUA_VFOREIGN`/`LUA_VFFUNC`, `struct Foreign`, value macros |
| `lstate.h`/`lstate.c` | `GCUnion` member, `gco2frn`, `global_State.allocs`, init + `luaA_close` |
| `lgc.c` | objsize/getgclist/reallymarkobject/propagatemark/freeobj cases; `luaA_markgcobject`; `luaA_markroots` in restartcollection+atomic; thread-pattern watching in sweep2old/correctgraylist |
| `ltm.c` | `"foreign"` type name; uniqtype names in `luaT_objtypename` |
| `lvm.c` | foreign branches in `luaV_finishget`/`finishset`/`objlen` |
| `ldo.c` | `LUA_VFFUNC` cases in `luaD_precall`/`pretailcall` |
| `lapi.c` | `lua_rawlen`/`lua_topointer` cases; the new API |
| `ltests.c` | printvalue/checkrefs/checkgraylist/checkobject cases |
| `linit.c`/`lualib.h` | `allocs` stdlib registration |
| `makefile` | `LIBALLOCS_CONFIG` block, per-file rules, dep lines |

After merging upstream: run testes in all four configs (stock,
stock+TESTS, flag-on/dummyweaks, flag-on+TESTS) and the pycallocs suite
under `--native`. `lgc.c` is the churn hotspot; the checklist comment in
`lallocs.h` mirrors this table.

## GC design notes

- Foreign objects follow the **thread pattern**: re-linked into
  `grayagain` on traversal, kept watched by `sweep2old` and
  `correctgraylist`, so they are re-traversed in the atomic phase of
  every collection (minor ones included). C memory mutated behind the
  collector's back is therefore never missed. The write-barrier-driven
  optimization (let barrier-clean foreigns rest black/old, re-gray from
  the `__notify_ptr_write` callback) is deliberately **not** implemented
  yet: always-gray is the safe reference discipline, and the callback
  runs off-thread in general. When it lands it should keep this mode as
  a `LUA_ALLOCS_PARANOID_GC` fallback.
- The GC pacer sees adopted C memory: creating a base view debits the
  chunk's size via `luaE_setdebt` (the pacer recalibrates each cycle).
- No defer-free window around `ffi_call` (the extension needed one):
  a chunk's `Foreign` survives until the next GC, which cannot run
  inside `ffi_call`, and the intern lookup resurrects dead-but-unswept
  objects when the callee returns the chunk.
- Threading: the state registry and chunk refcounts are mutex-guarded,
  but marking GC objects from a policy callback is only safe when the
  triggering write runs on the owning state's thread (the usual case:
  the write is made by C code that state called into).

## Known issues and deferred work

Observed in the current liballocs (worth fixing there):
- `LIBALLOCS_ERR=/dev/null` segfaults any preloaded process at startup.
- realloc of a large (mmap-served) chunk corrupts the malloc index
  (glibc `mremap` moves the chunk behind the index's back) — exposed by
  Lua's stack reallocation in testes `locals.lua`; this is why the
  upstream-testes gate links `dummyweaks` instead of the full runtime.
- `lifetime_policies.c` accepts `policy == LIFETIME_POLICIES` (off-by-
  one OOB write) — this fork registers exactly one policy.
- debug-build stderr noise (`lazily mapped ...`, `MMAP: ...`) is
  unconditional; `main.lua`'s `env -i` interpreter tests cannot pass
  with the runtime linked in.

Marshalling gaps carried over from the extension (fail cleanly):
array copy-in, materializing plain Lua values where `T*` is expected,
single return value only, no varargs, bit fields, `SUBRANGE`,
non-trivially-copiable assignment, uint64/long-double fidelity,
closure errors become warnings (`lua_warning`) + zeroed returns.

Semantics notes:
- A struct and its offset-0 first member are *different* values under
  `==` (identity is by `(address, type, length)`); use
  `allocs.address_of` to compare addresses.
- Scalars always convert by value (reads of `int` fields give Lua
  integers, not views), exactly like the extension.
