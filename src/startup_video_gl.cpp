#include "startup_video_gl.h"
#include "gl_shader_compile.h"

static constexpr const char* VertexShader = R"glsl(#version 330 core
out vec2 texcoord;

void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    texcoord = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

static constexpr const char* FragmentShader = R"glsl(#version 330 core
in vec2 texcoord;
out vec4 color;

uniform sampler2D planeY;
uniform sampler2D planeU;
uniform sampler2D planeV;
uniform ivec2 chromaShift;

vec3 sourceColor(ivec2 p)
{
    p = clamp(p, ivec2(0), textureSize(planeY, 0) - 1);
    ivec2 c = p >> chromaShift;

    float y = (texelFetch(planeY, p, 0).r - 16.0 / 255.0) * 1.16438356;
    float u = texelFetch(planeU, c, 0).r - 128.0 / 255.0;
    float v = texelFetch(planeV, c, 0).r - 128.0 / 255.0;

    // HL25 passes V,U to its yuv2rgb table and uploads its B,G,R byte order as
    // RGBA. Preserve those coefficients (including the green-channel quirk).
    return clamp(vec3(y + 2.01723214 * v,
                      y - 0.81296765 * u - 0.39176229 * v,
                      y + 1.59602678 * u), 0.0, 1.0);
}

void main()
{
    // The original converts subsampled YUV to RGB before GL_LINEAR filtering.
    // Interpolating the planes first changes clipped colors and chroma edges.
    vec2 p = texcoord * vec2(textureSize(planeY, 0)) - 0.5;
    ivec2 q = ivec2(floor(p));
    vec2 f = fract(p);

    vec3 top = mix(sourceColor(q), sourceColor(q + ivec2(1, 0)), f.x);
    vec3 bottom = mix(sourceColor(q + ivec2(0, 1)), sourceColor(q + ivec2(1, 1)), f.x);

    color = vec4(mix(top, bottom, f.y), 1.0);
}
)glsl";

CStartupVideoPainter::~CStartupVideoPainter()
{
    //Playback owns this phase. Leave neutral bindings and upload layout for startup drawing.
    if (m_hProgram)
    {
        glUseProgram(0);
        glBindVertexArray(0);
        for (int i = 0; i < 3; ++i)
        {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindSampler(i, 0);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    }
    glDeleteTextures(3, m_hTextures);

    if (m_hVAO)
        glDeleteVertexArrays(1, &m_hVAO);

    if (m_hProgram)
        glDeleteProgram(m_hProgram);
}

bool CStartupVideoPainter::Initialize()
{
    if (m_hProgram)
        return true;

    GLuint candidate = glCreateProgram();

    bool ok = (candidate != 0);

    GLuint vertexShader   = 0;
    GLuint fragmentShader = 0;

    if (ok)
    {
        vertexShader = GL_TryCompileShaderObject(GL_VERTEX_SHADER, VertexShader, m_Error);
        ok           = (vertexShader != 0);
    }

    if (ok)
    {
        fragmentShader = GL_TryCompileShaderObject(GL_FRAGMENT_SHADER, FragmentShader, m_Error);
        ok             = (fragmentShader != 0);
    }

    if (ok)
    {
        glAttachShader(candidate, vertexShader);
        glAttachShader(candidate, fragmentShader);
        glLinkProgram(candidate);

        GLint linked = 0;
        glGetProgramiv(candidate, GL_LINK_STATUS, &linked);

        if (!linked)
        {
            char log[1024] = {};
            glGetProgramInfoLog(candidate, sizeof(log), nullptr, log);
            m_Error = log;
            ok      = false;
        }
    }

    if (vertexShader)
        glDeleteShader(vertexShader);

    if (fragmentShader)
        glDeleteShader(fragmentShader);

    if (!ok)
    {
        if (candidate)
            glDeleteProgram(candidate);

        if (m_Error.empty())
            m_Error = "Cannot create startup shader";

        return false;
    }

    m_hProgram = candidate;

    glGenVertexArrays(1, &m_hVAO);
    glGenTextures(3, m_hTextures);

    glUseProgram(m_hProgram);
    glUniform1i(glGetUniformLocation(m_hProgram, "planeY"), 0);
    glUniform1i(glGetUniformLocation(m_hProgram, "planeU"), 1);
    glUniform1i(glGetUniformLocation(m_hProgram, "planeV"), 2);

    for (int i = 0; i < 3; ++i)
    {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, m_hTextures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    return true;
}

bool CStartupVideoPainter::Draw(const CStartupVideoFrameView& frame, const CStartupVideoRect& rect, int windowWidth, int windowHeight, GLuint framebuffer)
{
    GLint maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);

    int planeWidths[3];
    int planeHeights[3];

    for (int i = 0; i < 3; ++i)
    {
        if (!frame.GetPlaneSize(i, planeWidths[i], planeHeights[i]) ||
            planeWidths[i] > maxTextureSize ||
            planeHeights[i] > maxTextureSize)
        {
            m_Error = "Invalid or oversized startup video plane";
            return false;
        }
    }

    if (windowWidth <= 0 || windowHeight <= 0 || rect.width <= 0 || rect.height <= 0)
    {
        m_Error = "Invalid startup video viewport";
        return false;
    }

    if (!Initialize())
        return false;

    //Video playback owns rendering until it returns; explicitly establish each required state.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_DITHER);
    glDisablei(GL_BLEND, 0);
    glDisablei(GL_SCISSOR_TEST, 0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);

    for (int i = 0; i < 3; ++i)
    {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, m_hTextures[i]);
        glBindSampler(i, 0);

        const int w = planeWidths[i];
        const int h = planeHeights[i];

        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

        //Reallocate only when plane size changes
        if (m_TextureWidths[i] != w || m_TextureHeights[i] != h)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

            m_TextureWidths[i]  = w;
            m_TextureHeights[i] = h;
        }

        if (frame.strides[i] > 0)
        {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.strides[i]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, frame.planes[i]);
        }
        else
        {
            //GL_UNPACK_ROW_LENGTH can't express negative strides, upload row by row.
            for (int row = 0; row < h; ++row)
            {
                const unsigned char* rowData = frame.planes[i] + ptrdiff_t(row) * frame.strides[i];

                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, w, 1, GL_RED, GL_UNSIGNED_BYTE, rowData);
            }
        }
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    //Rect is top-left based, GL viewport is bottom-left based.
    glViewport(rect.x, windowHeight - rect.y - rect.height, rect.width, rect.height);

    glUseProgram(m_hProgram);
    glBindVertexArray(m_hVAO);
    glUniform2i(glGetUniformLocation(m_hProgram, "chromaShift"), frame.shiftX, frame.shiftY);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    return true;
}
