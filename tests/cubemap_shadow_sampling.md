# Cubemap shadow sampling regression (Renderer #15)

`gl_cubemap_shadow_sampling_tests` is a separate CTest case using the existing
`gl_shadow_cache_tests` executable's `--sampling` mode. It loads:

- `assets/cstrike/maps/cs_assault_shadow.obj` (33,507 triangles).
- `assets/cstrike/maps/cs_assault_entity.txt` (29 shadow-casting point lights;
  the directional light is outside this cubemap regression).

The origins, radii and shadow resolutions come from the entity file. The test
does not compare configuration/shader text or assert generated file contents.

## What is exercised

The hidden GLFW OpenGL 4.3 context renders the real OBJ into six depth faces,
using the existing fixture's multiview, radial-depth shader. Texture allocation
and comparison state come from production `src/cubemap_shadow_gl.h`.
The sampling shader compiles the actual `dlight_shader.frag.glsl` and `common.h`,
renaming its entry point and calling `CalcCubemapShadowIntensity` from a small
probe entry point. Bias and PCF are not copied into test code or changed.

An independent, two-sided CPU ray/triangle intersection supplies the occlusion
oracle. At the issue's camera position `(-143,2468,558)`, looking along +X with
90-degree horizontal FOV at 800x600, seven camera rays intersect the OBJ around
the ceiling and left-wall seams. The offending light `(-271,2448,380)` is selected
from the loaded lights. A probe must lie inside that light and behind a real
blocker before the test expects zero visibility. For example, pixel `(400,138)`
hits the ceiling at approximately `(-58.790,2467.895,592)`.

Additional coverage:

- CPU rays select 476 blocked/lit pairs across the map's 29 lights. Every signed
  cube edge and corner must be represented; pairs with a geometric penumbra in
  the sampling neighborhood are excluded. Lit controls prevent an always-dark
  sampler from passing.
- Controlled six-face depths cover all 12 edges and 8 corners, both static and
  dynamic layers, and the combined visibility.
- Different depths on neighboring faces check continuous filtering across both
  sides of an edge and all three approaches to a corner. `CLAMP_TO_EDGE` alone
  cannot pass this check with seamless filtering disabled.
- Both production PCF variants are compiled and run.
- The sampling scope preserves an initially enabled or disabled seamless state.

The test evaluates actual floating-point GPU results. Occluded/lit visibility
has a 0.0001 tolerance; mixed-depth edge and corner filtering use 0.03 and 0.04
tolerances for subtexel interpolation. It does not load the engine, Studio hooks,
or BulletPhysics. Those integration boundaries still require game validation.

## Run

With `RENDERER_BUILD_TESTS=ON` in an aggregator build:

```powershell
cmake --build build/x86/Debug --config Debug --target Renderer gl_shadow_cache_tests
ctest --test-dir build/x86/Debug/Plugins/Renderer -C Debug -R '^gl_cubemap_shadow_sampling_tests$' -V
```

The executable can also be run directly:

```powershell
gl_shadow_cache_tests.exe <cs_assault_shadow.obj> <cs_assault_entity.txt> --sampling <renderer/shader-directory>
```

A missing driver-backed GL context returns 2 and is **Skipped**, not a verified
pass. Invalid resources, shader compilation errors and assertion failures fail
the test. The original invocation without `--sampling` remains the cache suite.

## Root cause and verification (2026-10-11)

White `CLAMP_TO_BORDER` depth with non-seamless linear cubemap comparisons mixed
unoccluded border samples into otherwise blocked receivers. Before the fix the
new test failed on the actual ceiling probe with visibility 0.007813 instead of
zero. The fix uses `CLAMP_TO_EDGE` and enables seamless filtering within
`R_LightShadingPass`, restoring the caller's state on exit. The 2D/array shadow
paths and shader bias are unchanged.

- Renderer and sampling test built in MSVC Win32 Debug and Release.
- Debug CTest: 10/10 passed. Release: both shadow GPU suites passed.
- Actual test GPU: NVIDIA GeForce RTX 5060, driver 610.88, OpenGL 4.3.
- Shared formatting validation and `git diff --check` passed.
- Counter-Strike 10210, `cs_assault`: the ceiling/left-wall seams disappeared at
  the same camera position with the deployed Debug DLL. RenderDoc confirmed
  ClampEdge and seamless=true during lighting, and seamless=false after the
  pass. The offending light no longer changes the formerly leaking pixel.

Local capture and before/after artifacts are under
`D:/rdc-captures/renderer-issue15/` (`baseline.rdc`, `fixed.rdc`,
`before-after-fix.png`, `fixed-frame-state.json`); these are investigation
artifacts, not repository test dependencies. Other GPU vendors were not tested.
