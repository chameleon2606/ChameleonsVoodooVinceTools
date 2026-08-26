#include "world_extractor.h"
#include <iostream>
#include "imgui.h"
#include <filesystem>
#include <fstream>
#include <array>
#include <deque>
#include <thread>

#include "main_window.h"
#include "include/json.hpp"
#include "tdf_parser.h"
#include "FreeImage.h"

#include "GLFW/glfw3.h"

using namespace std;
size_t remember_position = 0;
int32_t last_object = -1;
string last_filename;

struct vertex_info
{
    float x_pos, y_pos, z_pos;
    float x_norm, y_norm, z_norm;
    float values_0[7];
    float x_uv1, y_uv1, x_uv2, y_uv2, lightmap_uv_x, lightmap_uv_y;
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
    int offsets_amount;
    short values2[4];
    float pos_x, pos_y, pos_z;
    int values3[7];
    int header_size;
    int vertex_indices_bytes;
    int raw_file_offset;
    uint32_t raw_file_offset_offset;
    int aux_u_offset;
    int string_index_offset;
    float min_x, min_y, min_z;
    float max_x, max_y, max_z;
    float min_x_2, min_y_2, min_z_2;
    float max_x_2, max_y_2, max_z_2;
    int object_id;
};

void append_floats(std::vector<char>& bytes, const std::vector<float>& floats) {
    const char* p = reinterpret_cast<const char*>(floats.data());
    bytes.insert(bytes.end(), p, p + floats.size() * sizeof(float));
}

vector<char> dds_to_png_vector_w(const string& src_name)
{
    FIBITMAP* ddsImage = FreeImage_Load(FIF_DDS, (filesystem::current_path().string()+ R"(\source files\textures\)" +src_name).c_str(), DDS_DEFAULT);
    if (!ddsImage)
    {
        cout << "invalid dds! " << src_name << "\n";
        vector<char>empty_list;
        return empty_list;
    }
    
    FIBITMAP* converted = FreeImage_ConvertTo32Bits(ddsImage);
    FreeImage_Unload(ddsImage);
    FIMEMORY* memStream = FreeImage_OpenMemory();
    FreeImage_SaveToMemory(FIF_PNG, converted, memStream, PNG_DEFAULT);
    BYTE* data = nullptr;
    DWORD sizeInBytes = 0;
    FreeImage_AcquireMemory(memStream, &data, &sizeInBytes);
    std::vector<char> pngBytes(reinterpret_cast<char*>(data), reinterpret_cast<char*>(data) + sizeInBytes);
    FreeImage_CloseMemory(memStream);
    
    return pngBytes;
}

