#include <pch.h>
#include "IFeature_Dx11wDx12.h"

#include <dlssnr/DlssNr.h>

#include <Config.h>

#include <proxies/DXGI_Proxy.h>
#include <proxies/D3D12_Proxy.h>
#include <misc/IdentifyGpu.h>

#include <with_dx12/with_dx12.h>

namespace
{
// The RTX30 pre-NR addon uses this private D3D12-object contract to prove
// that a command list belongs to a stable graphics queue before it allows a
// Feature 18 dispatch.  The contract is intentionally kept local to the
// bridge: it is not part of the public NGX ABI.
static const GUID kDlssNrQueueAffinityGuid = {
    0x2f90ef4a, 0x3aa3, 0x4de5, { 0xb5, 0x27, 0x91, 0x52, 0x31, 0xe2, 0x8c, 0x6a }
};

struct DlssNrQueueAffinityMarker
{
    ID3D12CommandQueue* queue;
    UINT32 version;
    UINT32 flags;
};

static_assert(sizeof(DlssNrQueueAffinityMarker) == 0x10, "The RTX30 pre-NR queue-affinity marker must remain 16 bytes");

void SetDlssNrQueueAffinityMarker(ID3D12GraphicsCommandList* commandList, ID3D12CommandQueue* commandQueue)
{
    if (commandList == nullptr || commandQueue == nullptr)
        return;

    DlssNrQueueAffinityMarker marker = { commandQueue, 2u, 0u };
    const HRESULT result = commandList->SetPrivateData(kDlssNrQueueAffinityGuid, sizeof(marker), &marker);

    static bool reportedSuccess = false;
    static bool reportedFailure = false;
    if (SUCCEEDED(result))
    {
        if (!reportedSuccess)
        {
            reportedSuccess = true;
            LOG_INFO("DLSS-NR queue-affinity marker installed on bridge command list (queue={}, version=2)",
                     static_cast<void*>(commandQueue));
        }
    }
    else if (!reportedFailure)
    {
        reportedFailure = true;
        LOG_WARN("DLSS-NR queue-affinity marker SetPrivateData failed: {:X}", static_cast<UINT>(result));
    }
}
} // namespace

void IFeature_Dx11wDx12::ResourceBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                                         D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    if (beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
}

bool IFeature_Dx11wDx12::CreateD3D12Objects()
{
    HRESULT result;

    for (size_t i = 0; i < DX11WDX12_NUM_OF_BUFFERS; i++)
    {
        if (Dx12CommandAllocator[i] == nullptr)
        {
            result =
                _dx11on12Device->CreateCommandAllocator(Dx12CommandListType, IID_PPV_ARGS(&Dx12CommandAllocator[i]));

            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocator error: {:X}", result);
                return false;
            }
        }

        if (Dx12CommandList[i] == nullptr && Dx12CommandAllocator[i] != nullptr)
        {
            // CreateCommandList
            result = _dx11on12Device->CreateCommandList(0, Dx12CommandListType, Dx12CommandAllocator[i], nullptr,
                                                        IID_PPV_ARGS(&Dx12CommandList[i]));

            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList error: {:X}", result);
                return false;
            }

            SetDlssNrQueueAffinityMarker(Dx12CommandList[i], Dx12CommandQueue);
            Dx12CommandList[i]->Close();
        }
    }

    if (Dx12Fence == nullptr)
    {
        result = _dx11on12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Dx12Fence));

        if (result != S_OK)
        {
            LOG_ERROR("CreateFence error: {0:X}", result);
            return false;
        }

        Dx12FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        if (Dx12FenceEvent == nullptr)
        {
            LOG_ERROR("CreateEvent error!");
            return false;
        }
    }

    return true;
}

void IFeature_Dx11wDx12::ReleaseSharedResources()
{
    for (size_t i = 0; i < DX11WDX12_NUM_OF_BUFFERS; i++)
    {
        SAFE_RELEASE(Dx12CommandList[i]);
        SAFE_RELEASE(Dx12CommandAllocator[i]);
        Dx12CommandAllocatorFenceValue[i] = 0;
    }

    SAFE_RELEASE(Dx12Fence);
    Dx12FenceValue = 0;

    if (Dx12FenceEvent)
    {
        CloseHandle(Dx12FenceEvent);
        Dx12FenceEvent = nullptr;
    }

    // SAFE_RELEASE(Dx12Device);
}

