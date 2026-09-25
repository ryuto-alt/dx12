#pragma once
// ===========================================================================
// 知覚層（dx12_perceive）の GPU 側。ID パス 1 本 + 被覆マスク + 読み戻し。
// ---------------------------------------------------------------------------
// ★要求があったフレームだけ使う。要求が無いフレームではこのクラスは 1 命令も記録しない
//   （リソースも最初の要求まで作らない）＝最終画は 1 ビットも変わらない
//   （tools/bench/golden.mjs で確かめている）。
// ★メインのルートシグネチャ（62/64 DWORD で満杯）には触らない。専用のルートシグネチャ:
//     [0] b0 32bit 定数 20 … transpose(world) 16 + entityId / flags / alphaCutoff / pad
//     [1] b1 32bit 定数 20 … transpose(viewProj) 16 + cameraPos 3 + pad
//     [2] b2 32bit 定数 4  … uvScaleOffset（MASK のときだけ意味がある）
//     [3] テーブル t0      … albedo（MASK のアルファクリップ用）
//     [4] テーブル t3      … ボーン行列（スキンド）
//     s0 = LINEAR WRAP の静的サンプラ
//   合計 46 DWORD。テーブルはメインと同じシェーダ可視 SRV ヒープを指す（呼び出し側が張る）。
// ★出力（shaders/perception/PerceptionId.hlsl）:
//     ID (R32_UINT) / ワールド座標+距離 (R32G32B32A32_FLOAT) / 法線 (R8G8B8A8_SNORM) + 深度 D32
//     被覆マスク (R8_UINT)… 対象 1 件ずつ、深度なしで描いて読み戻す（遮蔽率の分母）
// 描画ループそのもの（どのメッシュをどう描くか）は Application::RecordPerceptionIds
// （ApplicationRender.cpp）にある。深度プリパスと同じ m_drawItems を同じ LOD で回すため。
// ===========================================================================

#include "core/Types.h"

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include <string>
#include <vector>

namespace dx12e
{
class GraphicsDevice;

class PerceptionPass
{
public:
    // シェーダ（.cso）を読んでルートシグネチャと PSO を作る。失敗は例外。
    void Initialize(GraphicsDevice& device, const std::wstring& shaderDir);

    // 解析解像度 w x h と被覆マスクの枚数に合わせて RT / 読み戻しバッファを用意する（同じなら何もしない）。
    void EnsureTargets(GraphicsDevice& device, u32 w, u32 h, u32 isolationSlots);
    u32  Width()  const { return m_w; }
    u32  Height() const { return m_h; }

    // ---- 記録（すべて同じコマンドリストへ。順に呼ぶ）----
    // 最終画（バックバッファの矩形）を読み戻しバッファへ。バックバッファは RENDER_TARGET 状態の契約。
    bool CaptureColor(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backBuffer,
                      u32 x, u32 y, u32 w, u32 h, std::string& err);
    // ID パスを始める（クリア・RT/DSV・ビューポート・ルートシグネチャ・b1・t0 の既定値）。
    // srvHeap はメインのシェーダ可視ヒープ。defaultAlbedo は MASK でない描画でも t0 に張っておく白。
    void BeginIds(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* srvHeap,
                  const DirectX::XMMATRIX& viewProj, const DirectX::XMFLOAT3& cameraPos,
                  D3D12_GPU_DESCRIPTOR_HANDLE defaultAlbedo);
    // 被覆マスクの 1 件を始める（slot 番目の読み戻し先へ後で写す）。
    void BeginIsolation(ID3D12GraphicsCommandList* cmd, u32 slot);
    void EndIsolation(ID3D12GraphicsCommandList* cmd, u32 slot);
    // ID パスの結果を読み戻しバッファへ写す。
    void EndIds(ID3D12GraphicsCommandList* cmd);

    // 1 ドローぶんの設定。skinned で PSO を切り替える（直前と同じなら張り直さない）。
    void SetDraw(ID3D12GraphicsCommandList* cmd, bool skinned, const DirectX::XMMATRIX& world,
                 u32 entityId, bool mask, float alphaCutoff, const float uvScaleOffset[4],
                 D3D12_GPU_DESCRIPTOR_HANDLE albedo, D3D12_GPU_DESCRIPTOR_HANDLE bones);

    // ---- 読み戻し（GPU 完了後に呼ぶ）----
    struct CpuFrame
    {
        u32 w = 0, h = 0;
        std::vector<u32>   ids;
        std::vector<u8>    rgba;       // 解析解像度へ最近傍で合わせた最終画（RGBA8）
        std::vector<float> posDist;    // w*h*4
        std::vector<float> normal;     // w*h*4
        std::vector<std::vector<u8>> isolation;   // slot ごとの被覆マスク（w*h）
        u32 colorW = 0, colorH = 0;    // 最終画の元の大きさ
        std::vector<u8> colorRgba;     // 元の大きさの最終画（PNG 保存用）
    };
    bool Readback(CpuFrame& out, u32 isolationUsed, std::string& err);

private:
    struct ReadbackBuf
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buf;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT     fp{};
        u64                                    bytes = 0;
    };
    static void CreateReadback(ID3D12Device* dev, const D3D12_RESOURCE_DESC& texDesc, u32 count,
                               ReadbackBuf& out, u64& strideOut);
    static void CopyToReadback(ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex,
                               const ReadbackBuf& rb, u64 offset);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoStatic, m_psoSkinned;          // ID パス（3 MRT + 深度）
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoIsoStatic, m_psoIsoSkinned;    // 被覆マスク（R8_UINT のみ）

    u32 m_w = 0, m_h = 0, m_isoSlots = 0;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap, m_dsvHeap;
    u32 m_rtvStride = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_idTex, m_posTex, m_nrmTex, m_depthTex, m_isoTex;
    ReadbackBuf m_rbId, m_rbPos, m_rbNrm, m_rbIso, m_rbColor;
    u64      m_isoStride = 0;
    u32      m_colorW = 0, m_colorH = 0, m_colorFormat = 0;
    bool     m_colorValid = false;

    ID3D12PipelineState* m_lastPso = nullptr;
    bool                 m_isolation = false;   // 今どちらのパスを記録しているか
};

} // namespace dx12e
