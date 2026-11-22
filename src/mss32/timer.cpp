#include "timer.h"

#include <windows.h>

#include "shared.h"
#include "../shared/cod2_common.h"
#include "../shared/cod2_dvars.h"
#include "../shared/tickrate_patch.h"
#include <vector>

// 125 Hz fork: Windows timer resolution of 0.5 ms instead of the game's 1 ms.
//
// Com_Frame starts a new frame once the game clock (timeGetTime) has moved 1000 / com_maxfps ms (4 ms at 250 fps), waiting with
// Sleep(0) in between. timeGetTime only moves when the Windows timer interrupt fires, and the game asks for one every 1 ms
// (timeBeginPeriod(1)). When an interrupt comes late, which Windows 11 does more often, the clock jumps 2 ms at once and the
// frame takes 5 ms: 200 fps for that frame, shown as 240-248 instead of 250. With an interrupt every 0.5 ms the clock is
// updated twice a millisecond and keeps whole steps. This is what programs such as ISLC do. Dedicated servers use the same
// loop, so their frames (8 ms at sv_fps 125) get steadier too.
//
// A process has one timer resolution request and the last one counts, so the game's own timeBeginPeriod(1) during Com_Init
// replaces an earlier request. It is made again every second, which costs nothing when it is unchanged.
dvar_t* com_timerHighResolution = NULL;

typedef LONG (NTAPI *NtSetTimerResolution_t)(ULONG desiredResolution, BOOLEAN setResolution, PULONG currentResolution);
static NtSetTimerResolution_t timer_NtSetTimerResolution = NULL;
static bool timer_requested = false;
static DWORD timer_lastRequestTime = 0;

static void timer_request(bool enable)
{
    if (!timer_NtSetTimerResolution)
        return;

    ULONG current = 0;
    if (enable) {
        LONG status = timer_NtSetTimerResolution(5000, TRUE, &current); // 100 ns units
        if (!timer_requested) {
            logger_add("Timer resolution 0.5 ms requested, status 0x%08lx, now %lu00 ns", (unsigned long)status, current);
            Com_DPrintf("Timer resolution 0.5 ms requested, status 0x%08lx, now %lu00 ns\n", (unsigned long)status, current);
        }
        timer_requested = (status == 0);
    } else if (timer_requested) {
        timer_NtSetTimerResolution(5000, FALSE, &current);
        timer_requested = false;
    }
}

/** Called every frame on frame start. */
void timer_frame() {
    if (!com_timerHighResolution)
        return;

    DWORD now = GetTickCount();
    if (com_timerHighResolution->modified || (com_timerHighResolution->value.boolean && now - timer_lastRequestTime >= 1000)) {
        com_timerHighResolution->modified = false;
        timer_lastRequestTime = now;
        timer_request(com_timerHighResolution->value.boolean);
    }
}

// 125 Hz fork: exact frame caps.
//
// Com_Frame (00434f70) waits until the game clock, in whole ms, has moved 1000 / com_maxfps (integer division), so the
// only caps it can keep are 1000 / n: 1000, 500, 333, 250, 200... Every cap from 501 to 1000 runs at 1000 fps, from 334 to
// 500 at 500. With com_maxfpsPrecise the clock read in that wait first waits on the performance counter until exactly
// 1 / com_maxfps s has passed since the last frame started, so 480 or 610 hold. Every frame still moves the game clock
// by whole ms (1 or 2 ms at 610 fps), so nothing else changes. Clients only; dedicated servers keep their own timing.
dvar_t* com_maxfpsPrecise = NULL;
static dvar_t* timer_com_maxfps = NULL;
static LONGLONG timer_qpcFrequency = 0;
static LONGLONG timer_nextFrame = 0;

#define com_frameLastTime   (*((int*)0x00c28b30))     // the game clock when the last frame started (Com_Frame)