bool IFeature_Dx11wDx12::ProcessDx11Textures(const NVSDK_NGX_Parameter* InParameters)
{
    HRESULT result;

    auto frame = _frameCount % DX11WDX12_NUM_OF_BUFFERS;
    const auto cacheFrameKey = Dx11WithDx12::NextUpscalerFrameId();
    Dx11WithDx12::SetUpscalerFrameIndex((UINT) frame);

    auto mask = Dx11WithDx12::ResourceMask::Color | Dx11WithDx12::ResourceMask::Mv | Dx11WithDx12::ResourceMask::Depth |
                Dx11WithDx12::ResourceMask::Output;

    if (!AutoExposure())
        mask |= Dx11WithDx12::ResourceMask::Exposure;
    else
        LOG_DEBUG("AutoExposure enabled!");

    const bool reactiveDisabled = Config::Instance()->DisableReactiveMask.value_or(false);
    const bool reactiveRequired = Config::Instance()->Dx11Upscaler.value_or_default() == Upscaler::XeSS ||
                                  Config::Instance()->Dx11Upscaler.value_or_default() == Upscaler::XeSS_on12;

    if (!reactiveDisabled)
        mask |= Dx11WithDx12::ResourceMask::Reactive;
    else
        LOG_DEBUG("ReactiveMask disabled!");

    const auto prepareResult = Dx11WithDx12::PrepareUpscalerResources(
        InParameters, mask, (UINT) frame, cacheFrameKey, Config::Instance()->DontUseNTShared.value_or_default(),
        reactiveRequired, true);

    if (!prepareResult.Success)
    {
        if (prepareResult.MissingExposure)
        {
            LOG_WARN("AutoExposure disabled but ExposureTexture does not exist, enabling auto exposure and changing "
                     "backend");
            State::Instance().autoExposure = true;
            State::Instance().changeBackend[Handle()->Id] = true;
            return true;
        }

        if (prepareResult.MissingReactive && reactiveRequired)
        {
            LOG_WARN("Bias mask does not exist and is required by the current DX11 upscaler, disabling reactive mask");
            Config::Instance()->DisableReactiveMask.set_volatile_value(true);
            State::Instance().changeBackend[Handle()->Id] = true;
            return true;
        }

        LOG_ERROR("Dx11wDx12 resource cache preparation failed");
        return false;
    }

    const auto allocatorFenceValue = Dx12CommandAllocatorFenceValue[frame];
    if (allocatorFenceValue != 0 && Dx12Fence->GetCompletedValue() < allocatorFenceValue)
    {
        result = Dx12Fence->SetEventOnCompletion(allocatorFenceValue, Dx12FenceEvent);
        if (result != S_OK)
        {
            LOG_ERROR("SetEventOnCompletion error for allocator {} fence {}: {:X}", frame, allocatorFenceValue,
                      (UINT) result);
            return false;
        }

        const auto waitResult = WaitForSingleObject(Dx12FenceEvent, INFINITE);
        if (waitResult != WAIT_OBJECT_0)
        {
            LOG_ERROR("WaitForSingleObject failed for allocator {} fence {}: {:X}", frame, allocatorFenceValue,
                      (UINT) waitResult);
            return false;
        }
    }

    result = Dx12CommandAllocator[frame]->Reset();
    if (result != S_OK)
    {
        LOG_ERROR("CommandAllocator Reset error for frame {}, allocator fence {}, completed {}: {:X}", frame,
                  allocatorFenceValue, Dx12Fence->GetCompletedValue(), (UINT) result);
        return false;
    }

    result = Dx12CommandList[frame]->Reset(Dx12CommandAllocator[frame], nullptr);
    if (result != S_OK)
    {
        LOG_ERROR("CommandList Reset error: {:X}", (UINT) result);
        return false;
    }

    // Some D3D12 layers recreate or clear object metadata during Reset.  Set
    // the marker again while the list is open so the addon sees the current
    // queue-affinity proof on every frame.
    SetDlssNrQueueAffinityMarker(Dx12CommandList[frame], Dx12CommandQueue);

    LOG_DEBUG("Shared handles prepared and synchronized by Dx11WithDx12 cache, frameKey: {}", cacheFrameKey);
    return true;
}

