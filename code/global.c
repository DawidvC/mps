/* global.c: ARENA-GLOBAL INTERFACES
 *
 * $Id$
 * Copyright (c) 2001-2018 Ravenbrook Limited.  See end of file for license.
 * Portions copyright (C) 2002 Global Graphics Software.
 *
 * .sources: See <design/arena/>.  design.mps.thread-safety is relevant
 * to the functions ArenaEnter and ArenaLeave in this file.
 *
 *
 * TRANSGRESSIONS
 *
 * .static: Static data is used in ArenaAccess (in order to find the
 * appropriate arena) and GlobalsInit.  It's checked in GlobalsCheck.
 * See <design/arena/#static>.
 *
 * .non-mod: The Globals structure has many fields which properly belong
 * to other modules (see <code/mpmst.h>); GlobalsInit contains code which
 * breaks the usual module abstractions.  Such instances are documented
 * with a tag to the relevant module implementation.  Most of the
 * functions should be in some other module, they just ended up here by
 * confusion over naming.  */

#include "bt.h"
#include "poolmrg.h"
#include "mps.h" /* finalization */
#include "mpm.h"

SRCID(global, "$Id$");


/* All static data objects are declared here. See .static */

/* <design/arena/#static.ring.init> */
static Bool arenaRingInit = FALSE;
static RingStruct arenaRing;       /* <design/arena/#static.ring> */
static Serial arenaSerial;         /* <design/arena/#static.serial> */


/* arenaClaimRingLock, arenaReleaseRingLock -- lock/release the arena ring
 *
 * See <design/arena/#static.ring.lock>.  */

static void arenaClaimRingLock(void)
{
  LockClaimGlobal();  /* claim the global lock to protect arenaRing */
}

static void arenaReleaseRingLock(void)
{
  LockReleaseGlobal();  /* release the global lock protecting arenaRing */
}


/* GlobalsClaimAll -- claim all MPS locks
 * <design/thread-safety/#sol.fork.lock>
 */

void GlobalsClaimAll(void)
{
  LockClaimGlobalRecursive();
  arenaClaimRingLock();
  GlobalsArenaMap(ArenaEnter);
}

/* GlobalsReleaseAll -- release all MPS locks. GlobalsClaimAll must
 * previously have been called. <design/thread-safety/#sol.fork.lock> */

void GlobalsReleaseAll(void)
{
  GlobalsArenaMap(ArenaLeave);
  arenaReleaseRingLock();
  LockReleaseGlobalRecursive();
}

/* arenaReinitLock -- reinitialize the lock for an arena */

static void arenaReinitLock(Arena arena)
{
  AVERT(Arena, arena);
  ShieldLeave(arena);
  LockInit(ArenaGlobals(arena)->lock);
}

/* GlobalsReinitializeAll -- reinitialize all MPS locks, and leave the
 * shield for all arenas. GlobalsClaimAll must previously have been
 * called. <design/thread-safety/#sol.fork.lock> */

void GlobalsReinitializeAll(void)
{
  GlobalsArenaMap(arenaReinitLock);
  LockInitGlobal();
}


/* arenaAnnounce -- add a new arena into the global ring of arenas
 *
 * On entry, the arena must not be locked (there should be no need,
 * because other threads can't know about it).  On exit, it will be.  */

static void arenaAnnounce(Arena arena)
{
  Globals arenaGlobals;

  /* arena checked in ArenaEnter */

  arenaClaimRingLock();
  ArenaEnter(arena);
  arenaGlobals = ArenaGlobals(arena);
  AVERT(Globals, arenaGlobals);
  RingAppend(&arenaRing, &arenaGlobals->globalRing);
  arenaReleaseRingLock();
}


/* arenaDenounce -- remove an arena from the global ring of arenas
 *
 * After this, no other thread can access the arena through ArenaAccess.
 * On entry, the arena should be locked.  On exit, it will still be, but
 * the lock has been released and reacquired in the meantime, so callers
 * should not assume anything about the state of the arena.  */

static void arenaDenounce(Arena arena)
{
  Globals arenaGlobals;

  AVERT(Arena, arena);

  /* Temporarily give up the arena lock to avoid deadlock, */
  /* see <design/thread-safety/#deadlock>. */
  ArenaLeave(arena);

  /* Detach the arena from the global list. */
  arenaClaimRingLock();
  ArenaEnter(arena);
  arenaGlobals = ArenaGlobals(arena);
  AVERT(Globals, arenaGlobals);
  RingRemove(&arenaGlobals->globalRing);
  arenaReleaseRingLock();
}


/* GlobalsArenaMap -- map a function over the arenas. The caller must
 * have acquired the ring lock. */

void GlobalsArenaMap(void (*func)(Arena arena))
{
  Ring node, nextNode;
  AVERT(Ring, &arenaRing);
  RING_FOR(node, &arenaRing, nextNode) {
    Globals arenaGlobals = RING_ELT(Globals, globalRing, node);
    Arena arena = GlobalsArena(arenaGlobals);
    func(arena);
  }
}


