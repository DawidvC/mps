/* impl.c.seg: SEGMENTS
 *
 * $HopeName: MMsrc!seg.c(MMdevel_tony_sunset.2) $
 * Copyright (C) 1998 Harlequin Group plc.  All rights reserved.
 *
 * .design: The design for this module is design.mps.seg.
 *
 * PURPOSE
 *
 * .purpose: This is the implementation of the generic segment interface.
 * It defines the interface functions and two useful segment classes:-
 * .purpose.class.seg: Class Seg is a class which is as simple
 * as efficiency demands permit. (It includes fields for storing colour
 * for efficiency). It may be subclassed by clients of the module.
 * .purpose.class.seg-gc: Class SegGC is a concrete class support all
 * all current GC features, and providing full backwards compatibility
 * with "old-style" segments. It may be subclassed by clients of the
 *  module. 
 *
 * TRANSGRESSIONS
 *
 * .check.shield: The "pm", "sm", and "depth" fields are not checked by
 * SegCheck, because I haven't spent time working out the invariants.
 * We should certainly work them out, by studying impl.c.shield, and
 * assert things about shielding, protection, shield cache consistency,
 * etc. richard 1997-04-03
 */

#include "mpm.h"

SRCID(seg, "$HopeName: MMsrc!seg.c(MMdevel_tony_sunset.2) $");


/* SegSegGC -- convert generic Seg to SegGC */

#define SegSegGC(seg)             ((SegGC)(seg))

/* SegPoolRing -- Pool ring accessor */

#define SegPoolRing(gcseg)        (&(gcseg)->poolRing)



/* Generic interface support */


/* SegAlloc -- allocate a segment from the arena
 *
 * The allocated segment is of class SegGC - and is fully
 * compatible with "historic" segments.
 */

Res SegAlloc(Seg *segReturn, SegPref pref, Size size, Pool pool,
             Bool withReservoirPermit)
{
  return SegOfClassAlloc(segReturn, EnsureSegGCClass(), pref,
                         size, pool, withReservoirPermit);
}


/* SegOfClassAlloc -- allocate a segment from the arena */

Res SegOfClassAlloc(Seg *segReturn, SegClass class, SegPref pref, 
                    Size size, Pool pool,
                    Bool withReservoirPermit)
{
  Res res;
  Arena arena;
  Seg seg;
  Addr base;

  AVER(segReturn != NULL);
  AVERT(SegClass, class);
  AVERT(SegPref, pref);
  AVER(size > (Size)0);
  AVERT(Pool, pool);
  AVER(BoolCheck(withReservoirPermit));

  arena = PoolArena(pool);
  AVERT(Arena, arena);
  AVER(SizeIsAligned(size, arena->alignment));

  /* allocate the memory from the arena */
  res = ArenaAlloc(&base, pref, size, pool, withReservoirPermit);
  if(res != ResOK) {
    goto failArena;
  }

  /* allocate the segment object from the control pool */
  res = ControlAlloc((void **)&seg, arena, class->size,
                     withReservoirPermit);
  if(res != ResOK) {
    goto failControl;
  }

  seg->class = class;
  SegInit(seg, pool, base, size);

  EVENT_PPAWP(SegAlloc, arena, seg, SegBase(seg), size, pool);
  *segReturn = seg;
  return ResOK;

failControl:
  ArenaFree(base, size, pool);
failArena:
  EVENT_PWP(SegAllocFail, arena, size, pool);
  return res;
}


/* SegFree -- free a segment to the arena */

void SegFree(Seg seg)
{
  Arena arena;
  Pool pool;
  Addr base;
  Size size;
  SegClass class;

  AVERT(Seg, seg);
  pool = SegPool(seg);
  AVERT(Pool, pool);
  arena = PoolArena(pool);
  AVERT(Arena, arena);
  base = SegBase(seg);
  size = SegSize(seg);
  class = seg->class;

  SegFinish(seg);
  ControlFree(arena, seg, class->size);
  ArenaFree(base, size, pool);

  EVENT_PP(SegFree, arena, seg);
  return;
}


/* SegInit -- initialize a segment */

