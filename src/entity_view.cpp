//Renderer-owned implementation of IEntityViewRenderer (entity_view.h). A view
//renders the caller's entity into the caller's FBO with its own camera and
//light and leaves everything the scene renderer owns exactly as it found it.
//The Studio hooks only answer lighting and keep world-only work (events,
//deferred passes, bounding-box culling) out of a view.
#include "gl_local.h"
#include "entity_view.h"
#include <IEntityViewRenderer.h>
#include <IFileSystem.h>
#include <ScopeExit/ScopeExit.h>

#include <algorithm>
#include <cmath>

namespace
{

//"IDST": the id field of a Studio header. kHalfDegreeToRadians converts the
//half vertical fov to radians (VerticalFov * kHalfDegreeToRadians).
constexpr int   kStudioHeaderId      = 0x54534449;
constexpr float kHalfDegreeToRadians = 0.00872664626f;

//Creates the UBO for one zeroed value when it does not exist yet.
template <class T>
void CreateBuffer(GLuint& Buffer)
{
    if (Buffer)
        return;

    T Data{};
    Buffer = GL_GenBuffer();
    GL_UploadDataToUBODynamicDraw(Buffer, sizeof(Data), &Data);
}

void DeleteBuffer(GLuint& Buffer)
{
    if (Buffer)
    {
        GL_DeleteBuffer(Buffer);
    }

    Buffer = 0;
}

//One renderer value a view replaces: Begin stores the old value and writes the
//new one, End writes the old value back.
template <class T>
class CViewValue
{
public:
    void Begin(T& Target, T NewValue)
    {
        OldValue = Target;
        Target   = NewValue;
    }

    void End(T& Target) const
    {
        Target = OldValue;
    }

private:
    T OldValue{};
};

/*
    Everything one view replaces. Begin() saves and overwrites, End() restores.
    The renderer owns the UBOs, the current render target, the cvar switches
    and the GL state; none of them may leak out of a view.
*/
class CViewScope
{
public:
    bool IsActive() const
    {
        return bBegun;
    }

    bool Begin(const entity_view_t& NewView);
    void End();

    //Copies the light of the active view into *Light.
    bool ApplyLight(alight_t* Light);

    void FreeBuffers();

private:
    static bool IsCameraUsable(const entity_view_t& NewView);
    void        BeginSceneSwitches();
    void        EndSceneSwitches();
    void        SaveRenderState();
    void        EndRenderState();
    void        SaveGLState();
    void        RestoreGLState();
    void        BindViewBuffers();
    void        SetupCamera(const entity_view_t& NewView);
    void        SetViewDrawState();
    void        ClearTarget(const entity_view_t& NewView);

    //The view input while it is active, plus the values the scene had before.
    entity_view_t View{};
    bool          bBegun = false;

    cvar_t* LegacyDLightCvar{};
    cvar_t* LegacyELightCvar{};

    //Private UBOs: the scene UBOs are only generated on map load, and a view
    //must not upload into the scene UBOs while the scene still reads them.
    GLuint CameraBuffer{};
    GLuint SceneBuffer{};
    GLuint LightBuffer{};

    cl_entity_t*     SavedEntity{};
    FBO_Container_t* SavedSceneFBO{};
    FBO_Container_t* SavedRenderingFBO{};
    float            SavedRenderViewport[4]{};

    CViewValue<GLuint> CameraHandle;
    CViewValue<GLuint> SceneHandle;
    CViewValue<GLuint> DLightHandle;

    CViewValue<bool>  DrawOpaque;
    CViewValue<bool>  DrawOitBlend;
    CViewValue<bool>  DrawGBuffer;
    CViewValue<bool>  DrawGammaBlend;
    CViewValue<bool>  FogEnabled;
    CViewValue<bool>  DrawHideEntity;
    CViewValue<bool>  DrawMultiview;
    CViewValue<bool>  DrawShadowView;
    CViewValue<bool>  DrawReflectView;
    CViewValue<bool>  DrawRefractView;
    CViewValue<bool>  DrawLinearDepth;
    CViewValue<bool>  PortalClipPlane;
    CViewValue<int>   DrawClassify;
    CViewValue<float> Blend;
    CViewValue<float> Shadows;
    CViewValue<float> LegacyDLight;
    CViewValue<float> LegacyELight;

