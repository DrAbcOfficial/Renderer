// New implementation: world-independent previews through the existing Studio
// renderer. ModelPreview.h is the hook seam; IModelPreviewRenderer is the UI seam.
#include "gl_local.h"
#include "ModelPreview.h"
#include <IModelPreviewRenderer.h>
#include <IFileSystem.h>

#include <algorithm>
#include <cmath>

namespace modelpreview {
namespace {
constexpr int kStudioHeaderId = 0x54534449; // "IDST"
const ModelPreviewView* ActiveView = nullptr;
void (*OriginalEntityLight)(alight_t*) = nullptr;
hook_t* EntityLightHook = nullptr;
GLuint CameraBuffer = 0, SceneBuffer = 0, LightBuffer = 0;
#ifndef NDEBUG
int DrawCalls = 0, LitCalls = 0, ModelCounter = 0;
int VaoReady = 0, Meshes = 0;
unsigned int LastError = 0;
float BoneOrigin[3]{};
void status() {
    gEngfuncs.Con_Printf("[Renderer preview] draws=%d lights=%d models=%d vao=%d meshes=%d error=%x bone=%g,%g,%g\n",
        DrawCalls, LitCalls, ModelCounter, VaoReady, Meshes, LastError,
        BoneOrigin[0], BoneOrigin[1], BoneOrigin[2]);
}
#endif

void entityLight(alight_t* Light) {
    // The engine's entity-light routine can mutate live game elights.
    if (!ActiveView)
        OriginalEntityLight(Light);
}

template <class FileSystem>
bool hasStudioFile(FileSystem* Files, const char* Path) {
    if (!Files) return false;
    const auto File = Files->Open(Path, "rb");
    if (!File) return false;
    studiohdr_t Header{};
    const unsigned Size = Files->Size(File);
    const int Read = Files->Read(&Header, sizeof(Header), File);
    Files->Close(File);
    return Read == sizeof(Header) && Header.id == kStudioHeaderId &&
        Header.version == STUDIO_VERSION && Header.length >= sizeof(Header) &&
        static_cast<unsigned>(Header.length) <= Size;
}

template <class T> class SavedValue {
public:
    SavedValue(T& Target, T Value) : Target(Target), Saved(Target) { Target = Value; }
    ~SavedValue() { Target = Saved; }
private:
    T& Target;
    T Saved;
};

template <class T> void createBuffer(GLuint& Buffer) {
    if (Buffer) return;
    T Data{};
    Buffer = GL_GenBuffer();
    GL_UploadDataToUBODynamicDraw(Buffer, sizeof(Data), &Data);
}

class PreviewState {
public:
    PreviewState()
        : Entity(*currententity),
          SceneFbo(GL_GetCurrentSceneFBO()) {
        glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &UniformBuffer);
        for (int i = 0; i < 3; ++i) {
            glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, Bindings[i], &UniformBindings[i]);
            glGetInteger64i_v(GL_UNIFORM_BUFFER_START, Bindings[i], &UniformOffsets[i]);
            glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, Bindings[i], &UniformSizes[i]);
        }
        createBuffer<camera_ubo_t>(CameraBuffer);
        createBuffer<scene_ubo_t>(SceneBuffer);
        createBuffer<dlight_ubo_t>(LightBuffer);
        glBindBufferBase(GL_UNIFORM_BUFFER, Bindings[0], CameraBuffer);
        glBindBufferBase(GL_UNIFORM_BUFFER, Bindings[1], SceneBuffer);
        glBindBufferBase(GL_UNIFORM_BUFFER, Bindings[2], LightBuffer);
        glGetIntegerv(GL_VIEWPORT, Viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &Program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &Vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &ActiveTexture);
        glGetIntegerv(GL_DEPTH_FUNC, &DepthFunction);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &DepthMask);
        glGetDoublev(GL_DEPTH_RANGE, DepthRange);
        glGetIntegerv(GL_CULL_FACE_MODE, &CullMode);
        glGetIntegerv(GL_BLEND_SRC_RGB, &Blend[0]);
        glGetIntegerv(GL_BLEND_DST_RGB, &Blend[1]);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &Blend[2]);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &Blend[3]);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &Blend[4]);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &Blend[5]);
        glGetBooleanv(GL_COLOR_WRITEMASK, ColorMask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, ClearColor);
        glGetDoublev(GL_DEPTH_CLEAR_VALUE, &ClearDepth);
        glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &ClearStencil);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ArrayBuffer);
        for (int i = 0; i < 7; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &Textures[i][0]);
            glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &Textures[i][1]);
        }
        glActiveTexture(ActiveTexture);
        for (int i = 0; i < 6; ++i) {
            ClipEnabled[i] = glIsEnabled(GL_CLIP_DISTANCE0 + i);
            glDisable(GL_CLIP_DISTANCE0 + i);
        }
        for (int i = 0; i < 7; ++i) {
            glGetIntegerv(StencilQueries[i], &Stencil[0][i]);
            glGetIntegerv(BackStencilQueries[i], &Stencil[1][i]);
        }
        for (int i = 0; i < 5; ++i)
            Enabled[i] = glIsEnabled(Capabilities[i]);
        memcpy(RenderViewport, r_viewport, sizeof(RenderViewport));
        GL_PushFrameBuffer();
        R_PushRefDef();
        R_PushWorldMatrix();
        R_PushProjectionMatrix();
    }
    ~PreviewState() {
        R_PopProjectionMatrix();
        R_PopWorldMatrix();
        R_PopRefDef();
        R_UpdateRefDef();
        *currententity = Entity;
        GL_SetCurrentSceneFBO(SceneFbo);
        GL_PopFrameBuffer();
        memcpy(r_viewport, RenderViewport, sizeof(RenderViewport));
        glViewport(Viewport[0], Viewport[1], Viewport[2], Viewport[3]);
        glDepthFunc(DepthFunction);
        glDepthMask(DepthMask);
        glDepthRange(DepthRange[0], DepthRange[1]);
        glCullFace(CullMode);
        glBlendFuncSeparate(Blend[0], Blend[1], Blend[2], Blend[3]);
        glBlendEquationSeparate(Blend[4], Blend[5]);
        glColorMask(ColorMask[0], ColorMask[1], ColorMask[2], ColorMask[3]);
        glClearColor(ClearColor[0], ClearColor[1], ClearColor[2], ClearColor[3]);
        glClearDepth(ClearDepth);
        glClearStencil(ClearStencil);
        for (int i = 0; i < 2; ++i) {
            const auto Face = i == 0 ? GL_FRONT : GL_BACK;
            glStencilFuncSeparate(Face, Stencil[i][0], Stencil[i][1], Stencil[i][2]);
            glStencilMaskSeparate(Face, Stencil[i][3]);
            glStencilOpSeparate(Face, Stencil[i][4], Stencil[i][5], Stencil[i][6]);
        }
        for (int i = 0; i < 6; ++i) {
            if (ClipEnabled[i]) glEnable(GL_CLIP_DISTANCE0 + i);
            else glDisable(GL_CLIP_DISTANCE0 + i);
        }
        for (int i = 0; i < 5; ++i) {
            if (Enabled[i]) glEnable(Capabilities[i]);
            else glDisable(Capabilities[i]);
        }
        GL_UseProgram(Program);
        GL_BindVAO(Vao);
        glBindBuffer(GL_ARRAY_BUFFER, ArrayBuffer);
        for (int i = 0; i < 7; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, Textures[i][0]);
            glBindTexture(GL_TEXTURE_2D_ARRAY, Textures[i][1]);
        }
        glActiveTexture(ActiveTexture);
        for (int i = 0; i < 3; ++i) {
            if (UniformBindings[i] && UniformSizes[i])
                glBindBufferRange(GL_UNIFORM_BUFFER, Bindings[i], UniformBindings[i],
                    GLintptr(UniformOffsets[i]), GLsizeiptr(UniformSizes[i]));
            else
                glBindBufferBase(GL_UNIFORM_BUFFER, Bindings[i], UniformBindings[i]);
        }
        glBindBuffer(GL_UNIFORM_BUFFER, UniformBuffer);
        // Studio binds textures directly; do not leave the engine cache stale.
        *currenttexture = -1;
    }