/* GlobalsCheck -- check the arena globals */

Bool GlobalsCheck(Globals arenaGlobals)
{
  Arena arena;
  TraceId ti;
  Trace trace;
  Rank rank;

  CHECKS(Globals, arenaGlobals);
  arena = GlobalsArena(arenaGlobals);
  CHECKL(arena->serial < arenaSerial); 
  CHECKD_NOSIG(Ring, &arenaGlobals->globalRing);

  CHECKL(MPSVersion() == arenaGlobals->mpsVersionString);

  if (arenaGlobals->lock != NULL)
    CHECKD_NOSIG(Lock, arenaGlobals->lock);

  /* no check possible on pollThreshold */
  CHECKL(BoolCheck(arenaGlobals->insidePoll));
  CHECKL(BoolCheck(arenaGlobals->clamped));
  CHECKL(arenaGlobals->fillMutatorSize >= 0.0);
  CHECKL(arenaGlobals->emptyMutatorSize >= 0.0);
  CHECKL(arenaGlobals->allocMutatorSize >= 0.0);
  CHECKL(arenaGlobals->fillMutatorSize - arenaGlobals->emptyMutatorSize
         >= arenaGlobals->allocMutatorSize);
  CHECKL(arenaGlobals->fillInternalSize >= 0.0);
  CHECKL(arenaGlobals->emptyInternalSize >= 0.0);

  CHECKL(BoolCheck(arenaGlobals->bufferLogging));
  CHECKD_NOSIG(Ring, &arenaGlobals->poolRing);
  CHECKD_NOSIG(Ring, &arenaGlobals->rootRing);
  CHECKD_NOSIG(Ring, &arenaGlobals->rememberedSummaryRing);
  CHECKL(arenaGlobals->rememberedSummaryIndex < RememberedSummaryBLOCK);
  /* <code/global.c#remembered.summary> RingIsSingle imples index == 0 */
  CHECKL(!RingIsSingle(&arenaGlobals->rememberedSummaryRing) ||
    arenaGlobals->rememberedSummaryIndex == 0);
  CHECKD_NOSIG(Ring, &arena->formatRing);
  CHECKD_NOSIG(Ring, &arena->messageRing);
  if (arena->enabledMessageTypes != NULL)
    CHECKD_NOSIG(BT, arena->enabledMessageTypes);
  CHECKL(BoolCheck(arena->isFinalPool));
  if (arena->isFinalPool) {
    CHECKD(Pool, arena->finalPool);
  } else {
    CHECKL(arena->finalPool == NULL);
  }

  CHECKD_NOSIG(Ring, &arena->threadRing);
  CHECKD_NOSIG(Ring, &arena->deadRing);

  CHECKD(Shield, ArenaShield(arena));

  CHECKL(TraceSetCheck(arena->busyTraces));
  CHECKL(TraceSetCheck(arena->flippedTraces));
  CHECKL(TraceSetSuper(arena->busyTraces, arena->flippedTraces));

  TRACE_SET_ITER(ti, trace, TraceSetUNIV, arena)
    /* <design/arena/#trace> */
    if (TraceSetIsMember(arena->busyTraces, trace)) {
      CHECKD(Trace, trace);
    } else {
      /* <design/arena/#trace.invalid> */
      CHECKL(trace->sig == SigInvalid);
    }
    /* <design/message-gc/> */
    CHECKL(TraceIdMessagesCheck(arena, ti));
  TRACE_SET_ITER_END(ti, trace, TraceSetUNIV, arena);

  for(rank = RankMIN; rank < RankLIMIT; ++rank)
    CHECKD_NOSIG(Ring, &arena->greyRing[rank]);
  CHECKD_NOSIG(Ring, &arena->chainRing);

  CHECKL(arena->tracedWork >= 0.0);
  CHECKL(arena->tracedTime >= 0.0);
  /* no check for arena->lastWorldCollect (Clock) */

  /* can't write a check for arena->epoch */
  CHECKD(History, ArenaHistory(arena));

  /* we also check the statics now. <design/arena/#static.check> */
  CHECKL(BoolCheck(arenaRingInit));
  /* Can't CHECKD_NOSIG here because &arenaRing is never NULL and GCC
   * will warn about a constant comparison. */
  CHECKL(RingCheck(&arenaRing));

  CHECKL(BoolCheck(arena->emergency));
  /* .emergency.invariant: There can only be an emergency when a trace
   * is busy. */
  CHECKL(!arena->emergency || arena->busyTraces != TraceSetEMPTY);
  
  if (arenaGlobals->defaultChain != NULL)
    CHECKD(Chain, arenaGlobals->defaultChain);

  /* can't check arena->stackWarm */

  return TRUE;
}


/* GlobalsInit -- initialize the globals of the arena */