    static constexpr GLuint ViewBindingPoints[3] = {
        BINDING_POINT_CAMERA_UBO, BINDING_POINT_SCENE_UBO, BINDING_POINT_DLIGHT_UBO};

    static constexpr GLenum StencilQueries[7] = {
        GL_STENCIL_FUNC, GL_STENCIL_REF, GL_STENCIL_VALUE_MASK, GL_STENCIL_WRITEMASK,
        GL_STENCIL_FAIL, GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS};

    static constexpr GLenum BackStencilQueries[7] = {
        GL_STENCIL_BACK_FUNC, GL_STENCIL_BACK_REF, GL_STENCIL_BACK_VALUE_MASK, GL_STENCIL_BACK_WRITEMASK,
        GL_STENCIL_BACK_FAIL, GL_STENCIL_BACK_PASS_DEPTH_FAIL, GL_STENCIL_BACK_PASS_DEPTH_PASS};

    static constexpr GLenum Capabilities[5] = {
        GL_DEPTH_TEST, GL_CULL_FACE, GL_BLEND, GL_SCISSOR_TEST, GL_STENCIL_TEST};

    GLint     SavedGLViewport[4]{};
    GLint     SavedProgram{};
    GLint     SavedVAO{};
    GLint     SavedActiveTexture{};
    GLint     SavedDepthFunction{};
    GLboolean SavedDepthMask{};
    GLdouble  SavedDepthRange[2]{};
    GLint     SavedCullMode{};
    GLint     SavedBlend[6]{};
    GLboolean SavedColorMask[4]{};
    GLfloat   SavedClearColor[4]{};
    GLdouble  SavedClearDepth{};
    GLint     SavedClearStencil{};
    GLint     SavedArrayBuffer{};
    GLint     SavedTextures[7][2]{};
    GLboolean SavedClipEnabled[6]{};
    GLint     SavedStencil[2][7]{};
    GLboolean SavedCapabilities[5]{};
    GLint     SavedUniformBuffer{};
    GLint     SavedUniformBindings[3]{};
    GLint64   SavedUniformOffsets[3]{};
    GLint64   SavedUniformSizes[3]{};
};

CViewScope ViewScope;

bool CViewScope::IsCameraUsable(const entity_view_t& NewView)
{
    return std::isfinite(NewView.verticalFov) && NewView.verticalFov > 1 && NewView.verticalFov < 179 &&
        std::isfinite(NewView.nearPlane) && NewView.nearPlane > 0 &&
        std::isfinite(NewView.farPlane) && NewView.farPlane > NewView.nearPlane;
}

bool CViewScope::Begin(const entity_view_t& NewView)
{
    if (bBegun || g_bIsRenderingView || !NewView.target ||
        !NewView.target->s_hBackBufferFBO || NewView.target->iWidth < 1 || NewView.target->iHeight < 1 ||
        !IsCameraUsable(NewView))
        return false;

    //Both are Renderer-owned cvars; a view cannot control the light without them.
    LegacyDLightCvar = gEngfuncs.pfnGetCvarPointer("r_studio_legacy_dlight");
    LegacyELightCvar = gEngfuncs.pfnGetCvarPointer("r_studio_legacy_elight");

    if (!LegacyDLightCvar || !LegacyELightCvar)
        return false;

    View   = NewView;
    bBegun = true;

    //From here on every failure path restores through End().
    BeginSceneSwitches();
    SaveRenderState();
    SaveGLState();
    BindViewBuffers();

    GL_PushFrameBuffer();
    R_PushRefDef();
    R_PushWorldMatrix();
    R_PushProjectionMatrix();

    GL_BindFrameBuffer(View.target);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        End();
        return false;
    }

    GL_SetCurrentSceneFBO(View.target);

    SetupCamera(View);
    SetViewDrawState();
    ClearTarget(View);

    return true;
}

