#include "../runtime/present_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(pm_interp_hz(60,60,180,120)==120);
    assert(pm_interp_hz(60,180,180,0)==180);
    assert(pm_interp_hz(60,60,60,120)==0);
    assert(pm_interp_hz(60,60,180,0)==60);
    assert(pm_interp_hz(119.99f,119.99f,119.99f,120)==120);
    puts("VRR transient 60 Hz, nominal refresh, true 60 Hz display and 119.99 Hz mode: PASS");
    return 0;
}
