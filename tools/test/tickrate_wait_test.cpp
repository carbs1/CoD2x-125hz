#include "tickrate_wait.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void check(bool value) { if (!value) std::exit(1); }
static uint32_t run(double seconds,int repeats,int step) {
    tickrate::WaitPhase phase;
    uint32_t now=100,prior=0;
    for(int i=0;i<repeats;++i) {
        auto count=tickrate::preciseWait(phase,seconds,step,now,prior,1);
        now=(now+count)&tickrate::scriptTimeMask;prior=now;
    }
    return (now-100)&tickrate::scriptTimeMask;
}
int main() {
    using namespace tickrate;
    check(run(.05f,100,8)*8==5000);
    check(run(.05f,200,8)*8==10000);
    check(run(.05f,20,8)*8==1000);
    check(run(60,1,8)*8==60000);
    check(run(.05f,100,10)*10==5000);
    check(run(1,1000,33)*33>=999984 && run(1,1000,33)*33<=1000017);
    WaitPhase a,b;
    check(preciseWait(a,.05f,8,100,0,1)==6);
    check(preciseWait(b,.05f,8,100,0,1)==6);
    check(preciseWait(a,.05f,8,106,106,1)==7);
    check(preciseWait(b,.05f,8,106,106,1)==7);
    check(preciseWait(a,.05f,8,113,0,1)==6); // reused native thread ID
    check(preciseWait(a,.05f,8,119,119,2)==6); // new map/config generation
    check(preciseWait(a,.05f,8,140,125,2)==6); // event/late resume
    check(preciseWait(a,0,8,140,140,2)==0);
    check(preciseWait(a,.001,8,140,0,2)==1);
    check(preciseWait(a,-1,8,140,0,2)==scriptTimeMask);
    check(preciseWait(a,std::numeric_limits<double>::infinity(),8,140,0,2)==scriptTimeMask);
    check(preciseWait(a,std::numeric_limits<double>::quiet_NaN(),8,140,0,2)==scriptTimeMask);
    check(preciseWait(a,1,0,140,0,2)==scriptTimeMask);
    check(preciseWait(a,1e20,8,140,0,2)==scriptTimeMask);
    WaitPhase wrap;
    check(preciseWait(wrap,.05f,8,scriptTimeMask-5,0,1)==6);
    check(wrap.wake==0);
    check(preciseWait(wrap,.05f,8,0,0,1)==6); // wrap breaks the chain safely
    check(!realtimeWaitMode(0,false) && realtimeWaitMode(1,true));
    check(realtimeWaitMode(2,false) && !realtimeWaitMode(2,true));
    check(!modCompensatesWaits("wait level.bombtimer;"));
    check(modCompensatesWaits("wait level.fps_multiplier * level.bombtimer;"));
    check(modCompensatesWaits("wait LEVEL /* comment */ . FPS_MULTIPLIER * 60;"));
    check(!modCompensatesWaits("// level.fps_multiplier\nwait 60;"));
    check(!modCompensatesWaits("/* level.fps_multiplier */ wait 60;"));
    check(!modCompensatesWaits("println(\"level.fps_multiplier\"); wait 60;"));
    check(!modCompensatesWaits("wait level.fps_multiplier_other * 60;"));
    check(modCompensatesWaits(nullptr));
    std::puts("script wait policy, repeated waits and thread lifecycle: passed");
}
