#ifndef COD2X_TICKRATE_PATCH_H
#define COD2X_TICKRATE_PATCH_H
#include <cstdint>
#include <vector>
#include "tickrate_policy.h"
namespace tickrate {
struct Patch {
    unsigned int address;
    const char* name;
    std::vector<unsigned char> original;
    std::vector<unsigned char> replacement;
};
bool verifyPatches(const std::vector<Patch>& patches);
void applyPatches(const std::vector<Patch>& patches);
bool planDelta(std::vector<Patch>& patches, int capacity);
bool planTurretCountdown(std::vector<Patch>& patches, const int* frameMsec);
void planArchive(std::vector<Patch>& patches);
int clientDeltaSlots(const char* userinfo);
void printDeltaStats(int client);
void resetDelta();
void releaseDelta();
}
#endif
