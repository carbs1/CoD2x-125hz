#include "cgame.h"

#include <windows.h>

#include "shared.h"
#include "../shared/cod2_client.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_cmd.h"
#include "../shared/animation.h"
#include "tickrate_history.h"
#include "../shared/tickrate_patch.h"
#include <math.h>
#include <vector>



#define clientState                   (*((clientState_e*)0x00609fe0))
#define cl_snapServerTime             (*((int*)0x0096b670))
#define cl_serverTimeDelta            (*((int*)0x0096dd4c))
#define cl_oldServerTime              (*((int*)0x0096dd50))
#define sv_cheats                     (*((dvar_t**)0x00c5c5cc))
#define cg_thirdperson                (*((dvar_t**)0x014b5bdc))
#define cg_thirdPersonAngle           (*((dvar_t**)0x0166e024))
#define cg_thirdPersonRange           (*((dvar_t**)0x0166baa0))
#define bgs_client                    (*((bgs_t*)0x0151ad98))

dvar_t* cg_thirdPersonMode;
dvar_t* cl_hideServerIp;
bool cgame_hideServerIp = false; // cl_hideServerIp or sv_hideServerIp of the connected server, updated every frame

extern dvar_t* g_cod2x;
extern dvar_t* g_grenadeMoveSpeedScale;
extern dvar_t* g_pmoveFixedMsec;
extern dvar_t* g_radarPlayerColors;
extern dvar_t* g_radarEnemyPings;

int cgame_clientStateLast = -1;
bool cgame_demoPlayingLast = false;
int cgame_developerLast = 0;


void Cmd_Increase_Decrease() {
    const char* cmd = Cmd_Argv(0);
    
    if (Cmd_Argc() != 2) {
        Com_Printf("%s <variablename> : increase value\n", cmd);
        return;
    }

    int sign = 1;
    if (strcmp(cmd, "decrease") == 0) {
        sign = -1;
    }

    const char* dvarName = Cmd_Argv(1);
	dvar_t* dvar = Dvar_GetDvarByName(dvarName);

    if (dvar == NULL) {
        Com_Printf("%s not found\n", dvarName);
        return;
    }

    if (dvar->type == DVAR_TYPE_INT) {
        Dvar_SetInt(dvar, dvar->value.integer + sign);
    } else if (dvar->type == DVAR_TYPE_FLOAT) {
        Dvar_SetFloat(dvar, dvar->value.decimal + sign);
    } else {
        Com_Printf("%s is not an int or float\n", dvarName);
    }
}

dvar_t* cl_interpDelay;
dvar_t* cl_animSpeedWindow;

// Adjusts the client's clock after a new snapshot. It aims one snapshot interval + 5 ms behind the newest snapshot and moves
// by 1 or 2 ms per snapshot. Both are too tight at a high snapshot rate: at 125 snaps it is only 13 ms behind, so a little jitter
// runs out of snapshots to interpolate between, and the clock is pulled by up to 25% of its speed.
// 00402d70    void CL_AdjustTimeDelta()
static void CL_AdjustTimeDelta()
{
    // CoD2x: keep a minimum delay behind the newest snapshot, 0 is the original behavior
    static int lastOffset = 5;
    static int lastSlowAdjustTime = 0;

    const int interval = cl_snapServerTime - cl_oldServerTime;
    const int minDelay = cl_interpDelay->value.integer;

    int offset = 5;
    if (minDelay > 0 && interval > 0 && minDelay - interval > offset)
        offset = minDelay - interval;
    if (offset != lastOffset)
    {
        // 00402daa  83ee05             sub     esi, 5
        patch_byte(0x00402daa + 2, (uint8_t)offset);
        lastOffset = offset;
    }

    const int deltaBefore = cl_serverTimeDelta;
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x00402d70, 0);

    // CoD2x: above 40 snaps, adjust slowly no more often than at 20 snaps. Big corrections stay immediate.
    const int change = cl_serverTimeDelta - deltaBefore;
    if (minDelay > 0 && change != 0 && change >= -2 && change <= 2 && interval > 0 && interval < 25)
    {
        const int sinceLast = cl_snapServerTime - lastSlowAdjustTime;
        if (sinceLast >= 0 && sinceLast < 50)
            cl_serverTimeDelta = deltaBefore;
        else
            lastSlowAdjustTime = cl_snapServerTime;
    }
    // CoD2x: End
}