private:
    GLint UniformBuffer{}, UniformBindings[3]{};
    GLint64 UniformOffsets[3]{}, UniformSizes[3]{};
    static constexpr GLuint Bindings[3] = {
        BINDING_POINT_CAMERA_UBO, BINDING_POINT_SCENE_UBO, BINDING_POINT_DLIGHT_UBO};
    cl_entity_t* Entity;
    FBO_Container_t* SceneFbo;
    GLint Viewport[4]{}, Program{}, Vao{}, ActiveTexture{}, DepthFunction{}, CullMode{};
    GLfloat RenderViewport[4]{};
    GLdouble DepthRange[2]{};
    GLboolean DepthMask{}, Enabled[5]{};
    GLint Blend[6]{}, Textures[7][2]{}, Stencil[2][7]{}, ClearStencil{}, ArrayBuffer{};
    GLboolean ColorMask[4]{}, ClipEnabled[6]{};
    GLfloat ClearColor[4]{};
    GLdouble ClearDepth{};
    static constexpr GLenum StencilQueries[7] = {
        GL_STENCIL_FUNC, GL_STENCIL_REF, GL_STENCIL_VALUE_MASK, GL_STENCIL_WRITEMASK,
        GL_STENCIL_FAIL, GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS};
    static constexpr GLenum BackStencilQueries[7] = {
        GL_STENCIL_BACK_FUNC, GL_STENCIL_BACK_REF, GL_STENCIL_BACK_VALUE_MASK, GL_STENCIL_BACK_WRITEMASK,
        GL_STENCIL_BACK_FAIL, GL_STENCIL_BACK_PASS_DEPTH_FAIL, GL_STENCIL_BACK_PASS_DEPTH_PASS};
    static constexpr GLenum Capabilities[5] = {
        GL_DEPTH_TEST, GL_CULL_FACE, GL_BLEND, GL_SCISSOR_TEST, GL_STENCIL_TEST};
};

