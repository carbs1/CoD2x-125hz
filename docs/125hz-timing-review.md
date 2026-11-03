# 125 Hz timing review and corrections

Baseline: carbs1/CoD2x-125hz `combined-1.4.6.8`, experimental tag
`v1.4.6.8-125hz.1`, commit `6265e92e1aba7addbd3a0f92d7bb580f37e6cf3c`.
The repository's main branch does not contain those experimental changes.
Corrections are isolated on `fix/125hz-timing-and-capsules`.

The PR to `main` includes the original experiment as well as our fixes.
`capsule_hitboxes.cpp`, `capsule_models.cpp`, `capsule_trace.cpp` and the fitting
tool were introduced by `6265e92`; they were not all newly authored in this
review. Commits `be4f197` and `a5c84e9` isolate the review fixes and simplification.
The new history modules hold network delta bases, not an added display delay or
a change to the anti-lag policy.

## Corrected

| Area | Original issue | Result |
| --- | --- | --- |
| Mounted turret fire and sound | Subtracted 50 ms per world frame, consuming a 100 ms timer in 16 ms at 125 Hz | Subtract the configured frame duration; 100 ms takes 13 callbacks/104 ms at 125 Hz |
| Unmanned turret sound | Shares the sound routine, but runs on its own 50 ms think schedule | Keep the native decrement on this path; compensate only the mounted caller |
| Dropped-item velocity | Integer `1000/33` gave 30 rather than 30.303 at 30 Hz | Use floating-point reciprocal of actual frame milliseconds |
| Snapshot minimum | Missing `snaps` skipped `sv_minSnaps` and forced 50 ms | Apply the same default, clamp and configured minimum to missing userinfo |
| Server and client delta storage | 32 snapshots shortened useful history to about a quarter second at 125 Hz | Relocate 128 slots on both sides; patch readers, writers, masks, age guards, ping/profiling loops and reset paths together |
| Client parser pools | 2048 entries could be overwritten before expanded snapshots were decoded | Relocate 8192 entity/client parse entries and 128 packet timestamps, including safety thresholds |
| Archived snapshots | 1200 entries represented 9.6 seconds at 125 Hz | 7500 index entries, nominally 60 seconds at 125 Hz, plus a coherent 256 MiB payload ring |
| Capsule trace geometry | Parallel/grazing/axial/inside rays could miss or select the wrong entry | Shared robust finite-body solver; both roots and rounded ends; double intermediates; preserve wall fraction |
| Debug bot stance | Free/default mode erased existing crouch/prone input | Override stance only when explicitly selected |
| Windows release symbols | Build script assumed a particular local nm.exe and silently wrote an empty symbol table on failure | Use CMake's selected nm and fail the generator on error |

Native operand signatures and overlapping patch ranges are checked before the
new history/turret set is applied. Client capability is published only after
its complete relocation succeeds. Reloading a DLL whose storage is referenced
by native code is refused; restart the process to load a new build.

Server snapshot storage is allocated lazily when hosting, about 77.53 MiB for
64 clients. Client-only processes avoid that allocation. Hosting also reserves
the 256 MiB archive payload plus index and native snapshot pools. The index
duration is a capacity, not a guarantee: payload volume can expire archives
earlier. Capacities are fixed for the process lifetime and cover the tested
20..125 Hz range; clocks use the current `sv_fps` frame duration.

`cl_deltaSlots=128` reports installed decoding capability. It is not an access
rule or a secure client-integrity check. Peers without that capability receive
native-compatible full snapshots when a delta would exceed their history.
This does not change admission policy. A deployment requiring the new client
should distribute the patched DLL to every player and enforce its chosen
version policy separately.

## Timing decisions retained

- Native smooth-player trajectory duration stays 50 ms. It is a look-ahead cap,
  not the world simulation step. The client evaluates trajectories at snapshot
  times before interpolation; reducing this cap to 8 ms is not a valid general
  tickrate correction.
- Fuse and smoke lifetimes use elapsed milliseconds. Do not rescale those
  deadlines or blindly replace every 50 ms literal.
