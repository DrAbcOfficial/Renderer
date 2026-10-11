# HL25 startup video

Renderer replaces `CGame_DrawStartupVideo` only on HL25. It decodes VP8/VP9
through Mile.FFmpeg and draws planar 8-bit YUV with an independent Core Profile
shader and VAO, before `R_Init`. The shader preserves HL25's swapped-chroma
conversion coefficients and filters the converted RGB image. The original
`UI/valve_sound.wav` is played through the engine; embedded audio is ignored.
Enter, Escape, Space and the close event stop playback. Missing files, missing
DLL/API, incompatible ABI or decode failures log a diagnostic and skip the video.

## Dependency and installation

`MILE_FFMPEG_ROOT` accepts an extracted Mile.FFmpeg 6.1.0.0 SDK. Without an override,
CMake downloads the fixed release and verifies its SHA-256. The aggregate and
standalone builds use the same helper; the aggregate shares one download.

- Release: <https://github.com/ProjectMileArchived/Mile.FFmpeg/releases/tag/6.1.0.0>
- Archive: `Mile.FFmpeg.zip`
- SHA-256: `17425bd7cd737aa956daa9d6cb3cdff9b05fe6269634b06981e78f331dfd0272`
- Installed files: `svencoop/metahook/dlls/Mile.FFmpeg/{Mile.FFmpeg.dll,License.txt}`.

For Half-Life, the installer maps the `svencoop/` payload to `valve/` as usual.
Before loading plugins, MetaHook adds `metahook/dlls` and its subdirectories to
`PATH`. HL25 playback calls `LoadLibraryW(L"Mile.FFmpeg.dll")` using that host
search path. No import library is linked and Renderer does not change the search
path itself. The loader checks ABI major versions, allowing replacement
with compatible builds rather than enforcing a runtime hash.

## Distribution requirements

Renderer's own code keeps its existing license. Mile's wrapper is MIT and the
bundled FFmpeg build declares LGPL 2.1 or later. The archive contains the license,
but does **not** contain complete corresponding source. Before publishing a
binary distribution, provide the exact FFmpeg/dependency sources, patches and
build instructions corresponding to this SDK, with their notices. The GitHub
automatic source archive alone omits submodules and is insufficient. See
<https://ffmpeg.org/legal.html>. This change does not publish a binary release.

## Verification

With `RENDERER_BUILD_TESTS=ON`, `startup_video_tests` checks geometry, plane bounds,
timestamps, missing files/DLLs and cleanup. It accepts the DLL path and optionally
a WebM path to decode a real clip completely, then exercise interruption.
`startup_video_gl_tests` uses a real OpenGL 4.4 Core context to verify orientation,
odd dimensions, 420/422/444, negative stride, legacy colors and state restoration.
No GL-capable driver is reported as a skipped test, not a visual pass.

Real-game acceptance also requires normal playback with sound, all skip keys,
windowed/fullscreen playback, `-novid`, menu/map rendering after playback, and a
non-HL25 run without Mile.FFmpeg. Unit and GL probes do not replace those checks.
