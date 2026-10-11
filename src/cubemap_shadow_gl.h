#pragma once

#include <GL/glew.h>

// Shared by the renderer and the driver-backed shadow sampling regression.
namespace CubemapShadow
{
inline void CreateTexture(GLuint texture, int width, int height)
{
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture);
    // A cubemap has adjacent faces, not an unoccluded outside border. Linear
    // comparison filtering must never blend in a far-depth border at seams.
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, GL_DEPTH32F_STENCIL8, width, height);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

class SamplingScope
{
    GLboolean wasEnabled;

public:
    // Seamless filtering is context state, not a texture parameter. Keep it
    // local to the light pass so engine/client cubemaps retain their state.
    SamplingScope() : wasEnabled(glIsEnabled(GL_TEXTURE_CUBE_MAP_SEAMLESS))
    {
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    }
    ~SamplingScope()
    {
        if (!wasEnabled)
            glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    }
    SamplingScope(const SamplingScope&)            = delete;
    SamplingScope& operator=(const SamplingScope&) = delete;
};
} // namespace CubemapShadow
