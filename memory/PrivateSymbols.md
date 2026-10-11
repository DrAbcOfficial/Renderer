---
title: PrivateSymbols
type: reference
permalink: renderer/private-symbols
tags:
- renderer
- private-vars
- private-funcs
- private-globals
- gamedata
- symbol-resolution
- vtable
- patch
- reference
---

# Game-private symbols used by `Renderer`

This document inventories the unexported engine (`hw.dll` / `hw.decrypt.dll`) and client
(`client.dll`) functions, globals, virtuals, struct members and patched call sites that the
Renderer plugin consumes. Symbol names are the MetaHook gamedata catalog names (the string
passed to `ResolveGameSymbol`); the plugin's local `gPrivateFuncs.*` field or extern global is
given alongside where it differs.

The note is deliberately current-state only. Every earlier signature/string/disassembly
locator has been replaced by gamedata resolution, and the historical migration log that used
to trail this document is dropped. The earlier history is still reachable at the previous file
name — `git log -- memory/renderer-privatevars.md`; the rewrite is too large for
`git log --follow` to bridge the rename.

## Resolution model

- **Gamedata-first, exclusively.** Every game-private symbol is resolved from the catalog
  through MetaHook API **114** (`src/plugins.cpp:38` rejects older hosts): `ResolveGameSymbol`
  (`function` / `global` / `virtualFunction` / `patch`), `QueryGameSymbolStructMember`,
  `QueryGameSymbolScalar`, and `IsGameSymbolAvailable` for capability / optionality probes.
- **Helpers** (`src/plugins.h`):
  - `GamedataResolvePtr(moduleBase, moduleName, symbolName, kind)` — required symbol; a miss is
    a fatal `Sys_Error("Could not resolve gamedata symbol: …")`.
  - `GamedataResolvePtrIfAvailable(...)` — returns `nullptr` only when
    `IsGameSymbolAvailable` reports `SYMBOL_NOT_FOUND`; any other failure is fatal.
  - `GamedataQueryStructMember(...)` — required `structMember` byte offset; fatal if missing.
  - Portal layout scalars are read with `QueryGameSymbolScalar` directly inside
    `Client_FillAddress_CoreProfile` (`gl_hooks.cpp`).
- **Module base.** Resolution always uses the **real** module image
  (`RealDllInfo.ImageBase` / `g_ClientDLLInfo.ImageBase`). `Engine_FillAddress` /
  `Client_FillAddress` still take the `(DllInfo, RealDllInfo)` pair — the mirror image — but the
  `DllInfo` argument now only survives for `GetVFunctionFromVFTable` in `EngineSurfaceHook.cpp`;
  it is no longer used as a scan image.
- **No scanning remains.** `DisasmRanges`, `ReverseSearchFunctionBegin[Ex]`, `GetCallAddress`,
  `Convert_VA_to_RVA` / `Convert_RVA_to_VA` and `Search_Pattern*` have no callers anywhere in
  `src/` (some macros are still defined in `plugins.h`). The only disassembly left is a single
  Capstone instruction decode per legacy texture-allocation patch site
  (`R_RedirectEngineLegacyOpenGLTextureAllocation`). `<capstone.h>` is included only by
  `gl_hooks.cpp`.
- **Failure policy.** Required symbols are fatal at load; explicitly optional symbols
  (`g_ViewEntityIndex_SCClient`, the portal layout scalars, the second/third multitexture-init
  candidate) stay null without error when the identity publishes nothing.

## Where the contract lives

- `scripts/manifests/renderer.json` — the authoritative `(module, symbol, kind)` declaration in
  `symbols`, plus per-family `conditionalGroups` and the `optionalSymbols` / `numberedPatchSets`
  lists. 11 engine identities: `cof-5936`, `hl-3248/3266/3329/3647/4554/6153/8684`,
  `hl-10210`, `svencoop-10257/8948`.
- `scripts/validate-gamedata.py` — the `RENDERER_*` family tables and `validate_renderer()` pin
  every consumed symbol to the identities and module that must publish it.
- `scripts/tests/test_gamedata_contract.py::RendererGateTests` — behavioral contract tests.
- The build-time sync (`scripts/sync-gamedata.py`) prunes the upstream catalog into
  `metahook/gamedata/renderer/`, which the host launcher merges at runtime.

## Locators

All engine locators live in `src/gl_hooks.cpp` and are named `Engine_FillAddress_<anchor>`; the
Studio one-shot locators are `Engine_FillAddress_Studio*`. They are dispatched, in a fixed
order, by `Engine_FillAddress` (`src/gl_hooks.cpp:909`). Client locators are
`Client_FillAddress_*` in `gl_hooks.cpp` (dispatched by `Client_FillAddress`, `:1569`); the
client Studio fill is `ClientStudio_FillAddress` (`src/exportfuncs.cpp:150`). Two EngineSurface
locators are `Engine_FillAddress_EngineSurface_*` (`src/EngineSurfaceHook.cpp:44`, `:50`).

