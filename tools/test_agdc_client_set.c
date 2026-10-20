#include "../kexts/NVRMAGDC/nvrm-agdc-client.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint8_t unaligned[5] = {0,1,0,0,0}; uint32_t value=42;
    assert(nvrm_agdc_decode_client_set(unaligned+1,4,0,&value) && value==1);
    unaligned[1]=0;
    assert(nvrm_agdc_decode_client_set(unaligned+1,4,0,&value) && value==0);
    assert(!nvrm_agdc_decode_client_set(0,4,0,&value));
    assert(!nvrm_agdc_decode_client_set(unaligned+1,3,0,&value));
    assert(!nvrm_agdc_decode_client_set(unaligned+1,5,0,&value));
    assert(!nvrm_agdc_decode_client_set(unaligned+1,4,4,&value));
    assert(!nvrm_agdc_decode_client_set(unaligned+1,4,0,0));
    puts("PASS: unaligned client declarations and malformed payload/output rejection");
}
