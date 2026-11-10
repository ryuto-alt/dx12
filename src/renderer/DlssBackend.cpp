// NVIDIA DLSS（NGX D3D12）。SDK の取得は tools/fetch_dlss.ps1、ライセンスは third_party/dlss/LICENSE.txt。
// ★このファイルは PCH を使わない（CMake で UNO_ENABLE_DLSS をこの TU にだけ渡すため）。
#include "renderer/DlssBackend.h"
#include "core/Logger.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <iterator>
#include <string>
#include <thread>
#include <wrl/client.h>

#if defined(UNO_ENABLE_DLSS)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#endif

namespace dx12e
{

#if defined(UNO_ENABLE_DLSS)

namespace
{
// NGX のプロジェクト ID（NVIDIA から ID を貰っていないエンジンは任意の GUID + ENGINE_TYPE_CUSTOM で名乗る）。
// 利用者・プロジェクトを特定する情報は含めない（固定値）。
constexpr const char* kNgxProjectId    = "6b0f3c2a-9d41-4e57-8a1c-3f5e2b7d9c40";
constexpr const char* kNgxEngineVersion = "0.1";

std::string NarrowResult(NVSDK_NGX_Result r)
{
    const wchar_t* w = GetNGXResultAsString(r);
    std::string s;
    if (w) for (const wchar_t* p = w; *p; ++p) s.push_back((*p < 128) ? static_cast<char>(*p) : '?');
    char hex[16];
    std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned>(r));
    return s.empty() ? std::string(hex) : (s + " (" + hex + ")");
}

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level /*level*/, NVSDK_NGX_Feature /*src*/)
{
    if (!message) return;
    std::string m(message);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    // NGX は大量に喋る（DLL の探索で「exe の隣に _nvngx.dll が無い」を error と書くなど、正常系でも error が出る）。
    // 全部 Debug に落とす。失敗はこちらの戻り値で判定し、理由を Reason() / Warn に 1 行で出す。
    Logger::Debug("[NGX] {}", m);
}

NVSDK_NGX_PerfQuality_Value ToNgx(upscale::DlssQuality q)
{
    switch (q)
    {
    case upscale::DlssQuality::Dlaa:        return NVSDK_NGX_PerfQuality_Value_DLAA;
    case upscale::DlssQuality::Quality:     return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case upscale::DlssQuality::Balanced:    return NVSDK_NGX_PerfQuality_Value_Balanced;
    case upscale::DlssQuality::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    }
    return NVSDK_NGX_PerfQuality_Value_MaxQuality;
}

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return L".";
    return std::filesystem::path(std::wstring(buf, n)).parent_path().wstring();
}
} // namespace

struct DlssBackend::Impl
{
    ID3D12Device*        device  = nullptr;
    bool                 ngxInit = false;
    NVSDK_NGX_Parameter* params  = nullptr;   // GetCapabilityParameters（作成 / 評価にも使い回す）
    NVSDK_NGX_Handle*    feature = nullptr;
    u32 fRenderW = 0, fRenderH = 0, fOutW = 0, fOutH = 0;
    upscale::DlssQuality fQuality = upscale::DlssQuality::Quality;
    std::wstring exeDir;

    // ---- 非同期作成（PrepareFeatureAsync）。build: 0 = なし / 1 = 作成中 / 2 = 成功（回収待ち）/ 3 = 失敗（回収待ち）----
    std::thread          worker;
    std::atomic<int>     build{0};
    NVSDK_NGX_Handle*    newFeature = nullptr;   // ワーカーが書き、build が 2 になってからメインが読む
    u64                  newVram    = 0;
    double               newMs      = 0.0;
    std::string          buildErr;
    u32 bRenderW = 0, bRenderH = 0, bOutW = 0, bOutH = 0;
    upscale::DlssQuality bQuality = upscale::DlssQuality::Quality;
    void JoinWorker() { if (worker.joinable()) worker.join(); }
};

DlssBackend::DlssBackend() : m_impl(std::make_unique<Impl>()) {}
DlssBackend::~DlssBackend() { Shutdown(); }

bool DlssBackend::CompiledIn() { return true; }

