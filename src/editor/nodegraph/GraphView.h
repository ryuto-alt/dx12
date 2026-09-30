#pragma once

// ===== 汎用ノードグラフの ImGui ビュー（自前 ImDrawList 実装）=====
// キャンバス（ズーム 10〜400% / パン / グリッド / スナップ）・ノード・ワイヤ・選択・移動・コメント・ミニマップ・検索パレット・
// コンテキストメニュー・コピー/貼り付け/複製・整列・フレーム。データは GraphDocument（= IGraphModel）越しにだけ触る。
// 色・グローはすべて GraphTokens（theme:: から導出）。寸法は論理 px（GraphLayout の単位）に ui::Px 相当の DPI 倍率を掛ける。
//
// 入力: キャンバス全体を InvisibleButton 1 個で受け、ノード / ピン / ワイヤ / コメントの当たりは自前（GraphDocument::HitTest。
//       一様グリッドの空間分割）。ImGui アイテムはノードごとに作らない（500 ノードでも軽い）。
//
// 操作（設計書 §6.4）:
//   左ドラッグ = ノード移動 / 空き地で矩形選択（Shift = 追加）/ Ctrl+空き地ドラッグ = 横切ったワイヤを切断
//   ピンからドラッグ = 接続（入力ピンを掴むと付け替え・空き地で切断・出力から空き地で検索パレット）/ Alt+クリック = ピンの全切断
//   中ドラッグ / Space+左ドラッグ / 右ドラッグ = パン、ホイール = カーソル中心ズーム
//   右クリック（空き地）/ Space / 空き地ダブルクリック = 検索パレット、ノード・ワイヤ・コメント上の右クリック = メニュー
//   ワイヤのダブルクリック = リルート挿入、コメント見出しのダブルクリック = 名前変更、値欄のクリック / ドラッグ = 値の編集
//   ショートカット: EditorCommandTable.h の表（edit.* と graph.*）。

#include "editor/nodegraph/GraphDocument.h"
#include "editor/nodegraph/GraphIO.h"
#include "editor/nodegraph/GraphPalette.h"
#include "editor/nodegraph/GraphTokens.h"

