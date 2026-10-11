#include "startup_video_gl.h"
#include "gl_shader_compile.h"
#include <GLFW/glfw3.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr int TargetSize = 64;

static void TestShaderCompilation()
{
    std::string error;
    assert(0 == GL_TryCompileShaderObject(GL_VERTEX_SHADER, "#version 330 core\ninvalid shader", error));
    assert(!error.empty());

    //A failed compilation must not prevent a subsequent valid shader from compiling.
    GLuint shader = GL_TryCompileShaderObject(GL_VERTEX_SHADER,
                                              "#version 330 core\nvoid main() { gl_Position = vec4(0.0); }", error);
    assert(0 != shader);
    assert(error.empty());
    assert(GL_TRUE == glIsShader(shader));
    glDeleteShader(shader);
    assert(GL_FALSE == glIsShader(shader));
    assert(GL_NO_ERROR == glGetError());
}

//Video drawing must explicitly establish its state regardless of incoming state.
static void SetDirtyState()
{
    glViewport(3, 4, 17, 19);
    glEnable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_RASTERIZER_DISCARD);
    glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glScissor(0, 0, 1, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 27);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 3);
    glActiveTexture(GL_TEXTURE5);
    glColorMaski(1, GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
    glDisablei(GL_BLEND, 1);
}

static void CheckVideoState()
{
    assert(!glIsEnabledi(GL_BLEND, 0));
    assert(!glIsEnabledi(GL_SCISSOR_TEST, 0));
    assert(!glIsEnabled(GL_RASTERIZER_DISCARD));
}

//Odd dimensions, padded rows and changing sizes exercise allocation
//and chroma rounding. Top row is white; bottom row is black.
static void TestStateAndOrientation(CStartupVideoPainter& painter, GLuint fbo)
{
    for (const auto shifts : {std::array<int, 2>{1, 1}, {1, 0}, {0, 0}})
    {
        CStartupVideoFrameView frame;
        frame.width  = 5 + shifts[0] * 2;
        frame.height = 5;
        frame.shiftX = shifts[0];
        frame.shiftY = shifts[1];

        std::vector<unsigned char> planes[3];

        for (int i = 0; i < 3; ++i)
        {
            const int w = i ? (frame.width + shifts[0]) >> shifts[0] : frame.width;
            const int h = i ? (frame.height + shifts[1]) >> shifts[1] : frame.height;

            frame.strides[i] = w + 5;

            planes[i].assign(frame.strides[i] * h, i ? 128 : 16);
            frame.planes[i] = planes[i].data();
        }

        for (int x = 0; x < frame.width; ++x)
        {
            planes[0][x] = 235;
        }

        for (bool negative : {false, true})
        {
            if (negative)
            {
                //Flip to bottom-up layout: point at the last row, negate the stride.
                for (int i = 0; i < 3; ++i)
                {
                    int w, h;
                    assert(frame.GetPlaneSize(i, w, h));

                    frame.planes[i] += (h - 1) * frame.strides[i];
                    frame.strides[i] = -frame.strides[i];
                }
            }

            SetDirtyState();

            const CStartupVideoRect rect{0, 0, frame.width, frame.height};
            assert(painter.Draw(frame, rect, TargetSize, TargetSize, fbo));
            assert(GL_NO_ERROR == glGetError());

            CheckVideoState();

            unsigned char top[4];
            unsigned char bottom[4];
            glReadPixels(0, TargetSize - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, top);
            glReadPixels(0, TargetSize - frame.height, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bottom);

            for (int i = 0; i < 3; ++i)
            {
                assert(std::abs((negative ? 0 : 255) - int(top[i])) <= 2);
                assert(std::abs((negative ? 255 : 0) - int(bottom[i])) <= 2);
            }
        }
    }
}

//Expected bytes from hl-10210's yuv2rgb table with its V,U ordering.
static void TestColorConversion(CStartupVideoPainter& painter, GLuint fbo)
{
    //{Y, U, V, R, G, B}
    const std::array<int, 6> samples[] = {
        {81, 90, 240, 255, 63, 15},
        {41, 240, 110, 0, 0, 208},
        {145, 54, 34, 0, 246, 32},
    };

    for (const auto& sample : samples)
    {
        unsigned char yuv[] = {
            static_cast<unsigned char>(sample[0]),
            static_cast<unsigned char>(sample[1]),
            static_cast<unsigned char>(sample[2]),
        };

        CStartupVideoFrameView frame;
        frame.width  = 1;
        frame.height = 1;

        for (int i = 0; i < 3; ++i)
        {
            frame.planes[i]  = &yuv[i];
            frame.strides[i] = 1;
        }

        assert(painter.Draw(frame, {0, 0, TargetSize, TargetSize}, TargetSize, TargetSize, fbo));

        unsigned char pixel[4];
        glReadPixels(TargetSize / 2, TargetSize / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

        for (int i = 0; i < 3; ++i)
        {
            assert(std::abs(sample[3 + i] - int(pixel[i])) <= 2);
        }
    }
}

//Returns 2 (skipped) when no OpenGL 4.4 Core context is available.
int main()
{
    if (!glfwInit())
        return 2;

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 4);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    auto window = glfwCreateWindow(TargetSize, TargetSize, "Startup video test", nullptr, nullptr);

    if (!window)
    {
        glfwTerminate();
        return 2;
    }

    glfwMakeContextCurrent(window);

    glewExperimental = GL_TRUE;

    if (GLEW_OK != glewInit())
        return 2;

    //Discard errors raised during GLEW initialization
    while (glGetError() != GL_NO_ERROR)
    {
    }

    {
        TestShaderCompilation();

        GLuint fbo;
        GLuint color;

        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &color);

        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TargetSize, TargetSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        assert(GL_FRAMEBUFFER_COMPLETE == glCheckFramebufferStatus(GL_FRAMEBUFFER));

        GLint program = 0;
        {
            CStartupVideoPainter painter;
            TestStateAndOrientation(painter, fbo);
            TestColorConversion(painter, fbo);
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            assert(0 != program);
            CStartupVideoFrameView invalidFrame;
            assert(!painter.Draw(invalidFrame, {0, 0, TargetSize, TargetSize}, TargetSize, TargetSize, fbo));
        }
        //The last video program must actually be released, not left current/pending deletion.
        assert(GL_FALSE == glIsProgram(program));
        GLint value = -1;
        glGetIntegerv(GL_CURRENT_PROGRAM, &value);
        assert(0 == value);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &value);
        assert(GL_TEXTURE0 == value);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &value);
        assert(0 == value);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &value);
        assert(4 == value);

        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &color);
    }

    assert(GL_NO_ERROR == glGetError());

    glfwDestroyWindow(window);
    glfwTerminate();

    std::puts("startup_video_gl_tests passed (OpenGL 4.4 Core)");
}
