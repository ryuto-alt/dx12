#pragma once

#include <directx/d3d12.h>
#include <memory>
#include <string>

#include "core/Types.h"
#include "renderer/UpscalerMath.h"

namespace dx12e
{

// NVIDIA DLSS Super Resolution / DLAA（NGX の D3D12 API）の薄いラッパ。
//   - SDK はリポジトリに入っていない（tools/fetch_dlss.ps1）。UNO_ENABLE_DLSS が無いビルドでは
//     CompiledIn() = false で、Initialize は「SDK 無しのビルド」を理由に失敗するだけ。
//   - NGX のヘッダは DlssBackend.cpp だけが読む（他の TU に NVIDIA のヘッダを漏らさない）。
//   - 失敗しても例外は投げない。理由を 1 行で Reason() に残す（呼び出し側が TAA へ縮退してログに出す）。
class DlssBackend
{
public:
    DlssBackend();
    ~DlssBackend();
    DlssBackend(const DlssBackend&) = delete;
    DlssBackend& operator=(const DlssBackend&) = delete;

    static bool CompiledIn();

    // NGX を初期化して DLSS SR が使えるかを調べる。dataDir は NGX のログ / キャッシュの置き場。
    bool Initialize(ID3D12Device* device, const std::wstring& dataDir);
    void Shutdown();   // GPU アイドルの状態で呼ぶこと

    bool Available() const { return m_available; }
    const std::string& Reason() const { return m_reason; }
    const std::string& Version() const { return m_version; }   // 例 "nvngx_dlss 310.x"（取れなければ空）

    // NGX が推す内部解像度（品質モードごと）。取れなければ公称倍率で計算して false。
    bool RenderSize(u32 outW, u32 outH, upscale::DlssQuality q, u32& rw, u32& rh);

    struct Eval
    {
        ID3D12Resource* color  = nullptr;   // 内部解像度・リニア HDR（トーンマップ前）。NON_PIXEL_SHADER_RESOURCE を含む状態
        ID3D12Resource* depth  = nullptr;   // 内部解像度・D32（R32_TYPELESS）。同上
        ID3D12Resource* motion = nullptr;   // 内部解像度・RG16F（現UV - 前UV、ジッタなし）。同上
        ID3D12Resource* output = nullptr;   // 表示解像度・RGBA16F・UAV 状態
        u32   renderW = 0, renderH = 0, outW = 0, outH = 0;
        float jitterX = 0.0f, jitterY = 0.0f;   // 内部解像度のピクセル
        float mvScaleX = 1.0f, mvScaleY = 1.0f;
        bool  reset   = false;
        float frameMs = 16.6f;
        upscale::DlssQuality quality = upscale::DlssQuality::Quality;
    };
    // 機能（フィーチャ）が今の寸法 / 品質で無ければ作ってから評価する。寸法が変わったときの
    // 古い機能の解放は呼び出し側が GPU アイドルを保証してから ReleaseFeature() で行うこと。
    // ★戻った後はコマンドリストのディスクリプタヒープ / ルートシグネチャ / PSO が NGX のものに
    //   置き換わっている。呼び出し側で張り直すこと。
    bool Evaluate(ID3D12GraphicsCommandList* cmd, const Eval& e);
    // 今の機能が (render, out, quality) と食い違うか（＝作り直しが要るか）
    bool FeatureMismatch(u32 renderW, u32 renderH, u32 outW, u32 outH, upscale::DlssQuality q) const;
    void ReleaseFeature();
    u64  FeatureVramBytes() const { return m_vramBytes; }
    u32  EvalErrors() const { return m_evalErrors; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool        m_available = false;
    std::string m_reason;
    std::string m_version;
    u64         m_vramBytes = 0;
    u32         m_evalErrors = 0;
};

} // namespace dx12e
