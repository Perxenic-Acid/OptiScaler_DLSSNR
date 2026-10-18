#include "pch.h"

#include "Config.h"
#include "Util.h"

#include <upscalers/IFeature_Dx11.h>
#include <upscalers/FeatureProvider_Dx11.h>

#include "NVNGX_DLSS.h"
#include "NVNGX_Parameter.h"
#include "proxies/NVNGX_Proxy.h"

#include <with_dx12/with_dx12.h>
#include "FG/Upscaler_Inputs_Dx11wDx12.h"

#include <d3d11_4.h>

#include <ankerl/unordered_dense.h>
#include <imgui/ImGuiNotify.hpp>
#include <misc/IdentifyGpu.h>

static ID3D11Device* D3D11Device = nullptr;
static ankerl::unordered_dense::map<unsigned int, ContextData<IFeature_Dx11>> Dx11Contexts;
static int evalCounter = 0;
static bool shutdown = false;
static bool _skipInit = false;
static wchar_t const** paths;

// A D3D11 game using the DLSS-on-D3D12 bridge needs the native NGX core to be
// initialised before the bridge creates its private D3D12 feature (this is the
// ordering expected by the RTX 30/40 driver).  It still must receive
// OptiScaler's parameter object, however: the bridge and the pre-NR add-on
// exchange D3D12 resource pointers through that object.  Keep the two
// decisions independent instead of using the old "defer everything" switch.
static bool forceCustomDx11Parameters = false;

static bool UsesD3D12Bridge()
{
    return Config::Instance()->Dx11Upscaler.value_or_default() == Upscaler::DLSS_on12;
}

static bool UseCustomDx11Parameters()
{
    return forceCustomDx11Parameters || UsesD3D12Bridge();
}

static ID3D11Device* ResolveGimiPassThroughDevice(ID3D11Device* device, bool* acquiredReference);

// The bridge is entered through the D3D11 NGX ABI, but the actual DLSS
// feature is created by the native D3D12 runtime.  Passing OptiScaler's C++
// parameter implementation to that runtime is unsafe (the driver uses
// private storage in its parameter object).  Once the D3D11 device is known,
// prepare the same interop device the feature would create and use the
// driver's native D3D12 parameter table for the bridge hand-off.
static bool EnsureNativeD3D12ForBridgeParams()
{
    if (!UsesD3D12Bridge() || D3D11Device == nullptr)
        return false;

    if (!WithDx12::PrepareD3D12ForD3D11(D3D11Device, D3D_FEATURE_LEVEL_11_0))
    {
        LOG_WARN("Bridge parameter hand-off could not prepare the D3D12 interop device");
        return false;
    }

    auto* device = WithDx12::GetD3D12Device();
    if (device == nullptr || !NVNGXProxy::InitDx12(device))
    {
        LOG_WARN("Bridge parameter hand-off could not initialise native D3D12 NGX");
        return false;
    }

    return true;
}

// Return the driver's D3D12 parameter implementation when the D3D11 entry
// point is being used as a DLSS-on-D3D12 bridge.  The object returned by the
// D3D12 NGX ABI has the private storage expected by the driver and also
// accepts ID3D12Resource values written by IFeature_Dx11wDx12.  This is the
// only parameter representation that is valid on both sides of the bridge.
// The caller owns the returned table according to the allocation type set
// below (persistent for GetParameters, dynamic for the other two APIs).
enum class NativeParamRequest
{
    Persistent,
    Capability,
    Dynamic,
};

static bool TryGetNativeD3D12Parameters(NVSDK_NGX_Parameter** outParameters, NativeParamRequest request)
{
    if (outParameters == nullptr || !UsesD3D12Bridge() || !Config::Instance()->DLSSEnabled.value_or_default() ||
        NVNGXProxy::NVNGXModule() == nullptr)
        return false;

    if (!EnsureNativeD3D12ForBridgeParams())
        return false;

    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
    switch (request)
    {
    case NativeParamRequest::Persistent:
        if (NVNGXProxy::D3D12_GetParameters() != nullptr)
            result = NVNGXProxy::D3D12_GetParameters()(outParameters);
        break;
    case NativeParamRequest::Capability:
        if (NVNGXProxy::D3D12_GetCapabilityParameters() != nullptr)
            result = NVNGXProxy::D3D12_GetCapabilityParameters()(outParameters);
        break;
    case NativeParamRequest::Dynamic:
        if (NVNGXProxy::D3D12_AllocateParameters() != nullptr)
            result = NVNGXProxy::D3D12_AllocateParameters()(outParameters);
        break;
    }

    if (result != NVSDK_NGX_Result_Success || *outParameters == nullptr)
    {
        LOG_WARN("D3D11 bridge native D3D12 parameter request {} failed: result=0x{:X} ptr={:p}",
                 static_cast<int>(request), static_cast<UINT>(result),
                 outParameters == nullptr ? nullptr : static_cast<void*>(*outParameters));
        return false;
    }

    // Add the normal OptiScaler capability/configuration keys to the native
    // object. Unknown keys are legal in NGX parameter tables and are needed by
    // the bridge/add-on handshake (DLSSNR.* and the resource aliases).
    InitNGXParameters(*outParameters, API::DX12);
    SetNGXParamAllocType(*(*outParameters),
                         request == NativeParamRequest::Persistent ? NGX_AllocTypes::NVPersistent
                                                                     : NGX_AllocTypes::NVDynamic);

    LOG_INFO("D3D11 bridge using native D3D12 NGX parameters: request={} ptr={:p}", static_cast<int>(request),
             static_cast<void*>(*outParameters));
    return true;
}