bool IFeature_Dx11wDx12::CopyBackOutput()
{
    const auto frame = (UINT) (_frameCount % DX11WDX12_NUM_OF_BUFFERS);
    return Dx11WithDx12::CopyUpscalerOutputToDx11(frame);
}

bool IFeature_Dx11wDx12::Init(ID3D11Device* InDevice, ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (IsInited())
        return true;

    if (State::Instance().NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
        State::Instance().gameEngine == GameEngineType::Unreal ||
        State::Instance().gameQuirks & GameQuirk::ForceUnrealEngine)
    {
        LOG_INFO("Dx11 detected, disabling UE resource barrier overrides");
        Config::Instance()->ColorResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Config::Instance()->MVResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    Device = InDevice;
    DeviceContext = InContext;

    if (!BaseInit(Device, InContext, InParameters))
    {
        LOG_DEBUG("BaseInit failed!");
        return false;
    }

    SetInitParameters(InParameters);

    // The list has to be recording, and what is recorded on it has to run.
    //
    // Every command list here is closed the moment it is created, and the note this replaces said
    // non-DLSS upscalers do not use the list during Init -- which was true, and stopped being true the
    // day DLSS got a bridge variant. DLSS and Ray Reconstruction are the only features whose
    // InitInternal touches the argument at all: NVSDK_NGX_D3D12_CreateFeature records the model's
    // weight upload and history initialisation onto it, and documents that the caller must submit it
    // afterwards.
    //
    // Recorded onto a closed list, all of that is discarded. ID3D12GraphicsCommandList methods return
    // void, so nothing fails, nothing logs, and CreateFeature still answers Success -- the model then
    // runs against state that was never uploaded. That is the posterised, flat-blocked picture, and it
    // gets worse with model size: the old CNN degraded to soft, the transformer collapses to blocks.
    //
    // So: open the list, let Init record into it, submit it, and wait. The wait is not optional --
    // the first Evaluate resets this same allocator, and doing that under work still in flight is a
    // device removal rather than a bad picture.
    HRESULT prep = Dx12CommandAllocator[0]->Reset();

    if (prep != S_OK)
        LOG_WARN("Init: allocator reset before feature creation failed: {:X}", (UINT) prep);

    prep = Dx12CommandList[0]->Reset(Dx12CommandAllocator[0], nullptr);

    if (prep != S_OK)
        LOG_WARN("Init: command list reset before feature creation failed: {:X}", (UINT) prep);

    SetDlssNrQueueAffinityMarker(Dx12CommandList[0], Dx12CommandQueue);

    const bool initialised = dx12Feature->Init(_dx11on12Device, Dx12CommandList[0], InParameters);

    SetInit(initialised);

    if (Dx12CommandList[0]->Close() == S_OK && Dx12CommandQueue != nullptr)
    {
        ID3D12CommandList* lists[] = { Dx12CommandList[0] };
        Dx12CommandQueue->ExecuteCommandLists(1, lists);

        // Recorded against allocator 0, so allocator 0 must not be reset until this has retired. That
        // is what Dx12CommandAllocatorFenceValue is for, and ProcessDx11Textures already honours it.
        const UINT64 signalled = ++Dx12FenceValue;

        if (Dx12CommandQueue->Signal(Dx12Fence, signalled) == S_OK)
        {
            Dx12CommandAllocatorFenceValue[0] = signalled;

            if (Dx12Fence->GetCompletedValue() < signalled &&
                Dx12Fence->SetEventOnCompletion(signalled, Dx12FenceEvent) == S_OK)
            {
                WaitForSingleObject(Dx12FenceEvent, INFINITE);
            }
        }

        LOG_INFO("Init: feature creation work submitted and waited on (fence {})", signalled);
    }
    else
    {
        LOG_WARN("Init: could not submit the feature creation work; a feature that records during "
                 "creation will be missing it");
    }

    return IsInited();
}

bool IFeature_Dx11wDx12::Evaluate(ID3D11DeviceContext* InDeviceContext, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (!IsInited())
        return false;

    ID3D11DeviceContext4* dc;
    auto result = InDeviceContext->QueryInterface(IID_PPV_ARGS(&dc));

    if (result != S_OK)
    {
        LOG_ERROR("QueryInterface error: {0:x}", result);
        return false;
    }

    if (dc != Dx11DeviceContext)
    {
        LOG_WARN("Dx11DeviceContext changed!");
        ReleaseSharedResources();
        Dx11DeviceContext = dc;

        if (!CreateD3D12Objects())
        {
            LOG_ERROR("Failed to recreate Dx11wDx12 D3D12 objects after context change");
            if (dc != nullptr)
                dc->Release();
            return false;
        }
    }

    if (dc != nullptr)
        dc->Release();

    auto frame = _frameCount % DX11WDX12_NUM_OF_BUFFERS;
    auto cmdList = Dx12CommandList[frame];

    auto& cache = Dx11WithDx12::GetUpscalerResourceCache();
    auto& dx11Color = cache.Color;
    auto& dx11Mv = cache.Mv;
    auto& dx11Depth = cache.Depth;
    auto& dx11Reactive = cache.Reactive;
    auto& dx11Exp = cache.Exposure;
    auto& dx11Out = cache.Output[frame];

    auto getOriginalNgxResource = [](NVSDK_NGX_Parameter* parameters, const char* name, ID3D11Resource** outResource)
    {
        if (parameters == nullptr || name == nullptr || outResource == nullptr)
            return false;

        *outResource = nullptr;

        if (parameters->Get(name, outResource) != NVSDK_NGX_Result_Success)
            parameters->Get(name, (void**) outResource);

        return *outResource != nullptr;
    };

    // Native D3D12 NGX keeps the typed D3D12 resource slot separate from its
    // untyped/legacy pointer slot.  The old bridge wrote only Set(void*), so
    // a native parameter table continued to expose the game's packed R11
    // resource to the pre-NR observer even after we had prepared a FP16
    // carrier.  Always save and restore both views, and write both views for
    // the bridge hand-off below.
    auto getOriginalD3D12Resource = [](NVSDK_NGX_Parameter* parameters, const char* name, ID3D12Resource** outResource)
    {
        if (parameters == nullptr || name == nullptr || outResource == nullptr)
            return false;

        *outResource = nullptr;
        return parameters->Get(name, outResource) == NVSDK_NGX_Result_Success && *outResource != nullptr;
    };

    auto setBridgeD3D12Resource = [](NVSDK_NGX_Parameter* parameters, const char* name, ID3D12Resource* resource)
    {
        if (parameters == nullptr || name == nullptr)
            return;

        // Typed first is important for the native D3D12 table.  The untyped
        // write keeps compatibility with OptiScaler's internal table and
        // with older NGX consumers that read the legacy pointer slot.
        parameters->Set(name, resource);
        parameters->Set(name, static_cast<void*>(resource));
    };

    ID3D11Resource* restoreParamColor = nullptr;
    ID3D11Resource* restoreParamMv = nullptr;
    ID3D11Resource* restoreParamOutput = nullptr;
    ID3D11Resource* restoreParamDepth = nullptr;
    ID3D11Resource* restoreParamExposure = nullptr;
    ID3D11Resource* restoreParamReactive = nullptr;

    ID3D12Resource* restoreParamColor12 = nullptr;
    ID3D12Resource* restoreParamMv12 = nullptr;
    ID3D12Resource* restoreParamOutput12 = nullptr;
    ID3D12Resource* restoreParamDepth12 = nullptr;
    ID3D12Resource* restoreParamExposure12 = nullptr;
    ID3D12Resource* restoreParamReactive12 = nullptr;

    const bool hasRestoreParamColor =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_Color, &restoreParamColor);
    const bool hasRestoreParamMv =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_MotionVectors, &restoreParamMv);
    const bool hasRestoreParamOutput =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_Output, &restoreParamOutput);
    const bool hasRestoreParamDepth =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_Depth, &restoreParamDepth);
    const bool hasRestoreParamExposure =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_ExposureTexture, &restoreParamExposure);
    const bool hasRestoreParamReactive = getOriginalNgxResource(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, &restoreParamReactive);

    const bool hasRestoreParamColor12 =
        getOriginalD3D12Resource(InParameters, NVSDK_NGX_Parameter_Color, &restoreParamColor12);
    const bool hasRestoreParamMv12 =
        getOriginalD3D12Resource(InParameters, NVSDK_NGX_Parameter_MotionVectors, &restoreParamMv12);
    const bool hasRestoreParamOutput12 =
        getOriginalD3D12Resource(InParameters, NVSDK_NGX_Parameter_Output, &restoreParamOutput12);
    const bool hasRestoreParamDepth12 =
        getOriginalD3D12Resource(InParameters, NVSDK_NGX_Parameter_Depth, &restoreParamDepth12);
    const bool hasRestoreParamExposure12 =
        getOriginalD3D12Resource(InParameters, NVSDK_NGX_Parameter_ExposureTexture, &restoreParamExposure12);
    const bool hasRestoreParamReactive12 = getOriginalD3D12Resource(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, &restoreParamReactive12);

    ID3D11ShaderResourceView* restoreSRVs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    ID3D11SamplerState* restoreSamplerStates[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    ID3D11Buffer* restoreCBVs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    ID3D11UnorderedAccessView* restoreUAVs[D3D11_1_UAV_SLOT_COUNT] = {};
    ID3D11RenderTargetView* restoreRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* restoreDSV = nullptr;

    // backup compute shader resources
    for (UINT i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; i++)
    {
        restoreSRVs[i] = nullptr;
        InDeviceContext->CSGetShaderResources(i, 1, &restoreSRVs[i]);

        if (restoreSRVs[i] != nullptr)
            restoreSRVs[i]->Release();
    }

    for (UINT i = 0; i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; i++)
    {
        restoreSamplerStates[i] = nullptr;
        InDeviceContext->CSGetSamplers(i, 1, &restoreSamplerStates[i]);

        if (restoreSamplerStates[i] != nullptr)
            restoreSamplerStates[i]->Release();
    }

    for (UINT i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; i++)
    {
        restoreCBVs[i] = nullptr;
        InDeviceContext->CSGetConstantBuffers(i, 1, &restoreCBVs[i]);

        if (restoreCBVs[i] != nullptr)
            restoreCBVs[i]->Release();
    }

    for (UINT i = 0; i < D3D11_1_UAV_SLOT_COUNT; i++)
    {
        restoreUAVs[i] = nullptr;
        InDeviceContext->CSGetUnorderedAccessViews(i, 1, &restoreUAVs[i]);

        if (restoreUAVs[i] != nullptr)
            restoreUAVs[i]->Release();
    }

    InDeviceContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, restoreRTVs, &restoreDSV);

    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
    {
        if (restoreRTVs[i] != nullptr)
            restoreRTVs[i]->Release();
    }

    if (restoreDSV != nullptr)
        restoreDSV->Release();

    // Unbind RenderTargets
    ID3D11RenderTargetView* nullRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    InDeviceContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, nullRTVs, nullptr);

    bool dx12EvalResult = false;
    bool commandListRecording = false;
    bool commandListExecuted = false;
    do
    {
        if (!ProcessDx11Textures(InParameters))
        {
            LOG_ERROR("Can't process Dx11 textures!");
            break;
        }

        if (State::Instance().changeBackend[Handle()->Id])
        {
            break;
        }

        commandListRecording = true;

        ID3D12Resource* carrierColor = dx11Color.Dx12Resource;
        const auto carrierColorDesc = carrierColor->GetDesc();

        // The interop texture may retain a packed or typeless scene-color
        // format (the current Genshin path reports R8G8B8A8_TYPELESS, enum
        // value 27; older paths used R10/R11).  The RTX30 DLSS-NR model
        // accepts a four-channel FP16 carrier, while native DLSS SR can still
        // consume the original packed resource.  Promote every ambiguous
        // packed form before the observer sees Feature 1; in particular this
        // turns the otherwise rejected typeless R8 contract into FP16.
        const bool packedHdrColor = carrierColorDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                                    carrierColorDesc.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                                    carrierColorDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                                    carrierColorDesc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;

        if (packedHdrColor)
        {
            if (PreNrColorTransfer == nullptr)
            {
                PreNrColorTransfer = std::make_unique<FT_Dx12>("Genshin pre-NR FP16 color transfer", _dx11on12Device,
                                                               DXGI_FORMAT_R16G16B16A16_FLOAT);
            }

            if (!PreNrColorTransfer->IsInit() ||
                !PreNrColorTransfer->CreateBufferResource(_dx11on12Device, carrierColor,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
            {
                LOG_ERROR("Pre-NR FP16 color resource creation failed");
                break;
            }

            PreNrColorTransfer->SetBufferState(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            if (!PreNrColorTransfer->Dispatch(cmdList, carrierColor, PreNrColorTransfer->Buffer()))
            {
                LOG_ERROR("Pre-NR R10-to-FP16 color transfer failed");
                break;
            }

            PreNrColorTransfer->SetBufferState(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            carrierColor = PreNrColorTransfer->Buffer();

            static bool reportedPreNrColorTransfer = false;
            if (!reportedPreNrColorTransfer)
            {
                reportedPreNrColorTransfer = true;
                LOG_INFO("Pre-NR carrier color promoted from packed HDR format {} to R16G16B16A16_FLOAT",
                         static_cast<int>(carrierColorDesc.Format));
            }
        }

        ID3D12Resource* carrierOutput = dx11Out.Dx12Resource;
        const auto gameOutputDesc = carrierOutput->GetDesc();
        const bool useFp16OutputCarrier = gameOutputDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                                          gameOutputDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                                          gameOutputDesc.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                                          gameOutputDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                                          gameOutputDesc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;

        if (useFp16OutputCarrier)
        {
            if (DlssOutputTransfer == nullptr)
            {
                DlssOutputTransfer = std::make_unique<FT_Dx12>("Genshin DLSS/NR FP16 output carrier", _dx11on12Device,
                                                               DXGI_FORMAT_R16G16B16A16_FLOAT);
            }

            const auto carrierMatchesOutput = [&]()
            {
                if (!DlssOutputTransfer->CanRender())
                    return false;

                const auto carrierDesc = DlssOutputTransfer->Buffer()->GetDesc();
                return carrierDesc.Width == gameOutputDesc.Width && carrierDesc.Height == gameOutputDesc.Height &&
                       carrierDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            }();

            if (!DlssOutputTransfer->IsInit() ||
                (!carrierMatchesOutput && !DlssOutputTransfer->CreateBufferResource(
                                              _dx11on12Device, carrierOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)))
            {
                LOG_ERROR("DLSS/NR FP16 output carrier creation failed");
                break;
            }

            DlssOutputTransfer->SetBufferState(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            carrierOutput = DlssOutputTransfer->Buffer();

            static bool reportedFp16OutputCarrier = false;
            if (!reportedFp16OutputCarrier)
            {
                reportedFp16OutputCarrier = true;
                LOG_INFO("DLSS/NR output redirected from format {} to R16G16B16A16_FLOAT; completed output will be "
                         "encoded back before D3D11 copy-back",
                         static_cast<int>(gameOutputDesc.Format));
            }
        }

        setBridgeD3D12Resource(InParameters, NVSDK_NGX_Parameter_Color, carrierColor);
        setBridgeD3D12Resource(InParameters, NVSDK_NGX_Parameter_MotionVectors, dx11Mv.Dx12Resource);
        setBridgeD3D12Resource(InParameters, NVSDK_NGX_Parameter_Output, carrierOutput);
        setBridgeD3D12Resource(InParameters, NVSDK_NGX_Parameter_Depth, dx11Depth.Dx12Resource);

        if (!AutoExposure() && dx11Exp.Dx12Resource != nullptr)
            setBridgeD3D12Resource(InParameters, NVSDK_NGX_Parameter_ExposureTexture, dx11Exp.Dx12Resource);

        if (!Config::Instance()->DisableReactiveMask.value_or(false) && dx11Reactive.Dx12Resource != nullptr)
            setBridgeD3D12Resource(InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,
                                   dx11Reactive.Dx12Resource);

        static bool reportedBridgeResourceFormats = false;
        if (!reportedBridgeResourceFormats)
        {
            reportedBridgeResourceFormats = true;
            const auto observedColor = [&]() -> ID3D12Resource*
            {
                ID3D12Resource* value = nullptr;
                InParameters->Get(NVSDK_NGX_Parameter_Color, &value);
                return value;
            }();
            LOG_INFO("D3D11 bridge carrier resources: sourceFmt={} carrierFmt={} outputFmt={} typedColor={:p}",
                     static_cast<int>(carrierColorDesc.Format),
                     carrierColor == nullptr ? -1 : static_cast<int>(carrierColor->GetDesc().Format),
                     carrierOutput == nullptr ? -1 : static_cast<int>(carrierOutput->GetDesc().Format),
                     static_cast<void*>(observedColor));
        }

        LOG_DEBUG("Dispatch!!");
        dx12EvalResult = dx12Feature->Evaluate(cmdList, InParameters);

        // DLSS 5 Neural Rendering rides the bridge: at this moment the block carries the D3D12 copies
        // of every input, the list is still recording, and the model's edit lands on the D3D12 output
        // before it is copied back to the game's D3D11 texture. This one call is what makes the pass
        // work in DirectX 11 games, whatever upscaler carried it here.
        static bool reportedNrOffer = false;

        if (!reportedNrOffer)
        {
            reportedNrOffer = true;
            LOG_INFO("DLSS-NR: the D3D11 bridge reached the hand-off (upscale ok: {}, enabled: {})",
                     dx12EvalResult, Config::Instance()->DlssNrEnabled.value_or_default());

        }

        if (dx12EvalResult && Config::Instance()->DlssNrEnabled.value_or_default())
        {
            DlssNr::EvaluateAfterUpscale(cmdList, InParameters, Dx12CommandQueue);

            // Asked only after the D3D12 path has had its turn. Probing first would have made a D3D11
            // init the very first thing to ever touch the snippet, and if that had left its core
            // holding a D3D11 device the D3D12 create would have failed -- killing the feature in
            // exactly the games the probe was written to help. Off by default regardless.
            DlssNr::ProbeD3D11(Dx11Device);
        }

        if (dx12EvalResult && useFp16OutputCarrier)
        {
            // The old RTX30 addon can execute either NR -> SR or SR -> NR on
            // this FP16 output. Preserve the exact result by writing it to the
            // game's typeless R8 resource only after the selected chain has
            // completed. The UNORM UAV view is the same numeric contract that
            // native DLSS used when it wrote this resource directly.
            DlssOutputTransfer->SetBufferState(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ResourceBarrier(cmdList, dx11Out.Dx12Resource, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            const bool outputTransferSucceeded =
                DlssOutputTransfer->Dispatch(cmdList, carrierOutput, dx11Out.Dx12Resource);

            ResourceBarrier(cmdList, dx11Out.Dx12Resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COMMON);
            DlssOutputTransfer->SetBufferState(cmdList, D3D12_RESOURCE_STATE_COMMON);

            if (!outputTransferSucceeded)
            {
                LOG_ERROR("DLSS/NR FP16 output encoding failed");
                dx12EvalResult = false;
                break;
            }
        }

    } while (false);

    // Clear/update the typed D3D12 slots first, then restore the original
    // D3D11/legacy slots.  This prevents a promoted carrier from remaining in
    // a native table after the call and leaking into the next frame.
    InParameters->Set(NVSDK_NGX_Parameter_Color,
                      hasRestoreParamColor12 ? restoreParamColor12 : static_cast<ID3D12Resource*>(nullptr));
    InParameters->Set(NVSDK_NGX_Parameter_MotionVectors,
                      hasRestoreParamMv12 ? restoreParamMv12 : static_cast<ID3D12Resource*>(nullptr));
    InParameters->Set(NVSDK_NGX_Parameter_Output,
                      hasRestoreParamOutput12 ? restoreParamOutput12 : static_cast<ID3D12Resource*>(nullptr));
    InParameters->Set(NVSDK_NGX_Parameter_Depth,
                      hasRestoreParamDepth12 ? restoreParamDepth12 : static_cast<ID3D12Resource*>(nullptr));
    InParameters->Set(NVSDK_NGX_Parameter_ExposureTexture,
                      hasRestoreParamExposure12 ? restoreParamExposure12 : static_cast<ID3D12Resource*>(nullptr));
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,
                      hasRestoreParamReactive12 ? restoreParamReactive12 : static_cast<ID3D12Resource*>(nullptr));

    if (hasRestoreParamColor)
        InParameters->Set(NVSDK_NGX_Parameter_Color, (void*) restoreParamColor);

    if (hasRestoreParamMv)
        InParameters->Set(NVSDK_NGX_Parameter_MotionVectors, (void*) restoreParamMv);

    if (hasRestoreParamOutput)
        InParameters->Set(NVSDK_NGX_Parameter_Output, (void*) restoreParamOutput);

    if (hasRestoreParamDepth)
        InParameters->Set(NVSDK_NGX_Parameter_Depth, (void*) restoreParamDepth);

    if (hasRestoreParamExposure)
        InParameters->Set(NVSDK_NGX_Parameter_ExposureTexture, (void*) restoreParamExposure);

    if (hasRestoreParamReactive)
        InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, (void*) restoreParamReactive);

    if (commandListRecording)
    {
        const auto closeResult = cmdList->Close();
        if (closeResult != S_OK)
        {
            LOG_ERROR("CommandList Close error: {:X}", (UINT) closeResult);
            dx12EvalResult = false;
        }
    }

    if (dx12EvalResult)
    {
        ID3D12CommandList* ppCommandLists[] = { cmdList };
        Dx12CommandQueue->ExecuteCommandLists(1, ppCommandLists);
        commandListExecuted = true;

        const auto fenceValue = ++Dx12FenceValue;
        result = Dx12CommandQueue->Signal(Dx12Fence, fenceValue);
        if (result != S_OK)
        {
            LOG_ERROR("Dx12CommandQueue Signal failed for feature fence {}: {:X}", fenceValue, (UINT) result);
            dx12EvalResult = false;
        }
        else
        {
            Dx12CommandAllocatorFenceValue[frame] = fenceValue;
        }
    }

    auto evalResult = false;

    do
    {
        if (!dx12EvalResult || !commandListExecuted)
            break;

        if (!CopyBackOutput())
        {
            LOG_ERROR("Can't copy output texture back!");
            break;
        }

        evalResult = true;

    } while (false);

    if (evalResult)
        _frameCount++;
    else
        Dx11WithDx12::ClearLastPreparedUpscalerFrameState();

    // restore compute shader resources
    for (UINT i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; i++)
    {
        InDeviceContext->CSSetShaderResources(i, 1, &restoreSRVs[i]);
    }

    for (UINT i = 0; i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; i++)
    {
        InDeviceContext->CSSetSamplers(i, 1, &restoreSamplerStates[i]);
    }

    for (UINT i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; i++)
    {
        InDeviceContext->CSSetConstantBuffers(i, 1, &restoreCBVs[i]);
    }

    for (UINT i = 0; i < D3D11_1_UAV_SLOT_COUNT; i++)
    {
        InDeviceContext->CSSetUnorderedAccessViews(i, 1, &restoreUAVs[i], 0);
    }

    InDeviceContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, restoreRTVs, restoreDSV);

    return evalResult;
}

bool IFeature_Dx11wDx12::BaseInit(ID3D11Device* InDevice, ID3D11DeviceContext* InContext,
                                  NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (IsInited())
        return true;

    if (!InContext)
    {
        LOG_ERROR("context is null!");
        return false;
    }

    auto contextResult = InContext->QueryInterface(IID_PPV_ARGS(&Dx11DeviceContext));
    if (contextResult != S_OK)
    {
        LOG_ERROR("QueryInterface ID3D11DeviceContext4 result: {0:x}", contextResult);
        return false;
    }
    else
    {
        Dx11DeviceContext->Release();
    }

    if (!InDevice)
        Dx11DeviceContext->GetDevice(&InDevice);

    auto dx11DeviceResult = InDevice->QueryInterface(IID_PPV_ARGS(&Dx11Device));

    if (dx11DeviceResult != S_OK)
    {
        LOG_ERROR("QueryInterface ID3D11Device5 result: {0:x}", dx11DeviceResult);
        return false;
    }
    else
    {
        Dx11Device->Release();
    }

    if (!WithDx12::PrepareD3D12ForD3D11(InDevice, D3D_FEATURE_LEVEL_11_0))
    {
        LOG_ERROR("Cannot resolve D3D12 device/queue from WithDx12!");
        return false;
    }

    _dx11on12Device = WithDx12::GetD3D12Device();
    if (_dx11on12Device == nullptr)
    {
        LOG_ERROR("Cannot get D3D12 device from WithDx12!");
        return false;
    }

    Dx12CommandQueue = WithDx12::GetD3D12CommandQueue();
    if (Dx12CommandQueue == nullptr)
    {
        LOG_ERROR("Cannot get D3D12 command queue from WithDx12!");
        return false;
    }

    Dx12CommandListType = WithDx12::GetD3D12CommandListType();

    if (!CreateD3D12Objects())
    {
        LOG_ERROR("Failed to create D3D12 objects!");
        return false;
    }

    Dx11WithDx12::Init(Dx11Device, Dx11DeviceContext);

    return true;
}

IFeature_Dx11wDx12::IFeature_Dx11wDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx11(InHandleId, InParameters)
{
}

IFeature_Dx11wDx12::~IFeature_Dx11wDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    ReleaseSharedResources();
}
