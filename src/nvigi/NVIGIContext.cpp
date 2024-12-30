// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING 1

#include "NVIGIContext.h"

#include <donut/core/math/math.h>
#include <donut/core/math/basics.h>
#include <imgui.h>
#include <donut/app/imgui_renderer.h>

#if USE_DX12
#include <d3d12.h>
#include <nvrhi/d3d12.h>
#endif


// NVIGI
#include <nvigi.h>
#include <nvigi_ai.h>
#include <nvigi_hwi_cuda.h>
#include <nvigi_gpt.h>
#include <nvigi_asr_whisper.h>
#include <nvigi_cloud.h>
#include <nvigi_gpt_onnxgenai.h>
#include <nvigi_security.h>
#include <source/utils/nvigi.dsound/player.h>

#include <assert.h>
#include <atomic>
#include <codecvt>
#include <filesystem>
#include <mutex>
#include <regex>
#include <string>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#include <cstdio>
#include <climits>
#else
#define PATH_MAX MAX_PATH
#endif // _WIN32

namespace fs = std::filesystem;

struct Message {
    enum class Type {
        Question,
        Answer
    } type;
    std::string text;
};

static std::vector<Message> messages =
{
};

constexpr ImU32 TITLE_COL = IM_COL32(0, 255, 0, 255);

// NVIGI core functions
PFun_nvigiInit* m_nvigiInit{};
PFun_nvigiShutdown* m_nvigiShutdown{};
PFun_nvigiLoadInterface* m_nvigiLoadInterface{};
PFun_nvigiUnloadInterface* m_nvigiUnloadInterface{};

static std::wstring GetNVIGICoreDllLocation() {

    char path[PATH_MAX] = { 0 };
#ifdef _WIN32
    if (GetModuleFileNameA(nullptr, path, dim(path)) == 0)
        return std::wstring();
#else // _WIN32
    // /proc/self/exe is mostly linux-only, but can't hurt to try it elsewhere
    if (readlink("/proc/self/exe", path, std::size(path)) <= 0)
    {
        // portable but assumes executable dir == cwd
        if (!getcwd(path, std::size(path)))
            return ""; // failure
    }
#endif // _WIN32

    auto basePath = std::filesystem::path(path).parent_path();
    auto dllPath = basePath.wstring().append(L"\\nvigi.core.framework.dll");
    return dllPath;
}

NVIGIContext& NVIGIContext::Get() {
    static NVIGIContext instance;
    return instance;
}

bool NVIGIContext::CheckPluginCompat(nvigi::PluginID id, const std::string& name)
{
    const nvigi::AdapterSpec* adapterInfo = (m_adapter >= 0) ? m_pluginInfo->detectedAdapters[m_adapter] : nullptr;

    // find the plugin - make sure it is even there and can be supported...
    for (int i = 0; i < m_pluginInfo->numDetectedPlugins; i++)
    {
        auto& plugin = m_pluginInfo->detectedPlugins[i];

        if (plugin->id == id)
        {
            if (plugin->requiredAdapterVendor != nvigi::VendorId::eAny && plugin->requiredAdapterVendor != nvigi::VendorId::eNone && 
                (!adapterInfo || plugin->requiredAdapterVendor != adapterInfo->vendor))
            {
                donut::log::error("Plugin %s could not be loaded on adapters from this GPU vendor (found %0x, requires %0x)", name.c_str(),
                    adapterInfo->vendor, plugin->requiredAdapterVendor);
                return false;
            }

            if (plugin->requiredAdapterVendor == nvigi::VendorId::eNVDA && plugin->requiredAdapterArchitecture > adapterInfo->architecture)
            {
                donut::log::error("Plugin %s could not be loaded on this GPU architecture (found %d, requires %d)", name.c_str(),
                    adapterInfo->architecture, plugin->requiredAdapterArchitecture);
                return false;
            }

            if (plugin->requiredAdapterVendor == nvigi::VendorId::eNVDA && plugin->requiredAdapterDriverVersion > adapterInfo->driverVersion)
            {
                donut::log::error("Plugin %s could not be loaded on this driver (found %d.%d, requires %d.%d)", name.c_str(),
                    adapterInfo->driverVersion.major, adapterInfo->driverVersion.minor,
                    plugin->requiredAdapterDriverVersion.major, plugin->requiredAdapterDriverVersion.minor);
                return false;
            }

            return true;
        }
    }

    // Not found
    donut::log::error("Plugin %s could not be loaded", name.c_str());

    return false;
}

