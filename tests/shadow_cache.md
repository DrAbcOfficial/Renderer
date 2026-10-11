# Local-light shadow cache regression (#807)

`gl_shadow_cache_tests` creates a hidden GLFW OpenGL 4.3 context and real depth-stencil
framebuffers. It loads `assets/cstrike/maps/cs_assault_shadow.obj` and the point
lights from `assets/cstrike/maps/cs_assault_entity.txt`, without changing their
positions, radii or shadow resolutions. A cube represents a player. The fixture's
directional light is reported but excluded: camera-dependent CSM caching is not
part of this change. A spotlight derived from the first point light exercises the
single-layer path separately.

The candidate uses the production `shadow_cache.h` policy, bone-box transform and
`shadow_cache_gl.h` image-copy operation. The independent rendering oracle submits
every caster on every frame without spatial rejection or temporal reuse. All six
depth faces (or the spotlight layer) are compared with a tolerance of 0.000002;
static world depth is also compared with a fresh world render. The test verifies
that the player actually writes depth, so an entirely empty renderer cannot pass.
This is a depth regression, not a rendered-game screenshot or FPS benchmark. Its
small test shaders model linear-distance shadow depth; the GoldSrc ABI, renderer
hooks, game shaders and deferred-lighting pass are not loaded into this executable.

The separate [`--sampling` mode](cubemap_shadow_sampling.md) reuses the OBJ/light
fixture for issue #15. That mode compiles the production cubemap sampling shader
and checks light leaks at the reported position, cube edges and corners.

## Run

The test is registered when `RENDERER_BUILD_TESTS=ON`. In the aggregator workspace:

```powershell
cmake -S . -B build/x86/Debug -DRENDERER_BUILD_TESTS=ON
cmake --build build/x86/Debug --config Debug --target Renderer gl_shadow_cache_tests
ctest --test-dir build/x86/Debug/Plugins/Renderer -C Debug -R '^gl_shadow_cache_tests$' -V
```

It can also be run directly with two arguments: the OBJ path and entity-file path.
No asset download or running game is required. Exit code 2 means the driver-backed
GL context is unavailable and CTest reports **Skipped**, not a verified pass.

## Automated coverage

- Empty-cache initialization and reuse; conservative per-frame animated-caster updates.
- Local entry, movement, rotation, leaving every light, deletion and replacement.
- Multiple casters: a retained caster survives clearing/rebuilding after another leaves.
- Entity-list reorder and membership changes, invisible lights, motion while invisible,
  and return to visibility. Visibility is explicitly supplied to the harness; the
  engine's PVS/frustum generation is not simulated.
- Light translation, radius change, texture resize and explicit invalidation.
- Separate static world depth and the combined world/entity fallback when no static
  layer is configured.
- Client-owned geometry appearing and disappearing over a restored entity-only image.
- Unknown/non-finite bounds, sphere tangency, cone boundaries and forced refresh.
- Spotlight rotation, cone-angle changes, entity deletion and empty reuse.
- Bone-box transformation checked against independent vertex samples under rotation,
  translation, nonuniform scale, reflection and shear.

Output includes update/submission counts, compared depth pixels, CPU cache-decision
time and GPU candidate-pass time. Timing includes a regression-test workload with
readbacks and synchronization; do not convert it into a predicted game FPS gain.

## In-game controls and manual acceptance

`r_shadow_cache 0` uses uncached local-light rendering. `1` enables caching (default).
`2` enables caching and prints one frame's update/reuse counts, caster submissions,
transformed bone-box count and CPU preparation time approximately once a second.
The reported preparation time includes Studio pose preparation; compare this with
shadow-pass GPU time in a frame capture. CSM is unchanged and should be timed separately.

Ordinary Studio and brush casters remain conservatively volatile in this version,
including a stationary brush that may contain animated masked textures. Thus any
light with a related caster still updates every frame. Unknown bounds, follow
attachments, unusual render effects and brush shadow proxies retain unbounded
fallback. Cached entity depth costs one extra same-size depth-stencil texture per
local light. The final output is restored and client opaque drawing is replayed
every frame, even on a cache hit; cache-hit counts do not mean zero GL work.

The following remain real-game acceptance items, using identical camera/demo states
with `r_shadow_cache 0` and `1`:

| Scenario | Check |
| --- | --- |
| Idle animation, gait, controller/blending transitions | Shadows animate continuously; pose preparation does not duplicate events or alter the main-view pose. |
| Moving/rotating doors | Shadow follows interpolated brush motion and disappears at the old location. |
| BulletPhysics corpse and jiggle | Shadows follow physics outside the original sequence bounds. |
| Weapon/follow/lower-body attachments | No lost geometry or parent-state changes across shadow/main/water passes. |
| Turn away and return; entity leaves PVS | No residual or missing shadow; off-screen casters admitted by the engine still work. |
| Model/resource reload and map change | No stale bounds, dangling resources or inherited shadow contents. |
| Many players / large Studio models | CPU preparation remains below the saved rendering cost; inspect `bone boxes` and `prepare` values. |

GLFW passing does not establish these engine/plugin compatibility results.

## Verification record (2026-10-10)

- MSVC x86 Debug and Release: Renderer and all six Renderer test targets built.
- CTest: 6/6 passed in each configuration, including actual GLFW execution on
  NVIDIA GeForce RTX 5060 / driver 610.88 (OpenGL 4.3 context).
- Fixture: 33,507 OBJ triangles, 29 point lights. The complete scenario sequence
  compared 951,615,488 depth pixels. Candidate caster submissions were 62 versus
  725 for the uncached oracle; the local player affected 2/29 lights, and unrelated
  movement outside all lights caused 0 updates. These are harness results, not
  measurements of the actual game or Studio preparation cost.
- Renderer formatting check and `git diff --check` passed. A separate read-only
  review found no confirmed added regression; the manual items above remain open.
