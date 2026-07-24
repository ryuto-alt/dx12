#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>
#include <imgui.h>

#include "core/Types.h"
#include "ecs/Components.h"
#include "ui/UISystem.h"   // UiResolvedRect（スマートガイドの吸着候補を保持する）

struct ID3D12GraphicsCommandList;   // UIImage テクスチャの遅延ロード用

namespace dx12e
{

class EditorContext;
class ResourceManager;
class DescriptorHeap;

// ゲーム内 UI（UICanvas ツリー）を専用の 2D キャンバスで見ながら編集するフローティング
// ツール窓（Unreal UMG デザイナー / Unity UI Builder 相当）。ツールバー「窓 ▾ > UIエディタ」で開く。
// - 描画: UISystem::RenderPreview を窓内の「仮想スクリーン」矩形へ流す（SceneView プレビューと同経路）。
//   仮想スクリーンは画面サイズプリセット（既定はキャンバスの基準解像度）で、実行時のゲーム
//   ビューポートと同じ ScaleToFit / レターボックスがそのまま再現される。
// - ビュー: マウスホイール = カーソル中心ズーム、中/右ドラッグ = パン、Fit / 100% ボタン。
// - 編集: UISystem::ResolveRects でヒットテストし、クリック選択（Ctrl でマルチ）・移動ドラッグ・
//   8 ハンドルリサイズ・アンカーハンドル（Unity の 4 三角相当）ドラッグを
//   ComponentEditCommand<UIRect> の Undo つきで行う。矢印キーでナッジ（Shift で 10px）、
//   Del で削除、Ctrl+D で複製。アセットブラウザからテクスチャを UIImage へ D&D 可。
// - 生成: パレット / 右クリックメニューから ctx.pendingSpawns に __ui_*__ マーカーを積む
//   （Hierarchy の「UI（ゲーム内UI）」メニューと同じ Application 側ハンドラが処理する）。
class UiEditorPanel
{
public:
    // ImGui パス内（メインの cmdList 記録中）に毎フレーム呼ぶ。ctx.showUiEditor が false なら即 return。
    // resources/srvHeap/cmdList は UIImage テクスチャの遅延ロード用（##GameUI 経路と同じ）。
    void RenderWindow(entt::registry& reg, EditorContext& ctx, const std::string& assetsDir,
                      ResourceManager* resources, DescriptorHeap* srvHeap,
                      ID3D12GraphicsCommandList* cmdList);

private:
    // ---- 階層ツリー（左ペイン。UMG/UI Builder の Hierarchy 相当）----
    // クリック選択（Ctrl でマルチ）、D&D で親変更（要素の中央へドロップ）・兄弟並べ替え
    // （要素の上下 1/4 へドロップ = その前/後へ挿入）。並び順は UIRect::order に永続化され、
    // 描画順（上 = 奥、下 = 手前）と入力の前面判定の両方に効く。
    using UiChildrenMap = std::unordered_map<entt::entity, std::vector<entt::entity>>;
    void DrawHierarchyPane(entt::registry& reg, EditorContext& ctx);
    void DrawUiTreeNode(entt::registry& reg, EditorContext& ctx, entt::entity e,
                        const UiChildrenMap& children);
    // 左ペイン下段: assets/prefabs/ui/ の一覧。キャンバスへの D&D 元になる。
    void DrawPrefabPalette(const std::string& assetsDir);
    // マルチ選択の整列 / 等間隔分布。
    // mode: 0=左 1=左右中央 2=右 3=上 4=上下中央 5=下 6=横に等間隔 7=縦に等間隔
    void AlignSelection(entt::registry& reg, EditorContext& ctx, int mode);

    // ツリー D&D の確定内容（描画中に registry を書き換えない。ツリー走査後に適用）
    entt::entity m_dropEntity = entt::null;   // 動かす要素
    entt::entity m_dropParent = entt::null;   // 新しい親（キャンバス or UI 要素）
    int          m_dropIndex  = -1;           // 兄弟内の挿入位置。-1 = 末尾

    // キャンバスへ落とされた .prefab（絶対パス）と、その落下位置。
    // ドロップ処理の中で spawn 要求を積むと解決済み矩形をもう一度引き直す必要があるので、
    // 1 フレーム内で「受け取る → 位置を決めて積む」を分けている。
    std::string m_pendingPrefabDrop;
    ImVec2      m_pendingPrefabDropPos{0.0f, 0.0f};

