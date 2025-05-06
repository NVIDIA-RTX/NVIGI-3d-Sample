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
#include <queue>
#include <memory>
#include <thread>
#include <type_traits>
#include <nvigi.h>
#include <nvigi_struct.h>
#include <nvigi_types.h>
#include <nvigi_cuda.h>
#include <nvigi_stl_helpers.h>
#include <nvigi_tts.h>

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
    struct TTSCreationParameters;

    struct D3D12Parameters;
    struct IHWICuda;
    struct InferenceInterface;
    using IGeneralPurposeTransformer = InferenceInterface;
    using IAutoSpeechRecognition = InferenceInterface;
    using ITextToSpeech = InferenceInterface;

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
        AVAILABLE_MANUAL_DOWNLOAD,
        UNAVAILABLE
    };
    struct PluginModelInfo
    {
        std::string m_modelName;
        std::string m_pluginName;
        std::string m_caption; // plugin AND model
        std::string m_guid;
        std::string m_modelRoot;
        std::string m_url;
        size_t m_vram;
        nvigi::PluginID m_featureID;
        ModelStatus m_modelStatus;
    };

    struct PluginBackendChoices
    {
        nvigi::PluginID m_nvdaFeatureID;
        nvigi::PluginID m_gpuFeatureID;
        nvigi::PluginID m_cloudFeatureID;
        nvigi::PluginID m_cpuFeatureID;
    };

    struct StageInfo
    {
        PluginModelInfo* m_info{};
        nvigi::InferenceInstance* m_inst{};
        // Model GUID to info maps (maps model GUIDs to a list of plugins that run it)
        std::map<std::string, std::vector<PluginModelInfo*>> m_pluginModelsMap{};
        PluginBackendChoices m_choices{};
        std::atomic<bool> m_ready = false;
        std::atomic<bool> m_running = false;
        std::mutex m_callbackMutex;
        std::condition_variable m_callbackCV;
        std::atomic<nvigi::InferenceExecutionState> m_callbackState;
        size_t m_vramBudget{};
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
    bool AddTTSPlugin(nvigi::PluginID id, const std::string& name, const std::string& modelRoot);

    void GetVRAMStats(size_t& current, size_t& budget);

    void LaunchASR();
    void LaunchGPT(std::string prompt);
    void AppendTTSText(std::string text, bool done);
    void LaunchTTS(std::string prompt);

    bool ModelsComboBox(const std::string& label, bool automatic,
        StageInfo& stage,
        NVIGIContext::PluginModelInfo*& value);
    bool SelectAutoPlugin(const StageInfo& stage, const std::vector<PluginModelInfo*>& options, PluginModelInfo*& model);
    bool BuildModelsSelectUI();
    void BuildModelsStatusUI();
    void BuildChatUI();
    void BuildUI();

    static void PresentStart(donut::app::DeviceManager& manager) {};

    template <typename T> void FreeCreationParams(T* params);

    virtual nvigi::GPTCreationParameters* GetGPTCreationParams(bool genericInit, const std::string* modelRoot = nullptr);
    virtual nvigi::ASRWhisperCreationParameters* GetASRCreationParams(bool genericInit, const std::string* modelRoot = nullptr);
    virtual nvigi::TTSCreationParameters* GetTTSCreationParams(bool genericInit, const std::string* modelRoot = nullptr);

    void ReloadGPTModel(PluginModelInfo* newInfo);
    void ReloadASRModel(PluginModelInfo* newInfo);
    void ReloadTTSModel(PluginModelInfo* newInfo);
    void FlushInferenceThread();

    StageInfo m_asr;
    StageInfo m_gpt;
    StageInfo m_tts;

    std::string m_nvdaKey = "";
    std::string m_openAIKey = "";

    bool GetCloudModelAPIKey(const PluginModelInfo& info, const char* & key, std::string& apiKeyName)
    {
        if (info.m_url.find("integrate.api.nvidia.com") != std::string::npos)
        {
            if (m_nvdaKey.empty())
            {
                const char* ckey = getenv("NVIDIA_INTEGRATE_KEY");
                if (ckey)
                {
                    m_nvdaKey = ckey;
                }
                else
                {
                    apiKeyName = "NVIDIA_INTEGRATE_KEY";
                    return false;
                }
            }
            key = m_nvdaKey.c_str();
            return true;
        }
        else if (info.m_url.find("openai.com") != std::string::npos)
        {
            if (m_openAIKey.empty())
            {
                const char* ckey = getenv("OPENAI_KEY");
                if (ckey)
                {
                    m_openAIKey = ckey;
                }
                else
                {
                    apiKeyName = "OPENAI_KEY";
                    return false;
                }
            }
            key = m_openAIKey.c_str();
            return true;
        }
        else
        {
            apiKeyName = "UNKNOWN SERVICE";
            donut::log::warning("Unknown cloud model URL (%s); cannot send authentication token", info.m_url.c_str());
        }

        return false;
    }

    struct TTSInferenceContext {
        std::string m_selectedTargetVoice = "02_F-Emma_Maidenberg_15s";
        nvigi::InferenceDataTextSTLHelper dataTextTTS = "";
        nvigi::InferenceDataTextSTLHelper dataTextTargetPathSepctrogram = "";
        std::vector<nvigi::InferenceDataSlot> inSlotsTTS;
        nvigi::InferenceDataSlotArray inputsTTS;
        nvigi::TTSASqFlowRuntimeParameters runtimeTTS{};
        std::queue< std::unique_ptr<std::thread>> playAudioThreads;
        std::mutex mtxPlayAudio;
        std::mutex ttsCallbackMutex;

        nvigi::InferenceExecutionContext m_ttsCtx{};

        size_t posLastSpace = 0; // used to handle TTS input chunks
        size_t posLastPeriod = 0; // used to handle TTS input chunks
        size_t posLastComma = 0; // used to handle TTS input chunks

        ~TTSInferenceContext() {
            while (true) {
                std::unique_ptr<std::thread> thread;
                {
                    if (playAudioThreads.empty())
                        break;
                    thread = std::move(playAudioThreads.front());
                    playAudioThreads.pop();
                }

                if (thread->joinable()) {
                    thread->join();
                }
            }
        }
    };

    nvrhi::IDevice* m_Device = nullptr;
    nvrhi::RefCountPtr<ID3D12CommandQueue> m_D3D12Queue = nullptr;
    std::string m_appUtf8path = "";
    std::string m_shippedModelsPath = "../../nvigi.models";
    std::string m_modelASR;
    std::string m_LogFilename = "";
    std::string m_systemPromptGPT = "You are a helpful AI agent. Your goal is to provide information about queries.\
        Generate only medium size answers and avoid describing what you are doing physically.\
        Avoid using specific words that are not part of the dictionary.\n"; 
	TTSInferenceContext m_ttsInferenceCtx;
    bool m_useCiG = true;

    int m_adapter = -1;
    nvigi::PluginAndSystemInformation* m_pluginInfo;

    nvigi::IGeneralPurposeTransformer* m_igpt{};
    nvigi::IAutoSpeechRecognition* m_iasr{};
    nvigi::ITextToSpeech* m_itts{};
    nvigi::IHWICuda* m_cig{};
    std::string m_ttsInput;

    std::string grpcMetadata{};
    std::string nvcfToken{};

    bool m_recording = false;
    std::atomic<bool> m_gptInputReady = false;
    std::string m_a2t;
    std::string m_gptInput;
    std::mutex m_mtx;
    std::vector<uint8_t> m_wavRecording;
    bool m_conversationInitialized = false;
    std::atomic<bool> m_ttsInputReady = false;

    bool m_modelSettingsOpen = false;
    bool m_automaticBackendSelection = false;

    std::thread* m_inferThread{};
    std::atomic<bool> m_inferThreadRunning = false;
    std::thread* m_loadingThread{};

    std::vector<int16_t> m_ttsOutputAudio;
    AudioRecordingHelper::RecordingInfo* m_audioInfo{};

    nvigi::D3D12Parameters* ChainD3DInfo(PluginModelInfo* info);

#ifdef USE_DX12
    nvigi::D3D12Parameters* m_d3d12Params{};
    nvrhi::RefCountPtr<IDXGIAdapter3> m_targetAdapter;
#endif
    nvrhi::GraphicsAPI m_api = nvrhi::GraphicsAPI::D3D12;
    size_t m_maxVRAM = 0;
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

