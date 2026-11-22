#include "competitive.h"

#include <windows.h>

#include "shared.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_client.h"


int competitive_clientStateLast = 0;
// 125 Hz fork: the player's own com_maxfps above the limit, put back when a server allows it again (it is saved in the config,
// so without this a cap of 500 set for a server with fixed movement steps would stay 250 after a visit to one without)
static int competitive_wantedMaxFps = 0;

extern dvar_t* g_competitive;
extern dvar_t* g_pmoveFixedMsec;


void competitive_cmd_wait() {

    bool block = g_competitive->value.boolean;

    // Prevent blocking wait command untill connnected to the server
    if (COD2X_WIN32 && (clientState < CLIENT_STATE_PRIMED || demo_isPlaying)) {
        block = false;
    }

    if (block) {
        Com_Printf("Wait command is disabled by the server via competitive settings.\n");
        return;
    }
    ASM_CALL(RETURN_VOID, 0x00420a80);
}

/** Called every frame on frame start. */
void competitive_frame() {

    // Competitive settings - cvar that controls all other limits via one cvar
    // 125 Hz fork: also when the server's fixed movement steps change, which lift the FPS limit
    if (g_competitive->modified || (g_pmoveFixedMsec && g_pmoveFixedMsec->modified)) {
        g_competitive->modified = false;
        if (g_pmoveFixedMsec)
            g_pmoveFixedMsec->modified = false;

        // If demo is playing, ignore competitive settings
        // The cvar will be controled by MOD (like zPAM) on the server, we don't want to limit things when replaying the demo
        if (demo_isPlaying) {
            Dvar_SetBool(g_competitive, false);
        }

        dvar_t* com_maxFps = Dvar_GetDvarByName("com_maxFps");
        dvar_t* rate = Dvar_GetDvarByName("rate");
        dvar_t* snaps = Dvar_GetDvarByName("snaps");
        dvar_t* cl_maxpackets = Dvar_GetDvarByName("cl_maxpackets");
        dvar_t* sc_shadows = Dvar_GetDvarByName("sc_enable");
        dvar_t* fx_sort = Dvar_GetDvarByName("fx_sort");
        dvar_t* mss_q3fs = Dvar_GetDvarByName("mss_q3fs");


        // Apply competitive settings
        if (g_competitive->value.boolean) {
            
            // FPS: set the upper and lower limits. 125 Hz fork: up to 1000 when the server moves everyone in fixed steps
            // (g_pmoveFixedMsec), where the frame rate no longer changes speed, jumps or footsteps (movement.cpp)
            int maxFps = (g_pmoveFixedMsec && g_pmoveFixedMsec->value.integer > 0) ? 1000 : 250;
            com_maxFps->limits.integer.min = 125;
            com_maxFps->limits.integer.max = maxFps;
            if (com_maxFps->value.integer > maxFps)
                competitive_wantedMaxFps = com_maxFps->value.integer;
            else if (competitive_wantedMaxFps > com_maxFps->value.integer && competitive_wantedMaxFps <= maxFps) {
                Dvar_SetInt(com_maxFps, competitive_wantedMaxFps);
                competitive_wantedMaxFps = 0;
            }
            if (com_maxFps->value.integer < 125 || com_maxFps->value.integer > maxFps) {
                Dvar_SetInt(com_maxFps, com_maxFps->value.integer < 125 ? 250 : maxFps); // it will also set the latched value
            }

            // Rate and snaps: 125 Hz fork, the most this build allows (100000 and 125, CoD2x: 25000 and 40), what its servers send
            // anyway with sv_minRate and sv_minSnaps; the console shows what the player gets. A server that allows less limits it itself
            rate->limits.integer.min = 100000;
            if (rate->value.integer != 100000) {
                Dvar_SetInt(rate, 100000);
            }
            // Snaps
            snaps->limits.integer.min = 125;
            if (snaps->value.integer != 125) {
                Dvar_SetInt(snaps, 125);
            }
            // cl_maxpackets
            cl_maxpackets->limits.integer.min = 125;
            if (cl_maxpackets->value.integer != 125) {
                Dvar_SetInt(cl_maxpackets, 125);
            }
            // Shadows          
            sc_shadows->type = DVAR_TYPE_INT;
            sc_shadows->limits.integer.min = 0;
            sc_shadows->limits.integer.max = 0;
            if (sc_shadows->value.integer != 0) {
                Dvar_SetInt(sc_shadows, 0);
            }
            // Fx_sort
            fx_sort->type = DVAR_TYPE_INT;
            fx_sort->limits.integer.min = 1;
            fx_sort->limits.integer.max = 1;
            if (fx_sort->value.integer != 1) {
                Dvar_SetInt(fx_sort, 1);
            }
            // mss_q3fs
            mss_q3fs->type = DVAR_TYPE_INT;
            mss_q3fs->limits.integer.min = 1;
            mss_q3fs->limits.integer.max = 1;
            if (mss_q3fs->value.integer != 1) {
                Dvar_SetInt(mss_q3fs, 1);
            }

        // Restore the default limits
        } else {

            // FPS
            com_maxFps->limits.integer.min = 0;
            com_maxFps->limits.integer.max = 1000;
            if (competitive_wantedMaxFps > com_maxFps->value.integer) {     // 125 Hz fork: the player's own cap again
                Dvar_SetInt(com_maxFps, competitive_wantedMaxFps);
                competitive_wantedMaxFps = 0;
            }
            // Rate
            rate->limits.integer.min = 1000;
            // Snaps
            snaps->limits.integer.min = 1;
            // cl_maxpackets
            cl_maxpackets->limits.integer.min = 10;
            // Shadows
            sc_shadows->type = DVAR_TYPE_BOOL;
            sc_shadows->limits.integer.min = 0;
            // Fx_sort
            fx_sort->type = DVAR_TYPE_BOOL;
            fx_sort->limits.integer.min = 0;
            // mss_q3fs
            mss_q3fs->type = DVAR_TYPE_BOOL;
            mss_q3fs->limits.integer.min = 0;

        }
    }

    // Connections state changed
    if (clientState != competitive_clientStateLast) {
        //SHOW_ERROR("Client state changed from %d to %d", clientStateLast, clientState);
        competitive_clientStateLast = clientState;

        // If player is not connected to a server, reset the limit
        if (clientState == CLIENT_STATE_DISCONNECTED) 
        {
            if (g_competitive->value.boolean)
                Dvar_SetBool(g_competitive, false);
        }
    }
}


/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void competitive_init() {
    competitive_clientStateLast = clientState;
}


/** Called before the entry point is called. Used to patch the memory. */
void competitive_patch() {
    patch_int32(0x004344f9 + 1, (unsigned int)competitive_cmd_wait); // Cmd_AddCommand("wait", cmd_wait); in Com_Init
}