// Marker exposed by the bundled GIMI device wrapper. Native NGX requires the
// device and context to be the original pair, not the wrapper identity.
static constexpr GUID kGimiHackerDevice = {0x83ffd841,
                                           0xa5c9,
                                           0x46f4,
                                           {0x81, 0x09, 0xbc, 0x25, 0x95, 0x58, 0xfe, 0xf4}};

// Private ABI exported by the customized GIMI HackerContext. QueryInterface
// returns the ID3D11DeviceContext1 trampoline that calls the saved system
// D3D11 entry points without re-entering GIMI's process-wide context hooks.
static constexpr GUID kGimiPassThroughContext = {0x91acfd68,
                                                 0x5a6f,
                                                 0x45ea,
                                                 {0xb8, 0xd0, 0x71, 0xac, 0xc3, 0x21, 0x51, 0xb7}};

// Private ABI exported by the matching customized GIMI HackerDevice. This is
// needed before CreateFeature, since capability-parameter requests may already
// need to prepare the private D3D12 interop device.
static constexpr GUID kGimiPassThroughDevice = {0xdb17dc9a,
                                                 0x5a5a,
                                                 0x4ac7,
                                                 {0xa4, 0xce, 0xef, 0x41, 0xf7, 0xc5, 0x1d, 0x5c}};

static ID3D11Device* ResolveGimiPassThroughDevice(ID3D11Device* device, bool* acquiredReference)
{
    if (acquiredReference != nullptr)
        *acquiredReference = false;
    if (device == nullptr)
        return nullptr;

    ID3D11Device* nativeDevice = nullptr;
    if (SUCCEEDED(device->QueryInterface(kGimiPassThroughDevice,
                                         reinterpret_cast<void**>(&nativeDevice))) &&
        nativeDevice != nullptr)
    {
        if (acquiredReference != nullptr)
            *acquiredReference = true;
        LOG_INFO("GIMI pass-through device resolved before NGX parameter allocation: wrapper={:p}, device={:p}",
                 static_cast<void*>(device), static_cast<void*>(nativeDevice));
        return nativeDevice;
    }

    return device;
}

static bool IsGimiDevice(ID3D11Device* device)
{
    if (device == nullptr)
        return false;

    IUnknown* marker = nullptr;
    const auto result = device->QueryInterface(kGimiHackerDevice, reinterpret_cast<void**>(&marker));
    if (SUCCEEDED(result) && marker != nullptr)
    {
        marker->Release();
        return true;
    }

    return false;
}

struct NativeD3D11Context
{
    ID3D11DeviceContext1* context1 = nullptr;
    ID3D11Device* device = nullptr;

    NativeD3D11Context() = default;
    NativeD3D11Context(const NativeD3D11Context&) = delete;
    NativeD3D11Context& operator=(const NativeD3D11Context&) = delete;

    NativeD3D11Context(NativeD3D11Context&& other) noexcept
        : context1(other.context1), device(other.device)
    {
        other.context1 = nullptr;
        other.device = nullptr;
    }

    NativeD3D11Context& operator=(NativeD3D11Context&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            context1 = other.context1;
            device = other.device;
            other.context1 = nullptr;
            other.device = nullptr;
        }
        return *this;
    }

    ~NativeD3D11Context() { Reset(); }

    void Reset()
    {
        if (device != nullptr)
        {
            device->Release();
            device = nullptr;
        }
        if (context1 != nullptr)
        {
            context1->Release();
            context1 = nullptr;
        }
    }

    ID3D11DeviceContext* Context() const { return context1; }
};

static NativeD3D11Context GetNativeD3D11Context(ID3D11DeviceContext* context)
{
    NativeD3D11Context native;
    if (context == nullptr)
        return native;

    if (SUCCEEDED(context->QueryInterface(kGimiPassThroughContext,
                                         reinterpret_cast<void**>(&native.context1))) &&
        native.context1 != nullptr)
    {
        native.context1->GetDevice(&native.device);
        if (native.device != nullptr)
        {
            static ID3D11DeviceContext* lastWrapper = nullptr;
            static ID3D11DeviceContext1* lastTrampoline = nullptr;
            static ID3D11Device* lastDevice = nullptr;
            if (context != lastWrapper || native.context1 != lastTrampoline || native.device != lastDevice)
            {
                LOG_INFO("GIMI pass-through context resolved: supplied={:p}, trampoline={:p}, device={:p}",
                         static_cast<void*>(context), static_cast<void*>(native.context1),
                         static_cast<void*>(native.device));
                lastWrapper = context;
                lastTrampoline = native.context1;
                lastDevice = native.device;
            }
        }
        return native;
    }

    // Non-GIMI callers still need a same-device context. Context1 is widely
    // available and does not change pointer identity on a native D3D11 device.
    if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&native.context1))) && native.context1 != nullptr)
    {
        native.context1->GetDevice(&native.device);
        return native;
    }

    LOG_DEBUG("Native ID3D11DeviceContext1 unavailable; keeping supplied context");
    return native;
}

