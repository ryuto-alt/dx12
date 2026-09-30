#pragma once

// マテリアルグラフ窓の内部共有（MaterialGraphPanel*.cpp だけが include する）。
// 3 つの .cpp に分けている: 本体（窓・ツールバー・ファイル・ダイアログ）/ 側面（パレット・詳細）/ 下部（診断・HLSL）。

#include "editor/EditorContext.h"
#include "editor/panels/MaterialGraphPanel.h"
#include "editor/matgraph/HlslColorize.h"
#include "editor/matgraph/MatGraphEditor.h"
#include "editor/matgraph/NodeThumbs.h"
#include "editor/nodegraph/GraphView.h"
#include "renderer/matgraph/PreviewLogic.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <set>
#include <string>
#include <vector>

namespace dx12e::mgpanel
{

namespace mat = dx12e::matgraph;
namespace mg = dx12e::mg;
namespace ng = dx12e::ng;

enum class Dialog : unsigned char { None, ConfirmDiscard, Open, SaveAs, Custom, Texture };

// 連続編集（ドラッグ・入力中）は生の変更で反映し、どのウィジェットも操作されなくなった時点で 1 コマンドに確定する
struct PropEdit
{
    bool        live = false;
    ng::NodeId  node = 0;
    std::string key;
    mat::Json   old;
};

struct State
{
    mg::MatGraphEditor ed;
    ng::GraphView      view;
    bool init = false;

    // ---- レイアウト（論理 px）----
    float leftW = 244.0f, rightW = 330.0f, bottomH = 180.0f;
    bool  showLeft = true, showRight = true, showBottom = true;

    // ---- 通知・コマンド ----
    std::string message;
    double      messageUntil = 0.0;
    bool        messageBad = false;
    std::vector<std::string> pending;

    // ---- ダイアログ ----
    Dialog      dialog = Dialog::None;
    bool        dialogRequest = false;
    std::string confirmAction;               // 未保存の確認のあとに実行する（"matgraph.new" 等 / "open:<path>"）
    std::vector<std::string> fileList;       // 開く: assets 以下の .dxmg（相対）
    bool        fileListScanned = false;
    char        fileFilter[128] = {0};
    char        pathBuf[512] = {0};
    std::string dialogError;
    bool        overwriteAsk = false;
    // Custom の HLSL 編集
    bool        customRequest = false;
    ng::NodeId  customNode = 0;
    std::vector<char> codeBuf;               // 16 KB
    int         customOutType = 0;
    // テクスチャ選択
    bool        texRequest = false;
    bool        texScanned = false;
    ng::NodeId  texNode = 0;
    std::string texKey;
    std::vector<std::string> texList;
    char        texFilter[128] = {0};

    // ---- パレット ----
    char        palQuery[128] = {0};
    std::set<std::string> palOpen;           // 開いているカテゴリ
    bool        palOpenInit = false;
    int         palHover = -1;

    // ---- 詳細 ----
    PropEdit    pe;
    bool        settingsLive = false;
    mat::GraphSettings settingsOld;
    bool        detailPropsOpen = true, detailGraphOpen = true, detailParamsOpen = true, detailPreviewOpen = true;

    // ---- 下部 ----
    int         bottomTab = 0;               // 0 = 診断 / 1 = HLSL
    bool        showInfo = false, showErr = true, showWarn = true;
    int         diagCursor = -1;
    // HLSL ビューア（エラー時は直前に生成できた版を薄く出す）
    std::string hlsl;
    bool        hlslStale = false;
    std::vector<std::string> hlslLines;
    std::vector<std::vector<mg::HlslSpan>> hlslSpans;
    std::vector<ng::NodeId> hlslLineNode;    // 行 → ノード（#line ディレクティブと sourceMap から）
    int         hlslCompileSeen = -1;
    bool        hlslLineNums = true;
    int         hlslScrollToLine = -1;
    ng::NodeId  hlslSelSeen = 0;

