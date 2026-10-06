#pragma once
#include <string>

// draws a spinning 3D preview of a .glb/.gltf model at the current ImGui cursor (meant for tooltips)
// the model is loaded in the background on first use and cached afterwards
void draw_model_preview(const std::string& path);