class ScopedInitDx11
{
  private:
    bool previousState;

  public:
    ScopedInitDx11()
    {
        previousState = _skipInit;
        _skipInit = true;
    }

    ~ScopedInitDx11() { _skipInit = previousState; }
};

static void UpdateInitPaths(NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    State::Instance().NVNGX_FeatureInfo_Paths.clear();

    if (InFeatureInfo != nullptr)
    {
        auto exePath = Util::ExePath().remove_filename();

        std::optional<std::filesystem::path> nvngxDlssPath = std::nullopt;
        std::optional<std::filesystem::path> nvngxDlssDPath = std::nullopt;
        std::optional<std::filesystem::path> nvngxDlssGPath = std::nullopt;

        // Check DLSS path
        if (State::Instance().NVNGX_DLSS_Path.has_value())
        {
            nvngxDlssPath = std::filesystem::path(State::Instance().NVNGX_DLSS_Path.value());
        }
        else
        {
            auto path = Util::FindFilePath(exePath, "nvngx_dlss.dll");

            if (path.has_value())
                nvngxDlssPath = path.value();
        }

        // Check DLSS-D path
        if (State::Instance().NVNGX_DLSSD_Path.has_value())
        {
            nvngxDlssDPath = std::filesystem::path(State::Instance().NVNGX_DLSSD_Path.value());
        }
        else
        {
            auto path = Util::FindFilePath(exePath, "nvngx_dlssd.dll");

            if (path.has_value())
                nvngxDlssDPath = path.value();
        }

        // Check DLSS-G path
        if (State::Instance().NVNGX_DLSSG_Path.has_value())
        {
            nvngxDlssGPath = std::filesystem::path(State::Instance().NVNGX_DLSSG_Path.value());
        }
        else
        {
            auto path = Util::FindFilePath(exePath, "nvngx_dlssg.dll");

            if (path.has_value())
                nvngxDlssGPath = path.value();
        }

        // Override locations
        if (Config::Instance()->DLSSFeaturePath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(Config::Instance()->DLSSFeaturePath.value());

        // If DLSS path is overriden
        if (Config::Instance()->NVNGX_DLSS_Library.has_value() && nvngxDlssPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssPath.value().parent_path().wstring());

        // OptiDll Path
        State::Instance().NVNGX_FeatureInfo_Paths.push_back(Config::Instance()->MainDllPath.value());

        // Original paths from NVNGX
        for (size_t i = 0; i < InFeatureInfo->PathListInfo.Length; i++)
        {
            const wchar_t* path = InFeatureInfo->PathListInfo.Path[i];
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(std::wstring(path));
        }

        // Exe path
        State::Instance().NVNGX_FeatureInfo_Paths.push_back(exePath.wstring());

        // If DLSS path is not overriden
        if (!Config::Instance()->NVNGX_DLSS_Library.has_value() && nvngxDlssPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssPath.value().parent_path().wstring());

        // Add found locations
        if (nvngxDlssDPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssDPath.value().parent_path().wstring());

        if (nvngxDlssGPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssGPath.value().parent_path().wstring());

        // Build pointer array
        paths = new const wchar_t*[State::Instance().NVNGX_FeatureInfo_Paths.size()];
        for (size_t i = 0; i < State::Instance().NVNGX_FeatureInfo_Paths.size(); ++i)
        {
            paths[i] = State::Instance().NVNGX_FeatureInfo_Paths[i].c_str();
            LOG_DEBUG("Feature Path [{}]: {}", i, wstring_to_string(State::Instance().NVNGX_FeatureInfo_Paths[i]));
        }

        InFeatureInfo->PathListInfo.Path = paths;
        InFeatureInfo->PathListInfo.Length = (int) State::Instance().NVNGX_FeatureInfo_Paths.size();
    }
}

#pragma region NVSDK_NGX_D3D11_Init

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Init_Ext(unsigned long long InApplicationId,
                                                        const wchar_t* InApplicationDataPath, ID3D11Device* InDevice,
                                                        NVSDK_NGX_Version InSDKVersion,
                                                        const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    NVSDK_NGX_FeatureCommonInfo localFeatureInfo = {};

    if (InFeatureInfo != nullptr)
        std::memcpy(&localFeatureInfo, InFeatureInfo, sizeof(NVSDK_NGX_FeatureCommonInfo));

    if (!_skipInit)
        UpdateInitPaths(&localFeatureInfo);

    State::Instance().NVNGX_ApplicationId = InApplicationId;
    State::Instance().NVNGX_ApplicationDataPath = std::wstring(InApplicationDataPath);
    State::Instance().NVNGX_Version = InSDKVersion;
    State::Instance().NVNGX_FeatureInfo = InFeatureInfo;
    State::Instance().NVNGX_Version = InSDKVersion;

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InApplicationId = app_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D11_Init_Ext() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D11_Init_Ext");

            auto result = NVNGXProxy::D3D11_Init_Ext()(InApplicationId, InApplicationDataPath, InDevice, InSDKVersion,
                                                       &localFeatureInfo);

            LOG_INFO("calling NVNGXProxy::D3D11_Init_Ext result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx11Inited(true);
        }
        else
        {
            LOG_WARN("NVNGXProxy::NVNGXModule or NVNGXProxy::D3D11_Init_Ext is nullptr!");
        }
    }

    if (InFeatureInfo != nullptr && InSDKVersion > 0x0000013)
        State::Instance().NVNGX_Logger = InFeatureInfo->LoggingInfo;

    if (State::Instance().nvngxDx11Inited && InDevice == D3D11Device)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    LOG_INFO("AppId: {0}", InApplicationId);
    LOG_INFO("SDK: {0:x}", (unsigned int) InSDKVersion);
    LOG_INFO(L"InApplicationDataPath {0}", std::wstring(InApplicationDataPath));

    if (InDevice)
        D3D11Device = InDevice;

    State::Instance().currentD3D11Device = InDevice;
    State::Instance().nvngxDx11Inited = true;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Init(unsigned long long InApplicationId,
                                                    const wchar_t* InApplicationDataPath, ID3D11Device* InDevice,
                                                    const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
                                                    NVSDK_NGX_Version InSDKVersion)
{
    NVSDK_NGX_FeatureCommonInfo localFeatureInfo = {};

    if (InFeatureInfo != nullptr)
        std::memcpy(&localFeatureInfo, InFeatureInfo, sizeof(NVSDK_NGX_FeatureCommonInfo));

    if (!_skipInit)
        UpdateInitPaths(&localFeatureInfo);

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InApplicationId = app_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D11_Init() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D11_Init");

            auto result = NVNGXProxy::D3D11_Init()(InApplicationId, InApplicationDataPath, InDevice, &localFeatureInfo,
                                                   InSDKVersion);

            LOG_INFO("calling NVNGXProxy::D3D11_Init result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx11Inited(true);
        }
    }

    ScopedInitDx11 scopedInit {};
    auto result = NVSDK_NGX_D3D11_Init_Ext(0x1337, InApplicationDataPath, InDevice, InSDKVersion, &localFeatureInfo);
    LOG_DEBUG("was called NVSDK_NGX_D3D11_Init_Ext");
    return result;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Init_ProjectID(const char* InProjectId,
                                                              NVSDK_NGX_EngineType InEngineType,
                                                              const char* InEngineVersion,
                                                              const wchar_t* InApplicationDataPath,
                                                              ID3D11Device* InDevice, NVSDK_NGX_Version InSDKVersion,
                                                              const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    NVSDK_NGX_FeatureCommonInfo localFeatureInfo = {};

    if (InFeatureInfo != nullptr)
        std::memcpy(&localFeatureInfo, InFeatureInfo, sizeof(NVSDK_NGX_FeatureCommonInfo));

    if (!_skipInit)
        UpdateInitPaths(&localFeatureInfo);

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InProjectId = project_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D11_Init_ProjectID() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D11_Init_ProjectID");

            auto result =
                NVNGXProxy::D3D11_Init_ProjectID()(InProjectId, InEngineType, InEngineVersion, InApplicationDataPath,
                                                   InDevice, InSDKVersion, &localFeatureInfo);

            LOG_INFO("calling NVNGXProxy::D3D11_Init_ProjectID result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx11Inited(true);
        }
    }

    ScopedInitDx11 scopedInit {};
    auto result = NVSDK_NGX_D3D11_Init_Ext(0x1337, InApplicationDataPath, InDevice, InSDKVersion, &localFeatureInfo);

    LOG_INFO("InProjectId: {0}", InProjectId);
    LOG_INFO("InEngineType: {0}", (int) InEngineType);
    LOG_INFO("InEngineVersion: {0}", InEngineVersion);

    State::Instance().NVNGX_ProjectId = std::string(InProjectId);
    State::Instance().NVNGX_Engine = InEngineType;
    State::Instance().NVNGX_EngineVersion = std::string(InEngineVersion);

    return result;
}

