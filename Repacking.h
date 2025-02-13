#pragma once

#include <string>
#include <vector>
#include "json.hpp"
#include <fstream>
#include <chrono>

static constexpr bool IS_DEBUG = false;
static std::string currentLevel;
static std::string outputPath;
static std::vector<std::string> level_file_list;
static std::vector<char> raw_data_bytes;
static std::string TEXTURE_PATH = "\\source files\\textures\\";
static std::string SOUNDS_PATH = "\\source files\\sounds\\";
static std::string file_type = "textures";
static std::string SOURCE_FILES_PATH;
static constexpr int DDS_HEADER_SIZE = 128;
static constexpr int WAV_HEADER_SIZE = 72;
static int SRC_FILE_HEADER_SIZE;
static constexpr int HOT_HEADER_SIZE = 36;
static constexpr int METADATA_SIZE = 32;
static std::vector<char> filename_table;
static std::vector<unsigned long long> data_index;
static std::vector<char> metadata;
static unsigned long file_headers_size;
static unsigned long raw_data_size;
static unsigned long level_list_size;
static unsigned long raw_data_header_index;
inline FILE * jsonFile;
inline std::ifstream f("level_data.json");
inline nlohmann::json jsonData = nlohmann::json::parse(f);
static char game_path_input[128];
static char source_files_path_input[128];
static std::string repacking_mode;
static int e;
static float level_progress;
static std::chrono::duration<long long> duration;
inline auto startTime = std::chrono::high_resolution_clock::now();
inline auto endTime = std::chrono::high_resolution_clock::now();
static bool isRepacking = false;
static unsigned long long max_progress_value;
static bool repackingTextures = true;
static bool valid_game_dir = false;
static bool valid_source_dir = true;

void repack();
void init();
void update();
