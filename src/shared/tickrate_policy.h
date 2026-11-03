#ifndef COD2X_TICKRATE_POLICY_H
#define COD2X_TICKRATE_POLICY_H
namespace tickrate {
// The native client_t and clientActive_t keep 32 snapshot slots.
constexpr int nativeDeltaSlots = 32;
constexpr int maxDeltaSlots = 128;
// A delta is refused this many frames before the peer's history would wrap.
constexpr int deltaAgeMargin = 3;
// Archived server snapshots: 60 nominal seconds at the highest tested rate,
// plus a power-of-two payload ring whose masks and shifts use archiveShift.
constexpr int archiveFrames = 125 * 60;
constexpr int archiveShift = 28;
constexpr int archiveBytes = 1 << archiveShift;
static_assert(archiveBytes == 256 * 1024 * 1024, "archive payload ring size");

inline int snapshotInterval(int requested, bool provided, int minimum) {
    int snaps = provided ? requested : 20;
    if (snaps < 1) snaps = 1;
    if (snaps > 125) snaps = 125;
    if (minimum > 125) minimum = 125;
    if (snaps < minimum) snaps = minimum;
    return 1000 / snaps;
}
inline int peerDeltaSlots(int advertised) {
    return advertised == 64 || advertised == maxDeltaSlots ? advertised : nativeDeltaSlots;
}
}
#endif
