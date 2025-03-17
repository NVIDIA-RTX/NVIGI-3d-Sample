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
#include <nvigi_stl_helpers.h>

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

const static nvigi::PluginID nullPluginId = { 0, 0 };

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

static std::wstring GetNVIGICoreDllPath() {

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
    return basePath;
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
                donut::log::warning("Plugin %s could not be loaded on adapters from this GPU vendor (found %0x, requires %0x)", name.c_str(),
                    adapterInfo->vendor, plugin->requiredAdapterVendor);
                return false;
            }

            if (plugin->requiredAdapterVendor == nvigi::VendorId::eNVDA && plugin->requiredAdapterArchitecture > adapterInfo->architecture)
            {
                donut::log::warning("Plugin %s could not be loaded on this GPU architecture (found %d, requires %d)", name.c_str(),
                    adapterInfo->architecture, plugin->requiredAdapterArchitecture);
                return false;
            }

            if (plugin->requiredAdapterVendor == nvigi::VendorId::eNVDA && plugin->requiredAdapterDriverVersion > adapterInfo->driverVersion)
            {
                donut::log::warning("Plugin %s could not be loaded on this driver (found %d.%d, requires %d.%d)", name.c_str(),
                    adapterInfo->driverVersion.major, adapterInfo->driverVersion.minor,
                    plugin->requiredAdapterDriverVersion.major, plugin->requiredAdapterDriverVersion.minor);
                return false;
            }

            return true;
        }
    }

    // Not found
    donut::log::warning("Plugin %s could not be loaded", name.c_str());

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
            info->m_vram = models->modelMemoryBudgetMB[i];
            info->m_modelStatus = (models->modelFlags[i] & nvigi::kModelFlagRequiresDownload) 
                ? ModelStatus::AVAILABLE_MANUAL_DOWNLOAD : ModelStatus::AVAILABLE_LOCALLY;
            m_gpt.m_pluginModelsMap[info->m_guid].push_back(info);
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
            info->m_vram = 0;
            info->m_modelStatus = ModelStatus::AVAILABLE_CLOUD;
            info->m_url = cloudCaps->url;
            m_gpt.m_pluginModelsMap[info->m_guid].push_back(info);

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
            info->m_vram = models.modelMemoryBudgetMB[i];
            info->m_modelStatus = (models.modelFlags[i] & nvigi::kModelFlagRequiresDownload)
                ? ModelStatus::AVAILABLE_MANUAL_DOWNLOAD : ModelStatus::AVAILABLE_LOCALLY;
            m_asr.m_pluginModelsMap[info->m_guid].push_back(info);
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
        donut::log::warning("No NVIDIA adapters found.  GPU plugins will not be available\n");
        if (m_pluginInfo->numDetectedAdapters)
            m_adapter = 0;
    }

    m_gpt.m_vramBudget = 8500;

    AddGPTPlugin(nvigi::plugin::gpt::ggml::cuda::kId, "ggml.cuda", m_shippedModelsPath);
    AddGPTCloudPlugin();
    AddGPTPlugin(nvigi::plugin::gpt::onnxgenai::dml::kId, "onnxgenai", m_shippedModelsPath);

    m_gpt.m_choices = {
        nvigi::plugin::gpt::ggml::cuda::kId, // NVDA
        nvigi::plugin::gpt::onnxgenai::dml::kId, // GPU
        nvigi::plugin::gpt::cloud::rest::kId, // Cloud
        nullPluginId // CPU
    };

    {
        // Select initial plugin m_gpt.m_info...  Or we set it to null?
        m_gpt.m_info = nullptr;
        for (auto& m : m_gpt.m_pluginModelsMap)
        {
            for (auto& info : m.second)
            {
                ModelStatus status = info->m_modelStatus;
                if (status == ModelStatus::AVAILABLE_LOCALLY)
                {
                    if ((m_gpt.m_choices.m_nvdaFeatureID == info->m_featureID) ||
                        (m_gpt.m_info == nullptr && m_gpt.m_choices.m_gpuFeatureID == info->m_featureID))
                        m_gpt.m_info = info;
                }
            }
            if (m_gpt.m_info)
                break;
        }
    }

    m_asr.m_vramBudget = 3000;

    AddASRPlugin(nvigi::plugin::asr::ggml::cuda::kId, "ggml.cuda", m_shippedModelsPath);
    AddASRPlugin(nvigi::plugin::asr::ggml::cpu::kId, "ggml.cpu", m_shippedModelsPath);

    m_asr.m_choices = {
        nvigi::plugin::asr::ggml::cuda::kId, // NVDA
        nullPluginId, // GPU
        nullPluginId, // Cloud
        nvigi::plugin::asr::ggml::cpu::kId // CPU
    };

    {
        // Select initial plugin m_asr.m_info...  Or we set it to null?
        m_asr.m_info = nullptr;
        for (auto& m : m_asr.m_pluginModelsMap)
        {
            for (auto& info : m.second)
            {
                ModelStatus status = info->m_modelStatus;
                if (status == ModelStatus::AVAILABLE_LOCALLY)
                {
                    if ((m_asr.m_choices.m_nvdaFeatureID == info->m_featureID) ||
                        (m_asr.m_info == nullptr && m_asr.m_choices.m_gpuFeatureID == info->m_featureID))
                        m_asr.m_info = info;
                }
            }
            if (m_asr.m_info)
                break;
        }
        if (!m_asr.m_info)
        {
            for (auto& m : m_asr.m_pluginModelsMap)
            {
                for (auto& info : m.second)
                {
                    ModelStatus status = info->m_modelStatus;
                    if (status == ModelStatus::AVAILABLE_LOCALLY)
                    {
                        if (m_asr.m_choices.m_cpuFeatureID == info->m_featureID)
                            m_asr.m_info = info;
                    }
                }
                if (m_asr.m_info)
                    break;
            }
        }
    }

    m_gpt.m_callbackState.store(nvigi::kInferenceExecutionStateInvalid);

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
    }
    else
    {
        donut::log::info("Not using a shared CUDA context - CiG disabled");
    }

    {
        m_d3d12Params = new nvigi::D3D12Parameters;
        m_d3d12Params->device = m_Device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);

        if (m_D3D12Queue != nullptr)
            m_d3d12Params->queue = m_D3D12Queue;
    }

    size_t currentVRAM;
    GetVRAMStats(currentVRAM, m_maxVRAM);
    m_maxVRAM /= (1024*1024);

    //! Load A2T script    
    auto loadASR = [this]()->HRESULT
        {
            PluginModelInfo* gptInfo = m_gpt.m_info;

            if (gptInfo)
            {
                nvigi::GPTCreationParameters* params1 = GetGPTCreationParams(false);
                nvigi::Result nvigiRes = nvigiGetInterfaceDynamic(gptInfo->m_featureID, &m_igpt, m_nvigiLoadInterface);
                if (nvigiRes == nvigi::kResultOk)
                    nvigiRes = m_igpt->createInstance(*params1, &m_gpt.m_inst);
                if (nvigiRes != nvigi::kResultOk)
                {
                    donut::log::error("Unable to create GPT instance/model.  See log for details.  Most common issue is incorrect path to models");
                }

                m_gpt.m_ready.store(nvigiRes == nvigi::kResultOk);
                FreeCreationParams(params1);
            }
            else
            {
                m_gpt.m_ready.store(false);
            }

            PluginModelInfo* asrInfo = m_asr.m_info;

            if (asrInfo)
            {
                nvigi::ASRWhisperCreationParameters* params2 = GetASRCreationParams(false);
                nvigi::Result nvigiRes = nvigiGetInterfaceDynamic(asrInfo->m_featureID, &m_iasr, m_nvigiLoadInterface);
                if (nvigiRes == nvigi::kResultOk)
                    nvigiRes = m_iasr->createInstance(*params2, &m_asr.m_inst);
                if (nvigiRes != nvigi::kResultOk)
                {
                    donut::log::error("Unable to create ASR instance/model.  See log for details.  Most common issue is incorrect path to models");
                }

                m_asr.m_ready.store(nvigiRes == nvigi::kResultOk);
                FreeCreationParams(params2);
            }
            else
            {
                m_asr.m_ready.store(false);
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

nvigi::D3D12Parameters* NVIGIContext::ChainD3DInfo(PluginModelInfo* info)
{
    if (info == nullptr)
        return nullptr;

    nvigi::D3D12Parameters* d3d12Params = nullptr;
    if (m_useCiG)
    {
        d3d12Params = new nvigi::D3D12Parameters;
        d3d12Params->device = m_d3d12Params->device;
        d3d12Params->queue = m_d3d12Params->queue;
    }

    return d3d12Params;
}

nvigi::GPTCreationParameters* NVIGIContext::GetGPTCreationParams(bool genericInit, const std::string* modelRoot)
{
    PluginModelInfo* info = nullptr;
    
    if (!genericInit)
    {
        info = m_gpt.m_info;
        if (!info)
            return nullptr;
    }

    nvigi::CommonCreationParameters* common1 = new nvigi::CommonCreationParameters;

    common1->numThreads = 1;
    common1->vramBudgetMB = m_gpt.m_vramBudget;
    // Priority order of model roots:
    // if we've been passed a model root, use it
    // else if there's a model with info, use its root
    // all else fails, use the shipped models
    common1->utf8PathToModels = modelRoot ? modelRoot->c_str() :
        (info ? info->m_modelRoot.c_str() : m_shippedModelsPath.c_str());
    common1->modelGUID = info ? info->m_guid.c_str() : nullptr;

    nvigi::GPTCreationParameters* params1 = new nvigi::GPTCreationParameters;

    if (nvigi::D3D12Parameters* d3d12Params = ChainD3DInfo(m_gpt.m_info))
    {
        if (NVIGI_FAILED(res, params1->chain(*d3d12Params)))
            donut::log::error("Internal error chaining structs: %s: %s", __FILE__, __LINE__);
    }

    if (NVIGI_FAILED(res, params1->chain(*common1)))
        donut::log::error("Internal error chaining structs: %s: %s", __FILE__, __LINE__);
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
        if (NVIGI_FAILED(res, params1->chain(onnxgenaiParams)))
            donut::log::error("Internal error chaining structs: %s: %s", __FILE__, __LINE__);
    }
    else if (info->m_featureID == nvigi::plugin::gpt::cloud::rest::kId)
    {
        const char* key =  nullptr;
        std::string apiKeyName = "";
        if (!GetCloudModelAPIKey(*info, key, apiKeyName))
        {
            std::string text = "CLOUD API key not found at " + apiKeyName + " cloud model will not be available";
            donut::log::warning(text.c_str());
            FreeCreationParams(params1);
            return nullptr;
        }

        //! Cloud parameters
        nvigi::RESTParameters* nvcfParams = new nvigi::RESTParameters;
        nvcfParams->url = info->m_url.c_str();
        nvcfParams->authenticationToken = key;
        nvcfParams->verboseMode = true;
        if (NVIGI_FAILED(res, params1->chain(*nvcfParams)))
            donut::log::error("Internal error chaining structs: %s: %s", __FILE__, __LINE__);
    }

    return params1;
}

nvigi::ASRWhisperCreationParameters* NVIGIContext::GetASRCreationParams(bool genericInit, const std::string* modelRoot)
{
    PluginModelInfo* info = nullptr;

    if (!genericInit)
    {
        info = m_asr.m_info;
        if (!info)
            return nullptr;
    }

    nvigi::CommonCreationParameters* common1 = new nvigi::CommonCreationParameters;
    common1->numThreads = 4;
    common1->vramBudgetMB = m_asr.m_vramBudget;
    // Priority order of model roots:
    // if we've been passed a model root, use it
    // else if there's a model with info, use its root
    // all else fails, use the shipped models
    common1->utf8PathToModels = modelRoot ? modelRoot->c_str() :
        (info ? info->m_modelRoot.c_str() : m_shippedModelsPath.c_str());

    nvigi::ASRWhisperCreationParameters* params1 = new nvigi::ASRWhisperCreationParameters;

    if (nvigi::D3D12Parameters* d3d12Params = ChainD3DInfo(m_asr.m_info))
    {
        if (NVIGI_FAILED(res, params1->chain(*d3d12Params)))
            donut::log::error("Internal error chaining structs: %s: %s", __FILE__, __LINE__);
    }

    if (NVIGI_FAILED(res, params1->chain(*common1)))
        donut::log::error("Internal error chaining structs: %s: %s", __FILE__, __LINE__);

    if (genericInit)
        return params1;

    common1->modelGUID = info->m_guid.c_str();

    return params1;
}

void NVIGIContext::ReloadGPTModel(PluginModelInfo* newGptInfo)
{
    if (m_loadingThread)
    {
        m_loadingThread->join();
        delete m_loadingThread;
        m_loadingThread = nullptr;
    }

    m_conversationInitialized = false;

    PluginModelInfo* prevGptInfo = m_gpt.m_info;

    m_gpt.m_info = newGptInfo;

    nvigi::GPTCreationParameters* params1 = GetGPTCreationParams(false);
    // This will be null if there is an error OR if the new model is being downloaded
    if (!params1)
    {
        m_gpt.m_info = prevGptInfo;
        return;
    }

    m_gpt.m_ready.store(false);

    if (m_igpt)
    {
        m_igpt->destroyInstance(m_gpt.m_inst);
        m_gpt.m_inst = {};
    }

    auto loadModel = [this, prevGptInfo, newGptInfo, params1]()->void
        {
            nvigi::GPTCreationParameters* params = params1;
            if (params)
            {
                cerr_redirect ggmlLog;
                nvigi::Result nvigiRes = nvigiGetInterfaceDynamic(newGptInfo->m_featureID, &m_igpt, m_nvigiLoadInterface);
                if (nvigiRes == nvigi::kResultOk)
                    nvigiRes = m_igpt->createInstance(*params, &m_gpt.m_inst);
                if (nvigiRes != nvigi::kResultOk)
                {
                    FreeCreationParams(params);
                    donut::log::error("Unable to create GPT instance/model.  See log for details.  Most common issue is incorrect path to models.  Reverting to previous GPT instance/model");
                    m_gpt.m_info = prevGptInfo;
                    params = GetGPTCreationParams(false);
                    if (params && prevGptInfo)
                    {
                        nvigiRes = nvigiGetInterfaceDynamic(prevGptInfo->m_featureID, &m_igpt, m_nvigiLoadInterface);
                        if (nvigiRes == nvigi::kResultOk)
                            nvigiRes = m_igpt->createInstance(*params, &m_gpt.m_inst);
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

                m_gpt.m_ready.store(nvigiRes == nvigi::kResultOk);
                FreeCreationParams(params);
            }
            else
            {
                m_gpt.m_ready.store(false);
            }
        };
    m_loadingThread = new std::thread{ loadModel };
}

void NVIGIContext::ReloadASRModel(PluginModelInfo* newAsrInfo)
{
    if (m_loadingThread)
    {
        m_loadingThread->join();
        delete m_loadingThread;
        m_loadingThread = nullptr;
    }
    m_asr.m_ready.store(false);

    m_asr.m_info = newAsrInfo;

    if (m_iasr)
    {
        m_iasr->destroyInstance(m_asr.m_inst);
        m_asr.m_inst = {};
    }

    auto loadModel = [this, newAsrInfo]()->void
        {
            cerr_redirect ggmlLog;

            nvigi::ASRWhisperCreationParameters* params2 = GetASRCreationParams(false);
            if (params2 && newAsrInfo)
            {
                nvigi::Result nvigiRes = nvigiGetInterfaceDynamic(newAsrInfo->m_featureID, &m_iasr, m_nvigiLoadInterface);
                if (nvigiRes == nvigi::kResultOk)
                    nvigiRes = m_iasr->createInstance(*params2, &m_asr.m_inst);
                if (nvigiRes != nvigi::kResultOk)
                {
                    donut::log::error("Unable to create ASR instance/model.  See log for details.  Most common issue is incorrect path to models");
                }

                m_asr.m_ready.store(nvigiRes == nvigi::kResultOk);
                FreeCreationParams(params2);
            }
            else
            {
                m_asr.m_ready.store(false);
            }
        };
    m_loadingThread = new std::thread{ loadModel };
}

void NVIGIContext::LaunchASR()
{
    if (!m_asr.m_ready)
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

            std::vector<nvigi::InferenceDataSlot> inSlots = { {nvigi::kASRWhisperDataSlotAudio, wavData} };

            nvigi::InferenceExecutionContext ctx{};
            ctx.instance = m_asr.m_inst;
            ctx.callback = asrCallback;
            ctx.callbackUserData = this;
            nvigi::InferenceDataSlotArray inputs = { inSlots.size(), inSlots.data() };
            ctx.inputs = &inputs;
            m_asr.m_running.store(true);
            m_asr.m_inst->evaluate(&ctx);
            m_asr.m_running.store(false);

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
                std::unique_lock lck(nvigi.m_gpt.m_callbackMutex);
                nvigi.m_gpt.m_callbackState = state;
                nvigi.m_gpt.m_callbackCV.notify_one();
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
                    nvigi::InferenceDataTextSTLHelper data(prompt);

                    nvigi::InferenceExecutionContext ctx{};
                    ctx.instance = m_gpt.m_inst;
                    std::vector<nvigi::InferenceDataSlot> inSlots = { { initConversation ? nvigi::kGPTDataSlotSystem : nvigi::kGPTDataSlotUser, data} };
                    ctx.callback = gptCallback;
                    ctx.callbackUserData = this;
                    nvigi::InferenceDataSlotArray inputs = { inSlots.size(), inSlots.data() };
                    ctx.inputs = &inputs;
                    ctx.runtimeParameters = runtime;

                    // By default, before any callback, we always have "data pending"
                    m_gpt.m_callbackState = nvigi::kInferenceExecutionStateDataPending;

                    m_gpt.m_running.store(true);
                    nvigi::Result res = m_gpt.m_inst->evaluate(&ctx);

                    // Wait for the GPT to stop returning eDataPending in the callback
                    if (res == nvigi::kResultOk)
                    {
                        std::unique_lock lck(m_gpt.m_callbackMutex);
                        m_gpt.m_callbackCV.wait(lck, [&, this]() { return m_gpt.m_callbackState != nvigi::kInferenceExecutionStateDataPending; });
                    }
                };

            if (!m_conversationInitialized)
            {
                std::string initialPrompt = "You are a helpful AI assistant answering user questions.\n";
                eval(initialPrompt, true);
                m_conversationInitialized = true;
            }

            eval(prompt, false);

            m_gpt.m_running.store(false);

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

bool NVIGIContext::ModelsComboBox(const std::string& label, bool automatic, StageInfo& stage, PluginModelInfo*& value)
{
    const std::map<std::string, std::vector<NVIGIContext::PluginModelInfo*>>& models = stage.m_pluginModelsMap;
    NVIGIContext::PluginModelInfo* info = value;
    bool changed = false;
    if (automatic)
    {
        int newVram = (int)stage.m_vramBudget;


        const std::string uniqueLabel = "VRAM MB ##" + label;
        if (ImGui::InputInt(uniqueLabel.c_str(), &newVram, 100, 500, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (newVram < 0)
                newVram = 0;
            if (m_maxVRAM && newVram > m_maxVRAM)
                newVram = m_maxVRAM;
            stage.m_vramBudget = newVram;
        }

        PluginModelInfo* newInfo = nullptr;
        if (ImGui::BeginCombo(label.c_str(), (value == nullptr) ? "No Selection" : value->m_modelName.c_str()))
        {
            for (auto m : stage.m_pluginModelsMap)
            {
                PluginModelInfo* newInfo = nullptr;
                if (SelectAutoPlugin(stage, m.second, newInfo))
                {
                    bool is_selected_guid = info && newInfo && newInfo->m_guid == info->m_guid;
                    if (ImGui::Selectable(newInfo->m_modelName.c_str(), is_selected_guid) || is_selected_guid)
                    {
                        info = newInfo;
                    }
                }
            }
            ImGui::EndCombo();
        }
        else if (info)
        {
            // This will be hit when we move from manual to auto or adjust the vram values.
            for (auto m : stage.m_pluginModelsMap)
            {
                PluginModelInfo* newInfo = nullptr;
                if (SelectAutoPlugin(stage, m.second, newInfo))
                {
                    if (newInfo && newInfo->m_guid == info->m_guid)
                    {
                        info = newInfo;
                        break;
                    }
                }
            }
        }

        changed = value != info;
    }
    else
    {
        if (ImGui::BeginCombo(label.c_str(), (info == nullptr) ? "No Selection" : info->m_caption.c_str()))
        {
            for (auto m : models)
            {
                for (auto it : m.second)
                {
                    NVIGIContext::PluginModelInfo* newInfo = it;
                    bool is_selected = newInfo == info;
                    const char* key = nullptr;
                    if (newInfo)
                    {
                        std::string apiKeyName = "";
                        bool cloudNotAvailable = (newInfo->m_modelStatus == ModelStatus::AVAILABLE_CLOUD) && !GetCloudModelAPIKey(*newInfo, key, apiKeyName);
                        if (cloudNotAvailable)
                        {
                            ImGui::TextDisabled((newInfo->m_pluginName + ": No " + apiKeyName + " API KEY " + newInfo->m_modelName).c_str());
                        }
                        else if (newInfo->m_modelStatus == ModelStatus::AVAILABLE_LOCALLY || newInfo->m_modelStatus == ModelStatus::AVAILABLE_CLOUD)
                        {
                            if (ImGui::Selectable(newInfo->m_caption.c_str(), is_selected))
                            {
                                changed = !is_selected;
                                info = newInfo;
                            }
                        }
                        else if (newInfo->m_modelStatus == ModelStatus::AVAILABLE_MANUAL_DOWNLOAD)
                        {
                            ImGui::TextDisabled((newInfo->m_caption + ": MANUAL DOWNLOAD").c_str());
                        }
                        if (is_selected) ImGui::SetItemDefaultFocus();
                    }
                }
            }
            ImGui::EndCombo();
        }
    }

    value = info;

    return changed;
}

bool NVIGIContext::SelectAutoPlugin(const StageInfo& stage, const std::vector<PluginModelInfo*>& options, PluginModelInfo*& model)
{
    auto findOption = [options](nvigi::PluginID needId)->PluginModelInfo* {
            if (needId == 0)
                return nullptr;
            for (auto info : options)
                if (info->m_featureID == needId)
                    return info;

            return nullptr;
        };

    // First, can we use the NV-specific plugin?
    if (auto info = findOption(stage.m_choices.m_nvdaFeatureID))
    {
        // Only use if we have enough VRAM budgetted
        if (stage.m_vramBudget >= info->m_vram)
        {
            model = info;
            return true;
        }
    }

    // Can we use a generic GPU plugin?
    if (auto info = findOption(stage.m_choices.m_gpuFeatureID))
    {
        // Only use if we have enough VRAM budgetted
        if (stage.m_vramBudget >= info->m_vram)
        {
            model = info;
            return true;
        }
    }

    // Is cloud an option?
    if (auto info = findOption(stage.m_choices.m_cloudFeatureID))
    {
        const char* key = nullptr;
        std::string apiKeyName = "";
        if (GetCloudModelAPIKey(*info, key, apiKeyName))
        {
            model = info;
            return true;
        }
    }

    // What about CPU?
    if (auto info = findOption(stage.m_choices.m_cpuFeatureID))
    {
        model = info;
        return true;
    }

    // No viable options...
    return false;
}

bool NVIGIContext::BuildModelsSelectUI()
{
    if (!m_gpt.m_running && !m_asr.m_running && !m_recording)
    {
        if (ImGui::CollapsingHeader("Model Settings..."))
        {
            ImGui::Checkbox("Automatic Backend Selection", &m_automaticBackendSelection);
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, TITLE_COL);
            ImGui::Text("Automatic Speech Recognition");
            ImGui::PopStyleColor();

            PluginModelInfo* newInfo = m_asr.m_info;
            if (ModelsComboBox("##ASR", m_automaticBackendSelection, m_asr, newInfo))
                ReloadASRModel(newInfo);

            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, TITLE_COL);
            ImGui::Text("GPT");
            ImGui::PopStyleColor();

            newInfo = m_gpt.m_info;
            if (ModelsComboBox("##GPT", m_automaticBackendSelection, m_gpt, newInfo))
                ReloadGPTModel(newInfo);

            return true;
        }
    }
    return false;
}

void NVIGIContext::BuildModelsStatusUI()
{
    ImGui::Separator();

    if (m_asr.m_ready)
    {
        std::string asr = "ASR: " + m_asr.m_info->m_caption;
        ImGui::Text(asr.c_str());
    }
    else
    {
        if (m_asr.m_info != nullptr)
            ImGui::Text("ASR: Loading model please wait...");
        else
            ImGui::Text("ASR: No model selected ...");
    }

    if (m_gpt.m_ready)
    {
        std::string gpt = "GPT: " + m_gpt.m_info->m_caption;
        ImGui::Text(gpt.c_str());
    }
    else
    {
        if (m_gpt.m_info != nullptr)
            ImGui::Text("GPT: Loading model please wait...");
        else
            ImGui::Text("GPT: No model selected ...");
    }
}

void NVIGIContext::BuildChatUI()
{
    if (m_gpt.m_ready)
    {
        std::scoped_lock lock(m_mtx);

        static char inputBuffer[512] = {};
        auto child_size = ImVec2(ImGui::GetWindowContentRegionWidth(), 600);
        if (ImGui::BeginChild("Chat UI", child_size, false))
        {
            // Create a child window with a scrollbar for messages
            if (ImGui::BeginChild("Messages", ImVec2(0, -2*ImGui::GetFrameHeightWithSpacing()), true))
            {
                ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + child_size.x - 30);  // Wrapping text before the edge, added a small offset for aesthetics

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

            // Do not show Send when ASR or GPT is running, or when we're recording audio
            if (!m_gpt.m_running && !m_asr.m_running)
            {
                ImGui::PushItemWidth(ImGui::GetWindowContentRegionWidth());
                // Input text box and button to send messages
                if (ImGui::InputText("##Input", inputBuffer, sizeof(inputBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    m_gptInput = inputBuffer;
                    m_gptInputReady = true;
                    inputBuffer[0] = '\0';  // Clear the buffer
                }
                ImGui::PopItemWidth();

                if (m_asr.m_ready)
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
                    else if (!m_gpt.m_running && !m_asr.m_running && ImGui::Button("Record"))
                    {
                        FlushInferenceThread();
                        m_audioInfo = AudioRecordingHelper::StartRecordingAudio();
                        m_recording = true;

                        m_a2t = "";
                        m_gptInput = "";
                    }
                }

                if (!m_recording)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Reset Chat"))
                    {
                        m_conversationInitialized = false;
                        messages.clear();
                        messages.push_back({ Message::Type::Answer, "Conversation Reset: I'm here to chat - type a query or record audio to interact!" });
                    }
                }
            }
        }

        ImGui::EndChild();
    }
    else
    {
        if (m_gpt.m_info == nullptr || m_asr.m_info == nullptr)
            ImGui::Text("Loading models please wait ...");
        else
            ImGui::Text("No models selected ...");
    }
}

void NVIGIContext::BuildUI()
{
    if (m_gpt.m_ready)
    {
        if (m_gptInputReady)
        {
            m_gptInputReady = false;

            messages.push_back({ Message::Type::Question, m_gptInput });
            messages.push_back({ Message::Type::Answer, "" });

            FlushInferenceThread();

            LaunchGPT(m_gptInput);
        }
    }

    if (!m_modelSettingsOpen)
        BuildChatUI();
    BuildModelsStatusUI();
    m_modelSettingsOpen = BuildModelsSelectUI();
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