void SegInit(Seg seg, Pool pool, Addr base, Size size)
{
  Tract tract;
  Addr addr, limit;
  Size align;
  Arena arena;
  SegClass class;

  AVER(seg != NULL);
  AVERT(Pool, pool);
  arena = PoolArena(pool);
  align = ArenaAlign(arena);
  AVER(AddrIsAligned(base, align));
  AVER(SizeIsAligned(size, align));
  class = seg->class;
  AVERT(SegClass, class);

  limit = AddrAdd(base, size);
  seg->limit = limit;
  seg->rankSet = RankSetEMPTY;
  seg->white = TraceSetEMPTY;
  seg->nailed = TraceSetEMPTY;
  seg->grey = TraceSetEMPTY;
  seg->pm = AccessSetEMPTY;
  seg->sm = AccessSetEMPTY;
  seg->depth = 0;
  seg->firstTract = NULL;

  seg->sig = SegSig;  /* set sig now so tract checks will see it */

  TRACT_FOR(tract, addr, arena, base, limit) {
    AVER(NULL == TractSeg(tract));
    AVER(!TractHasSeg(tract));
    AVER(TractPool(tract) == pool);
    AVER(TractWhite(tract) == TraceSetEMPTY);
    tract->hasSeg = 1;
    TractSetP(tract, seg);
    if (addr == base) {
      AVER(seg->firstTract == NULL);
      seg->firstTract = tract;
    }
    AVER(seg->firstTract != NULL);
  }
  AVER(addr == seg->limit);

  /* Class specific initialization cames last */
  class->init(seg, pool, base, size);
    
  AVERT(Seg, seg);
}


/* SegFinish -- finish the generic part of a segment */

void SegFinish(Seg seg)
{
  Arena arena;
  Addr addr, base, limit;
  Tract tract;
  SegClass class;

  AVERT(Seg, seg);
  class = seg->class;
  AVERT(SegClass, class);

  arena = PoolArena(SegPool(seg));
  if(seg->sm != AccessSetEMPTY) {
    ShieldLower(arena, seg, seg->sm);
  }

  /* Class specific finishing cames first */
  class->finish(seg);

  seg->rankSet = RankSetEMPTY;

  /* See impl.c.shield.shield.flush */
  ShieldFlush(PoolArena(SegPool(seg)));

  base = SegBase(seg);
  limit = SegLimit(seg);
  TRACT_FOR(tract, addr, arena, base, limit) {
    tract->white = TraceSetEMPTY;
    tract->hasSeg = 0;
    TractSetP(tract, NULL);
  }
  AVER(addr == seg->limit);

  seg->sig = SigInvalid;

  /* Check that the segment is not exposed, or in the shield */
  /* cache (see impl.c.shield.def.depth). */
  AVER(seg->depth == 0);
  /* Check not shielded or protected (so that pages in hysteresis */
  /* fund are not protected) */
  AVER(seg->sm == AccessSetEMPTY);
  AVER(seg->pm == AccessSetEMPTY);
  
}


/* SegSetGrey -- change the greyness of a segment
 *
 * Sets the segment greyness to the trace set grey.
 */

void SegSetGrey(Seg seg, TraceSet grey)
{
  AVERT(Seg, seg);
  AVER(TraceSetCheck(grey));
  seg->class->setGrey(seg, grey);
}


/* SegSetWhite -- change the whiteness of a segment
 *
 * Sets the segment whiteness to the trace set ts.
 */

void SegSetWhite(Seg seg, TraceSet white)
{
  AVERT(Seg, seg);
  AVER(TraceSetCheck(white));
  seg->class->setWhite(seg, white);
}


/* SegSetRankSet -- set the rank set of a segment
 *
 * The caller must set the summary to empty before setting the rank
 * set to empty.  The caller must set the rank set to non-empty before
 * setting the summary to non-empty.
 */

void SegSetRankSet(Seg seg, RankSet rankSet)
{
  AVERT(Seg, seg);
  AVER(RankSetCheck(rankSet));
  seg->class->setRankSet(seg, rankSet);
}


/* SegSummary -- return the summary of a segment */

RefSet SegSummary(Seg seg)
{
  AVERT(Seg, seg);
  return seg->class->summary(seg);
}


/* SegSetSummary -- change the summary on a segment */

void SegSetSummary(Seg seg, RefSet summary)
{
  AVERT(Seg, seg);
  seg->class->setSummary(seg, summary);
}


/* SegSetRankAndSummary -- set the rank set and summary together */

void SegSetRankAndSummary(Seg seg, RankSet rankSet, RefSet summary)
{
  AVERT(Seg, seg);
  AVER(RankSetCheck(rankSet));
  seg->class->setRankSummary(seg, rankSet, summary);
}


/* SegBuffer -- return the buffer of a segment */

Buffer SegBuffer(Seg seg)
{
  AVERT(Seg, seg);
  return seg->class->buffer(seg);
}


/* SegSetBuffer -- change the buffer on a segment */

void SegSetBuffer(Seg seg, Buffer buffer)
{
  AVERT(Seg, seg);
  if (buffer != NULL)
    AVERT(Buffer, buffer);
  seg->class->setBuffer(seg, buffer);
}


/* SegP -- return the P field of a segment */

void *SegP(Seg seg)
{
  AVERT(Seg, seg);
  return seg->class->p(seg);
}