class ModelPreviewRenderer : public IModelPreviewRenderer {
public:
    model_t* loadModel(const char* Path) override {
        if (!Path || !*Path || !IEngineStudio.Mod_ForName)
            return nullptr;
        // SvEngine substitutes models/error.mdl for missing files, even with
        // crash=false. Validate through the mounted engine filesystem first.
        if (!(g_pFileSystem ? hasStudioFile(g_pFileSystem, Path) :
              hasStudioFile(g_pFileSystem_HL25, Path)))
            return nullptr;
        auto* Model = IEngineStudio.Mod_ForName(Path, false);
        return Model && Model->type == mod_studio && !_stricmp(Model->name, Path) ? Model : nullptr;
    }
    void* modelData(model_t* Model) override {
        return Model && IEngineStudio.Mod_Extradata ? IEngineStudio.Mod_Extradata(Model) : nullptr;
    }
    bool hasWorld() const override { return cl_worldmodel && *cl_worldmodel; }
    bool render(cl_entity_t* Entity, FBO_Container_t* Target,
                const ModelPreviewView& View) override {
        if (ActiveView || g_bIsRenderingView || !gpStudioInterface ||
            !*gpStudioInterface || !EntityLightHook || !Entity || !Entity->model ||
            Entity->model->type != mod_studio || Entity->index != 0 || Entity->player ||
            Entity->curstate.movetype == MOVETYPE_FOLLOW || !Target ||
            !Target->s_hBackBufferFBO || Target->iWidth < 1 || Target->iHeight < 1 ||
            !std::isfinite(View.VerticalFov) || View.VerticalFov <= 1 ||
            View.VerticalFov >= 179 || View.NearPlane <= 0 ||
            View.FarPlane <= View.NearPlane)
            return false;

        PreviewState State;
        // R_NewMap normally allocates scene UBOs. Use private preview buffers
        // instead: no map load, no large world/decal/OIT allocations, no
        // overwriting the main view's UBOs or synchronous GPU readback.
        SavedValue CameraHandle(g_WorldSurfaceRenderer.hCameraUBO, CameraBuffer);
        SavedValue SceneHandle(g_WorldSurfaceRenderer.hSceneUBO, SceneBuffer);
        SavedValue Active(ActiveView, &View);
        SavedValue Opaque(r_draw_opaque, false);
        SavedValue Oit(r_draw_oitblend, false);
        SavedValue GBuffer(r_draw_gbuffer, false);
        // The UI samples this RGBA8 target directly, without the scene's
        // linear-to-gamma postprocess.
        SavedValue Gamma(r_draw_gammablend, true);
        SavedValue Fog(r_fog_enabled, false);
        SavedValue Hide(r_draw_hide_entity, false);
        SavedValue Multiview(r_draw_multiview, false);
        SavedValue ShadowView(r_draw_shadowview, false);
        SavedValue ReflectView(r_draw_reflectview, false);
        SavedValue RefractView(r_draw_refractview, false);
        SavedValue LinearDepth(r_draw_lineardepth, false);
        SavedValue PortalClip(g_bPortalClipPlaneEnabled[0], false);
        SavedValue Classify(r_draw_classify, DRAW_CLASSIFY_OPAQUE_ENTITIES | DRAW_CLASSIFY_TRANS_ENTITIES);
        SavedValue Blend(*r_blend, 1.0f);
        SavedValue Shadows(r_shadows->value, 0.0f);
        auto* DlightCvar = gEngfuncs.pfnGetCvarPointer("r_studio_legacy_dlight");
        auto* ElightCvar = gEngfuncs.pfnGetCvarPointer("r_studio_legacy_elight");
        SavedValue Dlights(DlightCvar->value, 0.0f);
        SavedValue Elights(ElightCvar->value, 0.0f);

        GL_BindFrameBuffer(Target);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return false;
        GL_SetCurrentSceneFBO(Target);
        VectorCopy(View.Origin, *r_refdef.vieworg);
        VectorCopy(View.Angles, *r_refdef.viewangles);
        R_UpdateRefDef();
        R_LoadIdentityForWorldMatrix();
        R_SetupPlayerViewWorldMatrix(View.Origin, View.Angles);
        const float Aspect = float(Target->iWidth) / float(Target->iHeight);
        const float HalfHeight = View.NearPlane * tanf(View.VerticalFov * 0.00872664626f);
        R_LoadIdentityForProjectionMatrix();
        R_SetupFrustumProjectionMatrix(-HalfHeight * Aspect, HalfHeight * Aspect,
            -HalfHeight, HalfHeight, View.NearPlane, View.FarPlane);
        R_SetViewport(0, 0, float(Target->iWidth), float(Target->iHeight));
        camera_ubo_t Camera{};
        R_SetupCameraView(&Camera.views[0]);
        InvertMatrix(&Camera.views[0].worldMatrix[0][0], &Camera.views[0].invWorldMatrix[0][0]);
        InvertMatrix(&Camera.views[0].projMatrix[0][0], &Camera.views[0].invProjMatrix[0][0]);
        Camera.views[0].vright_znear[3] = View.NearPlane;
        Camera.views[0].vup_zfar[3] = View.FarPlane;
        Camera.numViews = 1;
        R_UploadCameraUBOData(&Camera);
        R_UploadSceneUBO();
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LEQUAL);
        glDepthRange(0, 1);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        float Background[4];
        memcpy(Background, View.Background, sizeof(Background));
        for (int i = 0; i < 3; ++i)
            Background[i] *= Background[3];
        GL_ClearColorDepthStencil(Background, 1, 0, STENCIL_MASK_ALL);
        *currententity = Entity;
        // Same implementation behind IMetaRenderer::DrawCurrentEntity. No
        // world render, fabricated BSP, or independent model pipeline.
        R_DrawCurrentEntity(Entity->curstate.rendermode != kRenderNormal);
#ifndef NDEBUG
        ++DrawCalls;
        ModelCounter = *r_amodels_drawn;
        LastError = glGetError();
        auto Data = R_GetStudioRenderDataFromModel(Entity->model);
        VaoReady = Data && Data->hVAO;
        Meshes = *c_alias_polys;
        for (int i = 0; i < 3; ++i)
            BoneOrigin[i] = (*pbonetransform)[0][i][3];
#endif
        return true;
    }
};
}

