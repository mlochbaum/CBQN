#include "gc.h"

#ifdef LOG_GC
#include "../utils/time.h"
#endif

u64 gc_depth = 1;


vfn gc_roots[8];
u32 gc_rootSz;
void gc_addFn(vfn f) {
  if (gc_rootSz>=8) err("Too many GC root functions");
  gc_roots[gc_rootSz++] = f;
}

Value* gc_rootObjs[512];
u32 gc_rootObjSz;
void gc_add(B x) {
  assert(isVal(x));
  if (gc_rootObjSz>=512) err("Too many GC root objects");
  gc_rootObjs[gc_rootObjSz++] = v(x);
}

B* gc_rootBRefs[64]; u32 gc_rootBRefsSz;
void gc_add_ref(B* x) {
  if (gc_rootBRefsSz>=64) err("Too many GC root B refs");
  gc_rootBRefs[gc_rootBRefsSz++] = x;
}





static void gc_freeFreed(Value* v) {
  if (v->type==t_freed) mm_free(v);
}

static void gc_resetTag(Value* x) {
  x->mmInfo&= 0x7F;
}

void gc_visitRoots() {
  for (u32 i = 0; i < gc_rootSz; i++) gc_roots[i]();
  for (u32 i = 0; i < gc_rootObjSz; i++) mm_visitP(gc_rootObjs[i]);
  for (u32 i = 0; i < gc_rootBRefsSz; i++) mm_visit(*gc_rootBRefs[i]);
}

static void gc_tryFree(Value* v) {
  u8 t = v->type;
  #if defined(DEBUG) && !defined(CATCH_ERRORS)
    if (t==t_freed) err("GC found t_freed\n");
  #endif
  if (t!=t_empty && !(v->mmInfo&0x80)) {
    if (t==t_shape || t==t_temp || t==t_talloc) return;
    #ifdef DONT_FREE
      v->flags = t;
    #else
      #if CATCH_ERRORS
        if (t==t_freed) { mm_free(v); return; }
      #endif
    #endif
    #ifdef LOG_GC
      gc_freedBytes+= mm_size(v); gc_freedCount++;
    #endif
    v->type = t_freed;
    ptr_inc(v); // required as otherwise the object may free itself while not done reading its own fields
    TIi(t,freeO)(v);
    tptr_dec(v, mm_free);
    // Object may not be immediately freed if it's not a part of a cycle, but instead a descendant of one.
    // It will be freed when the cycle is freed, and the t_freed type ensures it doesn't double-free itself
  }
}

#ifdef LOG_GC
  u64 gc_visitBytes, gc_visitCount, gc_freedBytes, gc_freedCount, gc_unkRefsBytes, gc_unkRefsCount;
#endif

#if GC_VISIT_V2
  i32 visit_mode;
  enum {
    GC_DEC_REFC, // decrement refcount
    GC_INC_REFC, // increment refcount
    GC_MARK,     // if unmarked, mark & visit
    GC_LISTBAD,  // 
  };
  
  void gc_onVisit(Value* x) {
    switch (visit_mode) { default: UD;
      case GC_DEC_REFC: x->refc--; return;
      case GC_INC_REFC: x->refc++; return;
      case GC_MARK: {
        if (x->mmInfo&0x80) return;
        x->mmInfo|= 0x80;
        #ifdef LOG_GC
          gc_visitBytes+= mm_size(x); gc_visitCount++;
        #endif
        TIv(x,visit)(x);
        return;
      }
    }
  }
  
  static void gcv2_visit(Value* x) { TIv(x,visit)(x); }
  
  #if HEAP_VERIFY
  void gcv2_runHeapverify(i32 mode) {
    visit_mode = mode==0? GC_DEC_REFC : GC_INC_REFC;
    mm_forHeap(gcv2_visit);
    gc_visitRoots();
  }
  #endif
  
  static Value** gcv2_bufS;
  static Value** gcv2_bufC;
  static Value** gcv2_bufE;
  FORCE_INLINE void gcv2_storeRemainingEnd(Value* x) {
    *gcv2_bufC = x;
    gcv2_bufC++;
    #ifdef LOG_GC
      gc_unkRefsBytes+= mm_size(x); gc_unkRefsCount++;
    #endif
  }
  static NOINLINE void gcv2_storeRemainingR(Value* x) {
    ux i = gcv2_bufC - gcv2_bufS;
    ux n = (gcv2_bufE-gcv2_bufS)*2;
    gcv2_bufS = realloc(gcv2_bufS, n*sizeof(Value*));
    gcv2_bufC = gcv2_bufS + i;
    gcv2_bufE = gcv2_bufS + n;
    gcv2_storeRemainingEnd(x);
  }
  static void gcv2_storeRemaining(Value* x) {
    gc_resetTag(x);
    if (x->refc == 0) return;
    if (gcv2_bufC == gcv2_bufE) return gcv2_storeRemainingR(x);
    gcv2_storeRemainingEnd(x);
  }
  
  static void gc_run() {
    visit_mode = GC_DEC_REFC;
    mm_forHeap(gcv2_visit);
    
    gcv2_bufS = gcv2_bufC = malloc(1<<20);
    gcv2_bufE = gcv2_bufS + ((1<<20) / sizeof(Value*));
    mm_forHeap(gcv2_storeRemaining); // incl. unmark
    
    visit_mode = GC_INC_REFC;
    mm_forHeap(gcv2_visit);
    
    visit_mode = GC_MARK;
    gc_visitRoots();
    Value** c = gcv2_bufS;
    while (c < gcv2_bufC) mm_visitP(*(c++));
    free(gcv2_bufS);
    
    mm_forHeap(gc_tryFree);
    mm_forHeap(gc_freeFreed);
  }
#else
  static void gc_run() {
    mm_forHeap(gc_resetTag);
    gc_visitRoots();
    mm_forHeap(gc_tryFree);
    mm_forHeap(gc_freeFreed);
  }
#endif

u64 gc_lastAlloc;
void gc_forceGC() {
  #if ENABLE_GC
    #ifdef LOG_GC
      u64 start = nsTime();
      gc_visitBytes = 0; gc_freedBytes = 0;
      gc_visitCount = 0; gc_freedCount = 0;
      gc_unkRefsBytes = 0; gc_unkRefsCount = 0;
      u64 startSize = mm_heapUsed();
    #endif
      gc_run();
    u64 endSize = mm_heapUsed();
    #ifdef LOG_GC
      fprintf(stderr, "GC kept "N64d"B/"N64d" objs, freed "N64d"B, incl. directly "N64d"B/"N64d" objs", gc_visitBytes, gc_visitCount, startSize-endSize, gc_freedBytes, gc_freedCount);
      #if GC_VISIT_V2
        fprintf(stderr, "; unknown refs: "N64d"B/"N64d" objs", gc_unkRefsBytes, gc_unkRefsCount);
      #endif
      fprintf(stderr, "; took %.3fms\n", (nsTime()-start)/1e6);
    #endif
    gc_lastAlloc = endSize;
  #endif
}


bool gc_maybeGC() {
  if (gc_depth) return false;
  u64 used = mm_heapUsed();
  if (used > gc_lastAlloc*2) {
    gc_forceGC();
    return true;
  }
  return false;
}
