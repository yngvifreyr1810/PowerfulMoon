// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include "../runtime/hd_wire.h"
int main(int argc,char **argv) {
    unsigned char b[56];PMVibration v[2]={{1,160,.5,320},{0,160,0,320}};
    pm_hd_encode(b,0x12345678,0x1122334455667788ull,v);
    assert(!memcmp(b,"PC2H\1\0\0\0\x78\x56\x34\x12\xc8\0\0\0",16));
    assert(b[16]==0x88 && b[23]==0x11);
    const unsigned char expected[16]={0,0,0x80,0x3f,0,0,0x20,0x43,0,0,0,0x3f,0,0,0xa0,0x43};
    assert(!memcmp(b+24,expected,16));
    if (argc>1) { FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(b,1,56,f)==56);fclose(f); }
    puts("HD wire layout, sequence, 64-bit timestamp and independent float bands: PASS");return 0;
}
