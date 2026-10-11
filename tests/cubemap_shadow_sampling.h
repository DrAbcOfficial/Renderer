// Included after the shared OBJ/light/depth-rendering fixture in
// gl_shadow_cache_tests.cpp. This mode tests sampling, not cache equivalence.

float Dot(const Vec3& a, const Vec3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 Subtract(const Vec3& a, const Vec3& b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vec3 Normalize(const Vec3& value)
{
    const float length = std::sqrt(Dot(value, value));
    Require(length > 0, "Zero probe direction");
    return {value[0] / length, value[1] / length, value[2] / length};
}

struct ShadowProbe
{
    Vec3 position, normal;
};

struct RayHit
{
    float distance = std::numeric_limits<float>::infinity();
    Vec3  normal{};
};

// Independent, two-sided ray/triangle oracle. It knows nothing about cube
// faces, texture wrapping or the GPU shadow comparison implementation.
RayHit TraceWorld(const std::vector<Vec3>& vertices, const Vec3& origin, const Vec3& direction)
{
    RayHit          hit;
    constexpr float epsilon = 0.00001f;
    for (size_t i = 0; i < vertices.size(); i += 3)
    {
        const auto  e1          = Subtract(vertices[i + 1], vertices[i]);
        const auto  e2          = Subtract(vertices[i + 2], vertices[i]);
        const auto  p           = Cross(direction, e2);
        const float determinant = Dot(e1, p);
        if (std::abs(determinant) < epsilon) continue;
        const auto  t = Subtract(origin, vertices[i]);
        const float u = Dot(t, p) / determinant;
        if (u < 0 || u > 1) continue;
        const auto  q = Cross(t, e1);
        const float v = Dot(direction, q) / determinant;
        if (v < 0 || u + v > 1) continue;
        const float distance = Dot(e2, q) / determinant;
        if (distance <= epsilon || distance >= hit.distance) continue;
        hit.distance = distance;
        hit.normal   = Normalize(Cross(e1, e2));
        if (Dot(hit.normal, direction) > 0)
            for (auto& component : hit.normal) component = -component;
    }
    return hit;
}

ShadowProbe ProbeOnRay(const Light& light, Vec3 direction, float distance)
{
    direction = Normalize(direction);
    ShadowProbe probe{};
    for (int axis = 0; axis < 3; ++axis)
    {
        probe.position[axis] = light.origin[axis] + direction[axis] * distance;
        probe.normal[axis]   = -direction[axis];
    }
    return probe;
}

std::string SamplingShaderSource(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    Require(stream.good(), "Cannot open shader: " + path.string());
    std::string source, line;
    while (std::getline(stream, line))
    {
        if (line.starts_with("#version")) continue;
        if (line.starts_with("#include \""))
        {
            const auto begin = line.find('"') + 1;
            source += SamplingShaderSource(path.parent_path() / line.substr(begin, line.find('"', begin) - begin));
        }
        else
            source += line + '\n';
    }
    return source;
}

class ShadowSampler
{
    GLuint               program = 0, fbo = 0, output = 0, vao = 0, buffer = 0;
    static constexpr int capacity = 4096;

public:
    explicit ShadowSampler(const char* shaderDirectory, bool pcf)
    {
        const char* vertex = R"GLSL(#version 430 core
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
uniform float probeCount;
out vec3 probePosition, probeNormal;
void main() {
    probePosition = position; probeNormal = normal;
    gl_Position = vec4((float(gl_VertexID) + 0.5) / probeCount * 2.0 - 1.0, 0, 0, 1);
    gl_PointSize = 1.0;
}
)GLSL";
        // Compile the real production GLSL, changing only its entry point.
        // No copied implementation of CalcCubemapShadowIntensity lives here.
        std::string fragment = "#version 430 core\n#define IS_FRAGMENT_SHADER\n#define POINT_ENABLED\n"
                               "#define STATIC_CUBEMAP_SHADOW_TEXTURE_ENABLED\n#define DYNAMIC_CUBEMAP_SHADOW_TEXTURE_ENABLED\n"
                               "#define main RendererMain\n";
        if (pcf) fragment += "#define PCF_ENABLED\n";
        fragment += SamplingShaderSource(std::filesystem::path(shaderDirectory) / "dlight_shader.frag.glsl");
        fragment += R"GLSL(
#undef main
in vec3 probePosition, probeNormal;
void main() {
    vec3 direction = normalize(probePosition - u_lightpos);
    float a = CalcCubemapShadowIntensity(staticCubemapShadowTex, probePosition, u_lightpos,
        probeNormal, direction, u_staticCubemapShadowTexel);
    float b = CalcCubemapShadowIntensity(dynamicCubemapShadowTex, probePosition, u_lightpos,
        probeNormal, direction, u_dynamicCubemapShadowTexel);
    float raw = CubemapShadowCompareDepth(staticCubemapShadowTex, direction,
        length(probePosition - u_lightpos) / u_lightradius);
    out_FragColor = vec4(a, b, min(a,b), raw);
}
)GLSL";
        program = glCreateProgram();
        for (auto shader : {Compile(GL_VERTEX_SHADER, vertex), Compile(GL_FRAGMENT_SHADER, fragment.c_str())})
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
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &output);
        glBindTexture(GL_TEXTURE_2D, output);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, capacity, 1);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        Require(GL_FRAMEBUFFER_COMPLETE == glCheckFramebufferStatus(GL_FRAMEBUFFER), "Incomplete sampling framebuffer");
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(ShadowProbe), nullptr);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(ShadowProbe), reinterpret_cast<void*>(sizeof(Vec3)));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
    }
    ~ShadowSampler()
    {
        glDeleteBuffers(1, &buffer);
        glDeleteVertexArrays(1, &vao);
        glDeleteTextures(1, &output);
        glDeleteFramebuffers(1, &fbo);
        glDeleteProgram(program);
    }
    std::vector<std::array<float, 4>> Sample(const Light& light, const Texture& world, const Texture& dynamic, const std::vector<ShadowProbe>& probes)
    {
        Require(!probes.empty() && probes.size() <= capacity, "Invalid probe count");
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, static_cast<GLsizei>(probes.size()), 1);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glUseProgram(program);
        glUniform1f(glGetUniformLocation(program, "probeCount"), static_cast<float>(probes.size()));
        glUniform3fv(glGetUniformLocation(program, "u_lightpos"), 1, light.origin.data());
        glUniform1f(glGetUniformLocation(program, "u_lightradius"), light.range);
        glUniform2f(glGetUniformLocation(program, "u_staticCubemapShadowTexel"), static_cast<float>(world.size), 1.0f / world.size);
        glUniform2f(glGetUniformLocation(program, "u_dynamicCubemapShadowTexel"), static_cast<float>(dynamic.size), 1.0f / dynamic.size);
        glUniform1i(glGetUniformLocation(program, "staticCubemapShadowTex"), 0);
        glUniform1i(glGetUniformLocation(program, "dynamicCubemapShadowTex"), 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, world.id);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_CUBE_MAP, dynamic.id);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBufferData(GL_ARRAY_BUFFER, probes.size() * sizeof(ShadowProbe), probes.data(), GL_STREAM_DRAW);
        {
            const CubemapShadow::SamplingScope sampling;
            glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(probes.size()));
        }
        std::vector<std::array<float, 4>> result(probes.size());
        glReadPixels(0, 0, static_cast<GLsizei>(probes.size()), 1, GL_RGBA, GL_FLOAT, result.data());
        CheckGL("shadow sampling");
        return result;
    }
};

