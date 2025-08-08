#pragma once
#include <string>
#include <vector>

void init_model_extractor();
void m_extractor_loop();
std::vector<std::string> extract_model(std::string current_file_path);