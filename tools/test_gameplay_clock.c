#include <assert.h>
#include <stdio.h>
#include "../runtime/gameplay_clock.h"

int main(void) {
    const unsigned rates[] = {30, 35, 40, 45, 50, 60};
    for (unsigned k=0;k<sizeof rates/sizeof *rates;k++) {
        unsigned fps=rates[k], ticks=0;
        GameplayClock c={.last_ns=1};
        for (unsigned frame=1;frame<=fps*10;frame++) {
            unsigned n=gameplay_clock_steps(&c,1+(uint64_t)frame*1000000000ull/fps,true);
            assert(n>=1 && n<=3); ticks+=n;
        }
        assert(ticks>=599 && ticks<=600); // ten seconds of physics at every render rate
    }
    GameplayClock c={.last_ns=1};
    assert(gameplay_clock_steps(&c,500000001,true)==1); // stall: discard old debt
    assert(gameplay_clock_steps(&c,550000001,false)==1); // menu/loading: no recovery
    assert(gameplay_clock_steps(&c,566666668,true)==1);
    assert(gameplay_clock_steps(&c,1,true)==1); // clock reset
    puts("PASS: 60 Hz updates at 30/35/40/45/50/60 draws, bounded recovery and inactive/stall resets");
}
