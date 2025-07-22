// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

#include "NVIGISample.h"
#include <thread>

#if USE_DX12
#include <d3d12.h>
#include <nvrhi/d3d12.h>
#endif

using namespace donut;
using namespace donut::math;
using namespace donut::engine;
using namespace donut::render;
using namespace donut::render;

// Constructor
NVIGISample::NVIGISample(DeviceManager* deviceManager, UIData& ui, const std::string& sceneName, ScriptingConfig scriptingConfig)
    : Super(deviceManager)
    , m_ui(ui)
    , m_BindingCache(deviceManager->GetDevice())
    , m_ScriptingConfig(scriptingConfig)
{

    std::shared_ptr<NativeFileSystem> nativeFS = std::make_shared<NativeFileSystem>();

    std::filesystem::path mediaPath = app::GetDirectoryWithExecutable().parent_path() / "media";
    std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());

    m_RootFs = std::make_shared<RootFileSystem>();
    m_RootFs->mount("/media", mediaPath);
    m_RootFs->mount("/shaders/donut", frameworkShaderPath);
    m_RootFs->mount("/native", nativeFS);

    m_TextureCache = std::make_shared<TextureCache>(GetDevice(), m_RootFs, nullptr);

    m_ShaderFactory = std::make_shared<ShaderFactory>(GetDevice(), m_RootFs, "/shaders");
    m_CommonPasses = std::make_shared<CommonRenderPasses>(GetDevice(), m_ShaderFactory);

    m_OpaqueDrawStrategy = std::make_shared<InstancedOpaqueDrawStrategy>();

    const nvrhi::Format shadowMapFormats[] = {
        nvrhi::Format::D24S8,
        nvrhi::Format::D32,
        nvrhi::Format::D16,
        nvrhi::Format::D32S8 };

    const nvrhi::FormatSupport shadowMapFeatures =
        nvrhi::FormatSupport::Texture |
        nvrhi::FormatSupport::DepthStencil |
        nvrhi::FormatSupport::ShaderLoad;

    nvrhi::Format shadowMapFormat = nvrhi::utils::ChooseFormat(GetDevice(), shadowMapFeatures, shadowMapFormats, std::size(shadowMapFormats));

    m_ShadowMap = std::make_shared<CascadedShadowMap>(GetDevice(), 2048, 4, 0, shadowMapFormat);
    m_ShadowMap->SetupProxyViews();

    m_ShadowFramebuffer = std::make_shared<FramebufferFactory>(GetDevice());
    m_ShadowFramebuffer->DepthTarget = m_ShadowMap->GetTexture();

    DepthPass::CreateParameters shadowDepthParams;
    shadowDepthParams.slopeScaledDepthBias = 4.f;
    shadowDepthParams.depthBias = 100;
    m_ShadowDepthPass = std::make_shared<DepthPass>(GetDevice(), m_CommonPasses);
    m_ShadowDepthPass->Init(*m_ShaderFactory, shadowDepthParams);

    m_CommandList = GetDevice()->createCommandList();

    m_FirstPersonCamera.SetMoveSpeed(3.0f);

    SetAsynchronousLoadingEnabled(false);

    if (sceneName.empty())
        SetCurrentSceneName("/media/sponza-plus.scene.json");
    else
        SetCurrentSceneName("/native/" + sceneName);

    deviceManager->m_callbacks.afterPresent = NVIGIContext::PresentEnd;
};

// Functions of interest

