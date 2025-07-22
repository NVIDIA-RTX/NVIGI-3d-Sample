// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//


#include <string>
#include <vector>
#include <memory>
#include <chrono>

#include <donut/core/vfs/VFS.h>
#include <donut/core/log.h>
#include <donut/core/string_utils.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ConsoleInterpreter.h>
#include <donut/engine/ConsoleObjects.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/Scene.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/render/BloomPass.h>
#include <donut/render/CascadedShadowMap.h>
#include <donut/render/DeferredLightingPass.h>
#include <donut/render/DepthPass.h>
#include <donut/render/DrawStrategy.h>
#include <donut/render/ForwardShadingPass.h>
#include <donut/render/GBuffer.h>
#include <donut/render/GBufferFillPass.h>
#include <donut/render/LightProbeProcessingPass.h>
#include <donut/render/PixelReadbackPass.h>
#include <donut/render/SkyPass.h>
#include <donut/render/SsaoPass.h>
#include <donut/render/TemporalAntiAliasingPass.h>
#include <donut/render/ToneMappingPasses.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/UserInterfaceUtils.h>
#include <donut/app/Camera.h>
#include <donut/app/imgui_console.h>
#include <donut/app/imgui_renderer.h>
#include <nvrhi/utils.h>
#include <nvrhi/common/misc.h>

#ifdef DONUT_WITH_TASKFLOW
#include <taskflow/taskflow.hpp>
#endif

#include "nvigi/NVIGIContext.h"
#include "RenderTargets.h"
#include "NVIGISample.h"
#include "UIRenderer.h"
#include "UIData.h"

#include <donut/app/DeviceManager.h>

std::ofstream log_file;
void logToFile(donut::log::Severity s, char const* txt) {
    if (log_file.is_open()) {
        auto str = std::string(txt);
        log_file << str.substr(0, str.size() - 1).c_str() << std::endl;
    }
};

bool ProcessCommandLine(int argc, const char* const* argv, Parameters& params)
{
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-width"))
        {
            params.deviceParams.backBufferWidth = std::stoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-height"))
        {
            params.deviceParams.backBufferHeight = std::stoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-debug"))
        {
            params.deviceParams.enableDebugRuntime = true;
            params.deviceParams.enableNvrhiValidationLayer = true;
        }
        else if (!strcmp(argv[i], "-verbose"))
        {
            donut::log::SetMinSeverity(donut::log::Severity::Info);
        }
        else if (!strcmp(argv[i], "-noSigCheck"))
        {
            params.checkSig = false;
        }
        else if (!strcmp(argv[i], "-vsync"))
        {
            params.deviceParams.vsyncEnabled = true;
        }
        else if (!strcmp(argv[i], "-scene"))
        {
            params.sceneName = argv[++i];
        }
        else if (!strcmp(argv[i], "-ui_only"))
        {
            params.renderScene = false;
        }
        else if (!strcmp(argv[i], "-fullscreen"))
        {
            params.deviceParams.startFullscreen = false;
        }
    }

    return true;
}

donut::app::DeviceManager* CreateDeviceManager(nvrhi::GraphicsAPI api)
{
    return donut::app::DeviceManager::Create(api);
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    nvrhi::GraphicsAPI api = donut::app::GetGraphicsAPIFromCommandLine(__argc, __argv);
#else //  _WIN32
int main(int __argc, const char* const* __argv)
{
    nvrhi::GraphicsAPI api = nvrhi::GraphicsAPI::VULKAN;
#endif //  _WIN32

    Parameters params{};

    params.deviceParams.backBufferWidth = 1920;
    params.deviceParams.backBufferHeight = 1080;
    params.deviceParams.swapChainSampleCount = 1;
    params.deviceParams.swapChainBufferCount = 3;
    params.deviceParams.startFullscreen = false;
    params.deviceParams.vsyncEnabled = false;
    params.deviceParams.swapChainFormat = nvrhi::Format::BGRA8_UNORM;
    if (api == nvrhi::GraphicsAPI::VULKAN)
    {
        params.deviceParams.enableComputeQueue = true;
        params.deviceParams.enableCopyQueue = true;
    }

    if (!ProcessCommandLine(__argc, __argv, params))
    {
        donut::log::error("Failed to process the command line.");
        return 1;
    }

    auto scripting = ScriptingConfig(__argc, __argv);

    // Initialise NVIGI before creating the device and swapchain.
    auto success = NVIGIContext::Get().Initialize_preDeviceManager(api, __argc, __argv);

    if (!success)
        return 0;

    DeviceManager* deviceManager = CreateDeviceManager(api);

    success = NVIGIContext::Get().Initialize_preDeviceCreate(deviceManager, params.deviceParams);

    if (!success)
        return 0;

    const char* apiString = nvrhi::utils::GraphicsAPIToString(deviceManager->GetGraphicsAPI());

    std::string windowTitle = "NVIGI Sample (" + std::string(apiString) + ")";

    if (!deviceManager->CreateWindowDeviceAndSwapChain(params.deviceParams, windowTitle.c_str()))
    {
        donut::log::error("Cannot initialize a %s graphics device with the requested parameters", apiString);
        return 1;
    }

    NVIGIContext::Get().SetDevice_nvrhi(deviceManager->GetDevice());

    NVIGIContext::Get().Initialize_postDevice();

    {
        UIData uiData;
        uiData.Resolution = donut::math::int2{ (int)params.deviceParams.backBufferWidth, (int)params.deviceParams.backBufferHeight };
        std::shared_ptr<NVIGISample> demo = std::make_shared<NVIGISample>(deviceManager, uiData, params.sceneName, scripting);

        std::shared_ptr<UIRenderer> gui = std::make_shared<UIRenderer>(deviceManager, demo, uiData);
        gui->Init(demo->GetShaderFactory());

        if (params.renderScene)
        {
            deviceManager->AddRenderPassToBack(demo.get());
        }
        deviceManager->AddRenderPassToBack(gui.get());

        deviceManager->RunMessageLoop();
    }

    // Most "real" apps have significantly more work to do between stopping the rendering loop and shutting down
    // SL.  Here, simulate that time as a WAR.
    Sleep(100);

    NVIGIContext::Get().Shutdown();

    deviceManager->Shutdown();
#ifdef _DEBUG
    deviceManager->ReportLiveObjects();
#endif

    delete deviceManager;

    return 0;
}