/* SegSetP -- set the P fielf of a segment */

void SegSetP(Seg seg, void *p)
{
  AVERT(Seg, seg);
  seg->class->setP(seg, p);
}


/* SegDescribe -- describe a segment */

Res SegDescribe(Seg seg, mps_lib_FILE *stream)
{
  Res res;
  Pool pool = SegPool(seg);
  AVERT(Seg, seg);

  res = WriteF(stream,
               "Segment $P [$A,$A) {\n", (WriteFP)seg,
               (WriteFA)SegBase(seg), (WriteFA)SegLimit(seg),
               "  pool $P ($U)\n",
               (WriteFP)pool, (WriteFU)pool->serial,
               NULL);
  if(res != ResOK)
    return res;

  res = seg->class->describe(seg, stream);
  if(res != ResOK)
    return res;

  res = WriteF(stream, "\n",
               "} Segment $P\n", (WriteFP)seg, NULL);
  return res;
}


/* .seg.critical: These seg functions are low-level and used 
 * through-out. They are therefore on the critical path and their 
 * AVERs are so-marked.
 */

/* SegBase -- return the base address of a seg */

Addr SegBase(Seg seg)
{
  AVERT_CRITICAL(Seg, seg); /* .seg.critical */
  return TractBase(seg->firstTract);
}


/* SegLimit -- return the limit address of a segment */

Addr SegLimit(Seg seg)
{
  AVERT_CRITICAL(Seg, seg); /* .seg.critical */
  return seg->limit;
}


/* SegSize -- return the size of a seg */

Size SegSize(Seg seg)
{
  AVERT_CRITICAL(Seg, seg); /* .seg.critcial */
  return AddrOffset(TractBase(seg->firstTract), seg->limit);
}


/* SegOfTract -- safely return the seg of a tract, if any */

static Bool SegOfTract(Seg *segReturn, Tract tract)
{
  AVERT_CRITICAL(Tract, tract);
  AVER_CRITICAL(segReturn != NULL);
  if (TractHasSeg(tract)) {
    Seg seg = TractSeg(tract);
    AVERT_CRITICAL(Seg, seg);
    *segReturn = seg;
    return TRUE;
  } else {
    return FALSE;
  }
}
 

/* SegOfAddr -- return the seg the given address is in, if any */

Bool SegOfAddr(Seg *segReturn, Arena arena, Addr addr)
{
  Tract tract;
  AVER(segReturn != NULL);
  AVERT(Arena, arena);
  if (TractOfAddr(&tract, arena, addr)) {
    return SegOfTract(segReturn, tract);
  } else {
    return FALSE;
  }
}


/* SegFirst -- return the first seg in the arena
 *
 * This is used to start an iteration over all segs in the arena.
 */

Bool SegFirst(Seg *segReturn, Arena arena)
{
  Tract tract;
  AVER(segReturn != NULL);
  AVERT(Arena, arena);

  if (TractFirst(&tract, arena)) {
    do {
      Seg seg;
      if (SegOfTract(&seg, tract)) {
        *segReturn = seg;
        return TRUE;
      } 
    } while (TractNext(&tract, arena, TractBase(tract)));
  } 
  return FALSE;
}


/* SegNext -- return the "next" seg in the arena
 *
 * This is used as the iteration step when iterating over all
 * segs in the arena.
 *
 * SegNext finds the seg with the lowest base address which is
 * greater than a specified address.  The address must be (or once
 * have been) the base address of a seg.
 */

Bool SegNext(Seg *segReturn, Arena arena, Addr addr)
{
  Tract tract;
  Addr base = addr;
  AVER_CRITICAL(segReturn != NULL); /* .seg.critical */
  AVERT_CRITICAL(Arena, arena);

  while (TractNext(&tract, arena, base)) {
    Seg seg;
    if (SegOfTract(&seg, tract)) {
      if (tract == seg->firstTract) {
        *segReturn = seg;
        return TRUE;
      } else {
        /* found the next tract in a large segment */
        /* base & addr must be the base of this segment */
        AVER(TractBase(seg->firstTract) == addr);
        AVER(addr == base);
        /* set base to the last tract in the segment */
        base = AddrSub(seg->limit, ArenaAlign(arena));
        AVER(base > addr);
      }
    } else {
      base = TractBase(tract);
    }
  } 
  return FALSE;
}



/* Class Seg -- The most basic segment class
 */


/* SegCheck -- check the integrity of a segment */

