#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <memory>
#include <string>

#include "core/Types.h"
#include "renderer/RtSettings.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class RenderTarget;
class ConstantBuffer;

// DXR 1.1 inline raytracing（RayQuery）を使うスクリーン空間パスをまとめたクラス。
//   ・RT サン影（Step 2）→ R8_UNORM。既存のコンタクトシャドウ枠(t11)へそのまま差し込む
//   ・RT-AO      （Step 3）→ R8_UNORM。既存の SSAO 枠(t8)へそのまま差し込む
//   ・RT デバッグ（Step 1）→ RG16F。dx12_render_debug の rt / rtDiff モードが表示する
//
// ★3 つを 1 クラスにまとめた理由は ScreenSpaceGiPass（SSR + SSGI）と同じ。
//   入力（深度 + TLAS）・ルートシグネチャ・cbuffer・フルスクリーン三角形 VS・
//   ライフサイクル（Resize / ホットリロード）が完全に共通で、分けると同期漏れの箇所が 3 倍になる。
//
// ★compute ではなくピクセルシェーダで走らせている（計画09 は cs_6_5 と書いているが変更した）。
//   DXR 1.1 の inline raytracing は任意のシェーダステージで使えて、ps_6_5 でコンパイルが
//   通ることを実測済み（ps_6_4 は "Opcode AllocateRayQuery not valid" で落ちる）。
//   PS にすると出力が RenderTarget（RTV+SRV を持つ既存クラス）のままで済み、
//   SSAO / ContactShadowPass と 1 バイトも違わない配線で t8 / t11 へ流し込める。
//   compute にすると UAV ディスクリプタと状態遷移が増えるだけで得が無い。
//
// ★RT 影と PCSS の関係（設計）
//   | RT 影 | PCSS | 何が起きるか |
//   |---|---|---|
//   | OFF | OFF | 従来の CSM 3x3 PCF のみ（導入前とビット一致） |
//   | OFF | ON  | CSM が可変ペナンブラになる（計画03 のまま） |
//   | ON  | OFF | 静的ジオメトリの影が RT（アクネ/peter-panning/カスケード境界なし）、
//   |     |     | スキンド/半透明は CSM の 3x3 PCF。両者は担当が排他なので min() で合成される |
//   | ON  | ON  | 上に加えてスキンド側だけ PCSS の半影が付く。**併用は正しく動く**。
//   |     |     | 見た目を揃えたいときは RtSettings::shadowSunAngle を
//   |     |     | atan(ShadowPcssSettings::lightTanAngle) に合わせること |
//   排他にしていないのは、RT 影がスキンドを担当できない（TLAS に入らない）ため。
//   PCSS を殺すとキャラの影だけ硬くなって不自然になる。
class RtScreenPass
{
public:
    // 影 / AO の出力フォーマット。SSAO / コンタクトシャドウと同じ（1=遮蔽なし）。
    static constexpr DXGI_FORMAT kMaskFormat  = DXGI_FORMAT_R8_UNORM;
    // デバッグ出力。 .r = RT のヒット距離(m) / .g = ラスタの距離(m)（どちらもミスは -1）
    static constexpr DXGI_FORMAT kDebugFormat = DXGI_FORMAT_R16G16_FLOAT;