bool isActive() { return ActiveView != nullptr; }

bool setupLight(alight_t* Light) {
    if (!ActiveView)
        return false;
#ifndef NDEBUG
    ++LitCalls;
#endif
    Light->ambientlight = ActiveView->Lighting ? std::clamp(ActiveView->Ambient, 0, 128) : 192;
    Light->shadelight = ActiveView->Lighting ? std::clamp(ActiveView->Shade, 0, 255 - Light->ambientlight) : 0;
    for (int i = 0; i < 3; ++i)
        Light->color[i] = ActiveView->Lighting ? ActiveView->LightColor[i] : 1.0f;
    VectorCopy(ActiveView->LightDirection, Light->plightvec);
    VectorNormalize(Light->plightvec);
    return true;
}

void install(engine_studio_api_t* Studio) {
#ifndef NDEBUG
    static bool Registered = false;
    if (!Registered) {
        gEngfuncs.pfnAddCommand("r_modelpreview_status", status);
        Registered = true;
    }
#endif
    if (!EntityLightHook)
        EntityLightHook = g_pMetaHookAPI->InlineHook(
            Studio->StudioEntityLight, entityLight, (void**)&OriginalEntityLight);
}

void uninstall() {
    if (EntityLightHook) g_pMetaHookAPI->UnHook(EntityLightHook);
    EntityLightHook = nullptr;
    OriginalEntityLight = nullptr;
}

void freeBuffers() {
    for (auto* Buffer : {&CameraBuffer, &SceneBuffer, &LightBuffer}) {
        if (*Buffer) GL_DeleteBuffer(*Buffer);
        *Buffer = 0;
    }
}

EXPOSE_SINGLE_INTERFACE(ModelPreviewRenderer, IModelPreviewRenderer, MODEL_PREVIEW_RENDERER_INTERFACE_VERSION);
}
