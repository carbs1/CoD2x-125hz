#include "tickrate.h"

#include "shared.h"
#include "cod2_dvars.h"
#include "cod2_server.h"
#include "patch.h"
#include "cod2_common.h"
#include "cod2_entity.h"
#include "cod2_cmd.h"
#include "tickrate_patch.h"


// Server frame rate above the original 20.
// sv_fps itself always accepted up to 1000, these are the places in the game that assumed 20 frames per second.

void SV_UserinfoChanged(client_t* cl);

dvar_t* sv_minSnaps;
dvar_t* sv_minRate;
dvar_t* sv_realtimeWait;

// Script wait is counted in server frames, by default 20 per second of wait no matter what sv_fps is.
// Mods like zPAM correct for that on their own (level.fps_multiplier), so this must stay optional.
static float tickrate_waitFramesPerSecond = 20.0f;

// How fast a dropped item moved since the last frame, G_RunItem uses the same frame time for the client's trajectory
static float tickrate_itemVelocityScale = 20.0f;
int tickrate::frameMsec = 50;
static bool historyInstalled = false;

static void printHistoryStatus()
{
	Com_Printf("TICKRATE_HISTORY server_slots=%d archive_frames=%d archive_mb=%d\n",
		tickrate::maxDeltaSlots, tickrate::archiveFrames, tickrate::archiveBytes / (1024 * 1024));
}

static void historyCommand()
{
	printHistoryStatus();
	if (!sv_maxclients || !svs_clients) return;
	for (int i = 0; i < sv_maxclients->value.integer; ++i) {
		if (svs_clients[i].state >= CS_CONNECTED) tickrate::printDeltaStats(i);
	}
}

// Only the manned path runs every world frame. The idle turret sound path
// remains on its original think cadence and must keep its own 50 ms decrement.
// The native routine tests remaining > 0 and then subtracts exactly 50, so
// pre-adding the difference makes one configured frame elapse per call.
static void turretSoundClock(gentity_t* ent)
{
	if (!ent->pTurretInfo) return;
	int& remaining = *(int*)((char*)ent->pTurretInfo + 40);
	// Keep the native positive-timer branch reachable even when a slow
	// configured frame consumes the entire remaining duration.
	if (remaining > 0) {
		remaining += 50 - tickrate::frameMsec;
		if (remaining <= 0) remaining = 1;
	}
}
#if COD2X_WIN32
static int turretSoundWin32()
{
	gentity_t* ent;
	ASM(movr, ent, "eax");
	turretSoundClock(ent);
	int result;
	ASM_CALL(RETURN(result), 0x0051c8e0, 0, EAX(ent));
	return result;
}
#else
static void turretSoundLinux(gentity_t* ent)
{
	turretSoundClock(ent);
	reinterpret_cast<void (__cdecl*)(gentity_t*)>(0x0810b926)(ent);
}
#endif

static dvar_t* tickrate_getFps()
{
	static dvar_t* sv_fps = NULL;
	if (!sv_fps)
		sv_fps = Dvar_GetDvarByName("sv_fps");
	return sv_fps;
}

// Values in the game's code that depend on the frame time
static void tickrate_apply(int msec, bool realtimeWait)
{
	const int frames = 1000 / msec;

	// G_RunItem: BG_EvaluateTrajectory(&ent->s.pos, level.time + 50, origin)
	patch_byte(ADDR(0x00520844 + 2, 0x08107eab + 2), (uint8_t)msec);
	// G_RunItem: ent->s.pos.trDuration = 50
	patch_int32(ADDR(0x005209f5 + 3, 0x08108092 + 3), msec);

	// Rates such as 30 use a 33 ms frame, so integer 1000/33 loses velocity.
	tickrate_itemVelocityScale = 1000.0f / msec;
	tickrate::frameMsec = msec;
	#if COD2X_LINUX
		// G_RunItem: VectorScale(delta, 20.0, ent->s.pos.trDelta)
		patch_float(0x081080de + 1, tickrate_itemVelocityScale);
	#endif

	// VM_Execute: wait, whole seconds and fractions
	const int waitFrames = realtimeWait ? frames : 20;
	tickrate_waitFramesPerSecond = (float)waitFrames;
	patch_int32(ADDR(0x0047e8a8 + 2, 0x08080f30 + 2), waitFrames);
}

