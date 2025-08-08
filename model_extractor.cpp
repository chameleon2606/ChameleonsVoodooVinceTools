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

using namespace std;

string path = filesystem::current_path().string()+"\\models\\";

//char output_path[128];
bool valid_output_path;
char gator_files_folder[128] = "C:\\Users\\leong\\Desktop\\vince stuff\\output";
bool valid_folders;
static constexpr uint32_t vert_header_size = 48;
static float uv_scale = 6.0f;
string error_message;
constexpr uint16_t bone_info_size = 288;
constexpr float maxfloat = 3.40282e+38;
struct gator_header
{
    char magic[4];
    uint32_t version, thing, thing2, vert_count, tri_count, tstrip_count, material_count, bone_count;
    int16_t thing5, thing6;
    uint16_t thing7, thing8, table_4_entries, string_count;
    uint32_t start_of_verts, start_of_tris, tstrip_table, materials_offset, hitbox_data_offset, bones_table, table_4_offset, string_offsets, string_lookup_table;
    float thing10, thing11, thing12, thing13, thing14, thing15, thing16, thing17, thing18, thing19, thing20;
};
struct vert_info
{
    float x_pos, y_pos, z_pos;
    uint32_t thing1, thing2;
    float x_norm, y_norm, z_norm, x_unknown, y_unknown, z_unknown;
    uint32_t thing3;
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
    string gator_string = gator_files_folder;
    size_t last_slash = current_filepath.find_last_of('\\');
    string current_filename = current_filepath.substr(last_slash+1, current_filepath.size()-last_slash);
    size_t last_dot = current_filename.find_last_of('.');
    current_filename = current_filename.substr(0,last_dot);

    //string output_path_string = output_path;
    // creates a new .obj file with the name of the gator file
    //ifstream src_file(gator_string + "\\" + selected_filename, ios::binary);                                                                 // input .gator file
    ifstream src_file(current_filepath, ios::binary);                                                                                    // input .gator file
    ofstream new_obj_file(combined_output_path + "\\" + current_filename + ".obj", ios::trunc);        // output .obj file
    new_obj_file << "mtllib " << current_filename << ".mtl\n\n";
    ofstream new_mtl_file(combined_output_path + "\\" + current_filename + ".mtl", ios::trunc);        // output material file
    ofstream bone_data(combined_output_path+ "\\" + current_filename + "_bone_data.txt", ios::trunc);  // output info file
    
    gator_header current_gator_header;
    
    // reads the header (128 bits)
    src_file.read(reinterpret_cast<char*>(&current_gator_header), sizeof(gator_header));
    if (strncmp(current_gator_header.magic, "GATR", 4) != 0)
    {
        glfwTerminate();
    }
    
    src_file.seekg(current_gator_header.string_offsets, ios::beg);
    vector<uint32_t> string_offsets(current_gator_header.string_count);
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
    
    src_file.seekg(current_gator_header.start_of_verts, ios::beg);
    
    vector<array<float, 3>> norms;
    vector<array<float, 2>> uvs;
    
    // loop through each vertex data section
    for (uint32_t i=0; i < current_gator_header.vert_count; i++)
    {
        vert_info current_verts;

        // grabs all data for that vertex
        src_file.read(reinterpret_cast<char*>(&current_verts), sizeof(vert_info));

        // puts normal data into a list
        norms.push_back({current_verts.x_norm,current_verts.y_norm,current_verts.z_norm});

        // multiplies the UVs times 6, so they appear correctly
        current_verts.x_uv1 *= uv_scale;
        current_verts.y_uv1 *= uv_scale;

        // puts UVs into a list
        uvs.push_back({current_verts.x_uv1, (current_verts.y_uv1 * -1) + 1});   // the UV is flipped upside down

        //writes x, y and z position of vertex into the .obj file and flips the x-axis
        new_obj_file << "v " << current_verts.x_pos*-1 << " " << current_verts.y_pos << " " << current_verts.z_pos << "\n";
    }

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
    