void CViewScope::End()
{
    if (!bBegun)
        return;

    bBegun = false;

    R_PopProjectionMatrix();
    R_PopWorldMatrix();
    R_PopRefDef();
    R_UpdateRefDef();

    *currententity = SavedEntity;

    GL_SetCurrentSceneFBO(SavedSceneFBO);

    //GL_PopFrameBuffer only restores the raw READ/DRAW bindings; the renderer's
    //current rendering FBO is restored the same way SCClientFramebufferScope does.
    GL_BindFrameBuffer(SavedRenderingFBO);
    GL_PopFrameBuffer();

    RestoreGLState();

    EndRenderState();
    EndSceneSwitches();
}

bool CViewScope::ApplyLight(alight_t* Light)
{
    if (!bBegun)
        return false;

    //The engine keeps ambientlight and shadelight inside these limits whenever
    //it samples its own light.
    Light->ambientlight = std::clamp(View.light.ambientlight, 0, 128);
    Light->shadelight   = std::clamp(View.light.shadelight, 0, 255 - Light->ambientlight);

    for (int i = 0; i < 3; ++i)
    {
        Light->color[i] = std::clamp(View.light.color[i], 0.0f, 1.0f);
    }

    //A zero-length direction would divide by zero while normalizing.
    VectorCopy(View.light.direction, Light->plightvec);

    if (!(VectorLength(Light->plightvec) > 0.0001f))
    {
        Light->plightvec[0] = 0;
        Light->plightvec[1] = 0;
        Light->plightvec[2] = -1;
    }

    VectorNormalize(Light->plightvec);

    return true;
}

void CViewScope::FreeBuffers()
{
    DeleteBuffer(CameraBuffer);
    DeleteBuffer(SceneBuffer);
    DeleteBuffer(LightBuffer);
}

void CViewScope::BeginSceneSwitches()
{
    //A view is not the scene: no opaque pass, no G-buffer or OIT, no fog, no
    //shadows, no multiview, no portal clipping and no entity hiding. Gamma
    //blending stays on: the caller samples the RGBA8 target directly, without
    //the scene's linear-to-gamma postprocess.
    DrawOpaque.Begin(r_draw_opaque, false);
    DrawOitBlend.Begin(r_draw_oitblend, false);
    DrawGBuffer.Begin(r_draw_gbuffer, false);
    DrawGammaBlend.Begin(r_draw_gammablend, true);
    FogEnabled.Begin(r_fog_enabled, false);
    DrawHideEntity.Begin(r_draw_hide_entity, false);
    DrawMultiview.Begin(r_draw_multiview, false);
    DrawShadowView.Begin(r_draw_shadowview, false);
    DrawReflectView.Begin(r_draw_reflectview, false);
    DrawRefractView.Begin(r_draw_refractview, false);
    DrawLinearDepth.Begin(r_draw_lineardepth, false);
    PortalClipPlane.Begin(g_bPortalClipPlaneEnabled[0], false);
    DrawClassify.Begin(r_draw_classify, DRAW_CLASSIFY_OPAQUE_ENTITIES | DRAW_CLASSIFY_TRANS_ENTITIES);
    Blend.Begin(*r_blend, 1.0f);
    Shadows.Begin(r_shadows->value, 0.0f);
    //The view light replaces the engine's light sampling, so the legacy light
    //switches stay off while the view draws.
    LegacyDLight.Begin(LegacyDLightCvar->value, 0.0f);
    LegacyELight.Begin(LegacyELightCvar->value, 0.0f);
}

void CViewScope::EndSceneSwitches()
{
    LegacyELight.End(LegacyELightCvar->value);
    LegacyDLight.End(LegacyDLightCvar->value);
    Shadows.End(r_shadows->value);
    Blend.End(*r_blend);
    DrawClassify.End(r_draw_classify);
    PortalClipPlane.End(g_bPortalClipPlaneEnabled[0]);
    DrawLinearDepth.End(r_draw_lineardepth);
    DrawRefractView.End(r_draw_refractview);
    DrawReflectView.End(r_draw_reflectview);
    DrawShadowView.End(r_draw_shadowview);
    DrawMultiview.End(r_draw_multiview);
    DrawHideEntity.End(r_draw_hide_entity);
    FogEnabled.End(r_fog_enabled);
    DrawGammaBlend.End(r_draw_gammablend);
    DrawGBuffer.End(r_draw_gbuffer);
    DrawOitBlend.End(r_draw_oitblend);
    DrawOpaque.End(r_draw_opaque);
}

