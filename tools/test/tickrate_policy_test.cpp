#include "tickrate_policy.h"
#include "tickrate_x86.h"
#include <cstdio>
#include <cstdlib>

static void check(bool okay) {
    if (!okay) { std::fputs("tickrate policy assertion failed\n",stderr); std::exit(1); }
}
static void hex(const std::vector<unsigned char>& value) {
    for (auto byte : value) std::printf("%02x",byte);
}
int main() {
    using tickrate::snapshotInterval;
    check(snapshotInterval(0,false,0)==50);
    check(snapshotInterval(0,false,125)==8);
    check(snapshotInterval(20,true,125)==8);
    check(snapshotInterval(125,true,0)==8);
    check(snapshotInterval(500,true,0)==8);
    check(snapshotInterval(0,true,0)==1000);
    check(snapshotInterval(-1,true,0)==1000);
    check(snapshotInterval(40,true,20)==25);
    check(snapshotInterval(30,true,0)==33);
    check(tickrate::peerDeltaSlots(0)==32);
    check(tickrate::peerDeltaSlots(32)==32);
    check(tickrate::peerDeltaSlots(64)==64);
    check(tickrate::peerDeltaSlots(128)==128);
    check(tickrate::peerDeltaSlots(256)==32);
    check(tickrate::peerDeltaSlots(-1)==32);
    check(tickrate::frameAdapter(4,0,0,0x20000).empty());
    check(tickrate::frameAdapter(0,4,0,0x20000).empty());
    check(tickrate::frameAdapter(0,0,4,0x20000).empty());
    std::printf("{\"accepted_configurations\":18,\"adapters\":[");
    bool first=true;
    for (int sequence=-1;sequence<8;++sequence)
        for (int client=0;client<=8;++client) for (int result=0;result<8;++result) {
            const auto bytes=tickrate::frameAdapter(sequence,client,result,0x20000);
            if (bytes.empty()) continue;
            std::printf("%s[%d,%d,%d,\"",first?"":",",sequence,client,result);
            hex(bytes);std::printf("\"]");first=false;
        }
    std::printf("],\"remainders\":[[7500,\"");hex(tickrate::archiveRemainder(7500));
    std::puts("\"]]}");
    return 0;
}
