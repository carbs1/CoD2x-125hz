#ifndef FIXEDMOVE_H
#define FIXEDMOVE_H

extern int fixedmove_clientCmdTime;     // Windows client: the newest command its prediction got (src/mss32/movement.cpp)

int FixedMove_StepMsec();
void fixedmove_init();
void fixedmove_patch();

#endif
