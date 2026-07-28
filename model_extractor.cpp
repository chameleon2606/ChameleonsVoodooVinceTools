#include <iostream>
#include <fstream>
#include <array>
#include <imgui.h>
#include <string>
#include <filesystem>
#include <typeinfo>
#include <unordered_map>
#include "model_extractor.h"
#include "GLFW/glfw3.h"
#include "main_window.h"
#include "include/json.hpp"
#include "hot_extractor.h"

using namespace std;

string path = filesystem::current_path().string()+"\\models\\";

bool valid_folders;
static constexpr uint32_t face_table_section_size = 48;
static float uv_scale = 6.0f;
constexpr uint16_t bone_info_size = 288;
const vector<float>default_matrix = {1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0};
struct gator_header
{
    char magic[4];
    uint32_t version, thing, thing2, vert_count, tri_count, tstrip_count, material_count, bone_count;
    int16_t thing5, thing6;
    uint16_t thing7, hitbox_points, bone_joints_count, string_count;
    uint32_t start_of_verts, start_of_tris, tstrip_table, materials_offset, hitbox_data_offset, bones_table, bone_joints_offset, string_offsets, string_lookup_table;
    float min_x, min_y, min_z, max_x, max_y, max_z, thing16, thing17, thing18, thing19, thing20;
};
struct vert_info
{
    float x_pos, y_pos, z_pos;
    uint8_t bone1_weight, bone2_weight, bone3_weight, bone4_weight;
    uint8_t bone1_index, bone2_index, bone3_index, bone4_index;
    float x_norm, y_norm, z_norm, x_unknown, y_unknown, z_unknown;
    int32_t thing3;
    float x_uv1, y_uv1, x_uv2, y_uv2;
};
struct tstrip_info
{
    uint16_t verts_in_strip;
    uint16_t u_value0[7];
    float u_value1;
    uint16_t material_index;
    uint16_t joint_table_index;
    uint16_t u_value2[2];
    uint16_t texture_name_2;
    uint16_t u_value3[9];
};
struct material_info
{
    int16_t texture_name_index, u_value0, normal_map_index, environment_texture_index, overlay_texture_index;
    uint16_t u_values1[7];
    float u_value2;
    uint16_t u_values3[18];
};
struct bones_data
{
    uint16_t index;
    int16_t parent_index;
    uint32_t padding;
    float value_x, value_y, value_z;
    float rot_x, rot_y, rot_z;
    float bind_matrix[4][4];
    float inverse_bind_matrix[4][4];
    float matrix_3[4][4];
    float matrix_4[4][4];
};
struct bone_joint_info
{
    uint32_t size, offset;
};
struct bsp_header
{
    char signature[4];
    uint32_t version;
    uint32_t file_size;
    
};

bool folder_validation(char folder[])
{
    if (filesystem::exists(folder))
    {
        return true;
    }
    else
    {
        return false;
    }
}

//converts 4x4 float to array
array<array<float,4>,4> to_array(float m[4][4])
{
    array<array<float,4>,4> result{};
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            result[row][col] = m[row][col];
    return result;
}

//mirrors 4x4 matrix for flipping the model
array<array<float,4>,4> mirror_x(const array<array<float,4>,4>& m)
{
    array<array<float,4>,4> result{};
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
        {
            float sign = ((row == 0) != (col == 0)) ? -1.0f : 1.0f;
            result[row][col] = m[row][col] * sign;
        }
    return result;
}

//multiplies 2 the current bind matrix with it's parent inverse bind matrix
array<array<float,4>,4> mat4_multiply(const array<array<float,4>,4>& a, const array<array<float,4>,4>& b)
{
    array<array<float,4>,4> result{};
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
                sum += a[row][k] * b[k][col];
            result[row][col] = sum;
        }
    return result;
}


void init_model_extractor()
{
    
}

