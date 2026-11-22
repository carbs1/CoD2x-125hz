#include "fixedmove.h"

#include <string.h>
#include <unordered_map>
#include <vector>

#include "shared.h"
#include "cod2_dvars.h"
#include "cod2_player.h"
#include "tickrate_patch.h"

// 125 Hz fork: movement in fixed steps, so every frame rate moves like 250 fps.
//
// The client turns each frame into one usercmd and moves the player one step of that frame's length (Pmove ->
// PmoveSingle); the server repeats the same steps from the same commands. At the end of every step the game rounds the
// velocity to whole units (Sys_SnapVector), so how much of gravity, acceleration and friction survives depends on the
// length of the step: gravity takes 800 * step off the vertical speed, 3.2 at 4 ms (250 fps, kept as 3), 2.4 at 3 ms
// (333 fps, kept as 2, the high jumps), 1.6 at 2 ms (500 fps, kept as 2) and 0.8 at 1 ms (1000 fps, kept as 1). The
// footstep counter has the same problem (the silent steps at 333).
//
// With g_pmoveFixedMsec set (a server setting, sent to the clients for their prediction), Pmove moves in steps of exactly
// that many ms on whole multiples of it, whatever the frame rate: a command that does not reach the next multiple only
// turns the view (PM_UpdateViewAngles with no frame time, as the game itself does for display), so the mouse still moves
// the view every frame; the command that reaches it makes the step with its own buttons and direction. 4 ms = 250 fps.
//
// A step's "previous command" (pmove_t.oldcmd, for button presses) is the command of the step before, not the command
// before this one, which may only have turned the view: otherwise a press during a step could be lost. They are kept per
// player state by the time they ended, so the client's prediction, which runs the same commands again every frame from
// the last snapshot, finds them too.
//
// Windows: the client's prediction and the server; Linux: the server.

dvar_t* g_pmoveFixedMsec = NULL;
int fixedmove_clientCmdTime = 0;

#define FIXEDMOVE_STEP_HISTORY  512     // steps remembered per player state (at 4 ms about 2 s, more than prediction replays)

#if COD2X_WIN32
#define cg_predictedPlayerState (*((playerState_t*)0x01513c44))
#endif

struct fixedmove_step_t {
    int endTime;
    usercmd_t cmd;
};

struct fixedmove_history_t {
    fixedmove_step_t steps[FIXEDMOVE_STEP_HISTORY];
};

static std::unordered_map<const void*, fixedmove_history_t*> fixedmove_histories;

static fixedmove_history_t* FixedMove_History(const playerState_t* ps)
{
    auto it = fixedmove_histories.find(ps);
    if (it != fixedmove_histories.end())
        return it->second;
    fixedmove_history_t* h = new fixedmove_history_t();
    memset(h, 0, sizeof(*h));
    fixedmove_histories[ps] = h;
    return h;
}

/** The fixed step in ms the server asks for, 0 for the game's own movement. */
int FixedMove_StepMsec()
{
    if (!g_pmoveFixedMsec)
        return 0;
    int msec = g_pmoveFixedMsec->value.integer;
    return msec > 0 ? msec : 0;
}


