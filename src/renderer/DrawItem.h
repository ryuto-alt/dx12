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
    // 保守的ワールド AABB。オクルージョンカリングのスクリーン矩形投影に使う。
    // ★球（center/radius）ではなくこちらを使うのは、球には max(1.0f, ...) の下限と
    //   1.25/2.0 倍のバイアスが積み重なっていて、屋内の小物ではスクリーン矩形が
    //   実体の何倍にも膨らむため。矩形が大きいほど遮蔽と判定されにくくなる＝
    //   安全側ではあるが、ほとんど何もカリングできなくなる。
    //   こちらは下限を継承しない。ただしスキンド/ノードアニメの変形ぶんの余裕は残す。
    DirectX::XMFLOAT3   aabbMin;
    DirectX::XMFLOAT3   aabbMax;
    u32                 lod;          // メインカメラ基準の選択LOD（Mesh 側でクランプされる）
    bool                hasNodeAnim;
    u32                 sortKey;      // 0=既定static / 1=カスタム不透明 / 2=skinned / 3=半透明(最後)
    // 透明の分類（エンティティ単位。サブメッシュの最大値）。0=OPAQUE 1=MASK 2=BLEND。
    // ★BLEND は sortKey=3 に落として「不透明の後に、カメラから遠い順」で描く。
    //   MASK は不透明パスのまま（深度を書くのでソート不要＝Hi-Z とも喧嘩しない）。
    u8                  alphaClass;
    // バウンディング球中心までのカメラ距離。半透明の後→前ソートに使う（LOD 選択で計算済み）。
    f32                 camDist;
    // 自動インスタンシングのバッチ鍵。0 = インスタンシング不可（従来の per-object 描画）。
    // 同一キー同士は「同じメッシュ・同じLOD・同じマテリアル/PBR値」＝1ドローに畳んで良い。
    u64                 batchKey;
};

// 自動インスタンシングで 1 ドローに畳まれる連続区間（ソート済み描画リスト上の [first, first+count)）。
// 区間の切れ目は batchKey だけで決まる＝視錐台カリングより前に確定できるので、
// 描画を記録し始める前に「バッチ全体が隠れているか」を GPU へ問い合わせられる。
//
// ★aabb は区間内**全インスタンス**の合成 AABB。視錐台で落ちるぶんも含んだ上位集合なので、
//   これが隠れていれば実際に描く 1 体 1 体も必ず隠れている（保守的）。
//   バッチは 1 ドローコールなので、述語もバッチ単位でしか張れない。
struct DrawBatch
{
    u32               first;
    u32               count;
    DirectX::XMFLOAT3 aabbMin;
    DirectX::XMFLOAT3 aabbMax;
};

// オクルージョン判定へ出す 1 件ぶんのワールド AABB。
// アイテム単体のものとバッチの合成のものが同じ配列に混ざる（判定側は区別しない）。
struct OcclusionBounds
{
    DirectX::XMFLOAT3 aabbMin;
    DirectX::XMFLOAT3 aabbMax;
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
//  - alphaClass == 1 … アルファテスト(MASK。葉・柵・金網・草)。
//    ★これを TLAS に入れると**葉の影が板の影になる**。
//      BLAS のジオメトリは全て D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE で作られ
//      （RaytracingScene.cpp:173,275 / 同 .h の設計メモ）、any-hit を一切使わないので、
//      レイはカードの矩形そのもので遮られる。テクスチャのアルファは見ない。
//      しかも RT 影が有効なフレームは CSM 側がこの関数で「RT が担当する」と判断して
//      描画から外すため、**正しく discard していた ShadowMask.hlsl の経路まで失われる**
//      （shaders/shadow/ShadowMask.hlsl は clip(baseColor.a - cutoff) を撃つためだけに存在する）。
//      結果、RT 影を ON にした瞬間に木や金網の太陽影が矩形になる＝機能を足したのに絵が劣化する。
//      正攻法は any-hit シェーダでアルファを見ることだが、OPAQUE を外すと交差判定が
//      2〜10 倍重くなるうえシェーダテーブルの新設が要る。MASK は CSM 側に
//      既に正しい実装があるので、そちらへ任せるのが素直で速い。
//      （代償: 葉は RT-AO の遮蔽物にもならない。精度より「絵が壊れない」を採る）
inline bool IsRaytracedItem(const DrawItem& it, bool skinnedInTlas)
{
    if (it.renderer == nullptr || it.sortKey == 3u || it.alphaClass == 1u) return false;
    return it.skin == nullptr || skinnedInTlas;
}

} // namespace dx12e
