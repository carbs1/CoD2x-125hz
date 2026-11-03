#include "tickrate_wait.h"
#include "tickrate_x86.h"
#include "shared.h"

namespace tickrate {
namespace {
WaitPhase phases[65536];
int waitStep = 50;
bool realClock = false;
uint32_t clockGeneration = 1;

uint32_t adjustWait(uint32_t localId, const void* value, uint32_t nativeTicks) {
    if (!realClock) return nativeTicks;
    const auto* words = static_cast<const uint32_t*>(value);
    double seconds;
    if (words[1] == 5) {
        float number; std::memcpy(&number,words,4); seconds = number;
    } else if (words[1] == 6) seconds = static_cast<int32_t>(words[0]);
    else return nativeTicks;

    const auto* variables = reinterpret_cast<const uint32_t*>(ADDR(0x00e08f00,0x08297500));
    const uint32_t now = *reinterpret_cast<const uint32_t*>(ADDR(0x00f08fa0,0x08397518)) & scriptTimeMask;
    // Nested function locals link to the root thread; the archived wait belongs
    // to that root, which remains stable across calls and suspension.
    for (int depth = 0; depth < 32 && localId && localId < 65536; ++depth) {
        const uint32_t status = variables[localId * 4 + 2];
        if ((status & 31) == 18) { localId = status >> 8; continue; }
        if ((status & 31) == 15)
            return preciseWait(phases[localId],seconds,waitStep,now,status >> 8,clockGeneration);
        break;
    }
    // An unfamiliar VM context still gets real-time units, without phase carry.
    WaitPhase isolated;
    return preciseWait(isolated,seconds,waitStep,now,0,clockGeneration);
}

__attribute__((force_align_arg_pointer))
uintptr_t scriptWaitClock(void* frame, uint32_t valueOrEax) {
    auto* bytes = static_cast<unsigned char*>(frame);
#if COD2X_WIN32
    auto& ticks = *reinterpret_cast<uint32_t*>(bytes + 0x50);
    ticks = adjustWait(*reinterpret_cast<uint32_t*>(bytes+0x70),
        reinterpret_cast<const void*>(valueOrEax),ticks);
    return ticks;
#else
    auto& ticks = *reinterpret_cast<uint32_t*>(bytes - 0x30);
    ticks = adjustWait(*reinterpret_cast<uint32_t*>(bytes+0xc),
        *reinterpret_cast<const void**>(bytes+0x14),ticks);
    return valueOrEax;
#endif
}
}

void resetScriptWaits() {
    if (++clockGeneration == 0) {
        for (auto& phase : phases) phase = {};
        clockGeneration = 1;
    }
}
void configureScriptWaits(int step, bool realtime) {
    if (waitStep != step || realClock != realtime) resetScriptWaits();
    waitStep = step; realClock = realtime;
}

bool planScriptWaits(std::vector<Patch>& patches) {
#if COD2X_WIN32
    auto code = frameAdapter(7,5,3,reinterpret_cast<uintptr_t>(scriptWaitClock));
    const unsigned int site = 0x0047e8eb;
    std::vector<unsigned char> compare{0x81,0xfb,0xff,0xff,0xff,0};
#else
    auto code = frameAdapter(0,5,0,reinterpret_cast<uintptr_t>(scriptWaitClock));
    const unsigned int site = 0x08080f70;
    std::vector<unsigned char> compare{0x81,0x7d,0xd0,0xfe,0xff,0xff,0};
#endif
    code.pop_back(); // restore native compare flags after the register adapter
    code.insert(code.end(),compare.begin(),compare.end());code.push_back(0xc3);
    void* target = executableCode(code);
    if (!target) return false;
    redirect(patches,site,"script wait clock",compare,target);
    return true;
}
}