void CViewScope::SaveRenderState()
{
    SavedEntity       = *currententity;
    SavedSceneFBO     = GL_GetCurrentSceneFBO();
    SavedRenderingFBO = GL_GetCurrentRenderingFBO();

    CameraHandle.Begin(g_WorldSurfaceRenderer.hCameraUBO, CameraBuffer);
    SceneHandle.Begin(g_WorldSurfaceRenderer.hSceneUBO, SceneBuffer);
    DLightHandle.Begin(g_WorldSurfaceRenderer.hDLightUBO, LightBuffer);
}

void CViewScope::EndRenderState()
{
    DLightHandle.End(g_WorldSurfaceRenderer.hDLightUBO);
    SceneHandle.End(g_WorldSurfaceRenderer.hSceneUBO);
    CameraHandle.End(g_WorldSurfaceRenderer.hCameraUBO);
}

void CViewScope::SaveGLState()
{
    glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &SavedUniformBuffer);

    for (int i = 0; i < 3; ++i)
    {
        glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, ViewBindingPoints[i], &SavedUniformBindings[i]);
        glGetInteger64i_v(GL_UNIFORM_BUFFER_START, ViewBindingPoints[i], &SavedUniformOffsets[i]);
        glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, ViewBindingPoints[i], &SavedUniformSizes[i]);
    }

    glGetIntegerv(GL_VIEWPORT, SavedGLViewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &SavedProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &SavedVAO);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &SavedActiveTexture);
    glGetIntegerv(GL_DEPTH_FUNC, &SavedDepthFunction);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &SavedDepthMask);
    glGetDoublev(GL_DEPTH_RANGE, SavedDepthRange);
    glGetIntegerv(GL_CULL_FACE_MODE, &SavedCullMode);
    glGetIntegerv(GL_BLEND_SRC_RGB, &SavedBlend[0]);
    glGetIntegerv(GL_BLEND_DST_RGB, &SavedBlend[1]);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &SavedBlend[2]);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &SavedBlend[3]);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &SavedBlend[4]);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &SavedBlend[5]);
    glGetBooleanv(GL_COLOR_WRITEMASK, SavedColorMask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, SavedClearColor);
    glGetDoublev(GL_DEPTH_CLEAR_VALUE, &SavedClearDepth);
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &SavedClearStencil);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &SavedArrayBuffer);

    for (int i = 0; i < 7; ++i)
    {
        glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &SavedTextures[i][0]);
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &SavedTextures[i][1]);
    }

    glActiveTexture(SavedActiveTexture);

    for (int i = 0; i < 6; ++i)
    {
        SavedClipEnabled[i] = glIsEnabled(GL_CLIP_DISTANCE0 + i);
        glDisable(GL_CLIP_DISTANCE0 + i);
    }

    for (int i = 0; i < 7; ++i)
    {
        glGetIntegerv(StencilQueries[i], &SavedStencil[0][i]);
        glGetIntegerv(BackStencilQueries[i], &SavedStencil[1][i]);
    }

    for (int i = 0; i < 5; ++i)
    {
        SavedCapabilities[i] = glIsEnabled(Capabilities[i]);
    }

    memcpy(SavedRenderViewport, r_viewport, sizeof(SavedRenderViewport));
}

