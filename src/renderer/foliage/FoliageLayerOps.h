#pragma once
// ===========================================================================
// 植生 F1: FoliageLayer コンポーネントに対する高レベル操作（読込 / 保存 / 差し替え / モデル指定の分解）。
// シーンのシリアライザ・MCP・エディタ・描画（FoliageSystem）が同じ関数を使う。
// ===========================================================================
#include <memory>
#include <string>
#include <vector>

#include "core/Types.h"
#include "ecs/Components.h"
#include "renderer/foliage/FoliageMath.h"
#include "renderer/foliage/FoliageTypes.h"

namespace dx12e::foliage
{

// 「a.glb;b.glb;c.glb」→ {"a.glb","b.glb","c.glb"}（空要素・前後の空白は捨てる。最大 kMaxLods）
std::vector<std::string> ParseLodList(const std::string& s);
inline std::string JoinLodList(const std::vector<std::string>& v)
{
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) { if (i) o += ';'; o += v[i]; }
    return o;
}
// variant0..3 の文字列へのポインタ（index 0..3）
inline const std::string& VariantString(const FoliageLayer& l, u32 i)
{
    return i == 0 ? l.variant0 : (i == 1 ? l.variant1 : (i == 2 ? l.variant2 : l.variant3));
}
inline std::string& VariantStringMut(FoliageLayer& l, u32 i)
{
    return i == 0 ? l.variant0 : (i == 1 ? l.variant1 : (i == 2 ? l.variant2 : l.variant3));
}
// 空でない variant の数（先頭から連続して空でないもの。途中に空があればそこまで）
u32 CountVariants(const FoliageLayer& l);

// instancePath から _set を読む（未読込のときだけ。失敗しても _loadTried が立つ＝毎フレーム再試行しない）。読めたら true。
bool EnsureLoaded(FoliageLayer& l, std::string* err = nullptr);
// 実体を差し替える（コピーオンライト。_needsSave を立てる）。
void ReplaceSet(FoliageLayer& l, std::shared_ptr<FoliageInstanceSet> s);
// 実体が空でなければ、このレイヤーの現在のインスタンス数（未読込は 0）
inline u32 InstanceCount(const FoliageLayer& l) { return l._set ? l._set->Count() : 0u; }

// .dxfoliage を assetsDir 配下へ書く。instancePath が空なら entityName から決めて設定する。
// _set が無い / 書く必要が無い（_needsSave が偽で instancePath がある）ときは何もしない。書いたら true。force で必ず書く。
bool FlushSidecar(FoliageLayer& l, const std::string& entityName, const std::string& assetsDir, std::string* err = nullptr,
                  bool force = false);

// CullParams のうち FoliageLayer から決まる部分（視点依存の planes / camPos は呼び出し側）
CullParams MakeLayerCullParams(const FoliageLayer& l, u32 variantCount, const u32 lodCount[kMaxVariants]);

} // namespace dx12e::foliage
