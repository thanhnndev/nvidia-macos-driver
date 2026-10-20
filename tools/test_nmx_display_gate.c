#include "../plugin/NVMTLDisplayGate.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(!nvmtl_ws_display_gate(0x1002, false, false));
    assert(!nvmtl_ws_display_gate(0x1002, false, true));
    assert(nvmtl_ws_display_gate(0x1002, true, false));
    assert(nvmtl_ws_display_gate(0x1002, true, true)); /* NVIDIA cannot veto the AMD owner. */
    assert(nvmtl_ws_display_gate(0x10de, false, false));
    assert(!nvmtl_ws_display_gate(0x10de, true, true)); /* AMD readiness cannot arm NVIDIA. */
    assert(!nvmtl_ws_display_gate(0x8086, true, false));
    assert(!nvmtl_ws_display_gate(0, true, false));
    puts("PASS: 8 vendor/ownership/headless display gates");
}
