#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/norm.hpp>
#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <vector>

namespace neon {

constexpr float kPi = 3.14159265358979323846f;

float saturate(float value) { return glm::clamp(value, 0.0f, 1.0f); }

struct AABB {
    glm::vec3 min{};
    glm::vec3 max{};
};

bool rayAABB(const glm::vec3& origin, const glm::vec3& direction, const AABB& box, float& distance) {
    float nearHit = 0.0f;
    float farHit = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1.0e-6f) {
            if (origin[axis] < box.min[axis] || origin[axis] > box.max[axis]) return false;
            continue;
        }
        const float inverse = 1.0f / direction[axis];
        float first = (box.min[axis] - origin[axis]) * inverse;
        float second = (box.max[axis] - origin[axis]) * inverse;
        if (first > second) std::swap(first, second);
        nearHit = std::max(nearHit, first);
        farHit = std::min(farHit, second);
        if (nearHit > farHit) return false;
    }
    distance = nearHit;
    return farHit >= 0.0f;
}

bool raySphere(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& center,
               float radius, float& distance) {
    const glm::vec3 offset = origin - center;
    const float b = glm::dot(offset, direction);
    const float c = glm::dot(offset, offset) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f) return false;
    const float root = std::sqrt(discriminant);
    const float nearHit = -b - root;
    const float farHit = -b + root;
    if (farHit < 0.0f) return false;
    distance = nearHit >= 0.0f ? nearHit : farHit;
    return true;
}

GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint okay = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &okay);
    if (okay == GL_FALSE) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error("OpenGL shader compile failed: " + log);
    }
    return shader;
}

GLuint createProgram(const char* vertexSource, const char* fragmentSource) {
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint okay = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &okay);
    if (okay == GL_FALSE) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
        glGetProgramInfoLog(program, length, nullptr, log.data());
        glDeleteProgram(program);
        throw std::runtime_error("OpenGL program link failed: " + log);
    }
    return program;
}

struct Vertex {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{};
};

class Mesh {
public:
    Mesh() = default;
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    Mesh(Mesh&& other) noexcept { moveFrom(other); }
    Mesh& operator=(Mesh&& other) noexcept {
        if (this != &other) {
            destroy();
            moveFrom(other);
        }
        return *this;
    }

    ~Mesh() { destroy(); }

    void upload(std::span<const Vertex> vertices, std::span<const std::uint32_t> indices) {
        destroy();
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glGenBuffers(1, &ebo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size_bytes()), vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size_bytes()), indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void*>(offsetof(Vertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void*>(offsetof(Vertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void*>(offsetof(Vertex, uv)));
        glBindVertexArray(0);
        indexCount_ = static_cast<GLsizei>(indices.size());
    }

    void draw() const {
        if (vao_ == 0 || indexCount_ == 0) return;
        glBindVertexArray(vao_);
        glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    }

    bool valid() const { return vao_ != 0 && indexCount_ > 0; }

private:
    void destroy() {
        if (ebo_ != 0) glDeleteBuffers(1, &ebo_);
        if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
        if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
        vao_ = vbo_ = ebo_ = 0;
        indexCount_ = 0;
    }

    void moveFrom(Mesh& other) {
        vao_ = std::exchange(other.vao_, 0);
        vbo_ = std::exchange(other.vbo_, 0);
        ebo_ = std::exchange(other.ebo_, 0);
        indexCount_ = std::exchange(other.indexCount_, 0);
    }

    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    GLsizei indexCount_ = 0;
};

Mesh makeCube() {
    const std::array<glm::vec3, 8> corners = {{
        {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
        {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f}
    }};
    struct Face { std::array<int, 4> corner; glm::vec3 normal; };
    const std::array<Face, 6> faces = {{
        {{{0, 3, 2, 1}}, {0, 0, -1}}, {{{5, 6, 7, 4}}, {0, 0, 1}},
        {{{4, 7, 3, 0}}, {-1, 0, 0}}, {{{1, 2, 6, 5}}, {1, 0, 0}},
        {{{3, 7, 6, 2}}, {0, 1, 0}}, {{{4, 0, 1, 5}}, {0, -1, 0}}
    }};
    const std::array<glm::vec2, 4> uvs = {{{0, 0}, {0, 1}, {1, 1}, {1, 0}}};
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(24);
    indices.reserve(36);
    for (const auto& face : faces) {
        const auto base = static_cast<std::uint32_t>(vertices.size());
        for (int i = 0; i < 4; ++i) vertices.push_back({corners[face.corner[i]], face.normal, uvs[i]});
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    Mesh result;
    result.upload(vertices, indices);
    return result;
}

Mesh makeCylinder(int segments = 20) {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    for (int i = 0; i <= segments; ++i) {
        const float angle = (static_cast<float>(i) / static_cast<float>(segments)) * 2.0f * kPi;
        const float x = std::cos(angle) * 0.5f;
        const float z = std::sin(angle) * 0.5f;
        const glm::vec3 normal = glm::normalize(glm::vec3{x, 0.0f, z});
        vertices.push_back({{x, -0.5f, z}, normal, {static_cast<float>(i) / segments, 0}});
        vertices.push_back({{x, 0.5f, z}, normal, {static_cast<float>(i) / segments, 1}});
    }
    for (int i = 0; i < segments; ++i) {
        const std::uint32_t base = static_cast<std::uint32_t>(i * 2);
        indices.insert(indices.end(), {base, base + 1, base + 3, base, base + 3, base + 2});
    }
    const auto bottomCenter = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back({{0, -0.5f, 0}, {0, -1, 0}, {0.5f, 0.5f}});
    const auto topCenter = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back({{0, 0.5f, 0}, {0, 1, 0}, {0.5f, 0.5f}});
    const auto rings = static_cast<std::uint32_t>(vertices.size());
    for (int i = 0; i < segments; ++i) {
        const float angle = (static_cast<float>(i) / static_cast<float>(segments)) * 2.0f * kPi;
        const float x = std::cos(angle) * 0.5f;
        const float z = std::sin(angle) * 0.5f;
        vertices.push_back({{x, -0.5f, z}, {0, -1, 0}, {x + 0.5f, z + 0.5f}});
        vertices.push_back({{x, 0.5f, z}, {0, 1, 0}, {x + 0.5f, z + 0.5f}});
    }
    for (int i = 0; i < segments; ++i) {
        const auto current = rings + static_cast<std::uint32_t>(i * 2);
        const auto next = rings + static_cast<std::uint32_t>(((i + 1) % segments) * 2);
        indices.insert(indices.end(), {bottomCenter, next, current});
        indices.insert(indices.end(), {topCenter, current + 1, next + 1});
    }
    Mesh result;
    result.upload(vertices, indices);
    return result;
}

class Renderer {
public:
    Renderer() {
        static constexpr const char* sceneVertex = R"GLSL(
            #version 330 core
            layout(location=0) in vec3 aPosition;
            layout(location=1) in vec3 aNormal;
            layout(location=2) in vec2 aUV;
            uniform mat4 uViewProjection;
            uniform mat4 uModel;
            out vec3 vWorldPosition;
            out vec3 vNormal;
            out vec2 vUV;
            void main() {
                vec4 world = uModel * vec4(aPosition, 1.0);
                vWorldPosition = world.xyz;
                vNormal = normalize(mat3(transpose(inverse(uModel))) * aNormal);
                vUV = aUV;
                gl_Position = uViewProjection * world;
            }
        )GLSL";
        static constexpr const char* sceneFragment = R"GLSL(
            #version 330 core
            in vec3 vWorldPosition;
            in vec3 vNormal;
            in vec2 vUV;
            uniform vec4 uColor;
            uniform sampler2D uTexture;
            uniform int uUseTexture;
            uniform float uEmissive;
            uniform vec3 uCameraPosition;
            out vec4 FragColor;
            void main() {
                vec4 texel = uUseTexture != 0 ? texture(uTexture, vUV) : vec4(1.0);
                if (texel.a < 0.08) discard;
                vec3 base = uColor.rgb * texel.rgb;
                vec3 n = normalize(vNormal);
                vec3 lightDir = normalize(vec3(-0.45, 0.86, -0.25));
                float diffuse = max(dot(n, lightDir), 0.0);
                float hemi = n.y * 0.5 + 0.5;
                vec3 viewDir = normalize(uCameraPosition - vWorldPosition);
                vec3 halfDir = normalize(lightDir + viewDir);
                float specular = pow(max(dot(n, halfDir), 0.0), 40.0) * 0.2;
                float rim = pow(1.0 - max(dot(n, viewDir), 0.0), 3.0) * 0.16;
                vec3 lit = base * (0.15 + diffuse * 0.74 + hemi * 0.18) + specular + rim * base;
                lit += base * uEmissive;
                float fog = smoothstep(28.0, 61.0, distance(uCameraPosition, vWorldPosition));
                vec3 fogColor = vec3(0.012, 0.022, 0.055);
                lit = mix(lit, fogColor, fog * 0.88);
                lit = lit / (lit + vec3(1.0));
                lit = pow(lit, vec3(1.0 / 2.2));
                FragColor = vec4(lit, uColor.a * texel.a);
            }
        )GLSL";
        sceneProgram_ = createProgram(sceneVertex, sceneFragment);

        static constexpr const char* uiVertex = R"GLSL(
            #version 330 core
            layout(location=0) in vec2 aPosition;
            layout(location=1) in vec4 aColor;
            uniform vec2 uResolution;
            out vec4 vColor;
            void main() {
                vec2 ndc = vec2(aPosition.x / uResolution.x * 2.0 - 1.0,
                                1.0 - aPosition.y / uResolution.y * 2.0);
                gl_Position = vec4(ndc, 0.0, 1.0);
                vColor = aColor;
            }
        )GLSL";
        static constexpr const char* uiFragment = R"GLSL(
            #version 330 core
            in vec4 vColor;
            out vec4 FragColor;
            void main() { FragColor = vColor; }
        )GLSL";
        uiProgram_ = createProgram(uiVertex, uiFragment);
        glGenVertexArrays(1, &uiVao_);
        glGenBuffers(1, &uiVbo_);
        glBindVertexArray(uiVao_);
        glBindBuffer(GL_ARRAY_BUFFER, uiVbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UIVertex),
                              reinterpret_cast<void*>(offsetof(UIVertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(UIVertex),
                              reinterpret_cast<void*>(offsetof(UIVertex, color)));
        glBindVertexArray(0);
    }

    ~Renderer() {
        if (uiVbo_) glDeleteBuffers(1, &uiVbo_);
        if (uiVao_) glDeleteVertexArrays(1, &uiVao_);
        if (uiProgram_) glDeleteProgram(uiProgram_);
        if (sceneProgram_) glDeleteProgram(sceneProgram_);
    }

    void beginFrame(int width, int height) {
        width_ = std::max(width, 1);
        height_ = std::max(height, 1);
        glViewport(0, 0, width_, height_);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glDisable(GL_BLEND);
        glClearColor(0.009f, 0.017f, 0.045f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        uiVertices_.clear();
    }

    void setCamera(const glm::mat4& viewProjection, const glm::vec3& position) {
        viewProjection_ = viewProjection;
        cameraPosition_ = position;
    }

    void draw(const Mesh& mesh, const glm::mat4& model, const glm::vec4& color,
              GLuint texture = 0, float emissive = 0.0f) {
        if (!mesh.valid()) return;
        glUseProgram(sceneProgram_);
        glUniformMatrix4fv(glGetUniformLocation(sceneProgram_, "uViewProjection"), 1, GL_FALSE,
                           glm::value_ptr(viewProjection_));
        glUniformMatrix4fv(glGetUniformLocation(sceneProgram_, "uModel"), 1, GL_FALSE, glm::value_ptr(model));
        glUniform4fv(glGetUniformLocation(sceneProgram_, "uColor"), 1, glm::value_ptr(color));
        glUniform3fv(glGetUniformLocation(sceneProgram_, "uCameraPosition"), 1, glm::value_ptr(cameraPosition_));
        glUniform1f(glGetUniformLocation(sceneProgram_, "uEmissive"), emissive);
        glUniform1i(glGetUniformLocation(sceneProgram_, "uUseTexture"), texture != 0 ? 1 : 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(glGetUniformLocation(sceneProgram_, "uTexture"), 0);
        mesh.draw();
    }

    void rect(float x, float y, float width, float height, const glm::vec4& color) {
        const std::array<glm::vec2, 4> point = {{{x, y}, {x + width, y}, {x + width, y + height}, {x, y + height}}};
        const std::array<int, 6> order = {{0, 1, 2, 0, 2, 3}};
        for (int index : order) uiVertices_.push_back({point[index], color});
    }

    void text(float x, float y, std::string_view value, float scale, const glm::vec4& color,
              bool centered = false) {
        if (centered) x -= measureText(value, scale) * 0.5f;
        const float pixel = std::max(scale, 1.0f);
        for (char raw : value) {
            const char character = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
            const auto rows = glyph(character);
            for (int row = 0; row < 7; ++row) {
                for (int column = 0; column < 5; ++column) {
                    if ((rows[row] & (1u << (4 - column))) != 0u) {
                        rect(x + column * pixel, y + row * pixel, pixel + 0.35f, pixel + 0.35f, color);
                    }
                }
            }
            x += pixel * 6.0f;
        }
    }

    float measureText(std::string_view value, float scale) const {
        return static_cast<float>(value.size()) * std::max(scale, 1.0f) * 6.0f;
    }

    void flushUI() {
        if (uiVertices_.empty()) return;
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(uiProgram_);
        glUniform2f(glGetUniformLocation(uiProgram_, "uResolution"), static_cast<float>(width_),
                    static_cast<float>(height_));
        glBindVertexArray(uiVao_);
        glBindBuffer(GL_ARRAY_BUFFER, uiVbo_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(uiVertices_.size() * sizeof(UIVertex)),
                     uiVertices_.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(uiVertices_.size()));
        glBindVertexArray(0);
        glDisable(GL_BLEND);
    }

    int width() const { return width_; }
    int height() const { return height_; }

private:
    struct UIVertex { glm::vec2 position; glm::vec4 color; };

    static std::array<std::uint8_t, 7> glyph(char c) {
        using G = std::array<std::uint8_t, 7>;
        switch (c) {
            case 'A': return G{14,17,17,31,17,17,17}; case 'B': return G{30,17,17,30,17,17,30};
            case 'C': return G{14,17,16,16,16,17,14}; case 'D': return G{30,17,17,17,17,17,30};
            case 'E': return G{31,16,16,30,16,16,31}; case 'F': return G{31,16,16,30,16,16,16};
            case 'G': return G{14,17,16,23,17,17,15}; case 'H': return G{17,17,17,31,17,17,17};
            case 'I': return G{31,4,4,4,4,4,31}; case 'J': return G{7,2,2,2,2,18,12};
            case 'K': return G{17,18,20,24,20,18,17}; case 'L': return G{16,16,16,16,16,16,31};
            case 'M': return G{17,27,21,21,17,17,17}; case 'N': return G{17,25,21,19,17,17,17};
            case 'O': return G{14,17,17,17,17,17,14}; case 'P': return G{30,17,17,30,16,16,16};
            case 'Q': return G{14,17,17,17,21,18,13}; case 'R': return G{30,17,17,30,20,18,17};
            case 'S': return G{15,16,16,14,1,1,30}; case 'T': return G{31,4,4,4,4,4,4};
            case 'U': return G{17,17,17,17,17,17,14}; case 'V': return G{17,17,17,17,17,10,4};
            case 'W': return G{17,17,17,21,21,21,10}; case 'X': return G{17,17,10,4,10,17,17};
            case 'Y': return G{17,17,10,4,4,4,4}; case 'Z': return G{31,1,2,4,8,16,31};
            case '0': return G{14,17,19,21,25,17,14}; case '1': return G{4,12,4,4,4,4,14};
            case '2': return G{14,17,1,2,4,8,31}; case '3': return G{30,1,1,14,1,1,30};
            case '4': return G{2,6,10,18,31,2,2}; case '5': return G{31,16,16,30,1,1,30};
            case '6': return G{14,16,16,30,17,17,14}; case '7': return G{31,1,2,4,8,8,8};
            case '8': return G{14,17,17,14,17,17,14}; case '9': return G{14,17,17,15,1,1,14};
            case ':': return G{0,4,4,0,4,4,0}; case '-': return G{0,0,0,31,0,0,0};
            case '/': return G{1,1,2,4,8,16,16}; case '.': return G{0,0,0,0,0,12,12};
            case '+': return G{0,4,4,31,4,4,0}; case '!': return G{4,4,4,4,4,0,4};
            default: return G{0,0,0,0,0,0,0};
        }
    }

    GLuint sceneProgram_ = 0;
    GLuint uiProgram_ = 0;
    GLuint uiVao_ = 0;
    GLuint uiVbo_ = 0;
    glm::mat4 viewProjection_{1.0f};
    glm::vec3 cameraPosition_{};
    std::vector<UIVertex> uiVertices_;
    int width_ = 1;
    int height_ = 1;
};

class GltfModel {
public:
    GltfModel() = default;
    GltfModel(const GltfModel&) = delete;
    GltfModel& operator=(const GltfModel&) = delete;

    ~GltfModel() {
        for (GLuint texture : textures_) {
            if (texture != 0) glDeleteTextures(1, &texture);
        }
    }

    bool load(const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) {
            SDL_Log("Optional model not found: %s", path.string().c_str());
            return false;
        }

        tinygltf::TinyGLTF loader;
        tinygltf::Model model;
        std::string warning;
        std::string error;
        const bool okay = path.extension() == ".glb"
            ? loader.LoadBinaryFromFile(&model, &error, &warning, path.string())
            : loader.LoadASCIIFromFile(&model, &error, &warning, path.string());
        if (!warning.empty()) SDL_Log("glTF warning: %s", warning.c_str());
        if (!okay) {
            SDL_Log("Could not load %s: %s", path.string().c_str(), error.c_str());
            return false;
        }

        textures_.assign(model.images.size(), 0);
        std::vector<bool> neededImages(model.images.size(), false);
        for (const auto& material : model.materials) {
            const int textureIndex = material.pbrMetallicRoughness.baseColorTexture.index;
            if (textureIndex >= 0 && textureIndex < static_cast<int>(model.textures.size())) {
                const int imageIndex = model.textures[static_cast<std::size_t>(textureIndex)].source;
                if (imageIndex >= 0 && imageIndex < static_cast<int>(neededImages.size()))
                    neededImages[static_cast<std::size_t>(imageIndex)] = true;
            }
        }
        for (std::size_t i = 0; i < model.images.size(); ++i) {
            if (!neededImages[i]) continue;
            const tinygltf::Image& image = model.images[i];
            if (image.image.empty() || image.width <= 0 || image.height <= 0) continue;
            GLenum format = GL_RGBA;
            GLenum internal = GL_RGBA8;
            if (image.component == 1) { format = GL_RED; internal = GL_R8; }
            else if (image.component == 2) { format = GL_RG; internal = GL_RG8; }
            else if (image.component == 3) { format = GL_RGB; internal = GL_RGB8; }
            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            const GLenum pixelType = image.bits == 16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
            int uploadWidth = image.width;
            int uploadHeight = image.height;
            const unsigned char* uploadPixels = image.image.data();
            std::vector<unsigned char> reducedPixels;
            constexpr int maximumModelTexture = 1024;
            if (image.bits == 8 && std::max(image.width, image.height) > maximumModelTexture) {
                const float scale = static_cast<float>(maximumModelTexture) /
                                    static_cast<float>(std::max(image.width, image.height));
                uploadWidth = std::max(1, static_cast<int>(image.width * scale));
                uploadHeight = std::max(1, static_cast<int>(image.height * scale));
                reducedPixels.resize(static_cast<std::size_t>(uploadWidth * uploadHeight * image.component));
                for (int y = 0; y < uploadHeight; ++y) {
                    const int sourceY = std::min(image.height - 1, y * image.height / uploadHeight);
                    for (int x = 0; x < uploadWidth; ++x) {
                        const int sourceX = std::min(image.width - 1, x * image.width / uploadWidth);
                        for (int component = 0; component < image.component; ++component) {
                            reducedPixels[static_cast<std::size_t>((y * uploadWidth + x) * image.component + component)] =
                                image.image[static_cast<std::size_t>((sourceY * image.width + sourceX) *
                                                                   image.component + component)];
                        }
                    }
                }
                uploadPixels = reducedPixels.data();
            }
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal), uploadWidth, uploadHeight, 0,
                         format, pixelType, uploadPixels);
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            textures_[i] = texture;
        }

        const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
        if (sceneIndex < 0) return false;
        for (int node : model.scenes[static_cast<std::size_t>(sceneIndex)].nodes) {
            visitNode(model, node, glm::mat4(1.0f));
        }
        return !parts_.empty();
    }

