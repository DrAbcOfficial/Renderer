// GPU regression for issue #807. The oracle draws every caster every frame;
// the candidate uses the exact production cache policy and texture-copy path.
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "../src/shadow_cache_gl.h"
#include "../src/cubemap_shadow_gl.h"
#include <tiny_obj_loader.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <limits>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
using ShadowCache::Bounds;
using ShadowCache::Cache;
using ShadowCache::Caster;
using ShadowCache::Light;
using ShadowCache::Vec3;
constexpr int   kSkip           = 2;
constexpr float kDepthTolerance = 0.000002f;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void CheckGL(const char* where)
{
    const auto error = glGetError();
    Require(error == GL_NO_ERROR, std::string(where) + ": GL error " + std::to_string(error));
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

GLuint Compile(GLenum type, const char* source)
{
    auto shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ready = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ready);
    char log[4096]{};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    Require(ready != 0, log);
    return shader;
}

GLuint CreateProgram()
{
    const char* vs      = R"GLSL(#version 430 core
layout(location=0) in vec3 vertex;
uniform mat4 model;
out vec3 worldPosition;
void main() { worldPosition = (model * vec4(vertex, 1)).xyz; gl_Position = vec4(worldPosition, 1); }
)GLSL";
    const char* gs      = R"GLSL(#version 430 core
layout(triangles) in;
layout(triangle_strip, max_vertices=18) out;
in vec3 worldPosition[];
out vec3 fragmentPosition;
uniform vec3 origin, forward[6], right[6], up[6];
uniform float range, tangent;
uniform int views;
void main() {
    const float nearPlane = 0.1;
    for (int face = 0; face < views; ++face) {
        for (int vertex = 0; vertex < 3; ++vertex) {
            vec3 delta = worldPosition[vertex] - origin;
            float z = dot(delta, forward[face]);
            gl_Position = vec4(dot(delta, right[face]) / tangent, dot(delta, up[face]) / tangent,
                (range + nearPlane) / (range - nearPlane) * z - 2 * range * nearPlane / (range - nearPlane), z);
            gl_Layer = face;
            fragmentPosition = worldPosition[vertex];
            EmitVertex();
        }
        EndPrimitive();
    }
}
)GLSL";
    const char* fs      = R"GLSL(#version 430 core
in vec3 fragmentPosition;
uniform vec3 origin;
uniform float range;
void main() { gl_FragDepth = length(fragmentPosition - origin) / range; }
)GLSL";
    GLuint      program = glCreateProgram();
    for (auto shader : {Compile(GL_VERTEX_SHADER, vs), Compile(GL_GEOMETRY_SHADER, gs), Compile(GL_FRAGMENT_SHADER, fs)})
    {
        glAttachShader(program, shader);
        glDeleteShader(shader);
    }
    glLinkProgram(program);
    GLint ready = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ready);
    char log[4096]{};
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    Require(ready != 0, log);
    return program;
}

struct Mesh
{
    GLuint  vao = 0, vbo = 0;
    GLsizei count = 0;
    explicit Mesh(const std::vector<Vec3>& vertices)
    {
        count = static_cast<GLsizei>(vertices.size());
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vec3), vertices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vec3), nullptr);
        glEnableVertexAttribArray(0);
    }
    ~Mesh()
    {
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
    }
};

std::vector<Vec3> LoadWorld(const char* path)
{
    tinyobj::ObjReader reader;
    Require(reader.ParseFromFile(path), std::string("OBJ: ") + reader.Error());
    std::vector<Vec3> vertices;
    const auto&       positions = reader.GetAttrib().vertices;
    for (const auto& shape : reader.GetShapes())
        for (const auto& index : shape.mesh.indices)
        {
            Require(index.vertex_index >= 0 && static_cast<size_t>(index.vertex_index) * 3 + 2 < positions.size(), "Invalid OBJ vertex");
            const auto offset = index.vertex_index * 3;
            vertices.push_back({positions[offset], positions[offset + 1], positions[offset + 2]});
        }
    Require(!vertices.empty() && vertices.size() % 3 == 0, "OBJ must contain triangles");
    std::printf("World: %zu triangles from %s\n", vertices.size() / 3, path);
    return vertices;
}