// 0051af70    void Pmove(struct pmove_t* pm)
// 080e9464    void Pmove(struct pmove_t* pm)
// Called by CG_PredictPlayerState (client prediction, Windows) and the server's client think (two call sites).
void Pmove(pmove_t* pm)
{
    playerState_t* ps = pm->ps;
    int finalTime = pm->cmd.serverTime;

    if (finalTime < ps->commandTime)
        return;     // should not happen

    if (finalTime > ps->commandTime + 1000)
        ps->commandTime = finalTime - 1000;

    pm->numtouch = 0;

    // CoD2x: the newest command of the client's prediction, for cl_pmoveSmooth (src/mss32/movement.cpp)
    #if COD2X_WIN32
    if (ps == &cg_predictedPlayerState)
        fixedmove_clientCmdTime = finalTime;
    #endif
    // CoD2x: End

    // CoD2x: fixed steps on whole multiples of g_pmoveFixedMsec
    int step = FixedMove_StepMsec();
    fixedmove_history_t* history = NULL;
    if (step > 0) {
        int target = finalTime - (finalTime % step);

        // Not at the next multiple yet: only the view follows this command
        if (target <= ps->commandTime) {
            PM_UpdateViewAngles(ps, 0.0f, &pm->cmd, pm->handler);
            return;
        }
        finalTime = target;

        // The command of the step before, for the button presses
        history = FixedMove_History(ps);
        fixedmove_step_t* prev = &history->steps[(unsigned int)(ps->commandTime / step) % FIXEDMOVE_STEP_HISTORY];
        if (prev->endTime == ps->commandTime && prev->endTime != 0)
            memcpy(&pm->oldcmd, &prev->cmd, sizeof(pm->oldcmd));
    }
    // CoD2x: End

    // chop the move up if it is too long, to prevent framerate dependent behavior
    while (ps->commandTime != finalTime) {
        int msec = finalTime - ps->commandTime;

        // CoD2x: fixed steps end on the next multiple (a short first step when the player is not on one)
        if (step > 0) {
            int next = ps->commandTime - (ps->commandTime % step) + step;
            if (next < finalTime)
                msec = next - ps->commandTime;
        } else
        // CoD2x: End
        if (msec > 66)
            msec = 66;

        pm->cmd.serverTime = ps->commandTime + msec;
        PmoveSingle(pm);
        memcpy(&pm->oldcmd, &pm->cmd, sizeof(pm->cmd));
    }

    // CoD2x: remember the command of this step for the next one
    if (history) {
        fixedmove_step_t* done = &history->steps[(unsigned int)(finalTime / step) % FIXEDMOVE_STEP_HISTORY];
        done->endTime = finalTime;
        memcpy(&done->cmd, &pm->cmd, sizeof(done->cmd));
    }
    // CoD2x: End
}


/** Called only once on game start. */
void fixedmove_init()
{
    // A server setting sent to the clients in the systeminfo (their prediction must step the same way). Servers default to 0
    // (the game's own movement), clients keep 0 until a server sends another value and go back to it when they leave
    // (src/mss32/cgame.cpp)
    bool server = COD2X_LINUX || dedicated->value.integer > 0;
    dvarFlags_e noWriteForClientFlag = server ? DVAR_NOFLAG : DEBUG_RELEASE(DVAR_CHEAT, DVAR_NOWRITE);
    g_pmoveFixedMsec = Dvar_RegisterInt("g_pmoveFixedMsec", 0, 0, 33, (enum dvarFlags_e)(noWriteForClientFlag | DVAR_SYSTEMINFO | DVAR_CHANGEABLE_RESET));
}

/** Patches the calls of Pmove. Only when each still holds its original bytes. */
void fixedmove_patch()
{
    std::vector<tickrate::Patch> plan;
#if COD2X_WIN32
    tickrate::redirect(plan, 0x004dee53, "Pmove (CG_PredictPlayerState)", {0xe8,0x18,0xc1,0x03,0x00}, (const void*)&Pmove);
    tickrate::redirect(plan, 0x004ff3e2, "Pmove (server)", {0xe8,0x89,0xbb,0x01,0x00}, (const void*)&Pmove);
    tickrate::redirect(plan, 0x00500294, "Pmove (ClientThink_real)", {0xe8,0xd7,0xac,0x01,0x00}, (const void*)&Pmove);
#else
    tickrate::redirect(plan, 0x080f50e8, "Pmove (server)", {0xe8,0x77,0x43,0xff,0xff}, (const void*)&Pmove);
    tickrate::redirect(plan, 0x080f63de, "Pmove (ClientThink_real)", {0xe8,0x81,0x30,0xff,0xff}, (const void*)&Pmove);
#endif
    if (tickrate::verifyPatches(plan))
        tickrate::applyPatches(plan);
}