void CViewScope::RestoreGLState()
{
    memcpy(r_viewport, SavedRenderViewport, sizeof(SavedRenderViewport));
    glViewport(SavedGLViewport[0], SavedGLViewport[1], SavedGLViewport[2], SavedGLViewport[3]);

    glDepthFunc(SavedDepthFunction);
    glDepthMask(SavedDepthMask);
    glDepthRange(SavedDepthRange[0], SavedDepthRange[1]);
    glCullFace(SavedCullMode);
    glBlendFuncSeparate(SavedBlend[0], SavedBlend[1], SavedBlend[2], SavedBlend[3]);
    glBlendEquationSeparate(SavedBlend[4], SavedBlend[5]);
    glColorMask(SavedColorMask[0], SavedColorMask[1], SavedColorMask[2], SavedColorMask[3]);
    glClearColor(SavedClearColor[0], SavedClearColor[1], SavedClearColor[2], SavedClearColor[3]);
    glClearDepth(SavedClearDepth);
    glClearStencil(SavedClearStencil);

    for (int i = 0; i < 2; ++i)
    {
        const GLenum Face = i == 0 ? GL_FRONT : GL_BACK;

        glStencilFuncSeparate(Face, SavedStencil[i][0], SavedStencil[i][1], SavedStencil[i][2]);
        glStencilMaskSeparate(Face, SavedStencil[i][3]);
        glStencilOpSeparate(Face, SavedStencil[i][4], SavedStencil[i][5], SavedStencil[i][6]);
    }

    for (int i = 0; i < 6; ++i)
    {
        if (SavedClipEnabled[i])
            glEnable(GL_CLIP_DISTANCE0 + i);
        else
            glDisable(GL_CLIP_DISTANCE0 + i);
    }

    for (int i = 0; i < 5; ++i)
    {
        if (SavedCapabilities[i])
            glEnable(Capabilities[i]);
        else
            glDisable(Capabilities[i]);
    }

    GL_UseProgram(SavedProgram);
    GL_BindVAO(SavedVAO);
    glBindBuffer(GL_ARRAY_BUFFER, SavedArrayBuffer);

    for (int i = 0; i < 7; ++i)
    {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, SavedTextures[i][0]);
        glBindTexture(GL_TEXTURE_2D_ARRAY, SavedTextures[i][1]);
    }

    glActiveTexture(SavedActiveTexture);

    for (int i = 0; i < 3; ++i)
    {
        if (SavedUniformBindings[i] && SavedUniformSizes[i])
        {
            glBindBufferRange(GL_UNIFORM_BUFFER, ViewBindingPoints[i], SavedUniformBindings[i],
                              GLintptr(SavedUniformOffsets[i]), GLsizeiptr(SavedUniformSizes[i]));
        }
        else
        {
            glBindBufferBase(GL_UNIFORM_BUFFER, ViewBindingPoints[i], SavedUniformBindings[i]);
        }
    }

    glBindBuffer(GL_UNIFORM_BUFFER, SavedUniformBuffer);

    //The Studio renderer binds its textures directly, so the engine's texture
    //cache must not believe its own binding is still current.
    *currenttexture = -1;
}

void CViewScope::BindViewBuffers()
{
    CreateBuffer<camera_ubo_t>(CameraBuffer);
    CreateBuffer<scene_ubo_t>(SceneBuffer);
    CreateBuffer<dlight_ubo_t>(LightBuffer);

    const GLuint Buffers[3] = {CameraBuffer, SceneBuffer, LightBuffer};

    for (int i = 0; i < 3; ++i)
    {
        glBindBufferBase(GL_UNIFORM_BUFFER, ViewBindingPoints[i], Buffers[i]);
    }
}

void CViewScope::SetupCamera(const entity_view_t& NewView)
{
    VectorCopy(NewView.origin, *r_refdef.vieworg);
    VectorCopy(NewView.angles, *r_refdef.viewangles);
    R_UpdateRefDef();
    R_LoadIdentityForWorldMatrix();
    R_SetupPlayerViewWorldMatrix(NewView.origin, NewView.angles);

    const float Aspect     = float(NewView.target->iWidth) / float(NewView.target->iHeight);
    const float HalfHeight = NewView.nearPlane * tanf(NewView.verticalFov * kHalfDegreeToRadians);

    R_LoadIdentityForProjectionMatrix();
    R_SetupFrustumProjectionMatrix(-HalfHeight * Aspect, HalfHeight * Aspect,
                                   -HalfHeight, HalfHeight, NewView.nearPlane, NewView.farPlane);
    R_SetViewport(0, 0, float(NewView.target->iWidth), float(NewView.target->iHeight));

    camera_ubo_t Camera{};

    R_SetupCameraView(&Camera.views[0]);
    InvertMatrix(&Camera.views[0].worldMatrix[0][0], &Camera.views[0].invWorldMatrix[0][0]);
    InvertMatrix(&Camera.views[0].projMatrix[0][0], &Camera.views[0].invProjMatrix[0][0]);
    Camera.views[0].vright_znear[3] = NewView.nearPlane;
    Camera.views[0].vup_zfar[3]     = NewView.farPlane;
    Camera.numViews                 = 1;

    R_UploadCameraUBOData(&Camera);
    R_UploadSceneUBO();
}