    // ---- G2c: GPU 連携（ライブプレビュー / ノード内サムネイル / 非同期コンパイル）----
    std::string previewKey = "__mgpreview/main";   // GraphMaterialSystem のインスタンスキー（PreviewLite）
    mat::PreviewSettings pv;                        // 形状・環境・カメラ・ライト（エディタ設定 matgraph.preview。グラフには入れない）
    bool        pvLoaded = false;
    std::string pvSaved;                            // 最後に保存した文字列（変化があったときだけ prefs へ）
    bool        pvVisible = false;                  // 今フレーム、プレビュー枠を描いた（RenderGpu が描く）
    unsigned    pvPixels = 256;
    ImVec2      pvMin{0, 0}, pvMax{0, 0};           // 直近のプレビュー枠（画面座標。テスト・撮影用）
    mat::Debouncer debounce;                        // 構造編集 → 250 ms → GPU へ（値のみの変更は通さない）
    uint64_t    seenStruct = ~0ull, seenValueSeq = ~0ull, pushedStruct = ~0ull;
    uint64_t    expectHash = 0;                     // 最後に GPU へ送ったグラフの HLSL ハッシュ
    double      editAt = -1.0;                      // 最後の構造編集の時刻（ImGui の時計）。見た目更新まで測る
    std::vector<double> latencyMs;                  // 編集 → 見た目更新（デバウンス込み）の実測
    std::vector<double> latencyNoDebounceMs;        // 同（デバウンス発火後 = コンパイル要求から）
    double      fireAt = -1.0;                      // デバウンスが発火した時刻
    bool        gpuCompiling = false;               // 新しい版を作っている最中（ワーカー）
    bool        gpuFailed = false;
    bool        gpuOptimizing = false;
    int         gpuLevel = 0, gpuPhase = 0, gpuOptPhase = 0;
    bool        gpuCacheHit = false;
    double      gpuDxcMs = 0, gpuPsoMs = 0, gpuUsableMs = 0, gpuFinalMs = 0, gpuCodegenMs = 0;
    uint64_t    gpuActiveHash = 0;
    std::string gpuError;
    mg::NodeThumbs thumbs;
    std::vector<ng::NodeId> thumbVisible;           // 今フレーム、サムネイル枠を描いたノード
    std::vector<ng::NodeId> thumbVisiblePrev;
    bool        thumbsOn = true;
    std::vector<float> frameMs;                     // 直近のフレーム時間（ImGui DeltaTime）
    std::vector<float> panelCpuMs;                  // 窓の CPU 時間（Render + RenderGpu）
    struct Bench
    {
        bool     active = false;
        bool     value = false;                     // true = 値のみの変更 / false = 構造の変更
        int      edits = 0, done = 0;
        double   intervalSec = 0.4;
        double   nextAt = 0.0;
        double   startedAt = 0.0;
        std::vector<float> frameMs, cpuMs;
        std::vector<double> lat;
    } bench;
    std::string pendingShotPath;                    // 次の RenderGpu でプレビューを撮る（PNG）
    std::string pendingShotPfm;

    // ---- 状態表示 ----
    int         compileSeen = -1;
    uint64_t    valueSeqSeen = 0;
    double      valueBadgeUntil = 0.0;
    double      compileFlashUntil = 0.0;
    uint64_t    nodeStateKey = ~0ull;
    double      now = 0.0;
    int         paletteCascade = 0;
};

State& S();
void InitState(State& s);            // 初期化（MaterialGraphPanel.cpp の Init）
void AfterLoadExternal(State& s);  // 外から作り直した（MCP の new）とき
// プレビュー枠の差し込み（MaterialGraphPanel::SetPreviewDrawer）。空なら空き枠の絵を描く
extern MaterialGraphPanel::PreviewDrawer g_previewDrawer;

void Say(State& s, const std::string& msg, bool bad = false);

// ---- 側面（MaterialGraphPanelSide.cpp）----
void DrawPalettePane(State& s, ImVec2 size);
void DrawDetailsPane(State& s, EditorContext& ctx, ImVec2 size);
// ノードのプロパティ編集の確定（どの入力欄も操作されなくなったら 1 コマンドに）。毎フレーム 1 回、詳細の描画後に呼ぶ。
void FlushLiveEdits(State& s);
void OpenCustomEditor(State& s, ng::NodeId id);
void OpenTexturePicker(State& s, ng::NodeId id, const char* key);
void DrawEditorDialogs(State& s);   // Custom の HLSL 編集・テクスチャ選択（モーダル）

// ---- 下部（MaterialGraphPanelBottom.cpp）----
void DrawBottomPane(State& s, ImVec2 size);
void NextDiagnostic(State& s);
void CopyHlsl(State& s);
// 診断 → ノードの状態表示（赤縁・黄縁・減光）へ
void ApplyNodeStates(State& s);

// ---- プレビュー / GPU 連携（MaterialGraphPanelPreview.cpp）----
void PreviewInit(State& s);                                // Init から 1 回（サムネイル枠の予約・ドロワー・設定の読み込み）
void PreviewAfterLoad(State& s);                           // グラフを開いた / 新規にしたとき（サムネイルと GPU 側を作り直す）
void PreviewUpdate(State& s);                              // 毎フレーム 1 回（Update の後）: 変更の検出・デバウンス・送信・状態・エラーの逆引き
float DrawPreviewPane(State& s, float pad);                // 右カラムのプレビュー枠 + 操作。戻り値 = 使った高さ
bool GpuChip(State& s, std::string& text, ImU32& dot, bool& ok, std::string& tip);   // 「生成中…」等のチップ（GPU が使えるとき）。true = 上書きする

// ---- 共通の小物 ----
// 種別ごとの色（パレットの帯・診断の点）
ImU32 CategoryColor(int slot);
// 直前のアイテムを名前つき要素（仮想入力の imgui_find）として登録
void Anchor(const char* kind, const std::string& label);
// ペインの見出し帯（左に光片・タイトル・右に補助文字）。戻り値 = 見出しの高さ
float PaneHeader(const char* title, const char* right = nullptr);
// 状態チップ（角丸の面 + 点 + 文字）。描いた幅を返す
float StatusChip(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 dot, ImU32 face, ImU32 textCol);

std::string PathLeaf(const std::string& p);

} // namespace dx12e::mgpanel
