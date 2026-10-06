#include <GL/glew.h>
#include "model_preview.h"
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

// stb_image's implementation lives in openGL.cpp, so tinygltf gets its own image loader below instead of compiling a second copy
#include "stb_image.h"
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "tiny_gltf.h"

namespace
{
// the model is rendered at twice the size it's shown at, which smooths the edges
constexpr int render_size = 512;
constexpr float display_size = 256.0f;
constexpr size_t max_cached_models = 16;

// ---------------------------------------------------------------- math
// column-major, like OpenGL and glTF
struct Mat4 { float m[16]; };

Mat4 identity()
{
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}
Mat4 mul(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            float v = 0;
            for (int k = 0; k < 4; ++k) v += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = v;
        }
    return r;
}
Mat4 translation(float x, float y, float z)
{
    Mat4 r = identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}
Mat4 rotation_x(float a)
{
    Mat4 r = identity();
    const float c = std::cos(a), s = std::sin(a);
    r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
    return r;
}
Mat4 rotation_y(float a)
{
    Mat4 r = identity();
    const float c = std::cos(a), s = std::sin(a);
    r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
    return r;
}
Mat4 perspective(float fov_y, float aspect, float near_plane, float far_plane)
{
    Mat4 r{};
    const float f = 1.0f / std::tan(fov_y * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (far_plane + near_plane) / (near_plane - far_plane);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * far_plane * near_plane / (near_plane - far_plane);
    return r;
}
Mat4 node_matrix(const tinygltf::Node& node)
{
    Mat4 r = identity();
    if (node.matrix.size() == 16)
    {
        for (int i = 0; i < 16; ++i) r.m[i] = static_cast<float>(node.matrix[i]);
        return r;
    }
    double t[3] = { 0, 0, 0 }, q[4] = { 0, 0, 0, 1 }, s[3] = { 1, 1, 1 };
    if (node.translation.size() == 3) std::copy_n(node.translation.begin(), 3, t);
    if (node.rotation.size() == 4) std::copy_n(node.rotation.begin(), 4, q);
    if (node.scale.size() == 3) std::copy_n(node.scale.begin(), 3, s);
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double rot[3][3] = {
        { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w) },
        { 2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
        { 2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y) } };
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row)
            r.m[c * 4 + row] = static_cast<float>(rot[row][c] * s[c]);
    r.m[12] = static_cast<float>(t[0]); r.m[13] = static_cast<float>(t[1]); r.m[14] = static_cast<float>(t[2]);
    return r;
}

// ---------------------------------------------------------------- loading (runs on a worker thread)
constexpr int vertex_floats = 8;    // position xyz, normal xyz, uv

struct CpuTexture
{
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;
};

struct CpuMaterial
{
    float base_color[4] = { 1, 1, 1, 1 };
    int texture = -1;                // index into CpuMesh::textures
    float alpha_cutoff = -1.0f;      // only for alphaMode MASK
    bool blend = false;              // alphaMode BLEND
};

// one glTF primitive: a run of indices drawn with one material
struct DrawRange
{
    uint32_t first_index = 0, index_count = 0;
    int material = -1;               // -1 = no material, drawn in the default color
};

struct CpuMesh
{
    std::vector<float> vertices;     // vertex_floats per vertex, already in model space
    std::vector<uint32_t> indices;
    std::vector<DrawRange> draws;
    std::vector<CpuMaterial> materials;
    std::vector<CpuTexture> textures;
    float center[3] = { 0, 0, 0 };
    float radius = 1.0f;
    std::string error;
};

// POSITION / NORMAL attributes, which glTF requires to be float vec3
bool read_vec3(const tinygltf::Model& model, int accessor_index, std::vector<float>& out)
{
    if (accessor_index < 0 || accessor_index >= static_cast<int>(model.accessors.size())) return false;
    const auto& accessor = model.accessors[accessor_index];
    if (accessor.type != TINYGLTF_TYPE_VEC3 || accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return false;
    if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size())) return false;
    const auto& view = model.bufferViews[accessor.bufferView];
    if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size())) return false;
    const auto& data = model.buffers[view.buffer].data;
    const int stride = accessor.ByteStride(view);
    const size_t start = view.byteOffset + accessor.byteOffset;
    if (stride <= 0 || accessor.count == 0 || start + stride * (accessor.count - 1) + 12 > data.size()) return false;

    out.resize(accessor.count * 3);
    for (size_t i = 0; i < accessor.count; ++i)
        std::memcpy(&out[i * 3], data.data() + start + i * stride, 12);
    return true;
}