Res GlobalsInit(Globals arenaGlobals)
{
  Arena arena;
  Rank rank;
  TraceId ti;

  /* This is one of the first things that happens, */
  /* so check static consistency here. */
  AVER(MPMCheck());

  arenaClaimRingLock();
  /* Ensure static things are initialized. */
  if (!arenaRingInit) {
    /* there isn't an arena ring yet */
    /* <design/arena/#static.init> */
    arenaRingInit = TRUE;
    RingInit(&arenaRing);
    arenaSerial = (Serial)0;
    /* The setup functions call pthread_atfork (on the appropriate
       platforms) and so must be called in the correct order. Here we
       require the locks to be taken first in the "prepare" case and
       released last in the "parent" and "child" cases. */
    ThreadSetup();
    ProtSetup();
    LockSetup();
  }
  arena = GlobalsArena(arenaGlobals);
  /* Ensure updates to arenaSerial do not race by doing the update
   * while the ring lock is claimed. */
  arena->serial = arenaSerial;
  ++ arenaSerial;
  arenaReleaseRingLock();

  RingInit(&arenaGlobals->globalRing);

  arenaGlobals->lock = NULL;

  arenaGlobals->pollThreshold = 0.0;
  arenaGlobals->insidePoll = FALSE;
  arenaGlobals->clamped = FALSE;
  arenaGlobals->fillMutatorSize = 0.0;
  arenaGlobals->emptyMutatorSize = 0.0;
  arenaGlobals->allocMutatorSize = 0.0;
  arenaGlobals->fillInternalSize = 0.0;
  arenaGlobals->emptyInternalSize = 0.0;

  arenaGlobals->mpsVersionString = MPSVersion();
  arenaGlobals->bufferLogging = FALSE;
  RingInit(&arenaGlobals->poolRing);
  arenaGlobals->poolSerial = (Serial)0;
  RingInit(&arenaGlobals->rootRing);
  arenaGlobals->rootSerial = (Serial)0;
  RingInit(&arenaGlobals->rememberedSummaryRing);
  arenaGlobals->rememberedSummaryIndex = 0;

  RingInit(&arena->threadRing);
  RingInit(&arena->deadRing);
  arena->threadSerial = (Serial)0;
  RingInit(&arena->formatRing);
  arena->formatSerial = (Serial)0;
  RingInit(&arena->messageRing);
  arena->enabledMessageTypes = NULL;
  arena->droppedMessages = 0;
  arena->isFinalPool = FALSE;
  arena->finalPool = NULL;
  arena->busyTraces = TraceSetEMPTY;    /* <code/trace.c> */
  arena->flippedTraces = TraceSetEMPTY; /* <code/trace.c> */
  arena->tracedWork = 0.0;
  arena->tracedTime = 0.0;
  arena->lastWorldCollect = ClockNow();
  ShieldInit(ArenaShield(arena));

  for (ti = 0; ti < TraceLIMIT; ++ti) {
    /* <design/arena/#trace.invalid> */
    arena->trace[ti].sig = SigInvalid;
    /* ti must be valid so that TraceSetIsMember etc. always work */
    arena->trace[ti].ti = ti;
    /* <design/message-gc/#lifecycle> */
    arena->tsMessage[ti] = NULL;
    arena->tMessage[ti] = NULL;
  }

  for(rank = RankMIN; rank < RankLIMIT; ++rank)
    RingInit(&arena->greyRing[rank]);
  STATISTIC(arena->writeBarrierHitCount = 0);
  RingInit(&arena->chainRing);

  HistoryInit(ArenaHistory(arena));
  
  arena->emergency = FALSE;

  arena->stackWarm = NULL;
  
  arenaGlobals->defaultChain = NULL;

  arenaGlobals->sig = GlobalsSig;
  AVERT(Globals, arenaGlobals);
  return ResOK;
}


/* GlobalsCompleteCreate -- complete creating the globals of the arena
 *
 * This is like the final initializations in a Create method, except
 * there's no separate GlobalsCreate.  */

Res GlobalsCompleteCreate(Globals arenaGlobals)
{
  Arena arena;
  Res res;
  void *p;
  TraceId ti;
  Trace trace;

  AVERT(Globals, arenaGlobals);
  arena = GlobalsArena(arenaGlobals);

  /* initialize the message stuff, <design/message/> */
  {
    void *v;

    res = ControlAlloc(&v, arena, BTSize(MessageTypeLIMIT));
    if (res != ResOK)
      return res;
    arena->enabledMessageTypes = v;
    BTResRange(arena->enabledMessageTypes, 0, MessageTypeLIMIT);
  }
  
  TRACE_SET_ITER(ti, trace, TraceSetUNIV, arena)
    /* <design/message-gc/#lifecycle> */
    res = TraceIdMessagesCreate(arena, ti);
    if(res != ResOK)
      return res;
  TRACE_SET_ITER_END(ti, trace, TraceSetUNIV, arena);

  res = ControlAlloc(&p, arena, LockSize());
  if (res != ResOK)
    return res;
  arenaGlobals->lock = (Lock)p;
  LockInit(arenaGlobals->lock);

  {
    GenParamStruct params[] = ChainDEFAULT;
    res = ChainCreate(&arenaGlobals->defaultChain, arena, NELEMS(params), params);
    if (res != ResOK)
      goto failChainCreate;
  }

  arenaAnnounce(arena);

  return ResOK;

failChainCreate:
  return res;
}


