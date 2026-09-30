#pragma once

#include "core/Types.h"

namespace dx12e::vg
{

// 仮想ジオメトリ（Nanite 風）のシーン単位の設定。RtSettings / DdgiSettings と同じ流儀のヘッダオンリー定義。
//
// ★既定は OFF。OFF のとき VG のバッファ / PSO は一切確保せず、絵は導入前と完全に一致する。
//   `.vgeo` を置いたエンティティは、OFF ではプロキシが従来経路で描かれる。
//   ON（P3）: メッシュシェーダ対応 GPU では VG 本体が可視性バッファ + 深度へ描かれ、プロキシは深度プリパス / フォワードから外れる
//   （影・TLAS・ピッキング・物理は引き続きプロキシを見る）。非対応 GPU / raster=false では従来どおりプロキシが描かれる（統計だけ出る）。
struct VirtualGeometrySettings
{
    bool enabled       = false;    // 既定 OFF。P2 では統計のみ（描かない）
    f32  lodPixelError = 1.0f;     // τ（レンダー px）。0.25〜8 にクランプして使う
    bool hzbCulling    = true;     // 二段 HZB オクルージョン
    bool coneCulling   = true;     // 法線コーンの背面棄却（両面材質は P4 で対応）
    f32  instanceMinPx = 0.5f;     // 画面上の半径がこれ未満のインスタンスは棄却（0 で無効）
    i32  vramBudgetMB  = 3072;     // 全アセットのページ + BVH の合計がこれを超える読込は構造化エラーで拒否

    // ── P3（実行時の設定。シーン JSON には保存しない = 既存シーンのバイト列は不変。シーンを開き直すと既定へ戻る）──
    bool raster        = true;     // false = P2 の挙動（統計だけ・プロキシを描く）。true でも GPU 非対応なら自動で無効
    bool rasterAs      = false;    // 増幅シェーダ（32 クラスタ / グループ + 追加カリング）経由。既定は実測で決めた値（VG_P3_REPORT.md）
    bool smallPrimCull = false;    // 画素中心を 1 つも覆わない三角形をメッシュシェーダで落とす（5060 の実測では速くならない = 既定 OFF。VG_P3_REPORT.md）
    bool measure       = false;    // 計測（断片数 / オーバードロー画像 / 被覆画素 / 辺長ヒストグラム）。少し遅い
    bool forceLod0     = false;    // 検証 / 計測用: LOD0 の葉だけを選ぶ（全 LOD0 の素朴な参照）
    // ── P4（実行時の設定。保存しない）──
    bool resolve       = true;     // 材質 resolve（フォワードと同じライティング）。false = P3 の暫定シェーディング（A/B 用）
    bool stableOrder   = false;    // 決定論（可視リストの安定ソート）。決定論キャプチャ中は自動で ON

    bool operator==(const VirtualGeometrySettings& o) const
    {
        return enabled == o.enabled && lodPixelError == o.lodPixelError && hzbCulling == o.hzbCulling
            && coneCulling == o.coneCulling && instanceMinPx == o.instanceMinPx && vramBudgetMB == o.vramBudgetMB;
        // ★P3 の実行時設定（raster / rasterAs / ...）は比較に含めない: シーン JSON の保存要否（!= 既定）を変えない＝既存シーンはバイト不変。
    }
    bool operator!=(const VirtualGeometrySettings& o) const { return !(*this == o); }
};

} // namespace dx12e::vg
