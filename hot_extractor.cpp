#include "hot_extractor.h"
#include <imgui.h>
#include <filesystem>
#include <iostream>
#include "main_window.h"
#include <fstream>
extern "C"{
#include <zlib.h>
}

using namespace std;

static char output_path[128];
constexpr uint8_t hot_header_size = 36;
bool valid_output_dir = false;

void init_hot_extractor()
{
    string current_path = filesystem::current_path().string() + "\\output\\";
    strncpy_s(output_path, current_path.c_str(), sizeof(output_path) - 1);
    output_path[sizeof(output_path) - 1] = '\0'; // Ensure null termination
    valid_output_dir = filesystem::exists(output_path);
}

static void extract_hot_file(string path)
{
    ifstream src_file(path, ios::binary);
    if (!src_file.is_open())
    {
        return;
    }
    struct hot_header
    {
        char signature[4];
        uint32_t version;
        uint32_t headers_offset;
        uint32_t data_offset;
        uint32_t file_overall_size;
        uint32_t file_name_table_offset;
        uint32_t file_count;
        uint32_t reserved[2];
    };
    hot_header current_hot_header;
    src_file.clear();
    src_file.seekg(0, ios::beg);
    src_file.read(reinterpret_cast<char*>(&current_hot_header), sizeof(hot_header));
    
    struct file_info
    {
        uint32_t header_size;
        uint32_t header_offset;
        uint32_t uncompressed_size;
        uint32_t compressed_size;
        uint32_t raw_file_offset;
        uint32_t reserved1;
        uint32_t next_name_offset;
        uint32_t reserved2;
    };
    uint32_t last_file_name_offset = 0;
    for (uint32_t i = 0; i < current_hot_header.file_count; i++)
    {
        // read the current file info table
        file_info current_file_info;
        src_file.clear();   // we have to call this or seeking might not work correctly
        src_file.seekg(hot_header_size+(sizeof(file_info)*i), ios::beg);
        src_file.read(reinterpret_cast<char*>(&current_file_info), sizeof(file_info));

        // read the filename for the current file
        src_file.clear();
        src_file.seekg(current_hot_header.file_name_table_offset + last_file_name_offset, ios::beg);
        last_file_name_offset = current_file_info.next_name_offset;
        char c;
        string filename;
        while (src_file.read(&c, 1)&& c != '\0')
        {
            filename += c;
        }

        // if the given output path does not have a backslash at the end, we add one
        string fixed_output_path = output_path;
        if (!fixed_output_path.ends_with("\\"))
        {
            fixed_output_path += "\\";
        }

        // reading the raw data of the new file but not writing it yet
        vector<char> data_buffer(current_file_info.uncompressed_size-current_file_info.header_size);
        src_file.clear();
        src_file.seekg(current_file_info.raw_file_offset);
        src_file.read(data_buffer.data(), current_file_info.uncompressed_size-current_file_info.header_size);

        // creating the new file
        ofstream output_file(fixed_output_path+filename, ios::binary, ios::trunc);
        
        if (current_hot_header.headers_offset != current_hot_header.data_offset)    // file has a header
        {
            src_file.clear();
            src_file.seekg(current_file_info.header_offset, ios::beg);
            vector<char> header_buffer(current_file_info.header_size);
            src_file.read(header_buffer.data(), current_file_info.header_size);
            output_file.write(header_buffer.data(), current_file_info.header_size);
        }
        if (current_file_info.compressed_size == 0) // writes uncompressed data
        {
            output_file.write(data_buffer.data(), current_file_info.uncompressed_size-current_file_info.header_size);
        }
        else   // reads compressed data, decompresses it, and writes it
        {
            uLongf uncompressed_size = current_file_info.uncompressed_size;
            vector<char> uncompressed_data(uncompressed_size);
            int ret = uncompress(reinterpret_cast<Bytef*>(uncompressed_data.data()), &uncompressed_size, reinterpret_cast<const Bytef*>(data_buffer.data()), current_file_info.compressed_size);
            if (ret != Z_OK)
            {
                cerr << "uncompress error " << ret << "\n";
                output_file.close();
                return;
            }
            // writing uncompressed data
            output_file.write(uncompressed_data.data(), current_file_info.uncompressed_size);
            output_file.close();
            extract_hot_file(fixed_output_path+filename);
        }
        if (output_file.is_open())
        {
            output_file.close();
        }
    }
}

static void display_file_tree(const string& path)
{
    for (auto& entry : filesystem::directory_iterator(path))
    {
        ImGuiTreeNodeFlags flag = ImGuiTreeNodeFlags_DefaultOpen;
        if (ImGui::TreeNodeEx(entry.path().filename().string().c_str(), flag))
        {
            for (auto& subentry : filesystem::directory_iterator(entry))
            {
                if (subentry.path().filename().string().starts_with("area") || subentry.path().filename().string().starts_with("common"))
                {
                    if (ImGui::TreeNodeEx(subentry.path().filename().string().c_str()))
                    {
                        for (auto& hot_files : filesystem::directory_iterator(subentry))
                        {
                            if (hot_files.path().filename().string().ends_with(".hot"))
                            {
                                if (ImGui::Selectable(hot_files.path().filename().string().c_str()))
                                {
                                    extract_hot_file(hot_files.path().string());
                                }
                            }
                        }
                        ImGui::TreePop();
                    }
                    
                }
            }
            ImGui::TreePop();
        }
    }
}

void hot_extractor_loop()
{
    if (ImGui::InputText("Output path", output_path, IM_ARRAYSIZE(output_path)))
    {
        valid_output_dir = filesystem::exists(output_path);
    }
    if (!valid_output_dir)
    {
        ImGui::TextColored(ImVec4(1,0,0,1),"Invalid folder");
        return;
    }
    else
    {
        ImGui::TextColored(ImVec4(0,1,0,1),"Valid folder");
    }
    
    string newp = game_path_input;
    ImGui::BeginChild(".hot files");
    display_file_tree(newp + path_extention);
    ImGui::EndChild();
}