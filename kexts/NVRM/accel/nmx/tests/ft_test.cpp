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
  CHECK(nmxFtFlagsOk(1)&&nmxFtFlagsOk(3)&&nmxFtFlagsOk(2)&&nmxFtFlagsOk(4)); CHECK(!nmxFtFlagsOk(0)&&!nmxFtFlagsOk(5)&&!nmxFtFlagsOk(6)&&!nmxFtFlagsOk(7)&&!nmxFtFlagsOk(8));
  printf("%s (%d failures)\n", fails?"FAIL":"PASS", fails); return fails!=0;
}