bool NVIGISample::SetupView()
{

    if (m_TemporalAntiAliasingPass) m_TemporalAntiAliasingPass->SetJitter(m_ui.TemporalAntiAliasingJitter);

    float2 pixelOffset = m_TemporalAntiAliasingPass ? m_TemporalAntiAliasingPass->GetCurrentPixelOffset() : float2(0.f);

    std::shared_ptr<PlanarView> planarView = std::dynamic_pointer_cast<PlanarView, IView>(m_View);

    dm::affine3 viewMatrix;
    float verticalFov = dm::radians(m_CameraVerticalFov);
    float zNear = 0.01f;
    viewMatrix = m_FirstPersonCamera.GetWorldToViewMatrix();

    bool topologyChanged = false;

    // Render View
    {
        if (!planarView)
        {
            m_View = planarView = std::make_shared<PlanarView>();
            m_ViewPrevious = std::make_shared<PlanarView>();
            topologyChanged = true;
        }

        float4x4 projection = perspProjD3DStyleReverse(verticalFov, float(m_DisplaySize.x) / m_DisplaySize.y, zNear);

        planarView->SetViewport(nvrhi::Viewport((float) m_DisplaySize.x, (float)m_DisplaySize.y));
        planarView->SetPixelOffset(pixelOffset);

        planarView->SetMatrices(viewMatrix, projection);
        planarView->UpdateCache();

        if (topologyChanged)
        {
            *std::static_pointer_cast<PlanarView>(m_ViewPrevious) = *std::static_pointer_cast<PlanarView>(m_View);
        }
    }

    // ToneMappingView
    {
        std::shared_ptr<PlanarView> tonemappingPlanarView = std::dynamic_pointer_cast<PlanarView, IView>(m_TonemappingView);

        if (!tonemappingPlanarView)
        {
            m_TonemappingView = tonemappingPlanarView = std::make_shared<PlanarView>();
            topologyChanged = true;
        }

        float4x4 projection = perspProjD3DStyleReverse(verticalFov, float(m_DisplaySize.x) / m_DisplaySize.y, zNear);

        tonemappingPlanarView->SetViewport(nvrhi::Viewport((float)m_DisplaySize.x, (float)m_DisplaySize.y));
        tonemappingPlanarView->SetMatrices(viewMatrix, projection);
        tonemappingPlanarView->UpdateCache();
    }

    return topologyChanged;
}

void NVIGISample::CreateRenderPasses(bool& exposureResetRequired, float lodBias)
{
    // Safety measure when we recreate the passes.
    GetDevice()->waitForIdle();

    {
        nvrhi::SamplerDesc samplerdescPoint = m_CommonPasses->m_PointClampSampler->getDesc();
        nvrhi::SamplerDesc samplerdescLinear = m_CommonPasses->m_LinearClampSampler->getDesc();
        nvrhi::SamplerDesc samplerdescLinearWrap = m_CommonPasses->m_LinearWrapSampler->getDesc();
        nvrhi::SamplerDesc samplerdescAniso = m_CommonPasses->m_AnisotropicWrapSampler->getDesc();
        samplerdescPoint.mipBias = lodBias;
        samplerdescLinear.mipBias = lodBias;
        samplerdescLinearWrap.mipBias = lodBias;
        samplerdescAniso.mipBias = lodBias;
        m_CommonPasses->m_PointClampSampler = GetDevice()->createSampler(samplerdescPoint);
        m_CommonPasses->m_LinearClampSampler = GetDevice()->createSampler(samplerdescLinear);
        m_CommonPasses->m_LinearWrapSampler = GetDevice()->createSampler(samplerdescLinearWrap);
        m_CommonPasses->m_AnisotropicWrapSampler = GetDevice()->createSampler(samplerdescAniso);
    }

    uint32_t motionVectorStencilMask = 0x01;

    GBufferFillPass::CreateParameters GBufferParams;
    GBufferParams.enableMotionVectors = true;
    GBufferParams.stencilWriteMask = motionVectorStencilMask;
    m_GBufferPass = std::make_unique<GBufferFillPass>(GetDevice(), m_CommonPasses);
    m_GBufferPass->Init(*m_ShaderFactory, GBufferParams);

    m_DeferredLightingPass = std::make_unique<DeferredLightingPass>(GetDevice(), m_CommonPasses);
    m_DeferredLightingPass->Init(m_ShaderFactory);

    m_SkyPass = std::make_unique<SkyPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets->ForwardFramebuffer, *m_View);

    {
        TemporalAntiAliasingPass::CreateParameters taaParams;
        taaParams.sourceDepth = m_RenderTargets->Depth;
        taaParams.motionVectors = m_RenderTargets->MotionVectors;
        taaParams.unresolvedColor = m_RenderTargets->HdrColor;
        taaParams.resolvedColor = m_RenderTargets->AAResolvedColor;
        taaParams.feedback1 = m_RenderTargets->TemporalFeedback1;
        taaParams.feedback2 = m_RenderTargets->TemporalFeedback2;
        taaParams.motionVectorStencilMask = motionVectorStencilMask;
        taaParams.useCatmullRomFilter = true;

        m_TemporalAntiAliasingPass = std::make_unique<TemporalAntiAliasingPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, *m_View, taaParams);
    }

    m_SsaoPass = std::make_unique<SsaoPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets->Depth, m_RenderTargets->GBufferNormals, m_RenderTargets->AmbientOcclusion); //DIFFERENCE
    nvrhi::BufferHandle exposureBuffer = nullptr;
    if (m_ToneMappingPass)
        exposureBuffer = m_ToneMappingPass->GetExposureBuffer();
    else
        exposureResetRequired = true;

    m_BloomPass = std::make_unique<BloomPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets->HdrFramebuffer, *m_TonemappingView);

    ToneMappingPass::CreateParameters toneMappingParams;
    toneMappingParams.exposureBufferOverride = exposureBuffer;
    m_ToneMappingPass = std::make_unique<ToneMappingPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets->LdrFramebuffer, *m_TonemappingView, toneMappingParams);

    m_PreviousViewsValid = false;
}