Bool SegCheck(Seg seg)
{
  Tract tract;
  Arena arena;
  Pool pool;
  Addr addr;
  Size align;
  
  CHECKS(Seg, seg);
  CHECKL(TraceSetCheck(seg->white));

  /* can't assume nailed is subset of white - mightn't be during whiten */
  /* CHECKL(TraceSetSub(seg->nailed, seg->white)); */
  CHECKL(TraceSetCheck(seg->grey));
  CHECKL(TractCheck(seg->firstTract));  /* design.mps.check.type.no-sig */
  pool = TractPool(seg->firstTract);
  CHECKD(Pool, pool);
  arena = PoolArena(pool);
  align = ArenaAlign(arena);
  CHECKL(AddrIsAligned(TractBase(seg->firstTract), align));
  CHECKL(AddrIsAligned(seg->limit, align));
  CHECKL(seg->limit > TractBase(seg->firstTract));

  /* Each tract of the segment must agree about white traces */
  TRACT_TRACT_FOR(tract, addr, arena, seg->firstTract, seg->limit) {
    CHECKL(TractHasSeg(tract) == 1);
    CHECKL(TractSeg(tract) == seg);
    CHECKL(TractWhite(tract) == seg->white);
    CHECKL(TractPool(tract) == pool);
  }
  CHECKL(addr == seg->limit);
    
  /* "pm", "sm", and "depth" not checked.  See .check.shield. */
  CHECKL(RankSetCheck(seg->rankSet));
  if(seg->rankSet == RankSetEMPTY) {
    /* design.mps.seg.field.rankSet.empty: If there are no refs */
    /* in the segment then it cannot contain black or grey refs. */
    CHECKL(seg->grey == TraceSetEMPTY);
    CHECKL(seg->sm == AccessSetEMPTY);
    CHECKL(seg->pm == AccessSetEMPTY);
  } else {
    /* design.mps.seg.field.rankSet.single: The Tracer only permits */
    /* one rank per segment [ref?] so this field is either empty or a */
    /* singleton. */
    CHECKL(RankSetIsSingle(seg->rankSet));
    /* Can't check barrier invariants because SegCheck is called */
    /* when raising or lowering the barrier. */
    /* .check.wb: If summary isn't universal then it must be */
    /* write shielded. */
    /* CHECKL(seg->_summary == RefSetUNIV || (seg->_sm & AccessWRITE)); */
    /* @@@@ What can be checked about the read barrier? */
  }
  return TRUE;
}


/* segTrivInit -- method to initialize the generic part of a segment */

static void segTrivInit(Seg seg, Pool pool, Addr base, Size size)
{
  /* all the initialization happens in SegInit so checks are safe */
  Size align;
  Arena arena;

  AVERT(Seg, seg);
  AVERT(Pool, pool);
  arena = PoolArena(pool);
  align = ArenaAlign(arena);
  AVER(AddrIsAligned(base, align));
  AVER(SizeIsAligned(size, align));
  AVER(SegBase(seg) == base);
  AVER(SegSize(seg) == size);
  AVER(SegPool(seg) == pool);
}


/* segTrivFinish -- finish the generic part of a segment */

static void segTrivFinish(Seg seg)
{
  /* all the generic finishing happens in SegFinish  */
  AVERT(Seg, seg);
}


/* segNoSetGrey -- non-method to change the greyness of a segment */

static void segNoSetGrey(Seg seg, TraceSet grey)
{
  AVERT(Seg, seg);
  AVER(TraceSetCheck(grey));
  AVER(seg->rankSet != RankSetEMPTY);
  NOTREACHED;
}


/* segNoSetWhite -- non-method to change the whiteness of a segment */

static void segNoSetWhite(Seg seg, TraceSet white)
{
  AVERT(Seg, seg);
  AVER(TraceSetCheck(white));
  NOTREACHED;
}


/* segNoSetRankSet -- non-method to set the rank set of a segment */

static void segNoSetRankSet(Seg seg, RankSet rankSet)
{
  AVERT(Seg, seg);
  AVER(RankSetCheck(rankSet));
  NOTREACHED;
}


/* segNoSummary -- non-method to return the summary of a segment */

static RefSet segNoSummary(Seg seg)
{
  AVERT(Seg, seg);
  NOTREACHED;
  return RefSetEMPTY;
}


/* segNoSetSummary -- non-method to set the summary of a segment */

static void segNoSetSummary(Seg seg, RefSet summary)
{
  AVERT(Seg, seg);
  UNUSED(summary);
  NOTREACHED;
}


/* segNoSetRankSummary -- non-method to set the rank set & summary */

static void segNoSetRankSummary(Seg seg, RankSet rankSet, RefSet summary)
{
  AVERT(Seg, seg);
  AVER(RankSetCheck(rankSet));
  UNUSED(summary);
  NOTREACHED;
}


/* segNoBuffer -- non-method to return the buffer of a segment */

static Buffer segNoBuffer(Seg seg)
{
  AVERT(Seg, seg);
  NOTREACHED;
  return (Buffer)0;
}


