// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//
#pragma once

// Donut
#include <donut/engine/Scene.h>
#include <donut/render/TemporalAntiAliasingPass.h>
#include <donut/render/SsaoPass.h>
#include <donut/render/SkyPass.h>
#include <donut/render/ToneMappingPasses.h>
#include <nvrhi/nvrhi.h>

/// <summary>
/// This enum describes the available anti-aliasing modes. These can be toggled from the UI
/// </summary>
enum class AntiAliasingMode {
    NONE,
    TEMPORAL,
};

struct UIData
{
    // General
    nvrhi::GraphicsAPI                  GraphicsAPI = nvrhi::GraphicsAPI::D3D12;
    bool                                EnableAnimations = true;
    float                               CpuLoad = 0;
    int                                 GpuLoad = 0;
    donut::math::int2                   Resolution = { 0,0 };
    
    // SSAO
    bool                                EnableSsao = true;
    donut::render::SsaoParameters       SsaoParams;

    // Tonemapping
    bool                                 EnableToneMapping = true;
    donut::render::ToneMappingParameters ToneMappingParams;

    // Sky
    bool                                EnableProceduralSky = true;
    donut::render::SkyParameters        SkyParams;
    float                               AmbientIntensity = .2f;

    // Antialising (+TAA)
    AntiAliasingMode                                   AAMode = AntiAliasingMode::TEMPORAL;
    donut::render::TemporalAntiAliasingJitter          TemporalAntiAliasingJitter = donut::render::TemporalAntiAliasingJitter::MSAA;
    donut::render::TemporalAntiAliasingParameters      TemporalAntiAliasingParams;

    // Bloom
    bool                                EnableBloom = true;
    float                               BloomSigma = 32.f;
    float                               BloomAlpha = 0.05f;

    // Shadows
    bool                                EnableShadows = true;
    float                               CsmExponent = 4.f;

};