// TEXCOORD_0, which glTF allows as float or as normalized unsigned byte / short
bool read_uvs(const tinygltf::Model& model, int accessor_index, std::vector<float>& out)
{
    if (accessor_index < 0 || accessor_index >= static_cast<int>(model.accessors.size())) return false;
    const auto& accessor = model.accessors[accessor_index];
    if (accessor.type != TINYGLTF_TYPE_VEC2) return false;
    if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size())) return false;
    const auto& view = model.bufferViews[accessor.bufferView];
    if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size())) return false;
    const auto& data = model.buffers[view.buffer].data;
    const int size = tinygltf::GetComponentSizeInBytes(accessor.componentType);
    const int stride = accessor.ByteStride(view);
    const size_t start = view.byteOffset + accessor.byteOffset;
    if (size <= 0 || stride <= 0 || accessor.count == 0 || start + stride * (accessor.count - 1) + 2 * size > data.size()) return false;

    out.resize(accessor.count * 2);
    for (size_t i = 0; i < accessor.count; ++i)
        for (int c = 0; c < 2; ++c)
        {
            const unsigned char* p = data.data() + start + i * stride + c * size;
            switch (accessor.componentType)
            {
            case TINYGLTF_COMPONENT_TYPE_FLOAT: std::memcpy(&out[i * 2 + c], p, 4); break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: out[i * 2 + c] = *p / 255.0f; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, p, 2); out[i * 2 + c] = v / 65535.0f; break; }
            default: return false;
            }
        }
    return true;
}

bool read_indices(const tinygltf::Model& model, int accessor_index, std::vector<uint32_t>& out)
{
    if (accessor_index < 0 || accessor_index >= static_cast<int>(model.accessors.size())) return false;
    const auto& accessor = model.accessors[accessor_index];
    if (accessor.type != TINYGLTF_TYPE_SCALAR) return false;
    if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size())) return false;
    const auto& view = model.bufferViews[accessor.bufferView];
    if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size())) return false;
    const auto& data = model.buffers[view.buffer].data;
    const int size = tinygltf::GetComponentSizeInBytes(accessor.componentType);
    const int stride = accessor.ByteStride(view);
    const size_t start = view.byteOffset + accessor.byteOffset;
    if (size <= 0 || stride <= 0 || accessor.count == 0 || start + stride * (accessor.count - 1) + size > data.size()) return false;

    out.resize(accessor.count);
    for (size_t i = 0; i < accessor.count; ++i)
    {
        const unsigned char* p = data.data() + start + i * stride;
        switch (accessor.componentType)
        {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: out[i] = *p; break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, p, 2); out[i] = v; break; }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: { uint32_t v; std::memcpy(&v, p, 4); out[i] = v; break; }
        default: return false;
        }
    }
    return true;
}