Dispatch order is observable only if two locators fill the same field; it is kept stable so the
resolution sequence does not churn.

## Engine symbols

### OpenGL context, textures and 2D

| Catalog symbol | Kind | Local | Resolver | Notes |
| --- | --- | --- | --- | --- |
| `GL_Init` | function | `GL_Init` | `_GL_Init` | inline-hooked |
| `gl_extensions` | global | `gl_extensions` | `_GL_Init` | |
| `GL_SetMode` | function | `GL_SetMode_SvEngine` / `GL_SetMode_GoldSrc` | `_GL_SetMode` | SvEngine = true 3-arg ABI; HL25 / SDL GoldSrc = six-arg ABI |
| `GL_SetModeLegacy` | function | `GL_SetModeLegacy` | `_GL_SetMode` | every other identity (legacy GoldSrc, blob, CoF) |
| `GL_SetMode_call_qwglCreateContext` | patch | `GL_SetMode_call_qwglCreateContext` | `_GL_SetMode` | non-SvEngine only; redirected to `CoreProfile_qwglCreateContext` |
| `SDL_InitGL` | function | `SDL_InitGL` | `_GL_SetMode` | GoldSrc / HL25 SDL builds only |
| `GL_SelectPixelFormat` | function | `GL_SelectPixelFormat` | `_GL_SetMode` | legacy branch only; inlined (no record) on HL25 / SvEngine |
| `GL_Bind` | function | `GL_Bind` | `_GL_Bind` | inline-hooked |
| `currenttexture` | global | `currenttexture` | `_GL_Bind` | |
| `GL_LoadTexture2` | function | `GL_LoadTexture2` | `_GL_LoadTexture2` | inline-hooked |
| `gHostSpawnCount` | global | `gHostSpawnCount` | `_GL_LoadTexture2` | |
| `gltextures` | global | `gltextures` (non-SvEngine) / `gltextures_SvEngine` | `_GL_LoadTexture2` | SvEngine uses the vector pointer slot |
| `numgltextures` | global | `numgltextures` | `_GL_LoadTexture2` | non-SvEngine only |
| `gltextures.m_Size` | global | `numgltextures` | `_GL_LoadTexture2` | SvEngine only |
| `gltextures.m_Memory.m_nAllocationCount` | global | `maxgltextures_SvEngine` | `_GL_LoadTexture2` | SvEngine only |
| `peakgltextures` | global | `peakgltextures_SvEngine` | `_GL_LoadTexture2` | SvEngine only |
| `realloc` | function | `realloc_SvEngine` | `_GL_LoadTexture2` | SvEngine only |
| `GL_SelectTexture` | function | `GL_SelectTexture` | `_GL_SelectTexture` | called by the plugin, not hooked |
| `GL_Set2D` / `GL_Finish2D` | function | same | `_GL_Set2D` / `_GL_Finish2D` | inline-hooked |
| `GL_BeginRendering` / `GL_EndRendering` | function | same | `_GL_BeginRendering` / `_GL_EndRendering` | inline-hooked; `GL_EndRendering` reads `window_rect` |
| `GL_UnloadTextures` | function | `GL_UnloadTextures` | `_R_NewMap` | inline-hooked |
| `GL_LoadFilterTexture` | function | `GL_LoadFilterTexture` | `_GL_LoadFilterTexture` | inline-hooked |
| `GL_BuildLightmaps` | function | `GL_BuildLightmaps` | `_GL_BuildLightmaps` | inline-hooked |
| `BuildGammaTable` | function | `BuildGammaTable` | `_BuildGammaTable` | inline-hooked |
| `texgammatable`, `lightgammatable` | global | same | `_BuildGammaTable` | hook wrapper writes the identity table; the engine reads it |
| `LegacyMultiTextureInit` | function | `LegacyMultiTextureInit` | `_LegacyMultiTextureInit` | first available of `CheckMultiTextureExtensions` / `InitMultitexturing` / `DT_Initialize`; inline-hooked with an empty body |
| `detTexSupported` | global | `detTexSupported` | `_LegacyMultiTextureInit` | write-through (forced false) |
| `Draw_Frame` | function | `Draw_Frame` | `_Draw_Frame` | inline-hooked |
| `giScissorTest` | global | `giScissorTest` | `_Draw_Frame` | |
| `Draw_SpriteFrameHoles`, `Draw_SpriteFrameAdditive`, `Draw_SpriteFrameGeneric` | function | same | `_Draw_SpriteFrameHoles` / `_Additive` / `_Generic` | non-SvEngine |
| `Draw_SpriteFrameHoles_SvEngine`, `Draw_SpriteFrameAdditive_SvEngine`, `Draw_SpriteFrameGeneric_SvEngine` | function | same | the same three locators | SvEngine variants; all six are inline-hooked |
| `Draw_FillRGBA` / `Draw_FillRGBABlend` | function | same | `_Draw_FillRGBA` / `_Draw_FillRGBABlend` | all identities; inline-hooked |
| `Draw_FillRGBABuf` | function | `Draw_FillRGBABuf` | `_Draw_FillRGBABuf` | SvEngine only (buffered eight-integer rectangle); inline-hooked |
| `D_FillRect` | function | `D_FillRect` | `_D_FillRect` | non-SvEngine only; SvEngine has no legacy `(vrect_t*, color*)` body |
| `Draw_Pic` | function | `Draw_Pic` | `_Draw_Pic` | inline-hooked |