bool NVIGIContext::AddGPTPlugin(nvigi::PluginID id, const std::string& name, const std::string& modelRoot)
{
    if (CheckPluginCompat(id, name))
    {
        nvigi::IGeneralPurposeTransformer* igpt{};
        nvigi::Result nvigiRes = nvigiGetInterfaceDynamic(id, &igpt, m_nvigiLoadInterface);
        if (nvigiRes != nvigi::kResultOk)
            return false;

        nvigi::GPTCreationParameters* params1 = GetGPTCreationParams(true, &modelRoot);
        if (!params1)
            return false;

        nvigi::CommonCapabilitiesAndRequirements* models{};
        nvigi::getCapsAndRequirements(igpt, *params1, &models);
        if (!models)
        {
            m_nvigiUnloadInterface(id, igpt);
            FreeCreationParams(params1);
            return false;
        }

        for (uint32_t i = 0; i < models->numSupportedModels; i++)
        {
            PluginModelInfo* info = new PluginModelInfo;
            info->m_featureID = id;
            info->m_modelName = models->supportedModelNames[i];
            info->m_pluginName = name;
            info->m_caption = name + " : " + models->supportedModelNames[i];
            info->m_guid = models->supportedModelGUIDs[i];
            info->m_modelRoot = modelRoot;
            info->m_modelStatus = (models->modelFlags[i] & nvigi::kModelFlagRequiresDownload) 
                ? ModelStatus::AVAILABLE_MANUAL_DOWNLOAD : ModelStatus::AVAILABLE_LOCALLY;
            m_gptPluginModels.push_back(info);
        }

        m_nvigiUnloadInterface(id, igpt);
        FreeCreationParams(params1);
        return true;
    }

    return false;
}

bool NVIGIContext::AddGPTCloudPlugin()
{
    nvigi::PluginID id = nvigi::plugin::gpt::cloud::rest::kId;
    const std::string name = "cloud.rest";

    if (CheckPluginCompat(id, name))
    {
        nvigi::IGeneralPurposeTransformer* igpt{};
        nvigi::Result nvigiRes = nvigiGetInterfaceDynamic(id, &igpt, m_nvigiLoadInterface);
        if (nvigiRes != nvigi::kResultOk)
            return false;

        nvigi::GPTCreationParameters* params1 = GetGPTCreationParams(true);
        if (!params1)
            return false;

        nvigi::CommonCapabilitiesAndRequirements* models{};
        nvigi::getCapsAndRequirements(igpt, *params1, &models);
        if (!models)
        {
            m_nvigiUnloadInterface(id, igpt);
            FreeCreationParams(params1);
            return false;
        }

        std::vector<std::tuple<std::string, std::string>> cloudItems;

        for (uint32_t i = 0; i < models->numSupportedModels; i++)
            cloudItems.push_back({ models->supportedModelGUIDs[i], models->supportedModelNames[i] });

        auto commonParams = nvigi::findStruct<nvigi::CommonCreationParameters>(*params1);
        
        for (auto& item : cloudItems)
        {
            auto guid = std::get<0>(item);
            auto modelName = std::get<1>(item);
            commonParams->modelGUID = guid.c_str();
            nvigi::getCapsAndRequirements(igpt, *params1, &models);
            auto cloudCaps = nvigi::findStruct<nvigi::CloudCapabilities>(*models);
            
            PluginModelInfo* info = new PluginModelInfo;
            info->m_featureID = id;
            info->m_modelName = modelName;
            info->m_pluginName = name;
            info->m_caption = name + " : " + modelName;
            info->m_guid = guid;
            info->m_modelRoot = m_shippedModelsPath;
            info->m_modelStatus = ModelStatus::AVAILABLE_CLOUD;
            info->m_url = cloudCaps->url;
            m_gptPluginModels.push_back(info);
        }

        m_nvigiUnloadInterface(id, igpt);
        FreeCreationParams(params1);
        return true;
    }

    return false;
}


bool NVIGIContext::AddASRPlugin(nvigi::PluginID id, const std::string& name, const std::string& modelRoot)
{
    if (CheckPluginCompat(id, name))
    {
        nvigi::IAutoSpeechRecognition* iasr{};
        nvigi::Result nvigiRes = nvigiGetInterfaceDynamic(id, &iasr, m_nvigiLoadInterface);
        if (nvigiRes != nvigi::kResultOk)
            return false;

        nvigi::ASRWhisperCreationParameters* params1 = GetASRCreationParams(true, &modelRoot);
        if (!params1)
            return false;

        nvigi::ASRWhisperCapabilitiesAndRequirements* caps{};
        nvigi::getCapsAndRequirements(iasr, *params1, &caps);
        if (!caps)
        {
            m_nvigiUnloadInterface(id, iasr);
            FreeCreationParams(params1);
            return false;
        }

        nvigi::CommonCapabilitiesAndRequirements& models = *(caps->common);
        for (uint32_t i = 0; i < models.numSupportedModels; i++)
        {
            PluginModelInfo* info = new PluginModelInfo;
            info->m_featureID = id;
            info->m_modelName = models.supportedModelNames[i];
            info->m_pluginName = name;
            info->m_caption = name + " : " + models.supportedModelNames[i];
            info->m_guid = models.supportedModelGUIDs[i];
            info->m_modelRoot = modelRoot;
            info->m_modelStatus = (models.modelFlags[i] & nvigi::kModelFlagRequiresDownload)
                ? ModelStatus::AVAILABLE_MANUAL_DOWNLOAD : ModelStatus::AVAILABLE_LOCALLY;
            m_asrPluginModels.push_back(info);
        }

        m_nvigiUnloadInterface(id, iasr);
        FreeCreationParams(params1);
        return true;
    }

    return false;
}