void add_mesh(const tinygltf::Model& model, const tinygltf::Mesh& gltf_mesh, const Mat4& world, CpuMesh& mesh)
{
    // normals get the cofactor matrix (inverse transpose times the determinant), so non-uniform scale doesn't bend them
    float a[3][3];
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 3; ++c) a[row][c] = world.m[c * 4 + row];
    float cof[3][3];
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 3; ++c)
            cof[row][c] = a[(row + 1) % 3][(c + 1) % 3] * a[(row + 2) % 3][(c + 2) % 3]
                        - a[(row + 1) % 3][(c + 2) % 3] * a[(row + 2) % 3][(c + 1) % 3];

    for (const auto& primitive : gltf_mesh.primitives)
    {
        if (primitive.mode != TINYGLTF_MODE_TRIANGLES && primitive.mode != -1) continue;
        auto position_it = primitive.attributes.find("POSITION");
        if (position_it == primitive.attributes.end()) continue;

        std::vector<float> positions, normals;
        if (!read_vec3(model, position_it->second, positions)) continue;
        const size_t count = positions.size() / 3;

        std::vector<uint32_t> triangles;
        if (primitive.indices >= 0)
        {
            if (!read_indices(model, primitive.indices, triangles)) continue;
        }
        else
        {
            triangles.resize(count);
            std::iota(triangles.begin(), triangles.end(), 0u);
        }
        triangles.resize(triangles.size() / 3 * 3);
        if (std::any_of(triangles.begin(), triangles.end(), [&](uint32_t i) { return i >= count; })) continue;

        auto normal_it = primitive.attributes.find("NORMAL");
        const bool has_normals = normal_it != primitive.attributes.end()
            && read_vec3(model, normal_it->second, normals) && normals.size() == positions.size();
        if (!has_normals)
        {
            // smooth normals from the faces around each vertex
            normals.assign(positions.size(), 0.0f);
            for (size_t t = 0; t < triangles.size(); t += 3)
            {
                const float* p0 = &positions[triangles[t] * 3];
                const float* p1 = &positions[triangles[t + 1] * 3];
                const float* p2 = &positions[triangles[t + 2] * 3];
                const float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
                const float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
                const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                for (int k = 0; k < 3; ++k)
                    for (int c = 0; c < 3; ++c) normals[triangles[t + k] * 3 + c] += n[c];
            }
        }

        std::vector<float> uvs;
        auto uv_it = primitive.attributes.find("TEXCOORD_0");
        if (uv_it == primitive.attributes.end() || !read_uvs(model, uv_it->second, uvs) || uvs.size() != count * 2)
            uvs.assign(count * 2, 0.0f);

        DrawRange draw;
        draw.first_index = static_cast<uint32_t>(mesh.indices.size());
        draw.index_count = static_cast<uint32_t>(triangles.size());
        draw.material = primitive.material;
        mesh.draws.push_back(draw);

        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size() / vertex_floats);
        for (size_t i = 0; i < count; ++i)
        {
            const float* p = &positions[i * 3];
            const float* n = &normals[i * 3];
            float out_n[3];
            for (int row = 0; row < 3; ++row)
            {
                mesh.vertices.push_back(world.m[row] * p[0] + world.m[4 + row] * p[1] + world.m[8 + row] * p[2] + world.m[12 + row]);
                out_n[row] = cof[row][0] * n[0] + cof[row][1] * n[1] + cof[row][2] * n[2];
            }
            const float len = std::sqrt(out_n[0] * out_n[0] + out_n[1] * out_n[1] + out_n[2] * out_n[2]);
            for (float v : out_n) mesh.vertices.push_back(len > 0 ? v / len : 0.0f);
            mesh.vertices.push_back(uvs[i * 2]);
            mesh.vertices.push_back(uvs[i * 2 + 1]);
        }
        for (uint32_t i : triangles) mesh.indices.push_back(base + i);
    }
}

void add_node(const tinygltf::Model& model, int node_index, const Mat4& parent, CpuMesh& mesh, int depth)
{
    if (depth > 128 || node_index < 0 || node_index >= static_cast<int>(model.nodes.size())) return;
    const auto& node = model.nodes[node_index];
    const Mat4 world = mul(parent, node_matrix(node));
    if (node.mesh >= 0 && node.mesh < static_cast<int>(model.meshes.size()))
    {
        // skinned meshes are placed by their joints, not by their node (glTF spec), and in the bind pose the joints cancel out
        add_mesh(model, model.meshes[node.mesh], node.skin >= 0 ? identity() : world, mesh);
    }
    for (int child : node.children) add_node(model, child, world, mesh, depth + 1);
}