/* GlobalsFinish -- finish the globals of the arena */

void GlobalsFinish(Globals arenaGlobals)
{
  Arena arena;
  Rank rank;
  
  arena = GlobalsArena(arenaGlobals);
  AVERT(Globals, arenaGlobals);

  STATISTIC(EVENT2(ArenaWriteFaults, arena, arena->writeBarrierHitCount));

  arenaGlobals->sig = SigInvalid;

  ShieldFinish(ArenaShield(arena));
  HistoryFinish(ArenaHistory(arena));
  RingFinish(&arena->formatRing);
  RingFinish(&arena->chainRing);
  RingFinish(&arena->messageRing);
  RingFinish(&arena->threadRing);
  RingFinish(&arena->deadRing);
  for(rank = RankMIN; rank < RankLIMIT; ++rank)
    RingFinish(&arena->greyRing[rank]);
  RingFinish(&arenaGlobals->rootRing);
  RingFinish(&arenaGlobals->poolRing);
  RingFinish(&arenaGlobals->globalRing);
}

/* GlobalsPrepareToDestroy -- prepare to destroy the globals of the arena
 *
 * This is like the final initializations in a Destroy method, except
 * there's no separate GlobalsDestroy.  */

void GlobalsPrepareToDestroy(Globals arenaGlobals)
{
  Arena arena;
  TraceId ti;
  Trace trace;
  Chain defaultChain;
  Rank rank;

  AVERT(Globals, arenaGlobals);

  /* Park the arena before destroying the default chain, to ensure
   * that there are no traces using that chain. */
  ArenaPark(arenaGlobals);

  arena = GlobalsArena(arenaGlobals);

  arenaDenounce(arena);

  defaultChain = arenaGlobals->defaultChain;
  arenaGlobals->defaultChain = NULL;
  ChainDestroy(defaultChain);

  LockRelease(arenaGlobals->lock);
  /* Theoretically, another thread could grab the lock here, but it's */
  /* not worth worrying about, since an attempt after the lock has been */
  /* destroyed would lead to a crash just the same. */
  LockFinish(arenaGlobals->lock);
  arenaGlobals->lock = NULL;

  TRACE_SET_ITER(ti, trace, TraceSetUNIV, arena)
    /* <design/message-gc/#lifecycle> */
    TraceIdMessagesDestroy(arena, ti);
  TRACE_SET_ITER_END(ti, trace, TraceSetUNIV, arena);

  /* report dropped messages */
  if(arena->droppedMessages > 0)
    EVENT1(MessagesDropped, arena->droppedMessages);

  /* .message.queue.empty: Empty the queue of messages before */
  /* proceeding to finish the arena.  It is important that this */
  /* is done before destroying the finalization pool as otherwise */
  /* the message queue would have dangling pointers to messages */
  /* whose memory has been unmapped. */
  if(MessagePoll(arena))
    EVENT0(MessagesExist);
  MessageEmpty(arena);

  /* throw away the BT used by messages */
  if (arena->enabledMessageTypes != NULL) {
    ControlFree(arena, (void *)arena->enabledMessageTypes,
                BTSize(MessageTypeLIMIT));
    arena->enabledMessageTypes = NULL;
  }

  /* destroy the final pool (see <design/finalize/>) */
  if (arena->isFinalPool) {
    /* All this subtlety is because PoolDestroy will call */
    /* ArenaCheck several times.  The invariant on finalPool */
    /* and isFinalPool should hold before, after, and during */
    /* the PoolDestroy call */
    Pool pool = arena->finalPool;

    arena->isFinalPool = FALSE;
    arena->finalPool = NULL;
    PoolDestroy(pool);
  }

  ShieldDestroyQueue(ArenaShield(arena), arena);

  /* Check that the tear-down is complete: that the client has
   * destroyed all data structures associated with the arena. We do
   * this here rather than in GlobalsFinish because by the time that
   * is called, the control pool has been destroyed and so the address
   * space containing all these rings has potentially been unmapped,
   * and so RingCheck dereferences a pointer into that unmapped memory
   * and we get a crash instead of an assertion. See job000652.
   */
  AVER(RingIsSingle(&arena->formatRing)); /* <design/check/#.common> */
  AVER(RingIsSingle(&arena->chainRing)); /* <design/check/#.common> */
  AVER(RingIsSingle(&arena->messageRing));
  AVER(RingIsSingle(&arena->threadRing)); /* <design/check/#.common> */
  AVER(RingIsSingle(&arena->deadRing));
  AVER(RingIsSingle(&arenaGlobals->rootRing)); /* <design/check/#.common> */
  for(rank = RankMIN; rank < RankLIMIT; ++rank)
    AVER(RingIsSingle(&arena->greyRing[rank]));

  /* At this point the following pools still exist:
   * 0. arena->freeCBSBlockPoolStruct
   * 1. arena->controlPoolStruct
   * 2. arena->controlPoolStruct.cbsBlockPoolStruct
   */
  AVER(RingLength(&arenaGlobals->poolRing) == 3); /* <design/check/#.common> */
}