bool NVIGIContext::Initialize_preDeviceManager(nvrhi::GraphicsAPI api, int argc, const char* const* argv)
{
    m_api = api;

    // Hack for now, as we don't really want to check the sigs
#ifdef NVIGI_PRODUCTION
    bool checkSig = true;
#else
    bool checkSig = false;
#endif
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-pathToModels"))
        {
            m_shippedModelsPath = argv[++i];
        }
        else if (!strcmp(argv[i], "-noSigCheck"))
        {
            checkSig = false;
        }
        else if (!strcmp(argv[i], "-logToFile"))
        {
            m_LogFilename = argv[++i];
        }
        else if (!strcmp(argv[i], "-noCiG") || !strcmp(argv[i], "-noCIG"))
        {
            m_useCiG = false;
        }
    }

    auto pathNVIGIDll = GetNVIGICoreDllLocation();

    HMODULE nvigiCore = {};
    if (checkSig) {
        donut::log::info("Checking NVIGI core DLL signature");
        if (!nvigi::security::verifyEmbeddedSignature(pathNVIGIDll.c_str())) {
            donut::log::error("NVIGI core DLL is not signed - disable signature checking with -noSigCheck or use a signed NVIGI core DLL");
            return false;
        }
    }
    nvigiCore = LoadLibraryW(pathNVIGIDll.c_str());
   
    if (!nvigiCore)
    {
        donut::log::error("Unable to load NVIGI core");
        return false;
    }

    m_nvigiInit = (PFun_nvigiInit*)GetProcAddress(nvigiCore, "nvigiInit");
    m_nvigiShutdown = (PFun_nvigiShutdown*)GetProcAddress(nvigiCore, "nvigiShutdown");
    m_nvigiLoadInterface = (PFun_nvigiLoadInterface*)GetProcAddress(nvigiCore, "nvigiLoadInterface");
    m_nvigiUnloadInterface = (PFun_nvigiUnloadInterface*)GetProcAddress(nvigiCore, "nvigiUnloadInterface");

    {
        wchar_t path[PATH_MAX] = { 0 };
        GetModuleFileNameW(nullptr, path, dim(path));
        auto basePath = std::filesystem::path(path).parent_path();
        static std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> convert;
        m_appUtf8path = convert.to_bytes(basePath);
        nvigi::Preferences nvigiPref;
        const char* paths[] =
        {
           m_appUtf8path.c_str(),
        };
        nvigiPref.logLevel = nvigi::LogLevel::eVerbose;
        nvigiPref.showConsole = true;
        nvigiPref.numPathsToPlugins = _countof(paths);
        nvigiPref.utf8PathsToPlugins = paths;

        if (!m_LogFilename.empty())
        {
            nvigiPref.utf8PathToLogsAndData = m_LogFilename.c_str();
        }

        auto result = m_nvigiInit(nvigiPref, &m_pluginInfo, nvigi::kSDKVersion);
    }

    uint32_t nvdaArch = 0;
    for (int i = 0; i < m_pluginInfo->numDetectedAdapters; i++)
    {
        auto& adapter = m_pluginInfo->detectedAdapters[i];
        if (adapter->vendor == nvigi::VendorId::eNVDA && nvdaArch < adapter->architecture)
        {
            nvdaArch = adapter->architecture;
            m_adapter = i;
        }
    }

    if (m_adapter < 0)
    {
        donut::log::error("No NVIDIA adapters found.  GPU plugins will not be available\n");
        if (m_pluginInfo->numDetectedAdapters)
            m_adapter = 0;
    }

    AddGPTPlugin(nvigi::plugin::gpt::ggml::cuda::kId, "ggml.cuda", m_shippedModelsPath);

    AddGPTCloudPlugin();

    AddGPTPlugin(nvigi::plugin::gpt::onnxgenai::dml::kId, "onnxgenai", m_shippedModelsPath);

    if (AnyGPTModelsAvailable())
    {
        m_gptIndex = 0;
        for (auto& model : m_gptPluginModels)
        {
            // For now, a model can be the default ONLY if it is locally-available
            if (model->m_modelStatus == ModelStatus::AVAILABLE_LOCALLY)
                break;
            m_gptIndex++;
        }
        // If the available ones were downloadable
        if (m_gptIndex >= m_gptPluginModels.size())
            m_gptIndex = -1;
    }

    if (m_gptIndex == -1)
    {
        donut::log::error("Warning: No local (non-cloud) supported GPT/LLM models available.  Please download a local-inference LLM model.\n");
    }

    AddASRPlugin(nvigi::plugin::asr::ggml::cuda::kId, "ggml.cuda", m_shippedModelsPath);
    AddASRPlugin(nvigi::plugin::asr::ggml::cpu::kId, "ggml.cpu", m_shippedModelsPath);

    if (AnyASRModelsAvailable())
    {
        m_asrIndex = 0;
        for (auto& model : m_asrPluginModels)
        {
            // For now, a model can be the default ONLY if it is locally-available
            if (model->m_modelStatus == ModelStatus::AVAILABLE_LOCALLY)
                break;
            m_asrIndex++;
        }
        // If the available ones were downloadable
        if (m_asrIndex >= m_asrPluginModels.size())
            m_asrIndex = -1;
    }

    if (m_asrIndex == -1)
    {
        donut::log::error("Warning: No local (non-cloud) supported ASR models available.  Please download a local-inference ASR model\n");
    }

    m_gptCallbackState.store(nvigi::kInferenceExecutionStateInvalid);

    messages.push_back({ Message::Type::Answer, "I'm here to chat - type a query or record audio to interact!" });

    return true;
}