std::shared_ptr<CpuMesh> load_model(const std::string& path)
{
    auto mesh = std::make_shared<CpuMesh>();
    tinygltf::TinyGLTF loader;
    // always decodes to 8-bit RGBA, which is all the preview needs; an image that fails only loses its texture
    loader.SetImageLoader([](tinygltf::Image* image, const int, std::string*, std::string* warn, int, int,
                             const unsigned char* bytes, int size, void*)
    {
        int width, height, channels;
        unsigned char* pixels = stbi_load_from_memory(bytes, size, &width, &height, &channels, 4);
        if (!pixels)
        {
            if (warn) *warn += "could not decode image " + image->name + "\n";
            return true;
        }
        image->width = width;
        image->height = height;
        image->component = 4;
        image->bits = 8;
        image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
        image->image.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
        stbi_image_free(pixels);
        return true;
    }, nullptr);

    tinygltf::Model model;
    std::string error, warning;
    const bool ascii = path.ends_with(".gltf");
    const bool loaded = ascii ? loader.LoadASCIIFromFile(&model, &error, &warning, path)
                              : loader.LoadBinaryFromFile(&model, &error, &warning, path);
    if (!loaded)
    {
        mesh->error = error.empty() ? "could not load the model" : error;
        return mesh;
    }

    const int scene = model.defaultScene >= 0 ? model.defaultScene : 0;
    if (scene < static_cast<int>(model.scenes.size()))
    {
        for (int node : model.scenes[scene].nodes) add_node(model, node, identity(), *mesh, 0);
    }
    else
    {
        // no scene: every node that isn't someone's child is a root
        std::vector<bool> is_child(model.nodes.size(), false);
        for (const auto& node : model.nodes)
            for (int child : node.children)
                if (child >= 0 && child < static_cast<int>(is_child.size())) is_child[child] = true;
        for (int n = 0; n < static_cast<int>(model.nodes.size()); ++n)
            if (!is_child[n]) add_node(model, n, identity(), *mesh, 0);
    }

    if (mesh->indices.empty())
    {
        mesh->error = "no triangle meshes in this file";
        return mesh;
    }

    // textures keep the image's index, images that didn't decode stay empty
    mesh->textures.resize(model.images.size());
    for (size_t i = 0; i < model.images.size(); ++i)
    {
        auto& image = model.images[i];
        if (image.width <= 0 || image.height <= 0 || image.component != 4 || image.bits != 8
            || image.image.size() != static_cast<size_t>(image.width) * image.height * 4) continue;
        mesh->textures[i].width = image.width;
        mesh->textures[i].height = image.height;
        mesh->textures[i].rgba = std::move(image.image);
    }
    for (const auto& material : model.materials)
    {
        CpuMaterial out;
        const auto& pbr = material.pbrMetallicRoughness;
        if (pbr.baseColorFactor.size() == 4)
            for (int c = 0; c < 4; ++c) out.base_color[c] = static_cast<float>(pbr.baseColorFactor[c]);
        const int texture = pbr.baseColorTexture.index;
        if (texture >= 0 && texture < static_cast<int>(model.textures.size()))
        {
            const int source = model.textures[texture].source;
            if (source >= 0 && source < static_cast<int>(mesh->textures.size()) && !mesh->textures[source].rgba.empty())
                out.texture = source;
        }
        if (material.alphaMode == "MASK") out.alpha_cutoff = static_cast<float>(material.alphaCutoff);
        else if (material.alphaMode == "BLEND") out.blend = true;
        mesh->materials.push_back(out);
    }

    float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (size_t i = 0; i < mesh->vertices.size(); i += vertex_floats)
        for (int c = 0; c < 3; ++c)
        {
            lo[c] = std::min(lo[c], mesh->vertices[i + c]);
            hi[c] = std::max(hi[c], mesh->vertices[i + c]);
        }
    float diagonal = 0;
    for (int c = 0; c < 3; ++c)
    {
        mesh->center[c] = (lo[c] + hi[c]) * 0.5f;
        diagonal += (hi[c] - lo[c]) * (hi[c] - lo[c]);
    }
    mesh->radius = std::max(std::sqrt(diagonal) * 0.5f, 1e-4f);
    return mesh;
}