    void draw(Renderer& renderer, const glm::mat4& transform, const glm::vec4& tint = glm::vec4(1.0f)) const {
        for (const Part& part : parts_) {
            const glm::vec4 color = part.color * tint;
            renderer.draw(part.mesh, transform * part.transform, color, part.texture);
        }
    }

    bool valid() const { return !parts_.empty(); }

private:
    struct Part {
        Mesh mesh;
        glm::mat4 transform{1.0f};
        glm::vec4 color{1.0f};
        GLuint texture = 0;
    };

    static glm::mat4 nodeMatrix(const tinygltf::Node& node) {
        if (node.matrix.size() == 16) {
            glm::mat4 result(1.0f);
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    result[column][row] = static_cast<float>(node.matrix[static_cast<std::size_t>(column * 4 + row)]);
                }
            }
            return result;
        }
        glm::vec3 translation(0.0f);
        glm::vec3 scale(1.0f);
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        if (node.translation.size() == 3) {
            translation = {static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]),
                           static_cast<float>(node.translation[2])};
        }
        if (node.scale.size() == 3) {
            scale = {static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]),
                     static_cast<float>(node.scale[2])};
        }
        if (node.rotation.size() == 4) {
            rotation = {static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]),
                        static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2])};
        }
        return glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) *
               glm::scale(glm::mat4(1.0f), scale);
    }

    static const unsigned char* accessorData(const tinygltf::Model& model, const tinygltf::Accessor& accessor) {
        const auto& view = model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
        const auto& buffer = model.buffers[static_cast<std::size_t>(view.buffer)];
        return buffer.data.data() + view.byteOffset + accessor.byteOffset;
    }

    static glm::vec3 readVec3(const tinygltf::Model& model, const tinygltf::Accessor& accessor,
                              std::size_t index) {
        const auto& view = model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
        const std::size_t stride = accessor.ByteStride(view) != 0 ? accessor.ByteStride(view) : sizeof(float) * 3;
        const auto* value = reinterpret_cast<const float*>(accessorData(model, accessor) + index * stride);
        return {value[0], value[1], value[2]};
    }

    static glm::vec2 readVec2(const tinygltf::Model& model, const tinygltf::Accessor& accessor,
                              std::size_t index) {
        const auto& view = model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
        const std::size_t stride = accessor.ByteStride(view) != 0 ? accessor.ByteStride(view) : sizeof(float) * 2;
        const auto* value = reinterpret_cast<const float*>(accessorData(model, accessor) + index * stride);
        return {value[0], value[1]};
    }

    static std::uint32_t readIndex(const tinygltf::Model& model, const tinygltf::Accessor& accessor,
                                   std::size_t index) {
        const auto& view = model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
        const std::size_t componentSize = tinygltf::GetComponentSizeInBytes(accessor.componentType);
        const std::size_t stride = accessor.ByteStride(view) != 0 ? accessor.ByteStride(view) : componentSize;
        const unsigned char* data = accessorData(model, accessor) + index * stride;
        switch (accessor.componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return *data;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return *reinterpret_cast<const std::uint16_t*>(data);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: return *reinterpret_cast<const std::uint32_t*>(data);
            default: return 0;
        }
    }

    void visitNode(const tinygltf::Model& model, int nodeIndex, const glm::mat4& parent) {
        const tinygltf::Node& node = model.nodes[static_cast<std::size_t>(nodeIndex)];
        const glm::mat4 world = parent * nodeMatrix(node);
        if (node.mesh >= 0) {
            const tinygltf::Mesh& sourceMesh = model.meshes[static_cast<std::size_t>(node.mesh)];
            for (const tinygltf::Primitive& primitive : sourceMesh.primitives) {
                if (primitive.mode != TINYGLTF_MODE_TRIANGLES && primitive.mode != -1) continue;
                const auto positionIt = primitive.attributes.find("POSITION");
                if (positionIt == primitive.attributes.end()) continue;
                const auto& positions = model.accessors[static_cast<std::size_t>(positionIt->second)];
                const tinygltf::Accessor* normals = nullptr;
                const tinygltf::Accessor* uvs = nullptr;
                if (const auto it = primitive.attributes.find("NORMAL"); it != primitive.attributes.end())
                    normals = &model.accessors[static_cast<std::size_t>(it->second)];
                if (const auto it = primitive.attributes.find("TEXCOORD_0"); it != primitive.attributes.end())
                    uvs = &model.accessors[static_cast<std::size_t>(it->second)];

                std::vector<Vertex> vertices(positions.count);
                for (std::size_t i = 0; i < positions.count; ++i) {
                    vertices[i].position = readVec3(model, positions, i);
                    if (normals) vertices[i].normal = readVec3(model, *normals, i);
                    if (uvs) vertices[i].uv = readVec2(model, *uvs, i);
                }
                std::vector<std::uint32_t> indices;
                if (primitive.indices >= 0) {
                    const auto& accessor = model.accessors[static_cast<std::size_t>(primitive.indices)];
                    indices.resize(accessor.count);
                    for (std::size_t i = 0; i < accessor.count; ++i) indices[i] = readIndex(model, accessor, i);
                } else {
                    indices.resize(vertices.size());
                    for (std::size_t i = 0; i < vertices.size(); ++i) indices[i] = static_cast<std::uint32_t>(i);
                }

                Part part;
                part.mesh.upload(vertices, indices);
                part.transform = world;
                if (primitive.material >= 0) {
                    const auto& material = model.materials[static_cast<std::size_t>(primitive.material)];
                    const auto& factor = material.pbrMetallicRoughness.baseColorFactor;
                    if (factor.size() == 4) {
                        part.color = {static_cast<float>(factor[0]), static_cast<float>(factor[1]),
                                      static_cast<float>(factor[2]), static_cast<float>(factor[3])};
                    }
                    const int textureIndex = material.pbrMetallicRoughness.baseColorTexture.index;
                    if (textureIndex >= 0 && textureIndex < static_cast<int>(model.textures.size())) {
                        const int imageIndex = model.textures[static_cast<std::size_t>(textureIndex)].source;
                        if (imageIndex >= 0 && imageIndex < static_cast<int>(textures_.size()))
                            part.texture = textures_[static_cast<std::size_t>(imageIndex)];
                    }
                }
                parts_.push_back(std::move(part));
            }
        }
        for (int child : node.children) visitNode(model, child, world);
    }

    std::vector<Part> parts_;
    std::vector<GLuint> textures_;
};