// Not sure about this one, original nvngx does not export this method
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Init_with_ProjectID(
    const char* InProjectId, NVSDK_NGX_EngineType InEngineType, const char* InEngineVersion,
    const wchar_t* InApplicationDataPath, ID3D11Device* InDevice, const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
    NVSDK_NGX_Version InSDKVersion)
{
    NVSDK_NGX_FeatureCommonInfo localFeatureInfo = {};
    bool nativeDeviceReference = false;
    ID3D11Device* nativeInitDevice = nullptr;

    // Keep the bridge device available before any parameter query.  Some
    // games ask for capability parameters immediately after this entry point
    // returns, before CreateFeature has had a chance to recover the device
    // from a context.
    if (InDevice != nullptr)
    {
        nativeInitDevice = ResolveGimiPassThroughDevice(InDevice, &nativeDeviceReference);
        D3D11Device = nativeInitDevice;
        State::Instance().currentD3D11Device = nativeInitDevice;
    }

    if (InFeatureInfo != nullptr)
        std::memcpy(&localFeatureInfo, InFeatureInfo, sizeof(NVSDK_NGX_FeatureCommonInfo));

    if (!_skipInit)
        UpdateInitPaths(&localFeatureInfo);

    const char* projectId = InProjectId;
    if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
        projectId = project_id_override;

    LOG_INFO("D3D11 with_ProjectID entry: skipInit={} dlssEnabled={} bridge={} ngxModule={:p}",
             _skipInit, Config::Instance()->DLSSEnabled.value_or_default(), UsesD3D12Bridge(),
             static_cast<void*>(NVNGXProxy::NVNGXModule()));

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        const bool usesD3D12Bridge = UsesD3D12Bridge();
        const bool isGimiDevice = IsGimiDevice(InDevice);
        const bool hasNativeGimiDevice = isGimiDevice && nativeInitDevice != nullptr && nativeInitDevice != InDevice;
        forceCustomDx11Parameters = isGimiDevice || usesD3D12Bridge;
        // The matching GIMI runtime exposes its original device before any
        // parameter query, so RTX 30/40 can use the required eager native NGX
        // init without pairing NGX with the HackerDevice wrapper. Older GIMI
        // builds retain the safe deferred path and will be diagnosed clearly.
        const bool deferNativeInit = isGimiDevice && !hasNativeGimiDevice;

        LOG_INFO("D3D11 init policy: gimiMarker={} bridge={} nativeGimiDevice={} deferNativeInit={} customParams={}",
                 isGimiDevice, usesD3D12Bridge, hasNativeGimiDevice, deferNativeInit, forceCustomDx11Parameters);

        if (deferNativeInit)
        {
            LOG_INFO("GIMI D3D11 wrapper detected; deferring native NGX init until CreateFeature");
        }
        else if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D11_Init_ProjectID() != nullptr)
        {
            const auto nativeResult = NVNGXProxy::D3D11_Init_ProjectID()(
                projectId, InEngineType, InEngineVersion, InApplicationDataPath, nativeInitDevice, InSDKVersion,
                &localFeatureInfo);
            LOG_INFO("calling NVNGXProxy::D3D11_Init_ProjectID from with_ProjectID result: {0:X}; custom bridge parameters: {}",
                     (UINT) nativeResult, forceCustomDx11Parameters);

            if (nativeResult == NVSDK_NGX_Result_Success ||
                nativeResult == NVSDK_NGX_Result_FAIL_FeatureAlreadyExists)
                NVNGXProxy::SetDx11Inited(true);
        }
    }

    ScopedInitDx11 scopedInit {};
    auto result = NVSDK_NGX_D3D11_Init_Ext(0x1337, InApplicationDataPath, InDevice, InSDKVersion,
                                           &localFeatureInfo);

    LOG_INFO("InProjectId: {0}", projectId);
    LOG_INFO("InEngineType: {0}", (int) InEngineType);
    LOG_INFO("InEngineVersion: {0}", InEngineVersion);

    State::Instance().NVNGX_ProjectId = std::string(projectId);
    State::Instance().NVNGX_Engine = InEngineType;
    State::Instance().NVNGX_EngineVersion = std::string(InEngineVersion);

    if (nativeDeviceReference && nativeInitDevice != nullptr)
        nativeInitDevice->Release();

    return result;
}