// ---------------------------------------------------------------- GPU side (main thread only)
struct PreviewModel
{
    std::future<std::shared_ptr<CpuMesh>> loading;
    std::filesystem::file_time_type write_time;
    GLuint vao = 0, vbo = 0, ebo = 0;
    std::vector<GLuint> textures;            // per glTF image, 0 where it didn't decode
    std::vector<CpuMaterial> materials;
    std::vector<DrawRange> draws;
    float center[3] = { 0, 0, 0 };
    float radius = 1.0f;
    std::string error;
    bool ready = false;
    int last_used_frame = 0;
};

struct PreviewTarget
{
    bool initialized = false;
    std::string error;
    GLuint fbo = 0, color = 0, depth = 0, program = 0;
    GLint mvp_location = -1, normal_matrix_location = -1, base_color_location = -1, use_texture_location = -1,
          alpha_cutoff_location = -1, blend_location = -1, texture_location = -1;
};

std::unordered_map<std::string, PreviewModel> models;
PreviewTarget target;

void release(PreviewModel& model)
{
    if (model.ebo) glDeleteBuffers(1, &model.ebo);
    if (model.vbo) glDeleteBuffers(1, &model.vbo);
    if (model.vao) glDeleteVertexArrays(1, &model.vao);
    model.vao = model.vbo = model.ebo = 0;
    for (GLuint texture : model.textures)
        if (texture) glDeleteTextures(1, &texture);
    model.textures.clear();
}

void upload(PreviewModel& model, const CpuMesh& mesh)
{
    if (!mesh.error.empty())
    {
        model.error = mesh.error;
        return;
    }
    GLint last_vao, last_array_buffer;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);

    glGenVertexArrays(1, &model.vao);
    glGenBuffers(1, &model.vbo);
    glGenBuffers(1, &model.ebo);
    glBindVertexArray(model.vao);
    glBindBuffer(GL_ARRAY_BUFFER, model.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(float)), mesh.vertices.data(), GL_STATIC_DRAW);
    constexpr GLsizei stride = vertex_floats * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(uint32_t)), mesh.indices.data(), GL_STATIC_DRAW);

    glBindVertexArray(last_vao);
    glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);

    GLint last_texture, last_unpack_alignment;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &last_unpack_alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    model.textures.assign(mesh.textures.size(), 0);
    for (size_t i = 0; i < mesh.textures.size(); ++i)
    {
        const CpuTexture& texture = mesh.textures[i];
        if (texture.rgba.empty()) continue;
        glGenTextures(1, &model.textures[i]);
        glBindTexture(GL_TEXTURE_2D, model.textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texture.width, texture.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture.rgba.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    glBindTexture(GL_TEXTURE_2D, last_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, last_unpack_alignment);

    model.materials = mesh.materials;
    model.draws = mesh.draws;
    // see-through parts go last so what's behind them is already drawn
    std::stable_partition(model.draws.begin(), model.draws.end(), [&](const DrawRange& draw)
    {
        return draw.material < 0 || draw.material >= static_cast<int>(model.materials.size()) || !model.materials[draw.material].blend;
    });
    std::copy_n(mesh.center, 3, model.center);
    model.radius = mesh.radius;
    model.ready = true;
}

GLuint compile_shader(GLenum type, const char* source, std::string& error)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        error = log;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool ensure_target()
{
    if (target.initialized) return target.error.empty();
    target.initialized = true;

    const char* vertex_source = R"(#version 330 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
uniform mat4 mvp;
uniform mat3 normal_matrix;
out vec3 v_normal;
out vec2 v_uv;
void main()
{
    v_normal = normal_matrix * normal;
    v_uv = uv;
    gl_Position = mvp * vec4(position, 1.0);
})";
    // lit from both sides, since game models are often mirrored or have flipped faces
    const char* fragment_source = R"(#version 330 core
in vec3 v_normal;
in vec2 v_uv;
uniform sampler2D base_texture;
uniform bool use_texture;
uniform vec4 base_color;
uniform float alpha_cutoff;
uniform bool blend;
out vec4 color;
void main()
{
    vec4 albedo = base_color;
    if (use_texture) albedo *= texture(base_texture, v_uv);
    if (albedo.a < alpha_cutoff) discard;
    vec3 n = normalize(v_normal);
    float key = abs(dot(n, normalize(vec3(0.4, 0.7, 0.6))));
    float fill = abs(dot(n, normalize(vec3(-0.6, -0.2, 0.4))));
    color = vec4(albedo.rgb * (0.35 + 0.6 * key + 0.15 * fill), blend ? albedo.a : 1.0);
})";
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_source, target.error);
    GLuint fs = vs ? compile_shader(GL_FRAGMENT_SHADER, fragment_source, target.error) : 0;
    if (!vs || !fs)
    {
        if (vs) glDeleteShader(vs);
        return false;
    }
    target.program = glCreateProgram();
    glAttachShader(target.program, vs);
    glAttachShader(target.program, fs);
    glLinkProgram(target.program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(target.program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        char log[512];
        glGetProgramInfoLog(target.program, sizeof(log), nullptr, log);
        target.error = log;
        return false;
    }
    target.mvp_location = glGetUniformLocation(target.program, "mvp");
    target.normal_matrix_location = glGetUniformLocation(target.program, "normal_matrix");
    target.base_color_location = glGetUniformLocation(target.program, "base_color");
    target.use_texture_location = glGetUniformLocation(target.program, "use_texture");
    target.alpha_cutoff_location = glGetUniformLocation(target.program, "alpha_cutoff");
    target.blend_location = glGetUniformLocation(target.program, "blend");
    target.texture_location = glGetUniformLocation(target.program, "base_texture");

    GLint last_texture, last_renderbuffer, last_fbo;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &last_renderbuffer);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);

    glGenTextures(1, &target.color);
    glBindTexture(GL_TEXTURE_2D, target.color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, render_size, render_size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &target.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, target.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, render_size, render_size);

    glGenFramebuffers(1, &target.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target.depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        target.error = "preview framebuffer is incomplete";

    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, last_renderbuffer);
    glBindTexture(GL_TEXTURE_2D, last_texture);
    return target.error.empty();
}

