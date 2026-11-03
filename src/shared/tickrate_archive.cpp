#include "tickrate_patch.h"
#include "tickrate_x86.h"
#include "shared.h"
#include <cstdint>
#include <utility>

// Fixed process-lifetime archive capacities (tickrate_policy.h). Native payload
// arenas are allocated only when hosting a server.
namespace {
using tickrate::Patch;
using tickrate::archiveBytes;
using tickrate::archiveFrames;
using tickrate::archiveShift;

// Replace the 32-bit immediate at `offset` of the original instruction.
void replace32(std::vector<Patch>& patches, unsigned int address, const char* name,
               std::vector<unsigned char> original, size_t offset, int32_t value) {
    tickrate::operand(patches, address, name, std::move(original), offset, 4, static_cast<uint32_t>(value));
}

void planArchiveBytes(std::vector<Patch>& patches) {
#if COD2X_WIN32
    for (auto address : {0x00458858u,0x00458871u})
        replace32(patches,address,"archive byte allocation/zeroing",{0x68,0,0,0,2},1,archiveBytes);
    replace32(patches,0x0045c70c,"archive byte overflow",{0x81,0x3d,0x38,0x57,0xd3,0,0xfe,0xff,0xff,0x7d},6,INT32_MAX-archiveBytes-1);
    replace32(patches,0x0045e0f8,"archive byte lower bound",{0x81,0xc1,0,0,0,0xfe},2,-archiveBytes);
    replace32(patches,0x0045e34d,"archive delta byte lower bound",{0x05,0,0,0,0xfe},1,-archiveBytes);
    const int32_t mask = static_cast<uint32_t>(INT32_MIN) | (archiveBytes-1);
    replace32(patches,0x0045e175,"archive read modulus",{0x81,0xe6,0xff,0xff,0xff,0x81},2,mask);
    replace32(patches,0x0045e1ac,"archive signed read modulus",{0x81,0xce,0,0,0,0xfe},2,-archiveBytes);
    replace32(patches,0x0046017b,"archive write modulus",{0x81,0xe7,0xff,0xff,0xff,0x81},2,mask);
    replace32(patches,0x00460184,"archive signed write modulus",{0x81,0xcf,0,0,0,0xfe},2,-archiveBytes);
    for (auto address : {0x0045e1b9u,0x004601aeu})
        replace32(patches,address,"archive wrap copy size",{0xb8,0,0,0,2},1,archiveBytes);
#else
    replace32(patches,0x08093369,"archive byte allocation",{0xc7,4,0x24,0,0,0,2},3,archiveBytes);
    replace32(patches,0x08096c0a,"archive byte overflow",{0x81,0x3d,0xb8,0x30,0x42,8,0xfd,0xff,0xff,0x7d},6,INT32_MAX-archiveBytes-2);
    for (auto address : {0x080990d9u,0x08099323u})
        replace32(patches,address,"archive byte lower bound",{0x2d,0,0,0,2},1,archiveBytes);
    replace32(patches,0x0809920f,"archive signed read modulus",{0x81,0x85,0x70,0xff,0xff,0xff,0xff,0xff,0xff,1},6,archiveBytes-1);
    replace32(patches,0x0809bb6c,"archive signed write modulus",{0x81,0x85,0x4c,0xd7,0xff,0xff,0xff,0xff,0xff,1},6,archiveBytes-1);
    for (auto address : {0x0809921fu,0x0809bb7cu})
        patches.push_back({address,"archive quotient shift",{0xc1,0xf8,0x19},{0xc1,0xf8,archiveShift}});
    for (auto address : {0x08099222u,0x0809bb7fu})
        patches.push_back({address,"archive modulus product shift",{0xc1,0xe0,0x19},{0xc1,0xe0,archiveShift}});
    for (auto address : {0x08099232u,0x0809bbbeu})
        replace32(patches,address,"archive wrap copy size",{0xb8,0,0,0,2},1,archiveBytes);
#endif
}
}

namespace tickrate {
void planArchive(std::vector<Patch>& patches) {
#if COD2X_WIN32
    replace32(patches, 0x0045882f, "archive allocation", {0x68,0x80,0x25,0,0}, 1, archiveFrames * 8);
    replace32(patches, 0x00458848, "archive zeroing", {0x68,0x80,0x25,0,0}, 1, archiveFrames * 8);
    replace32(patches, 0x0045e0e6, "archive decoder modulus", {0xb9,0xb0,4,0,0}, 1, archiveFrames);
    replace32(patches, 0x0045e32f, "archive decoder lower bound", {0x05,0x50,0xfb,0xff,0xff}, 1, -archiveFrames);
    replace32(patches, 0x0045e33b, "archive delta modulus", {0xbe,0xb0,4,0,0}, 1, archiveFrames);
    replace32(patches, 0x0045ea76, "archive lookup lower bound", {0x8d,0x88,0x50,0xfb,0xff,0xff}, 2, -archiveFrames);
    replace32(patches, 0x00460158, "archive writer modulus", {0xb9,0xb0,4,0,0}, 1, archiveFrames);
    replace32(patches, 0x0045c6cf, "archive counter overflow", {0x81,0x3d,0x2c,0x57,0xd3,0,0x4e,0xfb,0xff,0x7f},
        6, INT32_MAX - archiveFrames - 1);
#else
    replace32(patches, 0x08093358, "archive allocation", {0xc7,0x04,0x24,0x80,0x25,0,0}, 3, archiveFrames * 8);
    // Replace the complete compiler-specific signed remainder sequence with
    // idiv by the configured frame count, preserving ECX=remainder, EDX=quotient,
    // EAX=quotient*divisor and EBX. No hard-coded /6000 reciprocal remains.
    for (auto address : {0x080990a8u, 0x080992f2u, 0x0809bb04u}) {
        patches.push_back({address, "archive configured modulus",
            {0xb8,0xb5,0x81,0x4e,0x1b,0xf7,0xe9,0xc1,0xfa,0x07,0x89,0xc8,0xc1,0xf8,0x1f,
             0x29,0xc2,0x89,0xd0,0x69,0xc0,0xb0,0x04,0,0,0x29,0xc1},
            archiveRemainder(archiveFrames)});
    }
    for (auto address : {0x080992ceu, 0x08099c69u, 0x08099c78u})
        replace32(patches, address, "archive lower bound", {0x2d,0xb0,0x04,0,0}, 1, archiveFrames);
    replace32(patches, 0x08096bb3, "archive counter overflow", {0x81,0x3d,0xac,0x30,0x42,0x08,0x4d,0xfb,0xff,0x7f},
        6, INT32_MAX - archiveFrames - 2);
#endif
    planArchiveBytes(patches);
}
}