/* segNoSetBuffer -- non-method to set the buffer of a segment */

static void segNoSetBuffer(Seg seg, Buffer buffer)
{
  AVERT(Seg, seg);
  if (buffer != NULL)
    AVERT(Buffer, buffer);
  NOTREACHED;
}


/* segNoP -- non-method to return the P field of a segment */

static void *segNoP(Seg seg)
{
  AVERT(Seg, seg);
  NOTREACHED;
  return (void *)0;
}


/* segNoSetP -- non-method to set the P field of a segment */

static void segNoSetP(Seg seg, void *p)
{
  AVERT(Seg, seg);
  UNUSED(p);;
  NOTREACHED;
}


/* segTrivDescribe -- Basic Seg description method */

static Res segTrivDescribe(Seg seg, mps_lib_FILE *stream)
{
  Res res;

  AVERT(Seg, seg);

  res = WriteF(stream,
               "  shield depth $U\n", (WriteFU)seg->depth,
               "  protection mode:",
               NULL);
  if(res != ResOK)
    return res;
  if(AccessSetIsMember(seg->pm, AccessREAD)) {
     res = WriteF(stream, " read", NULL);
     if(res != ResOK)
       return res;
  }
  if(AccessSetIsMember(seg->pm, AccessWRITE)) {
     res = WriteF(stream, " write", NULL);
     if(res != ResOK)
       return res;
  }
  res = WriteF(stream, "\n  shield mode:", NULL);
  if(res != ResOK)
    return res;
  if(AccessSetIsMember(seg->sm, AccessREAD)) {
     res = WriteF(stream, " read", NULL);
     if(res != ResOK)
       return res;
  }
  if(AccessSetIsMember(seg->sm, AccessWRITE)) {
     res = WriteF(stream, " write", NULL);
     if(res != ResOK)
       return res;
  }
  res = WriteF(stream, "\n  ranks:", NULL);
  /* This bit ought to be in a RankSetDescribe in ref.c. */
  if(RankSetIsMember(seg->rankSet, RankAMBIG)) {
     res = WriteF(stream, " ambiguous", NULL);
     if(res != ResOK)
       return res;
  }
  if(RankSetIsMember(seg->rankSet, RankEXACT)) {
     res = WriteF(stream, " exact", NULL);
     if(res != ResOK)
       return res;
  }
  if(RankSetIsMember(seg->rankSet, RankFINAL)) {
     res = WriteF(stream, " final", NULL);
     if(res != ResOK)
       return res;
  }
  if(RankSetIsMember(seg->rankSet, RankWEAK)) {
     res = WriteF(stream, " weak", NULL);
     if(res != ResOK)
       return res;
  }
  res = WriteF(stream, "\n",
               "  white  $B\n", (WriteFB)seg->white,
               "  grey   $B\n", (WriteFB)seg->grey,
               "  nailed $B\n", (WriteFB)seg->nailed,
               NULL);
  return res;
}


/* Class SegGC -- Segment class with GC support
 */


/* SegGCCheck -- check the integrity of a SegGC */

Bool SegGCCheck(SegGC gcseg)
{
  Seg seg;
  CHECKS(SegGC, gcseg);
  seg = &gcseg->segStruct;
  CHECKL(SegCheck(seg));

  if(gcseg->buffer != NULL) {
    CHECKU(Buffer, gcseg->buffer);
    /* design.mps.seg.field.buffer.owner */
    CHECKL(BufferPool(gcseg->buffer) == SegPool(seg));
  }

  /* The segment must belong to some pool, so it should be on a */
  /* pool's segment ring.  (Actually, this isn't true just after */
  /* the segment is initialized.) */
  /*  CHECKL(RingNext(&gcseg->poolRing) != &gcseg->poolRing); */

  CHECKL(RingCheck(&gcseg->poolRing));

  /* The segment should be on a grey ring if and only if it is grey. */
  CHECKL(RingCheck(&gcseg->greyRing));
  CHECKL((seg->grey == TraceSetEMPTY) ==
         RingIsSingle(&gcseg->greyRing));

  if(seg->rankSet == RankSetEMPTY) {
    /* design.mps.seg.field.rankSet.empty: If there are no refs */
    /* in the segment then it cannot contain black or grey refs. */
    CHECKL(gcseg->summary == RefSetEMPTY);
  } 

  return TRUE;
}


/* segGCInit -- method to initialize a GC segment */