bool DlssBackend::Initialize(ID3D12Device* device, const std::wstring& dataDir)
{
    m_available = false;
    m_reason.clear();
    if (!device) { m_reason = "D3D12 デバイスが無い"; return false; }
    Impl& m = *m_impl;
    m.device = device;
    m.exeDir = ExeDir();

    std::error_code ec;
    std::filesystem::create_directories(dataDir, ec);

    const wchar_t* paths[] = { m.exeDir.c_str() };
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path   = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback          = &NgxLog;
    info.LoggingInfo.MinimumLoggingLevel      = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;   // NGX 既定のファイル / デバッガ出力を止め、エンジンのログへ寄せる

    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(
        kNgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kNgxEngineVersion,
        dataDir.c_str(), device, &info, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r))
    {
        // 非 NVIDIA GPU / ドライバが NGX を持たない場合はここで落ちる（FeatureNotSupported / PlatformError 等）
        m_reason = "NGX の初期化に失敗: " + NarrowResult(r) + "（NVIDIA 以外の GPU か、ドライバが古い）";
        return false;
    }
    m.ngxInit = true;

    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&m.params);
    if (NVSDK_NGX_FAILED(r) || !m.params)
    {
        m_reason = "NGX の能力パラメータが取れない: " + NarrowResult(r);
        Shutdown();
        return false;
    }

    int needsDriver = 0;
    unsigned minMajor = 0, minMinor = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(m.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver))
        && needsDriver)
    {
        NVSDK_NGX_Parameter_GetUI(m.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minMajor);
        NVSDK_NGX_Parameter_GetUI(m.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minMinor);
        m_reason = "ドライバが古い（DLSS には " + std::to_string(minMajor) + "." + std::to_string(minMinor) + " 以上が要る）";
        Shutdown();
        return false;
    }
    int avail = 0;
    NVSDK_NGX_Parameter_GetI(m.params, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    if (!avail)
    {
        int initRes = 0;
        NVSDK_NGX_Parameter_GetI(m.params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initRes);
        m_reason = "この GPU / ドライバでは DLSS SR が使えない（FeatureInitResult=" + NarrowResult(static_cast<NVSDK_NGX_Result>(initRes)) + "）";
        Shutdown();
        return false;
    }

    m_available = true;
    m_version   = "NGX " + std::to_string(NVSDK_NGX_Version_API);
    Logger::Info("DLSS: 利用可能（NGX 初期化 OK、データ {}）", std::filesystem::path(dataDir).string());
    return true;
}

void DlssBackend::Shutdown()
{
    if (!m_impl) return;
    Impl& m = *m_impl;
    ReleaseFeature();
    if (m.params) { NVSDK_NGX_D3D12_DestroyParameters(m.params); m.params = nullptr; }
    if (m.ngxInit && m.device) { NVSDK_NGX_D3D12_Shutdown1(m.device); }
    m.ngxInit = false;
    m_available = false;
}

bool DlssBackend::RenderSize(u32 outW, u32 outH, upscale::DlssQuality q, u32& rw, u32& rh)
{
    upscale::NominalRenderSize(outW, outH, q, rw, rh);
    if (!m_available || !m_impl->params) return false;
    if (q == upscale::DlssQuality::Dlaa) { rw = outW; rh = outH; return true; }
    unsigned ow = 0, oh = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
    float sharp = 0.0f;
    const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(m_impl->params, outW, outH, ToNgx(q),
                                                            &ow, &oh, &maxW, &maxH, &minW, &minH, &sharp);
    if (NVSDK_NGX_FAILED(r) || ow == 0 || oh == 0) return false;
    rw = ow; rh = oh;
    return true;
}

bool DlssBackend::FeatureMismatch(u32 renderW, u32 renderH, u32 outW, u32 outH, upscale::DlssQuality q) const
{
    const Impl& m = *m_impl;
    if (!m.feature) return false;
    return m.fRenderW != renderW || m.fRenderH != renderH || m.fOutW != outW || m.fOutH != outH || m.fQuality != q;
}

void DlssBackend::ReleaseFeature()
{
    if (!m_impl) return;
    Impl& m = *m_impl;
    // 作成中のワーカーは待ってから捨てる（出来た機能が宙に浮かないように）
    m.JoinWorker();
    if (m.newFeature) { NVSDK_NGX_D3D12_ReleaseFeature(m.newFeature); m.newFeature = nullptr; }
    m.build.store(0);
    if (m.feature) { NVSDK_NGX_D3D12_ReleaseFeature(m.feature); m.feature = nullptr; }
    m.fRenderW = m.fRenderH = m.fOutW = m.fOutH = 0;
    m_vramBytes = 0;
}

bool DlssBackend::FeatureBuilding() const
{
    return m_impl && m_impl->build.load() == 1;
}