enum class SoundEffect { Pistol, Rifle, Shotgun, Enemy, Hit, Kill, Pickup, Reload, Empty, Start };

class AudioEngine {
public:
    AudioEngine() {
        SDL_AudioSpec specification{};
        specification.freq = 48000;
        specification.format = SDL_AUDIO_F32;
        specification.channels = 2;
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &specification, &AudioEngine::callback, this);
        if (stream_ != nullptr) SDL_ResumeAudioStreamDevice(stream_);
        else SDL_Log("Audio disabled: %s", SDL_GetError());
    }

    ~AudioEngine() {
        if (stream_ != nullptr) SDL_DestroyAudioStream(stream_);
    }

    void play(SoundEffect effect) {
        std::scoped_lock lock(mutex_);
        switch (effect) {
            case SoundEffect::Pistol: addVoice(130.0f, 0.11f, 0.52f, 0.7f, -0.65f); break;
            case SoundEffect::Rifle: addVoice(105.0f, 0.075f, 0.40f, 0.82f, -0.45f); break;
            case SoundEffect::Shotgun:
                addVoice(72.0f, 0.22f, 0.72f, 0.92f, -0.72f);
                addVoice(44.0f, 0.18f, 0.42f, 0.15f, -0.35f);
                break;
            case SoundEffect::Enemy: addVoice(155.0f, 0.08f, 0.25f, 0.65f, -0.30f); break;
            case SoundEffect::Hit: addVoice(690.0f, 0.045f, 0.18f, 0.05f, 0.10f); break;
            case SoundEffect::Kill:
                addVoice(480.0f, 0.09f, 0.18f, 0.02f, 0.45f);
                addVoice(760.0f, 0.13f, 0.13f, 0.02f, 0.25f);
                break;
            case SoundEffect::Pickup: addVoice(920.0f, 0.18f, 0.17f, 0.01f, 0.35f); break;
            case SoundEffect::Reload:
                addVoice(260.0f, 0.08f, 0.12f, 0.3f, -0.1f);
                addVoice(390.0f, 0.07f, 0.09f, 0.2f, 0.2f, 0.13f);
                break;
            case SoundEffect::Empty: addVoice(1700.0f, 0.025f, 0.08f, 0.1f, -0.2f); break;
            case SoundEffect::Start:
                addVoice(330.0f, 0.32f, 0.12f, 0.02f, 0.6f);
                addVoice(495.0f, 0.32f, 0.10f, 0.02f, 0.5f, 0.08f);
                break;
        }
    }

private:
    struct Voice {
        float phase = 0.0f;
        float frequency = 440.0f;
        float volume = 0.2f;
        float noise = 0.0f;
        float sweep = 0.0f;
        int samplesLeft = 0;
        int totalSamples = 1;
        int delaySamples = 0;
    };

    void addVoice(float frequency, float duration, float volume, float noise, float sweep,
                  float delay = 0.0f) {
        Voice voice;
        voice.frequency = frequency;
        voice.volume = volume;
        voice.noise = noise;
        voice.sweep = sweep;
        voice.samplesLeft = std::max(1, static_cast<int>(duration * 48000.0f));
        voice.totalSamples = voice.samplesLeft;
        voice.delaySamples = static_cast<int>(delay * 48000.0f);
        voices_.push_back(voice);
    }

    static void SDLCALL callback(void* userData, SDL_AudioStream* stream, int additionalAmount, int) {
        auto& self = *static_cast<AudioEngine*>(userData);
        const int frames = additionalAmount / static_cast<int>(sizeof(float) * 2);
        if (frames <= 0) return;
        std::vector<float> samples(static_cast<std::size_t>(frames) * 2, 0.0f);
        {
            std::scoped_lock lock(self.mutex_);
            for (int frame = 0; frame < frames; ++frame) {
                float mixed = 0.0f;
                for (Voice& voice : self.voices_) {
                    if (voice.delaySamples > 0) { --voice.delaySamples; continue; }
                    if (voice.samplesLeft <= 0) continue;
                    const float progress = 1.0f - static_cast<float>(voice.samplesLeft) /
                                                        static_cast<float>(voice.totalSamples);
                    const float envelope = std::pow(1.0f - progress, 2.2f);
                    const float frequency = voice.frequency * (1.0f + voice.sweep * progress);
                    voice.phase += frequency / 48000.0f;
                    self.noiseState_ = self.noiseState_ * 1664525u + 1013904223u;
                    const float noise = (static_cast<float>((self.noiseState_ >> 8u) & 0xffffu) / 32767.5f) - 1.0f;
                    const float tone = std::sin(voice.phase * 2.0f * kPi);
                    mixed += (tone * (1.0f - voice.noise) + noise * voice.noise) * voice.volume * envelope;
                    --voice.samplesLeft;
                }
                mixed = glm::clamp(mixed, -0.82f, 0.82f);
                samples[static_cast<std::size_t>(frame) * 2] = mixed;
                samples[static_cast<std::size_t>(frame) * 2 + 1] = mixed;
            }
            std::erase_if(self.voices_, [](const Voice& voice) {
                return voice.samplesLeft <= 0 && voice.delaySamples <= 0;
            });
        }
        SDL_PutAudioStreamData(stream, samples.data(), additionalAmount);
    }

    SDL_AudioStream* stream_ = nullptr;
    std::mutex mutex_;
    std::vector<Voice> voices_;
    std::uint32_t noiseState_ = 0x71c3a519u;
};

struct InputFrame {
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    int wheel = 0;
    bool fireHeld = false;
    bool firePressed = false;
    bool reloadPressed = false;
    bool pausePressed = false;
    bool confirmPressed = false;
    std::array<bool, 6> weaponPressed{};
    bool mapPressed = false;
    bool difficultyPressed = false;
    bool qualityPressed = false;

    void clearTransient() {
        mouseX = mouseY = 0.0f;
        wheel = 0;
        firePressed = reloadPressed = pausePressed = confirmPressed = false;
        mapPressed = difficultyPressed = qualityPressed = false;
        weaponPressed.fill(false);
    }
};

struct WeaponDefinition {
    const char* name = "PISTOL";
    int magazine = 12;
    int startingReserve = 60;
    int damage = 25;
    int pellets = 1;
    float interval = 0.25f;
    float reload = 1.2f;
    float spread = 0.003f;
    float recoil = 0.012f;
    bool automatic = false;
    glm::vec3 color{0.15f, 0.85f, 1.0f};
};

struct WeaponState {
    int loaded = 0;
    int reserve = 0;
};

struct Block {
    glm::vec3 center{};
    glm::vec3 size{1.0f};
    glm::vec4 color{1.0f};
    bool collision = true;
};

struct Bot {
    glm::vec3 position{};
    glm::vec3 velocity{};
    float yaw = 0.0f;
    float health = 100.0f;
    float maxHealth = 100.0f;
    float fireTimer = 0.0f;
    float decisionTimer = 0.0f;
    float hitFlash = 0.0f;
    float muzzleFlash = 0.0f;
    float strafe = 1.0f;
    int type = 0;
};

enum class PickupType { Health, Ammo };

struct Pickup {
    glm::vec3 position{};
    PickupType type = PickupType::Health;
    float phase = 0.0f;
};

struct Particle {
    glm::vec3 position{};
    glm::vec3 velocity{};
    glm::vec4 color{1.0f};
    float life = 0.0f;
    float maxLife = 1.0f;
    float size = 0.04f;
};

struct Tracer {
    glm::vec3 start{};
    glm::vec3 end{};
    glm::vec4 color{1.0f};
    float life = 0.05f;
};

class Game {
public:
    enum class Mode { Title, Playing, Paused, Dead };

