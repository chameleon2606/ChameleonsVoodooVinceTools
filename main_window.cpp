#include <iostream>
#include <imgui.h>
#include "repacking.h"
#include "model_extractor.h"
#include "main_window.h"

void main_loop()
{
    ImGui::BeginTabBar("Chameleon's toolbox");
    {
        if (ImGui::BeginTabItem("Repacker"))
        {
            init_repacker();
            repack_loop();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Model extractor"))
        {
            init_extractor();
            m_extractor_loop();
            ImGui::EndTabItem();
        }
    }
}
