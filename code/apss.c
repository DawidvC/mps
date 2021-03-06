/* apss.c: AP MANUAL ALLOC STRESS TEST
 *
 * $Id$
 * Copyright (c) 2001-2016 Ravenbrook Limited.  See end of file for license.
 * Portions copyright (C) 2002 Global Graphics Software.
 */


#include "mpscmv.h"
#include "mpscmvff.h"
#include "mpscmvt.h"
#include "mpslib.h"
#include "mpsacl.h"
#include "mpsavm.h"

#include "testlib.h"
#include "mpslib.h"

#include <stdio.h> /* printf */
#include <stdlib.h> /* malloc */


#define testArenaSIZE   ((((size_t)3)<<24) - 4)
#define testSetSIZE 200
#define testLOOPS 10
#define MAX_ALIGN 64 /* TODO: Make this test work up to arena_grain_size? */


/* make -- allocate one object */

static mps_res_t make(mps_addr_t *p, mps_ap_t ap, size_t size)
{
  mps_res_t res;

  do {
    MPS_RESERVE_BLOCK(res, *p, ap, size);
    if(res != MPS_RES_OK)
      return res;
  } while(!mps_commit(ap, *p, size));

  return MPS_RES_OK;
}


/* check_allocated_size -- check the allocated size of the pool */

static void check_allocated_size(mps_pool_t pool, mps_ap_t ap, size_t allocated)
{
  size_t total_size = mps_pool_total_size(pool);
  size_t free_size = mps_pool_free_size(pool);
  size_t ap_free = (size_t)((char *)ap->limit - (char *)ap->init);
  Insist(total_size - free_size == allocated + ap_free);
}


/* stress -- create a pool of the requested type and allocate in it */

static mps_res_t stress(mps_arena_t arena, mps_pool_debug_option_s *options,
                        mps_align_t align,
                        size_t (*size)(size_t i, mps_align_t align),
                        const char *name, mps_pool_class_t pool_class,
                        mps_arg_s args[])
{
  mps_res_t res = MPS_RES_OK;
  mps_pool_t pool;
  mps_ap_t ap;
  size_t i, k;
  int *ps[testSetSIZE];
  size_t ss[testSetSIZE];
  size_t allocated = 0;         /* Total allocated memory */
  size_t debugOverhead = options ? 2 * alignUp(options->fence_size, align) : 0;

  printf("stress %s\n", name);

  die(mps_pool_create_k(&pool, arena, pool_class, args), "pool_create");
  die(mps_ap_create(&ap, pool, mps_rank_exact()), "BufferCreate");

  /* allocate a load of objects */
  for (i=0; i<testSetSIZE; ++i) {
    mps_addr_t obj;
    ss[i] = (*size)(i, align);
    res = make(&obj, ap, ss[i]);
    if (res != MPS_RES_OK)
      goto allocFail;
    ps[i] = obj;
    allocated += ss[i] + debugOverhead;
    if (ss[i] >= sizeof(ps[i]))
      *ps[i] = 1; /* Write something, so it gets swap. */
    check_allocated_size(pool, ap, allocated);
  }

  /* Check introspection functions */
  for (i = 0; i < NELEMS(ps); ++i) {
    mps_pool_t addr_pool = NULL;
    Insist(mps_arena_has_addr(arena, ps[i]));
    Insist(mps_addr_pool(&addr_pool, arena, ps[i]));
    Insist(addr_pool == pool);
  }

  mps_pool_check_fenceposts(pool);

  for (k=0; k<testLOOPS; ++k) {
    /* shuffle all the objects */
    for (i=0; i<testSetSIZE; ++i) {
      size_t j = rnd()%(testSetSIZE-i);
      void *tp;
      size_t ts;
     
      tp = ps[j]; ts = ss[j];
      ps[j] = ps[i]; ss[j] = ss[i];
      ps[i] = tp; ss[i] = ts;
    }
    /* free half of the objects */
    /* upper half, as when allocating them again we want smaller objects */
    /* see randomSize() */
    for (i=testSetSIZE/2; i<testSetSIZE; ++i) {
      mps_free(pool, (mps_addr_t)ps[i], ss[i]);
      /* if (i == testSetSIZE/2) */
      /*   PoolDescribe((Pool)pool, mps_lib_stdout); */
      Insist(ss[i] + debugOverhead <= allocated);
      allocated -= ss[i] + debugOverhead;
    }
    /* allocate some new objects */
    for (i=testSetSIZE/2; i<testSetSIZE; ++i) {
      mps_addr_t obj;
      ss[i] = (*size)(i, align);
      res = make(&obj, ap, ss[i]);
      if (res != MPS_RES_OK)
        goto allocFail;
      ps[i] = obj;
      allocated += ss[i] + debugOverhead;
    }
    check_allocated_size(pool, ap, allocated);
  }

allocFail:
  mps_ap_destroy(ap);
  mps_pool_destroy(pool);

  return res;
}


/* randomSizeAligned -- produce sizes both large and small, aligned to
 * align.
 */

static size_t randomSizeAligned(size_t i, mps_align_t align)
{
  size_t maxSize = 2 * 160 * 0x2000;
  /* Reduce by a factor of 2 every 10 cycles.  Total allocation about 40 MB. */
  return alignUp(rnd() % max((maxSize >> (i / 10)), 2) + 1, align);
}


static mps_pool_debug_option_s bothOptions = {
  /* .fence_template = */   "post",
  /* .fence_size = */       4,
  /* .free_template = */    "DEAD",
  /* .free_size = */        4
};