bool NVIGIContext::Initialize_preDeviceCreate(donut::app::DeviceManager* deviceManager, donut::app::DeviceCreationParameters& params)
{
#if USE_DX12
    if (m_api == nvrhi::GraphicsAPI::D3D11 || m_api == nvrhi::GraphicsAPI::D3D12)
    {
        donut::app::InstanceParameters instParams{};
#ifdef _DEBUG
        instParams.enableDebugRuntime = true;
#endif
        if (!deviceManager->CreateInstance(instParams))
            return false;

        std::vector<donut::app::AdapterInfo> outAdapters;
        if (!deviceManager->EnumerateAdapters(outAdapters))
            return false;

        nvrhi::RefCountPtr<IDXGIAdapter> dxgiAdapter;
        uint32_t index = 0;
        for (auto& adapterDesc : outAdapters)
        {
            if (adapterDesc.vendorID == 4318)
            {
                dxgiAdapter = adapterDesc.dxgiAdapter;
                params.adapterIndex = index;
                break;
            }
            index++;
        }

        if (dxgiAdapter)
            if (S_OK != dxgiAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)m_targetAdapter.GetAddressOf()))
                return false;
    }
#endif

    return true;
}

bool NVIGIContext::Initialize_postDevice()
{
    auto readFile = [](const char* fname)->std::vector<uint8_t>
        {
            fs::path p(fname);
            size_t file_size = fs::file_size(p);
            std::vector<uint8_t> ret_buffer(file_size);
            std::fstream file(fname, std::ios::binary | std::ios::in);
            file.read((char*)ret_buffer.data(), file_size);
            return ret_buffer;
        };

    // Setup CiG
    if (m_useCiG)
    {
        // Because of a current bug, we can't create and
        // destroy the CIG context many times in one app (yet). The CIG context is owned by
        // the HWI.cuda plugin, so we need to keep it alive between tests by 
        // getting it here.
        nvigiGetInterfaceDynamic(nvigi::plugin::hwi::cuda::kId, &m_cig, m_nvigiLoadInterface);

        if (m_D3D12Queue != nullptr)
        {
            m_d3d12Params = new nvigi::D3D12Parameters;
            m_d3d12Params->device = m_Device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
            m_d3d12Params->queue = m_D3D12Queue;
        }
    }
    else
    {
        donut::log::info("Not using a shared CUDA context - CiG disabled");
    }


    //! Load A2T script    
    auto loadASR = [this]()->HRESULT
        {
            PluginModelInfo* gptInfo = GetGPTPluginModel(m_gptIndex);

            if (gptInfo)
            {
                nvigi::GPTCreationParameters* params1 = GetGPTCreationParams(false);
                nvigi::Result nvigiRes = nvigiGetInterfaceDynamic(gptInfo->m_featureID, &m_igpt, m_nvigiLoadInterface);
                if (nvigiRes == nvigi::kResultOk)
                    nvigiRes = m_igpt->createInstance(*params1, &m_gpt);
                if (nvigiRes != nvigi::kResultOk)
                {
                    donut::log::error("Unable to create GPT instance/model.  See log for details.  Most common issue is incorrect path to models");
                }

                m_gptReady.store(nvigiRes == nvigi::kResultOk);
                FreeCreationParams(params1);
            }
            else
            {
                m_gptReady.store(false);
            }

            PluginModelInfo* asrInfo = GetASRPluginModel(m_asrIndex);

            if (asrInfo)
            {
                nvigi::ASRWhisperCreationParameters* params2 = GetASRCreationParams(false);
                nvigi::Result nvigiRes = nvigiGetInterfaceDynamic(asrInfo->m_featureID, &m_iasr, m_nvigiLoadInterface);
                if (nvigiRes == nvigi::kResultOk)
                    nvigiRes = m_iasr->createInstance(*params2, &m_asr);
                if (nvigiRes != nvigi::kResultOk)
                {
                    donut::log::error("Unable to create ASR instance/model.  See log for details.  Most common issue is incorrect path to models");
                }

                m_asrReady.store(nvigiRes == nvigi::kResultOk);
                FreeCreationParams(params2);
            }
            else
            {
                m_asrReady.store(false);
            }

            return S_OK;
        };
    m_loadingThread = new std::thread{ loadASR };

    return true;
}

void NVIGIContext::SetDevice_nvrhi(nvrhi::IDevice* device)
{
    m_Device = device;
    if (m_Device)
        m_D3D12Queue = m_Device->getNativeQueue(nvrhi::ObjectTypes::D3D12_CommandQueue, nvrhi::CommandQueue::Graphics);
}

void NVIGIContext::Shutdown()
{
    //    t1->join();
    m_loadingThread->join();
    //  delete t1;
    delete m_loadingThread;

    if (m_d3d12Params)
    {
        delete m_d3d12Params;
        m_d3d12Params = nullptr;
    }

    m_nvigiUnloadInterface(nvigi::plugin::hwi::cuda::kId, m_cig);
    m_cig = nullptr;
}

template <typename T> void NVIGIContext::FreeCreationParams(T* params)
{
    if (!params)
        return;
    auto base = reinterpret_cast<const nvigi::BaseStructure*>(params);
    while (base)
    {
        auto kill = base;
        base = static_cast<const nvigi::BaseStructure*>(base->next);
        delete kill;
    }
}

