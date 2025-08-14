#pragma once
#include <string>

inline std::string combined_path;
static char game_path_input[128] = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Voodoo Vince Remastered\\";
constexpr char path_extention[128] = "vincedata\\levels\\";
inline std::string combined_output_path;
static char global_output_path[128] = "";
inline bool dds_convert = true;
inline bool use_uv2 = false;
inline bool delete_extracted_file = true;
void init_main();
void main_loop();