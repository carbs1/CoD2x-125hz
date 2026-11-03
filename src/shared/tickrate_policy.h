#ifndef COD2X_TICKRATE_POLICY_H
#define COD2X_TICKRATE_POLICY_H
namespace tickrate {
constexpr int maxDeltaSlots = 128;
inline int snapshotInterval(int requested, bool provided, int minimum) {
    int snaps = provided ? requested : 20;
    if (snaps < 1) snaps = 1;
    if (snaps > 125) snaps = 125;
    if (minimum > 125) minimum = 125;
    if (snaps < minimum) snaps = minimum;
    return 1000 / snaps;
}
inline int peerDeltaSlots(int advertised) {
    return advertised == 64 || advertised == maxDeltaSlots ? advertised : 32;
}
}
#endif