nvigi::GPTCreationParameters* NVIGIContext::GetGPTCreationParams(bool genericInit, const std::string* modelRoot)
{
    PluginModelInfo* info = nullptr;
    
    if (!genericInit)
    {
        info = GetGPTPluginModel(m_gptIndex);
        if (!info)
            return nullptr;
    }

    nvigi::CommonCreationParameters* common1 = new nvigi::CommonCreationParameters;

    common1->numThreads = 1;
    common1->vramBudgetMB = 1024 * 8;
    // Priority order of model roots:
    // if we've been passed a model root, use it
    // else if there's a model with info, use its root
    // all else fails, use the shipped models
    common1->utf8PathToModels = modelRoot ? modelRoot->c_str() :
        (info ? info->m_modelRoot.c_str() : m_shippedModelsPath.c_str());
    common1->modelGUID = info ? info->m_guid.c_str() : nullptr;

    nvigi::GPTCreationParameters* params1 = new nvigi::GPTCreationParameters;

    if (m_d3d12Params)
    {
        nvigi::D3D12Parameters* d3d12Params = new nvigi::D3D12Parameters;
        d3d12Params->device = m_d3d12Params->device;
        d3d12Params->queue = m_d3d12Params->queue;
        params1->chain(*d3d12Params);
    }

    params1->chain(*common1);
    params1->seed = -1;
    params1->maxNumTokensToPredict = 200;
    params1->contextSize = 4096;

    if (genericInit)
        return params1;

    if (info->m_featureID == nvigi::plugin::gpt::onnxgenai::dml::kId)
    {
        nvigi::GPTOnnxgenaiCreationParameters* onnxgenaiParamsPtr = new nvigi::GPTOnnxgenaiCreationParameters;
        nvigi::GPTOnnxgenaiCreationParameters& onnxgenaiParams = *onnxgenaiParamsPtr;
        onnxgenaiParams.backgroundMode = false;
        onnxgenaiParams.allowAsync = false;
        params1->chain(onnxgenaiParams);
    }
    else if (info->m_featureID == nvigi::plugin::gpt::cloud::rest::kId)
    {
        const char* key = nullptr;
        if (info->m_url.find("integrate.api.nvidia.com") != std::string::npos)
        {
            key = getenv("NVIDIA_INTEGRATE_KEY");
            if (key == NULL) {
                donut::log::error("NVIDIA Integrate API key not found at NVIDIA_INTEGRATE_KEY; cloud model will not be available");
                FreeCreationParams(params1);
                return nullptr;
            }
        }
        else if (info->m_url.find("openai.com") != std::string::npos)
        {
            key = getenv("OPENAI_KEY");
            if (key == NULL) {
                donut::log::error("OpenAI API key not found at OPENAI_KEY; cloud model will not be available");
                FreeCreationParams(params1);
                return nullptr;
            }
        }
        else
        {
            donut::log::error("Unknown cloud model URL (%s); cannot send authentication token", info->m_url.c_str());
        }

        //! Cloud parameters
        nvigi::RESTParameters* nvcfParams = new nvigi::RESTParameters;
        nvcfParams->url = info->m_url.c_str();
        nvcfParams->authenticationToken = key;
        nvcfParams->verboseMode = true;
        params1->chain(*nvcfParams);
    }

    return params1;
}

nvigi::ASRWhisperCreationParameters* NVIGIContext::GetASRCreationParams(bool genericInit, const std::string* modelRoot)
{
    PluginModelInfo* info = nullptr;

    if (!genericInit)
    {
        info = GetASRPluginModel(m_asrIndex);
        if (!info)
            return nullptr;
    }

    nvigi::CommonCreationParameters* common1 = new nvigi::CommonCreationParameters;
    common1->numThreads = 4;
    common1->vramBudgetMB = 1024 * 3;
    // Priority order of model roots:
    // if we've been passed a model root, use it
    // else if there's a model with info, use its root
    // all else fails, use the shipped models
    common1->utf8PathToModels = modelRoot ? modelRoot->c_str() :
        (info ? info->m_modelRoot.c_str() : m_shippedModelsPath.c_str());

    nvigi::ASRWhisperCreationParameters* params1 = new nvigi::ASRWhisperCreationParameters;

    if (m_d3d12Params)
    {
        nvigi::D3D12Parameters* d3d12Params = new nvigi::D3D12Parameters;
        d3d12Params->device = m_d3d12Params->device;
        d3d12Params->queue = m_d3d12Params->queue;
        params1->chain(*d3d12Params);
    }

    params1->chain(*common1);

    if (genericInit)
        return params1;

    common1->modelGUID = info->m_guid.c_str();

    return params1;
}

