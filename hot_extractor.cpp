#include "hot_extractor.h"
#include <imgui.h>
#include <filesystem>
#include <iostream>
#include "main_window.h"
#include <fstream>
#include <thread>
#include "model_extractor.h"
#include "FreeImage.h"

extern "C"{
#include <zlib.h>
}

using namespace std;

constexpr uint8_t hot_header_size = 36;
bool valid_output_dir = false;
vector<string> texture_list;
string sub_e;
int name_iterations;
bool textures_extracted_check = true;
bool png_conversion_check = false;
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

void init_hot_extractor()
{
    
}

void dds_to_png(string& path, string& name)
{
    FreeImage_Initialise();
    FIBITMAP* ddsImage = FreeImage_Load(FIF_DDS, (path+name).c_str(), DDS_DEFAULT);
    if (!ddsImage)
    {
        cout << "invalid dds! " << name << "\n";
        return;
    }
    size_t last_dot = name.find_last_of('.');
    if (!FreeImage_Save(FIF_PNG, ddsImage, (path+name.substr(0, last_dot)+".png").c_str(), 0))
    {
        cout << "failed to save png image " << name << "\n";
    }
    FreeImage_Unload(ddsImage);
    FreeImage_DeInitialise();
}

void extract_hot_file()
{
    ifstream src_file(file_to_extract, ios::binary);
    if (!src_file.is_open())
    {
        return;
    }
    hot_header current_hot_header;
    src_file.clear();
    src_file.seekg(0, ios::beg);
    src_file.read(reinterpret_cast<char*>(&current_hot_header), sizeof(hot_header));
    
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
        size_t last_slash = file_to_extract.find_last_of('\\');
        if (!texture_list.empty() && file_to_extract.substr(last_slash+1, file_to_extract.length()-last_slash) == "textures.hot")
        {
            if (ranges::find(texture_list, filename) != texture_list.end())
            {
                name_iterations++;
            }
            else
            {
                continue;
            }
        }

        // reading the raw data of the new file but not writing it yet
        vector<char> data_buffer(current_file_info.uncompressed_size-current_file_info.header_size);
        src_file.clear();
        src_file.seekg(current_file_info.raw_file_offset);
        src_file.read(data_buffer.data(), current_file_info.uncompressed_size-current_file_info.header_size);

        // creating the new file
        ofstream output_file(combined_output_path+filename, ios::binary, ios::trunc);
        
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
            file_to_extract = combined_output_path+filename;
            extract_hot_file();
            
            // remove .hot file after extraction
            remove((combined_output_path+filename).c_str());
        }
        // checks if the .gator files should convert into .obj files directly
        if (filename.ends_with(".gator"))
        {
            output_file.close();
            vector<string> tmp_list = extract_model(combined_output_path+filename);
            texture_list.insert(texture_list.begin(), tmp_list.begin(), tmp_list.end());
            
            // delete the .gator after we have extracted it
            remove((combined_output_path+filename).c_str());
        }

        if (filename.ends_with(".dds"))
        {
            output_file.close();

            if (png_conversion_check)
            {
                dds_to_png(combined_output_path, filename);
                remove((combined_output_path+filename).c_str());
            }
            
        }
        if (output_file.is_open())
        {
            output_file.close();
        }
    }
    if (!texture_list.empty() && textures_extracted_check)
    {
        size_t last_slash = sub_e.find_last_of('\\');
        if (sub_e.substr(0, last_slash).ends_with("common"))
        {
            return;
        }
        textures_extracted_check = false;
        // erase duplicate texture names
        sort(texture_list.begin(), texture_list.end());
        auto last = unique(texture_list.begin(), texture_list.end());
        texture_list.erase(last, texture_list.end());

        // converts all texture names to lowercase for proper comparison
        ranges::for_each(texture_list, [](string& texture_name)
        {
            ranges::transform(texture_name, texture_name.begin(), [](char c)
            {
                return tolower(c);
            });
        });
        
        // extract all textures from the model
        file_to_extract = sub_e;
        extract_hot_file();
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
                                    texture_list.clear();
                                    textures_extracted_check = true;
                                    
                                    file_to_extract = hot_files.path().string();
                                    size_t last_slash = file_to_extract.find_last_of('\\');
                                    sub_e = file_to_extract.substr(0, last_slash);
                                    sub_e += "\\textures.hot";
                                    
                                    thread taskThread(extract_hot_file);
                                    taskThread.join();
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
    if (ImGui::InputText("Output path", global_output_path, IM_ARRAYSIZE(global_output_path)))
    {
        valid_output_dir = filesystem::exists(global_output_path);
        combined_output_path = global_output_path;
        if (!combined_output_path.ends_with("\\"))combined_output_path+="\\";
    }
    ImGui::Checkbox("convert dds files to png", &png_conversion_check);
    
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