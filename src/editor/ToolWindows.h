#pragma once

// ===== ツール窓レジストリ（窓の「唯一の正」）=====
// 以前は「表示」メニュー / 「ツール」メニュー / ツールバーの「窓▾」の 3 か所に別々の一覧があり、
// 内容が食い違っていた（UIエディタは「窓▾」にしか無い、「すべて閉じる」が窓を取りこぼす、等）。
// 今はこの表 1 つから全部を作る:
//   ・メニュー「表示」「ツール」とツールバー「窓▾」の項目（ToolbarPanel）
//   ・「すべて閉じる」「レイアウトをリセット」の対象（漏れない）
//   ・ドックの既定配置（EditorLayer::BuildDefaultLayout）
//   ・コマンドパレットの window.* コマンド
//   ・UI 自動テストの窓一覧
// 窓を足すときは (1) EditorContext に show* を足す (2) ここに 1 行足す。それだけで全部に出る。
//
// ドックの方針（ビューポートを縮めない）:
//   RightTab の窓は右カラムの「インスペクター」と同じタブ群に入る。窓の開閉でドックの分割は一切変えない
//   （以前は開閉のたびに右カラムを縦に割り直しており、比の丸め誤差が積もってビューポートが縮んでいった）。
//   Floating の窓はビューポートの上に浮かぶ独立窓（NoDocking）。

#include "editor/EditorContext.h"
#include "editor/EditorIcons.h"

#include <cstddef>

