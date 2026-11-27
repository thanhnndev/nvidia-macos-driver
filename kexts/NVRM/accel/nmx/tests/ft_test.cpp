// Userspace test of the fence table in nmx_drm_uc.cpp (no kernel, no install). Build and run:
//   cd kexts/NVRM/accel/nmx/tests && awk '/^\/\* ---- fence table/{f=1} /^class NMXDrmClient/{f=0} f' ../nmx_drm_uc.cpp > ft_body.inc &&
//   xcrun clang++ -std=c++17 -Wall -Wno-unused-function -I.. -o /tmp/ft_test ft_test.cpp && /tmp/ft_test
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>
#include <stdlib.h>
typedef int IOSimpleLock; typedef int64_t SInt64;
static IOSimpleLock *IOSimpleLockAlloc(){ return (IOSimpleLock*)calloc(1,sizeof(int)); }
static void IOSimpleLockFree(IOSimpleLock*l){ free(l); }
static void IOSimpleLockLock(IOSimpleLock*l){ if(*l) {puts("LOCK REENTERED"); exit(2);} *l=1; }
static void IOSimpleLockUnlock(IOSimpleLock*l){ *l=0; }
static SInt64 OSAddAtomic64(SInt64 v, int64_t *p){ SInt64 o=*p; *p+=v; return o; }
static uint64_t gNow = 1000;
static uint64_t mach_absolute_time(void){ return gNow; }
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *a){ *a = ns; }   /* test clock: 1 unit = 1 ns */
static bool OSCompareAndSwapPtr(void*o, void*n, void*volatile*p){ if(*p==o){*p=n;return true;} return false; }
#include "nmx_fence_table.h"
#include "ft_body.inc"
static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); fails++; } }while(0)
static nmx_fence_entry_v1 E(uint32_t sid, uint32_t f, uint64_t seq, uint64_t t=0, uint64_t g=0){ nmx_fence_entry_v1 e={sid,f,seq,t,g}; return e; }
static unsigned long long G(uint32_t sid,int k,bool*hit=nullptr){ unsigned long long v=~0ull; bool h=nmx_fence_table_get(sid,k,&v); if(hit)*hit=h; return h?v:~0ull; }
int main(){
  int A=1,B=2; bool hit;
  // gate off: nothing answered
  nmx_fence_entry_v1 e1[]={E(10,NMX_FENCE_F_PENDING|NMX_FENCE_F_SUBMITTED,5,100)};
  G(10,kNmxFkPending,&hit); CHECK(!hit);
  nmx_fence_table_set(1);
  CHECK(nmxFtApply(e1,1,&A));
  CHECK(G(10,kNmxFkPending)==5); CHECK(G(10,kNmxFkPendingT)==100); CHECK(G(10,kNmxFkSubmitted)==5);
  G(10,kNmxFkDone,&hit); CHECK(!hit);               // never written -> property
  CHECK(nmx_fence_key_index("NMXFenceGpuEndT")==kNmxFkGpuEndT); CHECK(nmx_fence_key_index("NMXFenceX")==-1); CHECK(nmx_fence_key_index("Foo")==-1);
  nmx_fence_entry_v1 d1[]={E(10,NMX_FENCE_F_DONE,5,200,150)};
  CHECK(nmxFtApply(d1,1,&A)); CHECK(G(10,kNmxFkDone)==5 && G(10,kNmxFkDoneT)==200 && G(10,kNmxFkGpuEndT)==150);
  // fill 64 rows in flight (owner B); sid 10 is done so it is evictable
  for(uint32_t i=0;i<63;i++){ nmx_fence_entry_v1 e[]={E(100+i,NMX_FENCE_F_PENDING,1)}; CHECK(nmxFtApply(e,1,&B)); }
  { nmx_fence_entry_v1 e[]={E(500,NMX_FENCE_F_PENDING,1)}; CHECK(nmxFtApply(e,1,&B)); }   // evicts sid 10
  G(10,kNmxFkPending,&hit); CHECK(!hit); CHECK(nmx_fence_table_evicted==1);
  // all 64 in flight: a new surface is refused and nothing changes; an existing one still updates
  { nmx_fence_entry_v1 e[]={E(100,NMX_FENCE_F_DONE,1,7), E(600,NMX_FENCE_F_PENDING,9)}; CHECK(!nmxFtApply(e,2,&B)); }
  G(100,kNmxFkDone,&hit); CHECK(!hit);              // atomic: the first entry was not applied
  { nmx_fence_entry_v1 e[]={E(100,NMX_FENCE_F_DONE,1,7)}; CHECK(nmxFtApply(e,1,&B)); CHECK(G(100,kNmxFkDone)==1); }
  // now 100 is evictable: the same call may not evict a row it uses itself
  { nmx_fence_entry_v1 e[]={E(100,NMX_FENCE_F_PENDING,2), E(601,NMX_FENCE_F_PENDING,3)}; CHECK(!nmxFtApply(e,2,&B)); CHECK(G(100,kNmxFkPending)==1); }
  // duplicates of one surface in a call share the row
  { nmx_fence_entry_v1 e[]={E(101,NMX_FENCE_F_PENDING,4,1), E(101,NMX_FENCE_F_SUBMITTED,4)}; CHECK(nmxFtApply(e,2,&A)); CHECK(G(101,kNmxFkSubmitted)==4); }
  // close of B clears B's rows only
  int64_t c0=nmx_fence_table_cleared; nmxFtClearOwner(&B); CHECK(nmx_fence_table_cleared-c0==63);
  CHECK(G(101,kNmxFkPending)==4); G(102,kNmxFkPending,&hit); CHECK(!hit);
  // gate write clears all; off -> no answers
  nmx_fence_table_set(0); G(101,kNmxFkPending,&hit); CHECK(!hit);
  nmx_fence_table_set(1); G(101,kNmxFkPending,&hit); CHECK(!hit);
  // review 2026-11-27 (1): with a full table, [A new, B existing and the LRU row not in flight] must keep both.
  // Before the fix A evicted B's row, B then landed on the same row and overwrote A (A's Pending lost).
  {
    nmx_fence_table_set(1);
    for(uint32_t i=0;i<64;i++){ nmx_fence_entry_v1 e[]={E(1000+i,NMX_FENCE_F_PENDING,1)}; CHECK(nmxFtApply(e,1,&B)); }
    { nmx_fence_entry_v1 e[]={E(1000,NMX_FENCE_F_DONE,1)}; CHECK(nmxFtApply(e,1,&B)); }      // 1000: oldest... now newest tick
    for(uint32_t i=1;i<64;i++){ nmx_fence_entry_v1 e[]={E(1000+i,NMX_FENCE_F_PENDING,2)}; CHECK(nmxFtApply(e,1,&B)); }  // 1000 is the LRU, done
    nmx_fence_entry_v1 e[]={E(2000,NMX_FENCE_F_PENDING,7), E(1000,NMX_FENCE_F_PENDING,8)};
    CHECK(!nmxFtApply(e,2,&B));                         // 1000 is claimed by its own entry, no other row is free: refused
    CHECK(G(1000,kNmxFkPending)==1); G(2000,kNmxFkPending,&hit); CHECK(!hit);
    // same with a free-able row: one more surface done -> A takes that row, B keeps its own
    { nmx_fence_entry_v1 d[]={E(1001,NMX_FENCE_F_DONE,2)}; CHECK(nmxFtApply(d,1,&B)); }
    CHECK(nmxFtApply(e,2,&B));
    CHECK(G(2000,kNmxFkPending)==7); CHECK(G(1000,kNmxFkPending)==8);
    G(1001,kNmxFkPending,&hit); CHECK(!hit);            // 1001 was the one evicted
    // stale (3): every row in flight, none written for 2 s -> the oldest in-flight row is reused
    { nmx_fence_entry_v1 p[]={E(1000,NMX_FENCE_F_PENDING,9)}; CHECK(nmxFtApply(p,1,&B)); }   // 1000 in flight again
    { nmx_fence_entry_v1 x[]={E(3000,NMX_FENCE_F_PENDING,1)}; CHECK(!nmxFtApply(x,1,&B)); }  // all in flight, fresh
    gNow += 2000000001ull;
    int64_t s0=nmx_fence_table_stale;
    { nmx_fence_entry_v1 x[]={E(3000,NMX_FENCE_F_PENDING,1)}; CHECK(nmxFtApply(x,1,&B)); }
    CHECK(nmx_fence_table_stale-s0==1); CHECK(G(3000,kNmxFkPending)==1);
    nmx_fence_table_set(0); nmx_fence_table_set(1);
  }
  CHECK(nmxFtFlagsOk(1)&&nmxFtFlagsOk(3)&&nmxFtFlagsOk(2)&&nmxFtFlagsOk(4)); CHECK(!nmxFtFlagsOk(0)&&!nmxFtFlagsOk(5)&&!nmxFtFlagsOk(6)&&!nmxFtFlagsOk(7)&&!nmxFtFlagsOk(8));
  printf("%s (%d failures)\n", fails?"FAIL":"PASS", fails); return fails!=0;
}
