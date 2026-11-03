#include "tickrate_history.h"
#include "shared.h"
#include "../shared/tickrate_patch.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_common.h"
#include "../shared/cod2_client.h"
#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

namespace {
constexpr int capacity = tickrate::maxDeltaSlots;
constexpr int parseCapacity = 2048 * capacity / 32;
constexpr int nativeSnapshotBytes = 9944;
bool installed = false;
unsigned int received = 0;
int receivedSequence = 0;
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
    received = 0;
    receivedSequence = 0;
    return reinterpret_cast<int (__cdecl*)()>(0x004357b0)();
}

// Original CL_ParseSnapshot(msg @ EAX, flags @ EDI), called from one native site.
// Count accepted snapshots at the parser, independently of client render FPS.
int parseSnapshot() {
    void* message;
    int flags;
    ASM(movr,message,"eax");
    ASM(movr,flags,"edi");
    int result;
    ASM_CALL(RETURN(result),0x004136c0,0,EAX(message),EDI(flags));
    const int sequence = *reinterpret_cast<int*>(0x0096b674);
    if (*reinterpret_cast<int*>(0x0096b668) && sequence != receivedSequence) {
        ++received;
        receivedSequence = sequence;
    }
    return result;
}

void redirect(std::vector<tickrate::Patch>& plan, unsigned int address, const char* name,
              std::initializer_list<unsigned char> original, void* destination, unsigned char opcode) {
    tickrate::Patch patch{address,name,original,std::vector<unsigned char>(original.size(),0x90)};
    patch.replacement[0] = opcode;
    const int32_t relative = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(destination)) - (address+5);
    memcpy(patch.replacement.data()+1,&relative,4);
    plan.push_back(patch);
}

bool install() {
    if (installed) return true;
    try {
        snapshots.resize(capacity * nativeSnapshotBytes / 4);
        entities.resize(parseCapacity * 240 / 4);
        clients.resize(parseCapacity * 92 / 4);
        packets.resize(capacity * 12 / 4);
        std::vector<tickrate::Patch> plan;
        for (const auto& site : sites) {
            tickrate::Patch patch{site.address,"client history operand",site.original,site.original};
            if (site.offset + site.width > patch.replacement.size()) return false;
            const uint32_t value = valueFor(site.kind) + site.addend;
            for (unsigned int i = 0; i < site.width; ++i)
                patch.replacement[site.offset+i] = static_cast<unsigned char>(value >> (i*8));
            plan.push_back(patch);
        }
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
        redirect(plan,0x0040cd02,"client history reset",{0xe9,0xa9,0x8a,2,0},reinterpret_cast<void*>(clearHistory),0xe9);
        redirect(plan,0x0041452c,"snapshot receive counter",{0xe8,0x8f,0xf1,0xff,0xff},reinterpret_cast<void*>(parseSnapshot),0xe8);
        if (!tickrate::verifyPatches(plan)) return false;
        tickrate::applyPatches(plan);
        installed = true;
        Com_Printf("TICKRATE_CLIENT_HISTORY slots=%d parse_slots=%d sites=%u\n",capacity,parseCapacity,
            static_cast<unsigned int>(plan.size()));
        return true;
    } catch (const std::bad_alloc&) {
        Com_Printf("TICKRATE_CLIENT_HISTORY allocation failed; retaining native 32-slot capability\n");
        return false;
    }
}
} // namespace

void tickrate_history_init() {
    if (!dedicated || !dedicated->value.integer) install();
    auto capability = Dvar_RegisterInt("cl_deltaSlots",installed ? capacity : 32,32,capacity,
        static_cast<dvarFlags_e>(DVAR_ROM | DVAR_USERINFO));
    Dvar_SetInt(capability,installed ? capacity : 32);
}
bool tickrate_history_can_reload() { return !installed; }
int tickrate_history_capacity() { return installed ? capacity : 32; }
unsigned int tickrate_history_snapshot_count() { return received; }
int tickrate_history_snapshot_time() { return installed ? *reinterpret_cast<int*>(0x0096b670) : 0; }