Ring GlobalsRememberedSummaryRing(Globals global)
{
  AVERT(Globals, global);

  return &global->rememberedSummaryRing;
}


/* ArenaEnter -- enter the state where you can look at the arena */

void ArenaEnter(Arena arena)
{
  ArenaEnterLock(arena, FALSE);
}

/*  The recursive argument specifies whether to claim the lock
    recursively or not. */
void ArenaEnterLock(Arena arena, Bool recursive)
{
  Lock lock;

  /* This check is safe to do outside the lock.  Unless the client
     is also calling ArenaDestroy, but that's a protocol violation by
     the client if so. */
  AVER(TESTT(Arena, arena));

  /* It's critical that the stack probe is outside the lock, because
   * the stack probe may cause arbitrary code to run (via a signal or
   * exception handler) and that code may enter the MPS. If we took
   * the lock first then this would deadlock. */
  StackProbe(StackProbeDEPTH);
  lock = ArenaGlobals(arena)->lock;
  if(recursive) {
    LockClaimRecursive(lock);
  } else {
    LockClaim(lock);
  }
  AVERT(Arena, arena); /* can't AVERT it until we've got the lock */
  if(recursive) {
    /* already in shield */
  } else {
    ShieldEnter(arena);
  }
}

/* Same as ArenaEnter, but for the few functions that need to be
   reentrant with respect to some part of the MPS.
   For example, mps_arena_has_addr. */

void ArenaEnterRecursive(Arena arena)
{
  ArenaEnterLock(arena, TRUE);
}

/* ArenaLeave -- leave the state where you can look at MPM data structures */

void ArenaLeave(Arena arena)
{
  AVERT(Arena, arena);
  ArenaLeaveLock(arena, FALSE);
}

void ArenaLeaveLock(Arena arena, Bool recursive)
{
  Lock lock;

  AVERT(Arena, arena);

  lock = ArenaGlobals(arena)->lock;

  if(recursive) {
    /* no need to leave shield */
  } else {
    ShieldLeave(arena);
  }
  ProtSync(arena);              /* <design/prot/#if.sync> */
  if(recursive) {
    LockReleaseRecursive(lock);
  } else {
    LockRelease(lock);
  }
}

void ArenaLeaveRecursive(Arena arena)
{
  ArenaLeaveLock(arena, TRUE);
}

Bool ArenaBusy(Arena arena)
{
  return LockIsHeld(ArenaGlobals(arena)->lock);
}


/* ArenaAccess -- deal with an access fault
 *
 * This is called when a protected address is accessed.  The mode
 * corresponds to which mode flags need to be cleared in order for the
 * access to continue.  */

Bool ArenaAccess(Addr addr, AccessSet mode, MutatorContext context)
{
  static Count count = 0;       /* used to match up ArenaAccess events */
  Seg seg;
  Ring node, nextNode;
  Res res;

  arenaClaimRingLock();    /* <design/arena/#lock.ring> */
  AVERT(Ring, &arenaRing);

  RING_FOR(node, &arenaRing, nextNode) {
    Globals arenaGlobals = RING_ELT(Globals, globalRing, node);
    Arena arena = GlobalsArena(arenaGlobals);
    Root root;

    ArenaEnter(arena);     /* <design/arena/#lock.arena> */
    EVENT4(ArenaAccess, arena, ++count, addr, mode);

    /* @@@@ The code below assumes that Roots and Segs are disjoint. */
    /* It will fall over (in TraceSegAccess probably) if there is a */
    /* protected root on a segment. */
    /* It is possible to overcome this restriction. */
    if (SegOfAddr(&seg, arena, addr)) {
      arenaReleaseRingLock();
      /* An access in a different thread (or even in the same thread,
       * via a signal or exception handler) may have already caused
       * the protection to be cleared. This avoids calling TraceAccess
       * on protection that has already been cleared on a separate
       * thread. */
      mode &= SegPM(seg);
      if (mode != AccessSetEMPTY) {
        res = SegAccess(seg, arena, addr, mode, context);
        AVER(res == ResOK); /* Mutator can't continue unless this succeeds */
      } else {
        /* Protection was already cleared, for example by another thread
           or a fault in a nested exception handler: nothing to do now. */
      }
      EVENT4(ArenaAccess, arena, count, addr, mode);
      ArenaLeave(arena);
      return TRUE;
    } else if (RootOfAddr(&root, arena, addr)) {
      arenaReleaseRingLock();
      mode &= RootPM(root);
      if (mode != AccessSetEMPTY)
        RootAccess(root, mode);
      EVENT4(ArenaAccess, arena, count, addr, mode);
      ArenaLeave(arena);
      return TRUE;
    } else {
      /* No segment or root was found at the address: this must mean
       * that activity in another thread (or even in the same thread,
       * via a signal or exception handler) caused the segment or root
       * to go away. So there's nothing to do now. */
    }

    ArenaLeave(arena);
  }

  arenaReleaseRingLock();
  return FALSE;
}