#pragma warning(push)
#pragma warning(disable: 4201)
#include <imgui.h>
#pragma warning(pop)

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dx12e::ng
{

struct GraphViewSettings
{
    bool  snap        = true;
    float snapStep    = 16.0f;
    bool  showGrid    = true;
    bool  showMinimap = true;
    enum class Flow : int { Off = 0, Highlighted = 1, All = 2 };
    Flow  flow        = Flow::Highlighted;   // ワイヤの流れるハイライト
    bool  glow        = true;                // 選択・ホバー・ワイヤの発光（トークンの glow が 0 なら無効）
    bool  animate     = true;                // false: スプリングを使わず即時（テスト・決定論スクショ）
    float fixedTime   = -1.0f;               // >= 0 なら演出の時間をこの値に固定（決定論スクショ）
    bool  showPreview = true;                // プレビュー領域（ズーム 0.6 以上）
};

struct GraphViewStats
{
    float cpuMs = 0.0f;          // Draw 全体の CPU 時間（CpuScopeTimer）
    int   nodes = 0, edges = 0;
    int   drawnNodes = 0, drawnEdges = 0;
    int   drawnPrims = 0;        // このビューが積んだ ImDrawList の頂点数
    float zoom = 1.0f;
};

class GraphView
{
public:
    // アンカー（仮想入力の imgui_find が引ける「名前つき要素」）を出すための差し込み口。
    // kind/label は VirtualInputImGui の Anchor と同じ流儀。mn/mx は ImGui のスクリーン座標。
    using AnchorFn = std::function<void(const char* kind, const std::string& label, ImVec2 mn, ImVec2 mx)>;

    GraphView();

    void SetDocument(GraphDocument* doc);
    GraphDocument* Doc() const { return m_doc; }
    GraphViewSettings& Settings() { return m_set; }
    const GraphViewSettings& Settings() const { return m_set; }
    void SetAnchorSink(AnchorFn fn, int maxNodesWithPins) { m_anchor = std::move(fn); m_anchorMaxNodes = maxNodesWithPins; }

    // キャンバスを描いて入力を処理する。size <= 0 の軸は残り全部。窓の中（Begin/End の間）で呼ぶ。
    void Draw(const char* strId, ImVec2 size);
    // ショートカット（表の chord を見て Execute する）。Draw の直後・同じ窓の中で呼ぶ（フォーカス判定を使う）。
    void ProcessKeys();
    // コマンド実行（EditorCommandTable の edit.* / graph.* の id）。実行できれば true。
    bool Execute(const std::string& id);
    bool WantsKeyboard() const { return m_keyboardActive; }   // グラフがキーを使うか（キャンバス or ポップアップにフォーカス）

    // ---- 選択 ----
    const Selection& GetSelection() const { return m_sel; }
    void SetSelection(const Selection& s) { m_sel = s; ++m_selRev; }
    void SelectAll();

    // ---- ビュー ----
    ViewState GetViewState() const { return ViewState{m_pan, m_zoom}; }
    void SetViewState(const ViewState& v, bool animate);
    void FrameAll(bool animate = true);
    void FrameSelection(bool animate = true);
    void ResetZoom(bool animate = true);
    void SettleAnimations();   // 進行中のビュー / ノードのアニメを最終値へ（テスト・決定論スクショ用）
    void CenterOn(Vec2 graphPos, bool animate = true);

    // ---- ノードの状態表示（診断・コンパイル状態などをモデルの外から載せる。G2c が使う）----
    enum class NodeState : uint8_t { Normal, Warning, Error, Compiling, Dimmed };
    void SetNodeState(NodeId id, NodeState s, const std::string& message = std::string());
    void ClearNodeStates() { m_nodeStates.clear(); }

    // ---- 状態 ----
    const GraphViewStats& Stats() const { return m_stats; }
    // 仮想入力・テストが読む 1 行の状態（"nodes=35 edges=40 sel=2 undo=3 redo=0 zoom=1.00 mode=idle ..."）
    std::string StatusLine() const;
    const char* ModeName() const;
    bool  IsPaletteOpen() const { return m_paletteOpen; }
    bool  IsContextMenuOpen() const { return m_ctxOpen; }

    // ---- テストフック（前フレームの配置を返す）----
    ImVec2 ScreenPinPos(PinRef pin) const;
    ImVec2 ScreenNodeRect(NodeId id, ImVec2* outMax = nullptr) const;
    ImVec2 CanvasMin() const { return m_canvasMin; }
    ImVec2 CanvasMax() const { return m_canvasMax; }
    Viewport CurrentViewport() const { return m_vp; }
    ImVec2 ToScreen(Vec2 g) const { const Vec2 s = m_vp.ToScreen(g); return ImVec2(s.x, s.y); }
    Vec2   ToGraph(ImVec2 p) const { return m_vp.ToGraph(Vec2(p.x, p.y)); }
    float  TitleHeightFor(float zoom) const;
    // 検索パレットの外部操作（テスト・MCP）: 開く / 検索語を入れる / 選んで確定
    void OpenPaletteAt(Vec2 graphPos);
    void SetPaletteQuery(const std::string& q);
    bool ConfirmPalette(int index);

private:
    enum class Mode : uint8_t { Idle, Panning, BoxSelect, MoveItems, ResizeComment, LinkDrag, CutStroke, SlotDrag, MinimapDrag };
    enum class CtxKind : uint8_t { None, Node, Edge, Comment, Pin, Selection };

    struct NodeAnim { Spring hover, sel, lift; };

    // ---- フレーム処理 ----
    void BeginFrame(ImVec2 size);
    void UpdateViewAnim(float dt);
    void HandleInput(bool hovered, bool canvasActive);
    void AutoPan(float dt);
    void UpdateFlowSet();
    void AnimStep(Spring& sp, float target, float freq, float damping);
    void Render();
    void DrawGrid(ImDrawList* dl);
    void DrawComments(ImDrawList* dl);
    void DrawEdges(ImDrawList* dl);
    void DrawNodes(ImDrawList* dl);
    void DrawOverlays(ImDrawList* dl);
    void DrawMinimap(ImDrawList* dl);
    void DrawPalette();
    void DrawContextMenus();
    void DrawEditPopups();

    // ---- 入力の内部 ----
    Hit  HitTestAt(ImVec2 screen);
    HitParams MakeHitParams() const;
    void BeginLinkFrom(PinRef pin, bool detachExisting);
    void FinishLink(bool dropped);
    void SelectClick(NodeId id, bool add, bool toggle);
    void ApplyBoxSelection();
    void BeginMoveSession(NodeId primary);
    void UpdateMoveSession();
    void OpenSlotEditor(const Hit& hit, ImVec2 screen);
    void OpenContext(CtxKind kind, const Hit& hit, Vec2 graphPos);
    void CreateFromPalette(const PaletteItem& item);
    void StartRename(CommentId id);
    bool MinimapRect(ImVec2& mn, ImVec2& mx) const;
    void MinimapCenterOn(ImVec2 mouse);
    Vec2 SnapPos(Vec2 p) const;
    void PruneSelection();
    void PasteFromClipboard(bool atMouse);
    void CopyToClipboard();
    void ShowBringToFront(const Selection& s);

    // ---- 描画の内部 ----
    ImFont* FontBody() const;
    ImFont* FontBold() const;
    ImFont* FontMono() const;
    void DrawText(ImDrawList* dl, ImFont* font, float units, ImVec2 pos, ImU32 col, const char* text, const ImVec4* clip = nullptr) const;
    void DrawPinShape(ImDrawList* dl, ImVec2 c, float r, PinShape shape, bool filled, ImU32 col, ImU32 inner, float alpha) const;
    void DrawGlow(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding, ImU32 col, float strength, float radiusPx) const;
    void DrawSlot(ImDrawList* dl, const NodeData& n, int pinIndex, const Rect& slotG, float scale, float alpha, bool hoverSlot, int hoverComp);
    void DrawPreview(ImDrawList* dl, const NodeData& n, const Rect& rg, float scale, ImU32 base);
    float Now() const;

    GraphDocument*    m_doc = nullptr;
    GraphViewSettings m_set;
    GraphTokens       m_tok;
    Selection         m_sel;
    uint64_t          m_selRev = 1;
    AnchorFn          m_anchor;
    int               m_anchorMaxNodes = 0;

    // ---- ビュー ----
    Vec2  m_pan, m_panTarget;
    float m_zoom = 1.0f, m_zoomTarget = 1.0f;
    Spring m_sPanX, m_sPanY, m_sZoom;
    bool  m_framing = false;
    bool  m_zoomAnimating = false;
    Vec2  m_zoomAnchor;
    Viewport m_vp;
    ImVec2 m_canvasMin, m_canvasMax;
    float  m_dpi = 1.0f;
    float  m_dt = 1.0f / 60.0f;
    double m_time = 0.0;
    bool   m_hovered = false;
    bool   m_stationary = false;
    bool   m_windowFocused = false;
    bool   m_keyboardActive = false;
    ImVec2 m_mouse;
    Vec2   m_mouseG;
    float  m_lastLayoutDpi = 0.0f;

    // ---- 入力状態 ----
    Mode  m_mode = Mode::Idle;
    ImVec2 m_pressPos;
    Vec2   m_pressG;
    int    m_pressButton = -1;
    bool   m_dragStarted = false;
    Hit    m_pressHit;
    bool   m_pendingClickSelect = false;
    NodeId m_clickNode = 0;
    bool   m_rmbMoved = false;
    bool   m_spacePan = false;

    GraphDocument::MoveSession m_move;
    NodeId m_movePrimary = 0;
    Vec2   m_movePrimaryStart;
    Selection m_moveSel;
    bool   m_moveSnapped = false;

    Selection m_boxBase;
    Comment   m_resizeOld;

    // 接続ドラッグ
    PinRef m_linkFrom;
    bool   m_linkFromOutput = true;
    bool   m_linkDetach = false;
    Edge   m_linkDetached;
    PinRef m_linkHoverPin;
    bool   m_linkHoverValid = false;
    bool   m_linkHoverAny = false;
    std::string m_linkReason;
    Vec2   m_linkDropG;

    // ワイヤ切断ストローク
    std::vector<Vec2> m_cutPts;
    std::vector<uint64_t> m_cutMarked;   // 切る予定の入力ピンの Key

    // 値欄の編集
    struct SlotDrag { PinRef pin; int comp = 0; PinValue oldValue; float startValue = 0.0f; bool dragging = false; bool moved = false; ImVec2 pressPos; };
    SlotDrag m_slot;

    // ---- ホバー / アニメ ----
    Hit    m_hover;
    NodeId m_hoverNode = 0;
    PinRef m_hoverPin;
    Edge   m_hoverEdge;
    bool   m_hoverEdgeValid = false;
    CommentId m_hoverComment = 0;
    Spring m_wireHover;
    Edge   m_wireHoverEdge;
    std::unordered_map<NodeId, NodeAnim> m_anim;
    std::unordered_map<uint64_t, Spring> m_pinAnim;
    std::unordered_map<NodeId, uint32_t> m_z;
    uint32_t m_zCounter = 1;
    std::unordered_set<NodeId> m_upSet, m_downSet;   // 選択ノードの上流 / 下流（選択自身を含む）。ワイヤの強調に使う
    uint64_t m_flowKey = 0;
    std::vector<uint64_t> m_connKeys;    // 接続されているピンの Key（昇順）
    std::vector<NodeId>   m_visNodes;    // 今フレーム見えているノード（前面順）
    std::vector<int>      m_visEdges;
    uint64_t m_connRev = ~0ull;
    ImDrawListSplitter m_split;
    float m_lhLabel = 12.0f, m_lhTitle = 13.0f, m_lhMono = 12.0f;   // 1 行の高さ（px。今フレームの倍率で測った値）
    float m_lastPx = 0.0f;

    // ---- ポップアップ ----
    bool   m_paletteOpen = false;
    bool   m_paletteRequest = false;
    bool   m_palFocus = false;
    Vec2   m_palGraphPos;
    ImVec2 m_palScreenPos;
    PaletteFilter m_palFilter;
    char   m_palQuery[128] = {0};
    int    m_palSel = 0;
    std::string m_palCategory;
    std::vector<PaletteItem> m_palItems;
    std::vector<std::string> m_palCats;
    PinRef m_palPin;
    bool   m_palHasPin = false;
    bool   m_palPinOutput = false;
    bool   m_palRefresh = true;
    bool   m_palQueryDirty = false;
    bool   m_palCloseRequest = false;

    bool    m_ctxOpen = false;
    bool    m_ctxRequest = false;
    CtxKind m_ctxKind = CtxKind::None;
    Hit     m_ctxHit;
    Vec2    m_ctxG;

    bool    m_renameRequest = false;
    CommentId m_renameComment = 0;
    char    m_renameBuf[128] = {0};

    bool    m_editRequest = false;
    enum class EditKind : uint8_t { None, Text, Enum, Color } m_editKind = EditKind::None;
    PinRef  m_editPin;
    int     m_editComp = 0;
    PinValue m_editOld;
    char    m_editBuf[64] = {0};
    ImVec2  m_editScreen;

    struct NodeStateInfo { NodeState state = NodeState::Normal; std::string message; };
    std::unordered_map<NodeId, NodeStateInfo> m_nodeStates;
    PinType m_linkSrcType = 0;
    std::string m_clipboard;
    GraphViewStats m_stats;
    std::string m_lastAction;
};

} // namespace dx12e::ng