void NVIGISample::RenderScene(nvrhi::IFramebuffer* framebuffer)
{

    // INITIALISE

    int windowWidth, windowHeight;
    GetDeviceManager()->GetWindowDimensions(windowWidth, windowHeight);
    nvrhi::Viewport windowViewport = nvrhi::Viewport((float)windowWidth, (float)windowHeight);

    m_Scene->RefreshSceneGraph(GetFrameIndex());

    bool exposureResetRequired = false;
    bool needNewPasses = false;

    m_DisplaySize = int2(windowWidth, windowHeight);
    float lodBias = 0.f;

    // PASS SETUP
    {
        bool needNewPasses = false;

        donut::math::int2 renderSize = m_DisplaySize;

        if (!m_RenderTargets || m_RenderTargets->IsUpdateRequired(renderSize, m_DisplaySize))
        {
            m_BindingCache.Clear();

            m_RenderTargets = nullptr;
            m_RenderTargets = std::make_unique<RenderTargets>();
            m_RenderTargets->Init(GetDevice(), renderSize, m_DisplaySize, framebuffer->getDesc().colorAttachments[0].texture->getDesc().format);

            needNewPasses = true;
        }

        if (SetupView())
        {
            needNewPasses = true;
        }

        if (needNewPasses)
        {
            CreateRenderPasses(exposureResetRequired, lodBias);
        }

    }

    // BEGIN COMMAND LIST
    m_CommandList->open();

    // DO RESETS
    m_Scene->RefreshBuffers(m_CommandList, GetFrameIndex());

    m_RenderTargets->Clear(m_CommandList);

    nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;
    m_CommandList->clearTextureFloat(framebufferTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));

    if (exposureResetRequired)
        m_ToneMappingPass->ResetExposure(m_CommandList, 8.f);

    m_AmbientTop = m_ui.AmbientIntensity * m_ui.SkyParams.skyColor * m_ui.SkyParams.brightness;
    m_AmbientBottom = m_ui.AmbientIntensity * m_ui.SkyParams.groundColor * m_ui.SkyParams.brightness;

    // SHADOW PASS
    if (m_ui.EnableShadows)
    {
        m_SunLight->shadowMap = m_ShadowMap;
        box3 sceneBounds = m_Scene->GetSceneGraph()->GetRootNode()->GetGlobalBoundingBox();

        frustum projectionFrustum = m_View->GetProjectionFrustum();
        projectionFrustum = projectionFrustum.grow(1.f); // to prevent volumetric light leaking
        const float maxShadowDistance = 100.f;

        dm::affine3 viewMatrixInv = m_View->GetChildView(ViewType::PLANAR, 0)->GetInverseViewMatrix();

        float zRange = length(sceneBounds.diagonal()) * 0.5f;
        m_ShadowMap->SetupForPlanarViewStable(*m_SunLight, projectionFrustum, viewMatrixInv, maxShadowDistance, zRange, zRange, m_ui.CsmExponent);

        m_ShadowMap->Clear(m_CommandList);

        DepthPass::Context context;

        RenderCompositeView(m_CommandList,
            &m_ShadowMap->GetView(), nullptr,
            *m_ShadowFramebuffer,
            m_Scene->GetSceneGraph()->GetRootNode(),
            *m_OpaqueDrawStrategy,
            *m_ShadowDepthPass,
            context,
            "ShadowMap");
    }
    else
    {
        m_SunLight->shadowMap = nullptr;
    }

    // Do CPU Load
    if (m_ui.CpuLoad != 0) {
        
        static std::random_device rd;
        static std::mt19937 mt(rd());
        static std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        
        
        float waitMs = 0;
        float prob = dist(mt);
        if (prob > 0.5f)
        {
            waitMs = m_ui.CpuLoad * dist(mt);
            auto start = std::chrono::high_resolution_clock::now();
            while ((std::chrono::high_resolution_clock::now() - start).count() / 1e6 < waitMs);
        }
    }

    // Deffered Shading
    {
        // DO GBUFFER
        GBufferFillPass::Context gbufferContext;

        for (auto i = 0; i <= m_ui.GpuLoad; ++i) {
            RenderCompositeView(m_CommandList,
                m_View.get(), m_ViewPrevious.get(),
                *m_RenderTargets->GBufferFramebuffer,
                m_Scene->GetSceneGraph()->GetRootNode(),
                *m_OpaqueDrawStrategy,
                *m_GBufferPass,
                gbufferContext,
                "GBufferFill");
        }

        // DO MOTION VECTORS
        if (m_PreviousViewsValid) m_TemporalAntiAliasingPass->RenderMotionVectors(m_CommandList, *m_View, *m_ViewPrevious);

        // DO SSAO
        nvrhi::ITexture* ambientOcclusionTarget = nullptr;
        if (m_ui.EnableSsao && m_SsaoPass)
        {
            m_SsaoPass->Render(m_CommandList, m_ui.SsaoParams, *m_View);
            ambientOcclusionTarget = m_RenderTargets->AmbientOcclusion;
        }

        // DO DEFFERED
        DeferredLightingPass::Inputs deferredInputs;
        deferredInputs.SetGBuffer(*m_RenderTargets);
        deferredInputs.ambientOcclusion = m_ui.EnableSsao ? m_RenderTargets->AmbientOcclusion : nullptr;
        deferredInputs.ambientColorTop = m_AmbientTop;
        deferredInputs.ambientColorBottom = m_AmbientBottom;
        deferredInputs.lights = &m_Scene->GetSceneGraph()->GetLights();
        deferredInputs.output = m_RenderTargets->HdrColor;

        m_DeferredLightingPass->Render(m_CommandList, *m_View, deferredInputs);
    }

    if (m_ui.EnableProceduralSky)
        m_SkyPass->Render(m_CommandList, *m_View, *m_SunLight, m_ui.SkyParams);

    // DO BLOOM
    if (m_ui.EnableBloom) m_BloomPass->Render(m_CommandList, m_RenderTargets->HdrFramebuffer, *m_View, m_RenderTargets->HdrColor, m_ui.BloomSigma, m_ui.BloomAlpha);

    // ANTI-ALIASING

    if (m_ui.AAMode != AntiAliasingMode::NONE) {
        // DO TAA
        if (m_ui.AAMode == AntiAliasingMode::TEMPORAL)
        {
            m_TemporalAntiAliasingPass->TemporalResolve(m_CommandList, m_ui.TemporalAntiAliasingParams, m_PreviousViewsValid, *m_View, m_PreviousViewsValid ? *m_ViewPrevious : *m_View);
        }

        m_PreviousViewsValid = true;
    }
    else
    {
        // IF YOU DO NOTHING SPECIAL -> FORWARD TEXTURE
        m_CommonPasses->BlitTexture(m_CommandList, m_RenderTargets->AAResolvedFramebuffer->GetFramebuffer(*m_View), m_RenderTargets->HdrColor, &m_BindingCache);
        m_PreviousViewsValid = false;
    }

    //DO TONEMAPPING
    nvrhi::ITexture* texToDisplay;
    if (m_ui.EnableToneMapping)
    {
        auto toneMappingParams = m_ui.ToneMappingParams;
        if (exposureResetRequired)
        {
            toneMappingParams.minAdaptedLuminance = 0.1f;
            toneMappingParams.eyeAdaptationSpeedDown = 0.0f;
        }
        m_ToneMappingPass->SimpleRender(m_CommandList, toneMappingParams, *m_TonemappingView, m_RenderTargets->AAResolvedColor);

        m_CommandList->copyTexture(m_RenderTargets->ColorspaceCorrectionColor, nvrhi::TextureSlice(), m_RenderTargets->LdrColor, nvrhi::TextureSlice());
        texToDisplay = m_RenderTargets->ColorspaceCorrectionColor;
    }
    else {
        texToDisplay = m_RenderTargets->AAResolvedColor;
    }

    // MOVE TO PPRE_UI
    m_CommonPasses->BlitTexture(m_CommandList, m_RenderTargets->PreUIFramebuffer->GetFramebuffer(*m_View), texToDisplay, &m_BindingCache);

    // Copy to framebuffer
    m_CommandList->copyTexture(framebufferTexture, nvrhi::TextureSlice(), m_RenderTargets->PreUIColor, nvrhi::TextureSlice());

    // CLOSE COMMANDLIST
    m_CommandList->close();
    GetDevice()->executeCommandList(m_CommandList);

    // CLEANUP
    {

        m_TemporalAntiAliasingPass->AdvanceFrame();

        std::swap(m_View, m_ViewPrevious);

        m_CameraPreviousMatrix = m_FirstPersonCamera.GetWorldToViewMatrix();
    }

    // CLOSE: 
    if (GetFrameIndex() == m_ScriptingConfig.maxFrames)
        glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(), GLFW_TRUE);
}