    // loops through the face strips and writes the tstrips
    int last_verts_amount = 0;
    for (uint32_t i = 0; i < current_gator_header.tstrip_count; i++)
    {
        tstrip_info current_tstrip;

        // seek to the beginning of the next tstrip table
        src_file.clear();
        src_file.seekg(current_gator_header.tstrip_table + (vert_header_size * i), ios::beg);
        src_file.read(reinterpret_cast<char*>(&current_tstrip), sizeof(tstrip_info));

        src_file.clear();
        src_file.seekg(current_gator_header.start_of_tris + (last_verts_amount * sizeof(uint16_t)), ios::beg);
        vector<uint16_t> strip(current_tstrip.verts_in_strip);
        src_file.read(reinterpret_cast<char*>(strip.data()), current_tstrip.verts_in_strip * sizeof(uint16_t));

        // creates vertex groups for the .obj file
        new_obj_file << "\ng " << current_tstrip.verts_in_strip << "\nusemtl Material" << current_tstrip.material_index << "\n\n";
        
        // goes through each 16bit int value, takes it's value and the 2 following values and stores them in a list
        for (uint16_t k = 0; k < current_tstrip.verts_in_strip-2;k++)
        {
            uint16_t f1 = strip[k + 0];
            uint16_t f2 = strip[k + 1];
            uint16_t f3 = strip[k + 2];

            if (k & 1)
            {
                new_obj_file << "f " << f1+1 << "/" << f1+1 << "/" << f1+1 << " " << f2+1 << "/" << f2+1 << "/" << f2+1 << " " << f3+1 << "/" << f3+1 << "/" << f3+1 << "\n";
            }
            else
            {
                new_obj_file << "f " << f2+1 << "/" << f2+1 << "/" << f2+1 << " " << f1+1 << "/" << f1+1 << "/" << f1+1 << " " << f3+1 << "/" << f3+1 << "/" << f3+1 << "\n";
            }
        }
        last_verts_amount += current_tstrip.verts_in_strip;
    }
    
    for (uint32_t i = 0; i < current_gator_header.material_count; i++)
    {
        material_info current_material;
        src_file.clear();
        src_file.seekg(current_gator_header.materials_offset + (sizeof(material_info) * i), ios::beg);
        src_file.read(reinterpret_cast<char*>(&current_material), sizeof(material_info));
        
        if (current_material.texture_name_index > -1)
        {
            size_t last_dot = string_list[current_material.texture_name_index].find_last_of('.');
            new_mtl_file << "newmtl Material" << i << "\nmap_Kd " << string_list[current_material.texture_name_index].substr(0, last_dot) << ".png\n";
            new_mtl_file << "map_d " << string_list[current_material.texture_name_index].substr(0, last_dot) << ".png\n";
        }
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
    
    for (uint32_t i = 0; i < current_gator_header.bone_count; i++)
    {
        struct bones_data
        {
            int16_t index, parent_index;
            uint32_t padding;
            float value_x, value_y, value_z;
            float rot_x, rot_y, rot_z, rot_w;
            float values[11];
            float pos_x, pos_y, pos_z;
            float values2[48];
        };
        bones_data current_bones;
        src_file.seekg(current_gator_header.bones_table + (bone_info_size * i), ios::beg);
        src_file.read(reinterpret_cast<char*>(&current_bones), sizeof(bones_data));
        
        if (current_bones.parent_index < 0)
        {
            bone_data << current_bones.index << " " << string_list[current_bones.index] << " has no parent\n";
        }
        else
        {
            bone_data << current_bones.index << " " << string_list[current_bones.index] << " connected to " << current_bones.parent_index << " " << string_list[current_bones.parent_index] << "\n";
        }
        
        bone_data << "world position = X: " << current_bones.pos_x << " Y: " << abs(current_bones.pos_y) << " Z: " << current_bones.pos_z << "\n\n";
    }

    bone_data.close();
    src_file.close();
    new_mtl_file.close();
    new_obj_file.close();

    return texture_list;
}

void m_extractor_loop()
{
    if (ImGui::InputText("gator files folder", gator_files_folder, IM_ARRAYSIZE(gator_files_folder)) || ImGui::InputText("output path", global_output_path, IM_ARRAYSIZE(global_output_path)))
    {
        valid_folders = folder_validation(gator_files_folder) && folder_validation(global_output_path);
        combined_output_path = global_output_path;
        if (!combined_output_path.ends_with("\\"))combined_output_path+="\\";
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
                taskThread.detach();
            }
        }
        ImGui::EndChild();
    }
    else
    {
        ImGui::TextColored(ImVec4(1,0,0,1),"Invalid folders");
    }
}