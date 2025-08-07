#include <iostream>
#include <imgui.h>
#include "repacking.h"
#include "model_extractor.h"
#include "main_window.h"
#include "hot_extractor.h"
#include "world_extractor.h"
#include <filesystem>

void init_main()
{
    combined_path = game_path_input;
    combined_path += path_extention;
}
void main_loop()
{
    if (ImGui::InputText("Game Path", game_path_input, sizeof(game_path_input)))
    {
        combined_path = game_path_input;
        combined_path += path_extention;
    }
    if (!std::filesystem::exists(combined_path))
    {
        ImGui::TextColored(ImVec4(1,0,0,1),"Invalid folder");
        return;
    }
    
    ImGui::BeginTabBar("Chameleon's toolbox");
    {
        if (ImGui::BeginTabItem("world"))
        {
            world_extractor_loop();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Hot file extractor"))
        {
            hot_extractor_loop();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Repacker"))
        {
            repack_loop();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Model extractor"))
        {
            m_extractor_loop();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}
