// New implementation: world-independent previews through the existing Studio
// renderer. ModelPreview.h is the hook seam; IModelPreviewRenderer is the UI seam.
#include "gl_local.h"
#include "ModelPreview.h"
#include <IModelPreviewRenderer.h>

#include <algorithm>
#include <cmath>

namespace modelpreview {
namespace {
const ModelPreviewView* ActiveView = nullptr;
void (*OriginalEntityLight)(alight_t*) = nullptr;
hook_t* EntityLightHook = nullptr;

void entityLight(alight_t* Light) {
    // The engine's entity-light routine can mutate live game elights.
    if (!ActiveView)
        OriginalEntityLight(Light);
}

template <class T> class SavedValue {
public:
    SavedValue(T& Target, T Value) : Target(Target), Saved(Target) { Target = Value; }
    ~SavedValue() { Target = Saved; }
private:
    T& Target;
    T Saved;
};

// Only used at the UI boundary, not in the scene's hot path. Preserve UBO bytes
// too: a cold menu has never run R_PrepareDrawWorld.
template <class T> class SavedBuffer {
public:
    explicit SavedBuffer(GLuint Buffer) : Buffer(Buffer) {
        glGetNamedBufferSubData(Buffer, 0, sizeof(Data), &Data);
    }
    ~SavedBuffer() { GL_UploadSubDataToUBO(Buffer, 0, sizeof(Data), &Data); }
private:
    GLuint Buffer;
    T Data{};
};

class PreviewState {
public:
    PreviewState()
        : Camera(g_WorldSurfaceRenderer.hCameraUBO),
          Scene(g_WorldSurfaceRenderer.hSceneUBO), Entity(*currententity),
          SceneFbo(GL_GetCurrentSceneFBO()) {
        glGetIntegerv(GL_VIEWPORT, Viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &Program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &Vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &ActiveTexture);
        glGetIntegerv(GL_DEPTH_FUNC, &DepthFunction);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &DepthMask);
        glGetDoublev(GL_DEPTH_RANGE, DepthRange);
        glGetIntegerv(GL_CULL_FACE_MODE, &CullMode);
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
        for (int i = 0; i < 5; ++i) {
            if (Enabled[i]) glEnable(Capabilities[i]);
            else glDisable(Capabilities[i]);
        }
        GL_UseProgram(Program);
        GL_BindVAO(Vao);
        glActiveTexture(ActiveTexture);
        // Studio binds textures directly; do not leave the engine cache stale.
        *currenttexture = -1;
    }
private:
    SavedBuffer<camera_ubo_t> Camera;
    SavedBuffer<scene_ubo_t> Scene;
    cl_entity_t* Entity;
    FBO_Container_t* SceneFbo;
    GLint Viewport[4]{}, Program{}, Vao{}, ActiveTexture{}, DepthFunction{}, CullMode{};
    GLfloat RenderViewport[4]{};
    GLdouble DepthRange[2]{};
    GLboolean DepthMask{}, Enabled[5]{};
    static constexpr GLenum Capabilities[5] = {
        GL_DEPTH_TEST, GL_CULL_FACE, GL_BLEND, GL_SCISSOR_TEST, GL_STENCIL_TEST};
};

class ModelPreviewRenderer : public IModelPreviewRenderer {
public:
    model_t* loadModel(const char* Path) override {
        if (!Path || !*Path || !IEngineStudio.Mod_ForName)
            return nullptr;
        auto* Model = IEngineStudio.Mod_ForName(Path, false);
        return Model && Model->type == mod_studio ? Model : nullptr;
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
        SavedValue Active(ActiveView, &View);
        SavedValue Opaque(r_draw_opaque, false);
        SavedValue Oit(r_draw_oitblend, false);
        SavedValue GBuffer(r_draw_gbuffer, false);
        SavedValue Gamma(r_draw_gammablend, false);
        SavedValue Fog(r_fog_enabled, false);
        SavedValue Hide(r_draw_hide_entity, false);
        SavedValue Multiview(r_draw_multiview, false);
        SavedValue Classify(r_draw_classify, DRAW_CLASSIFY_OPAQUE_ENTITIES | DRAW_CLASSIFY_TRANS_ENTITIES);
        SavedValue Blend(*r_blend, 1.0f);
        SavedValue Shadows(r_shadows->value, 0.0f);
        auto* DlightCvar = gEngfuncs.pfnGetCvarPointer("r_studio_legacy_dlight");
        auto* ElightCvar = gEngfuncs.pfnGetCvarPointer("r_studio_legacy_elight");
        SavedValue Dlights(DlightCvar->value, 0.0f);
        SavedValue Elights(ElightCvar->value, 0.0f);

        GL_BindFrameBuffer(Target);
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
        R_UploadCameraUBO();
        R_UploadSceneUBO();
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LEQUAL);
        glDepthRange(0, 1);
        float Background[4];
        memcpy(Background, View.Background, sizeof(Background));
        GL_ClearColorDepthStencil(Background, 1, 0, STENCIL_MASK_ALL);
        *currententity = Entity;
        // Same implementation behind IMetaRenderer::DrawCurrentEntity. No
        // world render, fabricated BSP, or independent model pipeline.
        R_DrawCurrentEntity(Entity->curstate.rendermode != kRenderNormal);
        return true;
    }
};
}

bool isActive() { return ActiveView != nullptr; }

bool setupLight(alight_t* Light) {
    if (!ActiveView)
        return false;
    Light->ambientlight = std::clamp(ActiveView->Ambient, 0, 128);
    Light->shadelight = std::clamp(ActiveView->Shade, 0, 255 - Light->ambientlight);
    VectorCopy(ActiveView->LightColor, Light->color);
    VectorCopy(ActiveView->LightDirection, Light->plightvec);
    VectorNormalize(Light->plightvec);
    return true;
}

void install(engine_studio_api_t* Studio) {
    if (!EntityLightHook)
        EntityLightHook = g_pMetaHookAPI->InlineHook(
            Studio->StudioEntityLight, entityLight, (void**)&OriginalEntityLight);
}

void uninstall() {
    if (EntityLightHook) g_pMetaHookAPI->UnHook(EntityLightHook);
    EntityLightHook = nullptr;
    OriginalEntityLight = nullptr;
}

EXPOSE_SINGLE_INTERFACE(ModelPreviewRenderer, IModelPreviewRenderer, MODEL_PREVIEW_RENDERER_INTERFACE_VERSION);
}