// Logistic functions 

std::shared_ptr<TextureCache> NVIGISample::GetTextureCache()
{
    return m_TextureCache;
}

std::vector<std::string> const& NVIGISample::GetAvailableScenes() const
{
    return m_SceneFilesAvailable;
}

std::string NVIGISample::GetCurrentSceneName() const
{
    return m_CurrentSceneName;
}

void NVIGISample::SetCurrentSceneName(const std::string& sceneName)
{
    if (m_CurrentSceneName == sceneName)
        return;

    m_CurrentSceneName = sceneName;

    BeginLoadingScene(m_RootFs, m_CurrentSceneName);
}

bool NVIGISample::KeyboardUpdate(int key, int scancode, int action, int mods)
{

    if (key == GLFW_KEY_SPACE && action == GLFW_PRESS) {
        m_ui.EnableAnimations = !m_ui.EnableAnimations;
    }

    m_FirstPersonCamera.KeyboardUpdate(key, scancode, action, mods);
    return true;
}

bool NVIGISample::MousePosUpdate(double xpos, double ypos)
{
    m_FirstPersonCamera.MousePosUpdate(xpos, ypos);
    return true;
}

bool NVIGISample::MouseButtonUpdate(int button, int action, int mods)
{

    m_FirstPersonCamera.MouseButtonUpdate(button, action, mods);
    return true;
}