/* ArenaPoll -- trigger periodic actions
 *
 * Poll all background activities to see if they need to do anything.
 * ArenaPoll does nothing if the amount of committed memory is less than
 * the arena poll threshold.  This means that actions are taken as the
 * memory demands increase.
 *
 * @@@@ This is where time is "stolen" from the mutator in addition
 * to doing what it asks and servicing accesses.  This is where the
 * amount of time should be controlled, perhaps by passing time
 * limits to the various other activities.
 *
 * @@@@ Perhaps this should be based on a process table rather than a
 * series of manual steps for looking around.  This might be worthwhile
 * if we introduce background activities other than tracing.  */

void (ArenaPoll)(Globals globals)
{
  Arena arena;
  Clock start;
  Bool worldCollected = FALSE;
  Bool moreWork, workWasDone = FALSE;
  Work tracedWork;

  AVERT(Globals, globals);

  if (globals->clamped)
    return;
  if (globals->insidePoll)
    return;
  arena = GlobalsArena(globals);
  if (!PolicyPoll(arena))
    return;

  globals->insidePoll = TRUE;

  /* fillMutatorSize has advanced; call TracePoll enough to catch up. */
  start = ClockNow();

  EVENT3(ArenaPoll, arena, start, FALSE);

  do {
    moreWork = TracePoll(&tracedWork, &worldCollected, globals,
                         !worldCollected);
    if (moreWork) {
      workWasDone = TRUE;
    }
  } while (PolicyPollAgain(arena, start, moreWork, tracedWork));

  /* Don't count time spent checking for work, if there was no work to do. */
  if (workWasDone) {
    ArenaAccumulateTime(arena, start, ClockNow());
  }

  EVENT3(ArenaPoll, arena, start, BOOLOF(workWasDone));

  globals->insidePoll = FALSE;
}


/* ArenaStep -- use idle time for collection work */

Bool ArenaStep(Globals globals, double interval, double multiplier)
{
  Bool workWasDone = FALSE;
  Clock start, intervalEnd, availableEnd, now;
  Clock clocks_per_sec;
  Arena arena;

  AVERT(Globals, globals);
  AVER(interval >= 0.0);
  AVER(multiplier >= 0.0);

  arena = GlobalsArena(globals);
  clocks_per_sec = ClocksPerSec();

  start = now = ClockNow();
  intervalEnd = start + (Clock)(interval * clocks_per_sec);
  AVER(intervalEnd >= start);
  availableEnd = start + (Clock)(interval * multiplier * clocks_per_sec);
  AVER(availableEnd >= start);

  /* loop while there is work to do and time on the clock. */
  do {
    Trace trace;
    if (arena->busyTraces != TraceSetEMPTY) {
      trace = ArenaTrace(arena, (TraceId)0);
    } else {
      /* No traces are running: consider collecting the world. */
      if (PolicyShouldCollectWorld(arena, (double)(availableEnd - now), now,
                                   clocks_per_sec))
      {
        Res res;
        res = TraceStartCollectAll(&trace, arena, TraceStartWhyOPPORTUNISM);
        if (res != ResOK)
          break;
        arena->lastWorldCollect = now;
      } else {
        /* Not worth collecting the world; consider starting a trace. */
        Bool worldCollected;
        if (!PolicyStartTrace(&trace, &worldCollected, arena, FALSE))
          break;
      }
    }
    TraceAdvance(trace);
    if (trace->state == TraceFINISHED)
      TraceDestroyFinished(trace);
    workWasDone = TRUE;
    now = ClockNow();
  } while (now < intervalEnd);

  if (workWasDone) {
    ArenaAccumulateTime(arena, start, now);
  }

  return workWasDone;
}

/* ArenaFinalize -- registers an object for finalization
 *
 * See <design/finalize/>.  */

Res ArenaFinalize(Arena arena, Ref obj)
{
  Res res;
  Pool refpool;

  AVERT(Arena, arena);
  AVER(PoolOfAddr(&refpool, arena, (Addr)obj));
  AVER(PoolHasAttr(refpool, AttrGC));

  if (!arena->isFinalPool) {
    Pool finalpool;

    res = PoolCreate(&finalpool, arena, PoolClassMRG(), argsNone);
    if (res != ResOK)
      return res;
    arena->finalPool = finalpool;
    arena->isFinalPool = TRUE;
  }

  res = MRGRegister(arena->finalPool, obj);
  return res;
}


