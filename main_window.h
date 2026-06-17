#pragma once
#include <string>

inline std::string combined_path;
static char game_path_input[128] = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Voodoo Vince Remastered\\";
constexpr char path_extention[128] = "vincedata\\levels\\";
inline std::string combined_output_path;
#ifdef _DEBUG
static char global_output_path[128] = "C:\\Users\\leong\\Desktop\\vince stuff\\output";
#else
static char global_output_path[128] = "";
#endif
inline bool dds_convert = true;
inline bool delete_extracted_file = true;
inline bool convert_level_bsp = true;
inline bool model_compression = true;
inline bool include_bones = true;
inline bool convert_world = true;
void init_main();
void main_loop();