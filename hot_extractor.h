#pragma once
#include <string>
#include <vector>

#include "json.hpp"

void init_hot_extractor();
void hot_extractor_loop();
void extract_hot_file(std::string filepath);
void bsp_converter(std::string &filepath, std::string &filename);
void extract_textures(std::string filepath, std::vector<std::string> *textures);