/** The cap com_maxfpsPrecise keeps now, 0 when the game's own wait applies (off, no cap, dedicated server). */
static int Timer_PreciseCap() {
    if (!timer_com_maxfps)
        timer_com_maxfps = Dvar_GetDvarByName("com_maxfps");    // registered in Com_Init, before the first frame
    int maxfps = timer_com_maxfps ? timer_com_maxfps->value.integer : 0;
    if (!com_maxfpsPrecise || !com_maxfpsPrecise->value.boolean || timer_qpcFrequency <= 0 || maxfps <= 0 || dedicated->value.integer != 0)
        return 0;
    return maxfps;
}

/** Waits on the performance counter until the next frame is due (1 / maxfps s after the last one started). */
static void Timer_WaitForFrame(int maxfps) {
    LONGLONG period = timer_qpcFrequency / maxfps;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // first frame, or far off (a long frame, a cap change): start counting from now
    if (timer_nextFrame == 0 || now.QuadPart - timer_nextFrame > period * 4 || timer_nextFrame - now.QuadPart > period * 4)
        timer_nextFrame = now.QuadPart;
    while (now.QuadPart < timer_nextFrame) {
        if (timer_nextFrame - now.QuadPart > timer_qpcFrequency / 2000)
            Sleep(0);
        else
            YieldProcessor();
        QueryPerformanceCounter(&now);
    }
}

/** With developer 1: prints the frames of every second, to check the cap. */
static void Timer_CountFrame(int ms, int cap, bool precise) {
    static int frames = 0;
    static int second = 0;
    frames++;
    if (ms / 1000 != second) {
        if (second != 0)
            Com_DPrintf("Frame rate: %d frames in the last second (com_maxfps %d, com_maxfpsPrecise %d)\n", frames, cap, precise ? 1 : 0);
        second = ms / 1000;
        frames = 0;
    }
}

// 00432cc0    int32_t Sys_Milliseconds()
// Hooked only in Com_Frame's wait for the next frame (00434ff0), which starts the frame once the clock moved 1000 / com_maxfps ms.
static int Sys_Milliseconds() {
    // CoD2x: exact frame caps, wait for the frame on the performance counter first
    int maxfps = Timer_PreciseCap();
    if (maxfps > 0)
        Timer_WaitForFrame(maxfps);
    else
        timer_nextFrame = 0;
    // CoD2x: End

    // Call the original function
    int ms;
    ASM_CALL(RETURN(ms), 0x00432cc0);

    // CoD2x: the game starts the frame once its clock moved as far as it asks (at least 1 ms); the next one is due a period later
    if (dedicated->value.integer == 0) {
        int cap = timer_com_maxfps ? timer_com_maxfps->value.integer : 0;
        int minMsec = cap > 0 ? 1000 / cap : 1;
        if (minMsec < 1)
            minMsec = 1;
        if (ms - com_frameLastTime >= minMsec) {
            if (maxfps > 0)
                timer_nextFrame += timer_qpcFrequency / maxfps;
            Timer_CountFrame(ms, cap, maxfps > 0);
        }
    }
    // CoD2x: End

    return ms;
}

/** Called only once on game start before common initialization. Used to initialize variables, cvars, etc. */
void timer_init() {
    com_timerHighResolution = Dvar_RegisterBool("com_timerHighResolution", true, (dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));
    com_maxfpsPrecise = Dvar_RegisterBool("com_maxfpsPrecise", true, (dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));
    timer_NtSetTimerResolution = (NtSetTimerResolution_t)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetTimerResolution");
    LARGE_INTEGER f;
    if (QueryPerformanceFrequency(&f))
        timer_qpcFrequency = f.QuadPart;
}

/** Patches the clock read in Com_Frame's wait. Only when the call still holds its original bytes. */
void timer_patch() {
    std::vector<tickrate::Patch> plan;
    tickrate::redirect(plan, 0x00434ff0, "frame wait clock", {0xe8,0xcb,0xdc,0xff,0xff}, (const void*)&Sys_Milliseconds);   // in Com_Frame
    if (tickrate::verifyPatches(plan))
        tickrate::applyPatches(plan);
}