    // 左ペイン下段の UI プレハブパレット（assets/prefabs/ui/ の一覧）。
    // ディレクトリ走査は毎フレームやらず、「更新」ボタンとプレハブ作成時にだけ取り直す。
    std::vector<std::string> m_prefabPaths;
    bool m_prefabListLoaded = false;

    // ---- ビュー（パン / ズーム / 画面サイズ）----
    f32    m_zoom = 1.0f;
    ImVec2 m_pan{0.0f, 0.0f};      // 仮想スクリーン中心の、キャンバス領域中心からのずれ（スクリーン px）
    bool   m_fitRequested = true;  // 初回表示 / Fit ボタンで仮想スクリーンを領域に収める
    int    m_screenSizeIdx = 0;    // 画面サイズプリセット（0 = Canvas 基準解像度）
    int    m_customW = 1920, m_customH = 1080;
    bool   m_showGrid  = false;    // 50px（キャンバス空間）グリッド
    bool   m_checkerBg = true;     // 仮想スクリーン背景を市松模様に（OFF = 単色ダーク）

    // ---- 移動 / リサイズドラッグ（SceneView の HandleUiEditing と同じ絶対値方式）----
    // m_dragEdges: -1=非ドラッグ, 0=移動, 正=エッジ bit（1=左 2=右 4=上 8=下、四隅は 2bit）
    int          m_dragEdges  = -1;
    entt::entity m_dragEntity = entt::null;
    UIRect       m_dragStartRect{};              // ドラッグ開始時の UIRect（Undo の before）
    ImVec2       m_dragStartMouse{0.0f, 0.0f};   // 開始時のマウス（スクリーン座標）
    ImVec2       m_dragStartMin{0.0f, 0.0f};     // 開始時の解決済みスクリーン矩形
    ImVec2       m_dragStartMax{0.0f, 0.0f};
    f32          m_dragCanvasScale = 1.0f;       // スクリーンΔ → キャンバス px の換算係数
    // マルチ選択の一括移動: 掴んだ要素以外の選択要素と、そのドラッグ開始時の UIRect。
    // 掴んだ要素の子孫は親と一緒に動くので含めない（二重に移動してしまうため）。
    std::vector<std::pair<entt::entity, UIRect>> m_dragExtra;

    // ---- スマートガイド（Figma 風の整列吸着。Alt 押下で一時無効）----
    bool m_smartGuides = true;
    // 吸着候補の供給元。ResolveRects は毎フレーム走るのでその結果を保持しておく。
    std::vector<UiResolvedRect> m_lastRects;
    // 今フレーム表示するガイド線（スクリーン座標）。vertical=true なら x=pos の縦線、
    // a/b はもう一方の軸の描画範囲。
    struct GuideLine { bool vertical; f32 pos; f32 a; f32 b; };
    std::vector<GuideLine> m_guides;

    // ---- アンカーハンドルドラッグ ----
    // m_anchorDrag: -1=非ドラッグ, 0..3=三角（bit0: 0=X min / 1=X max、bit1: 0=Y min / 1=Y max）,
    //               4=中央（4 つまとめて移動。アンカーの広がりは保つ）
    int    m_anchorDrag = -1;
    UIRect m_anchorStartRect{};                  // Undo の before
    ImVec2 m_anchorRectMinC{0.0f, 0.0f};         // 開始時の解決済み矩形（キャンバス空間 px）
    ImVec2 m_anchorRectMaxC{0.0f, 0.0f};
    ImVec2 m_anchorParentMinC{0.0f, 0.0f};       // 開始時の親矩形（キャンバス空間 px）
    ImVec2 m_anchorParentMaxC{0.0f, 0.0f};

    // ---- 矢印キーナッジ（押している間は蓄積し、離した時に 1 コマンドで Undo 登録）----
    bool         m_nudging     = false;
    entt::entity m_nudgeEntity = entt::null;
    UIRect       m_nudgeStartRect{};

    // ---- 右クリック（パンと区別するため押下位置を覚え、ほぼ動いてなければメニュー）----
    ImVec2 m_rmbDownPos{0.0f, 0.0f};
    bool   m_rmbDownOnCanvas = false;
    bool   m_mmbDownOnCanvas = false;   // 中ボタンパンもキャンバス上で押下開始した時だけ有効
};

} // namespace dx12e
