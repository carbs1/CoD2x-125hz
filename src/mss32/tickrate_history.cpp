#include "tickrate_history.h"
#include "shared.h"
#include "../shared/tickrate_patch.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_common.h"
#include "../shared/cod2_client.h"
#include <algorithm>
#include <new>
#include <vector>

namespace {
constexpr int capacity = tickrate::maxDeltaSlots;
constexpr int parseCapacity = 2048 * capacity / tickrate::nativeDeltaSlots;
constexpr int nativeSnapshotBytes = 9944;
bool installed = false;
std::vector<uint32_t> snapshots;
std::vector<uint32_t> entities;
std::vector<uint32_t> clients;
std::vector<uint32_t> packets;

// Fixed private capacities cover the tested 20..125 Hz range. Advertise them
// only after every parser bound and relocated operand has been installed.
enum class HistoryOperand { Snapshots,Entities,Clients,Packets,DeltaMask,ParseMask,ParseLimit,ParseSafeLimit };
struct HistorySite {
    unsigned int address;
    std::vector<unsigned char> original;
    unsigned int offset;
    unsigned int width;
    HistoryOperand kind;
    unsigned int addend;
};
const HistorySite sites[] = {
#include "tickrate_history_sites.inc"
};

uintptr_t valueFor(HistoryOperand kind) {
    switch (kind) {
        case HistoryOperand::Snapshots: return reinterpret_cast<uintptr_t>(snapshots.data());
        case HistoryOperand::Entities: return reinterpret_cast<uintptr_t>(entities.data());
        case HistoryOperand::Clients: return reinterpret_cast<uintptr_t>(clients.data());
        case HistoryOperand::Packets: return reinterpret_cast<uintptr_t>(packets.data());
        case HistoryOperand::DeltaMask: return capacity-1;
        case HistoryOperand::ParseMask: return parseCapacity-1;
        case HistoryOperand::ParseLimit: return parseCapacity;
        case HistoryOperand::ParseSafeLimit: return parseCapacity-128;
    }
    return 0;
}

// CL_ClearState tail-calls the native string cleanup after its own REP STOSD.
// Clear relocated storage at exactly the same lifecycle boundary.
int clearHistory() {
    std::fill(snapshots.begin(),snapshots.end(),0);
    std::fill(entities.begin(),entities.end(),0);
    std::fill(clients.begin(),clients.end(),0);
    std::fill(packets.begin(),packets.end(),0);
    return reinterpret_cast<int (__cdecl*)()>(0x004357b0)();
}

bool install() {
    if (installed) return true;
    try {
        snapshots.resize(capacity * nativeSnapshotBytes / 4);
        entities.resize(parseCapacity * 240 / 4);
        clients.resize(parseCapacity * 92 / 4);
        packets.resize(capacity * 12 / 4);
        std::vector<tickrate::Patch> plan;
        for (const auto& site : sites)
            tickrate::operand(plan,site.address,"client history operand",site.original,site.offset,site.width,
                valueFor(site.kind) + site.addend);
        // Compare against capacity-1 and adjust the branch to retain the exact
        // capacity boundary without sign-extending an imm8 value of 128.
        plan.push_back({0x00401322,"snapshot retrieval age",{0x83,0xf8,0x20,0x7d,0x3a},
            {0x83,0xf8,capacity-1,0x7f,0x3a}});
        plan.push_back({0x004138e7,"snapshot invalidation age",{0x83,0xff,0x20,0x7c,3},
            {0x83,0xff,capacity-1,0x7e,3}});
        plan.push_back({0x004138ec,"snapshot invalidation start",{0x8d,0x41,0xe1},
            {0x8d,0x41,static_cast<unsigned char>(-(capacity-1))}});
        plan.push_back({0x00413956,"packet timestamp search",{0x83,0xfa,0x20,0x7c,0xe8},
            {0x83,0xfa,capacity-1,0x7e,0xe8}});
        tickrate::redirect(plan,0x0040cd02,"client history reset",{0xe9,0xa9,0x8a,2,0},reinterpret_cast<void*>(clearHistory),0xe9);
        if (!tickrate::verifyPatches(plan)) return false;
        tickrate::applyPatches(plan);
        installed = true;
        Com_Printf("TICKRATE_CLIENT_HISTORY slots=%d parse_slots=%d sites=%u\n",capacity,parseCapacity,
            static_cast<unsigned int>(plan.size()));
        return true;
    } catch (const std::bad_alloc&) {
        Com_Printf("TICKRATE_CLIENT_HISTORY allocation failed; retaining native %d-slot capability\n",
            tickrate::nativeDeltaSlots);
        return false;
    }
}
} // namespace

void tickrate_history_init() {
    if (!dedicated || !dedicated->value.integer) install();
    const int advertised = installed ? capacity : tickrate::nativeDeltaSlots;
    auto capability = Dvar_RegisterInt("cl_deltaSlots",advertised,tickrate::nativeDeltaSlots,capacity,
        static_cast<dvarFlags_e>(DVAR_ROM | DVAR_USERINFO));
    Dvar_SetInt(capability,advertised);
}