`Draw_SpriteFrame*` also feed the plugin's sprite path; `Draw_FillRGBA` / `Draw_FillRGBABlend` /
`D_FillRect` / `Draw_Pic` back the 2D drawing wrappers in `gl_rmain.cpp`.

### Render view, scene, world and globals

| Catalog symbol | Kind | Local | Resolver | Notes |
| --- | --- | --- | --- | --- |
| `R_RenderView` | function | `R_RenderView` / `R_RenderView_SvEngine` | `_R_RenderView` | SvEngine takes a view index; inline-hooked |
| `c_alias_polys` / `c_model_polys` | global | `c_alias_polys` | `_R_RenderView` | SvEngine publishes the alias counter as `c_model_polys` |
| `c_brush_polys` | global | `c_brush_polys` | `_R_RenderView` | |
| `r_worldentity`, `cl_worldmodel` | global | same | `_R_RenderView` | |
| `V_RenderView` | function | `V_RenderView` | `_V_RenderView` | not hooked |
| `r_playerViewportAngles` | global | same | `_V_RenderView` | |
| `R_NewMap` | function | `R_NewMap` | `_R_NewMap` | inline-hooked |
| `R_CullBox` | function | `R_CullBox` | `_R_CullBox` | inline-hooked |
| `frustum`, `vpn`, `vup`, `vright` | global | same | `_R_CullBox` | |
| `R_ForceCVars`, `R_CheckVariables`, `R_AnimateLight` | function | same | `_R_ForceCVars` | `R_ForceCVars` is inline-hooked (full reimplementation); the other two are resolved only |
| `r_world_matrix`, `gProjectionMatrix` | global | `r_world_matrix`, `r_projection_matrix` | `_R_SetupGL` | the locator is named for `R_SetupGL`, which itself is not resolved |
| `gWorldToScreen`, `gScreenToWorld` | global | same | `_R_SetupGL` | |
| `gmodinfo_vertical_fov` | global | `vertical_fov_SvEngine` | `_R_SetupGL` | SvEngine only |
| `r_refdef` | global | `r_refdef_SvEngine` / `r_refdef_GoldSrc` | `_RenderSceneVars` | one catalog global, two local layouts; the plugin links `r_refdef` pointers through the matching struct |
| `CL_IsDevOverviewMode`, `CL_SetDevOverView` | function | same | `_RenderSceneVars` | |
| `cl_waterlevel` | global | `cl_waterlevel` | `_RenderSceneVars2` | |
| `ClientDLL_DrawNormalTriangles` | function | same | `_RenderSceneVars2` | |
| `gDevOverview` | global | `gDevOverview` | `_RenderSceneVars2` | |
| `ClientDLL_DrawTransparentTriangles` | function | same | `_R_DrawTEntitiesOnListVars` | |
| `cl_parsecount`, `r_blend`, `r_entorigin` | global | same | `_R_DrawTEntitiesOnListVars` | |
| `allow_cheats` | global | `allow_cheats` | `_CL_IsDevOverviewModeVars` | SvEngine only |
| `r_framecount`, `r_visframecount` | global | same | `_R_RecursiveWorldNodeVars` | |
| `r_viewleaf`, `r_oldviewleaf` | global | same | `_R_MarkLeaves` | |
| `modelorg` | global | `modelorg` | `_R_DrawWorld` | |
| `envmap`, `cl_stats`, `cl_weaponstarttime`, `cl_weaponsequence`, `cl_light_level` | global | same | `_R_DrawViewModel` | |
| `rtable` | global | `rtable` | `_R_TextureAnimation` | the locator is named for `R_TextureAnimation`, which itself is not resolved |
| `lightmaps`, `gDecalSurfCount` | global | same | `_R_DrawSequentialPoly` | locator named for `R_DrawSequentialPoly`, not resolved |

### Models, cache, memory and edicts