// Called at the start of every server frame
void tickrate_frame()
{
	static int lastMsec = 50;
	static bool lastRealtimeWait = false;

	dvar_t* sv_fps = tickrate_getFps();
	if (sv_fps && sv_fps->value.integer > 0)
	{
		const int msec = 1000 / sv_fps->value.integer;
		const bool realtimeWait = sv_realtimeWait->value.boolean;
		if (msec > 0 && (msec != lastMsec || realtimeWait != lastRealtimeWait))
		{
			lastMsec = msec;
			lastRealtimeWait = realtimeWait;
			tickrate_apply(msec, realtimeWait);
		}
	}

	// The limits are applied when the client's settings change, so apply them to the connected clients too
	if (sv_minSnaps->modified || sv_minRate->modified)
	{
		sv_minSnaps->modified = false;
		sv_minRate->modified = false;
		for (int i = 0; i < sv_maxclients->value.integer; i++)
		{
			if (svs_clients[i].state >= CS_CONNECTED)
				SV_UserinfoChanged(&svs_clients[i]);
		}
	}
}

void tickrate_init()
{
	// Storage and all native bounds are installed together, before the first
	// map. Capacities remain valid when this fork changes sv_fps live.
	if (!historyInstalled) {
		std::vector<tickrate::Patch> plan;
		bool valid = tickrate::planDelta(plan);
		valid = tickrate::planTurretCountdown(plan) && valid;
		tickrate::planArchive(plan);
#if COD2X_WIN32
		tickrate::redirect(plan, 0x0051ca35, "mounted turret sound clock", {0xe9,0xa6,0xfe,0xff,0xff},
			reinterpret_cast<const void*>(&turretSoundWin32), 0xe9);
#else
		tickrate::redirect(plan, 0x0810bb61, "mounted turret sound clock", {0xe8,0xc0,0xfd,0xff,0xff},
			reinterpret_cast<const void*>(&turretSoundLinux), 0xe8);
#endif
		valid = tickrate::verifyPatches(plan) && valid;
		if (!valid) {
			tickrate::releaseDelta();
			Com_Error(ERR_FATAL,"Expanded tickrate history verification failed; native arenas unchanged");
			return;
		}
		tickrate::applyPatches(plan);
		historyInstalled = true;
		printHistoryStatus();
	}
	// Clients ask for their own snaps and rate, and most of them still have the settings from 20 and 40 Hz servers.
	// Raises anything below these values, 0 keeps what the client asked for.
	sv_minSnaps = Dvar_RegisterInt("sv_minSnaps", 0, 0, 125, (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
	sv_minRate = Dvar_RegisterInt("sv_minRate", 0, 0, 100000, (dvarFlags_e)(DVAR_CHANGEABLE_RESET));

	sv_realtimeWait = Dvar_RegisterBool("sv_realtimeWait", false, (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
	Cmd_AddCommand("tickrate_history", historyCommand);
}

void tickrate_patch()
{
	// Cvar sv_maxRate max value change from 25000 to 100000, at 125 snaps 25000 is only 200 bytes per snapshot
	patch_int32(ADDR(0x004598c3 + 1, 0x08093c1d + 4), 100000);

	// VM_Execute: wait, fractions of a second. Originally 20.0, the constant is shared with other code.
	patch_int32(ADDR(0x0047e864 + 2, 0x08080eee + 2), (int32_t)&tickrate_waitFramesPerSecond);

	// VM_Execute: wait, whole seconds. Originally multiplied by 20 with shifts, imul eax, [reg], 20 has room for other values.
	#if COD2X_WIN32
		patch_copy(0x0047e8a8, (void*)"\x69\x07\x14\x00\x00\x00\x90\x90", 8);
	#endif
	#if COD2X_LINUX
		patch_copy(0x08080f30, (void*)"\x69\x00\x14\x00\x00\x00\x90\x90\x90\x90\x90\x90", 12);
	#endif

	#if COD2X_WIN32
		// G_RunItem: VectorScale(delta, 20.0, ent->s.pos.trDelta), inlined
		patch_int32(0x00520a35 + 2, (int32_t)&tickrate_itemVelocityScale);
		patch_int32(0x00520a41 + 2, (int32_t)&tickrate_itemVelocityScale);
		patch_int32(0x00520a4d + 2, (int32_t)&tickrate_itemVelocityScale);
	#endif
}