    void Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                    u32 width, u32 height, const std::wstring& shaderDir);
    // シェーダーホットリロード用（PSO のみ作り直す）
    void RecreatePipelines(GraphicsDevice& device);
    void Resize(GraphicsDevice& device, u32 width, u32 height);

    bool IsReady() const { return m_psoShadow != nullptr; }

    struct GenerateDesc
    {
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv{};       // カメラ深度（R32_FLOAT・フル RT サイズ）
        // SSAO の結果（R8_UNORM・フル解像度）。RtSettings::aoCombineWithSsao 用。
        // ★常に有効なハンドルを渡すこと（型不一致でデバッグレイヤが落ちるため）。
        D3D12_GPU_DESCRIPTOR_HANDLE ssaoSrv{};
        // ssaoSrv が「本物の SSAO 結果」かどうか。false（＝白 1x1 ダミー）のときは
        // シェーダ側の min 合成を必ず切ること。1x1 テクスチャの範囲外 Load は 0 を返すので、
        // そのまま min すると画面が真っ黒になる（このリポジトリで何度も踏んでいる罠）。
        bool ssaoValid = false;
        D3D12_GPU_VIRTUAL_ADDRESS   tlas = 0;         // RaytracingScene::GetTlasAddress()
        DirectX::XMMATRIX view{};
        DirectX::XMMATRIX proj{};                     // ★ジッタなし
        DirectX::XMFLOAT3 cameraPos{0, 0, 0};
        DirectX::XMFLOAT3 lightDir{0, -1, 0};         // 太陽の進行方向（PerFrame と同じ）
        float zNear = 0.1f, zFar = 1000.0f;
        u32   vpLeft = 0, vpTop = 0, vpW = 0, vpH = 0;
        u32   frameIndex  = 0;
        float frameJitter = 0.0f;                     // TAA 有効時のみ非 0
        float debugRange  = 100.0f;
        // G-Buffer（xy=oct ワールド法線）。AO のレイ方向とデノイザの bilateral 重みに使う。
        // ★ssaoSrv と同じく、常に有効なハンドルを渡すこと（型不一致でデバッグレイヤが落ちる）。
        D3D12_GPU_DESCRIPTOR_HANDLE gbufferSrv{};
        // gbufferSrv が本物か。false なら AO は ddx/ddy 法線へフォールバックし、
        // デノイザの空間フィルタは丸ごとスキップする（重みが作れないため）。
        bool  gbufferValid = false;
        // このフレームの通し番号。★TAA の有無に関係なく必ず回すこと。
        // 止まっているとレイ方向が毎フレーム同じになり、デノイザが原理的に収束しない。
        u32   denoiseFrame = 0;
    };

    // 生成して SRV index を返す。未準備なら DescriptorHeap::kInvalidIndex。
    // 呼び出し側は深度を PIXEL_SHADER_RESOURCE へ遷移しておくこと。
    // 戻り時、出力 RT は PIXEL_SHADER_RESOURCE。RT/ビューポート/RootSig は呼び出し側で再設定すること。
    u32 GenerateShadow(ID3D12GraphicsCommandList* cmd, const GenerateDesc& d, const RtSettings& s);
    u32 GenerateAo    (ID3D12GraphicsCommandList* cmd, const GenerateDesc& d, const RtSettings& s);
    u32 GenerateDebug (ID3D12GraphicsCommandList* cmd, const GenerateDesc& d);

private:
    // ★PassDebug までの番号は変えないこと（Run 内のフォーマット判定が依存している）。
    enum PassIndex : u32 { PassShadow = 0, PassAo = 1, PassDebug = 2,
                           PassAoDenoise = 3, PassCount = 4 };

    void CreateRootSignature(GraphicsDevice& device);
    // 3 パス共通の実行本体（CB を書いて全画面三角形を 1 枚描く）。
    u32  Run(ID3D12GraphicsCommandList* cmd, const GenerateDesc& d, const RtSettings& s,
             PassIndex pass);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoShadow;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoAo;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoAoDenoise;

    std::unique_ptr<RenderTarget>   m_rt[PassCount];
    std::unique_ptr<ConstantBuffer> m_paramCB;   // kFrameCount × PassCount 個
    // RenderTarget の内部状態を使わず自前で追跡する（native cmd で遷移するため。
    // ContactShadowPass / SSAOPass と同じ理由）。
    D3D12_RESOURCE_STATES m_rtState[PassCount] = {};

    // デノイズパスが読む「生 AO」の GPU ハンドル。GenerateAo がトレース直後に詰める。
    D3D12_GPU_DESCRIPTOR_HANDLE m_rawAoSrv{};
    DescriptorHeap* m_srvHeap = nullptr;   // 生 AO の SRV index → GPU ハンドル変換用

    u32 m_width  = 0;
    u32 m_height = 0;
    std::wstring m_shaderDir;
};

} // namespace dx12e