| Catalog symbol | Kind | Local | Resolver | Notes |
| --- | --- | --- | --- | --- |
| `Mod_PointInLeaf` | function | `Mod_PointInLeaf` | `_Mod_PointInLeaf` | inline-hooked |
| `PVSNode` | function | `PVSNode` | `_PVSNode` | inline-hooked |
| `Mod_LoadStudioModel` | function | `Mod_LoadStudioModel` | `_Mod_LoadStudioModel` | inline-hooked |
| `Mod_LoadBrushModel` | function | `Mod_LoadBrushModel` | `_Mod_LoadBrushModel` | resolved only |
| `Mod_LoadModel` | function | `Mod_LoadModel` | `_Mod_LoadModel` | resolved only |
| `Mod_LoadSpriteModel` | function | `Mod_LoadSpriteModel` | `_Mod_LoadSpriteModel` | inline-hooked |
| `Mod_UnloadSpriteTextures` | function | `Mod_UnloadSpriteTextures` | `_Mod_UnloadSpriteTextures` | inline-hooked (Linux note: may be inlined into `SPR_Shutdown` there) |
| `gSpriteMipMap` | global | `gSpriteMipMap` | `_Mod_LoadSpriteFrame` | |
| `Cache_Alloc` | function | `Cache_Alloc` | `_Cache_Alloc` | |
| `cache_head` | global | `cache_head` | `_Cache_Alloc` | |
| `Hunk_AllocName` | function | `Hunk_AllocName` | `_Hunk_AllocName` | |
| `Host_ClearMemory` | function | `Host_ClearMemory` | `_Host_ClearMemory` | inline-hooked |
| `Host_IsSinglePlayerGame` | function | `Host_IsSinglePlayerGame` | `_Host_IsSinglePlayerGame` | |
| `S_ExtraUpdate` | function | `S_ExtraUpdate` | `_S_ExtraUpdate` | |
| `V_FadeAlpha` | function | `V_FadeAlpha` | `_V_FadeAlpha` | |
| `cl_sf` | global | `cl_sf` | `_V_FadeAlpha` | |
| `mod_known`, `mod_numknown` | global | same | `_ModKnownVars` | |
| `cl_max_edicts`, `cl_entities` | global | same | `_CL_ReallocateDynamicData` | |
| `cl_numvisedicts`, `cl_visedicts` | global | same | `_VisEdicts` | |
| `cl_simorg` | global | `cl_simorg` | `_CL_SimOrgVars` | |
| `cl_viewentity` | global | `cl_viewentity` | `_CL_ViewEntityVars` | |
| `gTempEnts` | global | `gTempEnts` | `_TempEntsVars` | |
| `R_GetSpriteFrame` | function | `R_GetSpriteFrame` | `_R_GetSpriteFrame` | inline-hooked |
| `Draw_DecalTexture` | function | `Draw_DecalTexture` | `_Draw_DecalTexture` | wrapper readable |
| `gDecalPool`, `gDecalCache` | global | same | `_R_DecalInit` | |

### Studio, lighting, fog, particles and images

| Catalog symbol | Kind | Local | Resolver | Notes |
| --- | --- | --- | --- | --- |
| `R_GLStudioDrawPoints` | function | `R_GLStudioDrawPoints` | `_R_GLStudioDrawPoints` | inline-hooked |
| `R_FreeDeadParticles`, `R_TracerDraw`, `R_BeamDrawList` | function | same | `_R_DrawParticles` | called by the plugin's particle path |
| `active_particles`, `particletexture` | global | same | `_R_DrawParticles` | |
| `r_ambientlight`, `r_shadelight`, `r_plightvec` | global | same | `_R_StudioLighting` | |
| `currententity` | global | `currententity` | `_StudioGlobals` | |
| `pstudiohdr` | global | `pstudiohdr` | `_StudioGlobals` | |
| `r_origin` | global | `r_origin` | `_StudioGlobals` | |
| `cl_time`, `cl_oldtime` | global | same | `_StudioGetTimes` | |
| `g_ForcedFaceFlags` | global | same | `_StudioSetForceFaceFlags` | |
| `r_topcolor`, `r_bottomcolor` | global | same | `_StudioSetRemapColors` | |
| `CL_FxBlend` | function | `CL_FxBlend` | `_StudioSetRenderamt` | inline-hooked |
| `GlowBlend` | function | `GlowBlend` | `_GlowBlend` | non-SvEngine / non-HL25 only (inlined there); resolved only |
| `psubmodel` | global | `psubmodel` | `_StudioSetupModel` | |
| `r_colormix` | global | `r_colormix` | `_StudioSetupLighting` | |
| `g_bUserFogOn`, `flFinalFogColor`, `flFogDensity`, `flFogStart`, `flFogEnd` | global | same | `_R_RenderFinalFog` | |
| `cshift_water`, `gWaterColor` | global | same | `_WaterVars` | |
| `d_lightstylevalue` | global | `d_lightstylevalue` | `_LightstyleVars` | |
| `host_basepal` | global | `host_basepal` | `_BasePalette` | |
| `r_missingtexture` / `r_notexture_mip` | global | same | `_MissingTexture` / `_NoTexture` | SvEngine requires `r_missingtexture`; the other ten require `r_notexture_mip` |
| `scr_drawloading` | global | `scr_drawloading` | `_SCR_BeginLoadingPlaque` | |
| `scr_fov_value` | global | `scr_fov_value` | `_ScrFov` | |
| `movevars` | global | `pmovevars` | `_MoveVars` | |
| `gl_filter_min`, `gl_filter_max` | global | same | `_GL_FilterMinMaxVars` | |
| `filterMode` | global | `filterMode` | `_SetFilterMode` | |
| `filterColorRed`, `filterColorGreen`, `filterColorBlue` | global | same | `_SetFilterColor` | |
| `filterBrightness` | global | `filterBrightness` | `_SetFilterBrightness` | |
| `cl_dlights`, `cl_elights` | global | same | `_CL_DlightVars` | the plugin reaches the allocators through `gEngfuncs.pEfxAPI` |
| `numTransObjs`, `maxTransObjs`, `transObjects` | global | same | `_R_AllocTransObjectsVars` | |
| `window_rect` | global | `window_rect` | `_VID_UpdateWindowVars` | |
| `s_fXMouseAspectAdjustment`, `s_fYMouseAspectAdjustment` | global | same (+ plugin storage) | `_GL_EndRenderingVars` | published on the five FBO identities; the other six bind plugin-owned storage |

