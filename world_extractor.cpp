#include "world_extractor.h"
#include <iostream>
#include "imgui.h"
#include <filesystem>
#include <fstream>
#include <array>
#include <thread>
#include "include/json.hpp"

#include "GLFW/glfw3.h"

using namespace std;
char vertices_path[128] = "C:\\Users\\leong\\Desktop\\vince stuff\\output\\vertices.raw";
char index_path[128] = "C:\\Users\\leong\\Desktop\\vince stuff\\output\\index.json";
size_t remember_position = 0;
int32_t last_object = -1;
string last_filename = "";
string last_mtl = "";

void extract_world()
{
    ifstream index_file(index_path);
    nlohmann::json jsonData = nlohmann::json::parse(index_file);
    ifstream vertices_file(vertices_path, ios::binary);
    struct vertex_info
    {
        float x_pos, y_pos, z_pos;
        float x_norm, y_norm, z_norm;
        float values_0[7];
        float x_uv1, y_uv1, x_uv2, y_uv2;
        float values_1[2];
    };
    struct fld_header
    {
        char id[4];
        int version;
        uint32_t last_header;
        int16_t values[2];
        int16_t first_section;
        int16_t values2[3];
        uint32_t some_offset1;
        uint32_t string_indices_offset;
    };
    struct fld_info
    {
        int values0[5];
        int material_index;
        int section_variant;
        int strip_indices_amount;
        int other_data_amount;
        short values2[4];
        float pos_x, pos_y, pos_z;
        int values3[7];
        int header_size;
        int vertex_indices_bytes;
        int raw_file_offset;
        uint32_t raw_file_offset_offset;
        int raw_u_offset;
        short string_index_offset;
        short values4;
        float f_values[12];
        int object_id;
    };

    for (auto &zone : jsonData["Zones"].items())
    {
        string fld_path = "C:\\Users\\leong\\Desktop\\vince stuff\\output\\zone000"+to_string(zone.value())+".fld";
        cout << "now extracting " << zone.key() << "\n";
        cout << "fld_path: "<<fld_path << "\n";
        ifstream fld_file(fld_path, ios::binary);
        fld_file.clear();
        fld_file.seekg(0, ios::beg);
        
        fld_header current_flud_header;
        fld_info section_header;
        
        fld_file.read(reinterpret_cast<char*>(&current_flud_header), sizeof(fld_header));
        string path = "C:\\Users\\leong\\Desktop\\vince stuff\\output\\"+zone.key();
        float buffer_data = 0;
        uint32_t pos = 0;
        uint32_t vert_indices = 0;
        uint32_t vert_index_reference = 0;
        
        ofstream obj_file(path+".obj");
        ofstream mtl_file(path+".mtl");

        vector<string>material_list;
        while (pos < current_flud_header.string_indices_offset)
        {
            fld_file.read(reinterpret_cast<char*>(&section_header),sizeof(fld_info));
            
            if (section_header.section_variant == 2)
            {
                if (section_header.object_id > last_object)
                {
                    last_object = section_header.object_id;
                    obj_file << "g " << section_header.object_id << "\n";
                }
            
                if (section_header.material_index > 0)
                {
                    size_t last_dot = to_string(jsonData["Materials"][to_string(section_header.material_index)]["base"]).find_last_of('.');
                    string mtl = to_string(jsonData["Materials"][to_string(section_header.material_index)]["base"]).substr(1,last_dot-1);
                        
                    remember_position = fld_file.tellg();
                            
                    fld_file.seekg(current_flud_header.string_indices_offset + section_header.string_index_offset, ios::beg);
                    uint32_t offset;
                    fld_file.read(reinterpret_cast<char*>(&offset), sizeof(uint32_t));
                    fld_file.seekg(current_flud_header.string_indices_offset+offset, ios::beg);
                    char c;
                    string filename;
                    while (fld_file.read(&c, 1)&& c != '\0')
                    {
                        filename += c;
                    }
                    if (last_filename != filename)
                    {
                        obj_file << "o " << mtl << "\n";
                        last_filename = filename;
                    }
                    if (last_mtl != mtl)
                    {
                        obj_file << "usemtl " << mtl << "\n";
                        last_mtl = mtl;
                    }

                    fld_file.clear();
                    fld_file.seekg(remember_position, ios::beg);
                        
                    bool found = any_of(material_list.begin(), material_list.end(),[&mtl](const std::string& s)
                    {
                        return s == mtl;
                    });
                    if (!found)
                    {
                        if (!mtl.ends_with("ull"))
                        {
                            mtl_file << "newmtl " << mtl << "\n";
                            mtl_file << "map_Kd " << mtl << ".png" << "\n";
                            mtl_file << "map_d " << mtl << ".png" << "\n";
                        }
                        else
                        {
                            mtl_file << "newmtl Null" << "\n";
                        }
                        material_list.push_back(mtl);
                    }
                }
                    vector<array<float, 3>> norms;
                    vector<array<float, 2>> uvs;
                    vector<uint16_t>strip_buffer(section_header.strip_indices_amount);
                    
                    buffer_data = section_header.strip_indices_amount * 2.0;
                    
                    fld_file.read(reinterpret_cast<char*>(strip_buffer.data()),strip_buffer.size()*sizeof(uint16_t));
                    
                    vertices_file.seekg(section_header.raw_file_offset, ios::beg);
                    
                    for (size_t i = 0; i < section_header.raw_file_offset_offset / sizeof(vertex_info); i++)
                    {
                        vertex_info current_vertex;
                        vertices_file.read(reinterpret_cast<char*>(&current_vertex), sizeof(vertex_info));
                        norms.push_back({current_vertex.x_norm, current_vertex.y_norm, current_vertex.z_norm});
                        uvs.push_back({current_vertex.x_uv1*=16, current_vertex.y_uv1*=16});
                        obj_file << "v " << -current_vertex.x_pos << " " << current_vertex.y_pos << " " << current_vertex.z_pos << "\n";
                        vert_indices++;
                    }
                    for (auto& uv : uvs)
                    {
                        obj_file << "vt " << uv[0] << " " << uv[1]*-1 << "\n";
                    }
                    for (auto& norm : norms)
                    {
                        obj_file << "vn " << norm[0] << " " << norm[1] << " " << norm[2] << "\n";
                    }
                    
                    for (uint16_t k = 0; k < strip_buffer.size()-2;k++)
                    {
                        uint16_t f1 = strip_buffer[k + 0] + 1;
                        uint16_t f2 = strip_buffer[k + 1] + 1;
                        uint16_t f3 = strip_buffer[k + 2] + 1;
                        if (k & 1)
                        {
                            obj_file << "f " 
                                        << f1 + vert_index_reference << "/" << f1+vert_index_reference << "/" << f1+vert_index_reference << " "
                                        << f2 + vert_index_reference << "/" << f2+vert_index_reference << "/" << f2+vert_index_reference << " "
                                        << f3 + vert_index_reference << "/" << f3+vert_index_reference << "/" << f3+vert_index_reference << "\n";
                        }
                        else
                        {
                            obj_file << "f " 
                                        << f2 + vert_index_reference << "/" << f2+vert_index_reference << "/" << f2+vert_index_reference << " "
                                        << f1 + vert_index_reference << "/" << f1+vert_index_reference << "/" << f1+vert_index_reference << " "
                                        << f3 + vert_index_reference << "/" << f3+vert_index_reference << "/" << f3+vert_index_reference << "\n";
                        }
                    }
                    vert_index_reference = vert_indices;
                
                int remainder = (ceil(buffer_data / 16) * 16) - buffer_data;
                if (remainder > 0)fld_file.seekg(remainder, ios::cur);
            }
            
            else if (section_header.section_variant == 0 || section_header.section_variant == 3)
            {
                if (section_header.object_id || section_header.other_data_amount)
                {
                    buffer_data = section_header.other_data_amount * 4.0;
                    vector<uint32_t>u_buffer(section_header.other_data_amount);
                    fld_file.read(reinterpret_cast<char*>(u_buffer.data()),u_buffer.size()*sizeof(uint32_t));
                    
                    int remainder = (ceil(buffer_data / 16) * 16) - buffer_data;
                    if (remainder > 0)fld_file.seekg(remainder, ios::cur);
                }
            }
            
            pos = fld_file.tellg();
            if (pos%16!=0)
            {
                cout << pos << " pos error!" << "\n";
                exit(0);
            }
        }
        fld_file.close();
        mtl_file.close();
        obj_file.close();
    }
    vertices_file.close();
}

void world_extractor_loop()
{
    ImGui::InputText("input path", vertices_path, sizeof(vertices_path));
    if (ImGui::Button("extract"))
    {
        extract_world();
    }
}