vector<string> extract_model(string current_filepath)
{
    uint16_t accessor_count = 0;
    uint16_t buffer_view_count = 0;

    uint16_t vertex_position_index = 0;
    uint16_t normals_index = 0;
    uint16_t texcoord_index = 0;
    uint16_t indices_index = 0;
    uint16_t weights_index = 0;
    uint16_t bone_indices_index = 0;
    
    size_t last_slash = current_filepath.find_last_of('\\');
    string current_filename = current_filepath.substr(last_slash+1, current_filepath.size()-last_slash);
    size_t last_dot = current_filename.find_last_of('.');
    current_filename = current_filename.substr(0,last_dot);

    // creates a new .gltf & .bin file with the name of the gator file
    ifstream src_file(current_filepath, ios::binary); // input .gator file
    ofstream gltf_file(combined_output_path+"\\"+current_filename + ".gltf", ios::trunc);
    ofstream bin_file(combined_output_path+"\\"+current_filename + ".bin", ios::binary | ios::trunc);

    nlohmann::json gltf_data;
    gltf_data["nodes"] = nlohmann::json::array();
    
    nlohmann::json asset;
    gltf_data["asset"]["version"] = "2.0";
    gltf_data["asset"]["generator"] = "chameleon's voodoo tools";
    
    gator_header current_gator_header;
    
    // reads the header (128 bits)
    src_file.read(reinterpret_cast<char*>(&current_gator_header), sizeof(gator_header));
    if (strncmp(current_gator_header.magic, "GATR", 4) != 0)
    {
        exit(0);
    }

    // collects all strings
    vector<uint32_t> string_offsets(current_gator_header.string_count);
    src_file.seekg(current_gator_header.string_offsets, ios::beg);
    src_file.read(reinterpret_cast<char*>(string_offsets.data()), current_gator_header.string_count * sizeof(uint32_t));
    vector<string> string_list;
    vector<string> texture_list;
    
    for (auto& offset : string_offsets)
    {
        string texturename;
        char c;
        src_file.clear();
        src_file.seekg(current_gator_header.string_lookup_table + offset, ios::beg);
        while (src_file.read(&c, 1)&& c != '\0')
        {
            texturename += c;
        }
        if (texturename.ends_with(".dds"))
        {
            texture_list.push_back(texturename);
        }
        string_list.push_back(texturename);
    }
    

    gltf_data["scene"] = 0;
    nlohmann::json scene;
    scene["name"] = "Scene";
    if (include_bones)
    {
        if (current_gator_header.bone_count > 1)
        {
            scene["nodes"] = {0,1};
        }
        else
        {
            scene["nodes"] = {0};
        }
    }
    else
    {
        scene["nodes"] = {0};
    }
    gltf_data["scenes"].push_back(scene);

    uint32_t current_bin_size = 0;
    
    // collects bone data
    if (include_bones)
    {
        // loops through bones to collect all parents data
        vector<bones_data> bones;
        vector<int16_t> bone_parent_list;
        
        for (uint32_t i = 0; i < current_gator_header.bone_count; i++)
        {
            bones_data current_bone;
            src_file.clear();
            src_file.seekg(current_gator_header.bones_table + (bone_info_size * i), ios::beg);
            src_file.read(reinterpret_cast<char*>(&current_bone), sizeof(bones_data));
        
            bones.push_back(current_bone);
            bone_parent_list.push_back(current_bone.parent_index);
        }
        
        
        // build the index lookup
        unordered_map<uint16_t, size_t> bone_index_to_pos;
        for (size_t k = 0; k < bones.size(); k++)
        {
            bone_index_to_pos[bones[k].index] = k;
        }
        
        
        // mirror every bone's bind/inverse-bind matrices to match the flipped mesh
        vector<array<array<float,4>,4>> mirrored_bind(bones.size());
        vector<array<array<float,4>,4>> mirrored_inv_bind(bones.size());
        for (size_t k = 0; k < bones.size(); k++)
        {
            mirrored_bind[k]     = mirror_x(to_array(bones[k].bind_matrix));
            mirrored_inv_bind[k] = mirror_x(to_array(bones[k].inverse_bind_matrix));
        }

        
        // loops though bones again to collect all bone data
        vector<float>inverse_bind_matrix_list;
        for (uint32_t i = 0; i < bones.size(); i++)
        {
            vector<float>pose_positions;
            array<array<float,4>,4> local_matrix;

            // collects inverse bind matrix data for the .bin file
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    inverse_bind_matrix_list.push_back(mirrored_inv_bind[i][row][col]);

            // rest pose (relative to parent)
            if (bones[i].parent_index == -1)
            {
                local_matrix = mirrored_bind[i];
            }
            else
            {
                size_t parent_pos = bone_index_to_pos.at(static_cast<uint16_t>(bones[i].parent_index));
                local_matrix = mat4_multiply(mirrored_bind[i], mirrored_inv_bind[parent_pos]);
            }

            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    pose_positions.push_back(local_matrix[row][col]);
            
        
            nlohmann::json bone;
            bone["name"] = string_list[bones[i].index];
            if (pose_positions != default_matrix)
            {
                bone["matrix"] = pose_positions;
            }
            vector<int16_t> bone_children_list;
            if (i == 0)
            {
                bone["mesh"] = 0;
                if (current_gator_header.bone_count > 1)
                {
                    bone["skin"] = 0;
                }
            }
            for (uint32_t k = 0; k < bone_parent_list.size(); k++)
            {
                if (bone_parent_list[k] == bones[i].index)
                {
                    bone_children_list.push_back(k);
                }
            }
            if (!bone_children_list.empty())
            {
                bone["children"] = bone_children_list;
            }
            gltf_data["nodes"].push_back(bone);
        }

    
        nlohmann::json rig_accessor;
        rig_accessor["bufferView"] = buffer_view_count;
        rig_accessor["componentType"] = 5126;
        rig_accessor["count"] = bones.size();
        rig_accessor["type"] = "MAT4";
        gltf_data["accessors"].push_back(rig_accessor);
        accessor_count++;
    
        current_bin_size = bin_file.tellp();
        bin_file.write(reinterpret_cast<const char*>(inverse_bind_matrix_list.data()),inverse_bind_matrix_list.size() * sizeof(float));
    
        nlohmann::json rig_buffer_views;
        rig_buffer_views["byteLength"] = bones.size()*16*sizeof(float);
        rig_buffer_views["buffer"] = 0;
        rig_buffer_views["byteOffset"] = current_bin_size;
        gltf_data["bufferViews"].push_back(rig_buffer_views);
        buffer_view_count++;
    
        
        vector<int16_t> bone_joints_list;
        for (uint32_t i = 0; i < bones.size(); i++)
        {
            bone_joints_list.push_back(i);
        }

        if (current_gator_header.bone_count > 1)
        {
            nlohmann::json skin;
            skin["inverseBindMatrices"] = 0;
            skin["joints"] = bone_joints_list;
            skin["name"] = current_filename;
            gltf_data["skins"].push_back(skin);
        }
    }
    else
    {
        nlohmann::json node;
        node["mesh"] = 0;
        node["name"] = current_filename;
        gltf_data["nodes"].push_back(node);
    }
    
    
    // collects vertex data
    vector<array<float, 3>> norms;
    vector<array<float, 2>> uvs;

    vector<char>vertex_pos_buffer;
    vector<char>norms_buffer;
    vector<char>uv_buffer;
    
    vector<char>joints_buffer;
    vector<char>weights_buffer;
    vector<uint16_t>index_reference;
    
    vector<uint8_t>joints;
    vector<uint8_t>weights;
    
    vector<float>vertex_positions;
    vector<float>normals;
    vector<float>texcoords;
    
    
    unordered_map<uint16_t, uint16_t> joints_map;
    vector<vector<char>> joint_lists;
    unordered_map<uint16_t, uint8_t> joints_index_map;
    
    if (current_gator_header.bone_joints_offset)
    {
        vector<uint32_t>verts_in_face_list;
        
        for (uint32_t j = 0; j < current_gator_header.tstrip_count; j++)
        {
            //get table struct and find verts in face count
            src_file.clear();
            src_file.seekg(current_gator_header.tstrip_table + (face_table_section_size * j), ios::beg);
            tstrip_info current_tstrip;
            src_file.read(reinterpret_cast<char*>(&current_tstrip), sizeof(tstrip_info));
                        
            verts_in_face_list.push_back(current_tstrip.verts_in_strip);
            joints_index_map[j] = current_tstrip.joint_table_index;
        }
        
        //seek to beginning of faces
        //read all tris
        src_file.clear();
        vector<uint16_t> all_faces(current_gator_header.tri_count);
        src_file.seekg(current_gator_header.start_of_tris, ios::beg);
        src_file.read(reinterpret_cast<char*>(all_faces.data()), current_gator_header.tri_count*sizeof(uint16_t));
        
        uint16_t tick = 0;
        uint32_t vert_sum = verts_in_face_list[0];
        uint32_t last_vert = 0;
        
        //writes map of what vertex groups belong to what joint indices
        for (uint32_t j = 0; j < current_gator_header.tri_count; j++)
        {
            if (j >= vert_sum)
            {
                tick++;
                vert_sum += verts_in_face_list[tick];
            }
            if (all_faces[j] > last_vert)
            {
                joints_map[all_faces[j]] = joints_index_map[tick];
                last_vert = all_faces[j];
            }
        }
        
        //gets list of bone joints
        for (size_t j = 0; j < current_gator_header.bone_joints_count; j++)
        {
            src_file.clear();
            src_file.seekg(current_gator_header.bone_joints_offset + (j * sizeof(bone_joint_info)), ios::beg);
            bone_joint_info current_bone_info;
            src_file.read(reinterpret_cast<char*>(&current_bone_info), sizeof(current_bone_info));
            
            vector<char> bone_order(current_bone_info.size);
            src_file.clear();
            src_file.seekg(current_bone_info.offset, ios::beg);
            src_file.read(bone_order.data(), bone_order.size());
            
            joint_lists.push_back(bone_order);
        }
    }

    // loop through each vertex data section
    uint32_t pos = 0;
    for (uint32_t i=0; pos < current_gator_header.start_of_tris; i++)
    {
        src_file.seekg(current_gator_header.start_of_verts +(sizeof(vert_info)*i), ios::beg);
        
        vert_info current_verts;

        // grabs all data for that vertex
        src_file.read(reinterpret_cast<char*>(&current_verts), sizeof(vert_info));
        
        if (include_bones)
        {
            if (current_gator_header.bone_joints_offset)
            {
                //writes bone indices
                joints.push_back(joint_lists[joints_map[i]][current_verts.bone1_index]);
                joints.push_back(joint_lists[joints_map[i]][current_verts.bone2_index]);
                joints.push_back(joint_lists[joints_map[i]][current_verts.bone3_index]);
                joints.push_back(joint_lists[joints_map[i]][current_verts.bone4_index]);
            }
            else
            {
                joints.push_back(current_verts.bone1_index);
                joints.push_back(current_verts.bone2_index);
                joints.push_back(current_verts.bone3_index);
                joints.push_back(current_verts.bone4_index);
            }

            weights.push_back(current_verts.bone1_weight);
            weights.push_back(current_verts.bone2_weight);
            weights.push_back(current_verts.bone3_weight);
            weights.push_back(current_verts.bone4_weight);
        }

        
        vertex_positions.push_back(-current_verts.x_pos);
        vertex_positions.push_back(current_verts.y_pos);
        vertex_positions.push_back(current_verts.z_pos);

        normals.push_back(-current_verts.x_norm);
        normals.push_back(current_verts.y_norm);
        normals.push_back(current_verts.z_norm);
        
        
        texcoords.push_back(current_verts.x_uv1*6);
        texcoords.push_back(current_verts.y_uv1*6);

        // reads normals buffer
        src_file.seekg(current_gator_header.start_of_verts +(sizeof(vert_info)*i) + 20, ios::beg);
        vector<char>tmp_normal_buffer(3*sizeof(float));
        src_file.read(tmp_normal_buffer.data(), tmp_normal_buffer.size());
        copy(tmp_normal_buffer.begin(), tmp_normal_buffer.end(), back_inserter(norms_buffer));
        
        // puts normal data into a list
        norms.push_back({current_verts.x_norm,current_verts.y_norm,current_verts.z_norm});
        
        // puts UVs into a list
        current_verts.x_uv1 *= uv_scale;
        current_verts.y_uv1 *= uv_scale;
        uvs.push_back({current_verts.x_uv1, (current_verts.y_uv1 * -1) + 1});

        src_file.seekg(current_gator_header.start_of_verts +(sizeof(vert_info)*i) + 48, ios::beg);
        vector<char>tmp_uv_buffer(2*sizeof(float));
        src_file.read(tmp_uv_buffer.data(), tmp_uv_buffer.size());
        copy(tmp_uv_buffer.begin(), tmp_uv_buffer.end(), back_inserter(uv_buffer));
        
        pos = src_file.tellg();
    }

    if (include_bones)
    {
        
        if (current_gator_header.bone_count > 1)
        {
            current_bin_size = bin_file.tellp();
            
            // JOINTS
            nlohmann::json joints_accessor;
            joints_accessor["bufferView"] = buffer_view_count;
            joints_accessor["componentType"] = 5121;
            joints_accessor["count"] = current_gator_header.vert_count;
            joints_accessor["type"] = "VEC4";
            gltf_data["accessors"].push_back(joints_accessor);
            bone_indices_index = accessor_count;
            accessor_count++;

            nlohmann::json joints_buffer_views;
            joints_buffer_views["byteLength"] = current_gator_header.vert_count * 4;
            joints_buffer_views["buffer"] = 0;
            joints_buffer_views["byteOffset"] = current_bin_size;
            joints_buffer_views["target"] = 34962;
            gltf_data["bufferViews"].push_back(joints_buffer_views);
            bin_file.write(reinterpret_cast<const char*>(joints.data()),joints.size());
            buffer_view_count++;

            // WEIGHTS
            nlohmann::json weights_accessor;
            weights_accessor["bufferView"] = buffer_view_count;
            weights_accessor["componentType"] = 5121;
            weights_accessor["count"] = current_gator_header.vert_count;
            weights_accessor["type"] = "VEC4";
            weights_accessor["normalized"] = true;
            gltf_data["accessors"].push_back(weights_accessor);
            weights_index = accessor_count;
            accessor_count++;

            current_bin_size = bin_file.tellp();

            nlohmann::json weights_buffer_views;
            weights_buffer_views["byteLength"] = current_gator_header.vert_count * 4;
            weights_buffer_views["buffer"] = 0;
            weights_buffer_views["byteOffset"] = current_bin_size;
            weights_buffer_views["target"] = 34962;
            gltf_data["bufferViews"].push_back(weights_buffer_views);
            bin_file.write(reinterpret_cast<const char*>(weights.data()),weights.size());
            buffer_view_count++;
        }
    }

    // POSITIONS
    current_bin_size = bin_file.tellp();
    nlohmann::json vertex_pos_accessor;
    vertex_pos_accessor["bufferView"] = buffer_view_count;
    vertex_pos_accessor["componentType"] = 5126;
    vertex_pos_accessor["count"] = current_gator_header.vert_count;
    
    vertex_pos_accessor["max"] = {current_gator_header.max_x,current_gator_header.max_y,current_gator_header.max_z};
    vertex_pos_accessor["min"] = {current_gator_header.min_x,current_gator_header.min_y,current_gator_header.min_z};
    
    vertex_pos_accessor["type"] = "VEC3";
    gltf_data["accessors"].push_back(vertex_pos_accessor);
    vertex_position_index = accessor_count;
    accessor_count++;

    nlohmann::json vertex_pos_buffer_views;
    vertex_pos_buffer_views["byteLength"] = 3*current_gator_header.vert_count*sizeof(float);
    vertex_pos_buffer_views["buffer"] = 0;
    vertex_pos_buffer_views["byteOffset"] = current_bin_size;
    vertex_pos_buffer_views["target"] = 34962;
    gltf_data["bufferViews"].push_back(vertex_pos_buffer_views);
    bin_file.write(reinterpret_cast<const char*>(vertex_positions.data()),vertex_positions.size() * sizeof(float));
    buffer_view_count++;
    
    // NORMALS
    current_bin_size = bin_file.tellp();
    nlohmann::json norms_accessor;
    norms_accessor["bufferView"] = buffer_view_count;
    norms_accessor["componentType"] = 5126;
    norms_accessor["count"] = current_gator_header.vert_count;
    norms_accessor["type"] = "VEC3";
    gltf_data["accessors"].push_back(norms_accessor);
    normals_index = accessor_count;
    accessor_count++;
    
    nlohmann::json norms_buffer_views;
    norms_buffer_views["byteLength"] = 3*current_gator_header.vert_count*sizeof(float);
    norms_buffer_views["buffer"] = 0;
    norms_buffer_views["byteOffset"] = current_bin_size;
    norms_buffer_views["target"] = 34962;
    gltf_data["bufferViews"].push_back(norms_buffer_views);
    bin_file.write(reinterpret_cast<const char*>(normals.data()),normals.size() * sizeof(float));
    buffer_view_count++;

    if (!texture_list.empty())
    {
        // TEXCOORDS / UVs
        current_bin_size = bin_file.tellp();
        nlohmann::json uv_accessor;
        uv_accessor["bufferView"] = buffer_view_count;
        uv_accessor["componentType"] = 5126;
        uv_accessor["count"] = current_gator_header.vert_count;
        uv_accessor["type"] = "VEC2";
        gltf_data["accessors"].push_back(uv_accessor);
        texcoord_index = accessor_count;
        accessor_count++;
        
        nlohmann::json uv_buffer_views;
        uv_buffer_views["byteLength"] = 2*current_gator_header.vert_count*sizeof(float);
        uv_buffer_views["buffer"] = 0;
        uv_buffer_views["byteOffset"] = current_bin_size;
        uv_buffer_views["target"] = 34962;
        gltf_data["bufferViews"].push_back(uv_buffer_views);
        bin_file.write(reinterpret_cast<const char*>(texcoords.data()),texcoords.size() * sizeof(float));
        buffer_view_count++;
    }
    
    // reference to the next offset of vert strips
    int last_verts_amount = 0;
    vector<char>new_idx;
    
    nlohmann::json meshes;
    meshes["name"] = current_filename;

    indices_index = accessor_count;

    nlohmann::json primitives;
    
    // loops through the face strips and writes the tstrips
    for (uint32_t i = 0; i < current_gator_header.tstrip_count; i++)
    {
        vector<char>indices_buffer;
        vector<char> fixed_idx;
        
        // seek to the beginning of the next tstrip table
        src_file.clear();
        src_file.seekg(current_gator_header.tstrip_table + (face_table_section_size * i), ios::beg);
        tstrip_info current_tstrip;
        src_file.read(reinterpret_cast<char*>(&current_tstrip), sizeof(tstrip_info));
        
        // read binary data
        src_file.seekg(current_gator_header.start_of_tris + (last_verts_amount * sizeof(uint16_t)), ios::beg);
        vector<char>tmp_idx_buffer(current_tstrip.verts_in_strip*sizeof(uint16_t));
        src_file.read(tmp_idx_buffer.data(), tmp_idx_buffer.size());

        vector<array<char,2>>idx;
        
        for (size_t k=0; k<tmp_idx_buffer.size(); k+=2)
        {
            idx.push_back({tmp_idx_buffer[k], tmp_idx_buffer[k+1]});
        }        
        
        // seek to the vert indices of that section
        src_file.clear();
        src_file.seekg(current_gator_header.start_of_tris + (last_verts_amount * sizeof(uint16_t)), ios::beg);
        vector<uint16_t> strip(current_tstrip.verts_in_strip);
        src_file.read(reinterpret_cast<char*>(strip.data()), current_tstrip.verts_in_strip * sizeof(uint16_t));
        
        int vert_indices = 0;
        vector<uint16_t>vertex_indices;

        // goes through each 16bit int value, takes it's value and the 2 following values and stores them in a list
        for (uint16_t k = 0; k < current_tstrip.verts_in_strip-2;k++)
        {
            uint16_t f1 = strip[k + 0];
            uint16_t f2 = strip[k + 1];
            uint16_t f3 = strip[k + 2];

            if (f1 != f2 && f1 != f3 && f2 != f3)
            {
                if (k & 1)
                {
                    vertex_indices.push_back(f1);
                    vert_indices++;
                    vertex_indices.push_back(f2);
                    vert_indices++;
                    vertex_indices.push_back(f3);
                    vert_indices++;
                }
                else
                {
                    vertex_indices.push_back(f2);
                    vert_indices++;
                    vertex_indices.push_back(f1);
                    vert_indices++;
                    vertex_indices.push_back(f3);
                    vert_indices++;
                }
                
            }
            
        }
        
        if (include_bones)
        {
            if (current_gator_header.bone_count > 1)
            {
                primitives["attributes"]["JOINTS_0"] = bone_indices_index;
                primitives["attributes"]["WEIGHTS_0"] = weights_index;
            }
        }
        
        primitives["attributes"]["POSITION"] = vertex_position_index;
        primitives["attributes"]["NORMAL"] = normals_index;
        if (!texture_list.empty())
        {
            primitives["attributes"]["TEXCOORD_0"] = texcoord_index;
        }
        primitives["indices"] = indices_index+i;
        primitives["material"] = current_tstrip.material_index;
        
        nlohmann::json index_accessor;
        index_accessor["bufferView"] = buffer_view_count;
        index_accessor["componentType"] = 5123;
        index_accessor["count"] = vert_indices;
        index_accessor["type"] = "SCALAR";
        gltf_data["accessors"].push_back(index_accessor);
        accessor_count++;

        long long current_bin_size = bin_file.tellp();
        bin_file.write(reinterpret_cast<const char*>(vertex_indices.data()),vertex_indices.size() * sizeof(uint16_t));
    
        nlohmann::json indices_buffer_views;
        indices_buffer_views["byteLength"] = vert_indices * sizeof(uint16_t);
        indices_buffer_views["buffer"] = 0;
        indices_buffer_views["byteOffset"] = current_bin_size;
        indices_buffer_views["target"] = 34963;
        gltf_data["bufferViews"].push_back(indices_buffer_views);
        meshes["primitives"].push_back(primitives);
        buffer_view_count++;
        
        last_verts_amount += current_tstrip.verts_in_strip;
    }
    
    current_bin_size = bin_file.tellp();
    bin_file.seekp(0, ios::end);
    current_bin_size = bin_file.tellp();
    
    nlohmann::json buffer;
    buffer["byteLength"] = current_bin_size;
    buffer["uri"] = current_filename + ".bin";
    gltf_data["buffers"].push_back(buffer);
    
    if (!texture_list.empty())
    {
        nlohmann::json sampler;
        sampler["magFilter"] = 9729;
        sampler["minFilter"] = 9729;
        gltf_data["samplers"].push_back(sampler);
        for (size_t i = 0; i < texture_list.size(); i++)
        {
            last_dot = texture_list[i].find_last_of('.');
            
            nlohmann::json texture;
            texture["sampler"]=0;
            texture["source"]= i;
            gltf_data["textures"].push_back(texture);
            
            nlohmann::json image;
            image["mimeType"] = "image/png";
            image["name"] = texture_list[i].substr(0, last_dot);
            image["uri"] = texture_list[i].substr(0, last_dot)+".png";
            gltf_data["images"].push_back(image);
        }
    }
    
    // loops through each material
    for (uint32_t i = 0; i < current_gator_header.material_count; i++)
    {
        material_info current_material;
        src_file.clear();
        src_file.seekg(current_gator_header.materials_offset + (sizeof(material_info) * i), ios::beg);
        src_file.read(reinterpret_cast<char*>(&current_material), sizeof(material_info));

        nlohmann::json material;
        material["alphaMode"] = "MASK";
        material["doubleSided"] = false;
        material["name"] = "Material_"+to_string(i);
        material["pbrMetallicRoughness"]["metallicFactor"] = 0;
        material["pbrMetallicRoughness"]["roughnessFactor"] = 1;
        // looks for texture in texture list and applies it's index to the material
        if (current_material.texture_name_index >= 0)
        {
            for (size_t j = 0; j < texture_list.size(); j++)
            {
                if (string_list[current_material.texture_name_index] == texture_list[j])
                {
                    material["pbrMetallicRoughness"]["baseColorTexture"]["index"] = j;
                    break;
                }
            }
        }
        if (current_material.normal_map_index >= 0)
        {
            for (size_t j = 0; j < texture_list.size(); j++)
            {
                if (string_list[current_material.normal_map_index] == texture_list[j])
                {
                    material["normalTexture"]["index"] = j;
                    break;
                }
            }
        }
        
        gltf_data["materials"].push_back(material);
    }
    gltf_data["meshes"].push_back(meshes);
    
    // converts the hitbox into a model
    if (convert_level_bsp && !current_gator_header.hitbox_points && current_gator_header.hitbox_data_offset != current_gator_header.bones_table)
    {
        bsp_header current_bsp_header;
        src_file.seekg(current_gator_header.hitbox_data_offset, ios::beg);
        src_file.read(reinterpret_cast<char*>(&current_bsp_header), sizeof(bsp_header));
        
        vector<char> bsp_content(current_bsp_header.file_size);
        src_file.seekg(current_gator_header.hitbox_data_offset, ios::beg);
        src_file.read(bsp_content.data(), bsp_content.size());
        
        ofstream hitbox_file(combined_output_path+"\\"+current_filename + " collision.bsp",ios::binary | ios::trunc);
        hitbox_file.write(bsp_content.data(), bsp_content.size());
        hitbox_file.close();
        
        string bsp_path = combined_output_path+"\\"+current_filename + " collision.bsp";
        string bsp_name = current_filename;
        bsp_converter(bsp_path, bsp_name);
    }
    
    
    #ifdef _DEBUG
    gltf_file << setw(4) << gltf_data;
    
    #else
    gltf_file << gltf_data;
    
    #endif

    src_file.close();
    gltf_file.close();
    bin_file.close();
    return texture_list;
}


void m_extractor_loop()
{
    // textbox for the output path
    if (ImGui::InputText("output path", global_output_path, IM_ARRAYSIZE(global_output_path)))
    {
        // each interaction with the textbox, it checks if the provided text is a valid path
        valid_folders = folder_validation(global_output_path);
        combined_output_path = global_output_path;
        if (!combined_output_path.ends_with("\\"))combined_output_path+="\\";
    }

    if (valid_folders)
    {
        ImGui::TextColored(ImVec4(0,1,0,1),"Valid folders");
    }
    else
    {
        ImGui::TextColored(ImVec4(1,0,0,1),"Invalid folders");
    }
}
