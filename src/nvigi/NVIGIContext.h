// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//
#pragma once

#include <nvrhi/nvrhi.h>
#include <donut/app/DeviceManager.h>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <sstream>
#include <nvigi_struct.h>
#include <nvigi_types.h>
#include <nvigi_cuda.h>

#include <dxgi.h>
#include <dxgi1_5.h>

#include "AudioRecordingHelper.h"

struct Parameters
{
    donut::app::DeviceCreationParameters deviceParams;
    std::string sceneName;
    bool checkSig = false;
    bool renderScene = true;
};


// NVIGI forward decls
namespace nvigi {
    struct BaseStructure;
    struct CommonCreationParameters;
    struct GPTCreationParameters;
    struct ASRWhisperCreationParameters;

    struct D3D12Parameters;
    struct IHWICuda;
    struct InferenceInterface;
    using IGeneralPurposeTransformer = InferenceInterface;
    using IAutoSpeechRecognition = InferenceInterface;

    using InferenceExecutionState = uint32_t;
    struct InferenceInstance;
};

struct NVIGIContext
{
    enum ModelStatus {
        AVAILABLE_LOCALLY,
        AVAILABLE_CLOUD,
        AVAILABLE_DOWNLOADER, // Not yet supported
        AVAILABLE_DOWNLOADING, // Not yet supported
        AVAILABLE_MANUAL_DOWNLOAD
    };
    struct PluginModelInfo
    {
        std::string m_modelName;
        std::string m_pluginName;
        std::string m_caption; // plugin AND model
        std::string m_guid;
        std::string m_modelRoot;
        std::string m_url;
        nvigi::PluginID m_featureID;
        ModelStatus m_modelStatus;
    };
    enum DownloaderStatus {
        DOWNLOADER_IDLE,
        DOWNLOADER_ACTIVE,
        DOWNLOADER_SUCCESS,
        DOWNLOADER_FAILURE
    };

    NVIGIContext() {}
    virtual ~NVIGIContext() {}
    static NVIGIContext& Get();
    NVIGIContext(const NVIGIContext&) = delete;
    NVIGIContext(NVIGIContext&&) = delete;
    NVIGIContext& operator=(const NVIGIContext&) = delete;
    NVIGIContext& operator=(NVIGIContext&&) = delete;

    bool Initialize_preDeviceManager(nvrhi::GraphicsAPI api, int argc, const char* const* argv);
    bool Initialize_preDeviceCreate(donut::app::DeviceManager* deviceManager, donut::app::DeviceCreationParameters& params);
    bool Initialize_postDevice();
    void SetDevice_nvrhi(nvrhi::IDevice* device);
    void Shutdown();

    bool CheckPluginCompat(nvigi::PluginID id, const std::string& name);
    bool AddGPTPlugin(nvigi::PluginID id, const std::string& name, const std::string& modelRoot);
    bool AddGPTCloudPlugin();
    bool AddASRPlugin(nvigi::PluginID id, const std::string& name, const std::string& modelRoot);

    void GetVRAMStats(size_t& current, size_t& budget);

    void LaunchASR();
    void LaunchGPT(std::string prompt);

    bool ModelsComboBox(const std::string& label, const std::vector<std::string>& values, const std::vector<ModelStatus>* available, 
        int32_t& value, bool disabled = false);
    void BuildASRUI();
    void BuildGPTUI();
    void BuildUI();

    static void PresentStart(donut::app::DeviceManager& manager) {};

    template <typename T> void FreeCreationParams(T* params);

    virtual nvigi::GPTCreationParameters* GetGPTCreationParams(bool genericInit, const std::string* modelRoot = nullptr);
    virtual nvigi::ASRWhisperCreationParameters* GetASRCreationParams(bool genericInit, const std::string* modelRoot = nullptr);

    void ReloadGPTModel(int32_t index);
    void ReloadASRModel(int32_t index);
    void FlushInferenceThread();

    PluginModelInfo* GetGPTPluginModel(int32_t index) { return (index < 0) ? nullptr : m_gptPluginModels[index]; }
    PluginModelInfo* GetASRPluginModel(int32_t index) { return (index < 0) ? nullptr : m_asrPluginModels[index]; }

    bool AnyGPTModelsAvailable()
    {
        for (auto& info : m_gptPluginModels)
        {
            ModelStatus status = info->m_modelStatus;
            if (status == ModelStatus::AVAILABLE_LOCALLY)
                return true;
        }
        return false;
    }

    bool AnyASRModelsAvailable()
    {
        for (auto& info : m_asrPluginModels)
        {
            ModelStatus status = info->m_modelStatus;
            if (status == ModelStatus::AVAILABLE_LOCALLY)
                return true;
        }
        return false;
    }

    nvrhi::IDevice* m_Device = nullptr;
    nvrhi::RefCountPtr<ID3D12CommandQueue> m_D3D12Queue = nullptr;
    std::string m_appUtf8path = "";
    std::string m_shippedModelsPath = "../../nvigi.models";
    std::string m_modelASR;
    std::string m_LogFilename = "";
    bool m_useCiG = true;

    int m_adapter = -1;
    nvigi::PluginAndSystemInformation* m_pluginInfo;

    nvigi::IGeneralPurposeTransformer* m_igpt{};
    nvigi::InferenceInstance* m_gpt{};
    nvigi::IAutoSpeechRecognition* m_iasr{};
    nvigi::InferenceInstance* m_asr{};
    nvigi::IHWICuda* m_cig{};

    std::string grpcMetadata{};
    std::string nvcfToken{};

    std::vector<PluginModelInfo*> m_asrPluginModels{};
    std::vector<PluginModelInfo*> m_gptPluginModels{};

    std::atomic<bool> m_asrReady = false;
    std::atomic<bool> m_gptReady = false;
    std::atomic<bool> m_asrRunning = false;
    std::atomic<bool> m_gptRunning = false;

    bool m_recording = false;
    std::atomic<bool> m_gptInputReady = false;
    std::string m_a2t;
    std::string m_gptInput;
    std::mutex m_mtx;
    std::vector<uint8_t> m_wavRecording;
    bool m_conversationInitialized = false;

    std::thread* m_inferThread{};
    std::atomic<bool> m_inferThreadRunning = false;
    std::thread* m_loadingThread{};

    int32_t                            m_asrIndex = -1;
    int32_t                            m_gptIndex = -1;
    std::mutex                          m_gptCallbackMutex;
    std::condition_variable             m_gptCallbackCV;
    bool                                m_downloadEnded = true;
    std::atomic<nvigi::InferenceExecutionState> m_gptCallbackState;
    AudioRecordingHelper::RecordingInfo* m_audioInfo{};

#ifdef USE_DX12
    nvigi::D3D12Parameters* m_d3d12Params{};
    nvrhi::RefCountPtr<IDXGIAdapter3> m_targetAdapter;
#endif
    nvrhi::GraphicsAPI m_api = nvrhi::GraphicsAPI::D3D12;
};

struct cerr_redirect {
    cerr_redirect()
    {
        std::freopen("ggml.txt", "w", stderr);
    }

    ~cerr_redirect()
    {
        std::freopen("NUL", "w", stderr);
        std::ifstream t("ggml.txt");
        std::stringstream buffer;
        buffer << t.rdbuf();
    }

private:
    std::streambuf* old;
};

