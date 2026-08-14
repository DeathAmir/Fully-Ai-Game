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

#include "net_client.hpp"
#include "discord_rpc.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
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
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
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

        parts_.clear();
        animations_.clear();
        nodes_.clear();

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

        nodes_.resize(model.nodes.size());
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            const tinygltf::Node& source = model.nodes[i];
            Node& destination = nodes_[i];
            destination.baseMatrix = nodeMatrix(source);
            if (source.translation.size() == 3) {
                destination.translation = {static_cast<float>(source.translation[0]),
                                           static_cast<float>(source.translation[1]),
                                           static_cast<float>(source.translation[2])};
            }
            if (source.scale.size() == 3) {
                destination.scale = {static_cast<float>(source.scale[0]), static_cast<float>(source.scale[1]),
                                     static_cast<float>(source.scale[2])};
            }
            if (source.rotation.size() == 4) {
                destination.rotation = {static_cast<float>(source.rotation[3]),
                                        static_cast<float>(source.rotation[0]),
                                        static_cast<float>(source.rotation[1]),
                                        static_cast<float>(source.rotation[2])};
            }
            destination.usesMatrix = source.matrix.size() == 16;
            destination.children = source.children;
            for (int child : source.children) {
                if (child >= 0 && child < static_cast<int>(nodes_.size()))
                    nodes_[static_cast<std::size_t>(child)].parent = static_cast<int>(i);
            }
        }

        const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
        if (sceneIndex < 0) return false;
        sceneRoots_ = model.scenes[static_cast<std::size_t>(sceneIndex)].nodes;
        for (int node : model.scenes[static_cast<std::size_t>(sceneIndex)].nodes) {
            visitNode(model, node);
        }

        for (const tinygltf::Animation& sourceAnimation : model.animations) {
            Animation animation;
            animation.name = sourceAnimation.name;
            for (const tinygltf::AnimationChannel& sourceChannel : sourceAnimation.channels) {
                if (sourceChannel.target_node < 0 ||
                    sourceChannel.target_node >= static_cast<int>(nodes_.size()) ||
                    sourceChannel.sampler < 0 ||
                    sourceChannel.sampler >= static_cast<int>(sourceAnimation.samplers.size())) continue;
                const tinygltf::AnimationSampler& sampler =
                    sourceAnimation.samplers[static_cast<std::size_t>(sourceChannel.sampler)];
                if (sampler.input < 0 || sampler.output < 0) continue;
                const tinygltf::Accessor& input = model.accessors[static_cast<std::size_t>(sampler.input)];
                const tinygltf::Accessor& output = model.accessors[static_cast<std::size_t>(sampler.output)];
                AnimationChannel channel;
                channel.node = sourceChannel.target_node;
                channel.step = sampler.interpolation == "STEP";
                if (sourceChannel.target_path == "translation") channel.path = AnimationPath::Translation;
                else if (sourceChannel.target_path == "rotation") channel.path = AnimationPath::Rotation;
                else if (sourceChannel.target_path == "scale") channel.path = AnimationPath::Scale;
                else continue;
                const std::size_t sampleCount = std::min(input.count, output.count);
                channel.times.reserve(sampleCount);
                channel.values.reserve(sampleCount);
                for (std::size_t sample = 0; sample < sampleCount; ++sample) {
                    const float sampleTime = readFloat(model, input, sample);
                    channel.times.push_back(sampleTime);
                    channel.values.push_back(channel.path == AnimationPath::Rotation
                        ? readVec4(model, output, sample)
                        : glm::vec4(readVec3(model, output, sample), 0.0f));
                    animation.duration = std::max(animation.duration, sampleTime);
                }
                if (!channel.times.empty()) animation.channels.push_back(std::move(channel));
            }
            if (!animation.channels.empty()) animations_.push_back(std::move(animation));
        }
        return !parts_.empty();
    }

    void draw(Renderer& renderer, const glm::mat4& transform, const glm::vec4& tint = glm::vec4(1.0f)) const {
        drawAnimated(renderer, transform, -1, 0.0f, tint);
    }

    void drawAnimated(Renderer& renderer, const glm::mat4& transform, int animationIndex, float animationTime,
                      const glm::vec4& tint = glm::vec4(1.0f)) const {
        std::vector<glm::vec3> translations;
        std::vector<glm::quat> rotations;
        std::vector<glm::vec3> scales;
        translations.reserve(nodes_.size());
        rotations.reserve(nodes_.size());
        scales.reserve(nodes_.size());
        for (const Node& node : nodes_) {
            translations.push_back(node.translation);
            rotations.push_back(node.rotation);
            scales.push_back(node.scale);
        }
        if (animationIndex >= 0 && animationIndex < static_cast<int>(animations_.size())) {
            const Animation& animation = animations_[static_cast<std::size_t>(animationIndex)];
            const float localTime = animation.duration > 0.0f ? std::fmod(animationTime, animation.duration) : 0.0f;
            for (const AnimationChannel& channel : animation.channels) {
                if (channel.times.empty()) continue;
                const auto upper = std::upper_bound(channel.times.begin(), channel.times.end(), localTime);
                const std::size_t second = upper == channel.times.end()
                    ? channel.times.size() - 1 : static_cast<std::size_t>(upper - channel.times.begin());
                const std::size_t first = second == 0 ? 0 : second - 1;
                float blend = 0.0f;
                if (!channel.step && second != first) {
                    const float span = channel.times[second] - channel.times[first];
                    if (span > 0.00001f) blend = saturate((localTime - channel.times[first]) / span);
                }
                const glm::vec4 value = glm::mix(channel.values[first], channel.values[second], blend);
                const std::size_t nodeIndex = static_cast<std::size_t>(channel.node);
                if (channel.path == AnimationPath::Translation) translations[nodeIndex] = glm::vec3(value);
                else if (channel.path == AnimationPath::Scale) scales[nodeIndex] = glm::vec3(value);
                else {
                    const glm::quat firstRotation(channel.values[first].w, channel.values[first].x,
                                                  channel.values[first].y, channel.values[first].z);
                    const glm::quat secondRotation(channel.values[second].w, channel.values[second].x,
                                                   channel.values[second].y, channel.values[second].z);
                    rotations[nodeIndex] = glm::normalize(glm::slerp(firstRotation, secondRotation, blend));
                }
            }
        }

        std::vector<glm::mat4> worlds(nodes_.size(), glm::mat4(1.0f));
        std::vector<bool> resolved(nodes_.size(), false);
        const std::function<const glm::mat4&(int)> resolve = [&](int index) -> const glm::mat4& {
            const std::size_t nodeIndex = static_cast<std::size_t>(index);
            if (resolved[nodeIndex]) return worlds[nodeIndex];
            const Node& node = nodes_[nodeIndex];
            const bool animated = animationIndex >= 0 && !node.usesMatrix;
            const glm::mat4 local = animated
                ? glm::translate(glm::mat4(1.0f), translations[nodeIndex]) *
                  glm::mat4_cast(rotations[nodeIndex]) * glm::scale(glm::mat4(1.0f), scales[nodeIndex])
                : node.baseMatrix;
            worlds[nodeIndex] = node.parent >= 0 ? resolve(node.parent) * local : local;
            resolved[nodeIndex] = true;
            return worlds[nodeIndex];
        };
        for (int root : sceneRoots_) if (root >= 0) resolve(root);

        for (const Part& part : parts_) {
            const glm::vec4 color = part.color * tint;
            const glm::mat4 nodeTransform = part.node >= 0 ? resolve(part.node) : glm::mat4(1.0f);
            renderer.draw(part.mesh, transform * nodeTransform, color, part.texture);
        }
    }

    bool valid() const { return !parts_.empty(); }

    int findAnimation(std::string_view name) const {
        for (std::size_t i = 0; i < animations_.size(); ++i) {
            if (animations_[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }

private:
    struct Part {
        Mesh mesh;
        int node = -1;
        glm::vec4 color{1.0f};
        GLuint texture = 0;
    };

    struct Node {
        glm::mat4 baseMatrix{1.0f};
        glm::vec3 translation{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
        std::vector<int> children;
        int parent = -1;
        bool usesMatrix = false;
    };

    enum class AnimationPath { Translation, Rotation, Scale };

    struct AnimationChannel {
        int node = -1;
        AnimationPath path = AnimationPath::Translation;
        std::vector<float> times;
        std::vector<glm::vec4> values;
        bool step = false;
    };

    struct Animation {
        std::string name;
        float duration = 0.0f;
        std::vector<AnimationChannel> channels;
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

    static glm::vec4 readVec4(const tinygltf::Model& model, const tinygltf::Accessor& accessor,
                              std::size_t index) {
        const auto& view = model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
        const std::size_t stride = accessor.ByteStride(view) != 0 ? accessor.ByteStride(view) : sizeof(float) * 4;
        const auto* value = reinterpret_cast<const float*>(accessorData(model, accessor) + index * stride);
        return {value[0], value[1], value[2], value[3]};
    }

    static float readFloat(const tinygltf::Model& model, const tinygltf::Accessor& accessor,
                           std::size_t index) {
        const auto& view = model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
        const std::size_t stride = accessor.ByteStride(view) != 0 ? accessor.ByteStride(view) : sizeof(float);
        return *reinterpret_cast<const float*>(accessorData(model, accessor) + index * stride);
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

    void visitNode(const tinygltf::Model& model, int nodeIndex) {
        const tinygltf::Node& node = model.nodes[static_cast<std::size_t>(nodeIndex)];
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
                part.node = nodeIndex;
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
        for (int child : node.children) visitNode(model, child);
    }

    std::vector<Part> parts_;
    std::vector<GLuint> textures_;
    std::vector<Node> nodes_;
    std::vector<int> sceneRoots_;
    std::vector<Animation> animations_;
};

enum class SoundEffect { Pistol, Rifle, Shotgun, Enemy, Hit, Kill, Pickup, Reload, Empty, Start, Footstep, Plant, Defuse, Grenade };

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
            case SoundEffect::Footstep:
                addVoice(68.0f, 0.065f, 0.16f, 0.72f, -0.35f);
                break;
            case SoundEffect::Plant:
                addVoice(610.0f, 0.09f, 0.14f, 0.05f, 0.15f);
                addVoice(760.0f, 0.08f, 0.12f, 0.03f, 0.25f, 0.12f);
                break;
            case SoundEffect::Defuse:
                addVoice(880.0f, 0.11f, 0.12f, 0.02f, -0.18f);
                addVoice(1040.0f, 0.12f, 0.10f, 0.02f, 0.12f, 0.13f);
                break;
            case SoundEffect::Grenade:
                addVoice(82.0f, 0.35f, 0.48f, 0.82f, -0.74f);
                addVoice(42.0f, 0.42f, 0.35f, 0.68f, -0.45f);
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
    std::array<bool, 10> weaponPressed{};
    bool mapPressed = false;
    bool difficultyPressed = false;
    bool qualityPressed = false;
    bool gameModePressed = false;
    bool operatorPressed = false;
    bool inventoryPressed = false;
    bool thirdPersonPressed = false;
    bool aimHeld = false;
    bool scoreboardHeld = false;
    bool usePressed = false;
    bool grenadePressed = false;
    bool terroristPressed = false;
    bool counterTerroristPressed = false;
    bool fullscreenPressed = false;

    void clearTransient() {
        mouseX = mouseY = 0.0f;
        wheel = 0;
        firePressed = reloadPressed = pausePressed = confirmPressed = false;
        mapPressed = difficultyPressed = qualityPressed = false;
        gameModePressed = operatorPressed = inventoryPressed = thirdPersonPressed = false;
        usePressed = grenadePressed = terroristPressed = counterTerroristPressed = fullscreenPressed = false;
        weaponPressed.fill(false);
    }
};

enum class Rarity { Common, Uncommon, Rare, Epic, Legendary, Mythic };

const char* rarityName(Rarity rarity) {
    switch (rarity) {
        case Rarity::Common: return "COMMON";
        case Rarity::Uncommon: return "UNCOMMON";
        case Rarity::Rare: return "RARE";
        case Rarity::Epic: return "EPIC";
        case Rarity::Legendary: return "LEGENDARY";
        case Rarity::Mythic: return "MYTHIC";
    }
    return "COMMON";
}

glm::vec4 rarityColor(Rarity rarity) {
    switch (rarity) {
        case Rarity::Common: return {0.72f, 0.76f, 0.82f, 1.0f};
        case Rarity::Uncommon: return {0.18f, 0.92f, 0.38f, 1.0f};
        case Rarity::Rare: return {0.10f, 0.56f, 1.0f, 1.0f};
        case Rarity::Epic: return {0.68f, 0.20f, 1.0f, 1.0f};
        case Rarity::Legendary: return {1.0f, 0.58f, 0.08f, 1.0f};
        case Rarity::Mythic: return {1.0f, 0.08f, 0.38f, 1.0f};
    }
    return glm::vec4(1.0f);
}

enum class GameMode { BombDefusal, Survival, Elimination, HeadHunter, Mayhem };

const char* gameModeName(GameMode mode) {
    switch (mode) {
        case GameMode::BombDefusal: return "BOMB DEFUSAL";
        case GameMode::Survival: return "SURVIVAL";
        case GameMode::Elimination: return "ELIMINATION";
        case GameMode::HeadHunter: return "HEAD HUNTER";
        case GameMode::Mayhem: return "MYTHIC MAYHEM";
    }
    return "SURVIVAL";
}

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
    Rarity rarity = Rarity::Common;
    const char* model = "";
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
    bool visible = true;
};

struct ModelPlacement {
    int model = -1;
    glm::vec3 position{};
    glm::vec3 scale{1.0f};
    float yaw = 0.0f;
    glm::vec4 tint{1.0f};
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
    float alertTimer = 0.0f;
    float aimStability = 0.0f;
    float animationTime = 0.0f;
    glm::vec3 lastSeenPlayer{};
    glm::vec3 tacticalGoal{};
    std::vector<glm::vec3> path;
    std::size_t pathCursor = 0;
    int role = 0;
    int burstShots = 0;
    int model = 0;
    int type = 0;
};

struct Achievement {
    const char* id = "";
    const char* title = "";
    const char* description = "";
    int target = 1;
    int progress = 0;
    bool unlocked = false;
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
    enum class Mode { Protection, Title, Playing, Paused, Dead };

    Game(Renderer& renderer, AudioEngine& audio)
        : renderer_(renderer), audio_(audio), cube_(makeCube()), cylinder_(makeCylinder()) {
        weaponDefinitions_ = {{
            {"V9 PISTOL", 12, 72, 30, 1, 0.28f, 1.05f, 0.0025f, 0.018f, false,
             {0.72f, 0.76f, 0.82f}, Rarity::Common, "Guns/Quaternius/Pistol.glb"},
            {"AR-4 RIFLE", 30, 180, 16, 1, 0.085f, 1.48f, 0.011f, 0.009f, true,
             {0.18f, 0.92f, 0.38f}, Rarity::Uncommon, "Guns/Quaternius/Assault Rifle.glb"},
            {"BREACH SHOTGUN", 8, 48, 13, 8, 0.76f, 1.82f, 0.052f, 0.032f, false,
             {0.10f, 0.56f, 1.0f}, Rarity::Rare, "Guns/Quaternius/Shotgun.glb"},
            {"VECTOR SMG", 40, 240, 11, 1, 0.055f, 1.32f, 0.020f, 0.006f, true,
             {0.68f, 0.20f, 1.0f}, Rarity::Epic, "Guns/Quaternius/Submachine Gun.glb"},
            {"BULLPUP DMR", 15, 90, 45, 1, 0.34f, 1.72f, 0.0035f, 0.021f, false,
             {1.0f, 0.58f, 0.08f}, Rarity::Legendary, "Guns/Quaternius/Bullpup.glb"},
            {"ORACLE SNIPER", 5, 35, 96, 1, 0.92f, 2.15f, 0.0005f, 0.048f, false,
             {1.0f, 0.58f, 0.08f}, Rarity::Legendary, "Guns/Quaternius/Sniper Rifle.glb"},
            {"NOVA BLASTER", 24, 144, 22, 1, 0.105f, 1.55f, 0.007f, 0.014f, true,
             {1.0f, 0.08f, 0.38f}, Rarity::Mythic, "Guns/Blasters/blaster-a.glb"},
            {"DRAGON CORE", 10, 60, 18, 6, 0.52f, 1.90f, 0.032f, 0.026f, false,
             {1.0f, 0.22f, 0.04f}, Rarity::Mythic, "Guns/Blasters/blaster-f.glb"},
            {"VOID CANNON", 6, 42, 72, 1, 0.68f, 2.05f, 0.0022f, 0.038f, false,
             {0.78f, 0.10f, 1.0f}, Rarity::Mythic, "Guns/Blasters/blaster-p.glb"},
            {"RAIL LANCER", 9, 54, 58, 1, 0.48f, 1.95f, 0.0012f, 0.030f, false,
             {1.0f, 0.70f, 0.08f}, Rarity::Legendary, "Guns/Quaternius/Assault Rifle-Bgvuu4CUMV.glb"}
        }};
        const char* base = SDL_GetBasePath();
        const std::filesystem::path assetRoot = std::filesystem::path(base != nullptr ? base : "./") / "assets";
        helmet_.load(assetRoot / "SciFiHelmet" / "SciFiHelmet.gltf");
        bottle_.load(assetRoot / "WaterBottle.glb");
        for (std::size_t i = 0; i < gunModels_.size(); ++i)
            gunModels_[i].load(assetRoot / weaponDefinitions_[i].model);
        for (std::size_t i = 0; i < operatorModels_.size(); ++i) {
            const char variant = static_cast<char>('a' + static_cast<int>(i));
            operatorModels_[i].load(assetRoot / "Characters" / (std::string("character-") + variant + ".glb"));
        }
        const std::array<const char*, 16> environmentPaths = {{
            "Maps/Industrial/building-a.glb", "Maps/Industrial/building-b.glb",
            "Maps/Industrial/building-c.glb", "Maps/Industrial/detail-tank.glb",
            "Maps/Suburban/building-type-a.glb", "Maps/Suburban/building-type-b.glb",
            "Maps/Suburban/building-type-c.glb", "Maps/Suburban/tree-large.glb",
            "Maps/Roads/road-straight.glb", "Maps/Roads/road-crossroad.glb",
            "Maps/SpaceStation/wall.glb", "Maps/SpaceStation/door-double-closed.glb",
            "Maps/SpaceStation/computer-wide.glb", "Maps/SpaceStation/container-wide.glb",
            "Maps/SpaceStation/structure.glb", "Maps/SpaceStation/table-display-planet.glb"
        }};
        for (std::size_t i = 0; i < environmentModels_.size(); ++i)
            environmentModels_[i].load(assetRoot / environmentPaths[i]);
        buildMap();
        initializeAchievements();
        reset(false);
        network_.connect("irautox.ir", 9832, "iRxPlayer");
        gameMode_ = GameMode::BombDefusal;
        mode_ = Mode::Protection;
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
                if (event.button.button == SDL_BUTTON_RIGHT) input.aimHeld = true;
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_LEFT) input.fireHeld = false;
                if (event.button.button == SDL_BUTTON_RIGHT) input.aimHeld = false;
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
                if (event.key.scancode == SDL_SCANCODE_7) input.weaponPressed[6] = true;
                if (event.key.scancode == SDL_SCANCODE_8) input.weaponPressed[7] = true;
                if (event.key.scancode == SDL_SCANCODE_9) input.weaponPressed[8] = true;
                if (event.key.scancode == SDL_SCANCODE_0) input.weaponPressed[9] = true;
                if (event.key.scancode == SDL_SCANCODE_M) input.mapPressed = true;
                if (event.key.scancode == SDL_SCANCODE_D) input.difficultyPressed = true;
                if (event.key.scancode == SDL_SCANCODE_Q) input.qualityPressed = true;
                if (event.key.scancode == SDL_SCANCODE_G) {
                    input.gameModePressed = true;
                    input.grenadePressed = true;
                }
                if (event.key.scancode == SDL_SCANCODE_C) input.operatorPressed = true;
                if (event.key.scancode == SDL_SCANCODE_B) input.inventoryPressed = true;
                if (event.key.scancode == SDL_SCANCODE_TAB) input.scoreboardHeld = true;
                if (event.key.scancode == SDL_SCANCODE_V) input.thirdPersonPressed = true;
                if (event.key.scancode == SDL_SCANCODE_E) input.usePressed = true;
                if (event.key.scancode == SDL_SCANCODE_F1) input.terroristPressed = true;
                if (event.key.scancode == SDL_SCANCODE_F2) input.counterTerroristPressed = true;
                if (event.key.scancode == SDL_SCANCODE_F11) input.fullscreenPressed = true;
                break;
            case SDL_EVENT_KEY_UP:
                if (event.key.scancode == SDL_SCANCODE_TAB) input.scoreboardHeld = false;
                break;
            default: break;
        }
    }

    void update(float deltaTime, InputFrame& input) {
        time_ += deltaTime;
        deltaTime = std::min(deltaTime, 0.05f);
        achievementPopupTimer_ = std::max(0.0f, achievementPopupTimer_ - deltaTime);
        protectionTimer_ += deltaTime;
        network_.poll(deltaTime);
        discordTimer_ -= deltaTime;
        if (discordTimer_ <= 0.0f) {
            const std::string details = network_.connected() ? "Online Bomb Defusal" : "Offline Practice";
            const std::string presenceState = network_.connected()
                ? std::string(network_.team() == irx::Team::Terrorist ? "Terrorist" : "Counter-Terrorist") +
                  "  " + std::to_string(network_.snapshot().terroristScore) + ":" +
                  std::to_string(network_.snapshot().counterTerroristScore)
                : std::string(gameModeName(gameMode_));
            discord_.update(details, presenceState,
                            network_.connected() ? static_cast<int>(network_.snapshot().players.size()) + 1 : 1, 32);
            discordTimer_ = 12.0f;
        }
        scoreboardOpen_ = input.scoreboardHeld;
        aiming_ = input.aimHeld;
        if (input.fullscreenPressed) fullscreenToggleRequested_ = true;
        if (input.terroristPressed) network_.requestTeam(irx::Team::Terrorist);
        if (input.counterTerroristPressed) network_.requestTeam(irx::Team::CounterTerrorist);

        if (mode_ == Mode::Protection) {
            updateParticles(deltaTime);
            if (protectionTimer_ >= 2.35f) mode_ = Mode::Title;
            return;
        }

        if (mode_ == Mode::Title) {
            if (input.mapPressed) {
                selectedMap_ = (selectedMap_ + 1) % 6;
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
            if (input.gameModePressed) {
                gameMode_ = static_cast<GameMode>((static_cast<int>(gameMode_) + 1) % 5);
                audio_.play(SoundEffect::Pickup);
            }
            if (input.operatorPressed) {
                selectedOperator_ = (selectedOperator_ + 1) % static_cast<int>(operatorModels_.size());
                audio_.play(SoundEffect::Pickup);
            }
            for (int i = 0; i < static_cast<int>(weapons_.size()); ++i) {
                if (input.weaponPressed[static_cast<std::size_t>(i)]) switchWeapon(i);
            }
            if (input.confirmPressed || input.firePressed) {
                if (network_.connected()) gameMode_ = GameMode::BombDefusal;
                reset(!network_.connected());
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
            if (network_.connected() && network_.snapshot().selfHealth > 0) {
                playerHealth_ = static_cast<float>(network_.snapshot().selfHealth);
                mode_ = Mode::Playing;
            } else if (!network_.connected() && (input.confirmPressed || input.firePressed)) {
                reset(true);
                mode_ = Mode::Playing;
            }
            return;
        }
        if (input.inventoryPressed) {
            inventoryOpen_ = !inventoryOpen_;
            audio_.play(SoundEffect::Pickup);
        }
        if (input.thirdPersonPressed) thirdPerson_ = !thirdPerson_;
        if (input.pausePressed) {
            mode_ = mode_ == Mode::Paused ? Mode::Playing : Mode::Paused;
            return;
        }
        if (mode_ == Mode::Paused) return;
        if (inventoryOpen_) {
            for (int i = 0; i < static_cast<int>(weapons_.size()); ++i)
                if (input.weaponPressed[static_cast<std::size_t>(i)]) switchWeapon(i);
            return;
        }

        const bool* keys = SDL_GetKeyboardState(nullptr);
        updatePlayer(deltaTime, keys, input);
        updateWeapons(deltaTime, input);
        if (network_.connected()) {
            std::uint8_t actions = 0;
            if (input.fireHeld || input.firePressed) actions |= 1u;
            if (crouching_) actions |= 2u;
            if (reloadTimer_ > 0.0f || input.reloadPressed) actions |= 4u;
            if (keys[SDL_SCANCODE_E] && network_.team() == irx::Team::Terrorist) actions |= 8u;
            if (keys[SDL_SCANCODE_E] && network_.team() == irx::Team::CounterTerrorist) actions |= 16u;
            if (input.grenadePressed && grenadeCooldown_ <= 0.0f) {
                actions |= 32u;
                grenadeCooldown_ = 5.0f;
                audio_.play(SoundEffect::Grenade);
            }
            if (moving_) actions |= 64u;
            if (keys[SDL_SCANCODE_LSHIFT] && moving_ && !crouching_) actions |= 128u;
            network_.submit({playerPosition_, playerVelocity_, yaw_, pitch_,
                             static_cast<std::uint8_t>(selectedWeapon_), actions, irx::Team::Spectator});
            playerHealth_ = static_cast<float>(network_.snapshot().selfHealth);
            if (network_.snapshot().hasSelfPosition &&
                glm::distance2(playerPosition_, network_.snapshot().selfPosition) > 2.25f)
                playerPosition_ = network_.snapshot().selfPosition;
        } else {
            updateBots(deltaTime);
            updatePickups(deltaTime);
        }
        updateParticles(deltaTime);
        grenadeCooldown_ = std::max(0.0f, grenadeCooldown_ - deltaTime);

        damageFlash_ = std::max(0.0f, damageFlash_ - deltaTime * 1.65f);
        hitMarker_ = std::max(0.0f, hitMarker_ - deltaTime * 5.0f);
        killMarker_ = std::max(0.0f, killMarker_ - deltaTime * 3.0f);
        muzzleFlash_ = std::max(0.0f, muzzleFlash_ - deltaTime * 12.0f);
        weaponKick_ = glm::mix(weaponKick_, 0.0f, saturate(deltaTime * 14.0f));

        if (!network_.connected() && bots_.empty()) {
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
        if (mode_ == Mode::Title || mode_ == Mode::Protection) {
            eye = {std::sin(time_ * 0.17f) * 15.0f, 7.5f, std::cos(time_ * 0.17f) * 15.0f};
            forward = glm::normalize(glm::vec3(0.0f, 1.2f, 0.0f) - eye);
        } else {
            eye = cameraPosition();
            forward = cameraForward();
            if (thirdPerson_) {
                const glm::vec3 target = playerPosition_ + glm::vec3(0.0f, 1.25f, 0.0f);
                eye = target - forward * 5.4f + glm::vec3(0.0f, 1.85f, 0.0f);
                forward = glm::normalize(target - eye);
            }
        }
        const glm::mat4 view = glm::lookAt(eye, eye + forward, {0, 1, 0});
        const float fieldOfView = aiming_ && mode_ == Mode::Playing ? 54.0f : 72.0f;
        const glm::mat4 projection = glm::perspective(glm::radians(fieldOfView),
            static_cast<float>(std::max(width, 1)) / static_cast<float>(std::max(height, 1)), 0.045f, 100.0f);
        renderer_.setCamera(projection * view, eye);

        renderWorld();
        renderPickups();
        if (mode_ == Mode::Title) renderOperator({0.0f, 0.0f, 0.0f}, 0.0f, selectedOperator_, true);
        else if (thirdPerson_) renderOperator(playerPosition_, yaw_, selectedOperator_, moving_);
        renderBots();
        renderNetworkPlayers();
        renderParticles();

        if (mode_ != Mode::Title && mode_ != Mode::Protection && !thirdPerson_) {
            glClear(GL_DEPTH_BUFFER_BIT);
            renderWeapon(eye, forward);
        }
        renderHUD();
        renderer_.flushUI();
    }

    bool wantsMouseCapture() const { return mode_ == Mode::Playing && !inventoryOpen_; }
    bool quitRequested() const { return quitRequested_; }
    Mode mode() const { return mode_; }
    bool takeFullscreenToggle() {
        const bool requested = fullscreenToggleRequested_;
        fullscreenToggleRequested_ = false;
        return requested;
    }

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
        if (!startWave) selectedWeapon_ = 1;
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
        inventoryOpen_ = false;
        if (startWave) {
            mapUsageMask_ |= (1u << static_cast<unsigned>(selectedMap_));
            setAchievementProgress(6, std::popcount(mapUsageMask_ & 0x3fu));
            spawnWave();
        }
    }

    void buildMap() {
        blocks_.clear();
        spawnPoints_.clear();
        modelPlacements_.clear();
        auto add = [this](glm::vec3 center, glm::vec3 size, glm::vec4 color, bool collision = true,
                          bool visible = true) {
            blocks_.push_back({center, size, color, collision, visible});
        };
        auto addModel = [this](int model, glm::vec3 position, glm::vec3 scale, float yaw = 0.0f,
                               glm::vec4 tint = glm::vec4(1.0f)) {
            modelPlacements_.push_back({model, position, scale, yaw, tint});
        };
        const std::array<glm::vec4, 6> floorColors = {{
            {0.06f, 0.075f, 0.11f, 1}, {0.14f, 0.105f, 0.065f, 1}, {0.055f, 0.095f, 0.13f, 1},
            {0.075f, 0.10f, 0.075f, 1}, {0.035f, 0.055f, 0.085f, 1}, {0.09f, 0.075f, 0.065f, 1}
        }};
        const std::array<glm::vec4, 6> wallColors = {{
            {0.055f, 0.10f, 0.19f, 1}, {0.20f, 0.13f, 0.07f, 1}, {0.07f, 0.16f, 0.22f, 1},
            {0.10f, 0.14f, 0.10f, 1}, {0.075f, 0.10f, 0.16f, 1}, {0.15f, 0.11f, 0.07f, 1}
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
        } else if (selectedMap_ == 2) {
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
        } else if (selectedMap_ == 3) {
            const glm::vec4 hedge{0.08f, 0.20f, 0.09f, 1};
            for (int x = -15; x <= 15; x += 10) {
                for (int z = -15; z <= 15; z += 10) addModel(8, {static_cast<float>(x), 0.01f, static_cast<float>(z)}, {10,10,10});
            }
            addModel(9, {0, 0.02f, 0}, {10,10,10});
            const std::array<glm::vec3, 6> homes = {{
                {-15,0,-14}, {15,0,-14}, {-15,0,14}, {15,0,14}, {-15,0,0}, {15,0,0}
            }};
            for (std::size_t i = 0; i < homes.size(); ++i) {
                const int homeModel = 4 + static_cast<int>(i % 3);
                addModel(homeModel, homes[i], {7.2f,7.2f,7.2f}, i % 2 == 0 ? 0.0f : kPi);
                add(homes[i] + glm::vec3(0,2.7f,0), {8.8f,5.4f,7.0f}, wall, true,
                    !environmentModels_[static_cast<std::size_t>(homeModel)].valid());
            }
            for (const glm::vec3 tree : std::array<glm::vec3, 8>{{
                     {-8,0,-17}, {8,0,-17}, {-8,0,17}, {8,0,17}, {-18,0,-7}, {18,0,7}, {-4,0,5}, {5,0,-5}}}) {
                addModel(7, tree, {3.2f,3.2f,3.2f});
                add(tree + glm::vec3(0,1.0f,0), {1.1f,2.0f,1.1f}, hedge, true,
                    !environmentModels_[7].valid());
            }
            add({0,0.8f,-8}, {7.0f,1.6f,1.0f}, hedge);
            add({0,0.8f,8}, {7.0f,1.6f,1.0f}, hedge);
            spawnPoints_ = {{
                {-19,0,-19}, {19,0,-19}, {-19,0,19}, {19,0,19}, {-9,0,-13}, {9,0,13},
                {-10,0,2}, {10,0,-2}, {-2,0,-17}, {2,0,17}, {-17,0,7}, {17,0,-7}
            }};
        } else if (selectedMap_ == 4) {
            const glm::vec4 station{0.11f, 0.15f, 0.23f, 1};
            add({0,1.5f,0}, {5.5f,3.0f,5.5f}, station);
            for (int side = -1; side <= 1; side += 2) {
                add({side * 10.5f,1.4f,0}, {2.0f,2.8f,13.0f}, station);
                add({0,1.4f,side * 10.5f}, {13.0f,2.8f,2.0f}, station);
                addModel(10, {side * 20.5f,0.0f,0}, {4.0f,4.0f,4.0f}, kPi * 0.5f);
                addModel(10, {0,0.0f,side * 20.5f}, {4.0f,4.0f,4.0f});
                addModel(11, {side * 3.0f,0.0f,0}, {2.2f,2.2f,2.2f}, kPi * 0.5f);
                addModel(13, {side * 15.0f,0.0f,side * 7.0f}, {2.4f,2.4f,2.4f});
            }
            addModel(15, {0,0.0f,0}, {2.6f,2.6f,2.6f});
            addModel(12, {-7.5f,0.0f,-15.0f}, {2.5f,2.5f,2.5f}, kPi);
            addModel(12, {7.5f,0.0f,15.0f}, {2.5f,2.5f,2.5f});
            spawnPoints_ = {{
                {-18,0,-18}, {18,0,-18}, {-18,0,18}, {18,0,18}, {-15,0,0}, {15,0,0},
                {0,0,-15}, {0,0,15}, {-7,0,-7}, {7,0,7}, {-7,0,7}, {7,0,-7}
            }};
        } else {
            const glm::vec4 rust{0.30f,0.12f,0.035f,1};
            const std::array<glm::vec3, 6> factories = {{
                {-15,0,-15}, {15,0,-15}, {-15,0,15}, {15,0,15}, {-16,0,0}, {16,0,0}
            }};
            for (std::size_t i = 0; i < factories.size(); ++i) {
                const int factoryModel = static_cast<int>(i % 3);
                addModel(factoryModel, factories[i], {6.0f,6.0f,6.0f}, i % 2 ? kPi : 0.0f);
                add(factories[i] + glm::vec3(0,3.6f,0), {10.0f,7.2f,7.0f}, wall, true,
                    !environmentModels_[static_cast<std::size_t>(factoryModel)].valid());
            }
            for (const glm::vec3 tank : std::array<glm::vec3, 6>{{
                     {-7,0,-9}, {7,0,9}, {8,0,-8}, {-8,0,8}, {0,0,-16}, {0,0,16}}}) {
                addModel(3, tank, {4.0f,4.0f,4.0f});
                add(tank + glm::vec3(0,1.1f,0), {2.5f,2.2f,2.5f}, rust, true,
                    !environmentModels_[3].valid());
            }
            add({0,0.9f,0}, {7.0f,1.8f,2.4f}, rust);
            add({0,0.9f,-7}, {2.4f,1.8f,6.0f}, rust);
            add({0,0.9f,7}, {2.4f,1.8f,6.0f}, rust);
            spawnPoints_ = {{
                {-19,0,-19}, {19,0,-19}, {-19,0,19}, {19,0,19}, {-12,0,-5}, {12,0,5},
                {-5,0,-12}, {5,0,12}, {-15,0,8}, {15,0,-8}, {-3,0,3}, {3,0,-3}
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
        const glm::vec3 previousPosition = playerPosition_;
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
        if (moving_ && onGround_) {
            stepTime_ += deltaTime * (sprinting ? 1.32f : 1.0f);
            const int footstepBucket = static_cast<int>(stepTime_ * 3.1f);
            if (footstepBucket != lastFootstepBucket_) {
                lastFootstepBucket_ = footstepBucket;
                audio_.play(SoundEffect::Footstep);
            }
        }

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
        playerVelocity_ = (playerPosition_ - previousPosition) / std::max(deltaTime, 0.001f);
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
        weaponUsageMask_ |= (1u << static_cast<unsigned>(selectedWeapon_));
        if ((weaponUsageMask_ & 0x3ffu) == 0x3ffu) setAchievementProgress(4, 10);
        weaponCooldown_ = definition.interval;
        muzzleFlash_ = 1.0f;
        weaponKick_ = 1.0f;
        pitch_ = glm::clamp(pitch_ + definition.recoil, -1.42f, 1.42f);
        if (selectedWeapon_ == 0) audio_.play(SoundEffect::Pistol);
        else if (selectedWeapon_ == 2) audio_.play(SoundEffect::Shotgun);
        else if (selectedWeapon_ == 5) audio_.play(SoundEffect::Shotgun);
        else audio_.play(SoundEffect::Rifle);

        for (Bot& bot : bots_) {
            const float hearingDistance = definition.rarity == Rarity::Mythic ? 42.0f : 30.0f;
            if (glm::distance(bot.position, playerPosition_) < hearingDistance) {
                bot.lastSeenPlayer = playerPosition_;
                bot.alertTimer = std::max(bot.alertTimer, 4.0f);
            }
        }

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
        const Rarity killRarity = weaponDefinitions_[static_cast<std::size_t>(selectedWeapon_)].rarity;
        const int rarityBonus = killRarity == Rarity::Mythic ? 12 : (killRarity == Rarity::Legendary ? 6 : 0);
        const int deathParticles = (quality_ == 0 ? 7 : (quality_ == 1 ? 12 : 18)) + rarityBonus;
        const glm::vec4 killColor = rarityColor(killRarity);
        for (int i = 0; i < deathParticles; ++i) {
            glm::vec4 color = killRarity == Rarity::Mythic && i % 3 == 0
                ? glm::vec4(0.12f,0.85f,1.0f,1.0f) : killColor;
            glm::vec3 velocity = randomUnit() * randomRange(1.5f, killRarity == Rarity::Mythic ? 7.2f : 5.0f);
            if (killRarity == Rarity::Legendary && i % 2 == 0) velocity.y = std::abs(velocity.y) + 2.0f;
            particles_.push_back({deathPosition, velocity, color, randomRange(0.35f, 0.95f), 0.95f,
                                  randomRange(0.025f, 0.075f)});
        }
        if (killRarity == Rarity::Mythic) {
            for (int ray = 0; ray < 10; ++ray) {
                const float angle = static_cast<float>(ray) / 10.0f * kPi * 2.0f;
                const glm::vec3 direction{std::cos(angle), 0.18f, std::sin(angle)};
                tracers_.push_back({deathPosition, deathPosition + direction * 3.2f,
                                    ray % 2 == 0 ? killColor : glm::vec4(0.12f,0.85f,1.0f,0.9f), 0.22f});
            }
        }
        bots_.erase(bots_.begin() + hitBot);
        ++kills_;
        if (headshot) {
            ++headshots_;
            incrementAchievement(1);
        }
        incrementAchievement(0);
        incrementAchievement(2);
        if (weaponDefinitions_[static_cast<std::size_t>(selectedWeapon_)].rarity == Rarity::Mythic) {
            ++mythicKills_;
            incrementAchievement(5);
        }
        score_ += 100 + wave_ * 25 + (headshot ? 75 : 0);
        setAchievementProgress(7, score_);
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
        int requested = 3 + wave_ * 2;
        if (gameMode_ == GameMode::Elimination) requested = 8 + std::min(wave_, 4);
        else if (gameMode_ == GameMode::HeadHunter) requested = 4 + wave_;
        else if (gameMode_ == GameMode::Mayhem) requested = 7 + wave_ * 3;
        const int count = std::min(requested, gameMode_ == GameMode::Mayhem ? botLimit + 4 : botLimit);
        const std::array<float, 3> healthScale = {{0.82f, 1.0f, 1.24f}};
        std::shuffle(spawnPoints_.begin(), spawnPoints_.end(), random_);
        for (int i = 0; i < count; ++i) {
            Bot bot;
            bot.position = spawnPoints_[static_cast<std::size_t>(i % spawnPoints_.size())];
            if (i >= static_cast<int>(spawnPoints_.size())) bot.position += randomUnitFlat() * randomRange(0.8f, 2.3f);
            bot.type = (wave_ >= 3 && i % 5 == 0) || gameMode_ == GameMode::HeadHunter ? 1 : 0;
            bot.role = i % 4;
            bot.model = (i + wave_) % static_cast<int>(operatorModels_.size());
            bot.maxHealth = ((bot.type == 1 ? 150.0f : 82.0f) + wave_ * 11.0f) *
                            healthScale[static_cast<std::size_t>(difficulty_)];
            if (gameMode_ == GameMode::HeadHunter) bot.maxHealth *= 1.18f;
            bot.health = bot.maxHealth;
            bot.fireTimer = randomRange(0.45f, 1.8f);
            bot.strafe = randomRange(0.0f, 1.0f) > 0.5f ? 1.0f : -1.0f;
            bot.lastSeenPlayer = playerPosition_;
            bot.tacticalGoal = playerPosition_;
            bots_.push_back(bot);
        }
        setAchievementProgress(3, wave_);
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

    std::vector<glm::vec3> findPath(const glm::vec3& start, const glm::vec3& goal) const {
        constexpr int gridSize = 29;
        constexpr float cellSize = 1.5f;
        constexpr float origin = -21.0f;
        constexpr int cellCount = gridSize * gridSize;
        const auto cell = [](const glm::vec3& value) {
            return std::pair{
                glm::clamp(static_cast<int>(std::round((value.x - origin) / cellSize)), 1, gridSize - 2),
                glm::clamp(static_cast<int>(std::round((value.z - origin) / cellSize)), 1, gridSize - 2)};
        };
        const auto world = [](int x, int z) {
            return glm::vec3(origin + static_cast<float>(x) * cellSize, 0.0f,
                             origin + static_cast<float>(z) * cellSize);
        };
        const auto index = [](int x, int z) { return z * gridSize + x; };
        const auto [startX, startZ] = cell(start);
        auto [goalX, goalZ] = cell(goal);
        if (pointBlocked(world(goalX, goalZ), 0.46f)) {
            bool found = false;
            for (int radius = 1; radius <= 5 && !found; ++radius) {
                for (int dz = -radius; dz <= radius && !found; ++dz) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int x = glm::clamp(goalX + dx, 1, gridSize - 2);
                        const int z = glm::clamp(goalZ + dz, 1, gridSize - 2);
                        if (!pointBlocked(world(x, z), 0.46f)) {
                            goalX = x; goalZ = z; found = true; break;
                        }
                    }
                }
            }
        }

        struct OpenCell {
            float score = 0.0f;
            int index = 0;
            bool operator<(const OpenCell& other) const { return score > other.score; }
        };
        std::priority_queue<OpenCell> open;
        std::array<float, cellCount> cost{};
        std::array<int, cellCount> parent{};
        std::array<bool, cellCount> closed{};
        cost.fill(std::numeric_limits<float>::max());
        parent.fill(-1);
        const int startIndex = index(startX, startZ);
        const int goalIndex = index(goalX, goalZ);
        cost[static_cast<std::size_t>(startIndex)] = 0.0f;
        open.push({0.0f, startIndex});
        static constexpr std::array<std::pair<int,int>, 8> directions = {{
            {-1,0}, {1,0}, {0,-1}, {0,1}, {-1,-1}, {-1,1}, {1,-1}, {1,1}
        }};
        while (!open.empty()) {
            const int current = open.top().index;
            open.pop();
            if (closed[static_cast<std::size_t>(current)]) continue;
            closed[static_cast<std::size_t>(current)] = true;
            if (current == goalIndex) break;
            const int currentX = current % gridSize;
            const int currentZ = current / gridSize;
            for (const auto& [dx, dz] : directions) {
                const int x = currentX + dx;
                const int z = currentZ + dz;
                if (x <= 0 || z <= 0 || x >= gridSize - 1 || z >= gridSize - 1) continue;
                const int next = index(x, z);
                if (closed[static_cast<std::size_t>(next)] || pointBlocked(world(x, z), 0.46f)) continue;
                const float step = dx != 0 && dz != 0 ? 1.4142f : 1.0f;
                const float nextCost = cost[static_cast<std::size_t>(current)] + step;
                if (nextCost >= cost[static_cast<std::size_t>(next)]) continue;
                cost[static_cast<std::size_t>(next)] = nextCost;
                parent[static_cast<std::size_t>(next)] = current;
                const float heuristic = std::hypot(static_cast<float>(goalX - x), static_cast<float>(goalZ - z));
                open.push({nextCost + heuristic, next});
            }
        }

        std::vector<glm::vec3> result;
        if (goalIndex != startIndex && parent[static_cast<std::size_t>(goalIndex)] < 0) return result;
        for (int current = goalIndex; current != startIndex && current >= 0;
             current = parent[static_cast<std::size_t>(current)]) {
            result.push_back(world(current % gridSize, current / gridSize));
        }
        std::reverse(result.begin(), result.end());
        return result;
    }

    glm::vec3 findCoverPoint(const Bot& bot) const {
        glm::vec3 best = bot.position;
        float bestDistance = std::numeric_limits<float>::max();
        for (const Block& block : blocks_) {
            if (!block.collision || block.size.y < 1.0f) continue;
            glm::vec3 away = block.center - playerPosition_;
            away.y = 0.0f;
            if (glm::length2(away) < 0.01f) continue;
            away = glm::normalize(away);
            const float clearance = std::max(block.size.x, block.size.z) * 0.5f + 0.85f;
            const glm::vec3 candidate = glm::vec3(block.center.x, 0.0f, block.center.z) + away * clearance;
            const float distance = glm::distance(candidate, bot.position);
            if (distance > 17.0f || distance >= bestDistance || pointBlocked(candidate, 0.48f)) continue;
            if (!lineOfSight(candidate + glm::vec3(0,1.25f,0), cameraPosition())) {
                best = candidate;
                bestDistance = distance;
            }
        }
        return best;
    }

    glm::vec3 chooseTacticalGoal(const Bot& bot, bool visible, float distance,
                                 const glm::vec3& flatDirection) const {
        if (bot.health < bot.maxHealth * 0.30f) {
            const glm::vec3 cover = findCoverPoint(bot);
            if (glm::distance2(cover, bot.position) > 0.5f) return cover;
        }
        if (!visible && bot.alertTimer > 0.0f) return bot.lastSeenPlayer;
        const glm::vec3 side{-flatDirection.z, 0.0f, flatDirection.x};
        switch (bot.role) {
            case 1: return playerPosition_ + side * bot.strafe * 8.5f - flatDirection * 2.5f;
            case 2: return distance < 12.0f ? bot.position - flatDirection * 6.0f
                                           : playerPosition_ - flatDirection * 13.5f + side * bot.strafe * 3.0f;
            case 3: return playerPosition_ - flatDirection * 2.2f;
            default: return distance > 8.0f ? playerPosition_ - flatDirection * 6.5f
                                            : bot.position + side * bot.strafe * 3.0f;
        }
    }

    void updateBots(float deltaTime) {
        const glm::vec3 playerTarget = cameraPosition() - glm::vec3(0, 0.35f, 0);
        for (Bot& bot : bots_) {
            bot.fireTimer -= deltaTime;
            bot.decisionTimer -= deltaTime;
            bot.hitFlash = std::max(0.0f, bot.hitFlash - deltaTime * 5.0f);
            bot.muzzleFlash = std::max(0.0f, bot.muzzleFlash - deltaTime * 9.0f);
            bot.alertTimer = std::max(0.0f, bot.alertTimer - deltaTime);
            bot.animationTime += deltaTime * (glm::length2(bot.velocity) > 1.0f ? 1.35f : 1.0f);
            const glm::vec3 from = bot.position + glm::vec3(0, 1.32f, 0);
            glm::vec3 toPlayer = playerTarget - from;
            const float distance = glm::length(toPlayer);
            const glm::vec3 direction = distance > 0.01f ? toPlayer / distance : glm::vec3(0, 0, 1);
            const bool visible = lineOfSight(from, playerTarget);
            if (visible) {
                bot.lastSeenPlayer = playerPosition_;
                bot.alertTimer = 5.0f;
                bot.aimStability = std::min(1.0f, bot.aimStability + deltaTime * (0.7f + difficulty_ * 0.18f));
            } else {
                bot.aimStability = std::max(0.0f, bot.aimStability - deltaTime * 0.85f);
            }

            glm::vec3 flatDirection = glm::normalize(glm::vec3(direction.x, 0.0f, direction.z));
            if (!std::isfinite(flatDirection.x)) flatDirection = {0, 0, 1};
            const glm::vec3 side{-flatDirection.z, 0.0f, flatDirection.x};

            if (bot.decisionTimer <= 0.0f) {
                bot.decisionTimer = randomRange(0.42f, 0.95f) - difficulty_ * 0.06f;
                if (randomRange(0, 1) > 0.66f) bot.strafe *= -1.0f;
                bot.tacticalGoal = chooseTacticalGoal(bot, visible, distance, flatDirection);
                bot.path = findPath(bot.position, bot.tacticalGoal);
                bot.pathCursor = 0;
            }

            glm::vec3 desired(0.0f);
            while (bot.pathCursor < bot.path.size() &&
                   glm::distance2(bot.position, bot.path[bot.pathCursor]) < 0.75f * 0.75f) ++bot.pathCursor;
            if (bot.pathCursor < bot.path.size()) {
                glm::vec3 pathDirection = bot.path[bot.pathCursor] - bot.position;
                pathDirection.y = 0.0f;
                if (glm::length2(pathDirection) > 0.01f) desired += glm::normalize(pathDirection);
            } else if (bot.alertTimer > 0.0f || visible) {
                glm::vec3 goalDirection = bot.tacticalGoal - bot.position;
                goalDirection.y = 0.0f;
                if (glm::length2(goalDirection) > 1.0f) desired += glm::normalize(goalDirection);
            }
            if (visible && distance < 18.0f) desired += side * bot.strafe * (bot.role == 1 ? 1.05f : 0.48f);
            if (visible && distance < (bot.role == 3 ? 2.5f : 4.0f)) desired -= flatDirection;
            if (glm::length2(desired) > 0.01f) desired = glm::normalize(desired);
            const float roleSpeed = bot.role == 1 ? 0.45f : (bot.role == 3 ? 0.70f : 0.0f);
            const float speed = (bot.type == 1 ? 2.35f : 3.15f) + roleSpeed +
                                std::min(wave_ * 0.08f, 0.8f) + difficulty_ * 0.22f;
            glm::vec3 moved = moveWithCollision(bot.position, desired * speed * deltaTime, bot.type == 1 ? 0.56f : 0.43f);
            if (glm::length2(moved - bot.position) < 0.00001f) {
                moved = moveWithCollision(bot.position, side * bot.strafe * speed * deltaTime, 0.43f);
                bot.strafe *= -1.0f;
            }
            bot.velocity = (moved - bot.position) / std::max(deltaTime, 0.001f);
            bot.position = moved;
            bot.yaw = std::atan2(direction.x, direction.z);

            const float reactionThreshold = 0.58f - difficulty_ * 0.12f + (bot.role == 2 ? -0.08f : 0.0f);
            if (visible && distance < 30.0f && bot.fireTimer <= 0.0f && bot.aimStability >= reactionThreshold) {
                if (bot.burstShots <= 0) bot.burstShots = bot.role == 2 ? 1 : static_cast<int>(randomRange(2.0f, 5.8f));
                --bot.burstShots;
                const float fireRate = bot.type == 1 ? 0.52f : 0.78f;
                bot.fireTimer = bot.burstShots > 0 ? randomRange(0.10f, 0.18f)
                    : std::max(0.26f, fireRate - wave_ * 0.016f) + randomRange(0.08f, 0.30f);
                bot.muzzleFlash = 1.0f;
                audio_.play(SoundEffect::Enemy);
                const float accuracy = glm::clamp(0.25f + bot.aimStability * 0.36f + difficulty_ * 0.09f +
                                                  wave_ * 0.012f + (bot.role == 2 ? 0.12f : 0.0f) -
                                                  distance * 0.007f, 0.14f, 0.90f);
                const bool hit = randomRange(0, 1) < accuracy;
                const float leadTime = glm::clamp(distance / 90.0f, 0.04f, 0.22f);
                glm::vec3 end = playerTarget + playerVelocity_ * leadTime;
                if (!hit) end += randomUnit() * randomRange(0.8f, 2.5f);
                tracers_.push_back({from + direction * 0.35f, end, {1.0f, 0.08f, 0.22f, 0.8f}, 0.09f});
                if (hit) {
                    const float roleDamage = bot.role == 2 ? 3.0f : (bot.role == 3 ? -1.0f : 0.0f);
                    const float damage = ((bot.type == 1 ? 12.0f : 7.0f) + roleDamage + wave_ * 0.35f) *
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
        for (const Block& block : blocks_) {
            if (block.visible) renderer_.draw(cube_, modelMatrix(block.center, block.size), block.color);
        }
        for (const ModelPlacement& placement : modelPlacements_) {
            if (placement.model < 0 || placement.model >= static_cast<int>(environmentModels_.size())) continue;
            const glm::mat4 transform = glm::translate(glm::mat4(1.0f), placement.position) *
                glm::rotate(glm::mat4(1.0f), placement.yaw, glm::vec3(0,1,0)) *
                glm::scale(glm::mat4(1.0f), placement.scale);
            environmentModels_[static_cast<std::size_t>(placement.model)].draw(renderer_, transform, placement.tint);
        }

        const glm::vec4 lineColor{0.01f, 0.24f, 0.34f, 1.0f};
        const int gridStep = quality_ == 0 ? 4 : 2;
        if (quality_ > 0) {
            const glm::vec4 tileA{0.055f,0.07f,0.095f,1.0f};
            const glm::vec4 tileB{0.075f,0.09f,0.115f,1.0f};
            for (int x = -20; x <= 20; x += 4) {
                for (int z = -20; z <= 20; z += 4) {
                    const bool alternate = ((x / 4 + z / 4) & 1) != 0;
                    renderer_.draw(cube_, modelMatrix({static_cast<float>(x),0.006f,static_cast<float>(z)},
                                                       {3.94f,0.01f,3.94f}), alternate ? tileA : tileB);
                }
            }
        }
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
        if (gameMode_ == GameMode::BombDefusal || network_.connected()) {
            renderer_.draw(cylinder_, modelMatrix({12.0f,0.035f,12.0f}, {2.5f,0.035f,2.5f}),
                           {1.0f,0.18f,0.08f,0.72f}, 0, 1.2f);
            renderer_.draw(cylinder_, modelMatrix({-12.0f,0.035f,-12.0f}, {2.5f,0.035f,2.5f}),
                           {1.0f,0.62f,0.08f,0.72f}, 0, 1.2f);
            if (network_.snapshot().phase == irx::RoundPhase::BombPlanted) {
                renderer_.draw(cube_, modelMatrix({12.0f,0.22f,12.0f}, {0.52f,0.30f,0.34f}, time_ * 0.8f),
                               {0.08f,0.09f,0.11f,1.0f});
                renderer_.draw(cube_, modelMatrix({12.0f,0.39f,12.0f}, {0.22f,0.08f,0.16f}, time_ * 0.8f),
                               {1.0f,0.05f,0.03f,1.0f}, 0, 4.0f);
            }
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

    void renderOperator(const glm::vec3& position, float yaw, int modelIndex, bool moving, int weaponIndex = -1,
                        const glm::vec4& tint = glm::vec4(1.0f)) {
        if (modelIndex < 0 || modelIndex >= static_cast<int>(operatorModels_.size())) return;
        GltfModel& model = operatorModels_[static_cast<std::size_t>(modelIndex)];
        if (!model.valid()) return;
        const int animation = model.findAnimation(moving ? "sprint" : "idle");
        const glm::mat4 root = glm::translate(glm::mat4(1.0f), position) *
            glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0,1,0)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(0.245f));
        model.drawAnimated(renderer_, root, animation, time_, tint);

        const int equippedWeapon = weaponIndex >= 0 ? weaponIndex : selectedWeapon_;
        if (equippedWeapon >= 0 && equippedWeapon < static_cast<int>(gunModels_.size()) &&
            gunModels_[static_cast<std::size_t>(equippedWeapon)].valid()) {
            const glm::mat4 gun = glm::translate(glm::mat4(1.0f), position) *
                glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0,1,0)) *
                glm::translate(glm::mat4(1.0f), {0.28f, 1.25f, 0.42f}) *
                glm::rotate(glm::mat4(1.0f), -kPi * 0.5f, glm::vec3(0,1,0)) *
                glm::scale(glm::mat4(1.0f), glm::vec3(0.20f));
            gunModels_[static_cast<std::size_t>(equippedWeapon)].draw(renderer_, gun);
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
            const std::size_t modelIndex = static_cast<std::size_t>(bot.model % static_cast<int>(operatorModels_.size()));
            if (operatorModels_[modelIndex].valid()) {
                const bool running = glm::length2(bot.velocity) > 1.0f;
                const int animation = operatorModels_[modelIndex].findAnimation(
                    bot.muzzleFlash > 0.0f ? "holding-both-shoot" : (running ? "sprint" : "holding-both"));
                const float bodyScale = bot.type == 1 ? 0.235f : 0.205f;
                const glm::mat4 animatedRoot = bodyRoot * glm::scale(glm::mat4(1.0f), glm::vec3(bodyScale));
                operatorModels_[modelIndex].drawAnimated(renderer_, animatedRoot, animation, bot.animationTime, suit);

                const int botWeapon = bot.role == 2 ? 5 : (bot.role == 1 ? 3 : 1);
                if (gunModels_[static_cast<std::size_t>(botWeapon)].valid()) {
                    const glm::mat4 gun = bodyRoot * glm::translate(glm::mat4(1.0f), {0.25f,1.25f,0.42f}) *
                        glm::rotate(glm::mat4(1.0f), -kPi * 0.5f, glm::vec3(0,1,0)) *
                        glm::scale(glm::mat4(1.0f), glm::vec3(0.20f));
                    gunModels_[static_cast<std::size_t>(botWeapon)].draw(renderer_, gun);
                }
                if (bot.muzzleFlash > 0) {
                    renderer_.draw(cube_, bodyRoot * glm::translate(glm::mat4(1), {0.25f,1.25f,1.05f}) *
                                   glm::scale(glm::mat4(1), glm::vec3(0.10f + bot.muzzleFlash * 0.11f)),
                                   {1.0f,0.08f,0.15f,1}, 0, 3.0f);
                }
                renderer_.draw(cube_, modelMatrix(bot.position + glm::vec3(0, 2.05f, 0), {0.82f, 0.055f, 0.055f}),
                               {0.04f,0.04f,0.055f,1});
                renderer_.draw(cube_, modelMatrix(bot.position + glm::vec3(-0.41f + 0.41f * healthRatio, 2.052f, 0.002f),
                                                   {0.82f * healthRatio, 0.06f, 0.06f}),
                               healthRatio > 0.45f ? glm::vec4(0.1f,0.95f,0.45f,1) : glm::vec4(1,0.12f,0.16f,1), 0, 0.8f);
                continue;
            }
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

    void renderNetworkPlayers() {
        if (!network_.connected()) return;
        for (const irx::RemotePlayer& player : network_.snapshot().players) {
            if (!player.alive) continue;
            const glm::vec4 teamColor = player.team == irx::Team::Terrorist
                ? glm::vec4(1.0f,0.28f,0.08f,1.0f) : glm::vec4(0.08f,0.55f,1.0f,1.0f);
            renderOperator(player.position, player.yaw, static_cast<int>(player.id % operatorModels_.size()),
                           (player.flags & 64u) != 0, static_cast<int>(player.weapon), glm::mix(glm::vec4(1.0f), teamColor, 0.16f));
            renderer_.draw(cylinder_, modelMatrix(player.position + glm::vec3(0,0.025f,0), {0.68f,0.025f,0.68f}),
                           teamColor, 0, 1.5f);
            const float health = saturate(static_cast<float>(player.health) / 100.0f);
            renderer_.draw(cube_, modelMatrix(player.position + glm::vec3(0,2.22f,0), {0.92f,0.055f,0.055f}),
                           {0.025f,0.03f,0.04f,1.0f});
            renderer_.draw(cube_, modelMatrix(player.position + glm::vec3(-0.46f + 0.46f * health,2.225f,0.003f),
                                               {0.92f * health,0.06f,0.06f}), teamColor, 0, 0.8f);
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

        if (selectedWeapon_ >= 0 && selectedWeapon_ < static_cast<int>(gunModels_.size()) &&
            gunModels_[static_cast<std::size_t>(selectedWeapon_)].valid()) {
            glm::mat4 weaponTransform = root;
            float muzzleLength = 1.15f;
            if (selectedWeapon_ >= 6 && selectedWeapon_ <= 8) {
                weaponTransform *= glm::translate(glm::mat4(1.0f), {0.0f,-0.02f,0.48f}) *
                    glm::scale(glm::mat4(1.0f), glm::vec3(0.92f));
                muzzleLength = selectedWeapon_ == 7 ? 1.42f : 1.12f;
            } else {
                weaponTransform *= glm::translate(glm::mat4(1.0f), {0.0f,-0.02f,0.18f}) *
                    glm::rotate(glm::mat4(1.0f), -kPi * 0.5f, glm::vec3(0,1,0)) *
                    glm::scale(glm::mat4(1.0f), glm::vec3(0.22f));
                muzzleLength = selectedWeapon_ == 0 ? 0.82f : (selectedWeapon_ == 5 ? 1.48f : 1.18f);
            }
            gunModels_[static_cast<std::size_t>(selectedWeapon_)].draw(renderer_, weaponTransform);
            const Rarity rarity = weaponDefinitions_[static_cast<std::size_t>(selectedWeapon_)].rarity;
            if (rarity == Rarity::Legendary || rarity == Rarity::Mythic) {
                renderer_.draw(cube_, root * glm::translate(glm::mat4(1.0f), {0.0f,0.10f,0.48f}) *
                               glm::scale(glm::mat4(1.0f), {0.025f,0.025f,0.55f}),
                               rarityColor(rarity), 0, rarity == Rarity::Mythic ? 2.8f : 1.3f);
            }
            if (muzzleFlash_ > 0.0f) {
                const glm::vec4 flash = rarity == Rarity::Mythic ? rarityColor(rarity)
                                                                 : glm::vec4(1.0f,0.42f,0.04f,1.0f);
                renderer_.draw(cube_, root * glm::translate(glm::mat4(1.0f), {0,0,muzzleLength}) *
                               glm::scale(glm::mat4(1.0f), glm::vec3(0.11f + muzzleFlash_ * 0.12f)),
                               flash, 0, rarity == Rarity::Mythic ? 6.0f : 4.0f);
            }
            return;
        }

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

    void initializeAchievements() {
        achievements_ = {{
            {"first_blood", "FIRST BLOOD", "Eliminate one hostile", 1},
            {"headhunter", "HEAD HUNTER", "Land 10 lethal headshots", 10},
            {"terminator", "TERMINATOR", "Eliminate 25 hostiles", 25},
            {"wavebreaker", "WAVEBREAKER", "Reach wave five", 5},
            {"arsenal", "FULL ARSENAL", "Fire every weapon", 10},
            {"mythic", "MYTHIC EXECUTIONER", "Get 10 Mythic eliminations", 10},
            {"world_tour", "WORLD TOUR", "Deploy to every map", 6},
            {"high_score", "NEON LEGEND", "Score 5000 in one run", 5000}
        }};
        char* preferenceDirectory = SDL_GetPrefPath("IrAutoX", "iRx");
        if (preferenceDirectory != nullptr) {
            progressPath_ = std::filesystem::path(preferenceDirectory) / "achievements.txt";
            SDL_free(preferenceDirectory);
        } else {
            progressPath_ = "achievements.txt";
        }
        std::ifstream input(progressPath_);
        std::string id;
        int progress = 0;
        int unlocked = 0;
        while (input >> id >> progress >> unlocked) {
            for (Achievement& achievement : achievements_) {
                if (id == achievement.id) {
                    achievement.progress = glm::clamp(progress, 0, achievement.target);
                    achievement.unlocked = unlocked != 0 || achievement.progress >= achievement.target;
                }
            }
        }
    }

    void saveAchievements() const {
        if (progressPath_.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(progressPath_.parent_path(), error);
        std::ofstream output(progressPath_, std::ios::trunc);
        for (const Achievement& achievement : achievements_)
            output << achievement.id << ' ' << achievement.progress << ' ' << (achievement.unlocked ? 1 : 0) << '\n';
    }

    void setAchievementProgress(std::size_t index, int progress) {
        if (index >= achievements_.size()) return;
        Achievement& achievement = achievements_[index];
        achievement.progress = glm::clamp(std::max(achievement.progress, progress), 0, achievement.target);
        if (!achievement.unlocked && achievement.progress >= achievement.target) {
            achievement.unlocked = true;
            achievementPopup_ = achievement.title;
            achievementPopupTimer_ = 4.5f;
            audio_.play(SoundEffect::Start);
            saveAchievements();
        }
    }

    void incrementAchievement(std::size_t index, int amount = 1) {
        if (index >= achievements_.size()) return;
        setAchievementProgress(index, achievements_[index].progress + amount);
    }

    void renderAchievementPopup(float width) {
        if (achievementPopupTimer_ <= 0.0f || achievementPopup_.empty()) return;
        const float alpha = saturate(achievementPopupTimer_ * 2.0f);
        renderer_.rect(width * 0.5f - 230.0f, 28.0f, 460.0f, 66.0f, {0.03f,0.02f,0.055f,0.92f * alpha});
        renderer_.rect(width * 0.5f - 230.0f, 28.0f, 5.0f, 66.0f, rarityColor(Rarity::Legendary));
        renderer_.text(width * 0.5f, 39.0f, "ACHIEVEMENT UNLOCKED", 1.65f,
                       rarityColor(Rarity::Legendary), true);
        renderer_.text(width * 0.5f, 66.0f, achievementPopup_, 2.35f, {0.95f,0.97f,1.0f,alpha}, true);
    }

    void renderHUD() {
        const float width = static_cast<float>(renderer_.width());
        const float height = static_cast<float>(renderer_.height());
        const glm::vec4 white{0.88f,0.94f,1.0f,1};
        const glm::vec4 cyan{0.12f,0.88f,1.0f,1};
        const glm::vec4 red{1.0f,0.12f,0.26f,1};
        const glm::vec4 panel{0.008f,0.014f,0.035f,0.78f};
        static constexpr std::array<const char*, 6> mapNames = {{
            "NEON YARD", "DUST DEPOT", "ICE LAB", "SUBURBAN SIEGE", "ORBITAL STATION", "IRON FOUNDRY"
        }};
        static constexpr std::array<const char*, 3> difficultyNames = {{"ROOKIE", "VETERAN", "NIGHTMARE"}};
        static constexpr std::array<const char*, 3> qualityNames = {{"POTATO", "BALANCED", "ULTRA"}};
        static constexpr std::array<const char*, 4> operatorNames = {{"WRAITH", "VIPER", "NOMAD", "SPECTRE"}};

        if (mode_ == Mode::Protection) {
            const float progress = saturate(protectionTimer_ / 2.35f);
            renderer_.rect(0, 0, width, height, {0.002f,0.004f,0.012f,0.92f});
            renderer_.text(width * 0.5f, height * 0.39f, "IRX", 12.0f, cyan, true);
            renderer_.text(width * 0.5f, height * 0.54f, "PROTECTED BY IRAUTOX - AC", 3.0f, white, true);
            renderer_.rect(width * 0.5f - 220.0f, height * 0.62f, 440.0f, 8.0f, {0.04f,0.06f,0.09f,1.0f});
            renderer_.rect(width * 0.5f - 220.0f, height * 0.62f, 440.0f * progress, 8.0f, cyan);
            renderer_.text(width * 0.5f, height * 0.67f, network_.status(), 1.7f,
                           network_.connected() ? glm::vec4(0.25f,1.0f,0.52f,1.0f) : cyan, true);
            return;
        }

        if (mode_ == Mode::Title) {
            renderer_.rect(0, 0, width, height, {0.005f,0.008f,0.025f,0.56f});
            renderer_.text(width * 0.5f, height * 0.19f, "IRX", 11.0f, cyan, true);
            renderer_.text(width * 0.5f, height * 0.19f + 92.0f, "TACTICAL STRIKE", 3.4f, white, true);
            renderer_.text(width * 0.5f, height * 0.19f + 128.0f,
                           std::string("IRAUTOX.IR:9832  /  ") + network_.status(), 1.55f,
                           network_.connected() ? glm::vec4(0.25f,1.0f,0.52f,1.0f) : glm::vec4(1.0f,0.62f,0.12f,1.0f), true);
            renderer_.rect(width * 0.5f - 200, height * 0.72f - 18, 400, 50, {0.02f,0.18f,0.25f,0.88f});
            renderer_.text(width * 0.5f, height * 0.72f,
                           network_.connected() ? "ENTER  PLAY ONLINE" : "ENTER  PRACTICE OFFLINE", 2.8f, white, true);
            renderer_.rect(width * 0.5f - 330, height * 0.405f, 660, 174, panel);
            renderer_.text(width * 0.5f, height * 0.42f,
                           std::string("M MAP: ") + mapNames[static_cast<std::size_t>(selectedMap_)], 2.2f, white, true);
            renderer_.text(width * 0.5f, height * 0.42f + 28,
                           std::string("G MODE: ") + gameModeName(gameMode_), 2.2f,
                           gameMode_ == GameMode::Mayhem ? rarityColor(Rarity::Mythic) : cyan, true);
            renderer_.text(width * 0.5f, height * 0.42f + 56,
                           std::string("D DIFFICULTY: ") + difficultyNames[static_cast<std::size_t>(difficulty_)],
                           2.2f, difficulty_ == 2 ? red : cyan, true);
            renderer_.text(width * 0.5f, height * 0.42f + 84,
                           std::string("Q GRAPHICS: ") + qualityNames[static_cast<std::size_t>(quality_)],
                           2.2f, quality_ == 0 ? glm::vec4(0.35f,1.0f,0.45f,1) : white, true);
            renderer_.text(width * 0.5f, height * 0.42f + 112,
                           std::string("C OPERATOR: ") + operatorNames[static_cast<std::size_t>(selectedOperator_)],
                           2.2f, white, true);
            const WeaponDefinition& loadout = weaponDefinitions_[static_cast<std::size_t>(selectedWeapon_)];
            renderer_.text(width * 0.5f, height * 0.42f + 140,
                           std::string("1-0 LOADOUT: ") + loadout.name + "  [" + rarityName(loadout.rarity) + "]",
                           2.1f, rarityColor(loadout.rarity), true);
            renderer_.text(width * 0.5f, height - 70, "WASD MOVE  RMB AIM  B BUY  TAB SCOREBOARD  F1 T  F2 CT  F11 FULLSCREEN", 1.55f,
                           {0.55f,0.68f,0.78f,1}, true);
            renderer_.text(width - 12, height - 20, std::string("V") + NEON_VERSION, 1.4f,
                           {0.35f,0.45f,0.55f,1}, true);
            renderAchievementPopup(width);
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
        renderer_.text(width - 52, height - 100, rarityName(definition.rarity), 1.3f,
                       rarityColor(definition.rarity), true);
        renderer_.text(width - 332, height - 66,
                       std::to_string(weapon.loaded) + "/" + std::to_string(weapon.reserve), 4.2f, white);
        if (reloadTimer_ > 0.0f) {
            const float progress = 1.0f - reloadTimer_ / definition.reload;
            renderer_.rect(width - 332, height - 40, 280, 5, {0.08f,0.08f,0.11f,1});
            renderer_.rect(width - 332, height - 40, 280 * saturate(progress), 5, cyan);
            renderer_.text(width - 68, height - 64, "R", 2.2f, cyan, true);
        }

        renderer_.rect(24, 24, network_.connected() ? 390.0f : 270.0f, network_.connected() ? 104.0f : 78.0f, panel);
        if (network_.connected()) {
            const irx::MatchSnapshot& match = network_.snapshot();
            const char* teamName = network_.team() == irx::Team::Terrorist ? "TERRORIST" :
                                   (network_.team() == irx::Team::CounterTerrorist ? "COUNTER-TERRORIST" : "SPECTATOR");
            renderer_.text(42, 38, std::string("IRX ONLINE  ") + teamName, 1.75f,
                           network_.team() == irx::Team::Terrorist ? glm::vec4(1.0f,0.28f,0.08f,1.0f) : cyan);
            renderer_.text(42, 66, "T " + std::to_string(match.terroristScore) + "   CT " +
                           std::to_string(match.counterTerroristScore) + "   " +
                           std::to_string(std::max(0, static_cast<int>(std::ceil(match.roundRemaining)))) + " SEC", 2.0f, white);
            renderer_.text(42, 96, match.phase == irx::RoundPhase::BombPlanted
                           ? "BOMB " + std::to_string(std::max(0, static_cast<int>(std::ceil(match.bombRemaining)))) + " SEC"
                           : "E PLANT/DEFUSE   G GRENADE", 1.55f,
                           match.phase == irx::RoundPhase::BombPlanted ? red : glm::vec4(0.55f,0.72f,0.82f,1.0f));
            renderer_.text(width - 34, 36, "PING " + std::to_string(network_.pingMilliseconds()) + " MS", 1.7f, white, true);
        } else {
            renderer_.text(42, 40, "WAVE " + std::to_string(wave_), 2.6f, cyan);
            renderer_.text(42, 72, "HOSTILES " + std::to_string(bots_.size()), 2.0f, white);
            renderer_.text(width - 34, 36, "SCORE " + std::to_string(score_), 2.4f, white, true);
        }

        if (!network_.connected() && bots_.empty() && mode_ == Mode::Playing) {
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

        if (scoreboardOpen_ && network_.connected()) {
            renderer_.rect(0, 0, width, height, {0.002f,0.004f,0.012f,0.82f});
            renderer_.rect(width * 0.5f - 430.0f, 74.0f, 860.0f, height - 148.0f, {0.012f,0.020f,0.045f,0.97f});
            renderer_.text(width * 0.5f, 98.0f, "IRX MATCH SCOREBOARD", 3.8f, cyan, true);
            renderer_.text(width * 0.5f, 142.0f, "PLAYER        TEAM        HP        WEAPON", 1.65f,
                           {0.55f,0.68f,0.78f,1.0f}, true);
            float rowY = 184.0f;
            renderer_.text(width * 0.5f - 380.0f, rowY, "YOU #" + std::to_string(network_.selfId()), 1.8f, white);
            renderer_.text(width * 0.5f - 120.0f, rowY,
                           network_.team() == irx::Team::Terrorist ? "T" : "CT", 1.8f,
                           network_.team() == irx::Team::Terrorist ? glm::vec4(1.0f,0.28f,0.08f,1.0f) : cyan);
            renderer_.text(width * 0.5f + 40.0f, rowY, std::to_string(static_cast<int>(playerHealth_)), 1.8f, white);
            renderer_.text(width * 0.5f + 190.0f, rowY, weaponDefinitions_[static_cast<std::size_t>(selectedWeapon_)].name, 1.5f, white);
            rowY += 38.0f;
            for (const irx::RemotePlayer& player : network_.snapshot().players) {
                if (rowY > height - 105.0f) break;
                const glm::vec4 teamColor = player.team == irx::Team::Terrorist
                    ? glm::vec4(1.0f,0.28f,0.08f,1.0f) : cyan;
                renderer_.text(width * 0.5f - 380.0f, rowY, "PLAYER #" + std::to_string(player.id), 1.8f,
                               player.alive ? white : glm::vec4(0.42f,0.46f,0.52f,1.0f));
                renderer_.text(width * 0.5f - 120.0f, rowY,
                               player.team == irx::Team::Terrorist ? "T" : "CT", 1.8f, teamColor);
                renderer_.text(width * 0.5f + 40.0f, rowY, std::to_string(player.health), 1.8f, white);
                const std::size_t weaponIndex = std::min<std::size_t>(player.weapon, weaponDefinitions_.size() - 1);
                renderer_.text(width * 0.5f + 190.0f, rowY, weaponDefinitions_[weaponIndex].name, 1.5f, white);
                rowY += 38.0f;
            }
            renderer_.text(width * 0.5f, height - 102.0f, "F1 JOIN TERRORIST   F2 JOIN COUNTER-TERRORIST", 1.6f, white, true);
            renderAchievementPopup(width);
            return;
        }

        if (inventoryOpen_) {
            renderer_.rect(0, 0, width, height, {0.002f,0.004f,0.012f,0.90f});
            renderer_.rect(62, 42, width - 124, height - 84, {0.012f,0.020f,0.045f,0.97f});
            renderer_.text(width * 0.5f, 66, "ARSENAL / ACHIEVEMENTS", 4.0f, cyan, true);
            renderer_.text(width * 0.5f, 108, "PRESS 1-0 TO EQUIP  -  B TO CLOSE", 1.6f,
                           {0.56f,0.68f,0.78f,1}, true);
            const float cardWidth = (width - 190.0f) * 0.5f;
            for (int i = 0; i < static_cast<int>(weaponDefinitions_.size()); ++i) {
                const int column = i / 5;
                const int row = i % 5;
                const float x = 82.0f + column * (cardWidth + 26.0f);
                const float y = 142.0f + row * 64.0f;
                const WeaponDefinition& item = weaponDefinitions_[static_cast<std::size_t>(i)];
                const glm::vec4 rarity = rarityColor(item.rarity);
                renderer_.rect(x, y, cardWidth, 52, i == selectedWeapon_
                    ? glm::vec4(rarity.r * 0.18f, rarity.g * 0.18f, rarity.b * 0.18f, 0.96f)
                    : glm::vec4(0.025f,0.035f,0.065f,0.96f));
                renderer_.rect(x, y, 5, 52, rarity);
                const std::string key = i == 9 ? "0" : std::to_string(i + 1);
                renderer_.text(x + 18, y + 9, key + "  " + item.name, 1.75f, white);
                renderer_.text(x + cardWidth - 18, y + 10, rarityName(item.rarity), 1.25f, rarity, true);
                renderer_.text(x + 18, y + 32, "DMG " + std::to_string(item.damage) + "  MAG " +
                               std::to_string(item.magazine), 1.15f, {0.55f,0.68f,0.78f,1});
            }
            int unlockedCount = 0;
            for (const Achievement& achievement : achievements_) if (achievement.unlocked) ++unlockedCount;
            renderer_.text(88, 480, "ACHIEVEMENTS " + std::to_string(unlockedCount) + "/" +
                           std::to_string(achievements_.size()), 2.2f, rarityColor(Rarity::Legendary));
            for (std::size_t i = 0; i < achievements_.size(); ++i) {
                const Achievement& achievement = achievements_[i];
                const int column = static_cast<int>(i / 4);
                const int row = static_cast<int>(i % 4);
                const float x = 88.0f + column * ((width - 190.0f) * 0.5f + 26.0f);
                const float y = 518.0f + row * 49.0f;
                renderer_.text(x, y, std::string(achievement.unlocked ? "[UNLOCKED] " : "[LOCKED] ") +
                               achievement.title, 1.35f,
                               achievement.unlocked ? rarityColor(Rarity::Legendary) : glm::vec4(0.45f,0.5f,0.58f,1));
                renderer_.text(x, y + 20, std::to_string(achievement.progress) + "/" +
                               std::to_string(achievement.target) + "  " + achievement.description,
                               1.0f, {0.52f,0.62f,0.70f,1});
            }
            renderAchievementPopup(width);
            return;
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
        renderAchievementPopup(width);
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
    std::array<GltfModel, 10> gunModels_{};
    std::array<GltfModel, 4> operatorModels_{};
    std::array<GltfModel, 16> environmentModels_{};
    std::array<WeaponDefinition, 10> weaponDefinitions_{};
    std::array<WeaponState, 10> weapons_{};
    std::array<Achievement, 8> achievements_{};
    std::vector<Block> blocks_;
    std::vector<ModelPlacement> modelPlacements_;
    std::vector<glm::vec3> spawnPoints_;
    std::vector<Bot> bots_;
    std::vector<Pickup> pickups_;
    std::vector<Particle> particles_;
    std::vector<Tracer> tracers_;
    irx::NetClient network_;
    irx::DiscordRpc discord_;
    std::mt19937 random_{0x4e454f4eu};
    Mode mode_ = Mode::Title;
    glm::vec3 playerPosition_{0,0,8};
    glm::vec3 playerVelocity_{};
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
    float achievementPopupTimer_ = 0.0f;
    float protectionTimer_ = 0.0f;
    float grenadeCooldown_ = 0.0f;
    float discordTimer_ = 0.0f;
    std::string achievementPopup_;
    std::filesystem::path progressPath_;
    std::uint32_t weaponUsageMask_ = 0;
    std::uint32_t mapUsageMask_ = 0;
    int selectedWeapon_ = 1;
    int reloadWeapon_ = -1;
    int score_ = 0;
    int kills_ = 0;
    int headshots_ = 0;
    int mythicKills_ = 0;
    int wave_ = 0;
    int selectedMap_ = 0;
    int selectedOperator_ = 0;
    int difficulty_ = 1;
    int quality_ = 1;
    int lastFootstepBucket_ = -1;
    GameMode gameMode_ = GameMode::BombDefusal;
    bool inventoryOpen_ = false;
    bool scoreboardOpen_ = false;
    bool aiming_ = false;
    bool thirdPerson_ = false;
    bool crouching_ = false;
    bool moving_ = false;
    bool onGround_ = true;
    bool jumpWasDown_ = false;
    bool quitRequested_ = false;
    bool fullscreenToggleRequested_ = false;
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
    if (failures == 0) std::cout << "iRx self-tests passed.\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace neon

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--self-test") return neon::runSelfTests();

    SDL_SetAppMetadata("iRx", NEON_VERSION, "ir.irautox.irx");
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

    SDL_Window* window = SDL_CreateWindow("iRx - Tactical Strike", 1600, 900,
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
        bool fullscreen = false;
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
            if (game.takeFullscreenToggle()) {
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(window, fullscreen);
            }

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
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "iRx", exception.what(), window);
        result = EXIT_FAILURE;
    }

    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