void AllocateShadowSamplingTexture(Texture& texture, int size)
{
    texture.size = size;
    glGenTextures(1, &texture.id);
    CubemapShadow::CreateTexture(texture.id, size, size);
}

void ClearShadowFaces(Scene& scene, const Texture& texture, const std::array<float, 6>& depths)
{
    glBindFramebuffer(GL_FRAMEBUFFER, scene.fbo);
    glDepthMask(GL_TRUE);
    for (int face = 0; face < 6; ++face)
    {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, texture.id, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        Require(GL_FRAMEBUFFER_COMPLETE == glCheckFramebufferStatus(GL_FRAMEBUFFER), "Incomplete cube face");
        glClearBufferfi(GL_DEPTH_STENCIL, 0, depths[face], 0);
    }
}

void RenderShadowWorld(Scene& scene, Texture& texture, const Light& light)
{
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    scene.Bind(texture, light, true);
    scene.DrawWorld();
    CheckGL("fixture shadow depth");
}

void CheckVisibility(float expected, float actual, const std::string& name, float tolerance = 0.0001f)
{
    Require(std::isfinite(actual) && std::abs(expected - actual) <= tolerance,
            name + ": expected visibility " + std::to_string(expected) + ", actual " + std::to_string(actual));
}

std::vector<Vec3> CubeBoundaryDirections()
{
    std::vector<Vec3> directions;
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b)
            for (int sa : {-1, 1})
                for (int sb : {-1, 1})
                {
                    Vec3 direction{};
                    direction[a] = static_cast<float>(sa);
                    direction[b] = static_cast<float>(sb);
                    directions.push_back(direction);
                }
    for (int x : {-1, 1})
        for (int y : {-1, 1})
            for (int z : {-1, 1})
            {
                Vec3 direction{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
                directions.push_back(direction);
            }
    return directions;
}

