# 125 Hz timing and capsule implementation notes

The 125 Hz and capsule implementation originated in experimental commit
`6265e92`. The capsule implementation, fitted model data and fitting tool were
already part of that experiment. The corrections below address its timing,
network history and intersection behavior.

## Frame-dependent corrections

| Area | Correction |
| --- | --- |
| Mounted turret fire and sound | Subtract the configured world-frame duration rather than 50 ms per callback. At 125 Hz, a 100 ms countdown expires after 13 callbacks / 104 ms. |
| Idle turret sound | Keep its native decrement because this path runs on a separate 50 ms think schedule. |
| Dropped-item velocity | Use the floating-point reciprocal of actual frame milliseconds; integer `1000/33` loses velocity at 30 Hz. |
| Snapshot minimum | Apply the default, clamp and `sv_minSnaps` floor even when client userinfo omits `snaps`. |
| Debug bot stance | Override crouch/prone input only when an explicit debug stance is selected. |

## Snapshot history and storage

Client and server snapshot history use 128 slots instead of 32. At 125 Hz that
is 1.024 seconds of storage rather than 256 ms. These snapshots supply older
delta bases when packets are delayed; extra storage adds no intentional display
delay and changes no anti-lag policy. Both client and server need the native
patch for the expanded history.

Client entity/client parser pools grow from 2048 to 8192 entries, and packet
timestamps use 128 slots. Readers, writers, masks, age guards, ping/profiling
loops and reset paths are updated together. Reconnect epochs clear the relevant
client block; map/disconnect state clearing also clears relocated client data.

`cl_deltaSlots` advertises installed decoder capacity. A peer with a smaller
history receives full snapshots when a delta would exceed that capacity.
Capability reporting is separate from existing version and admission rules.

The server archive uses 7500 index entries and a 256 MiB payload ring. The index
covers 60 nominal seconds at 125 Hz; heavy payload traffic can expire archived
data sooner. Capacities are fixed for the process lifetime and cover the tested
20..125 Hz range, while frame-dependent clocks follow the active `sv_fps`.

Hosting allocates about 77.5 MiB for the 64-client snapshot arena, plus the
archive payload and other native pools. Client-only processes skip that server
arena. Native signatures and overlapping patch ranges are checked before
installation. Updating a DLL requires restarting the process because native
code retains references to its relocated storage and hooks.

## Timing decisions and configuration

- The native smooth-player trajectory duration stays **50 ms**. It is a
  look-ahead cap; the client evaluates trajectories at snapshot times before
  interpolation. Scaling it to the 8 ms world step changes reconstruction.
- Fuse and smoke lifetimes use elapsed milliseconds. Preserve those deadlines
  rather than replacing every 50 ms literal.
- Grenades retain the experiment's higher-rate integration. Different collision
  sampling can change bounce trajectories; stock throw parity is not established.
- `sv_realtimeWait` defaults off for compatibility with mods such as zPAM that
  compensate script waits themselves. Check a mod's wait compensation before
  enabling this option.
- `cl_interpDelay` retains its configurable 30 ms default. Buffering trades
  responsiveness for stability; it is separate from snapshot storage capacity.
- A mod or ruleset can override `sv_fps`. Check the active value after it loads.
  `sv_minSnaps` and `sv_minRate` can raise older client preferences; they do not
  replace the need for a patched client or a suitable server `sv_maxRate`.

## Capsule intersections and remaining checks

The corrected solver compares both quadratic roots and both rounded ends.
Double intermediates avoid cancellation on long grazing rays. Start-inside hits
respect the incoming wall fraction, and hit priorities and the near-equal-radius
threshold are preserved. Missing mandatory skeleton data keeps the native
fallback; a handled capsule miss stays a miss.

`sv_capsuleHitboxes` defaults to `2` (shapes fitted to each body, head and
headgear model, so gear on a model is not hittable). Models without fitted data
use the shared shapes of `1`; `0` restores the stock hitboxes.

Geometry tests use synthetic standing/crouched/prone poses. Actual animated
model fit, aimed head/torso/limb shots, stance transitions and direction changes
under latency still need gameplay checks with `g_antilag` off and on. Native
anti-lag rewinds origins; this patch adds no historical animation/stance rewind.

Enlarged shapes at scale 2 may exceed the native broad-phase bounds and need
in-engine assessment. Tapered impact normals retain a radial approximation;
tests check unit length rather than the exact cone gradient. Busy-server
performance, archive payload saturation and Linux connected-client/turret
gameplay also remain to be validated.

## Running the checks

CMake/CTest provides capsule geometry, patch-planner and policy regression
suites. Use `ctest --test-dir out/build --output-on-failure` for the Windows
build or the corresponding Linux build directory.

After a Release build, the Windows instruction replays can be run in PowerShell:

```powershell
out/build/tickrate_policy_test.exe | Set-Content -Encoding ascii out/policy.json
python tools/test/tickrate_x86_test.py out/policy.json
python tools/test/native_timer_test.py PATH_TO_SUPPORTED_EXE bin/windows/mss32.build.dll src/mss32/symbols_mss32.h out/policy.json
```

The instruction tests need `unicorn`; the compiled-hook replay also needs
`pefile`, `capstone` and a locally supplied supported game executable. Keep test
outputs and artifact hashes under the ignored `out/` directory.