void CViewScope::SetViewDrawState()
{
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glDepthRange(0, 1);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
}

void CViewScope::ClearTarget(const entity_view_t& NewView)
{
    //The caller composites the target directly, so the background has to
    //follow the premultiplied convention as well.
    vec4_t Background;

    memcpy(Background, NewView.background, sizeof(Background));

    for (int i = 0; i < 3; ++i)
    {
        Background[i] *= Background[3];
    }

    GL_ClearColorDepthStencil(Background, 1, 0, STENCIL_MASK_ALL);
}

//SvEngine substitutes models/error.mdl for missing files even with
//crash_if_missing = false, so the mounted engine filesystem is probed first.
bool HasStudioFile(const char* Path)
{
    FileHandle_t File = FILESYSTEM_ANY_OPEN(Path, "rb");

    if (!File)
        return false;

    SCOPE_EXIT { FILESYSTEM_ANY_CLOSE(File); };

    studiohdr_t    Header{};
    const unsigned Size = FILESYSTEM_ANY_SIZE(File);
    const int      Read = FILESYSTEM_ANY_READ(&Header, sizeof(Header), File);

    return Read == sizeof(Header) && Header.id == kStudioHeaderId &&
        Header.version == STUDIO_VERSION && Header.length >= sizeof(Header) &&
        static_cast<unsigned>(Header.length) <= Size;
}

hook_t* EntityLightHook                = nullptr;
void (*OriginalEntityLight)(alight_t*) = nullptr;

//The engine's entity light routine repositions the game's live elights onto
//the entity; a view supplies its own light instead.
void StudioEntityLightHook(alight_t* Light)
{
    if (ViewScope.IsActive())
        return;

    OriginalEntityLight(Light);
}

} // namespace

/*
    The Renderer's IEntityViewRenderer: the scope plus the engine model
    helpers. What a caller shows is the caller's own composition.
*/
class CEntityViewRenderer : public IEntityViewRenderer
{
public:
    model_s* LoadStudioModel(const char* Path) override
    {
        if (!Path || !*Path || !IEngineStudio.Mod_ForName)
            return nullptr;

        if (!HasStudioFile(Path))
            return nullptr;

        auto* Model = IEngineStudio.Mod_ForName(Path, false);

        if (!Model || Model->type != mod_studio || stricmp(Model->name, Path) != 0)
            return nullptr;

        return Model;
    }

    void* GetModelData(model_s* Model) override
    {
        return Model && IEngineStudio.Mod_Extradata ? IEngineStudio.Mod_Extradata(Model) : nullptr;
    }

    bool BeginView(const entity_view_t& View) override
    {
        return ViewScope.Begin(View);
    }

    void EndView() override
    {
        ViewScope.End();
    }

    bool HasWorld() const override
    {
        return cl_worldmodel && *cl_worldmodel;
    }
};

bool EntityView_IsActive()
{
    return ViewScope.IsActive();
}

bool EntityView_ApplyLight(alight_t* Light)
{
    return ViewScope.ApplyLight(Light);
}

void EntityView_InstallHooks(engine_studio_api_t* Studio)
{
    if (!EntityLightHook && Studio && Studio->StudioEntityLight)
    {
        EntityLightHook = g_pMetaHookAPI->InlineHook(
            Studio->StudioEntityLight, StudioEntityLightHook, (void**)&OriginalEntityLight);
    }
}

void EntityView_UninstallHooks()
{
    if (EntityLightHook)
        g_pMetaHookAPI->UnHook(EntityLightHook);

    EntityLightHook     = nullptr;
    OriginalEntityLight = nullptr;
}

void EntityView_FreeBuffers()
{
    ViewScope.FreeBuffers();
}

EXPOSE_SINGLE_INTERFACE(CEntityViewRenderer, IEntityViewRenderer, ENTITY_VIEW_RENDERER_INTERFACE_VERSION);
