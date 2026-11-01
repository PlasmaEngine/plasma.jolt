# Jolt headless tests

`JoltTest` is one executable on the engine test framework (`plTestFramework`, the same one CoreTest and RendererTest use). It links
the real `plJoltPlugin` and drives it through `plWorld`. It needs no editor, no GPU and no package store, but it does need an engine
SDK that has been built for the same configuration, including `TestFramework`.

```powershell
# From the package directory. PLASMA_SDK_ROOT is the engine checkout the package SDK builds against.
./Tests/Run-JoltTests.ps1 -SdkRoot E:\PlasmaTech\PlasmaEngine2

# One test, or several (-Filter is the framework's regex over test names)
./Tests/Run-JoltTests.ps1 -Filter JoltQueries
./Tests/Run-JoltTests.ps1 -Filter "JoltTriggers|JoltLayers"

# Known bugs fail the run (use while fixing one)
./Tests/Run-JoltTests.ps1 -StrictDefects

# The soak scene: 600 s under the Debug allocator is the R0 gate
./Tests/Run-JoltTests.ps1 -Configuration Debug -Filter JoltSoak -SoakSeconds 600
```

By hand: `plasmabuild build JoltTest -p Windows -a x64 -c Development`, put the engine's `Binaries/Windows_Development` on `PATH`
(`LD_LIBRARY_PATH` on Linux), then run `Bin/Windows_x64_Development/JoltTest.exe -run -close -noGui -noSave -all -assert 0 -json results.json`.

## Reading the result

The console prints one `Sub-Test '...' succeeded/failed` line per sub-test and ends with `All tests passed.` or the failure count, then
the open defects. `Output/JoltTest/results.json` holds the structured report: failing sub-tests with file and line. Exit code 0 means
every regular check passed.

A check for a roadmap bug that is still open is written `OpenDefect("PH-B14", condition, "message")`. While the bug exists it prints
`OPEN PH-B14: message (file:line)` and does not fail the run, and the run ends with `Open defects: PH-B12, PH-B14, ...`. When a fix makes
the check pass it prints `PH-B14 now passes, turn it into a regular check`; change it to `Check(...)` in the fixing commit.
`-StrictDefects` turns every open defect into a failure.

Messages the plugin logs as errors on purpose (a rejected capsule, ragdolls without a skeleton) are declared with
`plTestLogInterface::ExpectMessage` in the test that provokes them. Any other logged error fails the sub-test.

| Test | File | Covers |
|---|---|---|
| `JoltP0` | `JoltP0Regressions.cpp` | Temp allocator unwinding, `RaycastAll` beyond 256 hits, capsule dimensions, undeliverable impulses, actor teardown, `GetGravity()` |
| `JoltStacking` | `JoltStackingTests.cpp` | A five box tower at rest and asleep, a dropped box, a 64 box pile, a pyramid at 30/60/144 fps, removing the base of a sleeping tower, a fast box with CCD |
| `JoltLayers` | `JoltLayerTests.cpp`, `JoltTriggerTests.cpp` | Simulation filtering, query layers, query shape types, the ignore filter, a layer that does not collide with itself, triggers and layers |
| `JoltTriggers` | `JoltTriggerTests.cpp` | Enter and leave, bodies outside, a trigger of two child shapes, a body of two shapes, leaving one shape of a compound trigger, two bodies, a body deleted inside |
| `JoltQueries` | `JoltQueryTests.cpp`, `JoltTriggerTests.cpp` | Raycast (hit data on a rotated box), `RaycastAll`, all four sweeps and their hit data, initial overlap, all overlap tests, a box away from the origin, all shape queries, triggers and shape types, world space bounds |
| `JoltBreaks` | `JoltBreakTests.cpp` | Break force above and below the load, no break force, `BreakConstraint()`, deleting a joined body |
| `JoltBuoyancy` | `JoltBuoyancyTests.cpp` | A boat on a flat test water module, and its height at 30, 60 and 144 fps |
| `JoltCharacter` | `JoltCharacterTests.cpp` | Standing, walking, low and high steps, gentle and steep ramps, a moving platform, teleporting |
| `JoltLifecycle` | `JoltLifecycleTests.cpp` | Actor churn, sleeping actors being deleted, ragdoll component churn, world teardown |
| `JoltSoak` | `JoltStressScene.cpp` | Does nothing without `-joltSoakSeconds`. Spawns and deletes bodies, creates and deletes breakable joints and ragdoll components, keeps a character walking and sweeps box overlaps and ray casts over a 200 m square |

## Open defects

- `PH-B12` `GetGravity()` returns a hard-coded -10 instead of the configured gravity
- `PH-B13` `OverlapTestBox` places the box by its transform and ignores the position argument, which the AI voxelizer relies on
- `PH-B14` A body that overlaps two child shapes of one trigger, or a body of two shapes, is reported more than once and leaves early
- `PH-B15` A sweep with `m_bIgnoreInitialOverlap` still reports the shape it starts inside
- `PH-B32` Fixed constraint break force is compared with `ReduceMax()` of the solver lambda, which is negative under load, so a hanging box does not break the joint and the unit is a per-step impulse rather than newtons
- `UNLISTED-wake-on-remove` A sleeping box is not woken when the body it rests on is deleted, so it keeps floating. Not in the roadmap yet

`RaycastAll` also reports a convex shape twice (entry and exit face). The tests count distinct actors, so they do not depend on it.

## Not covered here

These need assets that a headless run does not have, so they are run from a project scene:

- Ragdoll reuse with a skeleton (limb creation, motor strength, reset on reuse, the Off mode) and simultaneous ragdoll deaths
- Glass slabs under automatic fire, and shattering in general
- Destructible meshes with contact damage enabled
- The AI voxelizer itself (the soak scene only repeats its `OverlapTestBox` and `RaycastAll` pattern)

## Adding a test with a fix

Add the check to the test of the area you fix, in the same commit as the fix. Write it as an `OpenDefect` before the fix, and change
it to a regular `Check` when the fix lands. New tests use `PL_CREATE_SIMPLE_TEST(<Group>, <Name>)` and the helpers in
`JoltTestSupport.h` (`World`, `AddStaticBox`, `AddDynamicBox`, `LayerConfig`).

## CI

The engine repository's "Build and test" workflow runs `JoltTest` on Windows and Linux through
`Utilities/Scripts/RunPackageTests.ps1`, at the plasma.jolt revision pinned in `Utilities/Scripts/TestPackages.json`. Bump that pin
when this directory changes. `.github/workflows/jolt-tests.yml` is a manual workflow for a self-hosted runner and the long soak.
