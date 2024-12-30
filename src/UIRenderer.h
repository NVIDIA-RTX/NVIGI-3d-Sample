// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//
#pragma once

#include <mutex>

#include "NVIGISample.h"
#include "nvigi/NVIGIContext.h"
#include "UIData.h"

#include <donut/app/DeviceManager.h>
#include <imgui_internal.h>
#include <donut/engine/Scene.h>
#include <nvrhi/nvrhi.h>
#include <donut/app/imgui_renderer.h>

void pushDisabled() {
    ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
}
void popDisabled() {
    ImGui::PopItemFlag();
    ImGui::PopStyleVar();
}

class UIRenderer : public ImGui_Renderer
{
private:
    std::shared_ptr<NVIGISample> m_app;
    ImFont* m_font_small = nullptr;
    ImFont* m_font_medium = nullptr;
    ImFont* m_font_large = nullptr;
    UIData& m_ui;

    bool    m_dev_view = false;

public:
    UIRenderer(DeviceManager* deviceManager, std::shared_ptr<NVIGISample> app, UIData& ui)
        : ImGui_Renderer(deviceManager)
        , m_app(app)
        , m_ui(ui) {

        m_font_small = this->LoadFont(*(app->GetRootFs()), "/media/fonts/DroidSans/DroidSans-Mono.ttf", 16.f);
        m_font_medium = this->LoadFont(*(app->GetRootFs()), "/media/fonts/DroidSans/DroidSans-Mono.ttf", 20.f);
        m_font_large = this->LoadFont(*(app->GetRootFs()), "/media/fonts/DroidSans/DroidSans-Mono.ttf", 25.f);

        // IMGUI by default writes in srgb colorSpace, but our back buffer is in rgb, we will simply pre-empt this by gamma shifting the colors.
        auto invGamma = 1.f / 2.2f;
        for (auto i = 0; i < ImGuiCol_COUNT; i++) {
            ImGui::GetStyle().Colors[i].x = powf(ImGui::GetStyle().Colors[i].x, invGamma);
            ImGui::GetStyle().Colors[i].y = powf(ImGui::GetStyle().Colors[i].y, invGamma);
            ImGui::GetStyle().Colors[i].z = powf(ImGui::GetStyle().Colors[i].z, invGamma);
            ImGui::GetStyle().Colors[i].w = powf(ImGui::GetStyle().Colors[i].w, invGamma);
        }
    }

protected:

    bool combo(const std::string& label, const std::vector<std::string>& values, std::string& value)
    {
        int index = -1;
        for (auto i = 0; i < values.size(); ++i) {
            if (values[i] == value) 
            {
                index = i;
                break;
            }
        }

        bool changed = false;
        if (ImGui::BeginCombo(label.c_str(), values[index].c_str()))
        {
            for (auto i = 0; i < values.size(); ++i)
            {
                bool is_selected = i == index;
                if (ImGui::Selectable(values[i].c_str(), is_selected))
                {
                    changed = index != i;
                    index = i;
                }
                if (is_selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        value = values[index];

        return changed;
    }

    virtual void buildUI(void) override
    {

        int width, height;
        GetDeviceManager()->GetWindowDimensions(width, height);

        //ImGui::SetNextWindowPos(ImVec2(width * 0.25f, height * 0.5f), 0, ImVec2(0.f, 0.5f));
        ImGui::SetNextWindowPos(ImVec2(width * 0.02f, height * 0.5f), 0, ImVec2(0.f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        ImGui::PushFont(m_font_medium);
        ImGui::Begin("NVIGI AI Sample", 0, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

        ImGui::Text("Renderer: %s", GetDeviceManager()->GetRendererString());

        size_t current = 0;
        size_t budget = 0;
        NVIGIContext::Get().GetVRAMStats(current, budget);
        ImGui::Text("VRAM: %.2f/%.2fGB", current / (1024.0 * 1024.0 * 1024.0), budget / (1024.0 * 1024.0 * 1024.0));

        double fps = 1.0 / GetDeviceManager()->GetAverageFrameTimeSeconds();
        ImGui::Text("FPS: %.0f ", fps);

        m_ui.Resolution = { 1920, 1080 };

        NVIGIContext::Get().BuildUI();

        ImGui::End();
        ImGui::PopFont();
    }
};