static mps_pool_debug_option_s fenceOptions = {
  /* .fence_template = */   "123456789abcdef",
  /* .fence_size = */       15,
  /* .free_template = */    NULL,
  /* .free_size = */        0
};


/* test -- create arena using given class and arguments; test all the
 * pool classes in this arena
 */

static void test(mps_arena_class_t arena_class, mps_arg_s arena_args[],
                 size_t arena_grain_size,
                 mps_pool_debug_option_s *options)
{
  mps_arena_t arena;
  die(mps_arena_create_k(&arena, arena_class, arena_args), "mps_arena_create");

  (void)arena_grain_size; /* TODO: test larger alignments up to this */

  MPS_ARGS_BEGIN(args) {
    mps_align_t align = rnd_align(sizeof(void *), MAX_ALIGN);
    MPS_ARGS_ADD(args, MPS_KEY_ALIGN, align);
    MPS_ARGS_ADD(args, MPS_KEY_MVFF_ARENA_HIGH, TRUE);
    MPS_ARGS_ADD(args, MPS_KEY_MVFF_SLOT_HIGH, TRUE);
    MPS_ARGS_ADD(args, MPS_KEY_MVFF_FIRST_FIT, TRUE);
    MPS_ARGS_ADD(args, MPS_KEY_SPARE, rnd_double());
    die(stress(arena, NULL, align, randomSizeAligned, "MVFF",
               mps_class_mvff(), args), "stress MVFF");
  } MPS_ARGS_END(args);

  MPS_ARGS_BEGIN(args) {
    mps_align_t align = rnd_align(sizeof(void *), MAX_ALIGN);
    MPS_ARGS_ADD(args, MPS_KEY_ALIGN, align);
    die(stress(arena, NULL, align, randomSizeAligned, "MV",
               mps_class_mv(), args), "stress MV");
  } MPS_ARGS_END(args);

  /* IWBN to test MVFFDebug, but the MPS doesn't support debugging
     APs, yet.  MV Debug used to work here, because it faked it
     through PoolAlloc, but MV Debug is now deprecated and replaced by
     MVFF Debug.  See job003995. */
  (void)options;

  MPS_ARGS_BEGIN(args) {
    mps_align_t align = rnd_align(sizeof(void *), MAX_ALIGN);
    MPS_ARGS_ADD(args, MPS_KEY_ALIGN, align);
    die(stress(arena, NULL, align, randomSizeAligned, "MVT",
               mps_class_mvt(), args), "stress MVT");
  } MPS_ARGS_END(args);

  /* Manual allocation should not cause any garbage collections. */
  Insist(mps_collections(arena) == 0);
  mps_arena_destroy(arena);
}


int main(int argc, char *argv[])
{
  size_t arena_grain_size;
  
  testlib_init(argc, argv);

  arena_grain_size = rnd_grain(testArenaSIZE);
  MPS_ARGS_BEGIN(args) {
    MPS_ARGS_ADD(args, MPS_KEY_ARENA_SIZE, 2 * testArenaSIZE);
    MPS_ARGS_ADD(args, MPS_KEY_ARENA_GRAIN_SIZE, arena_grain_size);
    MPS_ARGS_ADD(args, MPS_KEY_COMMIT_LIMIT, testArenaSIZE);
    test(mps_arena_class_vm(), args, arena_grain_size, &fenceOptions);
  } MPS_ARGS_END(args);

  arena_grain_size = rnd_grain(2 * testArenaSIZE);
  MPS_ARGS_BEGIN(args) {
    MPS_ARGS_ADD(args, MPS_KEY_ARENA_SIZE, 2 * testArenaSIZE);
    MPS_ARGS_ADD(args, MPS_KEY_ARENA_ZONED, FALSE);
    MPS_ARGS_ADD(args, MPS_KEY_ARENA_GRAIN_SIZE, arena_grain_size);
    test(mps_arena_class_vm(), args, arena_grain_size, &bothOptions);
  } MPS_ARGS_END(args);

  arena_grain_size = rnd_grain(testArenaSIZE);
  MPS_ARGS_BEGIN(args) {
    MPS_ARGS_ADD(args, MPS_KEY_ARENA_SIZE, testArenaSIZE);
    MPS_ARGS_ADD(args, MPS_KEY_ARENA_ZONED, FALSE);
    MPS_ARGS_ADD(args, MPS_KEY_ARENA_CL_BASE, malloc(testArenaSIZE));
    MPS_ARGS_ADD(args, MPS_KEY_ARENA_GRAIN_SIZE, arena_grain_size);
    test(mps_arena_class_cl(), args, arena_grain_size, &bothOptions);
  } MPS_ARGS_END(args);

  printf("%s: Conclusion: Failed to find any defects.\n", argv[0]);
  return 0;
}


/* C. COPYRIGHT AND LICENSE
 *
 * Copyright (c) 2001-2016 Ravenbrook Limited <http://www.ravenbrook.com/>.
 * All rights reserved.  This is an open source license.  Contact
 * Ravenbrook for commercial licensing options.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * 3. Redistributions in any form must be accompanied by information on how
 * to obtain complete source code for this software and any accompanying
 * software that uses this software.  The source code must either be
 * included in the distribution or be available for no more than the cost
 * of distribution plus a nominal fee, and must be freely redistributable
 * under reasonable conditions.  For an executable file, complete source
 * code means the source code for all modules it contains. It does not
 * include source code for modules or files that typically accompany the
 * major components of the operating system on which the executable file
 * runs.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, OR NON-INFRINGEMENT, ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS AND CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