bool NVIGISample::MouseScrollUpdate(double xoffset, double yoffset)
{
    m_FirstPersonCamera.MouseScrollUpdate(xoffset, yoffset);
    return true;
}

void NVIGISample::Animate(float fElapsedTimeSeconds)
{
    m_FirstPersonCamera.Animate(fElapsedTimeSeconds);

    if (m_ToneMappingPass)
        m_ToneMappingPass->AdvanceFrame(fElapsedTimeSeconds);

    if (IsSceneLoaded() && m_ui.EnableAnimations)
    {
        m_WallclockTime += fElapsedTimeSeconds;

        for (const auto& anim : m_Scene->GetSceneGraph()->GetAnimations())
        {
            float duration = anim->GetDuration();
            float integral;
            float animationTime = std::modf(m_WallclockTime / duration, &integral) * duration;
            (void)anim->Apply(animationTime);
        }
    }
}

void NVIGISample::SceneUnloading()
{
    if (m_DeferredLightingPass) m_DeferredLightingPass->ResetBindingCache();
    if (m_GBufferPass) m_GBufferPass->ResetBindingCache();
    if (m_ShadowDepthPass) m_ShadowDepthPass->ResetBindingCache();
    m_BindingCache.Clear();
    m_SunLight.reset();
}

bool NVIGISample::LoadScene(std::shared_ptr<IFileSystem> fs, const std::filesystem::path& fileName)
{
    using namespace std::chrono;

    Scene* scene = new Scene(GetDevice(), *m_ShaderFactory, fs, m_TextureCache, nullptr, nullptr);

    auto startTime = high_resolution_clock::now();

    if (scene->Load(fileName))
    {
        m_Scene = std::unique_ptr<Scene>(scene);

        auto endTime = high_resolution_clock::now();
        auto duration = duration_cast<milliseconds>(endTime - startTime).count();
        log::info("Scene loading time: %llu ms", duration);

        return true;
    }

    return false;
}