### Patches, struct members and layout scalars (engine)

| Symbol | Kind | Resolver | Notes |
| --- | --- | --- | --- |
| `Sys_ShutdownGame_to_GL_Shutdown_callsite_0` | patch | `_GL_Shutdown` | redirect to the plugin `GL_Shutdown`; installed once, never reverted |
| `GL_SetMode_call_qwglCreateContext` | patch | `_GL_SetMode` | non-SvEngine; redirect to `CoreProfile_qwglCreateContext` |
| `CL_LinkPacketEntities_to_R_ResetLatched_callsite_N` | patch (numbered set) | `R_PatchResetLatched` | every numbered site is redirected to `R_ResetLatched_Patched`; skipped on HL25 |
| `R_ResetLatched` | function | `R_PatchResetLatched` | the engine body, kept for `R_ResetLatched_Patched` |
| `texture_extension_number_mov_site_*` | patch (7 records, listed below) | `R_RedirectEngineLegacyOpenGLTextureAllocation` | legacy-allocation identities only; each site is decoded and rewritten to `call GL_RedirectedGenTexture` |

The seven texture-allocation patch records are
`texture_extension_number_mov_site_GL_BuildLightmaps`,
`texture_extension_number_mov_site_GL_LoadFilterTexture`,
`texture_extension_number_mov_site_GL_LoadTexture2`,
`texture_extension_number_mov_site_LoadTransPic_bind`,
`texture_extension_number_mov_site_LoadTransPic_increment`,
`texture_extension_number_mov_site_R_InitParticleTexture` and
`texture_extension_number_mov_site_R_Init_playertextures`. `cof-5936` publishes only the two
common records (`..._GL_LoadFilterTexture` and `..._R_InitParticleTexture`); the five
`hl-3248…4554` identities publish those plus the other five.
| `texture_extension_number` | global | `_HasOfficialGLTexAllocSupport` | presence = legacy allocation; absence = official allocation |
| `CVideoMode_Common.m_ImageID`, `CVideoMode_Common.m_iBaseResX`, `CVideoMode_Common.m_iBaseResY` | structMember | `_DrawStartupGraphic` | |
| `CVideoMode_Common_DrawStartupGraphic` | function | `_DrawStartupGraphic` | inline-hooked |
| `CGame_DrawStartupVideo` | function | `_DrawStartupVideo` | resolved and inline-hooked only on HL25 |
| `S_PrecacheSound`, `S_StartDynamicSound`, `S_Update` | function | `R_ResolveStartupVideoSound` | HL25 startup WAV playback; missing symbols skip video without blocking game startup |

## Client symbols

Resolved in `Client_FillAddress` / `Client_FillAddress_SCClient` (`gl_hooks.cpp`) against the
real client module.