std::vector<Vec3> CubeVertices()
{
    std::vector<Vec3> vertices;
    const int         indices[] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                                   2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    for (int corner : indices)
        vertices.push_back({corner & 1 ? 1.0f : -1.0f, corner & 2 ? 1.0f : -1.0f, corner & 4 ? 1.0f : -1.0f});
    return vertices;
}

std::vector<Light> LoadLights(const char* path)
{
    std::ifstream stream(path);
    Require(stream.good(), "Cannot open entity file");
    std::string text, line;
    while (std::getline(stream, line))
        text += line.substr(0, line.find("//")) + '\n';
    const std::regex         token(R"TOKEN("([^"]*)"|([{}]))TOKEN");
    std::vector<std::string> tokens;
    for (std::sregex_iterator it(text.begin(), text.end(), token), end; it != end; ++it)
        tokens.push_back((*it)[2].matched ? (*it)[2].str() : (*it)[1].str());
    std::vector<Light> lights;
    int                directional = 0;
    for (size_t i = 0; i < tokens.size();)
    {
        Require(tokens[i++] == "{", "Expected entity block");
        std::map<std::string, std::string> fields;
        while (i < tokens.size() && tokens[i] != "}")
        {
            Require(i + 1 < tokens.size(), "Truncated entity field");
            fields[tokens[i]] = tokens[i + 1];
            i += 2;
        }
        Require(i < tokens.size(), "Unclosed entity block");
        ++i;
        if (fields["classname"] != "light_dynamic" || fields["shadow"] != "1")
            continue;
        if (fields["type"] == "directional")
        {
            ++directional;
            continue; // CSM remains camera-dependent and is outside this cache.
        }
        Require(fields["type"] == "point", "Unexpected local-light type in fixture");
        Light              light;
        std::istringstream origin(fields.at("origin"));
        Require(static_cast<bool>(origin >> light.origin[0] >> light.origin[1] >> light.origin[2]), "Invalid light origin");
        light.range      = std::stof(fields.at("size"));
        light.size       = std::stoi(fields.at("dynamic_shadow_size"));
        light.staticSize = std::stoi(fields.at("static_shadow_size"));
        Require(light.range > 0 && light.size > 0 && light.size <= 2048, "Invalid point-light fixture");
        lights.push_back(light);
    }
    Require(!lights.empty(), "No shadow-casting point lights");
    std::printf("Lights: %zu point lights using fixture origins/radii/resolutions; %d directional light(s) excluded (unchanged CSM).\n", lights.size(), directional);
    return lights;
}

Caster Cube(std::uintptr_t id, Vec3 center, float halfSize = 16, bool animated = true, float yaw = 0)
{
    Caster caster;
    caster.id               = id;
    caster.model            = 1;
    caster.volatileGeometry = animated;
    const float c = std::cos(yaw) * halfSize, s = std::sin(yaw) * halfSize;
    caster.transform = {c, -s, 0, center[0], s, c, 0, center[1], 0, 0, halfSize, center[2], 0, 0, 0, 1};
    for (const auto& vertex : CubeVertices())
    {
        Vec3 world{};
        for (int axis = 0; axis < 3; ++axis)
            world[axis] = caster.transform[axis * 4] * vertex[0] + caster.transform[axis * 4 + 1] * vertex[1] +
                caster.transform[axis * 4 + 2] * vertex[2] + caster.transform[axis * 4 + 3];
        caster.bounds.Add(world);
    }
    return caster;
}

struct Texture
{
    GLuint id   = 0;
    int    size = 0;
    bool   cube = true;
    ~Texture()
    {
        if (id) glDeleteTextures(1, &id);
    }
    bool Allocate(int newSize, bool cubemap)
    {
        if (id && size == newSize && cube == cubemap)
            return false;
        if (id) glDeleteTextures(1, &id);
        size = newSize;
        cube = cubemap;
        glGenTextures(1, &id);
        const auto target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        glBindTexture(target, id);
        glTexStorage2D(target, 1, GL_DEPTH32F_STENCIL8, size, size);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return true;
    }
    std::vector<float> Read() const
    {
        const auto target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        glBindTexture(target, id);
        const size_t       pixels = static_cast<size_t>(size) * size;
        std::vector<float> result(pixels * (cube ? 6 : 1));
        for (int face = 0; face < (cube ? 6 : 1); ++face)
            glGetTexImage(cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, result.data() + pixels * face);
        return result;
    }
};

struct LightState
{
    Cache   cache;
    Texture output, stored, reference, world, worldReference;
    bool    worldValid = false;
};

class Scene
{
public:
    std::vector<Light>      lights;
    std::vector<LightState> states;
    Mesh                    world, cube;
    GLuint                  program = 0, fbo = 0;
    size_t                  comparedPixels = 0, updates = 0, draws = 0, referenceDraws = 0, worldUpdates = 0;
    double                  cpuMilliseconds = 0, gpuMilliseconds = 0;

    Scene(const char* obj, const char* entities) : lights(LoadLights(entities)), states(lights.size()), world(LoadWorld(obj)), cube(CubeVertices())
    {
        program = CreateProgram();
        glGenFramebuffers(1, &fbo);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDisable(GL_CULL_FACE);
    }
    ~Scene()
    {
        glDeleteFramebuffers(1, &fbo);
        glDeleteProgram(program);
    }

    void Bind(const Texture& texture, const Light& light, bool clear)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, texture.id, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        Require(GL_FRAMEBUFFER_COMPLETE == glCheckFramebufferStatus(GL_FRAMEBUFFER), "Incomplete shadow framebuffer");
        glViewport(0, 0, texture.size, texture.size);
        if (clear)
            glClearBufferfi(GL_DEPTH_STENCIL, 0, 1, 0);
        glUseProgram(program);
        const Vec3 directions[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const Vec3 ups[]        = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
        Vec3       forwards[6], rights[6], top[6];
        for (int face = 0; face < 6; ++face)
        {
            forwards[face] = light.spot ? light.forward : directions[face];
            top[face]      = light.spot ? light.up : ups[face];
            rights[face]   = light.spot ? light.right : Cross(forwards[face], top[face]);
        }
        glUniform3fv(glGetUniformLocation(program, "origin"), 1, light.origin.data());
        glUniform3fv(glGetUniformLocation(program, "forward"), 6, forwards[0].data());
        glUniform3fv(glGetUniformLocation(program, "right"), 6, rights[0].data());
        glUniform3fv(glGetUniformLocation(program, "up"), 6, top[0].data());
        glUniform1f(glGetUniformLocation(program, "range"), light.range);
        glUniform1f(glGetUniformLocation(program, "tangent"), light.spot ? light.coneTangent : 1.0f);
        glUniform1i(glGetUniformLocation(program, "views"), light.spot ? 1 : 6);
    }

    void Draw(const Mesh& mesh, const float* matrix)
    {
        glUniformMatrix4fv(glGetUniformLocation(program, "model"), 1, GL_TRUE, matrix);
        glBindVertexArray(mesh.vao);
        glDrawArrays(GL_TRIANGLES, 0, mesh.count);
    }
    void DrawWorld()
    {
        const float identity[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        Draw(world, identity);
    }
    void Compare(const Texture& actual, const Texture& expected, const std::string& name)
    {
        auto a = actual.Read(), e = expected.Read();
        Require(e.size() == a.size(), name + ": depth size mismatch");
        for (size_t pixel = 0; pixel < e.size(); ++pixel)
            if (!std::isfinite(a[pixel]) || std::abs(e[pixel] - a[pixel]) > kDepthTolerance)
                throw std::runtime_error(name + ": stale/missing shadow at pixel " + std::to_string(pixel));
        comparedPixels += e.size();
    }

    size_t Frame(const char* name, const std::vector<Caster>& casters, const std::vector<Caster>& custom = {}, bool visible = true, bool force = false)
    {
        size_t            frameUpdates = 0, frameDraws = 0;
        const auto        cpuStart = std::chrono::steady_clock::now();
        std::vector<bool> dirty(lights.size());
        if (visible)
            for (size_t i = 0; i < lights.size(); ++i)
            {
                auto& state = states[i];
                auto& light = lights[i];
                if (state.cache.ProjectionChanged(light)) state.worldValid = false;
                dirty[i] = state.cache.Prepare(light, casters, force);
            }
        cpuMilliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
        for (size_t i = 0; visible && i < lights.size(); ++i)
        {
            auto& state     = states[i];
            auto& light     = lights[i];
            bool  allocated = state.output.Allocate(light.size, !light.spot);
            allocated |= state.stored.Allocate(light.size, !light.spot);
            state.reference.Allocate(light.size, !light.spot);
            if (allocated)
                dirty[i] = state.cache.Prepare(light, casters, true);
            // Separate static world layer, as used by the map's point lights.
            if (light.staticSize > 0)
            {
                state.world.Allocate(light.staticSize, !light.spot);
                state.worldReference.Allocate(light.staticSize, !light.spot);
                if (!state.worldValid)
                {
                    Bind(state.world, light, true);
                    DrawWorld();
                    state.worldValid = true;
                    ++worldUpdates;
                }
                Bind(state.worldReference, light, true);
                DrawWorld();
                Compare(state.world, state.worldReference, std::string(name) + " static");
            }
            GLuint query = 0;
            glGenQueries(1, &query);
            glBeginQuery(GL_TIME_ELAPSED, query);
            Bind(state.output, light, true);
            if (dirty[i])
            {
                ++frameUpdates;
                if (!light.staticSize) DrawWorld();
                for (const auto& caster : casters)
                    if (state.cache.Contains(caster.id))
                    {
                        Draw(cube, caster.transform.data());
                        ++frameDraws;
                    }
            }
            ShadowCache::FinishCasterDepth(state.cache, state.output.id, state.stored.id, !light.spot, light.size);
            for (const auto& caster : custom) Draw(cube, caster.transform.data());
            glEndQuery(GL_TIME_ELAPSED);
            GLuint64 elapsed = 0;
            glGetQueryObjectui64v(query, GL_QUERY_RESULT, &elapsed);
            gpuMilliseconds += static_cast<double>(elapsed) / 1000000.0;
            glDeleteQueries(1, &query);

            Bind(state.reference, light, true);
            if (!light.staticSize) DrawWorld();
            for (const auto& caster : casters)
                if (caster.id != light.source)
                {
                    Draw(cube, caster.transform.data());
                    ++referenceDraws;
                }
            for (const auto& caster : custom) Draw(cube, caster.transform.data());
            Compare(state.output, state.reference, std::string(name) + " light " + std::to_string(i));
        }
        updates += frameUpdates;
        draws += frameDraws;
        CheckGL(name);
        glfwPollEvents();
        std::printf("PASS %-32s updates=%zu/%zu caster draws=%zu\n", name, frameUpdates, visible ? lights.size() : 0, frameDraws);
        return frameUpdates;
    }
};

#include "cubemap_shadow_sampling.h"

void Run(Scene& scene)
{
    const auto initialLights = scene.lights;
    auto       nearLight     = scene.lights[0].origin;
    nearLight[0] += scene.lights[0].range * 0.4f;
    auto       player = Cube(1, nearLight);
    const auto count  = scene.lights.size();
    Require(count == scene.Frame("empty warmup", {}), "Warmup must initialize every light");
    Require(0 == scene.Frame("empty cache reuse", {}), "Empty depth must be cacheable");
    auto changed = scene.Frame("player enters", {player});
    Require(changed > 0 && changed < count, "Local player must affect a subset of lights");
    const auto playerDepth = scene.states[0].output.Read();
    Require(std::any_of(playerDepth.begin(), playerDepth.end(), [](float depth) { return depth < 1.0f; }), "Vacuous test: player produced no depth");
    Require(changed == scene.Frame("conservative animated idle", {player}), "Studio-like caster must remain volatile");
    for (int step = 1; step <= 4; ++step)
    {
        nearLight[0] += 24;
        player = Cube(1, nearLight, 16, true, step * 0.25f);
        scene.Frame("moving and rotating player", {player});
    }
    scene.Frame("player leaves all lights", {Cube(1, {100000, 100000, 100000})});
    Require(0 == scene.Frame("outside player cache reuse", {Cube(1, {100010, 100000, 100000})}), "Unrelated motion invalidated lights");
    scene.Frame("player returns", {player});
    Require(scene.Frame("player deleted", {}) > 0, "Deletion must clear old shadows");
    Require(0 == scene.Frame("after deletion reuse", {}), "Deleted player left a dirty cache");

    player.volatileGeometry = false;
    scene.Frame("stable cube enters", {player});
    Require(0 == scene.Frame("stable cube reuse", {player}), "Unchanged known geometry should reuse depth");
    auto other = Cube(2, scene.lights[0].origin, 24, false);
    scene.Frame("two casters", {player, other});
    Require(0 == scene.Frame("entity list reorder", {other, player}), "List order is not geometry");
    scene.Frame("one caster removed", {other});
    // The retained cube must survive a dirty clear/rebuild, checked by the oracle.
    other.model = 7;
    scene.Frame("same slot model replaced", {other});
    scene.Frame("visibility membership removed", {});
    scene.Frame("visibility membership restored", {other});

    Require(0 == scene.Frame("camera turns away", {other}, {}, false), "Invisible lights must defer GPU work");
    other = Cube(2, nearLight, 20, false);
    scene.Frame("motion while lights invisible", {other}, {}, false);
    scene.Frame("camera returns after motion", {other});
    Require(0 == scene.Frame("camera only movement", {other}), "Camera movement alone invalidated point shadows");
    scene.Frame("clear scene", {});

    scene.lights[0].origin[0] += 16;
    Require(1 == scene.Frame("light moves", {}), "Only moved light should update");
    scene.lights[0].range *= 0.8f;
    Require(1 == scene.Frame("light radius changes", {}), "Radius must invalidate projection");
    scene.lights[0].size /= 2;
    Require(1 == scene.Frame("shadow texture resized", {}), "Resized texture must initialize");
    scene.states[0].cache.Invalidate();
    Require(1 == scene.Frame("resource invalidation", {}), "Invalidated resource reused");
    scene.lights[0].staticSize = 0;
    scene.Frame("no static layer fallback", {player});
    scene.Frame("no static layer removal", {});
    Require(0 == scene.Frame("combined world cache reuse", {}), "World fallback cannot reuse empty entity set");
    scene.lights = initialLights;
    scene.Frame("restore fixture lights", {});

    Require(0 == scene.Frame("custom geometry appears", {}, {player}), "Custom geometry must not force entity redraw");
    Require(0 == scene.Frame("custom geometry disappears", {}), "Custom geometry polluted entity cache");
    auto unknown             = player;
    unknown.bounds.valid     = false;
    unknown.volatileGeometry = true;
    Require(count == scene.Frame("unknown bounds fallback", {unknown}), "Unknown bounds must not be culled");
    scene.Frame("unknown caster removed", {});
    Require(count == scene.Frame("cache disabled baseline", {}, {}, true, true), "Force refresh must bypass reuse");

    // The fixture contains points only. Derive one spotlight at the first
    // fixture light to exercise the other supported cache projection.
    auto& spot       = scene.lights[0];
    spot.spot        = true;
    spot.staticSize  = 0;
    spot.forward     = {1, 0, 0};
    spot.right       = {0, -1, 0};
    spot.up          = {0, 0, 1};
    spot.coneTangent = 0.5f;
    nearLight        = spot.origin;
    nearLight[0] += spot.range * 0.5f;
    player = Cube(1, nearLight, 8);
    scene.Frame("spotlight inside cone", {player});
    spot.forward = {-1, 0, 0};
    spot.right   = {0, 1, 0};
    scene.Frame("spotlight rotates away", {player});
    spot.coneTangent = 1.0f;
    scene.Frame("spotlight cone changes", {player});
    scene.Frame("spotlight caster deletion", {});
    Require(0 == scene.Frame("spotlight empty reuse", {}), "Spotlight empty cache missed");

    std::printf("Compared %zu depth pixels; caster draws optimized=%zu baseline=%zu; updates=%zu static updates=%zu.\n",
                scene.comparedPixels, scene.draws, scene.referenceDraws, scene.updates, scene.worldUpdates);
    std::printf("Cache decision CPU total %.3f ms; candidate shadow GPU total %.3f ms (diagnostics, not a game FPS benchmark).\n",
                scene.cpuMilliseconds, scene.gpuMilliseconds);
    Require(scene.draws < scene.referenceDraws, "No caster submission reduction");
}

void CheckPolicyBoundaries()
{
    Light light;
    light.range = 10;
    Bounds tangent{{10, -1, -1}, {12, 1, 1}, true};
    Require(ShadowCache::Intersects(light, tangent), "Sphere tangency was rejected");
    Bounds corner{{8, 8, 8}, {9, 9, 9}, true};
    Require(!ShadowCache::Intersects(light, corner), "Sphere/AABB broad phase is too loose");
    Bounds unknown;
    Require(ShadowCache::Intersects(light, unknown), "Unknown bounds were rejected");
    unknown         = tangent;
    unknown.mins[0] = std::numeric_limits<float>::quiet_NaN();
    Require(ShadowCache::Intersects(light, unknown), "Non-finite bounds were rejected");
    light.spot        = true;
    light.forward     = {1, 0, 0};
    light.right       = {0, 1, 0};
    light.up          = {0, 0, 1};
    light.coneTangent = 0.5f;
    Require(!ShadowCache::Intersects(light, Bounds{{-5, -1, -1}, {-3, 1, 1}, true}), "Behind-cone box was retained");
    Require(ShadowCache::Intersects(light, Bounds{{-20, -20, -20}, {20, 20, 20}, true}), "Cone-containing box was rejected");
    Require(ShadowCache::Intersects(light, Bounds{{4, 2, -1}, {5, 3, 1}, true}), "Cone boundary box was rejected");

    Cache cache;
    auto  caster = Cube(1, {5, 0, 0}, 1, false);
    Require(cache.Prepare(light, {caster}), "New cache reused uninitialized depth");
    Require(cache.Prepare(light, {caster}), "Uncommitted render was treated as valid");
    cache.Commit();
    Require(!cache.Prepare(light, {caster}), "Stable geometry invalidated");
    light.source = caster.id;
    Require(cache.Prepare(light, {caster}) && !cache.Contains(caster.id), "Source entity was not excluded");
    cache.Commit();
    Require(!cache.Prepare(light, {}), "Empty result was not cached");

    // Independent vertex samples prove the bone-box transform contains actual
    // skinned points under rotation, reflection, nonuniform scale and shear.
    const Bounds local{{-2, -3, -4}, {5, 6, 7}, true};
    const float  matrix[]    = {0, -2, 0.3f, 100, 1, 0, 0, 200, 0.2f, 0, -3, 300};
    const auto   transformed = ShadowCache::TransformBounds(local, matrix);
    Require(transformed.valid, "Finite bone transform failed");
    for (int x = 0; x <= 10; ++x)
        for (int y = 0; y <= 10; ++y)
            for (int z = 0; z <= 10; ++z)
            {
                const Vec3 point = {-2 + 0.7f * x, -3 + 0.9f * y, -4 + 1.1f * z};
                for (int axis = 0; axis < 3; ++axis)
                {
                    const float value = matrix[axis * 4] * point[0] + matrix[axis * 4 + 1] * point[1] + matrix[axis * 4 + 2] * point[2] + matrix[axis * 4 + 3];
                    Require(value >= transformed.mins[axis] - 0.0001f && value <= transformed.maxs[axis] + 0.0001f, "Bone box omitted a skinned vertex");
                }
            }
    std::puts("PASS policy and bone-bounds boundaries");
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool sampling = argc == 5 && std::string(argv[3]) == "--sampling";
    if (argc != 3 && !sampling)
    {
        std::fprintf(stderr, "Usage: gl_shadow_cache_tests <cs_assault_shadow.obj> <cs_assault_entity.txt> [--sampling <shader-directory>]\n");
        return 1;
    }
    glfwSetErrorCallback([](int code, const char* message) { std::fprintf(stderr, "GLFW %d: %s\n", code, message); });
    if (!glfwInit()) return kSkip;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    auto window = glfwCreateWindow(64, 64, "shadow cache regression", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return kSkip;
    }
    glfwMakeContextCurrent(window);
    glewExperimental = GL_TRUE;
    if (GLEW_OK != glewInit() || !glCopyImageSubData)
    {
        glfwDestroyWindow(window);
        glfwTerminate();
        return kSkip;
    }
    while (glGetError() != GL_NO_ERROR) {}
    std::printf("GPU: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    int result = 0;
    try
    {
        Scene scene(argv[1], argv[2]);
        if (sampling)
            RunShadowSampling(scene, LoadWorld(argv[1]), argv[4]);
        else
        {
            CheckPolicyBoundaries();
            Run(scene);
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        result = 1;
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    return result;
}
