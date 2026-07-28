#pragma once
#include <string>

inline std::string combined_path;
static char game_path_input[128] = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Voodoo Vince Remastered\\";
//static char game_path_input[128] = "F:\\ROMS\\Xbox\\games\\Voodoo Vince\\levels";
constexpr char path_extention[128] = "vincedata\\levels\\";
//constexpr char path_extention[128] = "";
inline std::string combined_output_path;
#ifdef _DEBUG
static char global_output_path[128] = "C:\\Users\\leong\\Desktop\\vince stuff\\output";
#else
static char global_output_path[128] = "";
#endif
inline bool dds_convert = true;
inline bool delete_extracted_file = true;
inline bool convert_level_bsp = false;
inline bool model_compression = true;
inline bool include_bones = true;
inline bool convert_world = true;
inline int is_remastered = true;
void init_main();
void main_loop();