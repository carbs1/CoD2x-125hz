#include "tickrate_patch.h"
#include "tickrate_x86.h"
#include "shared.h"
#include "cod2_server.h"
#include "cod2_dvars.h"
#include "cod2_common.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>
#include <cstdlib>
#include <utility>

namespace tickrate {
namespace {
// Relocated per-client snapshot history, fixed for the process lifetime.
constexpr int slots = maxDeltaSlots;
// Native shifts that size per-client storage in multiples of 32 slots.
constexpr unsigned char slotShift = slots == 128 ? 2 : slots == 64 ? 1 : 0;
static_assert(slots == nativeDeltaSlots << slotShift, "delta slot capacity");

std::vector<clientSnapshot_t> history;
struct ClientEpoch { bool valid = false; int connected = 0; int challenge = 0; };
ClientEpoch epochs[MAX_CLIENTS];
struct DeltaStats { unsigned int full = 0; unsigned int delta = 0; unsigned int compatibility = 0; int lastAge = 0; int maxAge = 0; };
DeltaStats statistics[MAX_CLIENTS];
bool allocationOkay = true;
__attribute__((force_align_arg_pointer))
uintptr_t turretCountdown(void* turret, int) {
    return *reinterpret_cast<int*>(static_cast<char*>(turret) + 8) - frameMsec;
}
constexpr unsigned int nativeFrames = offsetof(client_t, frames);
static_assert(sizeof(clientSnapshot_t) == 9924 && nativeFrames == 134708, "native snapshot ABI");

// Slot of a client inside svs_clients, or -1 for any other client_t.
int clientIndex(const client_t* client) {
    const auto base = reinterpret_cast<uintptr_t>(&svs_clients[0]);
    const auto address = reinterpret_cast<uintptr_t>(client);
    if (address < base || (address - base) % sizeof(client_t)) return -1;
    const auto index = (address - base) / sizeof(client_t);
    return index < MAX_CLIENTS ? static_cast<int>(index) : -1;
}

clientSnapshot_t* frame(client_t* client, int sequence) {
    const int index = clientIndex(client);
    if (index < 0) return &client->frames[sequence & (nativeDeltaSlots - 1)];
    // Allocate only for a process that actually hosts a server. CL-only
    // processes install the hooks too, but do not need an 80 MiB server arena.
    if (history.empty()) {
        try { history.resize(MAX_CLIENTS * slots); }
        catch (const std::bad_alloc&) {
            Com_Error(ERR_FATAL, "Unable to allocate expanded snapshot history");
            std::abort();
        }
    }
    auto& epoch = epochs[index];
    if (!epoch.valid || epoch.connected != client->lastConnectTime || epoch.challenge != client->challenge) {
        std::fill_n(history.begin() + index * slots, slots, clientSnapshot_t{});
        epoch = {true, client->lastConnectTime, client->challenge};
        statistics[index] = {};
    }
    return &history[index * slots + (static_cast<unsigned int>(sequence) & (slots - 1))];
}

// Return a biased offset: existing native LEAs add the client address and field
// offset afterwards. The original client_t layout and stride remain intact.
__attribute__((force_align_arg_pointer))
uintptr_t frameOffset(client_t* client, int sequence) {
    return reinterpret_cast<uintptr_t>(frame(client, sequence)) -
        reinterpret_cast<uintptr_t>(client) - nativeFrames;
}
__attribute__((force_align_arg_pointer))
uintptr_t profileOrigin(client_t* client, int) {
    return reinterpret_cast<uintptr_t>(frame(client, 0)) + 2 * sizeof(clientSnapshot_t) - 12;
}

void compatibleDelta(client_t* client) {
    if (client->deltaMessage <= 0) return; // the native path already sends a full snapshot
    const int supported = clientDeltaSlots(client->userinfo);
    const auto age = static_cast<uint32_t>(client->netchan.outgoingSequence) -
        static_cast<uint32_t>(client->deltaMessage);
    if (age < static_cast<unsigned int>(supported - deltaAgeMargin)) return;
    const int index = clientIndex(client);
    if (index >= 0) ++statistics[index].compatibility;
    client->deltaMessage = -1; // stock full-snapshot fallback for older peers
}

void recordDelta(client_t* client, msg_t* message, int start) {
    const int index = clientIndex(client);
    if (index < 0 || message->overflowed || start < 0 ||
        message->cursize < start + 6 || message->data[start] != 6) return;
    auto& stats = statistics[index];
    stats.lastAge = message->data[start+5];
    stats.maxAge = std::max(stats.maxAge,stats.lastAge);
    if (stats.lastAge) ++stats.delta;
    else ++stats.full;
}

#if COD2X_WIN32
int writeSnapshot() {
    void* message;
    client_t* client;
    ASM(movr, message, "eax");
    ASM(movr, client, "ecx");
    compatibleDelta(client);
    const int start = static_cast<msg_t*>(message)->cursize;
    int result;
    ASM_CALL(RETURN(result), 0x0045d7c0, 0, EAX(message), ECX(client));
    recordDelta(client,static_cast<msg_t*>(message),start);
    return result;
}
#else
int writeSnapshot(client_t* client, void* message) {
    compatibleDelta(client);
    const int start = static_cast<msg_t*>(message)->cursize;
    const int result = reinterpret_cast<int (__cdecl*)(client_t*, void*)>(0x080985c8)(client, message);
    recordDelta(client,static_cast<msg_t*>(message),start);
    return result;
}
#endif

int calculatePings() {
    for (int i = 0; i < sv_maxclients->value.integer; ++i) {
        auto& client = svs_clients[i];
        int count = 0;
        int total = 0;
        if (client.state == CS_ACTIVE && client.gentity) {
            // One lookup settles the epoch; the client's slots are contiguous after it.
            const clientSnapshot_t* samples = frame(&client, 0);
            for (int sequence = 0; sequence < slots; ++sequence) {
                const auto& sample = samples[sequence];
                if (sample.messageAcked > 0) {
                    total += sample.messageAcked - sample.messageSent;
                    ++count;
                }
            }
        }
        client.ping = count ? std::min(999, total / count) : 999;
    }
    return sv_maxclients->value.integer;
}

// Saved PUSHAD order: EDI, ESI, EBP, old ESP, EBX, EDX, ECX, EAX.
// Each audited block replaces only mask/multiply, whose flags are dead before
// the next native comparison. Preserve all registers except its original result.
void* offsetThunk(int sequenceReg, int clientReg, int outputReg,
                  uintptr_t callback = reinterpret_cast<uintptr_t>(frameOffset)) {
    const auto bytes = frameAdapter(sequenceReg, clientReg, outputReg, callback);
    if (bytes.empty()) {
        allocationOkay = false;
        return nullptr;
    }
    void* page = executableCode(bytes);
    if (!page) allocationOkay = false;
    return page;
}

// Replace the last byte of the original instruction, its imm8 operand.
void immediate(std::vector<Patch>& patches, unsigned int address, const char* name,
               std::vector<unsigned char> original, unsigned char value) {
    const size_t last = original.size() - 1;
    operand(patches, address, name, std::move(original), last, 1, value);
}
// Native "and seq, 31; imul seq, sizeof(clientSnapshot_t)" block, optionally without the mask.
void frameSite(std::vector<Patch>& patches, unsigned int address, int sequenceReg, int clientReg,
               bool masked = true) {
    std::vector<unsigned char> original;
    if (masked) original = {0x83,static_cast<unsigned char>(0xe0 + sequenceReg),0x1f};
    original.insert(original.end(), {0x69,static_cast<unsigned char>(0xc0 + sequenceReg * 9),0xc4,0x26,0,0});
    redirect(patches, address, "server delta frame storage", std::move(original),
        offsetThunk(sequenceReg,clientReg,sequenceReg));
}
} // namespace

bool planTurretCountdown(std::vector<Patch>& patches) {
    // Replace both the load and decrement. A signed imm8 cannot represent all
    // configured frame durations; the adapter preserves every other register.
#if COD2X_WIN32
    redirect(patches,0x0051c89c,"mounted turret countdown",
        {0x8b,0x43,8,0x83,0xc0,0xce},offsetThunk(-1,3,0,reinterpret_cast<uintptr_t>(turretCountdown)));
#else
    redirect(patches,0x0810b8c2,"mounted turret countdown",
        {0x8b,0x40,8,0x83,0xe8,0x32},offsetThunk(-1,0,0,reinterpret_cast<uintptr_t>(turretCountdown)));
#endif
    return allocationOkay;
}

bool planDelta(std::vector<Patch>& patches) {
#if COD2X_WIN32
    for (auto site : {0x0045d7d2u,0x0045d80fu,0x0045f3d3u,0x0045f3fdu}) frameSite(patches,site,0,6);
    frameSite(patches,0x0045624c,2,5);
    frameSite(patches,0x0045eec7,1,0);
    frameSite(patches,0x0045f3eb,1,6);
    for (auto site : {0x0045d1a7u,0x0045d1c5u,0x0045d1e2u,0x0045d200u}) frameSite(patches,site,5,3);
    redirect(patches,0x0045d14f,"profile delta origin",{0x8d,0x83,0xb0,0x5b,2,0},
        offsetThunk(-1,3,0,reinterpret_cast<uintptr_t>(profileOrigin)));
    immediate(patches,0x0045d181,"profile delta slot limit",{0x83,0xfa,0x20},slots-1); // increments by four
    operand(patches,0x0045d19c,"profile delta loop",{0xc7,0x44,0x24,0x20,8,0,0,0},4,4,slots/4);
    immediate(patches,0x0045d7f6,"delta age limit",{0x83,0xf9,0x1d},slots-deltaAgeMargin);
    redirect(patches,0x0045f5ca,"peer delta capability",{0xe8,0xf1,0xe1,0xff,0xff},reinterpret_cast<void*>(writeSnapshot));
    redirect(patches,0x0045be70,"delta ping history",{0x83,0xec,8,0xa1,0x10,0x28,0xd5,0},reinterpret_cast<void*>(calculatePings),0xe9);
    immediate(patches,0x004582ae,"snapshot storage size shift",{0xc1,0xe2,0x0b},11+slotShift);
    immediate(patches,0x00458538,"snapshot storage size shift",{0xc1,0xe1,0x0b},11+slotShift);
    immediate(patches,0x004582bf,"snapshot slot count shift",{0xc1,0xe1,5},5+slotShift);
    immediate(patches,0x00458549,"snapshot slot count shift",{0xc1,0xe2,5},5+slotShift);
#else
    frameSite(patches,0x0809102d,0,2);
    for (auto site : {0x080985d7u,0x0809865eu,0x0809a41du}) {
        // These three functions receive client as their first stack argument.
        frameSite(patches,site,0,8);
    }
    for (auto site : {0x0809ac3fu,0x0809ac61u,0x0809ac85u}) frameSite(patches,site,0,2);
    for (auto site : {0x0809801fu,0x08098039u}) frameSite(patches,site,0,2,false);
    frameSite(patches,0x0809807f,0,2);
    immediate(patches,0x08098011,"delta mask",{0x83,0x7d,0xb0,0x1f},slots-1);
    immediate(patches,0x0809806e,"delta mask",{0x83,0x7d,0xac,0x1f},slots-1);
    immediate(patches,0x08098628,"delta age limit",{0x83,0xf8,0x1c},slots-deltaAgeMargin-1); // native jle instead of jl
    redirect(patches,0x0809ae92,"peer delta capability",{0xe8,0x31,0xd7,0xff,0xff},reinterpret_cast<void*>(writeSnapshot));
    redirect(patches,0x080963c8,"delta ping history",{0x55,0x89,0xe5,0x53,0x83,0xec,0x24},reinterpret_cast<void*>(calculatePings),0xe9);
    for (auto site : {0x08092e74u,0x08093151u}) immediate(patches,site,"snapshot storage size shift",{0xc1,0xe0,0x0b},11+slotShift);
    for (auto site : {0x08092e8eu,0x0809316bu}) immediate(patches,site,"snapshot slot count shift",{0xc1,0xe0,5},5+slotShift);
#endif
    return allocationOkay;
}

int clientDeltaSlots(const char* userinfo) {
    const int supported = peerDeltaSlots(atoi(Info_ValueForKey(userinfo,"cl_deltaSlots")));
    return std::min(supported,slots);
}
void printDeltaStats(int client) {
    if (client < 0 || client >= MAX_CLIENTS) return;
    const auto& stats = statistics[client];
    Com_Printf("TICKRATE_DELTA slot=%d history_slots=%d sent_delta=%u sent_full=%u compat_full=%u last_age_frames=%d max_age_frames=%d\n",
        client,clientDeltaSlots(svs_clients[client].userinfo),stats.delta,stats.full,stats.compatibility,
        stats.lastAge,stats.maxAge);
}

void releaseDelta() {
    std::vector<clientSnapshot_t>().swap(history);
    releaseExecutableCode();
}
} // namespace tickrate