static void segGCInit(Seg seg, Pool pool, Addr base, Size size)
{
  SegClass super;
  SegGC gcseg;
  Arena arena;
  Align align;

  AVERT(Seg, seg);
  AVERT(Pool, pool);
  arena = PoolArena(pool);
  align = ArenaAlign(arena);
  AVER(AddrIsAligned(base, align));
  AVER(SizeIsAligned(size, align));
  gcseg = SegSegGC(seg);
  AVER(&gcseg->segStruct == seg);

  /* Initialize the superclass fields first via next-method call */
  super = EnsureSegClass();
  super->init(seg, pool, base, size);

  gcseg->summary = RefSetEMPTY;
  gcseg->buffer = NULL;
  RingInit(&gcseg->poolRing);
  RingInit(&gcseg->greyRing);
  gcseg->p = NULL;
  gcseg->sig = SegGCSig;

  AVERT(SegGC, gcseg);
  RingAppend(&pool->segRing, SegPoolRing(gcseg));
}


/* segGCFinish -- finish a GC segment */

static void segGCFinish(Seg seg)
{
  SegClass super;
  SegGC gcseg;

  AVERT(Seg, seg);
  gcseg = SegSegGC(seg);
  AVERT(SegGC, gcseg);
  AVER(&gcseg->segStruct == seg);

  if(SegGrey(seg) != TraceSetEMPTY) {
    RingRemove(&gcseg->greyRing);
    seg->grey = TraceSetEMPTY;
  }
  gcseg->summary = RefSetEMPTY;

  gcseg->sig = SigInvalid;

  /* Don't leave a dangling buffer allocating into hyperspace. */
  AVER(gcseg->buffer == NULL);

  RingFinish(&gcseg->greyRing);
  RingRemove(SegPoolRing(gcseg));
  RingFinish(SegPoolRing(gcseg));

  /* finish the superclass fields last */
  super = EnsureSegClass();
  super->finish(seg);
}



/* segGCSetGrey -- SegGC method to change the greyness of a segment
 *
 * Sets the segment greyness to the trace set grey and adjusts
 * the shielding on the segment appropriately.
 */

static void segGCSetGrey(Seg seg, TraceSet grey)
{
  SegGC gcseg;
  Arena arena;
  TraceSet oldGrey, flippedTraces;
  Rank rank;
  
  AVERT(Seg, seg);
  AVER(TraceSetCheck(grey));
  AVER(seg->rankSet != RankSetEMPTY);
  gcseg = SegSegGC(seg);
  AVERT(SegGC, gcseg);
  AVER(&gcseg->segStruct == seg);

  arena = PoolArena(SegPool(seg));
  oldGrey = seg->grey;
  seg->grey = grey;

  /* If the segment is now grey and wasn't before, add it to the */
  /* appropriate grey list so that TraceFindGrey can locate it */
  /* quickly later.  If it is no longer grey and was before, */
  /* remove it from the list. */
  if(oldGrey == TraceSetEMPTY) {
    if(grey != TraceSetEMPTY) {
      AVER(RankSetIsSingle(seg->rankSet));
      for(rank = 0; rank < RankMAX; ++rank)
	if(RankSetIsMember(seg->rankSet, rank)) {
	  RingInsert(ArenaGreyRing(arena, rank), &gcseg->greyRing);
	  break;
	}
      AVER(rank != RankMAX); /* there should've been a match */
    }
  } else {
    if(grey == TraceSetEMPTY)
      RingRemove(&gcseg->greyRing);
  }

  /* The read barrier is raised when the segment is grey for */
  /* some _flipped_ trace, i.e. is grey for a trace for which */
  /* the mutator is black. */
  flippedTraces = arena->flippedTraces;
  if(TraceSetInter(oldGrey, flippedTraces) == TraceSetEMPTY) {
    if(TraceSetInter(grey, flippedTraces) != TraceSetEMPTY)
      ShieldRaise(arena, seg, AccessREAD);
  } else {
    if(TraceSetInter(grey, flippedTraces) == TraceSetEMPTY)
      ShieldLower(arena, seg, AccessREAD);
  }

  EVENT_PPU(SegSetGrey, arena, seg, grey);
}


/* segGCSetWhite -- SegGC method to change whiteness of a segment
 *
 * Sets the segment whiteness to the trace set ts.
 */

static void segGCSetWhite(Seg seg, TraceSet white)
{
  SegGC gcseg;
  Tract tract;
  Arena arena;
  Addr addr, limit;

  AVERT(Seg, seg);
  AVER(TraceSetCheck(white));
  gcseg = SegSegGC(seg);
  AVERT(SegGC, gcseg);
  AVER(&gcseg->segStruct == seg);

  arena = PoolArena(SegPool(seg));
  limit = SegLimit(seg);

  /* Each tract of the segment records white traces */
  TRACT_TRACT_FOR(tract, addr, arena, seg->firstTract, limit) {
    AVER(TractHasSeg(tract));
    AVER(TractSeg(tract) == seg);
    tract->white = white;
  }
  AVER(addr == limit);
  seg->white = white;
}


