// SPDX-License-Identifier: MIT
#pragma once
#include <string.h>
#include "controller_motion.h"
static inline void pm_hd_u32(unsigned char *p,uint32_t v) {
    for (int i=0;i<4;i++) p[i]=(unsigned char)(v>>(i*8));
}
static inline void pm_hd_encode(unsigned char out[56],uint32_t seq,uint64_t ns,const PMVibration values[2]) {
    memset(out,0,56);memcpy(out,"PC2H",4);out[4]=1;out[12]=200;
    pm_hd_u32(out+8,seq);pm_hd_u32(out+16,(uint32_t)ns);pm_hd_u32(out+20,(uint32_t)(ns>>32));
    for (int side=0;side<2;side++) {
        const float v[4]={values[side].low_amplitude,values[side].low_frequency,values[side].high_amplitude,values[side].high_frequency};
        for (int i=0;i<4;i++) { uint32_t bits;memcpy(&bits,&v[i],4);pm_hd_u32(out+24+side*16+i*4,bits); }
    }
}