bool DlssBackend::PrepareFeatureAsync(ID3D12CommandQueue* queue, u32 renderW, u32 renderH, u32 outW, u32 outH,
                                      upscale::DlssQuality q)
{
    if (!m_available || !queue || !m_impl->device) return false;
    Impl& m = *m_impl;

    // 出来上がり（または失敗）の回収
    const int st = m.build.load();
    if (st == 2 || st == 3)
    {
        m.JoinWorker();
        if (st == 2)
        {
            m.feature = m.newFeature; m.newFeature = nullptr;
            m.fRenderW = m.bRenderW; m.fRenderH = m.bRenderH; m.fOutW = m.bOutW; m.fOutH = m.bOutH; m.fQuality = m.bQuality;
            m_vramBytes = m.newVram;
            {   // 作成に使った一時パラメータの統計は 0 になるので、能力パラメータ側から読み直す（ログ / get_upscaler 用）
                unsigned long long vram = 0;
                if (m.params && NVSDK_NGX_SUCCEED(NGX_DLSS_GET_STATS(m.params, &vram)) && vram) m_vramBytes = vram;
            }
            Logger::Info("DLSS: 機能を作成（別スレッド・{:.0f} ms）{}x{} → {}x{}（{}、VRAM {:.1f} MB）", m.newMs,
                         m.fRenderW, m.fRenderH, m.fOutW, m.fOutH, upscale::QualityName(m.fQuality),
                         static_cast<double>(m_vramBytes) / (1024.0 * 1024.0));
        }
        else
        {
            ++m_evalErrors;
            m_available = false;   // 以後は TAA へ縮退（理由は Reason()）
            m_reason = "DLSS 機能の作成に失敗: " + m.buildErr;
            Logger::Warn("DLSS: {}（{}x{} → {}x{}, {}）", m_reason, m.bRenderW, m.bRenderH, m.bOutW, m.bOutH,
                         upscale::QualityName(m.bQuality));
        }
        m.build.store(0);
    }
    if (!m_available) return false;
    if (m.feature) return !FeatureMismatch(renderW, renderH, outW, outH, q);
    if (m.build.load() == 1) return false;

    m.bRenderW = renderW; m.bRenderH = renderH; m.bOutW = outW; m.bOutH = outH; m.bQuality = q;
    m.build.store(1);
    Logger::Info("DLSS: 機能の作成を別スレッドで開始（{}x{} → {}x{}、{}）。出来るまで TAA で描く", renderW, renderH, outW, outH,
                 upscale::QualityName(q));
    m.worker = std::thread([&m, queue, renderW, renderH, outW, outH, q]()
    {
        const auto t0 = std::chrono::steady_clock::now();
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    alloc;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        Microsoft::WRL::ComPtr<ID3D12Fence>               fence;
        NVSDK_NGX_Parameter* p = nullptr;
        NVSDK_NGX_Handle*    h = nullptr;
        bool ok = false;
        std::string err;
        do
        {
            if (FAILED(m.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) { err = "コマンドアロケータを作れない"; break; }
            if (FAILED(m.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) { err = "コマンドリストを作れない"; break; }
            if (FAILED(m.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) { err = "フェンスを作れない"; break; }
            NVSDK_NGX_Result r = NVSDK_NGX_D3D12_AllocateParameters(&p);
            if (NVSDK_NGX_FAILED(r) || !p) { err = "NGX パラメータを確保できない: " + NarrowResult(r); break; }
            NVSDK_NGX_DLSS_Create_Params cp{};
            cp.Feature.InWidth            = renderW;
            cp.Feature.InHeight           = renderH;
            cp.Feature.InTargetWidth      = outW;
            cp.Feature.InTargetHeight     = outH;
            cp.Feature.InPerfQualityValue = ToNgx(q);
            cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR
                                    | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
                                    | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
            cp.InEnableOutputSubrects = false;
            r = NGX_D3D12_CREATE_DLSS_EXT(list.Get(), 1, 1, &h, p, &cp);
            if (NVSDK_NGX_FAILED(r) || !h) { h = nullptr; err = NarrowResult(r); break; }
            unsigned long long vram = 0;
            if (NVSDK_NGX_SUCCEED(NGX_DLSS_GET_STATS(p, &vram))) m.newVram = vram;
            if (FAILED(list->Close())) { err = "コマンドリストを閉じられない"; break; }
            ID3D12CommandList* lists[] = { list.Get() };
            queue->ExecuteCommandLists(1, lists);
            // 評価は同じキューの後続コマンド＝順序は保証されるが、作成が GPU で終わるまで「出来た」とは言わない
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!ev || FAILED(queue->Signal(fence.Get(), 1)) || FAILED(fence->SetEventOnCompletion(1, ev)))
            {
                if (ev) CloseHandle(ev);
                err = "フェンス待ちに失敗";
                break;
            }
            WaitForSingleObject(ev, INFINITE);
            CloseHandle(ev);
            ok = true;
        } while (false);
        if (p) NVSDK_NGX_D3D12_DestroyParameters(p);
        if (!ok && h) { NVSDK_NGX_D3D12_ReleaseFeature(h); h = nullptr; }
        m.newFeature = ok ? h : nullptr;
        m.buildErr   = err;
        m.newMs      = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        m.build.store(ok ? 2 : 3);
    });
    return false;
}

bool DlssBackend::Evaluate(ID3D12GraphicsCommandList* cmd, const Eval& e)
{
    if (!m_available || !cmd || !e.color || !e.depth || !e.motion || !e.output) return false;
    Impl& m = *m_impl;

    if (m.feature && FeatureMismatch(e.renderW, e.renderH, e.outW, e.outH, e.quality))
        return false;   // 呼び出し側が GPU アイドルで ReleaseFeature() してから作り直す

    if (!m.feature)
    {
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth            = e.renderW;
        cp.Feature.InHeight           = e.renderH;
        cp.Feature.InTargetWidth      = e.outW;
        cp.Feature.InTargetHeight     = e.outH;
        cp.Feature.InPerfQualityValue = ToNgx(e.quality);
        // HDR（トーンマップ前のリニア色）/ MV は内部解像度 / MV にジッタは入っていない / 深度は標準 Z（反転しない）。
        // 露出は DLSS の自動露出に任せる（理由は UpscalerPass.h の冒頭）。
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR
                                | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
                                | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        cp.InEnableOutputSubrects = false;
        const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(cmd, 1, 1, &m.feature, m.params, &cp);
        if (NVSDK_NGX_FAILED(r) || !m.feature)
        {
            m.feature = nullptr;
            ++m_evalErrors;
            Logger::Warn("DLSS: 機能の作成に失敗 {}（{}x{} → {}x{}, {}）", NarrowResult(r), e.renderW, e.renderH,
                         e.outW, e.outH, upscale::QualityName(e.quality));
            return false;
        }
        m.fRenderW = e.renderW; m.fRenderH = e.renderH; m.fOutW = e.outW; m.fOutH = e.outH; m.fQuality = e.quality;
        unsigned long long vram = 0;
        if (NVSDK_NGX_SUCCEED(NGX_DLSS_GET_STATS(m.params, &vram))) m_vramBytes = vram;
        Logger::Info("DLSS: 機能を作成 {}x{} → {}x{}（{}、VRAM {:.1f} MB）", e.renderW, e.renderH, e.outW, e.outH,
                     upscale::QualityName(e.quality), static_cast<double>(m_vramBytes) / (1024.0 * 1024.0));
    }

    NVSDK_NGX_D3D12_DLSS_Eval_Params ep{};
    ep.Feature.pInColor  = e.color;
    ep.Feature.pInOutput = e.output;
    ep.pInDepth          = e.depth;
    ep.pInMotionVectors  = e.motion;
    ep.InJitterOffsetX   = e.jitterX;
    ep.InJitterOffsetY   = e.jitterY;
    ep.InRenderSubrectDimensions.Width  = e.renderW;
    ep.InRenderSubrectDimensions.Height = e.renderH;
    ep.InReset           = e.reset ? 1 : 0;
    ep.InMVScaleX        = e.mvScaleX;
    ep.InMVScaleY        = e.mvScaleY;
    ep.InFrameTimeDeltaInMsec = e.frameMs;
    const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(cmd, m.feature, m.params, &ep);
    if (NVSDK_NGX_FAILED(r))
    {
        if (m_evalErrors++ < 5) Logger::Warn("DLSS: 評価に失敗 {}", NarrowResult(r));
        return false;
    }
    return true;
}

#else  // UNO_ENABLE_DLSS が無いビルド: 常に「使えない」

struct DlssBackend::Impl {};
DlssBackend::DlssBackend() : m_impl(std::make_unique<Impl>()) {}
DlssBackend::~DlssBackend() = default;
bool DlssBackend::CompiledIn() { return false; }
bool DlssBackend::Initialize(ID3D12Device*, const std::wstring&)
{
    m_available = false;
    m_reason = "DLSS SDK 無しでビルドされている（pwsh tools/fetch_dlss.ps1 → cmake を構成し直す）";
    return false;
}
void DlssBackend::Shutdown() {}
bool DlssBackend::RenderSize(u32 outW, u32 outH, upscale::DlssQuality q, u32& rw, u32& rh)
{
    upscale::NominalRenderSize(outW, outH, q, rw, rh);
    return false;
}
bool DlssBackend::Evaluate(ID3D12GraphicsCommandList*, const Eval&) { return false; }
bool DlssBackend::PrepareFeatureAsync(ID3D12CommandQueue*, u32, u32, u32, u32, upscale::DlssQuality) { return false; }
bool DlssBackend::FeatureBuilding() const { return false; }
bool DlssBackend::FeatureMismatch(u32, u32, u32, u32, upscale::DlssQuality) const { return false; }
void DlssBackend::ReleaseFeature() {}

#endif

} // namespace dx12e
