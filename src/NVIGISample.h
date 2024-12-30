// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

#pragma once

#include "nvigi/NVIGIContext.h"
#include "RenderTargets.h"
#include "UIData.h"
#include <random>
#include <chrono>

// From Donut
#include <donut/core/vfs/VFS.h>
#include <donut/core/log.h>
#include <donut/engine/CommonRenderPasses.h>
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
#include <donut/render/GBufferFillPass.h>
#include <donut/render/LightProbeProcessingPass.h>
#include <donut/render/PixelReadbackPass.h>
#include <donut/render/SkyPass.h>
#include <donut/render/SsaoPass.h>
#include <donut/render/TemporalAntiAliasingPass.h>
#include <donut/render/ToneMappingPasses.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/app/DeviceManager.h>
#include <nvrhi/utils.h>

using namespace donut::math;
using namespace donut::app;
using namespace donut::vfs;
using namespace donut::engine;
using namespace donut::render;

struct ScriptingConfig {

    // Control at start behavior
    int maxFrames = -1;

    ScriptingConfig(int argc, const char* const* argv)
    {

        for (int i = 1; i < argc; i++)
        {
            //MaxFrames
            if (!strcmp(argv[i], "-maxFrames"))
            {
                maxFrames = std::stoi(argv[++i]);
            }
        }
    }
};

class NVIGISample : public ApplicationBase
{
private:
    typedef ApplicationBase Super;

    // Main Command queue and binding cache
    nvrhi::CommandListHandle                        m_CommandList;
    BindingCache                                    m_BindingCache;

    // Filesystem and scene
    std::shared_ptr<RootFileSystem>                 m_RootFs;
    std::vector<std::string>                        m_SceneFilesAvailable;
    std::string                                     m_CurrentSceneName;
    std::shared_ptr<Scene>				            m_Scene;
    float                                           m_WallclockTime = 0.f;
                                                    
    // Render Passes                                
    std::shared_ptr<ShaderFactory>                  m_ShaderFactory;
    std::shared_ptr<DirectionalLight>               m_SunLight;
    std::shared_ptr<CascadedShadowMap>              m_ShadowMap;
    std::shared_ptr<FramebufferFactory>             m_ShadowFramebuffer;
    std::shared_ptr<DepthPass>                      m_ShadowDepthPass;
    std::shared_ptr<InstancedOpaqueDrawStrategy>    m_OpaqueDrawStrategy;
    std::unique_ptr<GBufferFillPass>                m_GBufferPass;
    std::unique_ptr<DeferredLightingPass>           m_DeferredLightingPass;
    std::unique_ptr<SkyPass>                        m_SkyPass;
    std::unique_ptr<TemporalAntiAliasingPass>       m_TemporalAntiAliasingPass;
    std::unique_ptr<BloomPass>                      m_BloomPass;
    std::unique_ptr<ToneMappingPass>                m_ToneMappingPass;
    std::unique_ptr<SsaoPass>                       m_SsaoPass;

    // RenderTargets
    std::unique_ptr<RenderTargets>                  m_RenderTargets;

    //Views
    std::shared_ptr<IView>                          m_View;
    bool                                            m_PreviousViewsValid = false;
    std::shared_ptr<IView>                          m_ViewPrevious;
    std::shared_ptr<IView>                          m_TonemappingView;

    // Camera
    FirstPersonCamera                               m_FirstPersonCamera;
    float                                           m_CameraVerticalFov = 60.f;

    // UI
    UIData& m_ui;
    float3                                          m_AmbientTop = 0.f;
    float3                                          m_AmbientBottom = 0.f;

    // For Streamline
    int2                                            m_DisplaySize = { 0, 0 };
    std::default_random_engine                      m_Generator;
    float                                           m_PreviousLodBias;
    affine3                                         m_CameraPreviousMatrix;

    bool                                            m_presentStarted = false;

    // Scripting Behavior
    ScriptingConfig                                 m_ScriptingConfig;

public:
    NVIGISample(DeviceManager* deviceManager, UIData& ui, const std::string& sceneName, ScriptingConfig scriptingConfig);

    // Functions of interest
    bool SetupView();
    void CreateRenderPasses(bool& exposureResetRequired, float lodBias);
    virtual void RenderScene(nvrhi::IFramebuffer* framebuffer) override;

    // Logistic functions 
    std::shared_ptr<TextureCache> GetTextureCache();
    std::vector<std::string> const& GetAvailableScenes() const;
    std::string GetCurrentSceneName() const;
    void SetCurrentSceneName(const std::string& sceneName);
    std::shared_ptr<ShaderFactory> GetShaderFactory() const { return m_ShaderFactory; };
    std::shared_ptr<donut::vfs::IFileSystem> GetRootFs() const { return m_RootFs; };

    virtual bool KeyboardUpdate(int key, int scancode, int action, int mods) override;
    virtual bool MousePosUpdate(double xpos, double ypos) override;
    virtual bool MouseButtonUpdate(int button, int action, int mods) override;
    virtual bool MouseScrollUpdate(double xoffset, double yoffset) override;
    virtual void Animate(float fElapsedTimeSeconds) override;
    virtual void SceneUnloading() override;
    virtual bool LoadScene(std::shared_ptr<IFileSystem> fs, const std::filesystem::path& fileName) override;
    virtual void SceneLoaded() override;
    virtual void RenderSplashScreen(nvrhi::IFramebuffer* framebuffer) override;

};