    Game(Renderer& renderer, AudioEngine& audio)
        : renderer_(renderer), audio_(audio), cube_(makeCube()), cylinder_(makeCylinder()) {
        weaponDefinitions_ = {{
            {"PISTOL", 12, 72, 30, 1, 0.28f, 1.05f, 0.0025f, 0.018f, false, {0.10f, 0.80f, 1.0f}},
            {"RIFLE", 30, 180, 15, 1, 0.085f, 1.48f, 0.012f, 0.009f, true, {1.0f, 0.22f, 0.45f}},
            {"SHOTGUN", 8, 48, 12, 8, 0.76f, 1.82f, 0.055f, 0.032f, false, {1.0f, 0.58f, 0.10f}},
            {"SMG", 40, 240, 10, 1, 0.055f, 1.32f, 0.021f, 0.006f, true, {0.20f, 1.0f, 0.48f}},
            {"DMR", 15, 90, 43, 1, 0.36f, 1.72f, 0.0038f, 0.021f, false, {0.72f, 0.24f, 1.0f}},
            {"SNIPER", 5, 35, 92, 1, 0.95f, 2.15f, 0.0006f, 0.048f, false, {0.35f, 0.72f, 1.0f}}
        }};
        buildMap();
        const char* base = SDL_GetBasePath();
        const std::filesystem::path assetRoot = std::filesystem::path(base != nullptr ? base : "./") / "assets";
        helmet_.load(assetRoot / "SciFiHelmet" / "SciFiHelmet.gltf");
        bottle_.load(assetRoot / "WaterBottle.glb");
        reset(false);
        mode_ = Mode::Title;
    }

    void handleEvent(const SDL_Event& event, InputFrame& input) {
        switch (event.type) {
            case SDL_EVENT_MOUSE_MOTION:
                input.mouseX += event.motion.xrel;
                input.mouseY += event.motion.yrel;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    input.fireHeld = true;
                    input.firePressed = true;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_LEFT) input.fireHeld = false;
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                input.wheel += static_cast<int>(event.wheel.y);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.repeat) break;
                if (event.key.scancode == SDL_SCANCODE_R) input.reloadPressed = true;
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) input.pausePressed = true;
                if (event.key.scancode == SDL_SCANCODE_RETURN) input.confirmPressed = true;
                if (event.key.scancode == SDL_SCANCODE_1) input.weaponPressed[0] = true;
                if (event.key.scancode == SDL_SCANCODE_2) input.weaponPressed[1] = true;
                if (event.key.scancode == SDL_SCANCODE_3) input.weaponPressed[2] = true;
                if (event.key.scancode == SDL_SCANCODE_4) input.weaponPressed[3] = true;
                if (event.key.scancode == SDL_SCANCODE_5) input.weaponPressed[4] = true;
                if (event.key.scancode == SDL_SCANCODE_6) input.weaponPressed[5] = true;
                if (event.key.scancode == SDL_SCANCODE_M) input.mapPressed = true;
                if (event.key.scancode == SDL_SCANCODE_D) input.difficultyPressed = true;
                if (event.key.scancode == SDL_SCANCODE_Q) input.qualityPressed = true;
                break;
            default: break;
        }
    }

    void update(float deltaTime, InputFrame& input) {
        time_ += deltaTime;
        deltaTime = std::min(deltaTime, 0.05f);

        if (mode_ == Mode::Title) {
            if (input.mapPressed) {
                selectedMap_ = (selectedMap_ + 1) % 3;
                buildMap();
                audio_.play(SoundEffect::Pickup);
            }
            if (input.difficultyPressed) {
                difficulty_ = (difficulty_ + 1) % 3;
                audio_.play(SoundEffect::Pickup);
            }
            if (input.qualityPressed) {
                quality_ = (quality_ + 1) % 3;
                audio_.play(SoundEffect::Pickup);
            }
            if (input.confirmPressed || input.firePressed) {
                reset(true);
                mode_ = Mode::Playing;
                audio_.play(SoundEffect::Start);
            } else if (input.pausePressed) {
                quitRequested_ = true;
            }
            updateParticles(deltaTime);
            return;
        }
        if (mode_ == Mode::Dead) {
            damageFlash_ = std::max(0.0f, damageFlash_ - deltaTime * 0.5f);
            if (input.confirmPressed || input.firePressed) {
                reset(true);
                mode_ = Mode::Playing;
            }
            return;
        }
        if (input.pausePressed) {
            mode_ = mode_ == Mode::Paused ? Mode::Playing : Mode::Paused;
            return;
        }
        if (mode_ == Mode::Paused) return;

        const bool* keys = SDL_GetKeyboardState(nullptr);
        updatePlayer(deltaTime, keys, input);
        updateWeapons(deltaTime, input);
        updateBots(deltaTime);
        updatePickups(deltaTime);
        updateParticles(deltaTime);

        damageFlash_ = std::max(0.0f, damageFlash_ - deltaTime * 1.65f);
        hitMarker_ = std::max(0.0f, hitMarker_ - deltaTime * 5.0f);
        killMarker_ = std::max(0.0f, killMarker_ - deltaTime * 3.0f);
        muzzleFlash_ = std::max(0.0f, muzzleFlash_ - deltaTime * 12.0f);
        weaponKick_ = glm::mix(weaponKick_, 0.0f, saturate(deltaTime * 14.0f));

        if (bots_.empty()) {
            nextWaveTimer_ -= deltaTime;
            if (nextWaveTimer_ <= 0.0f) spawnWave();
        }

        if (playerHealth_ <= 0.0f) {
            playerHealth_ = 0.0f;
            mode_ = Mode::Dead;
        }
    }

    void render(int width, int height) {
        renderer_.beginFrame(width, height);

        glm::vec3 eye;
        glm::vec3 forward;
        if (mode_ == Mode::Title) {
            eye = {std::sin(time_ * 0.17f) * 15.0f, 7.5f, std::cos(time_ * 0.17f) * 15.0f};
            forward = glm::normalize(glm::vec3(0.0f, 1.2f, 0.0f) - eye);
        } else {
            eye = cameraPosition();
            forward = cameraForward();
        }
        const glm::mat4 view = glm::lookAt(eye, eye + forward, {0, 1, 0});
        const glm::mat4 projection = glm::perspective(glm::radians(72.0f),
            static_cast<float>(std::max(width, 1)) / static_cast<float>(std::max(height, 1)), 0.045f, 100.0f);
        renderer_.setCamera(projection * view, eye);

        renderWorld();
        renderPickups();
        renderBots();
        renderParticles();

        if (mode_ != Mode::Title) {
            glClear(GL_DEPTH_BUFFER_BIT);
            renderWeapon(eye, forward);
        }
        renderHUD();
        renderer_.flushUI();
    }

    bool wantsMouseCapture() const { return mode_ == Mode::Playing; }
    bool quitRequested() const { return quitRequested_; }
    Mode mode() const { return mode_; }