void NVIGIContext::ReloadGPTModel(int32_t index)
{
    if (m_loadingThread)
    {
        m_loadingThread->join();
        delete m_loadingThread;
        m_loadingThread = nullptr;
    }

    m_conversationInitialized = false;

    int32_t prevIndex = m_gptIndex;
    PluginModelInfo* prevGptInfo = GetGPTPluginModel(prevIndex);

    m_gptIndex = index;
    PluginModelInfo* newGptInfo = GetGPTPluginModel(m_gptIndex);

    nvigi::GPTCreationParameters* params1 = GetGPTCreationParams(false);
    // This will be null if there is an error OR if the new model is being downloaded
    if (!params1)
    {
        m_gptIndex = prevIndex;
        return;
    }

    m_gptReady.store(false);

    if (m_igpt)
    {
        m_igpt->destroyInstance(m_gpt);
        m_gpt = {};
    }

    auto loadModel = [this, prevIndex, prevGptInfo, newGptInfo, params1]()->void
        {
            nvigi::GPTCreationParameters* params = params1;
            if (params)
            {
                cerr_redirect ggmlLog;
                nvigi::Result nvigiRes = nvigiGetInterfaceDynamic(newGptInfo->m_featureID, &m_igpt, m_nvigiLoadInterface);
                if (nvigiRes == nvigi::kResultOk)
                    nvigiRes = m_igpt->createInstance(*params, &m_gpt);
                if (nvigiRes != nvigi::kResultOk)
                {
                    FreeCreationParams(params);
                    donut::log::error("Unable to create GPT instance/model.  See log for details.  Most common issue is incorrect path to models.  Reverting to previous GPT instance/model");
                    m_gptIndex = prevIndex;
                    params = GetGPTCreationParams(false);
                    if (params && prevGptInfo)
                    {
                        nvigiRes = nvigiGetInterfaceDynamic(prevGptInfo->m_featureID, &m_igpt, m_nvigiLoadInterface);
                        if (nvigiRes == nvigi::kResultOk)
                            nvigiRes = m_igpt->createInstance(*params, &m_gpt);
                    }
                    else
                    {
                        nvigiRes = nvigi::kResultInvalidParameter;
                    }

                    if (nvigiRes != nvigi::kResultOk)
                    {
                        donut::log::error("Unable to create GPT instance/model and cannot revert to previous model");
                    }
                }

                m_gptReady.store(nvigiRes == nvigi::kResultOk);
                FreeCreationParams(params);
            }
            else
            {
                m_gptReady.store(false);
            }
        };
    m_loadingThread = new std::thread{ loadModel };
}

void NVIGIContext::ReloadASRModel(int32_t index)
{
    if (m_loadingThread)
    {
        m_loadingThread->join();
        delete m_loadingThread;
        m_loadingThread = nullptr;
    }
    m_asrReady.store(false);

    m_asrIndex = index;
    PluginModelInfo* newAsrInfo = GetASRPluginModel(m_asrIndex);

    if (m_iasr)
    {
        m_iasr->destroyInstance(m_asr);
        m_asr = {};
    }

    auto loadModel = [this, newAsrInfo]()->void
        {
            cerr_redirect ggmlLog;

            nvigi::ASRWhisperCreationParameters* params2 = GetASRCreationParams(false);
            if (params2 && newAsrInfo)
            {
                nvigi::Result nvigiRes = nvigiGetInterfaceDynamic(newAsrInfo->m_featureID, &m_iasr, m_nvigiLoadInterface);
                if (nvigiRes == nvigi::kResultOk)
                    nvigiRes = m_iasr->createInstance(*params2, &m_asr);
                if (nvigiRes != nvigi::kResultOk)
                {
                    donut::log::error("Unable to create ASR instance/model.  See log for details.  Most common issue is incorrect path to models");
                }

                m_asrReady.store(nvigiRes == nvigi::kResultOk);
                FreeCreationParams(params2);
            }
            else
            {
                m_asrReady.store(false);
            }
        };
    m_loadingThread = new std::thread{ loadModel };
}

void NVIGIContext::LaunchASR()
{
    if (!m_asrReady)
    {
        donut::log::warning("Skipping Speech to Text as it is still loading or failed to load");
        return;
    }

    auto asrCallback = [](const nvigi::InferenceExecutionContext* ctx, nvigi::InferenceExecutionState state, void* data)->nvigi::InferenceExecutionState
        {
            if (!data)
                return nvigi::kInferenceExecutionStateInvalid;

            NVIGIContext& nvigi = *((NVIGIContext*)data);

            if (ctx)
            {
                auto slots = ctx->outputs;
                const nvigi::InferenceDataText* text{};
                slots->findAndValidateSlot(nvigi::kASRWhisperDataSlotTranscribedText, &text);
                auto str = std::string((const char*)text->getUTF8Text());

                if (str.find("<JSON>") == std::string::npos)
                {
                    std::scoped_lock lock(nvigi.m_mtx);
                    nvigi.m_a2t.append(str);
                    nvigi.m_gptInput.append(str);
                }
            }
            nvigi.m_gptInputReady = state == nvigi::kInferenceExecutionStateDone;
            return state;
        };

    auto l = [this, asrCallback]()->void
        {
            m_inferThreadRunning = true;
            nvigi::CpuData audioData;
            nvigi::InferenceDataAudio wavData(audioData);
            AudioRecordingHelper::StopRecordingAudio(m_audioInfo, &wavData);

            std::vector<nvigi::InferenceDataSlot> inSlots = { {nvigi::kASRWhisperDataSlotAudio, &wavData} };

            nvigi::InferenceExecutionContext ctx{};
            ctx.instance = m_asr;
            ctx.callback = asrCallback;
            ctx.callbackUserData = this;
            nvigi::InferenceDataSlotArray inputs = { inSlots.size(), inSlots.data() };
            ctx.inputs = &inputs;
            m_asrRunning.store(true);
            m_asr->evaluate(&ctx);
            m_asrRunning.store(false);

            m_inferThreadRunning = false;
        };
    m_inferThread = new std::thread{ l };
}