dvar_t* cg_obituaryFilter;

// The kill event's fields CG_Obituary reads: 004e03f0 CG_Obituary, eax = entityState_t*, "mov ecx, [eax+0x78]" (attacker),
// victim at +0x74, weapon or cause at +0xa0
static_assert(offsetof(entityState_t, otherEntityNum) == 0x74, "victim field moved");
static_assert(offsetof(entityState_t, attackerEntityNum) == 0x78, "attacker field moved");

/**
 * Whether a kill goes into the kill feed. cg_obituaryFilter:
 *   0 = every kill (the game's own behavior),
 *   1 = only kills the local player made or died in (so "Killed by" stays too),
 *   2 = only kills the local player made.
 * The local player is the one whose game it is - in a demo, the player who recorded it, also while they watch someone else.
 * Called from CG_Obituary_Filter below, before CG_Obituary; returning 0 skips the kill entirely.
 */
extern "C" int __cdecl cgame_obituaryShown(const entityState_t* es) {
    const int mode = cg_obituaryFilter ? cg_obituaryFilter->value.integer : 0;
    if (mode <= 0)
        return 1;
    const int me = cg.clientNum;
    if (es->attackerEntityNum == me)
        return 1;
    return mode == 1 && es->otherEntityNum == me;
}

// Takes the place of the call to CG_Obituary in CG_EntityEvent (case EV_OBITUARY). CG_Obituary takes its arguments in registers
// (eax = the event's entityState_t, edi = a second value), so this stub keeps eax, asks cgame_obituaryShown (which, as a cdecl
// function, leaves edi alone), and then either continues into the original CG_Obituary - which returns straight to CG_EntityEvent -
// or returns without it.
extern "C" void CG_Obituary_Filter();
__asm__(
    ".globl _CG_Obituary_Filter\n"
    "_CG_Obituary_Filter:\n"
    "    pushl %eax\n"                      // keep the entity for CG_Obituary
    "    pushl %eax\n"                      // argument of cgame_obituaryShown
    "    call _cgame_obituaryShown\n"
    "    addl $4, %esp\n"
    "    testl %eax, %eax\n"
    "    popl %eax\n"                       // the entity again (pop leaves the flags as they are)
    "    jz 1f\n"
    "    pushl $0x004e03f0\n"               // CG_Obituary
    "    ret\n"
    "1:\n"
    "    ret\n"
);

dvar_t* cg_drawServerHud;

/**
 * Draws the HUD elements the server's scripts made (newHudElem / newClientHudElem: zPAM's round clock, team score, players-left
 * counts, "Strat Time" ...), unless cg_drawServerHud is 0. They come to the client inside the snapshots, so a demo carries them and
 * no menu file can hide them. The kill feed and the "You killed" / "Killed by" text are not HUD elements and stay.
 * Takes the place of the three calls to CG_Draw2dHudElems (004ddff0, cdecl, one argument: 0 = drawn before the 2D HUD, 1 = after)
 * in CG_Draw2D.
 */
extern "C" void __cdecl CG_Draw2dHudElems_Filter(int foreground) {
    if (cg_drawServerHud && !cg_drawServerHud->value.boolean)
        return;
    ((void (__cdecl*)(int))0x004ddff0)(foreground);
}


/**
 * 125 Hz fork: a smooth view while following a player and in demos (cl_followSmooth).
 *
 * The view then comes from the snapshots' player state, blended between two snapshots by snapshot time (CG_InterpolatePlayerState).
 * But a player moves when his packets reach the server, not on its frames: at 8 ms snapshots the followed player's state did not move
 * in 16% of the snapshots while he was moving, then jumped two steps (demo up_st_lib, 2026-11-18), so the view stopped and lurched.
 * Each player state carries the time of the command it was made by (commandTime), so the view is blended between the player states
 * around a time by their command time instead, drawn a steady delay behind: the largest lag between snapshot and command time of
 * the last FOLLOW_LAG_WINDOW ms, plus FOLLOW_LAG_MARGIN, rising fast when packets come later and easing back slowly.
 * Replayed on that demo at 250 FPS: frames that stood still while moving 16.3% -> 0%, about 9 ms later than the original view.
 */
