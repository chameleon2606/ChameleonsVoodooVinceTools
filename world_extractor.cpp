#include "world_extractor.h"
#include <iostream>
#include "imgui.h"
#include <filesystem>
#include <fstream>
#include <array>

using namespace std;

char input_path[128] = "C:\\Users\\leong\\Desktop\\vince stuff\\output";
char fld_path[128] = "F:\\DotNetProjects\\OpenGLbackup\\output\\tstrip.bin";

void extract_world()
{
    ifstream input_file(input_path, ios::binary);
    ofstream output_file("C:\\Users\\leong\\Desktop\\vince stuff\\output\\world.obj", ios::binary);
    struct vertex_info
    {
        float x_pos, y_pos, z_pos;
        float value[66/sizeof(float)];
    };
    int iterations = filesystem::file_size(input_path) / sizeof(vertex_info)*4;
    vector<array<float, 3>> norms;
    vector<array<float, 2>> uvs;
    vector<float> normals;
    for (int i = 0; i < iterations; i++)
    {
        vertex_info current_vertex;
        input_file.read(reinterpret_cast<char*>(&current_vertex), sizeof(vertex_info));
        norms.push_back({current_vertex.value[3], current_vertex.value[4], current_vertex.value[5]});
        uvs.push_back({current_vertex.value[6], current_vertex.value[7]});
        output_file << "v " << current_vertex.x_pos << " " << current_vertex.y_pos << " " << current_vertex.z_pos << "\n";
    }
    for (auto& uv : uvs)
    {
        output_file << "vt " << uv[0] << " " << uv[1] << "\n";
    }
    for (auto& norm : norms)
    {
        output_file << "vn " << norm[0] << " " << norm[1] << " " << norm[2] << "\n";
    }
    ifstream fld_file(fld_path, ios::binary);

    
    vector<array<int, 3>> face_list;
    vector<uint16_t> strip(1896/2);
    fld_file.read(reinterpret_cast<char*>(strip.data()), 1896);
    // goes through each 16bit int value, takes it's value and the 2 following values and stores them in a list
    for (uint32_t i = 0; i < 1896/2-2;i++)
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
    for (auto& tri : face_list)
    {
        output_file << "f " << tri[0]+1 << "/" << tri[0]+1 << "/" << tri[0]+1 << " " << tri[1]+1 << "/" << tri[1]+1 << "/" << tri[1]+1 << " " << tri[2]+1 << "/" << tri[2]+1 << "/" << tri[2]+1 << "\n";
    }

    fld_file.close();
    input_file.close();
    output_file.close();
}

void world_extractor_loop()
{
    ImGui::InputText("input path", input_path, sizeof(input_path));
    if (ImGui::Button("extract"))
    {
        extract_world();
    }
}