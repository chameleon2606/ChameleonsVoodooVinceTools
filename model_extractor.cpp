#include <iostream>
#include <fstream>
#include <array>
#include <imgui.h>
#include <string>
#include <filesystem>
#include <typeinfo>
#include "model_extractor.h"
#include <thread>
#include "GLFW/glfw3.h"
#include "main_window.h"
#include "include/json.hpp"

using namespace std;

string path = filesystem::current_path().string()+"\\models\\";

bool valid_output_path;
char gator_files_folder[128] = "C:\\Users\\leong\\Desktop\\vince stuff\\output";
bool valid_folders;
static constexpr uint32_t vert_header_size = 48;
static float uv_scale = 6.0f;
string error_message;
constexpr uint16_t bone_info_size = 288;
constexpr uint8_t vertex_pos_index = 0;
constexpr uint8_t normals_index = 1;
constexpr uint8_t uvs_index = 2;
constexpr uint8_t vertex_indices_index = 1;
const vector<float>default_matrix = {1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0};
struct gator_header
{
    char magic[4];
    uint32_t version, thing, thing2, vert_count, tri_count, tstrip_count, material_count, bone_count;
    int16_t thing5, thing6;
    uint16_t thing7, thing8, table_4_entries, string_count;
    uint32_t start_of_verts, start_of_tris, tstrip_table, materials_offset, hitbox_data_offset, bones_table, bone_joints_offset, string_offsets, string_lookup_table;
    float thing10, thing11, thing12, thing13, thing14, thing15, thing16, thing17, thing18, thing19, thing20;
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
    uint16_t u_value2[13];
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
    int16_t index, parent_index;
    uint32_t padding;
    float value_x, value_y, value_z;
    float rot_x, rot_y, rot_z;
    float bind_matrix[4][4];
    float inverse_bind_matrix[4][4];
    float bind_matrix_2[4][4];
    float inverse_bind_matrix_2[4][4];
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
    
    string gator_string = gator_files_folder;
    size_t last_slash = current_filepath.find_last_of('\\');
    string current_filename = current_filepath.substr(last_slash+1, current_filepath.size()-last_slash);
    size_t last_dot = current_filename.find_last_of('.');
    current_filename = current_filename.substr(0,last_dot);

    // creates a new .obj file with the name of the gator file
    ifstream src_file(current_filepath, ios::binary);                                                      // input .gator file
    ofstream new_obj_file(combined_output_path + "\\" + current_filename + ".obj", ios::trunc);        // output .obj file
    new_obj_file << "mtllib " << current_filename << ".mtl\n\n";
    ofstream new_mtl_file(combined_output_path + "\\" + current_filename + ".mtl", ios::trunc);        // output material file
    ofstream gltf_file(combined_output_path+"\\"+current_filename + ".gltf", ios::trunc);
    ofstream bin_file(combined_output_path+"\\"+current_filename + ".bin", ios::binary | ios::trunc);

    nlohmann::json gltf_data;
    gltf_data["nodes"] = nlohmann::json::array();
    
    nlohmann::json asset;
    gltf_data["asset"]["version"] = "2.0";
    
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
    if (current_gator_header.bone_count > 1)
    {
        scene["nodes"] = {0,1};
    }
    else
    {
        scene["nodes"] = {0};
    }
    gltf_data["scenes"].push_back(scene);

    uint32_t current_bin_size = 0;

    // collects bone data
    
    // loops through bones to collect all parents data
    vector<int16_t> bone_parent_list;
    for (uint32_t i = 0; i < current_gator_header.bone_count; i++)
    {
        bones_data current_bone;
        src_file.clear();
        src_file.seekg(current_gator_header.bones_table + (bone_info_size * i), ios::beg);
        src_file.read(reinterpret_cast<char*>(&current_bone), sizeof(bones_data));
        
        bone_parent_list.push_back(current_bone.parent_index);
    }