dvar_t* cl_followSmooth;

#define FOLLOW_SAMPLES      64
#define FOLLOW_LAGS         128
#define FOLLOW_LAG_WINDOW   250         // ms of snapshots whose largest lag sets the delay
#define FOLLOW_LAG_MARGIN   2           // ms added to it
#define FOLLOW_LAG_RISE     0.5f        // ms of delay added per ms when it must grow
#define FOLLOW_LAG_FALL     0.05f       // ms of delay taken away per ms when it can shrink
#define FOLLOW_TELEPORT     64.0f       // units between two player states that start a new history (spawn, switching player)

struct followSample_t {
    int commandTime;
    vec3_t origin;
    vec3_t velocity;
    vec3_t viewangles;
};
struct followLag_t {
    int serverTime;
    int lag;
};
static followSample_t follow_samples[FOLLOW_SAMPLES];  // a ring, oldest to newest by command time
static int follow_sampleCount = 0;
static int follow_sampleNewest = -1;
static followLag_t follow_lags[FOLLOW_LAGS];          // a ring of the snapshots' lags
static int follow_lagCount = 0;
static int follow_lagNewest = -1;
static int follow_clientNum = -1;
static int follow_lastServerTime = 0;
static int follow_lastTime = 0;
static float follow_delay = -1.0f;

static void follow_reset() {
    follow_sampleCount = 0;
    follow_sampleNewest = -1;
    follow_lagCount = 0;
    follow_lagNewest = -1;
    follow_clientNum = -1;
    follow_lastServerTime = 0;
    follow_delay = -1.0f;
}

static float follow_lerpAngle(float from, float to, float frac) {
    float delta = to - from;
    delta -= 360.0f * floorf((delta + 180.0f) / 360.0f);
    return from + delta * frac;
}

static void follow_addSnapshot(const snapshot_t* snap) {
    int serverTime = snap->pad[2];
    const playerState_t* ps = &snap->ps;
    if (serverTime <= follow_lastServerTime && follow_clientNum == ps->clientNum)
        return;   // already added

    if (follow_sampleCount > 0) {
        const followSample_t* newest = &follow_samples[follow_sampleNewest];
        float dx = ps->origin[0] - newest->origin[0], dy = ps->origin[1] - newest->origin[1], dz = ps->origin[2] - newest->origin[2];
        if (ps->clientNum != follow_clientNum || ps->commandTime < newest->commandTime || serverTime - follow_lastServerTime > 500 ||
            serverTime < follow_lastServerTime || dx * dx + dy * dy + dz * dz > FOLLOW_TELEPORT * FOLLOW_TELEPORT)
            follow_reset();
    }
    follow_clientNum = ps->clientNum;
    follow_lastServerTime = serverTime;

    // A new player state only when the command time moved on
    if (follow_sampleCount == 0 || ps->commandTime > follow_samples[follow_sampleNewest].commandTime) {
        follow_sampleNewest = (follow_sampleNewest + 1) % FOLLOW_SAMPLES;
        followSample_t* s = &follow_samples[follow_sampleNewest];
        s->commandTime = ps->commandTime;
        for (int i = 0; i < 3; i++) {
            s->origin[i] = ps->origin[i];
            s->velocity[i] = ps->velocity[i];
            s->viewangles[i] = ps->viewangles[i];
        }
        if (follow_sampleCount < FOLLOW_SAMPLES)
            follow_sampleCount++;
    }

    follow_lagNewest = (follow_lagNewest + 1) % FOLLOW_LAGS;
    follow_lags[follow_lagNewest].serverTime = serverTime;
    follow_lags[follow_lagNewest].lag = serverTime - ps->commandTime;
    if (follow_lagCount < FOLLOW_LAGS)
        follow_lagCount++;
}

