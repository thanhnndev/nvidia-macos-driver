#include "../plugin/NVMTLHybridPolicy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(nvmtl_hybrid_vendor("WindowServer","amd",true,0x1002)==0x10de);
    assert(nvmtl_hybrid_vendor("WindowServer",NULL,false,0x1002)==0x10de);
    assert(nvmtl_hybrid_vendor("Safari","amd",true,0x1002)==0x10de);
    assert(nvmtl_hybrid_vendor("nvmtltest","amd",true,0x10de)==0x1002);
    assert(nvmtl_hybrid_vendor("mtlprobe","amd",true,0x10de)==0x1002);
    assert(nvmtl_hybrid_vendor("nvmtlrender","nvidia",false,0x1002)==0x10de);
    assert(nvmtl_hybrid_vendor("nvmtltest","bad",true,0x1002)==0x10de);
    assert(nvmtl_hybrid_vendor("nvmtltest",NULL,false,0x1002)==0x1002);
    assert(nvmtl_hybrid_vendor(NULL,"amd",true,0x1002)==0x10de);
    puts("PASS: 9 hybrid backend selection cases");
}
