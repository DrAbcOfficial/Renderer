#pragma once

#include <GL/glew.h>
#include "startup_video.h"

namespace StartupVideo
{
//Owns only startup resources; can run before Renderer::R_Init. Draw restores
//every state it changes, without touching Renderer's state caches.
class Painter
{
public:
    Painter() = default;
    ~Painter();

    Painter(const Painter&)            = delete;
    Painter& operator=(const Painter&) = delete;

    bool Draw(const Frame& frame, const Rect& rect, int windowWidth, int windowHeight, GLuint framebuffer = 0);

    const std::string& Error() const
    {
        return error;
    }

private:
    bool Initialize();

    GLuint program     = 0;
    GLuint vao         = 0;
    GLuint textures[3] = {};

    int widths[3]  = {};
    int heights[3] = {};

    std::string error;
};
} // namespace StartupVideo