| Catalog symbol | Kind | Local | Notes |
| --- | --- | --- | --- |
| `ClientPortalManager_ResetAll` | function | same | resolved only; no hook |
| `ClientPortalManager_GetOriginalSurfaceTexture` | function | same | called by the plugin's portal-surface path |
| `ClientPortalManager_DrawPortalSurface` | function | same | inline-hooked |
| `ClientPortalManager_EnableClipPlane` | function | same | inline-hooked |
| `ClientPortalManager_RenderPortals` | function | same | inline-hooked; records `g_pClientPortalManager` |
| `ClientPortalManager_InitShader` | function | same | inline-hooked |
| `CParticleSystem_ParticleDraw` | function | same | inline-hooked |
| `UpdatePlayerPitch` | function | same | inline-hooked |
| `ClientPortalManager_RenderPortals_to_AngleVectors_callsite_0` | patch | — | redirect to `ClientPortalManager_AngleVectors` |
| `ClientPortalManager.m_bShadersAvailable` | structMember | `offset_ClientPortalManager_m_bShadersAvailable` | |
| `ClientPortalManager_vector_begin_offset`, `ClientPortalManager_vector_end_offset` | scalar | `g_SCClientPortalLayout.vectorBegin` / `.vectorEnd` | |
| `ClientPortal_texture_id_offset`, `ClientPortal_texture_width_offset`, `ClientPortal_texture_height_offset` | scalar | `g_SCClientPortalLayout.textureId` / `.textureWidth` / `.textureHeight` | svencoop-10257 |
| `PortalSource_texture_id_offset`, `PortalSource_texture_width_offset`, `PortalSource_texture_height_offset` | scalar | the same three fields | svencoop-8948 (older source: `PortalSource`) |
| `ClientPortal_entity_offset`, `ClientPortal_mode_offset` | scalar | `g_SCClientPortalLayout.entity/mode` | optional; when present the transform comes from an entity |
| `ClientPortal_origin_offset`, `ClientPortal_angles_offset`, `ClientPortalSource_mode_offset` | scalar | `g_SCClientPortalLayout.origin/angles/mode` | optional; otherwise path |
| `g_bRenderingPortals_SCClient` | global | same | portal-pass gating |
| `g_iFogColor`, `g_iStartDist`, `g_iEndDist` | global | `g_iFogColor_SCClient`, `g_iStartDist_SCClient`, `g_iEndDist_SCClient` | Sven fog params, consumed by `V_CalcRefdef` |
| `g_ViewEntityIndex_SCClient` | global | same | svencoop-10257 only (`IfAvailable`; 8948 leaves it null) |
| `g_PlayerExtraInfo` | global | `g_PlayerExtraInfo` | cstrike / czero |
| `g_PlayerExtraInfo_CZDS` | global | `g_PlayerExtraInfo_CZDS` | czeror |
| `GameStudioRenderer_StudioDrawModel` | virtualFunction | same | resolved only |
| `GameStudioRenderer_StudioDrawPlayer` | virtualFunction | same | inline-hooked |
| `GameStudioRenderer_StudioRenderModel` | virtualFunction | same | inline-hooked |
| `GameStudioRenderer_StudioRenderFinal` | virtualFunction | same | inline-hooked |
| `GameStudioRenderer_StudioSetupBones` | virtualFunction | same | inline-hooked |
| `GameStudioRenderer_StudioSaveBones` | virtualFunction | same | inline-hooked |
| `GameStudioRenderer_StudioMergeBones` | virtualFunction | same | inline-hooked |

The engine-side Studio render pipeline is resolved in the same `ClientStudio_FillAddress` call
against the engine module: `R_StudioDrawModel`, `R_StudioDrawPlayer`, `R_StudioRenderModel`,
`R_StudioRenderFinal`, `R_StudioSetupBones`, `R_StudioMergeBones`, `R_StudioSaveBones`
(all `function`). `R_StudioDrawModel` is resolved only; the other six are inline-hooked.

The client branch is gated: the Sven symbols only resolve inside the `SCClientDLL001` factory
branch, the CS/CZ globals only for the `cstrike` / `czero` / `czeror` game directories.

## Engine `EngineSurface` (VGUI2)

`EngineSurface_FillAddress` (`src/EngineSurfaceHook.cpp:1273`) obtains the engine surface from
`g_pMetaHookAPI->GetEngineFactory()("EngineSurface007")` and the VGUI2 base surface from
`VGUI_Surface026`. The surface methods are consumed **by hardcoded vtable index**, not by name:
`GetVFunctionFromVFTable` reads each slot (`DllInfo` / `RealDllInfo` mirror mapping) into the
`gPrivateFuncs.enginesurface_*` pointers used only for validation. HL25 uses a higher-index
layout than GoldSrc / SvEngine.

- 20 surface methods are `VFTHook`-ed on HL25 and 19 on GoldSrc / SvEngine
  (`drawTexturedRectAdd` exists only in the HL25 layout): `pushMakeCurrent`, `popMakeCurrent`,
  `drawFilledRect`, `drawOutlinedRect`, `drawLine`, `drawPolyLine`, `drawTexturedPolygon`,
  `drawSetTextureRGBA`, `drawSetTexture`, `drawTexturedRect`, `drawTexturedRectAdd`,
  `createNewTextureID`, `drawPrintCharAdd`, `drawSetTextureFile`, `drawGetTextureSize`,
  `isTextureIDValid`, `drawSetSubTextureRGBA`, `drawFlushText`, `drawSetTextureBGRA`,
  `drawUpdateRegionTextureBGRA`. Two edges are deliberate: `pushMakeCurrent` is hooked but its
  slot pointer is not captured (only `index_enginesurface_pushMakeCurrent` is needed), and
  `drawTexturedPolygon` is hooked by index while its field is never populated.
- `offset_enginesurface_drawColor` / `drawTextColor` are hardcoded by engine family
  (SvEngine `4` / `20`, otherwise `8` / `24`), not resolved.
- `VGUI_Surface026::DrawSetTexture` is hooked at hardcoded index 27
  (`index_BaseUISurface_DrawSetTexture`); its wrapper is the only surface hook that calls the
  saved original. `offset_BaseUISurface_m_CurrentTextureId` is hardcoded to `32`.