/** Takes the place of the call to CG_InterpolatePlayerState (004de7c0, cdecl, one argument: 1 = view angles from the local input) in CG_PredictPlayerState. */
extern "C" void __cdecl CG_InterpolatePlayerState_Follow(int grabAngles) {
    ((void (__cdecl*)(int))0x004de7c0)(grabAngles);

    // Only where the view comes from the snapshots: in a demo or while following (CG_PredictPlayerState calls it with 0 then)
    if (grabAngles || !cl_followSmooth || !cl_followSmooth->value.boolean || !cg.snap || !cg.nextSnap) {
        follow_reset();
        return;
    }

    follow_addSnapshot(cg.snap);
    follow_addSnapshot(cg.nextSnap);
    if (follow_sampleCount < 2 || follow_clientNum != cg.predictedPlayerState.clientNum)
        return;

    // The delay: the largest lag of the last FOLLOW_LAG_WINDOW ms of snapshots
    int newestServerTime = follow_lags[follow_lagNewest].serverTime;
    int target = 0;
    for (int i = 0; i < follow_lagCount; i++) {
        const followLag_t* l = &follow_lags[(follow_lagNewest - i + FOLLOW_LAGS) % FOLLOW_LAGS];
        if (l->serverTime <= newestServerTime - FOLLOW_LAG_WINDOW)
            break;
        if (l->lag > target)
            target = l->lag;
    }
    target += FOLLOW_LAG_MARGIN;

    int frameMsec = cg.time - follow_lastTime;
    if (frameMsec < 0) frameMsec = 0;
    if (frameMsec > 50) frameMsec = 50;
    follow_lastTime = cg.time;
    if (follow_delay < 0.0f)
        follow_delay = (float)target;
    else if (target > follow_delay)
        follow_delay = fminf((float)target, follow_delay + frameMsec * FOLLOW_LAG_RISE);
    else
        follow_delay = fmaxf((float)target, follow_delay - frameMsec * FOLLOW_LAG_FALL);

    // The player states around that command time, the newest when it is not there yet
    float time = (float)cg.time - follow_delay;
    const followSample_t* older = NULL;
    const followSample_t* newer = &follow_samples[follow_sampleNewest];
    for (int i = 0; i < follow_sampleCount; i++) {
        const followSample_t* s = &follow_samples[(follow_sampleNewest - i + FOLLOW_SAMPLES) % FOLLOW_SAMPLES];
        if (s->commandTime <= time) {
            older = s;
            break;
        }
        newer = s;
    }
    float frac = 0.0f;
    if (older == NULL)
        older = newer;                              // all newer: the oldest
    else if (older == &follow_samples[follow_sampleNewest])
        newer = older;                              // not there yet: the newest
    else
        frac = (time - older->commandTime) / (float)(newer->commandTime - older->commandTime);

    playerState_t* ps = &cg.predictedPlayerState;
#ifdef FOLLOWSMOOTH_LOG
    float before[3] = { ps->origin[0], ps->origin[1], ps->origin[2] };
#endif
    for (int i = 0; i < 3; i++) {
        ps->origin[i] = older->origin[i] + (newer->origin[i] - older->origin[i]) * frac;
        ps->velocity[i] = older->velocity[i] + (newer->velocity[i] - older->velocity[i]) * frac;
        ps->viewangles[i] = follow_lerpAngle(older->viewangles[i], newer->viewangles[i], frac);
    }
#ifdef FOLLOWSMOOTH_LOG
    // Test builds only: the original and the smoothed view origin of every frame
    static FILE* log = fopen("followsmooth.log", "w");
    if (log)
        fprintf(log, "%d %d %.3f %.3f %.3f %.3f %.3f %.3f %.1f\n", cg.time, ps->clientNum, before[0], before[1], before[2],
                ps->origin[0], ps->origin[1], ps->origin[2], follow_delay);
#endif
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void cgame_init() {
    tickrate_history_init();

    // How far behind the newest snapshot other players are drawn, in ms. Changes nothing up to 40 snaps. 0 = original behavior
    cl_interpDelay = Dvar_RegisterInt("cl_interpDelay", 30, 0, 100, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));

    // 125 Hz fork: how many ms of movement the leg animation speed of other players is measured over. 0 = every snapshot, the original behavior
    cl_animSpeedWindow = Dvar_RegisterInt("cl_animSpeedWindow", 50, 0, 200, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));

    // Register USERINFO cvar that is automatically appended to the client's userinfo string sent to the server
    Dvar_RegisterInt("protocol_cod2x", APP_VERSION_PROTOCOL, APP_VERSION_PROTOCOL, APP_VERSION_PROTOCOL, (enum dvarFlags_e)(DVAR_USERINFO | DVAR_ROM));

    // 125 Hz fork: lets servers of this fork reject players without it
    Dvar_RegisterInt("protocol_125hz", FORK_PROTOCOL, FORK_PROTOCOL, FORK_PROTOCOL, (enum dvarFlags_e)(DVAR_USERINFO | DVAR_ROM));
    
    // Add another mode to thirdperson
    cg_thirdPersonMode = Dvar_RegisterInt("cg_thirdPersonMode", 0, 0, 1, (enum dvarFlags_e)(DVAR_CHEAT | DVAR_CHANGEABLE_RESET));

    // Hide IP addresses for streaming - the server address on the scoreboard and IP addresses in all drawn texts
    // (console, menus, the connect command while typing), see Text_HideIpAddresses in text.cpp.
    // The server can enforce the same with sv_hideServerIp while the client is connected, no matter this setting.
    cl_hideServerIp = Dvar_RegisterBool("cl_hideServerIp", false, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));

    // Which kills go into the kill feed (and the "You killed" / "Killed by" message), see cgame_obituaryShown
    cg_obituaryFilter = Dvar_RegisterInt("cg_obituaryFilter", 0, 0, 2, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));

    // 0 hides the HUD elements the server's scripts draw (zPAM's clock, score, players left ...), see CG_Draw2dHudElems_Filter
    cg_drawServerHud = Dvar_RegisterBool("cg_drawServerHud", true, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));

    // 125 Hz fork: a smooth view while following a player and in demos, see CG_InterpolatePlayerState_Follow. 0 = original behavior
    cl_followSmooth = Dvar_RegisterBool("cl_followSmooth", true, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));


    Cmd_AddCommand("increase", Cmd_Increase_Decrease);
    Cmd_AddCommand("decrease", Cmd_Increase_Decrease);
}


