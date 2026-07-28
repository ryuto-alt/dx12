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

// この DrawItem が DXR の TLAS に入るか（＝RT 影 / RT-AO が担当する範囲か）。
//
// ★CSM 側で「RT が担当するぶんを除外する」ときも必ずこの関数を使うこと。
//   判定が 2 箇所に分かれると、影が二重に出るか、どこにも出なくなる。
//   RT 影が有効なとき、CSM と RT は**担当が排他**になり、フォワード PS の
//     shadow = min(csmShadow, contactShadowTex)
//   が過不足なく合成する。CSM を全部描いたままにすると min() は暗い方を採るので
//   CSM のアクネ（本来明るいのに縞状に暗い）とカスケード境界の段差が残ってしまう。
//
//  - skin != nullptr … スキンド。変形後の頂点が GPU に要るので compute スキニング
//                      （計画09 Step 4 / SkinningCompute）が動いているフレームだけ TLAS に入る。
//                      それを表すのが引数 skinnedInTlas。**必ず両方の呼び出し側へ同じ値を渡すこと**
//                      （CSM 側だけ true にすると影が消え、TLAS 側だけ true にすると二重に出る）。
//  - sortKey == 3    … 半透明。TLAS に入れると any-hit が必要になり 2〜10 倍遅くなる。
inline bool IsRaytracedItem(const DrawItem& it, bool skinnedInTlas)
{
    if (it.renderer == nullptr || it.sortKey == 3u) return false;
    return it.skin == nullptr || skinnedInTlas;
}

} // namespace dx12e