/* ArenaDefinalize -- removes one finalization registration of an object
 *
 * See <design/finalize>.  */

Res ArenaDefinalize(Arena arena, Ref obj)
{
  Res res;

  AVERT(Arena, arena);
  AVER(ArenaHasAddr(arena, (Addr)obj));

  if (!arena->isFinalPool) {
    return ResFAIL;
  }
  res = MRGDeregister(arena->finalPool, obj);
  return res;
}


/* ArenaPeek -- read a single reference, possibly through a barrier */

Ref ArenaPeek(Arena arena, Ref *p)
{
  Seg seg;
  Ref ref;

  AVERT(Arena, arena);
  /* Can't check p as it is arbitrary */

  if (SegOfAddr(&seg, arena, (Addr)p))
    ref = ArenaPeekSeg(arena, seg, p);
  else
    ref = *p;
  return ref;
}

/* ArenaPeekSeg -- as ArenaPeek, but p must be in seg. */

Ref ArenaPeekSeg(Arena arena, Seg seg, Ref *p)
{
  Ref ref;
  Rank rank;

  AVERT(Arena, arena);
  AVERT(Seg, seg);
  AVER(PoolArena(SegPool(seg)) == arena);
  AVER(SegBase(seg) <= (Addr)p);
  AVER((Addr)p < SegLimit(seg));
  /* TODO: Consider checking p's alignment using seg->pool->alignment */

  /* .read.flipped: We AVER that the reference that we are reading */
  /* refers to an object for which all the traces that the object is */
  /* white for are also flipped.  This is because we don't have any */
  /* write-barrier (in the sense of write-barrier collectors) */
  /* mechanism in place for reading (strictly speaking, writing */
  /* it somewhere after having read it) references that are white. */
  AVER(TraceSetSub(SegWhite(seg), arena->flippedTraces));

  /* .read.conservative: Scan according to rank phase-of-trace, */
  /* See <code/trace.c#scan.conservative> */
  /* If the segment isn't grey it doesn't need scanning, and in fact it
     would be wrong to even ask what rank to scan it at, since there might
     not be any traces running. */
  if (TraceSetInter(SegGrey(seg), arena->flippedTraces) != TraceSetEMPTY) {
    rank = TraceRankForAccess(arena, seg);
    TraceScanSingleRef(arena->flippedTraces, rank, arena, seg, p);
  }

  /* We don't need to update the Seg Summary as in PoolSingleAccess
   * because we are not changing it after it has been scanned. */

  ShieldExpose(arena, seg);
  ref = *p;
  ShieldCover(arena, seg);
  return ref;
}

/* ArenaPoke -- write a single reference, possibly through a barrier */

void ArenaPoke(Arena arena, Ref *p, Ref ref)
{
  Seg seg;

  AVERT(Arena, arena);
  /* Can't check p as it is arbitrary */
  /* Can't check ref as it is arbitrary */

  if (SegOfAddr(&seg, arena, (Addr)p))
    ArenaPokeSeg(arena, seg, p, ref);
  else
    *p = ref;
}

/* ArenaPokeSeg -- as ArenaPoke, but p must be in seg. */

void ArenaPokeSeg(Arena arena, Seg seg, Ref *p, Ref ref)
{
  RefSet summary;

  AVERT(Arena, arena);
  AVERT(Seg, seg);
  AVER(PoolArena(SegPool(seg)) == arena);
  AVER(SegBase(seg) <= (Addr)p);
  AVER((Addr)p < SegLimit(seg));
  /* TODO: Consider checking p's alignment using seg->pool->alignment */
  /* ref is arbitrary and can't be checked */

  ShieldExpose(arena, seg);
  *p = ref;
  summary = SegSummary(seg);
  summary = RefSetAdd(arena, summary, (Addr)ref);
  SegSetSummary(seg, summary);
  ShieldCover(arena, seg);
}

/* ArenaRead -- like ArenaPeek, but reference known to be owned by arena */

Ref ArenaRead(Arena arena, Ref *p)
{
  Bool b;
  Seg seg = NULL;       /* suppress "may be used uninitialized" */

  AVERT(Arena, arena);

  b = SegOfAddr(&seg, arena, (Addr)p);
  AVER(b == TRUE);
  
  return ArenaPeekSeg(arena, seg, p);
}

/* ArenaWrite -- like ArenaPoke, but reference known to be owned by arena */

void ArenaWrite(Arena arena, Ref *p, Ref ref)
{
  Bool b;
  Seg seg = NULL;       /* suppress "may be used uninitialized" */

  AVERT(Arena, arena);

  b = SegOfAddr(&seg, arena, (Addr)p);
  AVER(b == TRUE);
  
  ArenaPokeSeg(arena, seg, p, ref);
}


/* GlobalsDescribe -- describe the arena globals */

