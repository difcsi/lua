/*
** $Id: lallocsbounds.c $
** Bounds inference for LUA_TFOREIGN address/array views
** See Copyright Notice in lua.h
**
** Port of pycallocs vmextensions/common/minicrunch.c (its
** __fetch_bounds_internal), renamed to luaA_fetchbounds so it can
** never interpose on (or collide with) liballocs' own symbol. See the
** original for the full commentary; the logic is unchanged.
*/

#define lallocsbounds_c
#define LUA_CORE

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "lprefix.h"

#include "lua.h"

#if defined(LUA_USE_LIBALLOCS)

#include <stddef.h>
#include <stdio.h>
#include <limits.h>

#include <liballocs.h>
#include <uniqtype-defs.h>

#include "lallocs.h"

/* must link against only the dummyweaks stubs too (see lallocs.c) */
#pragma weak __liballocs_walk_subobjects_spanning_rec
#pragma weak __liballocs_errstring
#pragma weak __uniqtype__signed_char
#pragma weak __uniqtype__unsigned_char

/* State threaded through the subobject walk: the search result proper
** (success/matched_t) plus the *array run* enclosing the target offset
** (the outermost array reached through an uninterrupted chain of array
** descents, and how many innermost elements it spans; descending
** through a non-array resets the run). */
struct bounds_cb_arg {
  const struct uniqtype *passed_in_t;
  unsigned target_offset;
  _Bool success;
  const struct uniqtype *matched_t;
  const struct uniqtype *array_run_t;
  unsigned array_run_start_offset;
  size_t array_run_nelems;
  _Bool array_run_unbounded;
};

static int bounds_cb (struct uniqtype *u, unsigned span_start_offset,
                      unsigned depth, struct uniqtype *containing,
                      struct uniqtype_rel_info *contained_pos,
                      unsigned containing_span_start_offset, void *arg_void) {
  struct bounds_cb_arg *arg = (struct bounds_cb_arg *)arg_void;
  (void)depth; (void)contained_pos;

  if (containing && UNIQTYPE_IS_ARRAY_TYPE(containing)) {
    if (!arg->array_run_t) {
      arg->array_run_t = containing;
      arg->array_run_start_offset = containing_span_start_offset;
      arg->array_run_nelems = UNIQTYPE_HAS_KNOWN_LENGTH(containing)
          ? (size_t)UNIQTYPE_ARRAY_LENGTH(containing) : 0;
      arg->array_run_unbounded = !UNIQTYPE_HAS_KNOWN_LENGTH(containing);
    }
    else {
      if (UNIQTYPE_HAS_KNOWN_LENGTH(containing))
        arg->array_run_nelems *= (size_t)UNIQTYPE_ARRAY_LENGTH(containing);
      else arg->array_run_unbounded = 1;
    }
  }
  else {
    arg->array_run_t = NULL;
    arg->array_run_start_offset = 0;
    arg->array_run_nelems = 0;
    arg->array_run_unbounded = 0;
  }

  if (span_start_offset < arg->target_offset)
    return 0;  /* keep going */
  if (span_start_offset > arg->target_offset)
    return 1;  /* overshot: target is not a subobject start offset */

  /* Keep going until we hit something of exactly the passed-in type's
     size (smaller: __like_a prefixing / de-facto unions; larger: the
     ordinary enclosing-object case). */
  if (u->pos_maxoff != arg->passed_in_t->pos_maxoff)
    return 0;

  arg->success = 1;
  arg->matched_t = u;
  return 1;  /* stop: descending further would clobber the run state */
}

static AllocsBounds make_bounds (unsigned long base, unsigned long limit) {
  AllocsBounds b;
  b.base = base;
  b.size = (unsigned)(limit - base);
  return b;
}

AllocsBounds luaA_fetchbounds (const void *obj, const struct uniqtype *t) {
  struct allocator *a = NULL;
  const void *alloc_start = NULL;
  unsigned long alloc_size_bytes = 0;
  struct uniqtype *alloc_uniqtype = (struct uniqtype *)0;
  const void *alloc_site = NULL;
  struct liballocs_err *err;

  if (!obj)
    goto return_min_bounds;
  if (&__liballocs_walk_subobjects_spanning_rec == NULL)
    goto return_min_bounds;  /* dummyweaks-only build: no walk available */

  err = __liballocs_get_alloc_info(obj, &a, &alloc_start, &alloc_size_bytes,
                                   &alloc_uniqtype, &alloc_site);
  if (__builtin_expect(err != NULL, 0))
    goto abort_returning_max_bounds;

  {
    unsigned target_offset_within_uniqtype =
        (unsigned)((char *)obj - (char *)alloc_start);

    if (alloc_start && alloc_size_bytes && !alloc_uniqtype) {
      alloc_uniqtype = &__uniqtype__signed_char;  /* pretend char allocation */
      if (alloc_uniqtype == NULL)  /* weak symbol absent */
        goto return_min_bounds;
    }
    if ((&__uniqtype__signed_char != NULL && t == &__uniqtype__signed_char)
        || (&__uniqtype__unsigned_char != NULL
            && t == &__uniqtype__unsigned_char))
      goto return_alloc_bounds;
    if (__builtin_expect(t->pos_maxoff == UNIQTYPE_POS_MAXOFF_UNBOUNDED, 0))
      goto return_min_bounds;

    {
      struct bounds_cb_arg arg = {
        .passed_in_t = t,
        .target_offset = target_offset_within_uniqtype,
      };
      int ret = __liballocs_walk_subobjects_spanning(
          target_offset_within_uniqtype, alloc_uniqtype, bounds_cb, &arg);
      (void)ret;
      if (arg.success) {
        if (arg.array_run_t) {
          const char *lower =
              (const char *)alloc_start + arg.array_run_start_offset;
          const char *upper = arg.array_run_unbounded
              ? (const char *)alloc_start + alloc_size_bytes
              : lower + arg.array_run_nelems * t->pos_maxoff;
          return make_bounds((unsigned long)lower, (unsigned long)upper);
        }
        else {
          char *limit = (char *)obj + (t->pos_maxoff > 0 ? t->pos_maxoff : 1);
          return make_bounds((unsigned long)obj, (unsigned long)limit);
        }
      }
      else {
        fprintf(stderr, "lallocs: no bounds for %p, target type %s, "
                "offset %d in allocation of %s at %p\n",
                obj, NAME_FOR_UNIQTYPE(t), target_offset_within_uniqtype,
                NAME_FOR_UNIQTYPE(alloc_uniqtype), alloc_start);
        goto return_min_bounds;
      }
    }
  }

return_min_bounds:
  return make_bounds((unsigned long)obj, (unsigned long)obj + 1);

return_alloc_bounds:
  {
    char *base = (char *)alloc_start;
    char *limit = (char *)alloc_start + alloc_size_bytes;
    unsigned long size = (unsigned long)(limit - base);
    if ((unsigned long)obj - (unsigned long)base > size)
      goto abort_returning_max_bounds;
    return make_bounds((unsigned long)base, (unsigned long)limit);
  }

abort_returning_max_bounds:
  fprintf(stderr, "lallocs: failed to fetch bounds for pointer %p; "
          "liballocs said %s (alloc site %p)\n", obj,
          (err && &__liballocs_errstring != NULL)
              ? __liballocs_errstring(err)
              : "no allocation found spanning queried pointer", alloc_site);
  return make_bounds((unsigned long)0, (unsigned long)-1);  /* MAX_BOUNDS */
}

#endif