private:
    void reset(bool startWave) {
        playerPosition_ = {0.0f, 0.0f, 8.0f};
        playerVerticalVelocity_ = 0.0f;
        playerHealth_ = 100.0f;
        yaw_ = 0.0f;
        pitch_ = 0.0f;
        score_ = 0;
        kills_ = 0;
        wave_ = 0;
        selectedWeapon_ = 1;
        weaponCooldown_ = 0.0f;
        reloadTimer_ = 0.0f;
        reloadWeapon_ = -1;
        nextWaveTimer_ = 0.25f;
        bots_.clear();
        pickups_.clear();
        particles_.clear();
        tracers_.clear();
        for (std::size_t i = 0; i < weapons_.size(); ++i) {
            weapons_[i].loaded = weaponDefinitions_[i].magazine;
            weapons_[i].reserve = weaponDefinitions_[i].startingReserve;
        }
        if (startWave) spawnWave();
    }

    void buildMap() {
        blocks_.clear();
        spawnPoints_.clear();
        auto add = [this](glm::vec3 center, glm::vec3 size, glm::vec4 color, bool collision = true) {
            blocks_.push_back({center, size, color, collision});
        };
        const std::array<glm::vec4, 3> floorColors = {{
            {0.06f, 0.075f, 0.11f, 1}, {0.14f, 0.105f, 0.065f, 1}, {0.055f, 0.095f, 0.13f, 1}
        }};
        const std::array<glm::vec4, 3> wallColors = {{
            {0.055f, 0.10f, 0.19f, 1}, {0.20f, 0.13f, 0.07f, 1}, {0.07f, 0.16f, 0.22f, 1}
        }};
        add({0, -0.35f, 0}, {44, 0.7f, 44}, floorColors[static_cast<std::size_t>(selectedMap_)], false);
        const glm::vec4 wall = wallColors[static_cast<std::size_t>(selectedMap_)];
        add({0, 2.2f, -22}, {44, 4.4f, 1}, wall);
        add({0, 2.2f, 22}, {44, 4.4f, 1}, wall);
        add({-22, 2.2f, 0}, {1, 4.4f, 44}, wall);
        add({22, 2.2f, 0}, {1, 4.4f, 44}, wall);

        const glm::vec4 cyan{0.03f, 0.28f, 0.36f, 1};
        const glm::vec4 magenta{0.38f, 0.045f, 0.18f, 1};
        const glm::vec4 orange{0.42f, 0.16f, 0.035f, 1};
        if (selectedMap_ == 0) {
            add({-8, 1.2f, -7}, {5, 2.4f, 1.2f}, cyan);
            add({8, 1.2f, 7}, {5, 2.4f, 1.2f}, magenta);
            add({8, 1.2f, -8}, {1.2f, 2.4f, 5}, orange);
            add({-8, 1.2f, 8}, {1.2f, 2.4f, 5}, cyan);
            add({0, 0.75f, 0}, {4.0f, 1.5f, 4.0f}, {0.12f, 0.14f, 0.22f, 1});
            add({-14, 1.7f, 0}, {2.2f, 3.4f, 2.2f}, magenta);
            add({14, 1.7f, 0}, {2.2f, 3.4f, 2.2f}, cyan);
            add({0, 1.7f, -14}, {2.2f, 3.4f, 2.2f}, orange);
            add({0, 1.7f, 14}, {2.2f, 3.4f, 2.2f}, magenta);
            for (int i = -1; i <= 1; i += 2) {
                add({static_cast<float>(i * 15), 0.6f, static_cast<float>(i * 14)}, {3.0f, 1.2f, 1.4f}, cyan);
                add({static_cast<float>(i * 15), 0.6f, static_cast<float>(-i * 14)}, {1.4f, 1.2f, 3.0f}, orange);
            }
            spawnPoints_ = {{
                {-17, 0, -17}, {17, 0, -17}, {-17, 0, 17}, {17, 0, 17},
                {0, 0, -18}, {0, 0, 18}, {-18, 0, 0}, {18, 0, 0},
                {-12, 0, -3}, {12, 0, 3}, {-3, 0, 12}, {3, 0, -12}
            }};
        } else if (selectedMap_ == 1) {
            const glm::vec4 sand{0.34f, 0.22f, 0.09f, 1};
            const glm::vec4 steel{0.13f, 0.14f, 0.16f, 1};
            add({-7.0f, 1.4f, -12.5f}, {1.2f, 2.8f, 13.0f}, sand);
            add({-7.0f, 1.4f, 10.5f}, {1.2f, 2.8f, 9.0f}, sand);
            add({7.0f, 1.4f, -10.5f}, {1.2f, 2.8f, 9.0f}, steel);
            add({7.0f, 1.4f, 12.5f}, {1.2f, 2.8f, 13.0f}, steel);
            add({0, 1.2f, -5.5f}, {8.0f, 2.4f, 1.2f}, orange);
            add({0, 1.2f, 5.5f}, {8.0f, 2.4f, 1.2f}, sand);
            for (int side = -1; side <= 1; side += 2) {
                add({side * 14.0f, 0.65f, -8.0f}, {4.0f, 1.3f, 2.2f}, sand);
                add({side * 14.0f, 0.65f, 8.0f}, {2.2f, 1.3f, 4.0f}, steel);
                add({side * 12.0f, 1.7f, 0}, {2.4f, 3.4f, 2.4f}, orange);
            }
            spawnPoints_ = {{
                {-18,0,-17}, {18,0,-17}, {-18,0,17}, {18,0,17}, {-12,0,0}, {12,0,0},
                {-3,0,-16}, {3,0,16}, {-13,0,-8}, {13,0,8}, {-2,0,2}, {2,0,-2}
            }};
        } else {
            const glm::vec4 ice{0.04f, 0.31f, 0.43f, 1};
            const glm::vec4 lab{0.16f, 0.21f, 0.27f, 1};
            add({0, 1.1f, 0}, {6.0f, 2.2f, 6.0f}, lab);
            add({-12, 1.0f, -12}, {6.0f, 2.0f, 2.0f}, ice);
            add({12, 1.0f, 12}, {6.0f, 2.0f, 2.0f}, ice);
            add({12, 1.0f, -12}, {2.0f, 2.0f, 6.0f}, magenta);
            add({-12, 1.0f, 12}, {2.0f, 2.0f, 6.0f}, magenta);
            for (int side = -1; side <= 1; side += 2) {
                add({side * 9.0f, 1.6f, 0}, {1.8f, 3.2f, 8.0f}, lab);
                add({0, 1.6f, side * 9.0f}, {8.0f, 3.2f, 1.8f}, ice);
                add({side * 17.0f, 0.7f, 0}, {2.4f, 1.4f, 4.0f}, cyan);
                add({0, 0.7f, side * 17.0f}, {4.0f, 1.4f, 2.4f}, magenta);
            }
            spawnPoints_ = {{
                {-18,0,-18}, {18,0,-18}, {-18,0,18}, {18,0,18}, {0,0,-18}, {0,0,18},
                {-18,0,0}, {18,0,0}, {-6,0,-6}, {6,0,6}, {-6,0,6}, {6,0,-6}
            }};
        }
    }

    AABB blockBounds(const Block& block) const {
        return {block.center - block.size * 0.5f, block.center + block.size * 0.5f};
    }

    bool pointBlocked(glm::vec3 position, float radius) const {
        for (const Block& block : blocks_) {
            if (!block.collision) continue;
            AABB bounds = blockBounds(block);
            bounds.min.x -= radius; bounds.min.z -= radius;
            bounds.max.x += radius; bounds.max.z += radius;
            if (position.x > bounds.min.x && position.x < bounds.max.x &&
                position.z > bounds.min.z && position.z < bounds.max.z &&
                position.y < bounds.max.y + 1.8f && position.y + 1.8f > bounds.min.y) return true;
        }
        return false;
    }

    glm::vec3 moveWithCollision(glm::vec3 position, glm::vec3 movement, float radius) const {
        glm::vec3 result = position;
        result.x += movement.x;
        if (pointBlocked(result, radius)) result.x = position.x;
        result.z += movement.z;
        if (pointBlocked(result, radius)) result.z = position.z;
        return result;
    }

    glm::vec3 cameraForward() const {
        return glm::normalize(glm::vec3(std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_),
                                        -std::cos(yaw_) * std::cos(pitch_)));
    }

    glm::vec3 cameraPosition() const {
        const float crouchOffset = crouching_ ? 1.12f : 1.62f;
        const float bob = onGround_ && moving_ ? std::sin(stepTime_ * 12.0f) * 0.028f : 0.0f;
        return playerPosition_ + glm::vec3(0.0f, crouchOffset + bob, 0.0f);
    }

    void updatePlayer(float deltaTime, const bool* keys, const InputFrame& input) {
        yaw_ += input.mouseX * 0.0021f;
        pitch_ -= input.mouseY * 0.0021f;
        pitch_ = glm::clamp(pitch_, -1.42f, 1.42f);

        glm::vec3 flatForward{std::sin(yaw_), 0.0f, -std::cos(yaw_)};
        const glm::vec3 right{std::cos(yaw_), 0.0f, std::sin(yaw_)};
        glm::vec3 desired(0.0f);
        if (keys[SDL_SCANCODE_W]) desired += flatForward;
        if (keys[SDL_SCANCODE_S]) desired -= flatForward;
        if (keys[SDL_SCANCODE_D]) desired += right;
        if (keys[SDL_SCANCODE_A]) desired -= right;
        moving_ = glm::length2(desired) > 0.01f;
        if (moving_) desired = glm::normalize(desired);
        crouching_ = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_C];
        const bool sprinting = keys[SDL_SCANCODE_LSHIFT] && moving_ && !crouching_;
        const float speed = crouching_ ? 3.05f : (sprinting ? 8.3f : 5.25f);
        playerPosition_ = moveWithCollision(playerPosition_, desired * speed * deltaTime, 0.42f);
        if (moving_ && onGround_) stepTime_ += deltaTime * (sprinting ? 1.32f : 1.0f);

        if (keys[SDL_SCANCODE_SPACE] && onGround_ && !jumpWasDown_) {
            playerVerticalVelocity_ = 5.5f;
            onGround_ = false;
        }
        jumpWasDown_ = keys[SDL_SCANCODE_SPACE];
        playerVerticalVelocity_ -= 14.5f * deltaTime;
        playerPosition_.y += playerVerticalVelocity_ * deltaTime;
        if (playerPosition_.y <= 0.0f) {
            playerPosition_.y = 0.0f;
            playerVerticalVelocity_ = 0.0f;
            onGround_ = true;
        }
    }

    void switchWeapon(int index) {
        index = (index % static_cast<int>(weapons_.size()) + static_cast<int>(weapons_.size())) %
                static_cast<int>(weapons_.size());
        if (index == selectedWeapon_) return;
        selectedWeapon_ = index;
        reloadTimer_ = 0.0f;
        reloadWeapon_ = -1;
        weaponCooldown_ = std::max(weaponCooldown_, 0.16f);
        weaponKick_ = 0.0f;
    }

    void beginReload() {
        WeaponState& state = weapons_[static_cast<std::size_t>(selectedWeapon_)];
        const WeaponDefinition& definition = weaponDefinitions_[static_cast<std::size_t>(selectedWeapon_)];
        if (reloadTimer_ > 0.0f || state.loaded >= definition.magazine || state.reserve <= 0) return;
        reloadTimer_ = definition.reload;
        reloadWeapon_ = selectedWeapon_;
        audio_.play(SoundEffect::Reload);
    }

    void finishReload() {
        if (reloadWeapon_ < 0) return;
        WeaponState& state = weapons_[static_cast<std::size_t>(reloadWeapon_)];
        const WeaponDefinition& definition = weaponDefinitions_[static_cast<std::size_t>(reloadWeapon_)];
        const int needed = definition.magazine - state.loaded;
        const int moved = std::min(needed, state.reserve);
        state.loaded += moved;
        state.reserve -= moved;
        reloadWeapon_ = -1;
    }

    void updateWeapons(float deltaTime, const InputFrame& input) {
        weaponCooldown_ = std::max(0.0f, weaponCooldown_ - deltaTime);
        if (reloadTimer_ > 0.0f) {
            reloadTimer_ -= deltaTime;
            if (reloadTimer_ <= 0.0f) finishReload();
        }

        for (int i = 0; i < static_cast<int>(weapons_.size()); ++i)
            if (input.weaponPressed[static_cast<std::size_t>(i)]) switchWeapon(i);
        if (input.wheel != 0) switchWeapon(selectedWeapon_ + (input.wheel < 0 ? 1 : -1));
        if (input.reloadPressed) beginReload();

        const WeaponDefinition& definition = weaponDefinitions_[static_cast<std::size_t>(selectedWeapon_)];
        const bool trigger = definition.automatic ? input.fireHeld : input.firePressed;
        if (!trigger || weaponCooldown_ > 0.0f || reloadTimer_ > 0.0f) return;
        WeaponState& state = weapons_[static_cast<std::size_t>(selectedWeapon_)];
        if (state.loaded <= 0) {
            weaponCooldown_ = 0.22f;
            audio_.play(SoundEffect::Empty);
            beginReload();
            return;
        }

        --state.loaded;
        weaponCooldown_ = definition.interval;
        muzzleFlash_ = 1.0f;
        weaponKick_ = 1.0f;
        pitch_ = glm::clamp(pitch_ + definition.recoil, -1.42f, 1.42f);
        if (selectedWeapon_ == 0) audio_.play(SoundEffect::Pistol);
        else if (selectedWeapon_ == 2) audio_.play(SoundEffect::Shotgun);
        else if (selectedWeapon_ == 5) audio_.play(SoundEffect::Shotgun);
        else audio_.play(SoundEffect::Rifle);

        const glm::vec3 origin = cameraPosition();
        const glm::vec3 forward = cameraForward();
        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
        const glm::vec3 up = glm::normalize(glm::cross(right, forward));
        bool anyHit = false;
        bool anyKill = false;
        for (int pellet = 0; pellet < definition.pellets; ++pellet) {
            const float movementPenalty = moving_ ? 1.65f : 1.0f;
            const float spread = definition.spread * movementPenalty;
            const glm::vec3 direction = glm::normalize(forward + right * randomRange(-spread, spread) +
                                                      up * randomRange(-spread, spread));
            const auto result = fireRay(origin, direction, definition.damage);
            anyHit = anyHit || result.first;
            anyKill = anyKill || result.second;
        }
        if (anyHit) {
            hitMarker_ = 1.0f;
            audio_.play(SoundEffect::Hit);
        }
        if (anyKill) {
            killMarker_ = 1.0f;
            audio_.play(SoundEffect::Kill);
        }
        if (state.loaded == 0 && state.reserve > 0) autoReloadDelay_ = 0.35f;
    }

    std::pair<bool, bool> fireRay(const glm::vec3& origin, const glm::vec3& direction, int damage) {
        float nearestDistance = 60.0f;
        for (const Block& block : blocks_) {
            if (!block.collision) continue;
            float distance = 0.0f;
            if (rayAABB(origin, direction, blockBounds(block), distance) && distance < nearestDistance)
                nearestDistance = distance;
        }

        int hitBot = -1;
        for (std::size_t i = 0; i < bots_.size(); ++i) {
            const Bot& bot = bots_[i];
            float bodyDistance = 0.0f;
            float headDistance = 0.0f;
            bool body = raySphere(origin, direction, bot.position + glm::vec3(0, 0.9f, 0), 0.62f, bodyDistance);
            bool head = raySphere(origin, direction, bot.position + glm::vec3(0, 1.62f, 0), 0.36f, headDistance);
            float candidate = std::numeric_limits<float>::max();
            if (body) candidate = bodyDistance;
            if (head) candidate = std::min(candidate, headDistance);
            if (candidate < nearestDistance) {
                nearestDistance = candidate;
                hitBot = static_cast<int>(i);
            }
        }

        const glm::vec3 end = origin + direction * nearestDistance;
        tracers_.push_back({origin + direction * 0.45f, end, {0.12f, 0.92f, 1.0f, 0.85f}, 0.07f});
        spawnImpact(end, hitBot >= 0 ? glm::vec4(1.0f, 0.12f, 0.22f, 1.0f)
                                     : glm::vec4(0.15f, 0.72f, 1.0f, 1.0f));
        if (hitBot < 0) return {false, false};

        Bot& bot = bots_[static_cast<std::size_t>(hitBot)];
        const bool headshot = glm::length(end - (bot.position + glm::vec3(0, 1.62f, 0))) < 0.39f;
        bot.health -= static_cast<float>(damage) * (headshot ? 1.7f : 1.0f);
        bot.hitFlash = 1.0f;
        if (bot.health > 0.0f) return {true, false};

        const glm::vec3 deathPosition = bot.position + glm::vec3(0, 0.8f, 0);
        const int deathParticles = quality_ == 0 ? 7 : (quality_ == 1 ? 12 : 18);
        for (int i = 0; i < deathParticles; ++i) {
            particles_.push_back({deathPosition, randomUnit() * randomRange(1.5f, 5.0f),
                                  {1.0f, 0.06f, 0.25f, 1.0f}, randomRange(0.35f, 0.8f), 0.8f,
                                  randomRange(0.025f, 0.075f)});
        }
        bots_.erase(bots_.begin() + hitBot);
        ++kills_;
        score_ += 100 + wave_ * 25 + (headshot ? 75 : 0);
        if (kills_ % 4 == 0) {
            pickups_.push_back({deathPosition - glm::vec3(0, 0.8f, 0),
                                playerHealth_ < 55.0f ? PickupType::Health : PickupType::Ammo, randomRange(0, kPi)});
        }
        if (bots_.empty()) nextWaveTimer_ = 2.1f;
        return {true, true};
    }

    void spawnImpact(const glm::vec3& position, const glm::vec4& color) {
        const int impactCount = quality_ == 0 ? 2 : (quality_ == 1 ? 4 : 5);
        for (int i = 0; i < impactCount; ++i) {
            Particle particle;
            particle.position = position;
            particle.velocity = randomUnit() * randomRange(0.5f, 2.7f);
            particle.color = color;
            particle.life = particle.maxLife = randomRange(0.12f, 0.35f);
            particle.size = randomRange(0.016f, 0.035f);
            particles_.push_back(particle);
        }
    }

    void spawnWave() {
        ++wave_;
        const int botLimit = quality_ == 0 ? 11 : (quality_ == 1 ? 15 : 19);
        const int count = std::min(3 + wave_ * 2, botLimit);
        const std::array<float, 3> healthScale = {{0.82f, 1.0f, 1.24f}};
        std::shuffle(spawnPoints_.begin(), spawnPoints_.end(), random_);
        for (int i = 0; i < count; ++i) {
            Bot bot;
            bot.position = spawnPoints_[static_cast<std::size_t>(i % spawnPoints_.size())];
            if (i >= static_cast<int>(spawnPoints_.size())) bot.position += randomUnitFlat() * randomRange(0.8f, 2.3f);
            bot.type = (wave_ >= 3 && i % 5 == 0) ? 1 : 0;
            bot.maxHealth = ((bot.type == 1 ? 150.0f : 82.0f) + wave_ * 11.0f) *
                            healthScale[static_cast<std::size_t>(difficulty_)];
            bot.health = bot.maxHealth;
            bot.fireTimer = randomRange(0.45f, 1.8f);
            bot.strafe = randomRange(0.0f, 1.0f) > 0.5f ? 1.0f : -1.0f;
            bots_.push_back(bot);
        }
        nextWaveTimer_ = 999.0f;
        audio_.play(SoundEffect::Start);
    }

    bool lineOfSight(const glm::vec3& from, const glm::vec3& to) const {
        const glm::vec3 delta = to - from;
        const float length = glm::length(delta);
        if (length < 0.01f) return true;
        const glm::vec3 direction = delta / length;
        for (const Block& block : blocks_) {
            if (!block.collision) continue;
            float distance = 0.0f;
            if (rayAABB(from, direction, blockBounds(block), distance) && distance < length) return false;
        }
        return true;
    }

    void updateBots(float deltaTime) {
        const glm::vec3 playerTarget = cameraPosition() - glm::vec3(0, 0.35f, 0);
        for (Bot& bot : bots_) {
            bot.fireTimer -= deltaTime;
            bot.decisionTimer -= deltaTime;
            bot.hitFlash = std::max(0.0f, bot.hitFlash - deltaTime * 5.0f);
            bot.muzzleFlash = std::max(0.0f, bot.muzzleFlash - deltaTime * 9.0f);
            const glm::vec3 from = bot.position + glm::vec3(0, 1.32f, 0);
            glm::vec3 toPlayer = playerTarget - from;
            const float distance = glm::length(toPlayer);
            const glm::vec3 direction = distance > 0.01f ? toPlayer / distance : glm::vec3(0, 0, 1);
            const bool visible = lineOfSight(from, playerTarget);

            if (bot.decisionTimer <= 0.0f) {
                bot.decisionTimer = randomRange(0.55f, 1.5f);
                if (randomRange(0, 1) > 0.62f) bot.strafe *= -1.0f;
            }

            glm::vec3 flatDirection = glm::normalize(glm::vec3(direction.x, 0.0f, direction.z));
            if (!std::isfinite(flatDirection.x)) flatDirection = {0, 0, 1};
            const glm::vec3 side{-flatDirection.z, 0.0f, flatDirection.x};
            glm::vec3 desired(0.0f);
            if (!visible || distance > 10.5f) desired += flatDirection;
            else if (distance < 4.2f) desired -= flatDirection * 0.85f;
            if (visible && distance < 16.0f) desired += side * bot.strafe * 0.72f;
            if (glm::length2(desired) > 0.01f) desired = glm::normalize(desired);
            const float speed = (bot.type == 1 ? 2.25f : 3.15f) + std::min(wave_ * 0.08f, 0.8f) +
                                difficulty_ * 0.22f;
            glm::vec3 moved = moveWithCollision(bot.position, desired * speed * deltaTime, bot.type == 1 ? 0.56f : 0.43f);
            if (glm::length2(moved - bot.position) < 0.00001f) {
                moved = moveWithCollision(bot.position, side * bot.strafe * speed * deltaTime, 0.43f);
                bot.strafe *= -1.0f;
            }
            bot.velocity = (moved - bot.position) / std::max(deltaTime, 0.001f);
            bot.position = moved;
            bot.yaw = std::atan2(direction.x, direction.z);

            if (visible && distance < 28.0f && bot.fireTimer <= 0.0f) {
                const float fireRate = bot.type == 1 ? 0.55f : 0.88f;
                bot.fireTimer = std::max(0.28f, fireRate - wave_ * 0.018f) + randomRange(0.0f, 0.34f);
                bot.muzzleFlash = 1.0f;
                audio_.play(SoundEffect::Enemy);
                const float accuracy = glm::clamp(0.34f + difficulty_ * 0.10f + wave_ * 0.018f -
                                                  distance * 0.008f, 0.14f, 0.84f);
                const bool hit = randomRange(0, 1) < accuracy;
                glm::vec3 end = playerTarget;
                if (!hit) end += randomUnit() * randomRange(0.8f, 2.5f);
                tracers_.push_back({from + direction * 0.35f, end, {1.0f, 0.08f, 0.22f, 0.8f}, 0.09f});
                if (hit) {
                    const float damage = ((bot.type == 1 ? 12.0f : 7.0f) + wave_ * 0.35f) *
                                         (0.78f + difficulty_ * 0.22f);
                    playerHealth_ -= damage;
                    damageFlash_ = 1.0f;
                    cameraShake_ = std::min(1.0f, cameraShake_ + 0.45f);
                }
            }
        }

        for (std::size_t i = 0; i < bots_.size(); ++i) {
            for (std::size_t j = i + 1; j < bots_.size(); ++j) {
                glm::vec3 delta = bots_[j].position - bots_[i].position;
                delta.y = 0;
                const float squared = glm::length2(delta);
                if (squared > 0.0001f && squared < 0.72f * 0.72f) {
                    const glm::vec3 push = glm::normalize(delta) * (0.72f - std::sqrt(squared)) * 0.5f;
                    bots_[i].position -= push;
                    bots_[j].position += push;
                }
            }
        }
        cameraShake_ = std::max(0.0f, cameraShake_ - deltaTime * 3.8f);
    }

    void updatePickups(float deltaTime) {
        for (Pickup& pickup : pickups_) pickup.phase += deltaTime;
        for (std::size_t i = 0; i < pickups_.size();) {
            if (glm::distance(glm::vec2(playerPosition_.x, playerPosition_.z),
                              glm::vec2(pickups_[i].position.x, pickups_[i].position.z)) < 1.15f) {
                if (pickups_[i].type == PickupType::Health) {
                    playerHealth_ = std::min(100.0f, playerHealth_ + 35.0f);
                } else {
                    for (std::size_t weapon = 0; weapon < weapons_.size(); ++weapon)
                        weapons_[weapon].reserve += weaponDefinitions_[weapon].magazine;
                }
                audio_.play(SoundEffect::Pickup);
                score_ += 25;
                pickups_.erase(pickups_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    void updateParticles(float deltaTime) {
        for (Particle& particle : particles_) {
            particle.life -= deltaTime;
            particle.velocity.y -= 5.5f * deltaTime;
            particle.position += particle.velocity * deltaTime;
        }
        std::erase_if(particles_, [](const Particle& particle) { return particle.life <= 0.0f; });
        for (Tracer& tracer : tracers_) tracer.life -= deltaTime;
        std::erase_if(tracers_, [](const Tracer& tracer) { return tracer.life <= 0.0f; });
        if (autoReloadDelay_ > 0.0f) {
            autoReloadDelay_ -= deltaTime;
            if (autoReloadDelay_ <= 0.0f) beginReload();
        }
    }

    glm::mat4 modelMatrix(const glm::vec3& position, const glm::vec3& scale,
                          float yaw = 0.0f) const {
        return glm::translate(glm::mat4(1.0f), position) *
               glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 1, 0)) *
               glm::scale(glm::mat4(1.0f), scale);
    }

    void renderWorld() {
        for (const Block& block : blocks_) renderer_.draw(cube_, modelMatrix(block.center, block.size), block.color);

        const glm::vec4 lineColor{0.01f, 0.24f, 0.34f, 1.0f};
        const int gridStep = quality_ == 0 ? 4 : 2;
        for (int i = -20; i <= 20; i += gridStep) {
            renderer_.draw(cube_, modelMatrix({static_cast<float>(i), 0.012f, 0}, {0.025f, 0.018f, 42.0f}), lineColor, 0, 0.45f);
            renderer_.draw(cube_, modelMatrix({0, 0.014f, static_cast<float>(i)}, {42.0f, 0.018f, 0.025f}), lineColor, 0, 0.45f);
        }
        for (const glm::vec3& position : std::array<glm::vec3, 8>{{
                 {-20, 2.1f, -20}, {20, 2.1f, -20}, {-20, 2.1f, 20}, {20, 2.1f, 20},
                 {-10, 2.1f, -21.4f}, {10, 2.1f, -21.4f}, {-21.4f, 2.1f, -10}, {21.4f, 2.1f, 10}}}) {
            renderer_.draw(cylinder_, modelMatrix(position, {0.10f, 4.2f, 0.10f}),
                           {0.05f, 0.76f, 1.0f, 1}, 0, 1.3f);
        }
    }

    void renderPickups() {
        for (const Pickup& pickup : pickups_) {
            const float y = 0.52f + std::sin(pickup.phase * 2.5f) * 0.12f;
            const glm::vec3 position = pickup.position + glm::vec3(0, y, 0);
            if (pickup.type == PickupType::Health && bottle_.valid()) {
                const glm::mat4 transform = glm::translate(glm::mat4(1), position) *
                    glm::rotate(glm::mat4(1), pickup.phase, glm::vec3(0, 1, 0)) *
                    glm::scale(glm::mat4(1), glm::vec3(3.0f));
                bottle_.draw(renderer_, transform, {0.25f, 1.0f, 0.55f, 1.0f});
            } else {
                const glm::vec4 color = pickup.type == PickupType::Health
                    ? glm::vec4(0.12f, 1.0f, 0.38f, 1.0f) : glm::vec4(1.0f, 0.58f, 0.08f, 1.0f);
                renderer_.draw(cube_, modelMatrix(position, {0.52f, 0.34f, 0.52f}, pickup.phase), color, 0, 0.8f);
                if (pickup.type == PickupType::Health) {
                    renderer_.draw(cube_, modelMatrix(position + glm::vec3(0, 0.01f, 0.27f), {0.12f, 0.22f, 0.03f}), {1,1,1,1}, 0, 1);
                    renderer_.draw(cube_, modelMatrix(position + glm::vec3(0, 0.01f, 0.27f), {0.28f, 0.09f, 0.03f}), {1,1,1,1}, 0, 1);
                }
            }
        }
    }

    void renderBots() {
        for (const Bot& bot : bots_) {
            const float healthRatio = saturate(bot.health / bot.maxHealth);
            glm::vec4 suit = bot.type == 1 ? glm::vec4(0.95f, 0.12f, 0.08f, 1)
                                            : glm::vec4(0.13f, 0.28f, 0.46f, 1);
            suit = glm::mix(suit, glm::vec4(1,1,1,1), bot.hitFlash * 0.75f);
            const glm::mat4 bodyRoot = glm::translate(glm::mat4(1), bot.position) *
                                       glm::rotate(glm::mat4(1), bot.yaw, glm::vec3(0,1,0));
            renderer_.draw(cube_, bodyRoot * glm::translate(glm::mat4(1), {0, 0.92f, 0}) *
                           glm::scale(glm::mat4(1), bot.type == 1 ? glm::vec3(0.75f, 1.08f, 0.46f)
                                                                 : glm::vec3(0.58f, 0.92f, 0.38f)), suit);
            renderer_.draw(cube_, bodyRoot * glm::translate(glm::mat4(1), {-0.22f, 0.32f, 0}) *
                           glm::scale(glm::mat4(1), {0.20f, 0.62f, 0.22f}), suit);
            renderer_.draw(cube_, bodyRoot * glm::translate(glm::mat4(1), {0.22f, 0.32f, 0}) *
                           glm::scale(glm::mat4(1), {0.20f, 0.62f, 0.22f}), suit);
            renderer_.draw(cube_, bodyRoot * glm::translate(glm::mat4(1), {0.52f, 1.03f, 0}) *
                           glm::scale(glm::mat4(1), {0.20f, 0.67f, 0.22f}), suit);
            renderer_.draw(cube_, bodyRoot * glm::translate(glm::mat4(1), {-0.52f, 1.03f, 0}) *
                           glm::scale(glm::mat4(1), {0.20f, 0.67f, 0.22f}), suit);
            renderer_.draw(cube_, bodyRoot * glm::translate(glm::mat4(1), {0, 1.58f, 0}) *
                           glm::scale(glm::mat4(1), {0.48f, 0.42f, 0.45f}), {0.13f, 0.09f, 0.08f, 1});
            if (quality_ > 0 && helmet_.valid()) {
                const glm::mat4 helmetTransform = bodyRoot * glm::translate(glm::mat4(1), {0, 1.67f, -0.03f}) *
                    glm::rotate(glm::mat4(1), kPi, glm::vec3(0,1,0)) * glm::scale(glm::mat4(1), glm::vec3(0.28f));
                helmet_.draw(renderer_, helmetTransform);
            }
            renderer_.draw(cube_, bodyRoot * glm::translate(glm::mat4(1), {0.18f, 1.13f, 0.42f}) *
                           glm::scale(glm::mat4(1), {0.12f, 0.12f, 0.76f}), {0.04f,0.045f,0.06f,1});
            if (bot.muzzleFlash > 0) {
                renderer_.draw(cube_, bodyRoot * glm::translate(glm::mat4(1), {0.18f, 1.13f, 0.85f}) *
                               glm::scale(glm::mat4(1), glm::vec3(0.11f + bot.muzzleFlash * 0.09f)),
                               {1.0f,0.08f,0.15f,1}, 0, 3.0f);
            }
            renderer_.draw(cube_, modelMatrix(bot.position + glm::vec3(0, 2.05f, 0), {0.82f, 0.055f, 0.055f}),
                           {0.04f,0.04f,0.055f,1});
            renderer_.draw(cube_, modelMatrix(bot.position + glm::vec3(-0.41f + 0.41f * healthRatio, 2.052f, 0.002f),
                                               {0.82f * healthRatio, 0.06f, 0.06f}),
                           healthRatio > 0.45f ? glm::vec4(0.1f,0.95f,0.45f,1) : glm::vec4(1,0.12f,0.16f,1), 0, 0.8f);
        }
    }

    void renderParticles() {
        for (const Particle& particle : particles_) {
            const float alpha = saturate(particle.life / std::max(particle.maxLife, 0.001f));
            glm::vec4 color = particle.color;
            color.a *= alpha;
            renderer_.draw(cube_, modelMatrix(particle.position, glm::vec3(particle.size)), color, 0, 1.8f);
        }
        for (const Tracer& tracer : tracers_) {
            const glm::vec3 delta = tracer.end - tracer.start;
            const float length = glm::length(delta);
            if (length < 0.001f) continue;
            const glm::quat rotation = glm::rotation(glm::vec3(0, 0, 1), delta / length);
            const glm::mat4 transform = glm::translate(glm::mat4(1), (tracer.start + tracer.end) * 0.5f) *
                glm::mat4_cast(rotation) * glm::scale(glm::mat4(1), {0.018f, 0.018f, length});
            renderer_.draw(cube_, transform, tracer.color, 0, 2.2f);
        }
    }

    void renderWeapon(const glm::vec3& eye, const glm::vec3& forward) {
        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0,1,0)));
        const glm::vec3 up = glm::normalize(glm::cross(right, forward));
        const float swayX = std::sin(stepTime_ * 12.0f) * (moving_ ? 0.018f : 0.004f);
        const float swayY = std::abs(std::cos(stepTime_ * 12.0f)) * (moving_ ? 0.022f : 0.003f);
        const glm::vec3 origin = eye + forward * (0.52f - weaponKick_ * 0.09f) +
                                 right * (0.26f + swayX) - up * (0.23f + swayY);
        glm::mat4 basis(1.0f);
        basis[0] = glm::vec4(right, 0.0f);
        basis[1] = glm::vec4(up, 0.0f);
        basis[2] = glm::vec4(forward, 0.0f);
        const glm::mat4 root = glm::translate(glm::mat4(1), origin) * basis;
        const glm::vec3 accent = weaponDefinitions_[static_cast<std::size_t>(selectedWeapon_)].color;
        const glm::vec4 dark{0.025f, 0.032f, 0.052f, 1};
        const glm::vec4 glow{accent, 1};

        if (selectedWeapon_ == 0) {
            renderer_.draw(cube_, root * glm::translate(glm::mat4(1), {0,0,0.18f}) *
                           glm::scale(glm::mat4(1), {0.18f,0.16f,0.52f}), dark);
            renderer_.draw(cube_, root * glm::translate(glm::mat4(1), {0,-0.18f,0.02f}) *
                           glm::rotate(glm::mat4(1), -0.18f, {1,0,0}) * glm::scale(glm::mat4(1), {0.13f,0.36f,0.18f}),
                           {0.08f,0.09f,0.13f,1});
            renderer_.draw(cube_, root * glm::translate(glm::mat4(1), {0,0.095f,0.18f}) *
                           glm::scale(glm::mat4(1), {0.13f,0.025f,0.38f}), glow, 0, 0.5f);
        } else if (selectedWeapon_ != 2) {
            const float longScale = selectedWeapon_ == 5 ? 1.18f : (selectedWeapon_ == 4 ? 1.05f : 1.0f);
            renderer_.draw(cube_, root * glm::translate(glm::mat4(1), {0,0,0.32f}) *
                           glm::scale(glm::mat4(1), {0.19f,0.18f,0.85f * longScale}), dark);
            renderer_.draw(cylinder_, root * glm::translate(glm::mat4(1), {0,0,0.92f}) *
                           glm::rotate(glm::mat4(1), kPi * 0.5f, {1,0,0}) *
                           glm::scale(glm::mat4(1), {0.09f,0.55f * longScale,0.09f}),
                           {0.07f,0.08f,0.11f,1});
            renderer_.draw(cube_, root * glm::translate(glm::mat4(1), {0,-0.2f,0.2f}) *
                           glm::rotate(glm::mat4(1), -0.14f, {1,0,0}) * glm::scale(glm::mat4(1), {0.14f,0.42f,0.22f}),
                           {0.08f,0.09f,0.13f,1});
            renderer_.draw(cube_, root * glm::translate(glm::mat4(1), {0,0.105f,0.38f}) *
                           glm::scale(glm::mat4(1), {0.12f,0.028f,0.72f}), glow, 0, 0.65f);
        } else {
            renderer_.draw(cube_, root * glm::translate(glm::mat4(1), {0,0,0.28f}) *
                           glm::scale(glm::mat4(1), {0.24f,0.21f,0.72f}), {0.07f,0.065f,0.055f,1});
            renderer_.draw(cylinder_, root * glm::translate(glm::mat4(1), {0,0,0.91f}) *
                           glm::rotate(glm::mat4(1), kPi * 0.5f, {1,0,0}) * glm::scale(glm::mat4(1), {0.14f,0.72f,0.14f}),
                           dark);
            renderer_.draw(cube_, root * glm::translate(glm::mat4(1), {0,-0.16f,0.08f}) *
                           glm::scale(glm::mat4(1), {0.16f,0.40f,0.20f}), {0.11f,0.08f,0.04f,1});
            renderer_.draw(cube_, root * glm::translate(glm::mat4(1), {0,0.13f,0.32f}) *
                           glm::scale(glm::mat4(1), {0.18f,0.035f,0.58f}), glow, 0, 0.55f);
        }
        if (muzzleFlash_ > 0.0f) {
            const float length = selectedWeapon_ == 0 ? 0.78f : (selectedWeapon_ == 2 ? 1.30f :
                                 (selectedWeapon_ == 5 ? 1.55f : 1.30f));
            renderer_.draw(cube_, root * glm::translate(glm::mat4(1), {0,0,length}) *
                           glm::scale(glm::mat4(1), glm::vec3(0.11f + muzzleFlash_ * 0.1f)),
                           {1.0f,0.42f,0.04f,1}, 0, 4.0f);
        }
    }

    void renderHUD() {
        const float width = static_cast<float>(renderer_.width());
        const float height = static_cast<float>(renderer_.height());
        const glm::vec4 white{0.88f,0.94f,1.0f,1};
        const glm::vec4 cyan{0.12f,0.88f,1.0f,1};
        const glm::vec4 red{1.0f,0.12f,0.26f,1};
        const glm::vec4 panel{0.008f,0.014f,0.035f,0.78f};
        static constexpr std::array<const char*, 3> mapNames = {{"NEON YARD", "DUST DEPOT", "ICE LAB"}};
        static constexpr std::array<const char*, 3> difficultyNames = {{"ROOKIE", "VETERAN", "NIGHTMARE"}};
        static constexpr std::array<const char*, 3> qualityNames = {{"POTATO", "BALANCED", "ULTRA"}};

        if (mode_ == Mode::Title) {
            renderer_.rect(0, 0, width, height, {0.005f,0.008f,0.025f,0.56f});
            renderer_.text(width * 0.5f, height * 0.24f, "NEON ASSAULT", 9.0f, cyan, true);
            renderer_.text(width * 0.5f, height * 0.24f + 82.0f, "3D ARENA SHOOTER", 3.4f, white, true);
            renderer_.rect(width * 0.5f - 180, height * 0.62f - 18, 360, 50, {0.02f,0.18f,0.25f,0.88f});
            renderer_.text(width * 0.5f, height * 0.62f, "PRESS ENTER TO DEPLOY", 3.0f, white, true);
            renderer_.rect(width * 0.5f - 310, height * 0.45f, 620, 102, panel);
            renderer_.text(width * 0.5f, height * 0.465f,
                           std::string("M MAP: ") + mapNames[static_cast<std::size_t>(selectedMap_)], 2.2f, white, true);
            renderer_.text(width * 0.5f, height * 0.465f + 29,
                           std::string("D DIFFICULTY: ") + difficultyNames[static_cast<std::size_t>(difficulty_)],
                           2.2f, difficulty_ == 2 ? red : cyan, true);
            renderer_.text(width * 0.5f, height * 0.465f + 58,
                           std::string("Q GRAPHICS: ") + qualityNames[static_cast<std::size_t>(quality_)],
                           2.2f, quality_ == 0 ? glm::vec4(0.35f,1.0f,0.45f,1) : white, true);
            renderer_.text(width * 0.5f, height - 84, "WASD MOVE  MOUSE AIM  1-6 WEAPONS  R RELOAD", 2.0f,
                           {0.55f,0.68f,0.78f,1}, true);
            renderer_.text(width - 12, height - 20, std::string("V") + NEON_VERSION, 1.4f,
                           {0.35f,0.45f,0.55f,1}, true);
            return;
        }

        const float centerX = width * 0.5f;
        const float centerY = height * 0.5f;
        const float crossGap = 8.0f + weaponKick_ * 5.0f;
        const glm::vec4 crossColor = hitMarker_ > 0 ? (killMarker_ > 0 ? red : white) : cyan;
        renderer_.rect(centerX - 1, centerY - crossGap - 8, 2, 8, crossColor);
        renderer_.rect(centerX - 1, centerY + crossGap, 2, 8, crossColor);
        renderer_.rect(centerX - crossGap - 8, centerY - 1, 8, 2, crossColor);
        renderer_.rect(centerX + crossGap, centerY - 1, 8, 2, crossColor);
        if (hitMarker_ > 0) {
            renderer_.rect(centerX - 11, centerY - 11, 4, 4, crossColor);
            renderer_.rect(centerX + 7, centerY - 11, 4, 4, crossColor);
            renderer_.rect(centerX - 11, centerY + 7, 4, 4, crossColor);
            renderer_.rect(centerX + 7, centerY + 7, 4, 4, crossColor);
        }

        renderer_.rect(24, height - 102, 286, 72, panel);
        renderer_.text(40, height - 88, "HP", 2.2f, white);
        renderer_.rect(92, height - 85, 195, 18, {0.08f,0.08f,0.10f,1});
        renderer_.rect(92, height - 85, 195 * saturate(playerHealth_ / 100.0f), 18,
                       playerHealth_ > 35 ? glm::vec4(0.08f,0.9f,0.45f,1) : red);
        renderer_.text(40, height - 56, std::to_string(static_cast<int>(std::ceil(playerHealth_))), 2.6f, white);

        const WeaponState& weapon = weapons_[static_cast<std::size_t>(selectedWeapon_)];
        const WeaponDefinition& definition = weaponDefinitions_[static_cast<std::size_t>(selectedWeapon_)];
        renderer_.rect(width - 354, height - 120, 330, 90, panel);
        renderer_.text(width - 332, height - 100, definition.name, 2.4f, glm::vec4(definition.color, 1));
        renderer_.text(width - 332, height - 66,
                       std::to_string(weapon.loaded) + "/" + std::to_string(weapon.reserve), 4.2f, white);
        if (reloadTimer_ > 0.0f) {
            const float progress = 1.0f - reloadTimer_ / definition.reload;
            renderer_.rect(width - 332, height - 40, 280, 5, {0.08f,0.08f,0.11f,1});
            renderer_.rect(width - 332, height - 40, 280 * saturate(progress), 5, cyan);
            renderer_.text(width - 68, height - 64, "R", 2.2f, cyan, true);
        }

        renderer_.rect(24, 24, 270, 78, panel);
        renderer_.text(42, 40, "WAVE " + std::to_string(wave_), 2.6f, cyan);
        renderer_.text(42, 72, "HOSTILES " + std::to_string(bots_.size()), 2.0f, white);
        renderer_.text(width - 34, 36, "SCORE " + std::to_string(score_), 2.4f, white, true);

        if (bots_.empty() && mode_ == Mode::Playing) {
            renderer_.text(centerX, height * 0.28f, "AREA CLEAR", 4.0f, cyan, true);
            renderer_.text(centerX, height * 0.28f + 42, "NEXT WAVE IN " +
                           std::to_string(std::max(0, static_cast<int>(std::ceil(nextWaveTimer_)))), 2.2f, white, true);
        }
        if (damageFlash_ > 0.0f) {
            const float alpha = damageFlash_ * 0.18f;
            renderer_.rect(0, 0, width, 24, {1,0,0,alpha});
            renderer_.rect(0, height - 24, width, 24, {1,0,0,alpha});
            renderer_.rect(0, 0, 24, height, {1,0,0,alpha});
            renderer_.rect(width - 24, 0, 24, height, {1,0,0,alpha});
        }

        if (mode_ == Mode::Paused) {
            renderer_.rect(0, 0, width, height, {0.003f,0.005f,0.015f,0.75f});
            renderer_.text(centerX, height * 0.38f, "PAUSED", 7.0f, cyan, true);
            renderer_.text(centerX, height * 0.52f, "PRESS ESC TO RESUME", 2.8f, white, true);
            renderer_.text(centerX, height * 0.60f,
                           std::string(mapNames[static_cast<std::size_t>(selectedMap_)]) + "  " +
                           difficultyNames[static_cast<std::size_t>(difficulty_)] + "  " +
                           qualityNames[static_cast<std::size_t>(quality_)], 1.9f, {0.55f,0.72f,0.82f,1}, true);
        } else if (mode_ == Mode::Dead) {
            renderer_.rect(0, 0, width, height, {0.16f,0.0f,0.015f,0.65f});
            renderer_.text(centerX, height * 0.32f, "MISSION FAILED", 6.4f, red, true);
            renderer_.text(centerX, height * 0.46f, "SCORE " + std::to_string(score_), 3.3f, white, true);
            renderer_.text(centerX, height * 0.54f, "WAVE " + std::to_string(wave_), 2.5f, white, true);
            renderer_.text(centerX, height * 0.68f, "PRESS ENTER TO REDEPLOY", 2.7f, cyan, true);
        }
    }

    float randomRange(float low, float high) {
        return std::uniform_real_distribution<float>(low, high)(random_);
    }

    glm::vec3 randomUnit() {
        glm::vec3 result{randomRange(-1, 1), randomRange(-1, 1), randomRange(-1, 1)};
        if (glm::length2(result) < 0.0001f) result = {0, 1, 0};
        return glm::normalize(result);
    }

    glm::vec3 randomUnitFlat() {
        glm::vec3 result{randomRange(-1, 1), 0, randomRange(-1, 1)};
        if (glm::length2(result) < 0.0001f) result = {1, 0, 0};
        return glm::normalize(result);
    }

    Renderer& renderer_;
    AudioEngine& audio_;
    Mesh cube_;
    Mesh cylinder_;
    GltfModel helmet_;
    GltfModel bottle_;
    std::array<WeaponDefinition, 6> weaponDefinitions_{};
    std::array<WeaponState, 6> weapons_{};
    std::vector<Block> blocks_;
    std::vector<glm::vec3> spawnPoints_;
    std::vector<Bot> bots_;
    std::vector<Pickup> pickups_;
    std::vector<Particle> particles_;
    std::vector<Tracer> tracers_;
    std::mt19937 random_{0x4e454f4eu};
    Mode mode_ = Mode::Title;
    glm::vec3 playerPosition_{0,0,8};
    float playerVerticalVelocity_ = 0.0f;
    float playerHealth_ = 100.0f;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float time_ = 0.0f;
    float stepTime_ = 0.0f;
    float weaponCooldown_ = 0.0f;
    float reloadTimer_ = 0.0f;
    float autoReloadDelay_ = 0.0f;
    float nextWaveTimer_ = 0.0f;
    float muzzleFlash_ = 0.0f;
    float weaponKick_ = 0.0f;
    float damageFlash_ = 0.0f;
    float cameraShake_ = 0.0f;
    float hitMarker_ = 0.0f;
    float killMarker_ = 0.0f;
    int selectedWeapon_ = 1;
    int reloadWeapon_ = -1;
    int score_ = 0;
    int kills_ = 0;
    int wave_ = 0;
    int selectedMap_ = 0;
    int difficulty_ = 1;
    int quality_ = 1;
    bool crouching_ = false;
    bool moving_ = false;
    bool onGround_ = true;
    bool jumpWasDown_ = false;
    bool quitRequested_ = false;
};

