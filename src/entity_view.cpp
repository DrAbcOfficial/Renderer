//Renderer-owned implementation of IEntityViewRenderer (entity_view.h). A view
//renders the caller's entity into the caller's FBO through the same scene path
//the shadow views use: the visible-entity list carries only the entity,
//R_RenderScene draws it, and the view's camera sits in the shared camera UBO
//in multiview mode. The scene pipeline owns the UBOs and the framebuffer
//chain; the view only binds its target, keeps the world (and the client's own
//3D drawing) out of it and puts the caller's state back when it ends.
#include "gl_local.h"
#include "entity_view.h"
#include <IEntityViewRenderer.h>
#include <IFileSystem.h>
#include <ScopeExit/ScopeExit.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{

//"IDST": the id field of a Studio header. kHalfDegreeToRadians converts the
//half vertical fov to radians (VerticalFov * kHalfDegreeToRadians).
constexpr int   kStudioHeaderId      = 0x54534449;
constexpr float kHalfDegreeToRadians = 0.00872664626f;

/*
    One view. Begin() turns the caller's target into the current scene FBO,
    moves the camera into the shared camera UBO and makes the scene pipeline
    keep both (multiview mode, exactly like a shadow view). DrawEntity() puts
    the entity into the visible-entity list and runs the scene render. End()
    restores everything the view replaced.
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

    //Draws one entity through the scene pipeline.
    void DrawEntity(cl_entity_t* Entity);

    //Copies the light of the active view into *Light.
    bool ApplyLight(alight_t* Light);

    //(Re)installs the view camera, the target viewport, the refdef rectangle
    //and the scene switches the view needs.
    void ApplyCamera();

private:
    static bool IsCameraUsable(const entity_view_t& NewView);
    void        SaveCallerState();
    void        EndCallerState();
    void        ClearTarget(const entity_view_t& NewView);

    //The view input while it is active.
    entity_view_t View{};
    bool          bBegun = false;

    //The frame state the view replaces: the caller's visible-entity list, its
    //current entity, the scene framebuffers, the refdef rectangle, the
    //viewport and the switch values that steer the scene pipeline.
    std::vector<cl_entity_t*> SavedVisEdicts;
    int                       SavedNumVisEdicts   = 0;
    cl_entity_t*              SavedEntity         = nullptr;
    FBO_Container_t*          SavedSceneFBO       = nullptr;
    FBO_Container_t*          SavedRenderingFBO   = nullptr;
    vrect_GoldSrc_t           SavedVrect{};
    float                     SavedViewport[4]{};
    GLint                     SavedGLViewport[4]{};
    int                       SavedDrawClassify   = 0;
    bool                      SavedDrawMultiview  = false;
    bool                      SavedGammaBlend     = false;
    bool                      SavedOnlyClientDraw = false;
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

    //The engine renders no 3D view while a level transition runs, and the
    //scene pipeline an entity view uses assumes the same world state; skip the
    //view until the new map is in place.
    if (SCR_IsLoadingVisible())
        return false;

    View   = NewView;
    bBegun = true;

    //From here on every failure path restores through End().
    SaveCallerState();

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

    ApplyCamera();

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

    EndCallerState();

    //The Studio renderer binds its textures directly, so the engine's texture
    //cache must not believe its own binding is still current.
    *currenttexture = -1;
}

void CViewScope::DrawEntity(cl_entity_t* Entity)
{
    if (!bBegun || !Entity)
        return;

    //The scene pipeline draws what the visible-entity list holds and
    //classifies opaque and transparent entities itself; a view holds exactly
    //the caller's entity.
    cl_visedicts[0]  = Entity;
    *cl_numvisedicts = 1;

    R_RenderScene();

    //Do not leave the entity visible for anything that runs before End().
    *cl_numvisedicts = 0;
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

void CViewScope::SaveCallerState()
{
    SavedNumVisEdicts = *cl_numvisedicts;
    SavedVisEdicts.assign(cl_visedicts, cl_visedicts + SavedNumVisEdicts);

    SavedEntity         = *currententity;
    SavedSceneFBO       = GL_GetCurrentSceneFBO();
    SavedRenderingFBO   = GL_GetCurrentRenderingFBO();
    SavedOnlyClientDraw = (*r_refdef.onlyClientDraws);

    memcpy(&SavedVrect, &(*r_refdef.vrect), sizeof(SavedVrect));
    memcpy(SavedViewport, r_viewport, sizeof(SavedViewport));
    glGetIntegerv(GL_VIEWPORT, SavedGLViewport);

    SavedDrawClassify  = r_draw_classify;
    SavedDrawMultiview = r_draw_multiview;
    SavedGammaBlend    = r_draw_gammablend;
}

void CViewScope::EndCallerState()
{
    r_draw_classify   = SavedDrawClassify;
    r_draw_multiview  = SavedDrawMultiview;
    r_draw_gammablend = SavedGammaBlend;

    (*r_refdef.onlyClientDraws) = SavedOnlyClientDraw;

    memcpy(&(*r_refdef.vrect), &SavedVrect, sizeof(SavedVrect));
    memcpy(r_viewport, SavedViewport, sizeof(SavedViewport));
    glViewport(SavedGLViewport[0], SavedGLViewport[1], SavedGLViewport[2], SavedGLViewport[3]);

    *currententity   = SavedEntity;
    *cl_numvisedicts = SavedNumVisEdicts;

    if (SavedNumVisEdicts > 0)
    {
        memcpy(cl_visedicts, SavedVisEdicts.data(), size_t(SavedNumVisEdicts) * sizeof(cl_entity_t*));
    }

    //GL_PopFrameBuffer only restores the raw READ/DRAW bindings; keep the
    //renderer's bookkeeping in sync the same way SCClientFramebufferScope does.
    GL_SetCurrentSceneFBO(SavedSceneFBO);
    GL_BindFrameBuffer(SavedRenderingFBO);
    GL_PopFrameBuffer();
}

void CViewScope::ApplyCamera()
{
    //The view owns the target's viewport and rectangle; the frame's own
    //viewport is restored in End().
    r_viewport[0] = 0;
    r_viewport[1] = 0;
    r_viewport[2] = float(View.target->iWidth);
    r_viewport[3] = float(View.target->iHeight);
    R_SetViewport(0, 0, float(View.target->iWidth), float(View.target->iHeight));

    (*r_refdef.vrect).x      = 0;
    (*r_refdef.vrect).y      = 0;
    (*r_refdef.vrect).width  = View.target->iWidth;
    (*r_refdef.vrect).height = View.target->iHeight;

    //The scene pipeline reads this flag before it draws world and entities.
    (*r_refdef.onlyClientDraws) = false;

    VectorCopy(View.origin, *r_refdef.vieworg);
    VectorCopy(View.angles, *r_refdef.viewangles);
    R_UpdateRefDef();
    R_LoadIdentityForWorldMatrix();
    R_SetupPlayerViewWorldMatrix(View.origin, View.angles);

    const float Aspect     = float(View.target->iWidth) / float(View.target->iHeight);
    const float HalfHeight = View.nearPlane * tanf(View.verticalFov * kHalfDegreeToRadians);

    R_LoadIdentityForProjectionMatrix();
    R_SetupFrustumProjectionMatrix(-HalfHeight * Aspect, HalfHeight * Aspect,
                                   -HalfHeight, HalfHeight, View.nearPlane, View.farPlane);

    //R_SetupGL skips its own camera setup for multiview, so the shared camera
    //UBO is the camera every camera-reading program uses for this view.
    camera_ubo_t Camera{};

    R_SetupCameraView(&Camera.views[0]);
    InvertMatrix(&Camera.views[0].worldMatrix[0][0], &Camera.views[0].invWorldMatrix[0][0]);
    InvertMatrix(&Camera.views[0].projMatrix[0][0], &Camera.views[0].invProjMatrix[0][0]);
    Camera.views[0].vright_znear[3] = View.nearPlane;
    Camera.views[0].vup_zfar[3]     = View.farPlane;
    Camera.numViews                 = 1;

    R_UploadCameraUBOData(&Camera);

    //The view draws like a shadow view: R_SetupGL keeps the camera the view
    //built, gamma-blend programs write the RGBA8 target the caller samples
    //directly, and the world stays out of the visible-entity list.
    r_draw_multiview  = true;
    r_draw_gammablend = true;
    r_draw_classify   = DRAW_CLASSIFY_OPAQUE_ENTITIES | DRAW_CLASSIFY_TRANS_ENTITIES;
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

void EntityView_DrawCurrentEntity(cl_entity_s* Entity)
{
    ViewScope.DrawEntity(Entity);
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

EXPOSE_SINGLE_INTERFACE(CEntityViewRenderer, IEntityViewRenderer, ENTITY_VIEW_RENDERER_INTERFACE_VERSION);