void extract_world()
{
    nlohmann::json gltf_data;
    gltf_data["asset"]["version"] = "2.0";
    gltf_data["asset"]["generator"] = "chameleon's voodoo tools";
    
    nlohmann::json sampler;
    sampler["magFilter"] = 9729;
    sampler["minFilter"] = 9987;
    gltf_data["samplers"].push_back(sampler);
    
    gltf_data["materials"] = nlohmann::json::array();
    
    string tdf_path = combined_output_path+"\\index.tdf";
    nlohmann::json jsonData = SwampParser::parseFile(tdf_path);
    ofstream json_file(combined_output_path+"j.json");
    json_file << setw(4) << jsonData;
    json_file.close();
    
    vector<string> texture_list;
    gltf_data["images"] = nlohmann::json::array();
    
    vector<char> binary_data;
    
    int buffer_view_count = 0;
    
    unordered_map<int, int> material_idx;
    
    for (size_t i = 0; auto& [id, mat] : jsonData["Materials"].items())
    {
        string base = mat.value("base", "");
        string lightmap = mat.value("lightmap", "");
        material_idx[stoi(id)] = i;
        nlohmann::json material_data;
        
        if (!base.empty())
        {
            size_t last_dot = base.find_last_of('.');
            string texture_name = base.substr(0,last_dot);
            
            auto name = ranges::find(texture_list.begin(), texture_list.end(), texture_name);
            if (name == texture_list.end())
            {
                nlohmann::json image;
                image["mimeType"] = "image/png";
                image["name"] = texture_name;
                image["bufferView"] = buffer_view_count;
                gltf_data["images"].push_back(image);
                
                nlohmann::json texture;
                texture["sampler"] = 0;
                texture["source"] = texture_list.size();
                gltf_data["textures"].push_back(texture);
                
                texture_list.push_back(texture_name);
                
                vector<char> texture_binary_data = dds_to_png_vector_w(texture_name+".dds");
                long long texture_file_size = texture_binary_data.size();
                
                nlohmann::json buffer_view;
                buffer_view["buffer"] = 0;
                buffer_view["byteLength"]= texture_file_size;
                buffer_view["byteOffset"]= binary_data.size();
                gltf_data["bufferViews"].push_back(buffer_view);
                buffer_view_count++;
                
                binary_data.insert(binary_data.end(), texture_binary_data.begin(),texture_binary_data.end());
                // write offset remainder
                int byte_remainder = (ceil(texture_file_size / 4.0)*4-texture_file_size);
                if (byte_remainder > 0)
                {
                    for (int j = 0; j < byte_remainder; j++)
                    {
                        binary_data.push_back('\0');
                    }
                }
            }
            // looks for texture in list and notes the index
            auto idx = ranges::find(texture_list, texture_name);
            material_data["pbrMetallicRoughness"]["baseColorTexture"]["index"] = distance(texture_list.begin(), idx);
        }
        
        material_data["alphaMode"] = "MASK";
        material_data["doubleSided"] = false;
        material_data["name"] = id;
        material_data["pbrMetallicRoughness"]["metallicFactor"] = 0;
        material_data["pbrMetallicRoughness"]["roughnessFactor"] = 1;
        
        gltf_data["materials"].push_back(material_data);
        i++;
    }
    
    gltf_data["scene"] = 0;
    nlohmann::json scenes;
    scenes["name"] = "Scene";
    
    ofstream glb_file(combined_output_path+"\\"+jsonData["Zones"].begin().key()+".glb", ios::binary | ios::trunc);
    
    ifstream vertices_file(combined_output_path+"\\vertices.raw", ios::binary);
    
    int mesh_count = 0;
    int meshes_in_zone = 0;
    int last_mesh_amount = 0;
    int accessor_count = 0;
    int node_count = 0;
    for (auto &zone : jsonData["Zones"].items())
    {
        ifstream fld_file(combined_output_path+ "zone000"+to_string(zone.value())+".fld", ios::binary);
        fld_header current_flud_header;
        fld_info section_header;
        fld_file.clear();
        fld_file.seekg(0, ios::beg);
        fld_file.read(reinterpret_cast<char*>(&current_flud_header), sizeof(fld_header));
        
        string path = combined_output_path+zone.key();
        float buffer_data = 0;
        uint32_t pos = 0;
        int current_zone_count = 0;
        
        vector<string>material_list;
        
        nlohmann::json mesh_entry;
        
        deque<uint32_t>offsets;
        bool repeats = false;
        while (pos < current_flud_header.string_indices_offset)
        {
            if (repeats)
            {
                fld_file.seekg(offsets[0], ios::beg);
                offsets.pop_front();
            }
            uint32_t before_section_offset = fld_file.tellg();
            
            fld_file.read(reinterpret_cast<char*>(&section_header),sizeof(fld_info));
            
            if (section_header.section_variant == 2)
            {
                nlohmann::json node;
                node["mesh"] = mesh_count;
                gltf_data["nodes"].push_back(node);
                node_count++;
                
                nlohmann::json mesh;
                nlohmann::json primitives;
                
                if (jsonData["Materials"][to_string(section_header.material_index)]["base"] != nullptr)
                {
                    // get the clean name of the texture of that object
                    size_t last_dot = to_string(jsonData["Materials"][to_string(section_header.material_index)]["base"]).find_last_of('.');
                    string mtl = to_string(jsonData["Materials"][to_string(section_header.material_index)]["base"]).substr(1,last_dot-1);
                    
                    node["name"] = mtl;
                    mesh["name"] = mtl;
                    primitives["material"] = material_idx[section_header.material_index];
                }
                else
                {
                    node["name"] = "object "+to_string(mesh_count);
                    mesh["name"] = "object "+to_string(mesh_count);
                }
                
                
                /*
                if (section_header.object_id > last_object)
                {
                    last_object = section_header.object_id;
                }
                */
                
                remember_position = fld_file.tellg();
                
                // seek to the name offset table
                fld_file.seekg(current_flud_header.string_indices_offset + section_header.string_index_offset, ios::beg);
                uint32_t offset;
                // read the offset
                fld_file.read(reinterpret_cast<char*>(&offset), sizeof(uint32_t));
                // seek to the start of the string
                fld_file.seekg(current_flud_header.string_indices_offset+offset, ios::beg);
                // read chars one by one until terminated by \0
                char c;
                string filename;
                while (fld_file.read(&c, 1)&& c != '\0')
                {
                    filename += c;
                }
                if (last_filename != filename)
                {
                    last_filename = filename;
                }

                // seek back
                fld_file.clear();
                fld_file.seekg(remember_position, ios::beg);
                
                vector<float> pos_list;
                vector<float> norms_list;
                vector<float> uvs_list;
                vector<float> lm_uvs_list;
                vector<uint16_t>strip_buffer(section_header.strip_indices_amount);
                    
                buffer_data = section_header.strip_indices_amount * 2.0;
                    
                fld_file.read(reinterpret_cast<char*>(strip_buffer.data()),strip_buffer.size()*sizeof(uint16_t));
                    
                vertices_file.seekg(section_header.raw_file_offset, ios::beg);
                
                int vertex_count = 0;
                for (size_t i = 0; i < section_header.raw_file_offset_offset / sizeof(vertex_info); i++)
                {
                    vertex_info current_vertex;
                    vertices_file.read(reinterpret_cast<char*>(&current_vertex), sizeof(vertex_info));
                    
                    pos_list.push_back(-current_vertex.x_pos);
                    pos_list.push_back(current_vertex.y_pos);
                    pos_list.push_back(current_vertex.z_pos);
                    
                    norms_list.push_back(current_vertex.x_norm);
                    norms_list.push_back(current_vertex.y_norm);
                    norms_list.push_back(current_vertex.z_norm);
                    
                    uvs_list.push_back(current_vertex.x_uv1*16);
                    uvs_list.push_back(current_vertex.y_uv1*16);
                    
                    lm_uvs_list.push_back(current_vertex.lightmap_uv_x*16);
                    lm_uvs_list.push_back(current_vertex.lightmap_uv_y*16);
                    
                    vertex_count++;
                }
                
                // convert vertex positions, normals and uvs from floats to bytes and write them to the char array
                // POSITIONS
                nlohmann::json vertex_pos_accessor;
                vertex_pos_accessor["bufferView"] = buffer_view_count;
                vertex_pos_accessor["componentType"] = 5126;
                vertex_pos_accessor["count"] = vertex_count;
                vertex_pos_accessor["min"] = {-section_header.max_x, section_header.min_y, section_header.min_z};
                vertex_pos_accessor["max"] = {-section_header.min_x, section_header.max_y, section_header.max_z};
                vertex_pos_accessor["type"] = "VEC3";
                gltf_data["accessors"].push_back(vertex_pos_accessor);
                
                primitives["attributes"]["POSITION"] = accessor_count;
                accessor_count++;
                
                nlohmann::json vertex_pos_buffer_views;
                vertex_pos_buffer_views["byteLength"] = pos_list.size() * sizeof(float);
                vertex_pos_buffer_views["buffer"] = 0;
                vertex_pos_buffer_views["byteOffset"] = binary_data.size();
                vertex_pos_buffer_views["target"] = 34962;
                gltf_data["bufferViews"].push_back(vertex_pos_buffer_views);
                buffer_view_count++;
                // write vertex pos data to binary data list
                append_floats(binary_data, pos_list);
                //binary_data.insert(binary_data.end(), pos_list.begin(), pos_list.end());
                /*
                for (auto& p : vpos)
                {
                    auto pos_as_bytes = reinterpret_cast<const char*>(p.data());
                    binary_data.insert(binary_data.end(), pos_as_bytes, pos_as_bytes+sizeof(p.data()));
                }
                */
                
                // NORMALS
                nlohmann::json norms_accessor;
                norms_accessor["bufferView"] = buffer_view_count;
                norms_accessor["componentType"] = 5126;
                norms_accessor["count"] = vertex_count;
                norms_accessor["type"] = "VEC3";
                gltf_data["accessors"].push_back(norms_accessor);
                primitives["attributes"]["NORMAL"] = accessor_count;
                accessor_count++;
                
                nlohmann::json norms_buffer_views;
                norms_buffer_views["byteLength"] = norms_list.size() * sizeof(float);
                norms_buffer_views["buffer"] = 0;
                norms_buffer_views["byteOffset"] = binary_data.size();
                norms_buffer_views["target"] = 34962;
                gltf_data["bufferViews"].push_back(norms_buffer_views);
                buffer_view_count++;
                
                append_floats(binary_data, norms_list);
                /*
                for (auto& norm : norms)
                {
                    obj_file << "vn " << norm[0] << " " << norm[1] << " " << norm[2] << "\n";
                    
                    auto norm_as_bytes = reinterpret_cast<const char*>(norm.data());
                    binary_data.insert(binary_data.end(), norm_as_bytes, norm_as_bytes+sizeof(norm.data()));
                }
                */

                // TEXTURE UVs
                nlohmann::json uv_accessor;
                uv_accessor["bufferView"] = buffer_view_count;
                uv_accessor["componentType"] = 5126;
                uv_accessor["count"] = vertex_count;
                uv_accessor["type"] = "VEC2";
                gltf_data["accessors"].push_back(uv_accessor);
                
                primitives["attributes"]["TEXCOORD_0"] = accessor_count;
                accessor_count++;
                
                nlohmann::json uv_buffer_views;
                uv_buffer_views["byteLength"] = uvs_list.size() * sizeof(float);
                uv_buffer_views["buffer"] = 0;
                uv_buffer_views["byteOffset"] = binary_data.size();
                uv_buffer_views["target"] = 34962;
                gltf_data["bufferViews"].push_back(uv_buffer_views);
                buffer_view_count++;
                
                append_floats(binary_data, uvs_list);
                /*
                for (auto& uv : uvs)
                {
                    obj_file << "vt " << uv[0] << " " << uv[1]*-1 << "\n";
                    
                    auto uv_as_bytes = reinterpret_cast<const char*>(uv.data());
                    binary_data.insert(binary_data.end(), uv_as_bytes, uv_as_bytes+sizeof(uv.data()));
                }
                */
                
                // LIGHTMAP UVs
                nlohmann::json lightmap_uv_accessor;
                lightmap_uv_accessor["bufferView"] = buffer_view_count;
                lightmap_uv_accessor["componentType"] = 5126;
                lightmap_uv_accessor["count"] = vertex_count;
                lightmap_uv_accessor["type"] = "VEC2";
                gltf_data["accessors"].push_back(lightmap_uv_accessor);
                
                primitives["attributes"]["TEXCOORD_1"] = accessor_count;
                accessor_count++;
                
                nlohmann::json lightmap_uv_buffer_views;
                lightmap_uv_buffer_views["byteLength"] = lm_uvs_list.size() * sizeof(float);
                lightmap_uv_buffer_views["buffer"] = 0;
                lightmap_uv_buffer_views["byteOffset"] = binary_data.size();
                lightmap_uv_buffer_views["target"] = 34962;
                gltf_data["bufferViews"].push_back(lightmap_uv_buffer_views);
                buffer_view_count++;
                
                append_floats(binary_data, lm_uvs_list);
                /*
                for (auto& uv : lm_uvs)
                {
                    auto uv_as_bytes = reinterpret_cast<const char*>(uv.data());
                    binary_data.insert(binary_data.end(), uv_as_bytes, uv_as_bytes+sizeof(uv.data()));
                }
                */
                
                vector<uint16_t> faces;
                for (uint16_t k = 0; k < section_header.strip_indices_amount-2;k++)
                {
                    uint16_t f1 = strip_buffer[k + 0];
                    uint16_t f2 = strip_buffer[k + 1];
                    uint16_t f3 = strip_buffer[k + 2];
                    if (f1 != f2 && f1 != f3 && f2 != f3)
                    {
                        if (k & 1)
                        {
                            faces.push_back(f1);
                            faces.push_back(f2);
                            faces.push_back(f3);
                        }
                        else
                        {
                            faces.push_back(f2);
                            faces.push_back(f1);
                            faces.push_back(f3);
                        }
                    }
                }
                int face_indices_amount = faces.size();
                nlohmann::json index_accessor;
                index_accessor["bufferView"] = buffer_view_count;
                index_accessor["componentType"] = 5123;
                index_accessor["count"] = face_indices_amount;
                index_accessor["type"] = "SCALAR";
                gltf_data["accessors"].push_back(index_accessor);
                
                primitives["indices"] = accessor_count;
                mesh["primitives"].push_back(primitives);
                gltf_data["meshes"].push_back(mesh);
                accessor_count++;

                nlohmann::json indices_buffer_views;
                indices_buffer_views["byteLength"] = face_indices_amount * sizeof(uint16_t);
                indices_buffer_views["buffer"] = 0;
                indices_buffer_views["byteOffset"] = binary_data.size();
                indices_buffer_views["target"] = 34963;
                
                gltf_data["bufferViews"].push_back(indices_buffer_views);
                buffer_view_count++;
                
                const char* p = reinterpret_cast<const char*>(faces.data());
                binary_data.insert(binary_data.end(), p, p + face_indices_amount * sizeof(uint16_t));
                int byte_remainder = (ceil((face_indices_amount * sizeof(uint16_t)) / 4.0)*4-(face_indices_amount*sizeof(uint16_t)));
                if (byte_remainder > 0)
                {
                    for (int j = 0; j < byte_remainder; j++)
                    {
                        binary_data.push_back('\0');
                    }
                }

                int remainder = (ceil(buffer_data / 16) * 16) - buffer_data;
                if (remainder > 0)fld_file.seekg(remainder, ios::cur);
                
                mesh_count++;
                meshes_in_zone++;
            }
            
            else if (section_header.section_variant == 0)
            {
                buffer_data = section_header.offsets_amount * 4.0;
                vector<int32_t>offsets_list(section_header.offsets_amount);
                fld_file.read(reinterpret_cast<char*>(offsets_list.data()),offsets_list.size()*sizeof(uint32_t));
                
                for (long long i = 0; i < section_header.offsets_amount; i++)
                {
                    offsets.push_back(before_section_offset+offsets_list[i]);
                }
                    
                int remainder = (ceil(buffer_data / 16) * 16) - buffer_data;
                if (remainder > 0)fld_file.seekg(remainder, ios::cur);
            }
            else if (section_header.section_variant == 3)
            {
                /*
                if (section_header.material_index > 0)
                {
                    fld_file.seekg(16, ios::cur);
                }*/
            }
            else
            {
                cout << "section error at: " << fld_file.tellg() << endl;
                return;
            }
            
            pos = fld_file.tellg();
            if (pos == 801056)
            {
                cout << pos;
            }
            if (remember_position-160 == current_flud_header.last_header)
            {
                cout << "same offset";
            }
            /*
            if (!repeats && pos >= current_flud_header.string_indices_offset)
            {
                repeats=true;
            }
            if (repeats && offsets.empty())
            {
                break;
            }
            */
        }
        // puts parent node into scene collection
        scenes["nodes"].push_back(node_count);
        // parent node
        vector<int> children_list(meshes_in_zone);
        iota(children_list.begin(), children_list.end(), last_mesh_amount+current_zone_count);//maybe + zone_count
        nlohmann::json node;
        node["children"] = children_list;
        node["name"] = zone.key();
        gltf_data["nodes"].push_back(node);
        node_count++;
        
        last_mesh_amount = mesh_count+1;
        meshes_in_zone = 0;
        current_zone_count+=2;
        
        fld_file.close();
    }
    vertices_file.close();
    
    vector<int> mesh_list(mesh_count);
    iota(mesh_list.begin(), mesh_list.end(), 0);
    
    //scenes["nodes"] = mesh_list;
    gltf_data["scenes"].push_back(scenes);
    
    // write bin file first to get the actual size of the bin section
    glb_file.write(binary_data.data(), binary_data.size());
    uint32_t raw_bin_size = glb_file.tellp();
    uint32_t binary_buffer_alignment = static_cast<int>(ceil(raw_bin_size / 4.0)*4)-raw_bin_size;
    uint32_t combined_bin_size = raw_bin_size + binary_buffer_alignment;
    
    // write bin size into JSON
    nlohmann::json buffer;
    buffer["byteLength"] = combined_bin_size;
    gltf_data["buffers"].push_back(buffer);
    
    // write JSON section
    glb_file.seekp(20, ios::beg);
#ifdef _DEBUG
    glb_file << setw(4) << gltf_data;
#else
    glb_file << gltf_data;
#endif
    // get size of JSON section
    streamoff json_size = glb_file.tellp();
    json_size -= 20;
    
    long long json_buffer_alignment = (ceil(json_size / 4.0)*4)-json_size;
    if (json_buffer_alignment > 0)
    {
        json_size += json_buffer_alignment;
        glb_file.write(reinterpret_cast<const char*>(&"   "), json_buffer_alignment*sizeof(char));
    }
    
    // write bin header
    glb_file.write(reinterpret_cast<const char*>(&combined_bin_size), sizeof(uint32_t));
    glb_file.write(reinterpret_cast<const char*>(&"BIN"), 4*sizeof(char));
    
    // write bin section for real this time
    glb_file.write(binary_data.data(), binary_data.size());
    glb_file.write(reinterpret_cast<const char*>(&""), binary_buffer_alignment*sizeof(char));
    
    // writes glb header
    uint32_t total_file_size = glb_file.tellp();
    glb_file.seekp(0, ios::beg);
    glb_file.write(reinterpret_cast<const char*>(&"glTF"), 4*sizeof(char));
    uint32_t version = 2;
    glb_file.write(reinterpret_cast<const char*>(&version), sizeof(uint32_t));
    glb_file.write(reinterpret_cast<const char*>(&total_file_size), sizeof(uint32_t));
    
    // writes JSON header
    glb_file.write(reinterpret_cast<const char*>(&json_size), sizeof(uint32_t));
    glb_file.write(reinterpret_cast<const char*>(&"JSON"), 4*sizeof(char));
    
    glb_file.close();
}