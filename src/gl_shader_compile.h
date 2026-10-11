#pragma once

#include <GL/glew.h>
#include <string>

/*
    Requires only a current GL context and initialized entry points, including before R_Init.
    The caller owns the returned shader. Failure deletes it and returns 0 with a diagnostic.
*/
inline GLuint GL_TryCompileShaderObject(GLenum type, const char* code, std::string& error)
{
    error.clear();

    GLuint shader = glCreateShader(type);

    if (!shader)
    {
        error = "Cannot create shader object";
        return 0;
    }

    glShaderSource(shader, 1, &code, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

    if (compiled == GL_TRUE)
        return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);

    //Release the GL object even if allocating the diagnostic throws.
    try
    {
        if (length > 1)
        {
            error.resize(length);

            GLsizei written = 0;
            glGetShaderInfoLog(shader, length, &written, &error[0]);

            error.resize(written);
        }

        if (error.empty())
            error = "Shader compilation failed without an info log";
    }
    catch (...)
    {
        glDeleteShader(shader);
        throw;
    }

    glDeleteShader(shader);

    return 0;
}
