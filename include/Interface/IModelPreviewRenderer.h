#pragma once

// Renderer-owned, world-independent Studio preview. Optional interface: the
// IMetaRenderer_API_002 vtable is unchanged. Call only on the render thread,
// outside a scene pass (e.g. OnRenderEndFrame, before painting UI).
#include <interface.h>

struct cl_entity_s;
struct model_s;
struct FBO_Container_s;

struct ModelPreviewView {
    float Origin[3] = {120, 0, 0};
    float Angles[3] = {0, 180, 0};
    float VerticalFov = 40;
    float NearPlane = 1;
    float FarPlane = 4096;
    int Ambient = 96;
    int Shade = 96;
    float LightColor[3] = {1, 1, 1};
    float LightDirection[3] = {-1, 0, -1};
    float Background[4] = {0, 0, 0, 0};
    bool Lighting = true;
};

class IModelPreviewRenderer : public IBaseInterface {
public:
    // Engine filesystem/model cache owns the returned model; never free it.
    virtual model_s* loadModel(const char* Path) = 0;
    virtual void* modelData(model_s* Model) = 0;
    // Entity must be a non-player Studio entity, index 0, with no follow parent.
    // No BSP, game lights, client events or main-view postprocessing are used.
    virtual bool render(cl_entity_s* Entity, FBO_Container_s* Target,
                        const ModelPreviewView& View) = 0;
    virtual bool hasWorld() const = 0;
};

#define MODEL_PREVIEW_RENDERER_INTERFACE_VERSION "ModelPreviewRenderer001"