Res GlobalsDescribe(Globals arenaGlobals, mps_lib_FILE *stream, Count depth)
{
  Res res;
  Arena arena;
  Ring node, nextNode;
  TraceId ti;
  Trace trace;

  if (!TESTT(Globals, arenaGlobals))
    return ResFAIL;
  if (stream == NULL)
    return ResFAIL;

  res = WriteF(stream, depth, "Globals\n", NULL);
  if (res != ResOK)
    return res;  

  arena = GlobalsArena(arenaGlobals);
  res = WriteF(stream, depth + 2,
               "mpsVersion $S\n", (WriteFS)arenaGlobals->mpsVersionString,
               "lock $P\n", (WriteFP)arenaGlobals->lock,
               "pollThreshold $U kB\n",
               (WriteFU)(arenaGlobals->pollThreshold / 1024),
               arenaGlobals->insidePoll ? "inside" : "outside", " poll\n",
               arenaGlobals->clamped ? "clamped\n" : "released\n",
               "fillMutatorSize $U kB\n",
               (WriteFU)(arenaGlobals->fillMutatorSize / 1024),
               "emptyMutatorSize $U kB\n",
               (WriteFU)(arenaGlobals->emptyMutatorSize / 1024),
               "allocMutatorSize $U kB\n",
               (WriteFU)(arenaGlobals->allocMutatorSize / 1024),
               "fillInternalSize $U kB\n",
               (WriteFU)(arenaGlobals->fillInternalSize / 1024),
               "emptyInternalSize $U kB\n",
               (WriteFU)(arenaGlobals->emptyInternalSize / 1024),
               "poolSerial $U\n", (WriteFU)arenaGlobals->poolSerial,
               "rootSerial $U\n", (WriteFU)arenaGlobals->rootSerial,
               "formatSerial $U\n", (WriteFU)arena->formatSerial,
               "threadSerial $U\n", (WriteFU)arena->threadSerial,
               "busyTraces    $B\n", (WriteFB)arena->busyTraces,
               "flippedTraces $B\n", (WriteFB)arena->flippedTraces,
               NULL);
  if (res != ResOK)
    return res;

  res = HistoryDescribe(ArenaHistory(arena), stream, depth + 2);
  if (res != ResOK)
    return res;

  res = ShieldDescribe(ArenaShield(arena), stream, depth + 2);
  if (res != ResOK)
    return res;

  res = RootsDescribe(arenaGlobals, stream, depth + 2);
  if (res != ResOK)
    return res;

  RING_FOR(node, &arenaGlobals->poolRing, nextNode) {
    Pool pool = RING_ELT(Pool, arenaRing, node);
    res = PoolDescribe(pool, stream, depth + 2);
    if (res != ResOK)
      return res;
  }

  RING_FOR(node, &arena->formatRing, nextNode) {
    Format format = RING_ELT(Format, arenaRing, node);
    res = FormatDescribe(format, stream, depth + 2);
    if (res != ResOK)
      return res;
  }

  RING_FOR(node, &arena->threadRing, nextNode) {
    Thread thread = ThreadRingThread(node);
    res = ThreadDescribe(thread, stream, depth + 2);
    if (res != ResOK)
      return res;
  }

  RING_FOR(node, &arena->chainRing, nextNode) {
    Chain chain = RING_ELT(Chain, chainRing, node);
    res = ChainDescribe(chain, stream, depth + 2);
    if (res != ResOK)
      return res;
  }

  TRACE_SET_ITER(ti, trace, TraceSetUNIV, arena)
    if (TraceSetIsMember(arena->busyTraces, trace)) {
      res = TraceDescribe(trace, stream, depth + 2);
      if (res != ResOK)
        return res;
    }
  TRACE_SET_ITER_END(ti, trace, TraceSetUNIV, arena);

  /* @@@@ What about grey rings? */
  return res;
}


/* ArenaSetEmergency -- move the arena into emergency mode
 *
 * Emergency mode is set when garbage collection cannot make progress because
 * it can't allocate memory.
 *
 * Emergency mode affects the choice of PoolFixMethod in new ScanStates.
 * See ScanStateInit.
 *
 * If the traces aren't normal GC traces, and have their fix method
 * set to something other than PoolFix, then this won't affect the choice
 * of fix method in ScanStateInit and so won't have any effect.  Whatever
 * caused the first failure will likely repeat.
 */

void ArenaSetEmergency(Arena arena, Bool emergency)
{
  AVERT(Arena, arena);
  AVERT(Bool, emergency);

  EVENT2(ArenaSetEmergency, arena, BOOLOF(emergency));

  arena->emergency = emergency;
}

Bool ArenaEmergency(Arena arena)
{
  AVERT(Arena, arena);
  return arena->emergency;
}



/* C. COPYRIGHT AND LICENSE
 *
 * Copyright (C) 2001-2018 Ravenbrook Limited <http://www.ravenbrook.com/>.
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
