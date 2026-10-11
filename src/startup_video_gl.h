#pragma once

#include <GL/glew.h>
#include "startup_video.h"

/*
    Owns only startup resources; can run before R_Init. Draw restores
    every state it changes, without touching Renderer's state caches.
*/
class CStartupVideoPainter
{
public:
    CStartupVideoPainter() = default;
    ~CStartupVideoPainter();

    CStartupVideoPainter(const CStartupVideoPainter&)            = delete;
    CStartupVideoPainter& operator=(const CStartupVideoPainter&) = delete;

    bool Draw(const CStartupVideoFrame& frame, const CStartupVideoRect& rect, int windowWidth, int windowHeight, GLuint framebuffer = 0);

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
