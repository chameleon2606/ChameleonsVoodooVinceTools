#include <iostream>
#include <fstream>
#include <string>
#include <array>
#include <imgui.h>
#include <filesystem>
#include <typeinfo>
#include "model_extractor.h"

#include "imgui_internal.h"
#include "main_window.h"

#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"

using namespace std;

string path = filesystem::current_path().string()+"\\models\\";
char output_path[128];
bool valid_output_path;
char gator_files_folder[128];
bool valid_folders;
static constexpr uint32_t vert_header_size = 48;
static float uv_scale = 6.0f;
string error_message;
constexpr uint16_t bone_info_size = 288;
constexpr float maxfloat = 3.40282e+38;

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
    string current_path = filesystem::current_path().string() + "\\output\\";
    strncpy_s(output_path, current_path.c_str(), sizeof(output_path) - 1);
    output_path[sizeof(output_path) - 1] = '\0'; // Ensure null termination
    valid_folders = filesystem::exists(output_path) && filesystem::exists(gator_files_folder);
}


auto open_file(const string& file_path, const string& file_name)
{
    string extention;
    if (!file_path.ends_with("\\"))
    {
        extention = "\\";
        ifstream src_file(file_path + "\\" + file_name, ios::binary);
    }

    // creates a new .obj file with the name of the gator file
    size_t last_dot = file_name.find_last_of('.');
    string new_object_name;
    if (last_dot != string::npos)
    {
        new_object_name = file_name.substr(0, last_dot) + ".obj";
    }
    
    ifstream src_file(file_path + extention + file_name, ios::binary);          // input .gator file
    ofstream new_obj_file(path + new_object_name, ios::trunc);                  // output .obj file
    ofstream bone_data(path + new_object_name + "_bone_data.txt", ios::trunc);  // output info file
    
    struct gator_header
    {
        char magic[4];
        uint32_t version, thing, thing2, vert_count, tri_count, tstrip_count, table_2_entries, bone_count, extra_object_count; 
        uint16_t thing7, thing8, table_4_entries, string_count;
        uint32_t start_of_verts, start_of_tris, tstrip_table, table_2_offset, table_3_offset, bones_table, table_4_offset, string_offsets, start_of_string_table;
        float thing10, thing11, thing12, thing13, thing14, thing15, thing16, thing17, thing18, thing19, thing20;
    };
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
    vector<string> texturelist;

    
    for (auto& offset : string_offsets)
    {
        string texturename;
        char c;
        src_file.seekg(current_gator_header.start_of_string_table + offset, ios::beg);
        while (src_file.read(&c, 1)&& c != '\0')
        {
            texturename += c;
        }
        if (texturename.ends_with(".dds"))
        {
            bone_data << texturename << endl;
        }
        texturelist.push_back(texturename);    
    }
    
    src_file.seekg(current_gator_header.start_of_verts, ios::beg);
    
    vector<array<float, 3>> norms;
    vector<array<float, 2>> uvs;

    // loop through each vertex data section
    for (uint32_t i=0; i < current_gator_header.vert_count; i++)
    {
        struct vert_info
        {
            float x_pos, y_pos, z_pos;
            uint32_t thing1, thing2;
            float x_norm, y_norm, z_norm, x_unknown, y_unknown, z_unknown;
            uint32_t thing3;
            float x_uv1, y_uv1, x_uv2, y_uv2;
        };
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
    
    src_file.seekg(current_gator_header.tstrip_table, ios::beg);
    vector<uint32_t> strip_list;
    // loops through the face strips and takes the first value in the table
    for (uint32_t i = 0; i < current_gator_header.tstrip_count; i++)
    {
        struct tstrip_info
        {
            uint16_t verts_in_strip;
            uint16_t u_value2[23];
        };
        tstrip_info current_tstrip;
        
        src_file.read(reinterpret_cast<char*>(&current_tstrip), sizeof(tstrip_info));
        strip_list.push_back(current_tstrip.verts_in_strip);
        src_file.seekg(current_gator_header.tstrip_table + (vert_header_size*(i+1)), ios::beg);
    }

    src_file.seekg(current_gator_header.start_of_tris, ios::beg);
    for (auto& face_count : strip_list)
    {
        vector<array<int, 3>> face_list;
        vector<uint16_t> strip(face_count);
        src_file.read(reinterpret_cast<char*>(strip.data()), face_count * sizeof(uint16_t));
        // goes through each 16bit int value, takes it's value and the 2 following values and stores them in a list
        for (uint32_t i = 0; i < face_count-2;i++)
        {
            int f1 = strip[i + 0];
            int f2 = strip[i + 1];
            int f3 = strip[i + 2];
    
            if (i & 1)
            {
                face_list.push_back({f1, f2, f3});
            }
            else
            {
                face_list.push_back({f2, f1, f3});
            }
        }
        // creates vertex groups for the .obj file
        new_obj_file << "\ng " << face_count << "\n\n";
        for (auto& tri : face_list)
        {
            new_obj_file << "f " << tri[0]+1 << "/" << tri[0]+1 << "/" << tri[0]+1 << " " << tri[1]+1 << "/" << tri[1]+1 << "/" << tri[1]+1 << " " << tri[2]+1 << "/" << tri[2]+1 << "/" << tri[2]+1 << "\n";
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
            bone_data << current_bones.index << " " << texturelist[current_bones.index] << " has no parent\n";
        }
        else
        {
            bone_data << current_bones.index << " " << texturelist[current_bones.index] << " connected to " << current_bones.parent_index << " " << texturelist[current_bones.parent_index] << "\n";
        }
        
        bone_data << "world position = X: " << current_bones.pos_x << " Y: " << abs(current_bones.pos_y) << " Z: " << current_bones.pos_z << "\n\n";
    }
    
    return texturelist;
}

void m_extractor_loop()
{
    if (ImGui::InputText("gator files folder", gator_files_folder, IM_ARRAYSIZE(gator_files_folder)) || ImGui::InputText("output path", output_path, IM_ARRAYSIZE(output_path)))
    {
        valid_folders = folder_validation(gator_files_folder) && folder_validation(output_path);
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
                vector<string> textures = open_file(gator_files_folder, filename);
            }
        }
    }
    else
    {
        ImGui::TextColored(ImVec4(1,0,0,1),"Invalid folders");
    }
    ImGui::EndChild();
}