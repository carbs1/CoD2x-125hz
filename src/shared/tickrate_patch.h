#ifndef COD2X_TICKRATE_PATCH_H
#define COD2X_TICKRATE_PATCH_H
#include <cstddef>
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
// Duration of the configured server frame in milliseconds, kept by tickrate_frame().
extern int frameMsec;

// Plan a relative call (0xe8) or jump (0xe9) to target over the original
// instruction bytes, NOP-filling the remainder of the site.
void redirect(std::vector<Patch>& patches, unsigned int address, const char* name,
              std::vector<unsigned char> original, const void* target, unsigned char opcode = 0xe8);
// Plan replacing `width` little-endian bytes at `offset` inside the original instruction.
void operand(std::vector<Patch>& patches, unsigned int address, const char* name,
             std::vector<unsigned char> original, size_t offset, size_t width, uint32_t value);
// Every site must still hold its original bytes and no two sites may overlap.
bool verifyPatches(const std::vector<Patch>& patches);
void applyPatches(const std::vector<Patch>& patches);
// False once applyPatches has pointed native code at this module's storage or code.
bool canReload();

bool planDelta(std::vector<Patch>& patches);
bool planTurretCountdown(std::vector<Patch>& patches);
void planArchive(std::vector<Patch>& patches);
int clientDeltaSlots(const char* userinfo);
void printDeltaStats(int client);
void releaseDelta();
}
#endif
