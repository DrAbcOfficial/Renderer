#pragma once

#ifndef ENTITY_VIEW_RENDERER_INTERFACE_H
#    define ENTITY_VIEW_RENDERER_INTERFACE_H

//Renderer-owned primitives for drawing entities outside the world scene, for
//example a model card in a menu. A view only provides what the scene renderer
//has to handle itself: an isolated camera, the target framebuffer, entity
//lighting and the Studio hooks that keep world-only work (events, deferred
//passes, bounding-box culling) out of the way. The caller prepares the entity,
//sets it with IMetaRenderer::SetCurrentEntity and draws it with
//IMetaRenderer::DrawCurrentEntity.
//
//Optional interface: the IMetaRenderer_API_002 vtable is unchanged. Enter and
//leave views on the render thread, outside a scene pass (for example from
//OnRenderEndFrame, before the UI is painted).
#    include <interface.h>

struct model_s;
struct FBO_Container_s;

//Light applied to the entities of a view, in the engine's alight_t units.
typedef struct entity_light_s
{
    int   ambientlight; //ambient term, clipped at 128
    int   shadelight;   //shade term, clipped at 255 - ambientlight
    float color[3];     //diffuse light color, clipped to 0..1
    float direction[3]; //direction the light travels; normalized internally
} entity_light_t;

//One isolated view: the caller's camera, target and light.
typedef struct entity_view_s
{
    //Framebuffer to render into; its color texture must exist.
    FBO_Container_s* target;

    float origin[3];   //camera origin
    float angles[3];   //camera angles (pitch, yaw, roll)
    float verticalFov; //vertical field of view in degrees, 1..179
    float nearPlane;   //greater than 0
    float farPlane;    //greater than nearPlane

    //RGBA the target is cleared with. RGB is premultiplied by alpha, and
    //entities blend premultiplied into the target, so the color texture
    //composites correctly with DRAW_TEXTURED_RECT_PREMULTIPLIED_ALPHA_ENABLED.
    float background[4];

    //Entities drawn in the view use this light; the engine's dynamic and
    //entity light are not consulted, so game light state is never touched.
    entity_light_t light;
} entity_view_t;

class IEntityViewRenderer : public IBaseInterface
{
public:
    /*
        Load a Studio model by its mod-relative path ("models/..."). The path
        is validated through the engine filesystem first, so a missing file
        returns null instead of the engine's error model. The engine model
        cache owns the returned model; never free it.
    */
    virtual model_s* LoadStudioModel(const char* Path) = 0;

    /*
        Studio header of a model returned by LoadStudioModel, null when the
        model has no data block.
    */
    virtual void* GetModelData(model_s* Model) = 0;

    /*
        Open a view: save the scene state, bind and clear View.target and
        prepare the camera, projection and light. Returns false when called
        inside a scene pass, while another view is active, or when the target
        or camera is invalid.
    */
    virtual bool BeginView(const entity_view_t& View) = 0;

    /*
        Close the view opened by BeginView and restore the saved scene state.
        Safe to call without an active view.
    */
    virtual void EndView() = 0;

    /*
        True while a BSP world is loaded.
    */
    virtual bool HasWorld() const = 0;
};

#    define ENTITY_VIEW_RENDERER_INTERFACE_VERSION "EntityViewRenderer_API_001"

#endif //ENTITY_VIEW_RENDERER_INTERFACE_H