- Grenade integration still uses the fork's higher simulation rate. Bounce
  collision sampling can change trajectories; this patch does not claim stock
  grenade-throw parity or introduce a separate 50 ms physics cadence.
- `sv_realtimeWait` remains optional and defaults off, preserving this fork's
  policy for mods such as zPAM that compensate script waits themselves. Enable
  it for unmodified scripts; do not double-compensate a mod.
- `cl_interpDelay` remains configurable with the fork's 30 ms default. Additional
  buffering trades latency for stability; it is not a timer-unit defect.
- A mod can override `sv_fps`. Check the active value after its ruleset loads.
  This review does not force a zPAM ruleset or change anti-lag policy.

## Validation

Windows MinGW32 and Linux32 builds pass all three CTest suites. The capsule
matrix also passes optimized SSE2 builds on both platforms. See
[capsule-hitreg-validation.md](capsule-hitreg-validation.md) for cases and limits.

Emitted x86 runs in Unicorn: 448 register-adapter cases, 111 signed archive
remainder cases for 7500 entries, 880 payload-ring arithmetic cases, 1026 exact
client-age boundary cases, and 18 policy checks. A second replay loads the
actual compiled Windows turret hooks together with the supported original EXE:
60 duration/frame combinations pass on mounted fire, mounted sound and native
idle sound, including 8, 33, 50 and 100 ms frames.

Private Windows acceptance used 125 Hz, `sv_minSnaps 125`, capsule mode 1 and
`g_antilag 1`. A muted patched client remained active at 0/400/800 ms simulated
round-trip delay, used delta ages up to 103 frames, and survived `map_restart`.
Integer and floating 1-second script waits measured 1000 ms with real-time waits
enabled. Reconnecting a client without expanded history selected 32 slots and
safely used full snapshots under 400 ms round-trip delay. That client also ran
a short firing smoke test; it was not an aimed hit/damage assertion.

Private Linux acceptance also loaded the complete guarded patch set and ran
at 125 Hz across the 7500-entry archive-index wrap (8314 recorded frames).
This empty-server run did not fill the 256 MiB payload ring; no Linux connected
client or mounted-turret gameplay test is claimed.

The source/test changes and machine-readable results are local. No public
server restart, client rollout, remote push or release was performed. All
private test clients and servers are closed after their bounded runs.

For reproducibility after a Release build:

```text
ctest --test-dir out/build --output-on-failure
out/build/tickrate_policy_test.exe > out/policy.json
python tools/test/tickrate_x86_test.py out/policy.json
python tools/test/native_timer_test.py PATH_TO_SUPPORTED_EXE bin/windows/mss32.build.dll src/mss32/symbols_mss32.h out/policy.json
```

The Python instruction tests need `unicorn`; the compiled-hook replay also
needs `pefile` and `capstone`. No proprietary game executable is included with
these tests. Runtime evidence and tested artifact hashes are summarized in
[125hz-validation.json](125hz-validation.json).

## Claude Code simplification pass

Claude Code `/simplify` reviewed `be4f197` for reuse, structure and efficiency.
The follow-up shares redirect/operand planning, capacity constants and the
process-lifetime reload guard; removes an unused receive-counter hook and dead
exports; performs one snapshot-block lookup per client during ping calculation;
and reuses capsule scalars and test emulators. Capsule tolerances, root selection,
priority and wall limits remain intact. No admission or rollout policy changed.

The invoking agent reviewed the edits, tightened operand bounds to reject
overflowing ranges, and added a patch-planner regression suite. Both platforms
pass all four CTest suites; both optimized SSE2 capsule tests and the x86/turret
replays pass. An offline comparison of the three server planners matches all
41 Windows and 40 Linux patch sites after normalizing address-dependent branch
displacements. This pass launched no game and changed no live server.
See [125hz-simplify-validation.json](125hz-simplify-validation.json).

The PR branch was then reconciled with `main` at `28f6cfe`, retaining its newer
CI/symbol generation, crash diagnostics, text and IP-hiding changes. Both merged
builds pass all four CTest suites; x86/turret replays and the normalized server
patch-plan comparison also pass. No new connected-gameplay run was performed
for that merge.