#pragma endregion

#pragma region NVSDK_NGX_D3D11_Shutdown

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Shutdown()
{
    shutdown = true;

    // for (auto const& [key, val] : Dx11Contexts)
    //{
    //     if (val.feature)
    //         NVSDK_NGX_D3D11_ReleaseFeature(val.feature->Handle());
    // }

    // Dx11Contexts.clear();

    D3D11Device = nullptr;
    State::Instance().currentFeature = nullptr;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsDx11Inited() &&
        NVNGXProxy::D3D11_Shutdown() != nullptr)
    {
        auto result = NVNGXProxy::D3D11_Shutdown()();
        NVNGXProxy::SetDx11Inited(false);
    }

    // Unhooking and cleaning stuff causing issues during shutdown.
    // Disabled for now to check if it cause any issues
    // HooksDx::UnHook();

    shutdown = false;
    State::Instance().nvngxDx11Inited = false;

    Dx11WithDx12::ResetUpscalerResourceCache(true);

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Shutdown1(ID3D11Device* InDevice)
{
    shutdown = true;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsDx11Inited() &&
        NVNGXProxy::D3D11_Shutdown1() != nullptr)
    {
        auto result = NVNGXProxy::D3D11_Shutdown1()(InDevice);
        NVNGXProxy::SetDx11Inited(false);
    }

    return NVSDK_NGX_D3D11_Shutdown();
}

#pragma endregion

#pragma region NVSDK_NGX_D3D11 Parameters

