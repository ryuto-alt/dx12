#pragma once
//
// VgEligibility.h ― 仮想ジオメトリ P4: エンティティが VG で描けるか（対象外なら理由）。
//
//   VG の resolve が扱うのは「不透明 PBR の標準材質（1 クラスタ = 1 材質）」だけ（VG 設計書 §2.2.2 の確定事項）。
//   対象外のエンティティは VG のインスタンスに載せず、プロキシ（MeshRenderer）を従来経路で描く。
//   ★判定はこの 1 関数に一本化する（ApplicationVg.cpp のインスタンス収集 / プロキシを外す集合 / エディタの Inspector の警告が
//     同じ関数を呼ぶ。IsRaytracedItem と同じ理由: 判定が 2 箇所に割れると「VG でもプロキシでも描かれない」事故になる）。
//   ヘッダオンリー（ECS の読み取りだけ）。
//
//   2026-11-10（feat/vg-materials）: 材質アセット（.dxmat）・テクスチャの上書き・materialRemap は VG で描けるようになった
//   （エンティティ単位の材質区間。ApplicationVg.cpp の BuildVgEntityMaterials）。残る対象外は下の列挙だけ。
//   マテリアルグラフ（.dxmat の graph = .dxmg）は材質ごとにシェーダが違うので、resolve の材質 ID ごとのパスが要る（未実装。
//   docs/VIRTUAL_GEOMETRY_DESIGN.md §2.4.9）。判定には .dxmat の中身が要るので、呼び出し側が isGraphMaterial を渡す
//   （渡さなければグラフかどうかは見ない = Inspector の簡易表示）。
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "ecs/Components.h"

namespace dx12e::vg
{

// 戻り値: 対象外の理由（日本語の短文）/ nullptr = VG で描ける。
//   isGraphMaterial(.dxmat の assets 相対パス) = その材質がマテリアルグラフか（未読込なら false でよい。次のフレームで読める）。
inline const char* VgIneligibleReason(const entt::registry& reg, entt::entity e,
                                      const std::function<bool(const std::string&)>& isGraphMaterial = {})
{
    if (reg.all_of<SkeletalAnimation>(e)) return "スキンドメッシュは仮想ジオメトリの対象外です（プロキシで描きます）";
    if (reg.all_of<NodeAnimationComp>(e)) return "ノードアニメのあるモデルは仮想ジオメトリの対象外です（プロキシで描きます）";
    if (const Terrain* t = reg.try_get<Terrain>(e); t && !t->layerSetPath.empty())
        return "地形マテリアルは仮想ジオメトリの対象外です（プロキシで描きます）";
    const MeshRenderer* mr = reg.try_get<MeshRenderer>(e);
    if (!mr) return nullptr;
    if (!mr->shaderPath.empty()) return "カスタムシェーダーは仮想ジオメトリの対象外です（プロキシで描きます）";
    if (isGraphMaterial)
    {
        const u32 n = static_cast<u32>((std::max)(mr->meshes.size(), mr->materialAsset.size()));
        for (u32 mi = 0; mi < n; ++mi)
            if (mr->HasMaterialAsset(mi) && isGraphMaterial(mr->MaterialAssetOf(mi)))
                return "マテリアルグラフ（.dxmg）の材質は仮想ジオメトリの対象外です（プロキシで描きます。設計は VIRTUAL_GEOMETRY_DESIGN.md §2.4.9）";
    }
    if (mr->alphaModeOverride == 1 || mr->alphaModeOverride == 2)
        return "アルファクリップ / 半透明は仮想ジオメトリの対象外です（プロキシで描きます）";
    if (mr->opacity < 0.999f) return "不透明度が 1 未満（半透明）は仮想ジオメトリの対象外です（プロキシで描きます）";
    if (mr->animFrames > 0 || mr->uvScrollU != 0.0f || mr->uvScrollV != 0.0f)
        return "UV アニメ（スクロール / 連番）は仮想ジオメトリの対象外です（プロキシで描きます）";
    return nullptr;
}

} // namespace dx12e::vg