/** Called every frame on frame start. */
void cgame_frame() {

    if (clientState != cgame_clientStateLast) {
        logger_add("Client state changed from %d:%s to %d:%s", cgame_clientStateLast, get_client_state_name(cgame_clientStateLast), clientState, get_client_state_name(clientState));
        Com_DPrintf("Client state changed from %d:%s to %d:%s\n", cgame_clientStateLast, get_client_state_name(cgame_clientStateLast), clientState, get_client_state_name(clientState));
    }


    // Player disconnected from the server, reset the cvar
    if (g_cod2x->value.integer > 0 && clientState != cgame_clientStateLast && clientState <= CLIENT_STATE_CONNECTED) {
        Dvar_SetInt(g_cod2x, 0);
        g_cod2x->modified = true;
    }
    // 125 Hz fork: the grenade walk speed comes from the server's systeminfo, which servers without this fork do not send.
    // Reset it while connecting, the new server's gamestate arrives only after CLIENT_STATE_CONNECTED
    if (g_grenadeMoveSpeedScale->value.decimal != 0 && clientState != cgame_clientStateLast && clientState < CLIENT_STATE_CONNECTED) {
        Dvar_SetFloat(g_grenadeMoveSpeedScale, 0);
    }
    // 125 Hz fork: the radar colors of the players come from the server the same way
    if (g_radarPlayerColors->value.string[0] != '\0' && clientState != cgame_clientStateLast && clientState < CLIENT_STATE_CONNECTED) {
        Dvar_SetString(g_radarPlayerColors, "");
    }
    // 125 Hz fork: the server's rule for the radar's enemy dots too. Not while this game runs the server itself (devmap),
    // whose scripts set it before the local player connects, and which does not send it to itself through systeminfo
    if (!g_radarEnemyPings->value.boolean && !(sv_running && sv_running->value.boolean) && clientState != cgame_clientStateLast && clientState < CLIENT_STATE_CONNECTED) {
        Dvar_SetBool(g_radarEnemyPings, true);
    }
    // 125 Hz fork: the server's fixed movement steps, sent the same way (also not while this game runs the server itself)
    if (g_pmoveFixedMsec->value.integer != 0 && !(sv_running && sv_running->value.boolean) && clientState != cgame_clientStateLast && clientState < CLIENT_STATE_CONNECTED) {
        Dvar_SetInt(g_pmoveFixedMsec, 0);
    }
    // Player just connected to 1.3 server (g_cod2x == 0)
    // Set the cvar modified so the text is printed in the console again below
    if (g_cod2x->value.integer == 0 && clientState != cgame_clientStateLast && clientState == CLIENT_STATE_ACTIVE && cgame_clientStateLast <= CLIENT_STATE_PRIMED) {
        g_cod2x->modified = true;
    }
    // Cvar changed (by server, init or disconenct), apply the appropriate bug fixes
    if (g_cod2x->modified) {
        g_cod2x->modified = false;

        Com_Printf("---------------------------------------------------------------------------------\n");
        if (g_cod2x->value.integer == 0) {
            Com_Printf("CoD2x: Changes turned off, using legacy CoD2 1.3\n");
        } else {
            Com_Printf("CoD2x: Changes turned on, using changes according to server version 1.4.%d.x\n", g_cod2x->value.integer);          
            if (g_cod2x->value.integer != APP_VERSION_PROTOCOL)
                Com_Printf("CoD2x: ^3Server is running older version 1.4.%d.x, your version is %s\n", g_cod2x->value.integer, APP_VERSION);
        }
        Com_Printf("---------------------------------------------------------------------------------\n");

        // Fix animation time from crouch to stand since version 1.4.3.x
        animation_changeFix(g_cod2x->value.integer >= 3);
    }


    // Demo playback state changed
    if (demo_isPlaying != cgame_demoPlayingLast) {
        dvar_t* developer = Dvar_GetDvarByName("developer");

        // Demo playback started
        if (demo_isPlaying) {
            // Enable cheats and developer mode for demo playback
            Dvar_SetBool(sv_cheats, true);
            cgame_developerLast = developer->value.integer;
            Dvar_SetInt(developer, 2);

        // Demo playback stopped
        } else {
            Dvar_SetInt(developer, cgame_developerLast);
        }
    }
    cgame_demoPlayingLast = demo_isPlaying;


    // IP addresses are hidden if the client enables cl_hideServerIp or the connected server sets sv_hideServerIp
    cgame_hideServerIp = cl_hideServerIp->value.boolean ||
        (clientState >= CLIENT_STATE_CONNECTED && Info_ValueForKey(CL_GetConfigString(CS_SERVERINFO), "sv_hideServerIp")[0] == '1');


    // 125 Hz fork: BG_RunLerpFrameRate sets the leg animation speed of other players to the distance they moved since its last
    // update divided by the time passed, and updates it whenever this snapshot time changes. At 125 snaps that is every 8 ms,
    // in which a player moves by whole client packets, so the speed jumped between 0.1x and 2.5x. This time is used only there,
    // so it advances in steps of cl_animSpeedWindow ms instead, like at the stock 20 snaps. The game's own copy is removed in cgame_patch.
    static int animSpeedSnapshotTime = 0;
    const int latestSnapshotTime = cg.latestSnapshotTime;
    const int window = cl_animSpeedWindow->value.integer;
    if (window == 0 || latestSnapshotTime < animSpeedSnapshotTime || latestSnapshotTime - animSpeedSnapshotTime >= window)
        animSpeedSnapshotTime = latestSnapshotTime;
    bgs_client.latestSnapshotTime = animSpeedSnapshotTime;


    cgame_clientStateLast = clientState;
}