void NVIGIContext::LaunchGPT(std::string prompt)
{
    auto gptCallback = [](const nvigi::InferenceExecutionContext* ctx, nvigi::InferenceExecutionState state, void* data)->nvigi::InferenceExecutionState
        {
            if (!data)
                return nvigi::kInferenceExecutionStateInvalid;

            NVIGIContext& nvigi = *((NVIGIContext*)data);

            if (ctx)
            {
                auto slots = ctx->outputs;
                const nvigi::InferenceDataText* text{};
                slots->findAndValidateSlot(nvigi::kGPTDataSlotResponse, &text);
                auto str = std::string((const char*)text->getUTF8Text());
                if (nvigi.m_conversationInitialized)
                {
                    if (str.find("<JSON>") == std::string::npos)
                    {
                        std::scoped_lock lock(nvigi.m_mtx);
                        if (nvigi.m_conversationInitialized)
                        {
                            messages.back().text.append(str);
                        }
                    }
                    else
                    {
                        str = std::regex_replace(str, std::regex("<JSON>"), "");
                        std::scoped_lock lock(nvigi.m_mtx);
                    }
                }
            }
            if (state == nvigi::kInferenceExecutionStateDone)
            {
                std::scoped_lock lock(nvigi.m_mtx);
            }

            // Signal the calling thread, since we may be an async evalutation
            {
                std::unique_lock lck(nvigi.m_gptCallbackMutex);
                nvigi.m_gptCallbackState = state;
                nvigi.m_gptCallbackCV.notify_one();
            }

            return state;
        };

    auto l = [this, prompt, gptCallback]()->void
        {
            m_inferThreadRunning = true;

            nvigi::GPTRuntimeParameters runtime{};
            runtime.seed = -1;
            runtime.tokensToPredict = 200;
            runtime.interactive = true;
            runtime.reversePrompt = "User: ";

            auto eval = [this, &gptCallback, &runtime](std::string prompt, bool initConversation)->void
                {
                    nvigi::CpuData text(prompt.length() + 1, (void*)prompt.c_str());
                    nvigi::InferenceDataText data(text);

                    nvigi::InferenceExecutionContext ctx{};
                    ctx.instance = m_gpt;
                    std::vector<nvigi::InferenceDataSlot> inSlots = { { initConversation ? nvigi::kGPTDataSlotSystem : nvigi::kGPTDataSlotUser, &data} };
                    ctx.callback = gptCallback;
                    ctx.callbackUserData = this;
                    nvigi::InferenceDataSlotArray inputs = { inSlots.size(), inSlots.data() };
                    ctx.inputs = &inputs;
                    ctx.runtimeParameters = runtime;

                    // By default, before any callback, we always have "data pending"
                    m_gptCallbackState = nvigi::kInferenceExecutionStateDataPending;

                    m_gptRunning.store(true);
                    nvigi::Result res = m_gpt->evaluate(&ctx);

                    // Wait for the GPT to stop returning eDataPending in the callback
                    if (res == nvigi::kResultOk)
                    {
                        std::unique_lock lck(m_gptCallbackMutex);
                        m_gptCallbackCV.wait(lck, [&this]() { return m_gptCallbackState != nvigi::kInferenceExecutionStateDataPending; });
                    }
                };

            if (!m_conversationInitialized)
            {
                std::string initialPrompt = "You are a helpful AI assistant answering user questions.\n";
                eval(initialPrompt, true);
                m_conversationInitialized = true;
            }

            eval(prompt, false);

            m_gptRunning.store(false);

            m_inferThreadRunning = false;
        };
    m_inferThread = new std::thread{ l };
}

void NVIGIContext::FlushInferenceThread()
{
    if (m_inferThread)
    {
        m_inferThread->join();
        delete m_inferThread;
        m_inferThread = nullptr;
    }
}