void CheckCubeBoundaries(Scene& scene, ShadowSampler& sampler, const Light& light, Texture& world, Texture& dynamic)
{
    std::vector<ShadowProbe> blocked, lit;
    for (const auto& direction : CubeBoundaryDirections())
    {
        blocked.push_back(ProbeOnRay(light, direction, light.range * 0.6f));
        lit.push_back(ProbeOnRay(light, direction, light.range * 0.1f));
    }
    ClearShadowFaces(scene, world, {0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f});
    ClearShadowFaces(scene, dynamic, {1, 1, 1, 1, 1, 1});
    for (const auto& result : sampler.Sample(light, world, dynamic, blocked))
    {
        CheckVisibility(0, result[0], "blocked cube edge/corner static");
        CheckVisibility(1, result[1], "empty dynamic layer");
        CheckVisibility(0, result[2], "combined shadow");
    }
    for (const auto& result : sampler.Sample(light, dynamic, world, blocked))
        CheckVisibility(0, result[1], "blocked cube edge/corner dynamic");
    for (const auto& result : sampler.Sample(light, world, dynamic, lit))
        CheckVisibility(1, result[2], "lit cube edge/corner");

    // A clamped but non-seamless sampler passes the uniform-depth checks above,
    // yet jumps from 0 to 1 across this boundary. Check cross-face filtering too.
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b)
            for (int sa : {-1, 1})
                for (int sb : {-1, 1})
                {
                    std::array<float, 6> depths{0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
                    depths[b * 2 + (sb < 0)] = 0.75f;
                    ClearShadowFaces(scene, world, depths);
                    std::vector<ShadowProbe> probes;
                    for (float step : {-0.02f, 0.0f, 0.02f})
                    {
                        Vec3 direction{};
                        direction[a] = sa * (1.0f + step / world.size);
                        direction[b] = static_cast<float>(sb);
                        probes.push_back(ProbeOnRay(light, direction, light.range * 0.5f));
                    }
                    for (const auto& value : sampler.Sample(light, world, dynamic, probes))
                        CheckVisibility(0.5f, value[3], "mixed-depth edge continuity", 0.03f);
                }
    for (int x : {-1, 1})
        for (int y : {-1, 1})
            for (int z : {-1, 1})
            {
                std::array<float, 6> depths{0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
                depths[x < 0] = 0.75f;
                ClearShadowFaces(scene, world, depths);
                std::vector<ShadowProbe> probes;
                for (int dominant = 0; dominant < 3; ++dominant)
                {
                    Vec3 direction{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
                    direction[dominant] *= 1.0f + 0.02f / world.size;
                    probes.push_back(ProbeOnRay(light, direction, light.range * 0.5f));
                }
                for (const auto& value : sampler.Sample(light, world, dynamic, probes))
                    CheckVisibility(1.0f / 3.0f, value[3], "mixed-depth corner continuity", 0.04f);
            }
    std::puts("PASS all 12 edges / 8 corners: blocked, lit, static/dynamic layers and cross-face continuity");
}

struct MapBoundaryProbes
{
    std::vector<ShadowProbe> blocked, lit;
};

std::vector<MapBoundaryProbes> BuildMapBoundaryProbes(const Scene& scene, const std::vector<Vec3>& vertices)
{
    const auto                     directions = CubeBoundaryDirections();
    std::array<bool, 20>           covered{};
    std::vector<MapBoundaryProbes> result(scene.lights.size());
    size_t                         blockedCount = 0;
    for (size_t index = 0; index < scene.lights.size(); ++index)
    {
        const auto& light = scene.lights[index];
        for (size_t boundary = 0; boundary < directions.size(); ++boundary)
        {
            const auto  direction = Normalize(directions[boundary]);
            const auto  hit       = TraceWorld(vertices, light.origin, direction);
            const float receiver  = (std::min)(hit.distance + light.range * 0.15f, light.range * 0.85f);
            if (hit.distance < 0.2f || hit.distance + 8 >= receiver) continue;
            // Require a blocker across a neighborhood wider than the actual
            // PCF + bilinear footprint. Real geometric penumbras are not leaks.
            bool solidNeighborhood = true;
            for (int x : {-1, 1})
                for (int y : {-1, 1})
                    for (int z : {-1, 1})
                    {
                        const float offset   = 2.0f / light.staticSize;
                        const auto  neighbor = Normalize(Vec3{direction[0] + x * offset, direction[1] + y * offset, direction[2] + z * offset});
                        if (TraceWorld(vertices, light.origin, neighbor).distance + 8 >= receiver)
                            solidNeighborhood = false;
                    }
            if (!solidNeighborhood) continue;
            covered[boundary] = true;
            result[index].blocked.push_back(ProbeOnRay(light, direction, receiver));
            result[index].lit.push_back(ProbeOnRay(light, direction, hit.distance * 0.5f));
            ++blockedCount;
        }
    }
    Require(std::all_of(covered.begin(), covered.end(), [](bool value) { return value; }),
            "Map fixture did not exercise every signed edge and corner");
    std::printf("Independent OBJ rays selected %zu blocked/lit pairs across %zu map lights, covering all 12 edges / 8 corners.\n",
                blockedCount, scene.lights.size());
    return result;
}

void CheckMapBoundaries(Scene& scene, ShadowSampler& sampler, const std::vector<MapBoundaryProbes>& probes)
{
    for (size_t index = 0; index < scene.lights.size(); ++index)
    {
        if (probes[index].blocked.empty()) continue;
        const auto& light = scene.lights[index];
        Texture     world, dynamic;
        AllocateShadowSamplingTexture(world, light.staticSize);
        AllocateShadowSamplingTexture(dynamic, light.size);
        RenderShadowWorld(scene, world, light);
        ClearShadowFaces(scene, dynamic, {1, 1, 1, 1, 1, 1});
        for (const auto& value : sampler.Sample(light, world, dynamic, probes[index].blocked))
            CheckVisibility(0, value[2], "map boundary blocked, light " + std::to_string(index));
        for (const auto& value : sampler.Sample(light, world, dynamic, probes[index].lit))
            CheckVisibility(1, value[2], "map boundary lit, light " + std::to_string(index));
    }
    std::puts("PASS real OBJ/light fixture boundaries: occluded and unoccluded controls");
}

void RunShadowSampling(Scene& scene, const std::vector<Vec3>& vertices, const char* shaderDirectory)
{
    const Vec3 camera{-143, 2468, 558};
    const Vec3 offendingLight{-271, 2448, 380};
    const auto found = std::find_if(scene.lights.begin(), scene.lights.end(), [&](const Light& light) {
        const auto delta = Subtract(light.origin, offendingLight);
        return Dot(delta, delta) < 0.01f;
    });
    Require(found != scene.lights.end(), "Issue #15 light is missing from the fixture");
    const auto& light = *found;
    Texture     world, dynamic;
    AllocateShadowSamplingTexture(world, light.staticSize);
    AllocateShadowSamplingTexture(dynamic, light.size);
    std::vector<ShadowProbe> probes;
    // Camera rays around the captured 800x600 / 90-degree horizontal-FOV seam.
    // Intersect the loaded OBJ; never substitute a made-up receiving plane.
    for (const auto& pixel : std::vector<std::array<float, 2>>{{400, 136}, {400, 137}, {400, 138}, {400, 139}, {128, 460}, {190, 332}, {250, 208}})
    {
        const auto direction = Normalize(Vec3{1, -(pixel[0] + 0.5f - 400) / 400, (300 - pixel[1] - 0.5f) / 400});
        const auto hit       = TraceWorld(vertices, camera, direction);
        Require(std::isfinite(hit.distance), "Camera ray missed the fixture");
        auto probe                   = ProbeOnRay(Light{camera}, direction, hit.distance);
        probe.normal                 = hit.normal;
        const auto  toReceiver       = Subtract(probe.position, light.origin);
        const float receiverDistance = std::sqrt(Dot(toReceiver, toReceiver));
        const auto  blocker          = TraceWorld(vertices, light.origin, Normalize(toReceiver));
        Require(blocker.distance + 8 < receiverDistance && receiverDistance < light.range,
                "Issue probe must be inside the light and behind a real OBJ blocker");
        probes.push_back(probe);
        std::printf("Issue probe (%g,%g): world=(%.3f,%.3f,%.3f), blocker=%.3f receiver=%.3f\n",
                    pixel[0], pixel[1], probe.position[0], probe.position[1], probe.position[2], blocker.distance, receiverDistance);
    }
    const auto mapProbes = BuildMapBoundaryProbes(scene, vertices);
    for (bool pcf : {false, true})
    {
        ShadowSampler sampler(shaderDirectory, pcf);
        RenderShadowWorld(scene, world, light);
        ClearShadowFaces(scene, dynamic, {1, 1, 1, 1, 1, 1});
        glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
        const auto values = sampler.Sample(light, world, dynamic, probes);
        for (size_t i = 0; i < values.size(); ++i)
            CheckVisibility(0, values[i][2], "cs_assault issue #15 probe " + std::to_string(i));
        Require(!glIsEnabled(GL_TEXTURE_CUBE_MAP_SEAMLESS), "Sampling leaked global state");
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
        sampler.Sample(light, world, dynamic, probes);
        Require(glIsEnabled(GL_TEXTURE_CUBE_MAP_SEAMLESS), "Sampling disabled inherited seamless state");
        glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
        CheckCubeBoundaries(scene, sampler, light, world, dynamic);
        CheckMapBoundaries(scene, sampler, mapProbes);
        std::printf("PASS cs_assault issue #15, production sampler, PCF=%d\n", pcf);
    }
}
