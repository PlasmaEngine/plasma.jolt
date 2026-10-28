# Jolt headless tests

`JoltTests` is one executable that links the real `plJoltPlugin` and drives it through `plWorld`. It needs no editor, no GPU
and no package store, but it does need an engine SDK that has been built for the same configuration.

```powershell
# From the package directory. PLASMA_SDK_ROOT is the engine checkout the package SDK builds against.
./Tests/Run-JoltTests.ps1 -SdkRoot E:\PlasmaTech\PlasmaEngine2

# One group, or with the R1 items counted as failures
./Tests/Run-JoltTests.ps1 -Group queries
./Tests/Run-JoltTests.ps1 -Strict

# The R0 soak scene: ten minutes under the Debug allocator
./Tests/Run-JoltTests.ps1 -Configuration Debug -StressSeconds 600
```

By hand: `plasmabuild build JoltTests --configuration Development`, put the engine's `Binaries/Windows_Development` on `PATH`,
then run `Bin/Windows_x64_Development/JoltTests.exe [--group <name>] [--strict] [--stress <seconds>]`.

Checks come in two kinds. A regression check must pass and fails the run. An R1 check describes behaviour that an R1 fix
delivers; while the fix is open it prints `OPEN R1` and only `--strict` turns it into a failure. When an R1 fix lands,
its check starts passing and the item disappears from the list.

| Group | File | Covers |
|---|---|---|
| `p0` | `JoltP0Regressions.cpp` | Temp allocator unwinding, `RaycastAll` beyond 256 hits, capsule dimensions, undeliverable impulses, actor teardown, `GetGravity()` |
| `stacking` | `JoltStackingTests.cpp` | A five box tower at rest and asleep, a dropped box, a 64 box pile |
| `layers` | `JoltLayerTests.cpp` | Simulation filtering between layers, query layers, query shape types, the ignore filter |
| `triggers` | `JoltTriggerTests.cpp` | Enter and leave, bodies outside, a trigger made of two child shapes |
| `queries` | `JoltQueryTests.cpp` | Raycast, `RaycastAll`, all four sweeps, initial overlap, all overlap tests, all shape queries, world space bounds |
| `breaks` | `JoltBreakTests.cpp` | Break force above and below the load, no break force, `BreakConstraint()`, deleting a joined body |
| `buoyancy` | `JoltBuoyancyTests.cpp` | A boat on a flat test water module, and its height at 30, 60 and 144 fps |
| `character` | `JoltCharacterTests.cpp` | Standing, walking, low and high steps, gentle and steep ramps, a moving platform, teleporting |
| `lifecycle` | `JoltLifecycleTests.cpp` | Actor churn, sleeping actors being deleted, ragdoll component churn, world teardown |

`--stress` runs `JoltStressScene.cpp` instead of the groups. It spawns and deletes bodies, creates and deletes breakable
joints and ragdoll components, keeps a character walking and sweeps box overlaps and ray casts over a 200 m square.

## Not covered here

These need assets that a headless run does not have, so they are run from a project scene:

- Ragdoll reuse with a skeleton (limb creation, motor strength, reset on reuse, the Off mode) and simultaneous ragdoll deaths
- Glass slabs under automatic fire, and shattering in general
- Destructible meshes with contact damage enabled
- The AI voxelizer itself (the soak scene only repeats its `OverlapTestBox` and `RaycastAll` pattern)

## Adding a test with a fix

Add the check to the group of the area you fix, in the same commit as the fix. Use `Category::R1Gate` while a behaviour is
still open, and drop the category argument once it has landed.