void NVIGISample::SceneLoaded()
{
    Super::SceneLoaded();

    m_Scene->FinishedLoading(GetFrameIndex());

    m_WallclockTime = 0.f;
    m_PreviousViewsValid = false;

    for (auto light : m_Scene->GetSceneGraph()->GetLights())
    {
        if (light->GetLightType() == LightType_Directional)
        {
            m_SunLight = std::static_pointer_cast<DirectionalLight>(light);
            break;
        }
    }

    if (!m_SunLight)
    {
        m_SunLight = std::make_shared<DirectionalLight>();
        m_SunLight->angularSize = 0.53f;
        m_SunLight->irradiance = 1.f;

        auto node = std::make_shared<SceneGraphNode>();
        node->SetLeaf(m_SunLight);
        m_SunLight->SetDirection(dm::double3(0.1, -0.9, 0.1));
        m_SunLight->SetName("Sun");
        m_Scene->GetSceneGraph()->Attach(m_Scene->GetSceneGraph()->GetRootNode(), node);
    }

    m_FirstPersonCamera.LookAt(float3(0.f, 1.8f, 0.f), float3(1.f, 1.8f, 0.f));
    m_CameraVerticalFov = 60.f;

}

void NVIGISample::RenderSplashScreen(nvrhi::IFramebuffer* framebuffer)
{
    nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;
    m_CommandList->open();
    m_CommandList->clearTextureFloat(framebufferTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));
    m_CommandList->close();
    GetDevice()->executeCommandList(m_CommandList);
    GetDeviceManager()->SetVsyncEnabled(true);
}