/* segGCSetRankSet -- SegGC method to set the rank set of a segment
 *
 * If the rank set is made non-empty then the segment's summary is
 * now a subset of the mutator's (which is assumed to be RefSetUNIV)
 * so the write barrier must be imposed on the segment.  If the
 * rank set is made empty then there are no longer any references
 * on the segment so the barrier is removed.
 *
 * The caller must set the summary to empty before setting the rank
 * set to empty.  The caller must set the rank set to non-empty before
 * setting the summary to non-empty.
 */

static void segGCSetRankSet(Seg seg, RankSet rankSet)
{
  SegGC gcseg;
  RankSet oldRankSet;
  Arena arena;

  AVERT(Seg, seg);
  AVER(RankSetCheck(rankSet));
  AVER(rankSet == RankSetEMPTY || RankSetIsSingle(rankSet));
  gcseg = SegSegGC(seg);
  AVERT(SegGC, gcseg);
  AVER(&gcseg->segStruct == seg);

  arena = PoolArena(SegPool(seg));
  oldRankSet = seg->rankSet;
  seg->rankSet = rankSet;

  if(oldRankSet == RankSetEMPTY) {
    if(rankSet != RankSetEMPTY) {
      AVER(gcseg->summary == RefSetEMPTY);
      ShieldRaise(arena, seg, AccessWRITE);
    }
  } else {
    if(rankSet == RankSetEMPTY) {
      AVER(gcseg->summary == RefSetEMPTY);
      ShieldLower(arena, seg, AccessWRITE);
    }
  }
}


/* segGCSummary -- SegGC method to return the summary of a segment */

static RefSet segGCSummary(Seg seg)
{
  SegGC gcseg;

  AVERT(Seg, seg);
  gcseg = SegSegGC(seg);
  AVERT(SegGC, gcseg);
  AVER(&gcseg->segStruct == seg);

  return gcseg->summary;
}

/* segGCSetSummary -- SegGC method to change the summary on a segment
 *
 * In fact, we only need to raise the write barrier if the
 * segment contains references, and its summary is strictly smaller 
 * than the summary of the unprotectable data (i.e. the mutator).
 * We don't maintain such a summary, assuming that the mutator can 
 * access all references, so its summary is RefSetUNIV.
 */

static void segGCSetSummary(Seg seg, RefSet summary)
{
  SegGC gcseg;
  RefSet oldSummary;
  Arena arena;

  AVERT(Seg, seg);
  gcseg = SegSegGC(seg);
  AVERT(SegGC, gcseg);
  AVER(&gcseg->segStruct == seg);

  arena = PoolArena(SegPool(seg));
  oldSummary = gcseg->summary;
  gcseg->summary = summary;

  AVER(seg->rankSet != RankSetEMPTY);

  /* Note: !RefSetSuper is a test for a strict subset */
  if(!RefSetSuper(summary, RefSetUNIV)) {
    if(RefSetSuper(oldSummary, RefSetUNIV))
      ShieldRaise(arena, seg, AccessWRITE);
  } else {
    if(!RefSetSuper(oldSummary, RefSetUNIV))
      ShieldLower(arena, seg, AccessWRITE);
  }
}


/* segGCSetRankSummary -- SegGC method to set the rank set 
 * and summary together 
 */

static void segGCSetRankSummary(Seg seg, RankSet rankSet, RefSet summary)
{
  SegGC gcseg;
  Bool wasShielded, willbeShielded;
  Arena arena;

  AVERT(Seg, seg);
  AVER(RankSetCheck(rankSet));
  AVER(rankSet == RankSetEMPTY || RankSetIsSingle(rankSet));
  gcseg = SegSegGC(seg);
  AVERT(SegGC, gcseg);
  AVER(&gcseg->segStruct == seg);

  /* rankSet == RankSetEMPTY implies summary == RefSetEMPTY */
  AVER(rankSet != RankSetEMPTY || summary == RefSetEMPTY);

  arena = PoolArena(SegPool(seg));

  if(seg->rankSet != RankSetEMPTY && gcseg->summary != RefSetUNIV) {
    wasShielded = TRUE;
  } else {
    wasShielded = FALSE;
  }

  if(rankSet != RankSetEMPTY && summary != RefSetUNIV) {
    willbeShielded = TRUE;
  } else {
    willbeShielded = FALSE;
  }

  seg->rankSet = rankSet;
  gcseg->summary = summary;

  if(willbeShielded && !wasShielded) {
    ShieldRaise(arena, seg, AccessWRITE);
  } else if(wasShielded && !willbeShielded) {
    ShieldLower(arena, seg, AccessWRITE);
  }
}


/* segGCBuffer -- SegGC method to return the buffer of a segment */