void CG_RegisterItems() {
    if (strlen(CL_GetConfigString(CS_ITEMS)) >= 0x108u) {
        Com_Error(ERR_DROP, "CG_RegisterItems: overflowed");
    }

    // Call the original function
    ((void(*)())0x004d5270)();
}



void CG_TraceCapsule(trace_t *result, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int skipNumber, int mask) {
    ASM_CALL(RETURN_VOID, 0x004de690, 7, PUSH(result), PUSH(start), PUSH(mins), PUSH(maxs), PUSH(end), PUSH(skipNumber), PUSH(mask));
}

void CG_OffsetThirdPersonView( void ) {

    // Use the original function if not connected to cod2x server, unless the player is in the new third person mode
    if (g_cod2x->value.integer < 3 && cg_thirdPersonMode->value.integer != 1) {
        // Call the original function
        ((void(*)())0x004ce890)();
        return;
    }

	vec3_t forward, right, up;
	vec3_t view;
	vec3_t focusAngles;
	trace_t trace;
	static vec3_t mins = { -4, -4, -4 };
	static vec3_t maxs = { 4, 4, 4 };
	vec3_t focusPoint;
	float focusDist;

    cg.refdef.vieworg[2] += cg.predictedPlayerState.viewHeightCurrent;

	VectorCopy( cg.refdefViewAngles, focusAngles );

    if (cg.predictedPlayerState.pm_type > 5) {
        focusAngles[1] = cg.predictedPlayerState.stats[1];
        cg.refdefViewAngles[1] = cg.predictedPlayerState.stats[1];
    }

	if ( focusAngles[PITCH] > 45 ) {
		focusAngles[PITCH] = 45;        // don't go too far overhead
	}

	AngleVectors( focusAngles, forward, NULL, NULL );
	VectorMA( cg.refdef.vieworg, 512, forward, focusPoint );

	VectorCopy( cg.refdef.vieworg, view );
	view[2] += 8;
	cg.refdefViewAngles[PITCH] *= 0.5;
    cg.refdefViewAngles[YAW] -= cg_thirdPersonAngle->value.decimal;
        
	AngleVectors( cg.refdefViewAngles, forward, right, up );
	VectorMA( view, -cg_thirdPersonRange->value.decimal, forward, view );

    // CoD2x: New mode that rotates around head without collision
    if (cg_thirdPersonMode->value.integer == 1) {
        VectorCopy( view, cg.refdef.vieworg );
        cg.refdefViewAngles[PITCH] *= 1.2;
        return;
    }
    // CoD2x: end

	// trace a ray from the origin to the viewpoint to make sure the view isn't
	// in a solid block.  Use an 8 by 8 block to prevent the view from near clipping anything
	CG_TraceCapsule( &trace, cg.refdef.vieworg, mins, maxs, view, cg.predictedPlayerState.clientNum, 0x811 );

	if ( trace.fraction != 1.0 ) {

        vec3_t diff;   
        vec3_t endpos;
        VectorSubtract( view, cg.refdef.vieworg, diff );
        VectorMA( cg.refdef.vieworg, trace.fraction, diff, endpos );
		VectorCopy( endpos, view );

		view[2] += ( 1.0 - trace.fraction ) * 32;

		// try another trace to this position, because a tunnel may have the ceiling
		// close enogh that this is poking out
		CG_TraceCapsule( &trace, cg.refdef.vieworg, mins, maxs, view, cg.predictedPlayerState.clientNum, 0x811 );
        
        VectorSubtract( view, cg.refdef.vieworg, diff );
        VectorMA( cg.refdef.vieworg, trace.fraction, diff, endpos );
		VectorCopy( endpos, view );
	}

	VectorCopy( view, cg.refdef.vieworg );

	// select pitch to look at focus point from vieword
	VectorSubtract( focusPoint, cg.refdef.vieworg, focusPoint );
	focusDist = sqrt( focusPoint[0] * focusPoint[0] + focusPoint[1] * focusPoint[1] );
	if ( focusDist < 1 ) {
		focusDist = 1;  // should never happen
	}
    cg.refdefViewAngles[PITCH] = -180 / M_PI * atan2( focusPoint[2], focusDist );
}