/**
 * @brief [Deprecated NGX API] Superceeded by NVSDK_NGX_AllocateParameters and NVSDK_NGX_GetCapabilityParameters.
 *
 * Retrieves a common NVSDK parameter map for providing params to the SDK. The lifetime of this
 * map is NOT managed by the application. It is expected to be managed internally by the SDK.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_GetParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (OutParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    // A DLSS-on-D3D12 bridge must hand the same native D3D12 parameter
    // object to the real D3D12 Create/Evaluate calls.  Trying this first is
    // what avoids the RTX 30 driver's private-ABI crash while preserving the
    // D3D12 resource slots consumed by the pre-NR observer.
    if (TryGetNativeD3D12Parameters(OutParameters, NativeParamRequest::Persistent))
        return NVSDK_NGX_Result_Success;

    if (!UseCustomDx11Parameters() && Config::Instance()->DLSSEnabled.value_or_default() &&
        NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D11_GetParameters() != nullptr)
    {
        LOG_INFO("calling NVNGXProxy::D3D11_GetParameters");

        auto result = NVNGXProxy::D3D11_GetParameters()(OutParameters);

        LOG_INFO("calling NVNGXProxy::D3D11_GetParameters result: {0:X}", (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            InitNGXParameters(*OutParameters, API::DX11);
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVPersistent);
            return result;
        }
    }

    // Get custom parameters if using custom backend
    static NVNGX_Parameters oldParams = NVNGX_Parameters(API::DX11, true);
    *OutParameters = &oldParams;
    InitNGXParameters(*OutParameters, API::DX11);

    LOG_DEBUG("Returning custom Opti parameters");

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Allocates a new NVSDK parameter map pre-populated with NGX capabilities and information about available
 * features. The output parameter map may also be used in the same ways as a parameter map allocated with
 * AllocateParameters(). The lifetime of this map is managed by the calling application with DestroyParameters().
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_GetCapabilityParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (OutParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    if (TryGetNativeD3D12Parameters(OutParameters, NativeParamRequest::Capability))
        return NVSDK_NGX_Result_Success;

    if (!UseCustomDx11Parameters() && Config::Instance()->DLSSEnabled.value_or_default() &&
        NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::IsDx11Inited() &&
        NVNGXProxy::D3D11_GetCapabilityParameters() != nullptr)
    {
        LOG_INFO("calling NVNGXProxy::D3D11_GetCapabilityParameters");

        auto result = NVNGXProxy::D3D11_GetCapabilityParameters()(OutParameters);

        LOG_INFO("calling NVNGXProxy::D3D11_GetCapabilityParameters result: {0:X}", (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            InitNGXParameters(*OutParameters, API::DX11);
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVDynamic);
            return result;
        }
    }

    *OutParameters = new NVNGX_Parameters(API::DX11, false);
    InitNGXParameters(*OutParameters, API::DX11);

    LOG_DEBUG("Returning custom Opti parameters");

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Allocates a new parameter map used to provide parameters needed by the DLSS API. The lifetime of this map
 * is managed by the calling application with DestroyParameters().
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_AllocateParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (TryGetNativeD3D12Parameters(OutParameters, NativeParamRequest::Dynamic))
        return NVSDK_NGX_Result_Success;

    if (!UseCustomDx11Parameters() && Config::Instance()->DLSSEnabled.value_or_default() &&
        NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D11_AllocateParameters() != nullptr)
    {
        LOG_INFO("calling NVNGXProxy::D3D11_AllocateParameters");

        auto result = NVNGXProxy::D3D11_AllocateParameters()(OutParameters);

        LOG_INFO("calling NVNGXProxy::D3D11_AllocateParameters result: {0:X}", (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVDynamic);
            return result;
        }
    }

    *OutParameters = new NVNGX_Parameters(API::DX11, false);

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_PopulateParameters_Impl(NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (InParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    InitNGXParameters(InParameters, API::DX11);

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_DestroyParameters(NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (InParameters == nullptr)
        return NVSDK_NGX_Result_Fail;

    // Objects returned by the bridge path above are native D3D12 tables and
    // must be released through the matching D3D12 entry point.  The custom
    // fallback is also safe here (TryDestroy sees InternDynamic and uses
    // delete), while non-bridge callers retain the normal D3D11 path.
    bool success = false;
    if (UsesD3D12Bridge() && NVNGXProxy::IsDx12Inited())
        success = TryDestroyNGXParameters(InParameters, NVNGXProxy::D3D12_DestroyParameters());

    if (!success)
        success = TryDestroyNGXParameters(InParameters, NVNGXProxy::D3D11_DestroyParameters());

    return success ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_GetScratchBufferSize(NVSDK_NGX_Feature InFeatureId,
                                                                    const NVSDK_NGX_Parameter* InParameters,
                                                                    size_t* OutSizeInBytes)
{
    LOG_WARN("-> 52428800");
    *OutSizeInBytes = 52428800;
    return NVSDK_NGX_Result_Success;
}

#pragma endregion

#pragma region NVSDK_NGX_D3D11 Feature

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_CreateFeature(ID3D11DeviceContext* InDevCtx,
                                                             NVSDK_NGX_Feature InFeatureID,
                                                             NVSDK_NGX_Parameter* InParameters,
                                                             NVSDK_NGX_Handle** OutHandle)
{
    NativeD3D11Context nativeContext = GetNativeD3D11Context(InDevCtx);
    ID3D11DeviceContext* featureContext = nativeContext.Context() != nullptr ? nativeContext.Context() : InDevCtx;
    bool featureDeviceRefFromGetDevice = false;

    if (nativeContext.device != nullptr)
    {
        D3D11Device = nativeContext.device;
    }
    else if (D3D11Device == nullptr && featureContext != nullptr)
    {
        featureContext->GetDevice(&D3D11Device);
        featureDeviceRefFromGetDevice = D3D11Device != nullptr;
    }

    if (featureContext == nullptr || D3D11Device == nullptr)
    {
        LOG_ERROR("Can't resolve a D3D11 device/context for CreateFeature");
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }

    // FeatureId check
    if (InFeatureID != NVSDK_NGX_Feature_SuperSampling && InFeatureID != NVSDK_NGX_Feature_RayReconstruction)
    {
        if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::InitDx11(D3D11Device) &&
            NVNGXProxy::D3D11_CreateFeature() != nullptr)
        {
            auto result = NVNGXProxy::D3D11_CreateFeature()(featureContext, InFeatureID, InParameters, OutHandle);
            LOG_INFO("D3D11_CreateFeature result for ({0}): {1:X}", (int) InFeatureID, (UINT) result);
            if (featureDeviceRefFromGetDevice)
                D3D11Device->Release();
            return result;
        }
        else
        {
            LOG_ERROR("Can't create this feature ({0})!", (int) InFeatureID);
            if (featureDeviceRefFromGetDevice)
                D3D11Device->Release();
            return NVSDK_NGX_Result_Fail;
        }
    }

    // CreateFeature
    auto handleId = IFeature::GetNextHandleId();
    LOG_INFO("HandleId: {0}", handleId);

    if (InFeatureID == NVSDK_NGX_Feature_SuperSampling)
    {
        Upscaler upscalerChoice = Upscaler::FSR22; // Default FSR 2.2.1

        // If original NVNGX available use DLSS as base upscaler
        if (IdentifyGpu::getPrimaryGpu().dlssCapable && NVNGXProxy::IsDx11Inited())
            upscalerChoice = Upscaler::DLSS;

        if (Config::Instance()->Dx11Upscaler.has_value())
            upscalerChoice = Config::Instance()->Dx11Upscaler.value();

        LOG_INFO("Creating new {} feature", UpscalerDisplayName(upscalerChoice));

        Dx11Contexts[handleId] = {};

        if (!FeatureProvider_Dx11::GetFeature(upscalerChoice, handleId, InParameters, &Dx11Contexts[handleId].feature))
        {
            LOG_ERROR("Can't create {} feature", UpscalerDisplayName(upscalerChoice));
            return NVSDK_NGX_Result_Fail;
        }
    }
    else if (InFeatureID == NVSDK_NGX_Feature_RayReconstruction)
    {
        LOG_INFO("Creating new DLSSD feature");

        Dx11Contexts[handleId] = {};

        if (!FeatureProvider_Dx11::GetFeature(Upscaler::DLSSD, handleId, InParameters, &Dx11Contexts[handleId].feature))
        {
            LOG_ERROR("Can't create DLSSD feature");
            return NVSDK_NGX_Result_Fail;
        }
    }

    State::Instance().api = DX11;
    auto deviceContext = Dx11Contexts[handleId].feature.get();
    *OutHandle = deviceContext->Handle();

    LOG_DEBUG("Using resolved native D3D11 device/context for CreateFeature");
    evalCounter = 0;

    if (!D3D11Device)
    {
        LOG_ERROR("Can't get Dx11Device from InDevCtx!");
        return NVSDK_NGX_Result_Fail;
    }

    State::Instance().autoExposure.reset();

    if (deviceContext->ModuleLoaded() && deviceContext->Init(D3D11Device, featureContext, InParameters))
    {
        State::Instance().currentFeature = deviceContext;
        if (featureDeviceRefFromGetDevice)
            D3D11Device->Release();
        return NVSDK_NGX_Result_Success;
    }

    LOG_ERROR("CreateFeature failed");

    State::Instance().newBackend = Upscaler::FSR22;
    State::Instance().changeBackend[handleId] = true;

    if (featureDeviceRefFromGetDevice)
        D3D11Device->Release();

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_ReleaseFeature(NVSDK_NGX_Handle* InHandle)
{
    if (!InHandle)
        return NVSDK_NGX_Result_Success;

    auto handleId = InHandle->Id;
    if (handleId < DLSS_MOD_ID_OFFSET)
    {
        if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::D3D11_ReleaseFeature() != nullptr)
        {
            auto result = NVNGXProxy::D3D11_ReleaseFeature()(InHandle);

            if (!shutdown)
                LOG_INFO("D3D11_ReleaseFeature result for ({0}): {1:X}", handleId, (UINT) result);

            return result;
        }
        else
        {
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;
        }
    }

    if (!shutdown)
        LOG_INFO("releasing feature with id {0}", handleId);

    if (auto deviceContext = Dx11Contexts[handleId].feature.get(); deviceContext != nullptr)
    {
        if (!shutdown)
        {
            LOG_TRACE("sleeping for 500ms before reset()!");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        if (deviceContext == State::Instance().currentFeature)
            State::Instance().currentFeature = nullptr;

        Dx11Contexts[handleId].feature.reset();
        auto it = std::find_if(Dx11Contexts.begin(), Dx11Contexts.end(),
                               [&handleId](const auto& p) { return p.first == handleId; });
        Dx11Contexts.erase(it);

        if (!shutdown && Config::Instance()->Dx11DelayedInit.value_or_default())
        {
            LOG_TRACE("sleeping for 500ms after reset()!");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_GetFeatureRequirements(
    IDXGIAdapter* Adapter, const NVSDK_NGX_FeatureDiscoveryInfo* FeatureDiscoveryInfo,
    NVSDK_NGX_FeatureRequirement* OutSupported)
{
    LOG_FUNC();

    if (FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_SuperSampling)
    {
        if (OutSupported == nullptr)
            OutSupported = new NVSDK_NGX_FeatureRequirement();

        OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
        OutSupported->MinHWArchitecture = 0;

        // Some windows 10 os version
        strcpy_s(OutSupported->MinOSVersion, "10.0.10240.16384");
        return NVSDK_NGX_Result_Success;
    }

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() == nullptr)
        NVNGXProxy::InitNVNGX();

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::D3D11_GetFeatureRequirements() != nullptr)
    {
        LOG_DEBUG("D3D11_GetFeatureRequirements for ({0})", (int) FeatureDiscoveryInfo->FeatureID);
        auto result = NVNGXProxy::D3D11_GetFeatureRequirements()(Adapter, FeatureDiscoveryInfo, OutSupported);
        LOG_DEBUG("result for D3D11_GetFeatureRequirements ({0}): {1:X}", (int) FeatureDiscoveryInfo->FeatureID,
                  (UINT) result);
        return result;
    }

    OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_AdapterUnsupported;
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext* InDevCtx,
                                                               const NVSDK_NGX_Handle* InFeatureHandle,
                                                               NVSDK_NGX_Parameter* InParameters,
                                                               PFN_NVSDK_NGX_ProgressCallback InCallback)
{
    if (InFeatureHandle == nullptr)
    {
        LOG_DEBUG("InFeatureHandle is null");
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }
    else
    {
        LOG_DEBUG("Handle: {0}", InFeatureHandle->Id);
    }

    if (InDevCtx == nullptr)
    {
        LOG_ERROR("InDevCtx is null!!!");
        return NVSDK_NGX_Result_Fail;
    }

    NativeD3D11Context nativeContext = GetNativeD3D11Context(InDevCtx);
    ID3D11DeviceContext* featureContext = nativeContext.Context() != nullptr ? nativeContext.Context() : InDevCtx;
    if (nativeContext.device != nullptr)
        D3D11Device = nativeContext.device;

    State& state = State::Instance();
    auto handleId = InFeatureHandle->Id;
    if (handleId < DLSS_MOD_ID_OFFSET)
    {
        if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::D3D11_EvaluateFeature() != nullptr)
        {
            LOG_DEBUG("D3D11_EvaluateFeature for ({0})", handleId);
            auto result =
                NVNGXProxy::D3D11_EvaluateFeature()(featureContext, InFeatureHandle, InParameters, InCallback);
            LOG_INFO("D3D11_EvaluateFeature result for ({0}): {1:X}", handleId, (UINT) result);
            return result;
        }
        else
        {
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;
        }
    }

    evalCounter++;
    if (Config::Instance()->SkipFirstFrames.has_value() && evalCounter < Config::Instance()->SkipFirstFrames.value())
        return NVSDK_NGX_Result_Success;

    if (InCallback)
        LOG_INFO("callback exist");

    IFeature_Dx11* deviceContext = nullptr;
    auto activeContext = &Dx11Contexts[handleId];

    if (state.changeBackend[handleId])
    {
        auto successfulPhase = FeatureProvider_Dx11::ChangeFeature(state.newBackend, D3D11Device, featureContext, handleId,
                                                                   InParameters, activeContext);

        evalCounter = 0;

        if (activeContext->changeBackendCounter != 0 || !successfulPhase)
        {
            return NVSDK_NGX_Result_Success;
        }
    }

    if (activeContext->feature == nullptr) // prevent source api name flicker when dlssg is active
    {
        state.setInputApiName = state.currentInputApiName;
    }
    else
    {
        deviceContext = activeContext->feature.get();
        state.currentFeature = deviceContext;
    }

    const auto targetApiName =
        !state.setInputApiName.has_value() ? ApiUpscalerInput::DLSS_DX11 : state.setInputApiName.value();

    if (state.currentInputApiName != targetApiName)
        state.currentInputApiName = targetApiName;

    state.setInputApiName.reset();

    if (deviceContext == nullptr)
    {
        LOG_DEBUG("trying to use released handle, returning NVSDK_NGX_Result_Success");
        return NVSDK_NGX_Result_Success;
    }

    auto upscaleResult = deviceContext->Evaluate(featureContext, InParameters);

    if (State::Instance().activeFgInput == FGInput::Upscaler)
    {
        if (WithDx12::IsInited())
        {
            auto cq = WithDx12::GetD3D12CommandQueue();
            auto device = WithDx12::GetD3D12Device();

            UpscalerInputsDx11wDx12::Init(D3D11Device, featureContext, device, cq);

            UpscalerInputsDx11wDx12::UpscaleStart(InParameters, deviceContext);
            UpscalerInputsDx11wDx12::UpscaleEnd(InParameters, deviceContext);
        }
    }

    auto upscaler = deviceContext->GetUpscalerType();
    if (!upscaleResult && !deviceContext->IsInited() &&
        (upscaler == Upscaler::XeSS || upscaler == Upscaler::XeSS_on12 || upscaler == Upscaler::DLSS ||
         upscaler == Upscaler::DLSS_on12 ||
         upscaler == Upscaler::FFX_on12))
    {
        ImGui::InsertNotification({ ImGuiToastType::Error, 10000, "Upscaler failed to run!" });
        state.newBackend = Upscaler::FSR22;
        state.changeBackend[handleId] = true;
    }

    return NVSDK_NGX_Result_Success;
}

#pragma endregion
