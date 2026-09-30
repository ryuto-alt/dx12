#pragma once

// ===== ワークスペース / レイアウトの管理（フェーズ 1b W）=====
// ドックの構築・分割比の保存 / 復元・ワークスペース切替・名前つきレイアウト・ツール窓の配置先・下部ドックのタブ（BottomDock.h）を受け持つ。
// 純ロジックは editor/WorkspaceLogic.h（tests/workspace_logic_test.cpp）。ここは ImGui のドックノードに触る層。
//
// 方針（設計メモ。詳細は WorkspaceLogic.h の冒頭）:
//   ・レイアウトは自前の Layout 構造体（分割比 4 本 + 開く窓 + 配置先の上書き + 下部タブ）で持ち、ImGui の ini 文字列は使わない。
//   ・ドックは起動時とリセット / 配置先変更（右分割・フローティング）のときだけ作り直す。ツール窓の開閉・ワークスペース切替・最大化では
//     作り直さず、既存ノードの SizeRef を書き換えて分割比を変える（タブの選択状態を失わず、ビューポートが縮み続けない）。
//   ・分割比の保存は「ユーザーがスプリッタを動かした差分」だけを取り込む（ImGui 内部の丸め・セパレータ幅で毎回少しずつ縮む drift を避ける）。
//   ・永続化（editor_state.json の "prefs"）は --background / UI 自動テスト / 仮想入力中は書かない（メモリ上では動く）。
//     フローティング窓の位置だけは従来どおり imgui.ini（ユーザーデータ領域の絶対パス）に任せる。

#include "editor/WorkspaceLogic.h"
#include "editor/ToolWindows.h"

#include <set>
#include <string>
#include <vector>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

namespace dx12e
{

class EditorContext;

class WorkspaceManager
{
public:
    // 起動時に 1 回。永続化が有効なら prefs からレイアウトを復元する（最初の BuildLayout が使う）。
    void Initialize(EditorContext& ctx);

    // EditorLayer::Render の頭（DockSpace より前）。要求（切替 / 復元 / 削除 / 最大化 / リセット）を消化する。
    // 戻り値 true = ドックを作り直す（EditorLayer が BuildLayout を呼ぶ）。
    bool BeginFrame(EditorContext& ctx);

    // ドックの構築（起動時と作り直し時）。dockspaceId は EditorLayer の DockSpace の ID。
    void BuildLayout(ImGuiID dockspaceId);

    // 下部ドックの登録タブの窓（BottomDock.h）を描く。パネル群の描画のあと・ビューポート矩形の計算より前で呼ぶ。
    void RenderBottomTabs(EditorContext& ctx);

    // DockSpace() のあと毎フレーム 1 回。分割比の吸い上げ / 保存 / タブ帯のダブルクリック / 整列 / レイアウトの永続化。
    //   area = 整列に使うビューポート領域（x,y,w,h）。minViewportPx = 下部ドック最大化でもビューポートに残す高さ（px）。
    void EndDockFrame(EditorContext& ctx, float areaX, float areaY, float areaW, float areaH, float minViewportPx);

    // レイアウト管理の窓（保存名の入力 / ツール窓の配置先）。最後に描く。
    void DrawWindows(EditorContext& ctx);

    // ---- 問い合わせ / 操作（メニュー・テスト用）----
    ws::Slot SlotOf(const tools::Desc& d) const;                 // 実効の配置先（既定 + ユーザーの上書き）
    void     SetToolSlot(EditorContext& ctx, const tools::Desc& d, ws::Slot slot);   // 配置先を変える（保存もされる）
    static bool IsMovable(const tools::Desc& d);                 // 配置先を選べる窓か（既定が右タブで、ドック用の窓名を持つもの）
    ws::Layout Snapshot(const EditorContext& ctx) const;         // 今の状態（開いている窓・比・配置先・下部タブ）
    const ws::Layout& CurrentLayout() const { return m_layout; }
    bool Maximized() const { return m_bottomMax; }
    // 分割の計測値（テスト用）。無効なら false。
    bool Measure(ws::Ratios& out) const;
    ImGuiID NodeBottom() const { return m_nodeBottom; }
    ImGuiID NodeRightTop() const { return m_nodeRightTop; }
    ImGuiID NodeRightSplit() const { return m_nodeRightSplit; }

    // 永続化してよいか（--background / UI 自動テスト / 仮想入力中は false）。
    static bool PersistAllowed();

private:
    void ApplyLayout(EditorContext& ctx, const ws::Layout& layout, bool arrange);
    void ApplyRatiosLive(const ws::Ratios& r);
    void SetBottomPixels(float px);
    void SwitchWorkspace(EditorContext& ctx, const std::string& id);
    void SaveNamed(EditorContext& ctx, const std::string& rawName);
    void RestoreNamed(EditorContext& ctx, const std::string& name);
    void DeleteNamed(EditorContext& ctx, const std::string& name);
    void RefreshNames(EditorContext& ctx);
    void DockWindowTo(const char* windowName, ws::Slot slot);
    void PersistNow(const EditorContext& ctx);
    void SetMaximized(EditorContext& ctx, bool on);
    void ArrangeNewFloating(float ax, float ay, float aw, float ah);

    ws::Layout         m_layout;        // 現在の論理レイアウト（開く窓は ctx のフラグが正。ここは比・配置先・下部タブの状態）
    ws::WorkspaceState m_state;
    bool m_persist = false;

    // ドックのノード ID（BuildLayout が埋める。0 = 無い）
    ImGuiID m_dockspaceId = 0;
    ImGuiID m_nodeLeft = 0, m_nodeRemaining = 0, m_nodeRightCol = 0, m_nodeCenter = 0;
    ImGuiID m_nodeBottom = 0, m_nodeViewport = 0, m_nodeRightTop = 0, m_nodeRightSplit = 0;
    bool    m_needRebuild = false;

    // 分割比の吸い上げ（ユーザーがスプリッタを動かした差分だけ取り込む）
    ws::Ratios m_baseline;
    bool       m_baselineValid = false;
    int        m_baselineWait = 0;

    // 下部ドックの最大化
    bool  m_bottomMax = false;
    float m_minViewportPx = 160.0f;

    // 登録タブ（BottomDock.h）でドック済みの id。閉じたら外す（開き直すとまたドックする）
    std::set<std::string> m_bottomDocked;
    std::string m_focusBottomId;   // 開いた直後 / 復元直後に選択状態にする登録タブ
    int         m_focusBottomFrames = 0;

    // ワークスペース切替の整列（新しく開いたフローティング窓を重ねて置く）
    std::set<ImGuiID> m_arrangeBefore;
    std::set<ImGuiID> m_arrangeDone;
    int               m_arrangeFrames = 0;

    // 永続化の間引き
    ws::Layout m_lastPersisted;
    bool       m_hasPersisted = false;
    int        m_persistTick = 0;

    // 窓の状態
    bool m_saveWindowOpen = false;
    char m_saveNameBuf[64] = {};
    bool m_slotsWindowOpen = false;
};

} // namespace dx12e
