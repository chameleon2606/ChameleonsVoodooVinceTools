#pragma once
#include <string>

void init_hot_extractor();
void hot_extractor_loop();
void extract_hot_file(std::string filepath);
void bsp_converter(std::string &filepath, std::string &filename);