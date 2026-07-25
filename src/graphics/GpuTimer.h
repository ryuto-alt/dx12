#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>

#include "core/Types.h"

namespace dx12e
{

// GPU タイムスタンプ計測（パフォーマンス診断用）。
// フレームを固定スコープ（影/プリパス/メイン/…）で挟んでタイムスタンプを打ち、
// フレーム末に readback バッファへ Resolve、kFrames 周回後（フェンス完了保証済み）に読む。
// 結果は「約 kFrames フレーム前」の値だが、診断用途にはそれで十分。
// ponytail: スコープは固定 enum。動的な階層プロファイラは要るまで作らない。
class GpuTimer
{
public:
    enum Scope : u32
    {
        Total = 0,     // フレーム全体（cmdList 記録範囲）
        Shadows,       // スポット影 + ポイント影 + CSM
        PrepassSSAO,   // 深度プリパス + SSAO 生成
        ClusterCull,   // クラスタ AABB 構築 + ライトカリング（compute 2 パス）
        MainScene,     // 不透明メッシュ（RenderSceneMeshes メインビュー）
        Particles,     // CPU/GPU パーティクル + 歪み
        PostFX,        // ブルーム/DoF/自動露出/uber 等ポスト一式
        UI,            // エディタアイコン/スプライト/ImGui/トランジション
        Count
    };
    static const char* Name(u32 s);

    static constexpr u32 kFrames = 3;   // FrameResources::kFrameCount と一致させる

    // queue はタイムスタンプ周波数の取得用。失敗時は m_valid=false のまま（全 API が no-op）。
    void Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);

    // フレーム先頭（フェンス待ち後）に呼ぶ。同スロットの前回結果を読み戻し、書込みマスクをリセット。
    void NewFrame(u32 frameIndex);
    void Begin(ID3D12GraphicsCommandList* cmd, Scope s);
    void End(ID3D12GraphicsCommandList* cmd, Scope s);
    // フレーム末（cmdList Close 前）に呼ぶ。今フレームで Begin/End したスコープだけ Resolve する。
    void Resolve(ID3D12GraphicsCommandList* cmd);

    bool  IsValid() const { return m_valid; }
    // 直近の完了フレームの計測値(ms)。未計測スコープは 0。
    float GetMs(Scope s) const { return (s < Count) ? m_ms[s] : 0.0f; }

private:
    u32 QueryIndex(u32 frame, u32 scope, u32 beginEnd) const
    {
        return (frame * Count + scope) * 2 + beginEnd;
    }

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_heap;
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_readback;   // u64 × kFrames×Count×2
    u64   m_freq = 0;
    bool  m_valid = false;
    u32   m_frame = 0;                  // NewFrame で受けた現在スロット
    u32   m_writtenMask[kFrames] = {};  // スロットごとの「このフレームで End まで打った」スコープビット
    float m_ms[Count] = {};
};

} // namespace dx12e