- Engine globals resolved alongside: `pmainwindow`, `g_bScissor`, `g_ScissorRect`
  (`_EngineSurface_pushMakeCurrent`) and `g_VertexBuffer`, `g_iVertexBufferEntriesUsed`
  (`_EngineSurface_drawFlushText`) — all `global`, all required on every identity.
- `EngineSurface_UninstallHooks` is empty; no surface hook is ever restored.

## Legacy OpenGL redirects and patches

- `R_RedirectEngineLegacyOpenGLCall` (`gl_hooks.cpp`) fans out to
  `R_RedirectEngineLegacyOpenGLTextureAllocation` and `R_RedirectEngineLegacyOpenGLCallAPI`.
- The texture-allocation pass rewrites the seven `texture_extension_number_mov_site_*` PATCH
  sites to `GL_RedirectedGenTexture`. Each site is decoded with Capstone; a site that is not
  `MOV EAX, [mem]` of length ≥ 5 is a fatal misconfiguration rather than a silent truncation.
  The whole pass is skipped when the engine has official allocation support.
- The IAT pass branches by engine type: SvEngine (`opengl32.dll` + `SDL2.dll` imports), SDL
  GoldSrc / HL25 (`SDL2.dll` imports), blob (`kernel32.dll!GetProcAddress` via
  `BlobIATHook`), and non-SDL GoldSrc (`kernel32.dll!GetProcAddress`). It also redirects the
  `qwglCreateContext` patch when present.
- The Sven client import redirect (`R_SCClientRedirectLegacyOpenGLCall` →
  `g_SCClientGLHooks`) hooks nine `opengl32.dll` imports in the client
  (`glEnable`, `glDisable`, `glTexEnvf`, `glColor4f`, `glBegin`, `glEnd`, `glNormal3f`,
  `glClear`, `glCopyTexSubImage2D`) plus the portal `AngleVectors` patch.
- `R_PatchResetLatched` redirects every numbered `CL_LinkPacketEntities_to_R_ResetLatched`
  call site; HL25 is skipped.

## Hooks installed

- **Engine** (`Engine_InstallHooks`, `gl_hooks.cpp:1131`): `GL_Init`;
  `GL_SetModeLegacy` + `GL_SelectPixelFormat` (legacy), `GL_SetMode_GoldSrc`, or
  `GL_SetMode_SvEngine`; `GL_Bind`; `GL_Set2D`; `GL_Finish2D`; `GL_BeginRendering`;
  `GL_EndRendering`; `R_RenderView` / `R_RenderView_SvEngine`; `R_LoadSkys` /
  `R_LoadSkyBox_SvEngine`; `R_ForceCVars`; `R_NewMap`; `Mod_PointInLeaf`; `R_GLStudioDrawPoints`;
  `GL_UnloadTextures`; `GL_LoadFilterTexture`; `GL_LoadTexture2`; `GL_BuildLightmaps`;
  `LegacyMultiTextureInit`; `Mod_LoadStudioModel`; `Mod_LoadSpriteModel`;
  `Mod_UnloadSpriteTextures`; `BuildGammaTable`; `R_CullBox`; `PVSNode`; `Host_ClearMemory`;
  `CVideoMode_Common_DrawStartupGraphic`; `CGame_DrawStartupVideo`; `Draw_Frame`;
  `Draw_SpriteFrame{Holes,Additive,Generic}` and their `_SvEngine` variants; `Draw_FillRGBA`;
  `Draw_FillRGBABlend`; `Draw_FillRGBABuf`; `Draw_Pic`; `D_FillRect`; `R_GetSpriteFrame`.
  Plus the `Sys_ShutdownGame` → `GL_Shutdown` branch redirect and the `pTriAPI` slot
  assignments. `Engine_UninstallHooks` mirrors this list except the branch redirect and the
  `pTriAPI` assignments, which are not reverted.
- **Client** (`Client_InstallHooks`, `gl_hooks.cpp:1589`, Sven only): `ClientPortalManager_InitShader`,
  `CParticleSystem_ParticleDraw`, `ClientPortalManager_DrawPortalSurface`,
  `ClientPortalManager_EnableClipPlane`, `ClientPortalManager_RenderPortals`, `UpdatePlayerPitch`
  (all also uninstalled by `Client_UninstallHooks`); the nine client GL IAT hooks; the deferred
  `SCClientDLL_glewInit()` call. The `AngleVectors` patch is not reverted.
- **Engine Studio** (`EngineStudio_InstalHooks`, `exportfuncs.cpp:140`, called from
  `HUD_GetStudioModelInterface`): `studioapi_GL_SetRenderMode`, `studioapi_SetupRenderer`,
  `studioapi_RestoreRenderer`, `studioapi_StudioDynamicLight`, `studioapi_StudioCheckBBox`,
  `CL_FxBlend`.
