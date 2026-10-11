#pragma once

#include <GL/glew.h>
#include "startup_video.h"

/*
    Owns startup rendering until playback returns; can run before R_Init.
    Draw explicitly sets its GL state without saving/restoring caller state.
    Destruction releases video resources and resets bindings and pixel unpack layout.
*/
class CStartupVideoPainter
{
public:
    CStartupVideoPainter() = default;
    ~CStartupVideoPainter();

    CStartupVideoPainter(const CStartupVideoPainter&)            = delete;
    CStartupVideoPainter& operator=(const CStartupVideoPainter&) = delete;

    bool Draw(const CStartupVideoFrameView& frame, const CStartupVideoRect& rect, int windowWidth, int windowHeight, GLuint framebuffer = 0);

    const std::string& GetError() const
    {
        return m_Error;
    }

private:
    bool Initialize();

    GLuint m_hProgram{};
    GLuint m_hVAO{};
    GLuint m_hTextures[3]{};

    int m_TextureWidths[3]{};
    int m_TextureHeights[3]{};

    std::string m_Error;
};
