#pragma once

#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <imgui.h>                  // UiResolvedRect の ImVec2（値メンバのため完全型が必要）

struct ImDrawList;                  // ImGui（描画先 DrawList）
struct ID3D12GraphicsCommandList;   // UIImage テクスチャの遅延ロード用

namespace dx12e
{
class ResourceManager;
class DescriptorHeap;
class EventBus;

// クリック確定（release-inside）した UIButton / 値が変わった UISlider / UIToggle の保留イベント。
// Render 中に積み、次フレームの Update 冒頭（Lua OnUpdate より前）で EventBus へ流す。
struct UIPendingClick
{
    std::string  eventName;               // UIButton::onClickEvent / UISlider・UIToggle::onChangeEvent
    entt::entity source = entt::null;     // ウィジェットのエンティティ
    bool   hasValue = false;              // true なら emit 時に data.value を載せる
    double value    = 0.0;                // スライダー=実値、トグル= 1/0
};

// ゲームパッド/キーボードの UI ナビゲーション入力（1 フレームぶんの「押しっぱなし」状態）。
// Application がキーボード(矢印/Enter/Space)と XInput(D-pad/左スティック/A ボタン)をまとめて
// 渡す。エッジ検出・キーリピート・フォーカス管理は UISystem 側で行う。
struct UiNavInput
{
    bool left = false, right = false, up = false, down = false;
    bool confirm = false;   // 決定（Enter / Space / パッド A）
};

// エディタ支援用: レイアウト解決済みの UIRect 1 件（スクリーン座標）。
// ResolveRects が描画順（= 奥→手前。後方ほど手前）で出力する。
// canvasScale/canvasOrigin は「スクリーンΔ → キャンバス空間 px」の換算
// （canvasPx = (screen - canvasOrigin) / canvasScale）に使う。
struct UiResolvedRect
{
    entt::entity e = entt::null;             // UIRect を持つエンティティ
    ImVec2       min{0.0f, 0.0f};            // 矩形 min（スクリーン座標）
    ImVec2       max{0.0f, 0.0f};            // 矩形 max（スクリーン座標）
    float        canvasScale = 1.0f;         // キャンバス空間 → スクリーンの倍率
    ImVec2       canvasOrigin{0.0f, 0.0f};   // キャンバス原点（スクリーン座標）
    entt::entity canvas = entt::null;        // 属する UICanvas エンティティ
};

// ゲーム内 retained-mode UI（UICanvas / UIRect / UIImage / UIText / UIButton）の
// レイアウト解決・描画・ボタン入力を担う（Play / ゲームモード中のみ駆動される）。
// - レイアウト: UICanvas を sortOrder 昇順に走査し、子孫（Transform::parent 階層）を
//   親→子の DFS で解決する。UIRect の解決式は Components.h のコメント参照。
// - 描画: 既存 ##GameUI ウィンドウの DrawList へ描く（ImGui 経路）。GPU リソースは
//   一切保持せず、テクスチャは ResourceManager のキャッシュ + 共有 SRV ヒープを借りる。
// - 入力: 最前面（topmost）のレイキャスト対象（interactable な UIButton / raycastBlock=true の
//   UIImage）だけがクリック/ホバーを受け、Transform::parent を遡って親ボタンへバブリングする
//   （Unity uGUI 方式）。UIButton の _hovered/_pressed を更新し、release-inside でクリック確定。
class UISystem
{
public:
    // ##GameUI ウィンドウ内（Begin 後・PushClipRect 済み）から毎フレーム呼ぶ。
    // ox/oy = ゲームビューポート左上（スクリーン座標、メインビューポート原点込み）、
    // vw/vh = ゲームビューポートのピクセルサイズ。
    // resources/srvHeap/cmdList は UIImage テクスチャの遅延ロード用
    // （エディタアイコンと同じ「SRV index → GPU ハンドル(u64) = ImTextureID」経路）。
    // nav はゲームパッド/キーボードのフォーカスナビゲーション入力（省略 = ナビ無効）。
    // 方向でフォーカス移動（フォーカス中のスライダーは左右で値変更）、決定でクリック確定。
    // フォーカスリングは UI の最前面に描かれる。マウスクリックでもフォーカスは移る。
    void RenderAndUpdateInput(entt::registry& reg, ImDrawList* dl,
                              float ox, float oy, float vw, float vh,
                              ResourceManager* resources, DescriptorHeap* srvHeap,
                              ID3D12GraphicsCommandList* cmdList,
                              const UiNavInput& nav = {});

    // エディタの SceneView 用プレビュー描画（非 Play 時）。レイアウト解決＋描画のみで
    // 入力処理を一切行わない: ボタンは normalColor 固定、_hovered/_pressed は読み書き
    // せず、クリックキューにも積まない。内部状態を使わないため static（インスタンス不要）。
    // 引数の意味は RenderAndUpdateInput と同じ。
    static void RenderPreview(entt::registry& reg, ImDrawList* dl,
                              float ox, float oy, float vw, float vh,
                              ResourceManager* resources, DescriptorHeap* srvHeap,
                              ID3D12GraphicsCommandList* cmdList);

    // エディタ支援用: 描画せずレイアウト解決のみ行い、UIRect を持つ全要素（ボタンに
    // 限らない）の解決済みスクリーン矩形を描画順（= 奥→手前）で out に積む（out は
    // 冒頭でクリア）。SceneView での UI ピッキングやハンドル描画のヒットテストに使う。
    static void ResolveRects(entt::registry& reg,
                             float ox, float oy, float vw, float vh,
                             std::vector<UiResolvedRect>& out);

    // 前フレームの Render で確定したクリックを EventBus へ即時 Emit する。
    // Application::Update の Lua OnUpdate 呼び出しより前に呼ぶこと
    // （既存の即時 UI ボタンと同じ 1 フレーム遅延・フレーム境界配信）。
    void DispatchPendingClicks(entt::registry& reg, EventBus& bus);

    // 保留クリックと全 UIButton のランタイム状態を破棄（Play 終了・ランタイムシーン切替時）。
    void ResetRuntimeState(entt::registry& reg);

    // このフレームで鳴らすべき UI 効果音（UIButton::hoverSound/clickSound、assets 相対パス）を
    // 取り出す（内部リストは空になる）。呼び出し元（Application）が AudioSystem::PlaySFX へ流す。
    // UISystem は AudioSystem に依存しない（描画/入力と同じ「収集→呼び出し元が配信」方式）。
    std::vector<std::string> TakePendingSfx() { return std::move(m_pendingSfx); }

private:
    std::vector<UIPendingClick> m_pendingClicks;
    std::vector<std::string>    m_pendingSfx;

    // ---- フォーカスナビゲーション（ランタイム状態）----
    entt::entity m_focused = entt::null;   // 現在フォーカス中のウィジェット
    UiNavInput   m_prevNav;                // 前フレームの nav（エッジ検出用）
    float        m_navRepeatT = 0.0f;      // 方向ホールドのリピートタイマー（秒）
    int          m_navHeldDir = -1;        // ホールド中の方向 0=左 1=右 2=上 3=下（-1=なし）
    bool         m_confirmHeld = false;    // フォーカス上で決定を押下中（離しで確定）
};

} // namespace dx12e
