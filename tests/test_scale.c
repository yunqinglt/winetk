#include "winetk/scene.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const uint8_t source[]={0,0,0,0, 255,0,0,64, 0,255,0,128, 0,0,255,255};
    uint8_t output[6*5*4];
    wt_scale_rgba(source,2,2,output,2,2);
    assert(!memcmp(source,output,sizeof(source)));
    wt_scale_rgba(source,2,2,output,4,4);
    /* Quarter-pixel samples mix all four neighbors instead of forming blocks. */
    assert(!memcmp(output+(1*4+1)*4,(uint8_t[]){48,48,16,52},4));
    assert(!memcmp(output+(2*4+2)*4,(uint8_t[]){48,48,143,179},4));
    assert(!memcmp(output,source,4));
    assert(!memcmp(output+(3*4+3)*4,source+12,4));
    /* One-pixel axes and clamped borders must never sample beyond the source. */
    const uint8_t column[]={0,0,0,255, 255,128,64,255};
    wt_scale_rgba(column,1,2,output,6,5);
    for (unsigned x=0;x<6;++x) {
        assert(!memcmp(output+x*4,column,4));
        assert(!memcmp(output+(2*6+x)*4,(uint8_t[]){128,64,32,255},4));
        assert(!memcmp(output+(4*6+x)*4,column+4,4));
    }
    wt_scale_rgba(source+4,1,1,output,6,5);
    for (unsigned i=0;i<6*5;++i) assert(!memcmp(output+i*4,source+4,4));
    /* Keep the existing shrink sampling contract. */
    wt_scale_rgba(source,2,2,output,1,1);
    assert(!memcmp(output,source,4));
    puts("RGBA scaling: passed");
    return 0;
}