- **Client Studio** (`ClientStudio_InstallHooks`, `exportfuncs.cpp:190`): the six
  `GameStudioRenderer_*` hooks above plus the six engine `R_Studio*` hooks.
- **Uninstall asymmetry (present state, not a defect to "fix" silently):**
  `GameStudioRenderer_StudioDrawPlayer` and `R_StudioDrawPlayer` are installed but not
  uninstalled by either uninstall path; `EngineStudio_UninstallHooks` uninstalls the engine
  `R_Studio*` hooks even though they are installed by `ClientStudio_InstallHooks`.
  `EngineSurface_UninstallHooks` is intentionally empty.

## Boundary: public-API-derived pointers (not engine-private)

Stored in `gPrivateFuncs` or externs but sourced from public interfaces, so excluded from the
private inventory:

- `triapi_*` — `gEngfuncs.pTriAPI->*`. Every slot is reassigned to a plugin wrapper in
  `Engine_InstallHooks`; the saved originals are only read for `triapi_Fog`,
  `triapi_FogParams` and `triapi_SpriteTexture`.
- `studioapi_*` — `pstudio->*` (`GL_SetRenderMode`, `SetupRenderer`, `RestoreRenderer`,
  `StudioDynamicLight`, `StudioCheckBBox`), hooked by `EngineStudio_InstalHooks`.
- SDL2 exports (`SDL_GetWindowPosition`, `SDL_GL_SetAttribute`, `SDL_GetWindowSize`,
  `SDL_GL_SwapWindow`, `SDL_GL_GetProcAddress`, `SDL_CreateWindow`,
  `SDL_GL_ExtensionSupported`) — plain `GetProcAddress` on `SDL2.dll`.
- `SvEngine_glewInit` / `SCClientDLL_glewInit` — `GetProcAddress(module, "_glewInit@0")`, gated
  on the `SCEngineClient002` / `SCClientDLL001` factories.
- `pbonetransform`, `plighttransform`, `rotationmatrix` (`pstudio->StudioGetBoneTransform` /
  `StudioGetLightTransform` / `StudioGetRotationMatrix`); `r_smodels_total` / `r_amodels_drawn`
  (`pstudio->GetModelCounters`); `cl_viewent` (`gEngfuncs.GetViewModel`);
  `cl_sprite_white` / `cl_sprite_shell` (`IEngineStudio.Mod_ForName`); `spec_pip` and
  `pmove_10152` (public cvars / the `HUD_PlayerMoveInit` parameter).

## Notes and rules worth keeping

- **The catalog is the address authority.** If a symbol is published, resolve it; do not add a
  scan fallback. The remaining non-gamedata lookups are the VGUI2 vtable indices and the
  client `_glewInit@0` export.
- **Publishing ≠ consuming.** A catalog record is a reason to *re-check* a locator, not a reason
  to wire it into the plugin or the release gate. Symbols published upstream but not consumed
  here stay ungated (e.g. `R_DrawSpriteModel`, `R_DrawWorld`, `R_DrawViewModel`,
  `R_RecursiveWorldNode`, `CGame_DrawStartupVideo`'s siblings, the retired `NET_DrawRect`).
  Conversely, do not add a gate entry for something nothing reads — that converts dead code
  into a release-blocking dependency.
- **Write-through is a consumer; write-only is dead.** `texgammatable`, `lightgammatable`,
  `gl_extensions`, `detTexSupported`, `c_alias_polys` have no in-plugin reads but are written
  through to engine memory the engine then reads, so they stay. A mirror that is only ever
  assigned (never read, never hooked, never used as an anchor) should be deleted together with
  its locator.
- **Name collisions are expected.** The plugin reimplements several engine entry points under
  the same name — `R_DrawWorld`, `R_RenderScene`, `R_SetupGL`, `R_MarkLeaves`,
  `R_DrawBrushModel`, `R_AddTEntity`, `R_RotateForEntity`, `R_DrawSpriteModel`, `R_SetupFrame`,
  `R_StudioGetSkin`, `R_RecursiveWorldNode` — plus `GL_Shutdown` and `R_DrawParticles`. Deleting
  an engine-side field never implies deleting the plugin function of the same name (and vice
  versa); check which one the reference resolves to.
- **`Install_InlineHook(fn)` is a token-paste macro** (`g_phook_##fn`), and `Uninstall_Hook(fn)`
  likewise. Grep `Install_InlineHook(X)` / `Uninstall_Hook(X)` — grepping `g_phook_X` cannot
  prove a hook is unused.
- **Engine-family branches are load-bearing.** The `ENGINE_SVENGINE` / `ENGINE_GOLDSRC_HL25` /
  SDL / legacy splits select catalog names the other identities do not publish (for example
  `gmodinfo_vertical_fov`, `realloc`, `Draw_SpriteFrame*_SvEngine`, `R_LoadSkyBox_SvEngine`,
  `r_missingtexture` on SvEngine; `r_notexture_mip`, `numgltextures`, `R_LoadSkys`, `D_FillRect`
  elsewhere). Do not collapse them.
