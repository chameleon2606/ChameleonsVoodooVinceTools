#include "hot_extractor.h"
#include <imgui.h>
#include <filesystem>
#include <iostream>
#include "main_window.h"
#include <fstream>
#include <thread>
#include "model_extractor.h"
#include "FreeImage.h"
#include "include/json.hpp"

extern "C"{
#include <zlib.h>
}

using namespace std;

constexpr uint8_t hot_header_size = 36;
bool valid_output_dir = false;
vector<string> texture_list;
string sub_e;
string sub_entry;
int name_iterations;
bool textures_extracted_check = true;
bool png_conversion_check = true;
int is_remastered;

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
struct remastered_file_info
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
struct model_info
{
    vector<string> textures;
    string name;
};


void init_hot_extractor()
{
    valid_output_dir = filesystem::exists(global_output_path);
    combined_output_path = global_output_path;
    if (!combined_output_path.ends_with("\\"))combined_output_path+="\\";
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

void bsp_converter(string &bsp_path)
{
    struct verts_structure
    {
        float pos_x, pos_y, pos_z;
    };
    struct strip_structure
    {
        uint32_t point1, point2, point3, point4, unknown_value;
    };
    struct bsp_header
    {
        char signature[4];
        uint32_t version;
        uint32_t file_size;
        uint32_t section_1_entries;
        uint32_t section_2_entries;
        uint32_t section_3_entries;
        uint32_t section_4_entries;
        uint32_t unknown_value;
    };
    bsp_header current_bsp_header;
    
    ifstream bsp_file(bsp_path, ios::binary);
    ofstream hitbox_file(combined_output_path + sub_entry + "_collision.obj");

    bsp_file.read(reinterpret_cast<char*>(&current_bsp_header), sizeof(bsp_header));
    // seeks to the section of the vertex data
    bsp_file.seekg((current_bsp_header.section_1_entries * 4 * 4) + (current_bsp_header.section_2_entries * 5 * 4) + sizeof(bsp_header));
    // collects all vertex positions and writes them to the .obj file
    for (uint32_t i = 0; i < current_bsp_header.section_3_entries; i++)
    {
        verts_structure verts;
        bsp_file.read(reinterpret_cast<char*>(&verts), sizeof(verts_structure));
        // flips the x position
        hitbox_file << "v " << to_string(-verts.pos_x) << " " << to_string(verts.pos_y) << " " << to_string(verts.pos_z) << "\n";
    }
    // seeks to the position of the vertex indices
    bsp_file.seekg((current_bsp_header.section_1_entries * 4 * 4) + sizeof(bsp_header));
    for (uint32_t i = 0; i < current_bsp_header.section_2_entries; i++)
    {
        strip_structure strip;
        bsp_file.read(reinterpret_cast<char*>(&strip), sizeof(strip_structure));
        hitbox_file << "f " << to_string(strip.point3+1) << " " << to_string(strip.point2+1) << " " << to_string(strip.point1+1) << "\n";
    }
    
    bsp_file.close();
    hitbox_file.close();
    if (delete_extracted_file)
    {
        cout << bsp_path << "\n";
        remove(bsp_path.c_str());
        if (!remove(bsp_path.c_str()))
        {
            cout << bsp_path << " could not be deleted\n";
        }
    }
}

void glb_compressor(vector<model_info>&models)
{    
    for (auto& model : models)
    {
        string pathstring = global_output_path;
        pathstring+=model.name;
        ifstream gltf_file(pathstring+".gltf", ios::binary);
        ifstream binary_file(pathstring+".bin", ios::binary);
        ofstream glb_file(pathstring+".glb", ios::binary | ios::trunc);
        
        vector<char> texture_buffer;

        nlohmann::json gltf_json;
        gltf_json = nlohmann::json::parse(gltf_file);
        gltf_json["buffers"][0].erase("uri");
        
        for (size_t i = 0; i < model.textures.size(); i++)
        {
            // opens the texture
            size_t last_dot = model.textures[i].find_last_of('.');
            string texture_path = global_output_path;
            texture_path+=model.textures[i].substr(0, last_dot)+".png";
            ifstream texture_file(texture_path, ios::binary);
            if (!texture_file.is_open())
            {
                cout << "failed to open " << texture_path << "\n";
                continue;
            }
            texture_file.seekg(0, ios::end);
            long long texture_file_size = texture_file.tellg();
            
            vector<char> current_texture_data(texture_file_size);

            // deletes the uri from the json
            gltf_json["images"][i].erase("uri");
            // creates reference to the new buffer view
            gltf_json["images"][i]["bufferView"] = gltf_json["bufferViews"].size();

            // save file size, to calculate the offset for the texture buffer
            binary_file.seekg(0, ios::end);
            long long binary_file_size = binary_file.tellg();

            // creates buffer view
            nlohmann::json buffer_view;
            buffer_view["buffer"] = 0;
            buffer_view["byteLength"] = texture_file_size;
            buffer_view["byteOffset"] = binary_file_size+texture_buffer.size();
            gltf_json["bufferViews"].push_back(buffer_view);
            
            texture_file.seekg(0, ios::beg);
            texture_file.read(current_texture_data.data(),texture_file_size);
            copy(current_texture_data.begin(), current_texture_data.end(), back_inserter(texture_buffer));
            
            texture_file.close();
        }
        
        // calculate the size of the entire binary buffer
        binary_file.seekg(0,ios::end);
        gltf_json["buffers"][0]["byteLength"] = texture_buffer.size()+binary_file.tellg();
        
        // creating temporary new json, as the blueprint for the glb file
        ofstream tmp_gltf_file(pathstring+"_tmp.gltf",ios::binary | ios::trunc);
        
        tmp_gltf_file << setw(4) << gltf_json;

        // calculate the gltf file size and delete the temp file
        tmp_gltf_file.seekp(0, ios::end);
        long long gltf_file_size = tmp_gltf_file.tellp();
        tmp_gltf_file.close();
        remove((pathstring+"_tmp.gltf").c_str());

        // place an empty section as reservation for the glb header
        glb_file.seekp(0, ios::beg);
        glb_file.write(reinterpret_cast<const char*>(&""), sizeof(char)*12);
        
        // WRITING JSON SECTION
        glb_file.write(reinterpret_cast<const char*>(&gltf_file_size), sizeof(uint32_t));
        glb_file.write(reinterpret_cast<const char*>(&"JSON"), sizeof(char)*4);
        glb_file << setw(4) << gltf_json;
        gltf_file.close();
        remove((pathstring+".gltf").c_str());

        // WRITING BINARY SECTION
        // creating vector for the buffer
        binary_file.seekg(0,ios::end);
        long long bin_data_size = binary_file.tellg();
        vector<char> bin_data(bin_data_size);

        // writes texture buffer to the bin buffer
        binary_file.seekg(0, ios::beg);
        binary_file.read(bin_data.data(), bin_data_size);
        copy(texture_buffer.begin(), texture_buffer.end(), back_inserter(bin_data));
        binary_file.close();
        remove((pathstring+".bin").c_str());
        
        
        // writes the bin buffer to the glb file
        bin_data_size = bin_data.size();
        glb_file.write(reinterpret_cast<const char*>(&bin_data_size), sizeof(uint32_t));
        glb_file.write(reinterpret_cast<const char*>(&"BIN"), sizeof(char)*4);
        glb_file.write(bin_data.data(), bin_data_size);

        // WRITING GLB HEADER
        glb_file.seekp(0, ios::beg);
        glb_file.write(reinterpret_cast<const char*>(&"glTF"), sizeof(char)*4);
        uint32_t version = 2;
        glb_file.write(reinterpret_cast<const char*>(&version), sizeof(uint32_t));
        long long rem_pos = glb_file.tellp();
        glb_file.seekp(0, ios::end);
        uint32_t glb_file_size = glb_file.tellp();
        glb_file.seekp(rem_pos, ios::beg);
        glb_file.write(reinterpret_cast<const char*>(&glb_file_size), sizeof(uint32_t));

        glb_file.close();
    }
    
    for (auto& model : models)
    {
        for (const auto& texture : model.textures)
        {
            size_t last_dot = texture.find_last_of('.');
            string pathstring = global_output_path;
            pathstring+= texture.substr(0, last_dot)+".png";
            
            if (filesystem::exists(pathstring)){remove(pathstring.c_str());}
        }
    }
    
}

void extract_hot_file()
{
    // variables
    hot_header current_hot_header;
    ifstream src_file(file_to_extract, ios::binary);
    if (!src_file.is_open())
    {
        return;
    }
    src_file.clear();
    src_file.seekg(0, ios::beg);
    src_file.read(reinterpret_cast<char*>(&current_hot_header), sizeof(hot_header));

    vector<model_info> models(current_hot_header.file_count);
    
    uint32_t last_file_name_offset = 0;
    for (uint32_t i = 0; i < current_hot_header.file_count; i++)
    {
        // read the current file info table
        remastered_file_info current_file_info;
        src_file.clear();   // we have to call this or seeking might not work correctly
        src_file.seekg(hot_header_size+(sizeof(remastered_file_info)*i), ios::beg);
        src_file.read(reinterpret_cast<char*>(&current_file_info), sizeof(remastered_file_info));

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
            if (delete_extracted_file) remove((combined_output_path+filename).c_str());
        }
        // checks if the .gator files should convert into .obj files directly
        if (filename.ends_with(".gator"))
        {
            output_file.close();
            vector<string> tmp_list = extract_model(combined_output_path+filename);
            texture_list.insert(texture_list.begin(), tmp_list.begin(), tmp_list.end());
            
            size_t last_dot = filename.find_last_of('.');
            models[i].name = filename.substr(0, last_dot);
            models[i].textures = tmp_list;
            
            // delete the .gator after we have extracted it
            if (delete_extracted_file) remove((combined_output_path+filename).c_str());
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
        if (filename.ends_with(".bsp") && convert_level_bsp)
        {
            string bsp_path = combined_output_path+filename;
            output_file.close();
            bsp_converter(bsp_path);
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
        if (model_compression)glb_compressor(models);
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
                                    sub_entry = subentry.path().filename().string();
                                    
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
    if (ImGui::TreeNodeEx("Options"))
    {
        ImGui::Checkbox("convert .dds files to .png", &png_conversion_check);
        ImGui::Checkbox("Use alternative UV maps", &use_uv2);
        ImGui::SetItemTooltip("If textures don't appear correctly on the 3D model, try checking this box");
        ImGui::Checkbox("delete extracted and converted files", &delete_extracted_file);
        ImGui::Checkbox("Convert row-major bind matrix to column-major", &matrix_convert);
        ImGui::SetItemTooltip("DirectX uses row-major matrices, OpenGL (blender) uses col-major");
        ImGui::Checkbox("Pack models and textures into .glb files", &model_compression);
        ImGui::SetItemTooltip("May take longer to extract");
        /*
        ImGui::RadioButton("Remastered", &is_remastered, 0);
        ImGui::RadioButton("Original", &is_remastered, 1);
        */
        ImGui::Checkbox("extract level colliders", &convert_level_bsp);
        ImGui::TreePop();
    }
    if (ImGui::InputText("Output path", global_output_path, IM_ARRAYSIZE(global_output_path)))
    {
        valid_output_dir = filesystem::exists(global_output_path);
        combined_output_path = global_output_path;
        if (!combined_output_path.ends_with("\\"))combined_output_path+="\\";
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