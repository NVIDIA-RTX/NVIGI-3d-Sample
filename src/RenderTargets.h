// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

#pragma once

#include <donut/core/math/math.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/render/GBuffer.h>
#include <nvrhi/nvrhi.h>
#include <nvrhi/common/misc.h>


/// <summary>
/// This class stores all the color buffers we will use in our render pipeline.
/// </summary>
class RenderTargets : public donut::render::GBufferRenderTargets
{
public:
    nvrhi::TextureHandle HdrColor;
    nvrhi::TextureHandle LdrColor;
    nvrhi::TextureHandle ColorspaceCorrectionColor;
    nvrhi::TextureHandle AAResolvedColor;
    nvrhi::TextureHandle TemporalFeedback1;
    nvrhi::TextureHandle TemporalFeedback2;
    nvrhi::TextureHandle AmbientOcclusion;
    nvrhi::TextureHandle NisColor;
    nvrhi::TextureHandle PreUIColor;

    nvrhi::HeapHandle Heap;

    std::shared_ptr<donut::engine::FramebufferFactory> ForwardFramebuffer;
    std::shared_ptr<donut::engine::FramebufferFactory> HdrFramebuffer;
    std::shared_ptr<donut::engine::FramebufferFactory> LdrFramebuffer;
    std::shared_ptr<donut::engine::FramebufferFactory> AAResolvedFramebuffer;
    std::shared_ptr<donut::engine::FramebufferFactory> PreUIFramebuffer;

    donut::math::int2 m_RenderSize;// size of render targets pre-AA
    donut::math::int2 m_DisplaySize; // size of render targets post-AA

    void Init(
        nvrhi::IDevice* device,
        donut::math::int2 renderSize,
        donut::math::int2 displaySize,
        nvrhi::Format backbufferFormat,
        dm::uint sampleCount = 1,
        bool enableMotionVectors = true,
        bool useReverseProjection = true)
    {
        GBufferRenderTargets::Init(device, (donut::math::uint2) renderSize, sampleCount, enableMotionVectors, useReverseProjection);

        m_RenderSize = renderSize;
        m_DisplaySize = displaySize;

        nvrhi::TextureDesc desc;
        desc.width = renderSize.x;
        desc.height = renderSize.y;
        desc.isRenderTarget = true;
        desc.useClearValue = true;
        desc.clearValue = nvrhi::Color(1.f);
        desc.sampleCount = sampleCount;
        desc.dimension = sampleCount > 1 ? nvrhi::TextureDimension::Texture2DMS : nvrhi::TextureDimension::Texture2D;
        desc.keepInitialState = true;
        desc.isVirtual = device->queryFeatureSupport(nvrhi::Feature::VirtualResources);

        desc.clearValue = nvrhi::Color(0.f);
        desc.isTypeless = false;
        desc.isUAV = sampleCount == 1;
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.initialState = nvrhi::ResourceStates::RenderTarget;
        desc.debugName = "HdrColor";
        HdrColor = device->createTexture(desc);

        // The render targets below this point are non-MSAA
        desc.sampleCount = 1;
        desc.dimension = nvrhi::TextureDimension::Texture2D;
        desc.format = nvrhi::Format::R8_UNORM;
        desc.isUAV = true;
        desc.debugName = "AmbientOcclusion";
        AmbientOcclusion = device->createTexture(desc);

        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.width = displaySize.x;
        desc.height = displaySize.y;
        desc.isUAV = true;
        desc.debugName = "AAResolvedColor";
        AAResolvedColor = device->createTexture(desc);

        desc.format = nvrhi::Format::RGBA16_SNORM;
        desc.isUAV = true;
        desc.debugName = "TemporalFeedback1";
        TemporalFeedback1 = device->createTexture(desc);
        desc.debugName = "TemporalFeedback2";
        TemporalFeedback2 = device->createTexture(desc);

        desc.format = nvrhi::Format::SRGBA8_UNORM;
        desc.isUAV = false;
        desc.debugName = "LdrColor";
        LdrColor = device->createTexture(desc);

        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.isUAV = true;
        desc.debugName = "ColorspaceCorrectionColor";
        ColorspaceCorrectionColor = device->createTexture(desc);

        desc.format = backbufferFormat;
        desc.isUAV = true;
        desc.debugName = "NisColor";
        NisColor = device->createTexture(desc);

        desc.format = backbufferFormat;
        desc.isUAV = true;
        desc.debugName = "PreUIColor";
        PreUIColor = device->createTexture(desc);



        if (desc.isVirtual)
        {
            uint64_t heapSize = 0;
            nvrhi::ITexture* const textures[] = {
                HdrColor,
                AAResolvedColor,
                TemporalFeedback1,
                TemporalFeedback2,
                LdrColor,
                ColorspaceCorrectionColor,
                PreUIColor,
                NisColor,
                AmbientOcclusion
            };

            for (auto texture : textures)
            {
                nvrhi::MemoryRequirements memReq = device->getTextureMemoryRequirements(texture);
                heapSize = nvrhi::align(heapSize, memReq.alignment);
                heapSize += memReq.size;
            }

            nvrhi::HeapDesc heapDesc;
            heapDesc.type = nvrhi::HeapType::DeviceLocal;
            heapDesc.capacity = heapSize;
            heapDesc.debugName = "RenderTargetHeap";

            Heap = device->createHeap(heapDesc);

            uint64_t offset = 0;
            for (auto texture : textures)
            {
                nvrhi::MemoryRequirements memReq = device->getTextureMemoryRequirements(texture);
                offset = nvrhi::align(offset, memReq.alignment);

                device->bindTextureMemory(texture, Heap, offset);

                offset += memReq.size;
            }
        }

        ForwardFramebuffer = std::make_shared<donut::engine::FramebufferFactory>(device);
        ForwardFramebuffer->RenderTargets = { HdrColor };
        ForwardFramebuffer->DepthTarget = Depth;

        HdrFramebuffer = std::make_shared<donut::engine::FramebufferFactory>(device);
        HdrFramebuffer->RenderTargets = { HdrColor };

        LdrFramebuffer = std::make_shared<donut::engine::FramebufferFactory>(device);
        LdrFramebuffer->RenderTargets = { LdrColor };

        AAResolvedFramebuffer = std::make_shared<donut::engine::FramebufferFactory>(device);
        AAResolvedFramebuffer->RenderTargets = { AAResolvedColor };

        PreUIFramebuffer = std::make_shared<donut::engine::FramebufferFactory>(device);
        PreUIFramebuffer->RenderTargets = { PreUIColor };
    }

    bool IsUpdateRequired(donut::math::int2 renderSize, donut::math::int2 displaySize, donut::math::uint sampleCount = 1) const
    {
        if (any(m_RenderSize != renderSize) || any(m_DisplaySize != displaySize) || m_SampleCount != sampleCount) return true;
        return false;
    }

    void Clear(nvrhi::ICommandList* commandList) override
    {
        GBufferRenderTargets::Clear(commandList);

        commandList->clearTextureFloat(HdrColor, nvrhi::AllSubresources, nvrhi::Color(0.f));
        commandList->clearTextureFloat(LdrColor, nvrhi::AllSubresources, nvrhi::Color(0.f));
        commandList->clearTextureFloat(NisColor, nvrhi::AllSubresources, nvrhi::Color(0.f));
        commandList->clearTextureFloat(PreUIColor, nvrhi::AllSubresources, nvrhi::Color(0.f));
        commandList->clearTextureFloat(AAResolvedColor, nvrhi::AllSubresources, nvrhi::Color(0.f));
    }
};