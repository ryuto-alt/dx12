#pragma once

// フレーム描画リストの 1 要素。Application::BuildDrawList() が毎フレーム 1 回だけ構築し、
// メイン / 深度プリパス / CSM 各カスケード / スポット影 / ポイント影 の全パスで共有する。
//
// エディタの精密ピッキング（editor/ScenePick）もこれをブロードフェーズの候補列として
// 読む＝ワールド行列（親階層合成済み）とバウンディング球が計算済みなので、クリック 1 回で
// 10 万体ぶんの ComputeWorldMatrix を回し直す必要がなくなる。
// Application の入れ子 struct のままだとエディタ側から型が見えないため独立ヘッダに置く。

#include <entt/entt.hpp>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

struct MeshRenderer;
class SkinningBuffer;

struct DrawItem
{
    entt::entity        e;
    const MeshRenderer* renderer;
    DirectX::XMFLOAT4X4 world;        // 親階層合成済みワールド行列
    // 前フレームのワールド行列（速度バッファ生成用）。TAA 無効時は world と同値のまま
    // （＝速度 0）。新規スポーンしたエンティティも world と同値にして初回のゴーストを防ぐ。
    DirectX::XMFLOAT4X4 prevWorld;
    SkinningBuffer*     skin;         // スキンドなら該当バッファ / 静的は nullptr
    // 保守的バウンディング球。中心は「メッシュAABBの中心をワールドへ移した点」で、
    // エンティティ原点ではない（原点からジオメトリがズレたモデルの誤カリング防止）。
    DirectX::XMFLOAT3   center;
    f32                 radius;       // 半径（ワールドスケール込み）
    u32                 lod;          // メインカメラ基準の選択LOD（Mesh 側でクランプされる）
    bool                hasNodeAnim;
    u32                 sortKey;      // 0=既定static / 1=カスタム不透明 / 2=skinned / 3=カスタム半透明(最後)
    // 自動インスタンシングのバッチ鍵。0 = インスタンシング不可（従来の per-object 描画）。
    // 同一キー同士は「同じメッシュ・同じLOD・同じマテリアル/PBR値」＝1ドローに畳んで良い。
    u64                 batchKey;
};

} // namespace dx12e
