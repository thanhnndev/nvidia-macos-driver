#include "../kexts/NMXAGDC/nmx-agdc-client.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    uint8_t unaligned[5] = {0, 1, 0, 0, 0};
    uint32_t value = 42;

    assert(nmx_agdc_decode_client_set(unaligned + 1, 4, 0, &value) && value == 1);
    unaligned[1] = 0;
    assert(nmx_agdc_decode_client_set(unaligned + 1, 4, 0, &value) && value == 0);
    assert(!nmx_agdc_decode_client_set(NULL, 4, 0, &value));
    assert(!nmx_agdc_decode_client_set(unaligned + 1, 3, 0, &value));
    assert(!nmx_agdc_decode_client_set(unaligned + 1, 5, 0, &value));
    assert(!nmx_agdc_decode_client_set(unaligned + 1, 4, 4, &value));
    assert(!nmx_agdc_decode_client_set(unaligned + 1, 4, 0, NULL));

    puts("PASS: exact 4-byte client declaration accepted; malformed payload/output rejected");
    return 0;
}