static Buffer segGCBuffer(Seg seg)
{
  SegGC gcseg;

  AVERT(Seg, seg);
  gcseg = SegSegGC(seg);
  AVERT(SegGC, gcseg);
  AVER(&gcseg->segStruct == seg);

  return gcseg->buffer;
}


/* segGCSetBuffer -- SegGC method to change the buffer of a segment */

static void segGCSetBuffer(Seg seg, Buffer buffer)
{
  SegGC gcseg;

  AVERT(Seg, seg);
  if (buffer != NULL)
    AVERT(Buffer, buffer);
  gcseg = SegSegGC(seg);
  AVERT(SegGC, gcseg);
  AVER(&gcseg->segStruct == seg);

  gcseg->buffer = buffer;
}


/* segGCP -- SegGC method to return the P field of a segment */

static void *segGCP(Seg seg)
{
  SegGC gcseg;

  AVERT(Seg, seg);
  gcseg = SegSegGC(seg);
  AVERT(SegGC, gcseg);
  AVER(&gcseg->segStruct == seg);

  return gcseg->p;
}


/* segGCSetP -- SegGC method to set the P field of a segment */

static void segGCSetP(Seg seg, void *p)
{
  SegGC gcseg;

  AVERT(Seg, seg);
  gcseg = SegSegGC(seg);
  AVERT(SegGC, gcseg);
  AVER(&gcseg->segStruct == seg);

  gcseg->p = p;
}


/* segGCDescribe -- SegGC  description method */

static Res segGCDescribe(Seg seg, mps_lib_FILE *stream)
{
  Res res;
  SegClass super;
  SegGC gcseg;

  AVERT(Seg, seg);
  gcseg = SegSegGC(seg);
  AVERT(SegGC, gcseg);
  AVER(&gcseg->segStruct == seg);

  /* Describe the superclass fields first via next-method call */
  super = EnsureSegClass();
  res = super->describe(seg, stream);
  if(res != ResOK)
    return res;

  if(gcseg->buffer != NULL) {
    res = BufferDescribe(gcseg->buffer, stream);
    if(res != ResOK)
      return res;
  }
  res = WriteF(stream,
               "  summary $W\n", (WriteFW)gcseg->summary,
               NULL);
  return res;
}



Bool SegClassCheck(SegClass class)
{
  CHECKL(ProtocolClassCheck(&class->protocol));
  CHECKL(class->name != NULL); /* Should be <=6 char C identifier */
  CHECKL(class->size >= sizeof(SegStruct));
  CHECKL(FUNCHECK(class->init));
  CHECKL(FUNCHECK(class->finish));
  CHECKL(FUNCHECK(class->summary));
  CHECKL(FUNCHECK(class->setGrey));
  CHECKL(FUNCHECK(class->setWhite));
  CHECKL(FUNCHECK(class->setRankSet));
  CHECKL(FUNCHECK(class->setRankSummary));
  CHECKL(FUNCHECK(class->describe));
  CHECKL(FUNCHECK(class->setP));
  CHECKS(SegClass, class);
  return TRUE;
} 


/* SegClass -- the vanilla segment class definition */

DEFINE_CLASS(SegClass, class)
{
  INHERIT_CLASS(&class->protocol, ProtocolClass);
  class->name = "SEG";
  class->size = sizeof(SegStruct);
  class->init = segTrivInit;
  class->finish = segTrivFinish;
  class->summary = segNoSummary;  
  class->setSummary = segNoSetSummary;  
  class->buffer = segNoBuffer;  
  class->setBuffer = segNoSetBuffer;  
  class->setGrey = segNoSetGrey;
  class->setWhite = segNoSetWhite;
  class->setRankSet = segNoSetRankSet;
  class->setRankSummary = segNoSetRankSummary;
  class->describe = segTrivDescribe;
  class->p = segNoP;
  class->setP = segNoSetP;
  class->sig = SegClassSig;
}

/* SegGCClass -- GC-supporting segment class definition */
 
DEFINE_SEG_CLASS(SegGCClass, class)
{
  INHERIT_CLASS(class, SegClass);
  class->name = "SEGGC";
  class->size = sizeof(SegGCStruct);
  class->init = segGCInit;
  class->finish = segGCFinish;
  class->summary = segGCSummary;  
  class->setSummary = segGCSetSummary;  
  class->buffer = segGCBuffer;  
  class->setBuffer = segGCSetBuffer;  
  class->setGrey = segGCSetGrey;
  class->setWhite = segGCSetWhite;
  class->setRankSet = segGCSetRankSet;
  class->setRankSummary = segGCSetRankSummary;
  class->describe = segGCDescribe;
  class->p = segGCP;
  class->setP = segGCSetP;
}