// ImGui only records draw commands until the end of the frame, so drawing into our own framebuffer here is safe
// as long as every bit of GL state we touch is put back
void render(const PreviewModel& model)
{
    GLint last_fbo, last_program, last_vao, last_viewport[4];
    GLfloat last_clear_color[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vao);
    glGetIntegerv(GL_VIEWPORT, last_viewport);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, last_clear_color);
    const GLboolean last_depth_test = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean last_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean last_cull_face = glIsEnabled(GL_CULL_FACE);
    const GLboolean last_blend = glIsEnabled(GL_BLEND);
    GLint last_active_texture, last_texture, last_blend_src_rgb, last_blend_dst_rgb, last_blend_src_alpha, last_blend_dst_alpha;
    GLboolean last_depth_mask;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
    glGetIntegerv(GL_BLEND_SRC_RGB, &last_blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &last_blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &last_blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &last_blend_dst_alpha);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &last_depth_mask);

    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    glViewport(0, 0, render_size, render_size);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);  // glClear only clears depth while writing is on
    glClearColor(0.16f, 0.16f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // turntable: center the model, spin it around the vertical axis and look at it slightly from above
    const float spin = static_cast<float>(ImGui::GetTime()) * 0.8f;
    const Mat4 rotation = mul(rotation_x(0.35f), rotation_y(spin));
    const Mat4 world = mul(rotation, translation(-model.center[0], -model.center[1], -model.center[2]));
    const float fov = 35.0f * 3.14159265f / 180.0f;
    const float distance = model.radius / std::sin(fov * 0.5f) * 1.05f;
    const Mat4 view = translation(0, 0, -distance);
    const Mat4 projection = perspective(fov, 1.0f, std::max(distance - model.radius * 1.5f, distance * 0.01f),
                                        distance + model.radius * 1.5f);
    const Mat4 mvp = mul(projection, mul(view, world));
    const float normal_matrix[9] = {
        rotation.m[0], rotation.m[1], rotation.m[2],
        rotation.m[4], rotation.m[5], rotation.m[6],
        rotation.m[8], rotation.m[9], rotation.m[10] };

    glUseProgram(target.program);
    glUniformMatrix4fv(target.mvp_location, 1, GL_FALSE, mvp.m);
    glUniformMatrix3fv(target.normal_matrix_location, 1, GL_FALSE, normal_matrix);
    glUniform1i(target.texture_location, 0);
    glActiveTexture(GL_TEXTURE0);
    // see-through parts blend their color but leave the image opaque, so the tooltip doesn't show through
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glBindVertexArray(model.vao);
    for (const DrawRange& draw : model.draws)
    {
        // parts without a material get a neutral clay color
        static const CpuMaterial no_material = { { 0.78f, 0.74f, 0.68f, 1.0f } };
        const bool has_material = draw.material >= 0 && draw.material < static_cast<int>(model.materials.size());
        const CpuMaterial& material = has_material ? model.materials[draw.material] : no_material;
        const GLuint texture = material.texture >= 0 && material.texture < static_cast<int>(model.textures.size())
            ? model.textures[material.texture] : 0;

        glUniform4fv(target.base_color_location, 1, material.base_color);
        glUniform1i(target.use_texture_location, texture != 0);
        glUniform1f(target.alpha_cutoff_location, material.alpha_cutoff);
        glUniform1i(target.blend_location, material.blend);
        glBindTexture(GL_TEXTURE_2D, texture);
        if (material.blend) { glEnable(GL_BLEND); glDepthMask(GL_FALSE); }
        else { glDisable(GL_BLEND); glDepthMask(GL_TRUE); }
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(draw.index_count), GL_UNSIGNED_INT,
                       reinterpret_cast<void*>(static_cast<uintptr_t>(draw.first_index) * sizeof(uint32_t)));
    }

    glBindVertexArray(last_vao);
    glUseProgram(last_program);
    glBindTexture(GL_TEXTURE_2D, last_texture);  // still on unit 0, where it was saved from
    glActiveTexture(last_active_texture);
    glBlendFuncSeparate(last_blend_src_rgb, last_blend_dst_rgb, last_blend_src_alpha, last_blend_dst_alpha);
    glDepthMask(last_depth_mask);
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
    glClearColor(last_clear_color[0], last_clear_color[1], last_clear_color[2], last_clear_color[3]);
    if (last_depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (last_scissor_test) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (last_cull_face) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (last_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
}

// drops the least recently shown models once the cache is full (models still loading are kept,
// destroying their future would block until the load finishes)
void evict_old_models()
{
    while (models.size() > max_cached_models)
    {
        auto oldest = models.end();
        for (auto it = models.begin(); it != models.end(); ++it)
            if (!it->second.loading.valid() && (oldest == models.end() || it->second.last_used_frame < oldest->second.last_used_frame))
                oldest = it;
        if (oldest == models.end()) return;
        release(oldest->second);
        models.erase(oldest);
    }
}
}