    // loops though bones again to collect all bone data
    vector<float>bind_matrix_list;
    for (uint32_t i = 0; i < current_gator_header.bone_count; i++)
    {
        vector<float>pose_positions;
        
        bones_data current_bone;
        
        // go back and read data into the "current_bone" struct
        src_file.clear();
        src_file.seekg(current_gator_header.bones_table + (bone_info_size * i), ios::beg);
        src_file.read(reinterpret_cast<char*>(&current_bone), sizeof(bones_data));
        
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                // matrix list for binary data
                bind_matrix_list.push_back(current_bone.inverse_bind_matrix[row][col]);
                // matrix list for gltf file
                pose_positions.push_back(current_bone.bind_matrix[row][col]);
            }
        }
            
        nlohmann::json bone;
        bone["name"] = string_list[current_bone.index];
        if (pose_positions != default_matrix)
        {
            bone["matrix"] = pose_positions;
        }
        vector<int16_t> bone_children_list;
        if (string_list[current_bone.index] == "identity")
        {
            bone["mesh"] = 0;
            if (current_gator_header.bone_count > 1)
            {
                bone["skin"] = 0;
            }
        }
        for (uint32_t k = 0; k < bone_parent_list.size(); k++)
        {
            if (bone_parent_list[k] == current_bone.index)
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
    rig_accessor["count"] = current_gator_header.bone_count;
    rig_accessor["type"] = "MAT4";
    gltf_data["accessors"].push_back(rig_accessor);
    accessor_count++;

    current_bin_size = bin_file.tellp();
    bin_file.write(reinterpret_cast<const char*>(bind_matrix_list.data()),bind_matrix_list.size() * sizeof(float));
    
    nlohmann::json rig_buffer_views;
    rig_buffer_views["byteLength"] = current_gator_header.bone_count*16*sizeof(float);
    rig_buffer_views["buffer"] = 0;
    rig_buffer_views["byteOffset"] = current_bin_size;
    gltf_data["bufferViews"].push_back(rig_buffer_views);
    buffer_view_count++;
        
    vector<int16_t> bone_joints_list;
    for (uint32_t i = 0; i < current_gator_header.bone_count; i++)
    {
        bone_joints_list.push_back(i);
    }

    if (current_gator_header.bone_count > 1)
    {
        nlohmann::json skin;
        skin["inverseBindMatrices"] = 0;
        skin["joints"] = bone_joints_list;
        gltf_data["skins"].push_back(skin);
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

    float max_x = 0, min_x = 0, max_y = 0, min_y = 0, max_z = 0, min_z = 0;

    vector<uint8_t>joints;
    vector<float>weights;
    vector<float>vertex_positions;
    vector<float>normals;
    vector<float>texcoords;

    // loop through each vertex data section
    for (uint32_t i=0; i < current_gator_header.vert_count; i++)
    {
        src_file.seekg(current_gator_header.start_of_verts +(sizeof(vert_info)*i), ios::beg);
        
        vert_info current_verts;

        // grabs all data for that vertex
        src_file.read(reinterpret_cast<char*>(&current_verts), sizeof(vert_info));

        joints.push_back(current_verts.bone1_index);
        joints.push_back(current_verts.bone2_index);
        joints.push_back(current_verts.bone3_index);
        joints.push_back(current_verts.bone4_index);

        weights.push_back((100.0f/255.0f)*(current_verts.bone1_weight/100.0f));
        weights.push_back((100.0f/255.0f)*(current_verts.bone2_weight/100.0f));
        weights.push_back((100.0f/255.0f)*(current_verts.bone3_weight/100.0f));
        weights.push_back((100.0f/255.0f)*(current_verts.bone4_weight/100.0f));

        vertex_positions.push_back(current_verts.x_pos*-1);
        vertex_positions.push_back(current_verts.y_pos);
        vertex_positions.push_back(current_verts.z_pos);

        normals.push_back(current_verts.x_norm*-1);
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
        if (use_uv2)
        {
            // multiplies the UVs times 6, so they appear correctly
            current_verts.x_uv2 *= uv_scale;
            current_verts.y_uv2 *= uv_scale;
            uvs.push_back({current_verts.x_uv2, (current_verts.y_uv2 * -1) + 1});   // flipps the UV upside down

            src_file.seekg(current_gator_header.start_of_verts +(sizeof(vert_info)*i) + 56, ios::beg);
            vector<char>tmp_uv_buffer(2*sizeof(float));
            src_file.read(tmp_uv_buffer.data(), tmp_uv_buffer.size());
            copy(tmp_uv_buffer.begin(), tmp_uv_buffer.end(), back_inserter(uv_buffer));
        }
        else
        {
            current_verts.x_uv1 *= uv_scale;
            current_verts.y_uv1 *= uv_scale;
            uvs.push_back({current_verts.x_uv1, (current_verts.y_uv1 * -1) + 1});

            src_file.seekg(current_gator_header.start_of_verts +(sizeof(vert_info)*i) + 48, ios::beg);
            vector<char>tmp_uv_buffer(2*sizeof(float));
            src_file.read(tmp_uv_buffer.data(), tmp_uv_buffer.size());
            copy(tmp_uv_buffer.begin(), tmp_uv_buffer.end(), back_inserter(uv_buffer));
        }

        //writes x, y and z position of vertex into the .obj file and flips the x-axis
        new_obj_file << "v " << current_verts.x_pos*-1 << " " << current_verts.y_pos << " " << current_verts.z_pos << "\n";

        // the gltf file wants min and max position values
        if (current_verts.x_pos*-1 > max_x)max_x = current_verts.x_pos*-1;
        else if (current_verts.x_pos*-1 < min_x)min_x = current_verts.x_pos*-1;
        if (current_verts.y_pos > max_y)max_y = current_verts.y_pos;
        else if (current_verts.y_pos < min_y)min_y = current_verts.y_pos;
        if (current_verts.z_pos > max_z)max_z = current_verts.z_pos;
        else if (current_verts.z_pos < min_z)min_z = current_verts.z_pos;
    }

    current_bin_size = bin_file.tellp();

    if (current_gator_header.bone_count > 1)
    {
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
        bin_file.write(reinterpret_cast<const char*>(joints.data()),joints.size() * sizeof(uint8_t));
        buffer_view_count++;

        // WEIGHTS
        nlohmann::json weights_accessor;
        weights_accessor["bufferView"] = buffer_view_count;
        weights_accessor["componentType"] = 5126;
        weights_accessor["count"] = current_gator_header.vert_count;
        weights_accessor["type"] = "VEC4";
        gltf_data["accessors"].push_back(weights_accessor);
        weights_index = accessor_count;
        accessor_count++;

        current_bin_size = bin_file.tellp();

        nlohmann::json weights_buffer_views;
        weights_buffer_views["byteLength"] = current_gator_header.vert_count*4*sizeof(float);
        weights_buffer_views["buffer"] = 0;
        weights_buffer_views["byteOffset"] = current_bin_size;
        weights_buffer_views["target"] = 34962;
        gltf_data["bufferViews"].push_back(weights_buffer_views);
        bin_file.write(reinterpret_cast<const char*>(weights.data()),weights.size() * sizeof(float));
        buffer_view_count++;
    }

    // POSITIONS
    current_bin_size = bin_file.tellp();
    nlohmann::json vertex_pos_accessor;
    vertex_pos_accessor["bufferView"] = buffer_view_count;
    vertex_pos_accessor["componentType"] = 5126;
    vertex_pos_accessor["count"] = current_gator_header.vert_count;
    vertex_pos_accessor["max"] = {max_x,max_y,max_z};
    vertex_pos_accessor["min"] = {min_x,min_y,min_z};
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
    
    // loops through the UV list and writes them into the .obj file
    for (auto& uv : uvs)
    {
        new_obj_file << "vt " << uv[0] << " " << uv[1] << "\n";
        
    }
    // loops through the normals list and writes them into the .obj file
    for (auto& normal : norms)
    {
        // x-axis is flipped, because the x-axis of the vertex position was flipped
        new_obj_file << "vn " << normal[0]*-1 << " " << normal[1] << " " << normal[2] << "\n";
    }

    
    // reference to the next offset of vert strips
    int last_verts_amount = 0;
    vector<char>new_idx;
    
    nlohmann::json meshes;
    meshes["name"] = current_filename;
    nlohmann::json primitives;

    indices_index = accessor_count;
    
    // loops through the face strips and writes the tstrips
    for (uint32_t i = 0; i < current_gator_header.tstrip_count; i++)
    {
        vector<char>indices_buffer;
        vector<char> fixed_idx;
        
        // seek to the beginning of the next tstrip table
        src_file.clear();
        src_file.seekg(current_gator_header.tstrip_table + (vert_header_size * i), ios::beg);
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
        
        // creates vertex groups for the .obj file
        new_obj_file << "\ng " << current_tstrip.verts_in_strip << "\nusemtl Material" << current_tstrip.material_index << "\n\n";
        
        int vert_indices = 0;
        vector<uint16_t>vertex_indices;

        // goes through each 16bit int value, takes it's value and the 2 following values and stores them in a list
        for (uint16_t k = 0; k < current_tstrip.verts_in_strip-2;k++)
        {
            uint16_t f1 = strip[k + 0];
            uint16_t f2 = strip[k + 1];
            uint16_t f3 = strip[k + 2];
            
            if (k & 1)
            {
                new_obj_file << "f " << f1+1 << "/" << f1+1 << "/" << f1+1 << " " << f2+1 << "/" << f2+1 << "/" << f2+1 << " " << f3+1 << "/" << f3+1 << "/" << f3+1 << "\n";
                vertex_indices.push_back(f1);
                vertex_indices.push_back(f2);
                vertex_indices.push_back(f3);
            }
            else
            {
                new_obj_file << "f " << f2+1 << "/" << f2+1 << "/" << f2+1 << " " << f1+1 << "/" << f1+1 << "/" << f1+1 << " " << f3+1 << "/" << f3+1 << "/" << f3+1 << "\n";
                vertex_indices.push_back(f2);
                vertex_indices.push_back(f1);
                vertex_indices.push_back(f3);
            }
            vert_indices+=3;
        }

        if (current_gator_header.bone_count > 1)
        {
            primitives["attributes"]["JOINTS_0"] = bone_indices_index;
            primitives["attributes"]["WEIGHTS_0"] = weights_index;
        }
        primitives["attributes"]["POSITION"] = vertex_position_index;
        primitives["attributes"]["NORMAL"] = normals_index;
        primitives["attributes"]["TEXCOORD_0"] = texcoord_index;
        
        nlohmann::json index_accessor;
        index_accessor["bufferView"] = buffer_view_count;
        index_accessor["componentType"] = 5123;
        index_accessor["count"] = vert_indices;
        index_accessor["type"] = "SCALAR";
        gltf_data["accessors"].push_back(index_accessor);
        accessor_count++;
        primitives["indices"] = indices_index+i;
        primitives["material"] = current_tstrip.material_index;

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

    if (current_filename == "grateexit_base")
    {
        cout << "last verts = " << last_verts_amount << "\n";
    }
    
    current_bin_size = bin_file.tellp();
    bin_file.seekp(0, ios::end);
    current_bin_size = bin_file.tellp();
    
    nlohmann::json buffer;
    buffer["byteLength"] = current_bin_size;
    buffer["uri"] = current_filename + ".bin";
    gltf_data["buffers"].push_back(buffer);

    nlohmann::json sampler;
    sampler["magFilter"] = 9728;
    sampler["minFilter"] = 9728;
    gltf_data["samplers"].push_back(sampler);
    
    if (!texture_list.empty())
    {        
        for (size_t i = 0; i < texture_list.size(); i++)
        {
            last_dot = texture_list[i].find_last_of('.');
            
            if (current_filename == "curio_base")
            {
                cout << texture_list[i] << "\n";
            }
            
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
        
        gltf_data["materials"].push_back(material);

        if (current_material.texture_name_index > -1)
        {
            size_t last_dot = string_list[current_material.texture_name_index].find_last_of('.');
            
            // collects diffuse maps
            new_mtl_file << "newmtl Material" << i << "\nmap_Kd " << string_list[current_material.texture_name_index].substr(0, last_dot) << ".png\n";
            // assigns alpha texture
            new_mtl_file << "map_d " << string_list[current_material.texture_name_index].substr(0, last_dot) << ".png\n";
        }
        // checks if the material has a normal map
        if (current_material.normal_map_index > -1)
        {
            size_t last_dot = string_list[current_material.normal_map_index].find_last_of('.');
            new_mtl_file << "bump " << string_list[current_material.normal_map_index].substr(0, last_dot) << ".png\n\n";
        }
        else
        {
            new_mtl_file << "\n";
        }
    }
    gltf_data["meshes"].push_back(meshes);
    

    gltf_file << setw(4) << gltf_data;

    src_file.close();
    new_mtl_file.close();
    new_obj_file.close();
    gltf_file.close();
    bin_file.close();
    return texture_list;
}


void m_extractor_loop()
{
    // textbox for the output path
    if (ImGui::InputText("gator files folder", gator_files_folder, IM_ARRAYSIZE(gator_files_folder)) || ImGui::InputText("output path", global_output_path, IM_ARRAYSIZE(global_output_path)))
    {
        // each interaction with the textbox, it checks if the provided text is a valid path
        valid_folders = folder_validation(gator_files_folder) && folder_validation(global_output_path);
        combined_output_path = global_output_path;
        if (!combined_output_path.ends_with("\\"))combined_output_path+="\\";
    }
    if (ImGui::TreeNodeEx("Options"))
    {
        ImGui::SetItemTooltip("If the UV doesn't appear correctly, try checking this box");
        ImGui::Checkbox("Use alternative UV", &use_uv2);
        ImGui::Checkbox("Convert row-major bind matrix to column-major", &matrix_convert);
        ImGui::SetItemTooltip("DirectX uses row-major matrices, OpenGL (blender) uses col-major");
        ImGui::TreePop();
    }

    if (valid_folders)
    {
        ImGui::TextColored(ImVec4(0,1,0,1),"Valid folders");
        ImGui::BeginChild("Gator files");
        for (auto& dir_entry : filesystem::directory_iterator(gator_files_folder))
        {
            if (!dir_entry.path().filename().string().ends_with(".gator"))
            {
                continue;
            }
            string filename = dir_entry.path().filename().string();
            if (ImGui::Selectable(filename.c_str()))
            {
                std::thread taskThread(extract_model, dir_entry.path().string());    // we don't pass the string as a reference, cause the name could change while extracting
                taskThread.join();
            }
        }
        ImGui::EndChild();
    }
    else
    {
        ImGui::TextColored(ImVec4(1,0,0,1),"Invalid folders");
    }
}

