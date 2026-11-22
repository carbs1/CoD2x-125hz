#include "movement.h"

#include <windows.h>

#include "shared.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_math.h"
#include "../shared/cod2_player.h"
#include "../shared/fixedmove.h"
#include "../shared/tickrate_patch.h"
#include <vector>

// 125 Hz fork: the player's own view between fixed movement steps (g_pmoveFixedMsec, src/shared/fixedmove.cpp).
//
// With fixed steps the prediction ends on the last multiple of the step, up to a step behind the newest command, so on a
// screen faster than the steps the view would move in steps of 0.8 units (at 190 u/s). After CG_PredictPlayerState the
// predicted origin is moved on by the velocity for the time to the newest command (up to one step), and put back before the
// next prediction, so only what is drawn sees it (the view, the player's own entity). cl_pmoveSmooth 0 turns it off.

dvar_t* cl_pmoveSmooth = NULL;

#define cg_predictedPlayerState (*((playerState_t*)0x01513c44))

static bool movement_viewNudged = false;
static vec3_t movement_trueOrigin;
static vec3_t movement_nudgedOrigin;


// 004deae0    void CG_PredictPlayerState()
// Hooked in its caller (004def20), which then copies the predicted origin to the player's own entity.
void CG_PredictPlayerState()
{
    playerState_t* ps = &cg_predictedPlayerState;

    // CoD2x: the exact origin back before predicting again (unless something else wrote the player state since)
    if (movement_viewNudged) {
        if (ps->origin[0] == movement_nudgedOrigin[0] && ps->origin[1] == movement_nudgedOrigin[1] && ps->origin[2] == movement_nudgedOrigin[2])
            VectorCopy(movement_trueOrigin, ps->origin);
        movement_viewNudged = false;
    }
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x004deae0);

    // CoD2x: the view moved on to the newest command
    int step = FixedMove_StepMsec();
    int dt = fixedmove_clientCmdTime - ps->commandTime;
    if (step > 0 && cl_pmoveSmooth && cl_pmoveSmooth->value.boolean && dt > 0 && dt < step) {
        VectorCopy(ps->origin, movement_trueOrigin);
        VectorMA(ps->origin, dt * 0.001f, ps->velocity, ps->origin);
        VectorCopy(ps->origin, movement_nudgedOrigin);
        movement_viewNudged = true;
    }
    // CoD2x: End
}


/** Called only once on game start (SV_Init). */
void movement_init()
{
    cl_pmoveSmooth = Dvar_RegisterBool("cl_pmoveSmooth", true, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));
}

/** Patches the call of CG_PredictPlayerState. Only when it still holds its original bytes. */
void movement_patch()
{
    std::vector<tickrate::Patch> plan;
    tickrate::redirect(plan, 0x004def26, "CG_PredictPlayerState", {0xe8,0xb5,0xfb,0xff,0xff}, (const void*)&CG_PredictPlayerState);
    if (tickrate::verifyPatches(plan))
        tickrate::applyPatches(plan);
}