int runSelfTests() {
    int failures = 0;
    const auto expect = [&failures](bool condition, const char* name) {
        if (!condition) {
            std::cerr << "FAILED: " << name << '\n';
            ++failures;
        }
    };
    const AABB box{{-1,-1,-1}, {1,1,1}};
    float distance = 0.0f;
    expect(rayAABB({0,0,5}, {0,0,-1}, box, distance) && std::abs(distance - 4.0f) < 0.001f,
           "ray hits AABB at expected distance");
    expect(!rayAABB({3,0,5}, {0,0,-1}, box, distance), "ray misses AABB");
    expect(raySphere({0,0,5}, {0,0,-1}, {0,0,0}, 1.0f, distance) &&
           std::abs(distance - 4.0f) < 0.001f, "ray hits sphere at expected distance");
    expect(!raySphere({4,0,5}, {0,0,-1}, {0,0,0}, 1.0f, distance), "ray misses sphere");
    if (failures == 0) std::cout << "Neon Assault self-tests passed.\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace neon

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--self-test") return neon::runSelfTests();

    SDL_SetAppMetadata("Neon Assault", NEON_VERSION, "io.github.deathamir.neonassault");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, 1);

    SDL_Window* window = SDL_CreateWindow("NEON ASSAULT", 1600, 900,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        std::fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }

    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (context == nullptr) {
        std::fprintf(stderr, "OpenGL context creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    SDL_GL_MakeCurrent(window, context);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::fprintf(stderr, "Could not load OpenGL functions through GLEW.\n");
        SDL_GL_DestroyContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    SDL_GL_SetSwapInterval(1);

    int result = EXIT_SUCCESS;
    try {
        neon::Renderer renderer;
        neon::AudioEngine audio;
        neon::Game game(renderer, audio);
        neon::InputFrame input;
        bool running = true;
        bool mouseCaptured = false;
        auto previous = std::chrono::steady_clock::now();

        while (running && !game.quitRequested()) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) running = false;
                if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) input.fireHeld = false;
                game.handleEvent(event, input);
            }

            const auto now = std::chrono::steady_clock::now();
            const float deltaTime = std::chrono::duration<float>(now - previous).count();
            previous = now;
            game.update(deltaTime, input);

            const bool desiredCapture = game.wantsMouseCapture();
            if (desiredCapture != mouseCaptured) {
                SDL_SetWindowRelativeMouseMode(window, desiredCapture);
                mouseCaptured = desiredCapture;
                input.mouseX = input.mouseY = 0.0f;
            }

            int width = 1;
            int height = 1;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            game.render(width, height);
            SDL_GL_SwapWindow(window);
            input.clearTransient();
            SDL_Delay(1);
        }
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Fatal error: %s\n", exception.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Neon Assault", exception.what(), window);
        result = EXIT_FAILURE;
    }

    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
