# Capsule hit registration validation

Reviewed experimental base `6265e92` against the corrected working branch.
No game was launched for these tests. These are geometry and source integration
checks, not a networked hit-registration acceptance test.

## Reproduced and fixed

- **Axial shots through a tapered limb could enter too late.** For axis
  `(0,0,0)` to `(0,0,20)`, radii `2` to `5`, and ray `(3,0,-10)` along `+z`,
  the old solver returned distance `26`; the finite tapered side is entered at
  `16.666667`. Both quadratic roots and both rounded ends must participate in
  the closest-hit calculation. The old code tested one root and could return
  before comparing the end spheres.
- **Long grazing shots could miss through float cancellation.** A radius-3
  capsule on that axis is hit by a ray from `(-8192,2.999,10)` along `+x`.
  The old code missed it. The corrected solver uses double intermediates,
  stable quadratic roots, and the direction vector's actual squared length.
  This also replaces the cancellation-sensitive constant-radius parallel-ray
  branch with the shared finite-body solver.
- **Starting inside an overlapping end sphere could miss.** The wider end
  sphere contains `(4.87,0,19)` in the tapered example, even though that point
  is outside the linearly interpolated body radius. Starting inside either
  rounded end now returns an immediate hit.
- **Starting inside skipped the wall fraction limit.** A zero incoming
  fraction now excludes an inside hit, as it already excluded outside entries.

## Tests and results

`tools/test/capsule_hitreg_regression.cpp` uses a separate double-precision
intersection reference. It checks fixed axis/parallel/tangent/inside/away/long
shot cases, 20,000 deterministic primitive rays, and 18,000 public API rays.
The public API matrix covers synthetic standing, crouched and prone joint
arrangements, three rotations, shared shapes plus four fitted body models,
head/torso/limb priority selection, scale, and wall limits. It also checks
unit normals and untouched fallback results for empty skeletons and zero-length
rays. Synthetic poses are not captures of the game's animation assets.

| Build | Primitive disagreements | Public API disagreements |
| --- | ---: | ---: |
| Original `6265e92`, Linux 32-bit x87, `-O0` | 462 / 20,000 | 36 / 18,000 |
| Corrected, Linux 32-bit x87, `-O0` | 0 | 0 |
| Corrected, Windows MinGW 32-bit x87, `-O0` | 0 | 0 |
| Corrected, Linux 32-bit SSE2, `-O2` | 0 | 0 |
| Corrected, Windows MinGW 32-bit SSE2, `-O2` | 0 | 0 |

The original counts include wrong hit distances as well as hit/miss or hit-group
differences; they are not percentages of real gameplay shots. The fixed runs'
worst primitive distance error was below `0.000004` game units, and worst public
trace fraction error below `0.000000084`. The existing capsule test suite also
passes alongside the regression suite.

Run with CMake/CTest after configuring a 32-bit-capable GCC/MinGW toolchain:

```text
cmake --build out/build-linux --target capsule_trace_test capsule_hitreg_regression
ctest --test-dir out/build-linux --output-on-failure
```

The regression includes the implementation in its own translation unit to test
private primitive routines without exporting them from the game library.

## Integration scope and remaining acceptance work

The capsule hook replaces the native `G_LocationalTrace` call to
`DObjTraceline`; it does not replace bullet damage, world collision, or anti-lag
policy. Source inspection of the native reconstruction shows full skeleton
calculation before the local-space trace. The incoming wall fraction and hit
priority still control which hit can be accepted. A handled capsule miss keeps
the miss; it does not retry the stock boxes. Non-player or incomplete mandatory
skeletons keep the native fallback.

Native anti-lag rewinds target origins around bullet firing. The capsule patch
adds no historical animation or stance rewind. A connected-client test should
therefore compare stationary head/torso/limb shots, crouch/prone transitions,
direction changes, and wall edges with `g_antilag` both off and on, using known
latency and authoritative hit/damage logs. Enable `sv_capsuleHitboxes 1` or `2`:
the default `0` uses stock hitboxes and cannot validate capsules.

Two limits need separate integration assessment: the native locational
broad-phase bounds may clip shapes enlarged beyond the normal player bounds
at scale `2`, and the existing impact-normal calculation is radial rather than
the exact gradient of a tapered side. The tests validate unit normal length,
not exact impact-normal orientation. Neither is a demonstrated networked miss
in this test run. Visual fit across the actual animated model library, native
damage/penetration behavior, and performance under a busy server remain outside
these offline results.