namespace dx12e::tools
{

enum class DockSlot : unsigned char
{
    RightTab,   // 右カラム（インスペクターのタブ群）へ。開くとタブが増えるだけで分割は変えない
    Floating,   // ビューポートの上に浮かぶ独立窓
    // ---- 以下はレジストリの既定にはしない。ユーザーが「ツール窓の配置先」で選ぶ配置先（editor/WorkspaceLogic.h の ws::Slot と同じ 4 択）----
    RightSplit, // 右カラムを縦に割った下側（インスペクターと並べて見る）
    BottomTab,  // 中央下のドック（アセットブラウザ / コンソールのタブ群）
};

// メニューのどちらに置くか（「窓▾」には全部出る）。
enum class MenuHome : unsigned char { View, Tools };

struct Desc
{
    const char*        id;         // "postProcess"（コマンド window.postProcess / 一意な識別子）
    const char*        title;      // メニュー表示（日本語主）
    const char*        imguiName;  // ImGui 上の窓名（"表示名###固定ID" 形式なら ### を含む）。UI 自動テスト / ドック配置用
    const char*        icon;       // ICON_*
    const char*        category;   // メニューの見出し
    bool EditorContext::* flag;    // 開閉フラグ
    DockSlot           slot;
    MenuHome           home;
    const char*        keywords;   // パレット検索用の別名（英語など）
};

inline constexpr Desc kAll[] = {
    // ---- レンダリング ----
    {"postProcess",    "Post Process",             "Post Process",            ICON_T_LAYERS,  "レンダリング", &EditorContext::showPostProcess,       DockSlot::RightTab, MenuHome::View,  "ポストプロセス bloom tonemap"},
    {"postParams",     "Post Process パラメータ",  "Post Process パラメータ", ICON_T_SLIDERS, "レンダリング", &EditorContext::showPostParams,        DockSlot::RightTab, MenuHome::View,  "post process parameters"},
    {"skybox",         "Skybox / IBL",             "Skybox / IBL",            ICON_T_SUN,     "レンダリング", &EditorContext::showSkybox,            DockSlot::RightTab, MenuHome::View,  "空 環境光 sky ibl"},
    {"ssao",           "SSAO",                     "SSAO",                    ICON_T_GRID,    "レンダリング", &EditorContext::showSSAO,              DockSlot::RightTab, MenuHome::View,  "ambient occlusion"},
    {"ssr",            "SSR / SSGI",               "SSR / SSGI",              ICON_T_MONITOR, "レンダリング", &EditorContext::showScreenSpaceGi,     DockSlot::RightTab, MenuHome::View,  "反射 reflection gi"},
    {"fog",            "Volumetric Fog",           "Volumetric Fog",          ICON_T_FOG,     "レンダリング", &EditorContext::showVolumetricFog,     DockSlot::RightTab, MenuHome::View,  "霧 フォグ"},
    {"lighting",       "ライティング",             "",                        ICON_T_LIGHT,   "レンダリング", &EditorContext::showLighting,          DockSlot::Floating, MenuHome::View,  "lighting sun shadow 太陽 影"},
    // ---- プロジェクト・システム ----
    {"engineSettings", "エンジン設定",             "エンジン設定",            ICON_SETTINGS,  "プロジェクト", &EditorContext::showEngineSettings,    DockSlot::RightTab, MenuHome::View,  "engine settings 設定"},
    {"buildSettings",  "ビルド設定",               "ビルド設定",              ICON_HAMMER,    "プロジェクト", &EditorContext::showBuildSettings,     DockSlot::RightTab, MenuHome::View,  "build 配布 パッケージ"},
    {"sceneFlow",      "Scene Flow",               "Scene Flow",              ICON_T_SPLINE,  "プロジェクト", &EditorContext::showSceneFlow,         DockSlot::RightTab, MenuHome::View,  "シーン遷移"},
    {"project",        "Project",                  "Project",                 ICON_FOLDER,    "プロジェクト", &EditorContext::showProject,           DockSlot::RightTab, MenuHome::View,  "プロジェクト"},
    {"git",            "Git 変更",                 "Git 変更###Version Control (Git)", ICON_GIT_BRANCH, "プロジェクト", &EditorContext::showVersionControl, DockSlot::RightTab, MenuHome::View, "git version control コミット"},
    // ---- 制作ツール ----
    {"terrain",        "地形ツール",               "",                        ICON_T_TERRAIN, "制作ツール",   &EditorContext::showTerrainEditor,     DockSlot::Floating, MenuHome::Tools, "terrain heightfield 山 地面"},
    {"foliage",        "植生ツール",               "",                        ICON_T_TERRAIN, "制作ツール",   &EditorContext::showFoliageTool,       DockSlot::Floating, MenuHome::Tools, "foliage vegetation grass tree 植生 草 木 散布 ブラシ 風"},
    {"sculpt",         "スカルプト",               "",                        ICON_T_MESH,    "制作ツール",   &EditorContext::showSculptEditor,      DockSlot::Floating, MenuHome::Tools, "sculpt 彫刻 異形"},
    {"navmesh",        "ナビメッシュ",             "",                        ICON_T_NAV,     "制作ツール",   &EditorContext::showNavMesh,           DockSlot::Floating, MenuHome::Tools, "navmesh ai 経路"},
    {"pathTracer",     "リファレンスレンダー",     "",                        ICON_CAMERA,    "レンダリング", &EditorContext::showPathTracer,        DockSlot::Floating, MenuHome::Tools, "path tracer reference ground truth パストレーサー 地上真値 基準画像 レイトレ"},
    {"particle",       "パーティクルエディタ",     "",                        ICON_T_PARTICLE,"制作ツール",   &EditorContext::showVfxEditor,         DockSlot::Floating, MenuHome::Tools, "particle vfx effect エフェクト"},
    {"uiEditor",       "UIエディタ",               "",                        ICON_T_UI,      "制作ツール",   &EditorContext::showUiEditor,          DockSlot::Floating, MenuHome::Tools, "ui editor canvas"},
    {"uiAnim",         "UIアニメーション",         "",                        ICON_T_ANIM,    "制作ツール",   &EditorContext::showAnimEditor,        DockSlot::Floating, MenuHome::Tools, "ui animation timeline"},
    {"spriteSheet",    "スプライトシート",         "",                        ICON_T_SPRITE,  "制作ツール",   &EditorContext::showSpriteSheetEditor, DockSlot::Floating, MenuHome::Tools, "sprite sheet"},
    {"transition",     "トランジション",           "トランジション",          ICON_FILM,      "制作ツール",   &EditorContext::showTransitionPreview, DockSlot::Floating, MenuHome::Tools, "transition シーン切り替え"},
    {"material",       "マテリアルエディタ",       "",                        ICON_T_MATERIAL,"制作ツール",   &EditorContext::showMaterialEditor,    DockSlot::Floating, MenuHome::Tools, "material editor"},
    {"materialLib",    "マテリアルライブラリ (Poly Haven)", "",               ICON_PACKAGE,   "制作ツール",   &EditorContext::showMaterialLibrary,   DockSlot::Floating, MenuHome::Tools, "material library polyhaven"},
    {"materialGraph",  "マテリアルグラフ",         "マテリアルグラフ###MaterialGraph", ICON_T_SPLINE, "制作ツール", &EditorContext::showMaterialGraph,  DockSlot::Floating, MenuHome::Tools, "material graph node shader dxmg マテリアル グラフ ノード シェーダー"},
    {"nodeGraphSandbox", "ノードグラフ サンドボックス", "ノードグラフ サンドボックス###NodeGraphSandbox", ICON_T_SPLINE, "制作ツール", &EditorContext::showNodeGraphSandbox, DockSlot::Floating, MenuHome::Tools, "node graph sandbox ノード ワイヤ material graph 開発用"},
    {"audioMixer",     "オーディオミキサー",       "",                        ICON_T_AUDIO,   "制作ツール",   &EditorContext::showAudioMixer,        DockSlot::Floating, MenuHome::Tools, "audio mixer 音"},
    // ---- 接続・診断 ----
    {"mcp",            "MCP / AI Bridge",          "MCP / AI Bridge",         ICON_CLOUD,     "接続・診断",   &EditorContext::showMcpBridge,         DockSlot::RightTab, MenuHome::Tools, "mcp ai bridge"},
    {"network",        "Network",                  "Network",                 ICON_T_NET,     "接続・診断",   &EditorContext::showNetworkStatus,     DockSlot::RightTab, MenuHome::Tools, "multiplayer マルチプレイ"},
    {"networkSettings","Network 設定",             "Network 設定",            ICON_SETTINGS,  "接続・診断",   &EditorContext::showNetworkSettings,   DockSlot::RightTab, MenuHome::Tools, "network settings"},
    {"hardware",       "ハードウェア",             "ハードウェア###HardwareFloating", ICON_CPU, "接続・診断", &EditorContext::showHardware,       DockSlot::Floating, MenuHome::Tools, "hardware arduino esp32 シリアル 書き込み flash ボード"},
    {"diagnostics",    "エンジン診断 (UI 自動テスト)", "",                    ICON_BUG,       "接続・診断",   &EditorContext::showEngineDiagnostics, DockSlot::Floating, MenuHome::Tools, "diagnostics ui test"},
};
inline constexpr size_t kCount = sizeof(kAll) / sizeof(kAll[0]);

inline const Desc* Find(const char* id)
{
    for (const Desc& d : kAll)
    {
        const char* a = d.id; const char* b = id;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == *b) return &d;
    }
    return nullptr;
}

inline bool IsOpen(const EditorContext& ctx, const Desc& d) { return ctx.*(d.flag); }

// 開いている窓が 1 つでもあるか（「窓▾」ボタンの強調表示に使う）
inline bool AnyOpen(const EditorContext& ctx)
{
    for (const Desc& d : kAll) if (ctx.*(d.flag)) return true;
    return false;
}

// 全部閉じる（取りこぼしが無い。表に載っている窓すべて）
inline void CloseAll(EditorContext& ctx)
{
    for (const Desc& d : kAll) ctx.*(d.flag) = false;
}

} // namespace dx12e::tools
