#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <iostream>
#include "openGL.h"
#include "repacking.h"
#include "model_extractor.h"
#include "hot_extractor.h"
#include "main_window.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <windows.h>

using namespace std;

void init(GLFWwindow* window){}

static float red = 1.0f;
static float green = 1.0f;
static float blue = 1.0f;
static float alpha = 1.0f;

void draw()
{
    glClearColor(red, green, blue, alpha);
    glClear(GL_COLOR_BUFFER_BIT);
        
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Color", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::SetWindowSize(ImVec2(display_w,display_h));
    
    main_loop();
    
    ImGui::End();
    ImGui::Render();
    
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    
    glfwSwapBuffers(window);
    glfwPollEvents();
}

static void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    display_w = static_cast<float>(width);
    display_h = static_cast<float>(height);
    draw();
}

int main(void)
{
    //makes console dissapear
    ::ShowWindow(::GetConsoleWindow(), SW_HIDE);
    if(!glfwInit()){ exit(EXIT_FAILURE); }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    display_w = 800;
    display_h = 600;
    window = glfwCreateWindow(static_cast<int>(display_w), static_cast<int>(display_h), "Chameleon's Voodoo Tools", nullptr,nullptr);
    glfwMakeContextCurrent(window);
    glfwSetWindowAttrib(window, GLFW_RESIZABLE, GLFW_FALSE);  // disables resizing
    if(glewInit() != GLEW_OK){ exit(EXIT_FAILURE); }
    glfwSwapInterval(1);

    // sets the icon //
    int width, height, channels;
    unsigned char* data = stbi_load("source files/textures/zgcvinceheadicon.png", &width, &height, &channels, 4);
    if (!data)
    {
        std::cerr << "Failed to load image\n";
        //glfwTerminate();
        //return -1;
    }

    GLFWimage icon;
    icon.width = width;
    icon.height = height;
    icon.pixels = data;

    glfwSetWindowIcon(window, 1, &icon);
    stbi_image_free(data);
    // *** //

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    init(window);
    
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init();

    init_main();
    init_repacker();
    init_model_extractor();
    init_hot_extractor();
    
    while(!glfwWindowShouldClose(window))
    {
        draw();
    }
    
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown(); 

    glfwDestroyWindow(window);
    glfwTerminate();
    exit(EXIT_SUCCESS);
}