void draw_model_preview(const std::string& path)
{
    std::error_code ec;
    const auto write_time = std::filesystem::last_write_time(path, ec);

    if (!models.contains(path)) evict_old_models();
    PreviewModel& model = models[path];
    model.last_used_frame = ImGui::GetFrameCount();

    // the file changed since it was loaded, e.g. an animation was applied to it
    if (!model.loading.valid() && (model.ready || !model.error.empty()) && write_time != model.write_time)
    {
        release(model);
        model = PreviewModel{};
        model.last_used_frame = ImGui::GetFrameCount();
    }
    if (!model.ready && model.error.empty() && !model.loading.valid())
    {
        model.write_time = write_time;
        model.loading = std::async(std::launch::async, load_model, path);
    }
    if (model.loading.valid() && model.loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        upload(model, *model.loading.get());
    }

    if (!model.error.empty())
    {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "No preview: %s", model.error.c_str());
        return;
    }
    if (!model.ready)
    {
        ImGui::TextUnformatted("Loading preview...");
        return;
    }
    if (!ensure_target())
    {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Preview unavailable: %s", target.error.c_str());
        return;
    }
    render(model);
    // flip vertically, OpenGL textures start at the bottom
    ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(target.color)), ImVec2(display_size, display_size), ImVec2(0, 1), ImVec2(1, 0));
}