bool NVIGIContext::ModelsComboBox(const std::string& label, const std::vector<std::string>& values, const std::vector<ModelStatus>* available, int32_t& value, bool disabled)
{
    int index = value;

    bool changed = false;
    if (!disabled)
    {
        if (ImGui::BeginCombo(label.c_str(), (index < 0) ? "No Selection" : values[index].c_str()))
        {
            for (auto i = 0; i < values.size(); ++i)
            {
                bool is_selected = i == index;
                if (!available || (*available)[i] == ModelStatus::AVAILABLE_LOCALLY
                    || (*available)[i] == ModelStatus::AVAILABLE_CLOUD)
                {
                    if (ImGui::Selectable(values[i].c_str(), is_selected))
                    {
                        changed = index != i;
                        index = i;
                    }
                }
                else if ((*available)[i] == ModelStatus::AVAILABLE_MANUAL_DOWNLOAD)
                {
                    ImGui::TextDisabled((values[i] + ": MANUAL DOWNLOAD").c_str());
                }
                if (is_selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    else
    {
        if (ImGui::BeginCombo(label.c_str(), (index != -1) ? values[index].c_str() : values[0].c_str()))
            ImGui::EndCombo();
    }
    value = index;

    return changed;
}

void NVIGIContext::BuildASRUI()
{
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, TITLE_COL);
    ImGui::Text("Automatic Speech Recognition");
    ImGui::PopStyleColor();

    std::vector<std::string> asrListCaptions;
    std::vector<ModelStatus> asrListAvailable;
    for (auto it : m_asrPluginModels)
    {
        asrListCaptions.push_back(it->m_caption);
        asrListAvailable.push_back(it->m_modelStatus);
    }

    if (ModelsComboBox("Inference##ASR", asrListCaptions, &asrListAvailable, m_asrIndex, m_asrRunning))
    {
        ReloadASRModel(m_asrIndex);
    }

    if (m_asrReady)
    {
        if (m_recording)
        {
            if (ImGui::Button("Stop"))
            {
                m_recording = false;
                m_gptInputReady = false;

                FlushInferenceThread();

                LaunchASR();
            }
        } // Do not show Record button when ASR or GPT is running
        else if (!m_gptRunning && !m_asrRunning && ImGui::Button("Record"))
        {
            FlushInferenceThread();
            m_audioInfo = AudioRecordingHelper::StartRecordingAudio();
            m_recording = true;

            m_a2t = "";
            m_gptInput = "";
        }

        // Create a child window with a scrollbar for messages
        auto child_size = ImVec2(ImGui::GetWindowContentRegionWidth(), 60);
        if (ImGui::BeginChild("Recognized Text", ImVec2(0, 60), true))
        {
            std::scoped_lock lock(m_mtx);
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + child_size.x - 15);  // Wrapping text before the edge, added a small offset for aesthetics
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "%s", m_a2t.c_str());

            ImGui::PopTextWrapPos();  // Reset wrapping position

            // Scroll to the bottom when a new message is added
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    else
    {
        if (m_asrIndex >= 0)
            ImGui::Text("ASR Loading...");
        else
            ImGui::Text("No model selected ...");
    }
}

void NVIGIContext::BuildGPTUI()
{
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, TITLE_COL);
    ImGui::Text("GPT");
    ImGui::PopStyleColor();

    std::vector<std::string> gptListCaptions;
    std::vector<ModelStatus> gptListAvailable;
    for (auto it : m_gptPluginModels)
    {
        gptListCaptions.push_back(it->m_caption);
        gptListAvailable.push_back(it->m_modelStatus);
    }
    
    int newIndex = m_gptIndex;
    if (ModelsComboBox("Inference##GPT", gptListCaptions, &gptListAvailable, newIndex, m_gptRunning))
        ReloadGPTModel(newIndex);

    if (m_gptReady)
    {
        if (m_gptInputReady)
        {
            m_gptInputReady = false;

            messages.push_back({ Message::Type::Question, m_gptInput });
            messages.push_back({ Message::Type::Answer, "" });

            FlushInferenceThread();

            LaunchGPT(m_gptInput);
        }

        if (ImGui::Button("Reset Conversation"))
        {
            std::scoped_lock lock(m_mtx);
            m_conversationInitialized = false;
            messages.clear();
            messages.push_back({ Message::Type::Answer, "Conversation Reset: I'm here to chat - type a query or record audio to interact!" });
        }

        {
            std::scoped_lock lock(m_mtx);

            static char inputBuffer[256] = {};
            auto child_size = ImVec2(ImGui::GetWindowContentRegionWidth(), 600);
            if (ImGui::BeginChild("Chat UI", child_size, false))
            {
                // Create a child window with a scrollbar for messages
                if (ImGui::BeginChild("Messages", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true))
                {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + child_size.x - 15);  // Wrapping text before the edge, added a small offset for aesthetics

                    for (const auto& message : messages)
                    {
                        if (message.type == Message::Type::Question)
                            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Q: %s", message.text.c_str());
                        else
                            ImGui::TextColored(ImVec4(0, 1, 0, 1), "A: %s", message.text.c_str());
                    }

                    ImGui::PopTextWrapPos();  // Reset wrapping position

                    // Scroll to the bottom when a new message is added
                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                        ImGui::SetScrollHereY(1.0f);
                }
                ImGui::EndChild();

                // Input text box and button to send messages
                if (ImGui::InputText("##Input", inputBuffer, sizeof(inputBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) 
                {
                    m_gptInput = inputBuffer;
                    m_gptInputReady = true;
                    inputBuffer[0] = '\0';  // Clear the buffer
                }

                // Do not show Send when ASR or GPT is running, or when we're recording audio
                if (!m_gptRunning && !m_asrRunning && !m_recording)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Send"))
                    {
                        m_gptInput = inputBuffer;
                        m_gptInputReady = true;
                        inputBuffer[0] = '\0';  // Clear the buffer
                    }
                }

                ImGui::SameLine();
            }
        }

        ImGui::EndChild();
    }
    else
    {
        if (m_gptIndex >= 0)
            ImGui::Text("Loading models please wait ...");
        else
            ImGui::Text("No model selected ...");
    }
}

void NVIGIContext::BuildUI()
{
    BuildASRUI();
    BuildGPTUI();
}

void NVIGIContext::GetVRAMStats(size_t& current, size_t& budget)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfo{};
    if (m_targetAdapter)
    {
        m_targetAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &videoMemoryInfo);
    }
    current = videoMemoryInfo.CurrentUsage;
    budget = videoMemoryInfo.Budget;
}
