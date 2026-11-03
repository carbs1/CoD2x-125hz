#ifndef COD2X_TICKRATE_WAIT_H
#define COD2X_TICKRATE_WAIT_H
#include "tickrate_patch.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cctype>

namespace tickrate {
constexpr uint32_t scriptTimeMask = 0xffffff;
struct WaitPhase {
    double remainder = 0;
    uint32_t wake = 0;
    uint32_t generation = 0;
};

// Native thread status retains its previous wake key when a timed wait resumes.
// New/reused thread IDs start with key zero; events or late resumes break the
// chain. Carry only a continuous timed-wait chain, never another thread's phase.
inline uint32_t preciseWait(WaitPhase& phase, double seconds, int step,
                           uint32_t now, uint32_t priorWake, uint32_t generation) {
    if (!std::isfinite(seconds) || seconds < 0 || step < 1) return scriptTimeMask;
    if (seconds == 0) return 0;
    double carry = phase.generation == generation && phase.wake != 0 &&
        phase.wake == priorWake && phase.wake == now ? phase.remainder : 0;
    const double exact = seconds * 1000.0 / step + carry;
    if (exact >= scriptTimeMask - 0.5) return scriptTimeMask;
    uint32_t ticks = static_cast<uint32_t>(std::floor(exact + 0.5));
    phase.remainder = ticks ? exact - ticks : 0;
    if (!ticks) ticks = 1;
    phase.wake = (now + ticks) & scriptTimeMask;
    phase.generation = generation;
    return ticks;
}

inline bool realtimeWaitMode(int mode, bool modCompensation) {
    return mode == 1 || (mode == 2 && !modCompensation);
}
inline bool modCompensatesWaits(const char* source) {
    if (!source) return true; // unavailable source: preserve the legacy clock
    int part = 0;
    while (*source) {
        if (std::isspace(static_cast<unsigned char>(*source))) { ++source; continue; }
        if (source[0] == '/' && source[1] == '/') {
            while (*source && *source != '\n') ++source;
            continue;
        }
        if (source[0] == '/' && source[1] == '*') {
            source += 2;
            while (*source && !(source[0] == '*' && source[1] == '/')) ++source;
            if (*source) source += 2;
            continue;
        }
        if (*source == '"') {
            ++source;
            while (*source && *source != '"') {
                if (*source == '\\' && source[1]) ++source;
                ++source;
            }
            if (*source) ++source;
            part = 0;
            continue;
        }
        if (std::isalnum(static_cast<unsigned char>(*source)) || *source == '_') {
            const char* expected = part == 2 ? "fps_multiplier" : "level";
            bool equal = true;
            while (std::isalnum(static_cast<unsigned char>(*source)) || *source == '_') {
                if (!*expected || std::tolower(static_cast<unsigned char>(*source)) != *expected) equal = false;
                if (*expected) ++expected;
                ++source;
            }
            if (equal && !*expected) {
                if (part == 2) return true;
                part = 1;
            } else part = 0;
        } else {
            part = *source++ == '.' && part == 1 ? 2 : 0;
        }
    }
    return false;
}
bool planScriptWaits(std::vector<Patch>& patches);
void configureScriptWaits(int step, bool realtime);
void resetScriptWaits();
}
#endif