/**
 * Returns "ip:port" of the server the client is connected to - drawn in the bottom right corner of the scoreboard.
 * When the client enables cl_hideServerIp or the server enables sv_hideServerIp (for example while a streamed match
 * is running), an empty string is returned so the IP address is not drawn and streams do not leak the server address.
 * The server can always hide it, the client can hide it on any server. The hostname in the bottom left corner stays visible.
 */
// 004120f0    char* CL_GetServerIPAddress()
const char* CL_GetServerIPAddress() {
    // CoD2x: Hide the server IP address if the client or the server requests it
    if (cgame_hideServerIp) {
        return "";
    }
    // CoD2x: End

    // Call the original function
    const char* ret;
    ASM_CALL(RETURN(ret), 0x004120f0);
    return ret;
}


/** Called before the entry point is called. Used to patch the memory. */
void cgame_patch() {

    patch_call(0x004cfb27, (unsigned int)CG_OffsetThirdPersonView);

    patch_call(0x004031dc, (unsigned int)CL_AdjustTimeDelta); // CL_SetCGameTime

    // 125 Hz fork: the snapshot time used for the leg animation speed is set in cgame_frame instead
    patch_nop(0x004dfdde, 6); // 004dfdde  890d78f95c01  mov dword [bgs_client.latestSnapshotTime], ecx (inside CG_ProcessSnapshots)

    // Kill feed filter (cg_obituaryFilter)
    patch_call(0x004e1919, (unsigned int)CG_Obituary_Filter); // 004e1919  e8d2eaffff  call CG_Obituary (inside CG_EntityEvent, case EV_OBITUARY)

    // Server HUD elements on/off (cg_drawServerHud): the three calls to CG_Draw2dHudElems inside CG_Draw2D
    patch_call(0x004cbd46, (unsigned int)CG_Draw2dHudElems_Filter); // 004cbd46  e8a5220100  call CG_Draw2dHudElems (push 0)
    patch_call(0x004cbd80, (unsigned int)CG_Draw2dHudElems_Filter); // 004cbd80  e86b220100  call CG_Draw2dHudElems (push 0)
    patch_call(0x004cbd9f, (unsigned int)CG_Draw2dHudElems_Filter); // 004cbd9f  e84c220100  call CG_Draw2dHudElems (push 1)

    // 125 Hz fork: smooth view while following and in demos (cl_followSmooth). Only changed while the call still holds its original bytes
    {
        std::vector<tickrate::Patch> plan;
        tickrate::redirect(plan, 0x004def04, "follow view", {0xe8,0xb7,0xf8,0xff,0xff}, (const void*)&CG_InterpolatePlayerState_Follow); // call CG_InterpolatePlayerState
        if (tickrate::verifyPatches(plan))
            tickrate::applyPatches(plan);
    }

    // Hook the function that returns the server address drawn at the bottom of the scoreboard,
    // so it can be hidden by the client cvar cl_hideServerIp or the serverinfo cvar sv_hideServerIp
    patch_call(0x004d9387, (unsigned int)CL_GetServerIPAddress); // 004d9387  e8648df3ff  call CL_GetServerIPAddress (inside CG_DrawScoreboard)



    // Cvar "snaps" max value change from 30 to 125
    patch_int32(0x00411067 + 1, 125); // 00411067  bb1e000000         mov     ebx, 30

    // Cvar "rate" max value change from 25000 to 100000
    patch_int32(0x00411025 + 1, 100000); // 00411025  bba8610000         mov     ebx, 25000

    // Cvar cl_maxpackets max value change from 100 to 125
    patch_int32(0x00410c64 + 1, 125); // 00410c64  bb64000000         mov     ebx, 100


    // Make "RECORDING %s" message smaller
    patch_float(0x004147e0 + 1, 0.2f); // 004147e0  68abaaaa3e push    0.333333343


    patch_jump(0x004d0c6b, (unsigned int)CG_RegisterItems);
    patch_call(0x004bff65, (unsigned int)CG_RegisterItems); 
}
