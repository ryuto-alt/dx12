#pragma once

#include <string>
#include <vector>

#include <entt/entt.hpp>

struct ImDrawList;                  // ImGui（描画先 DrawList）
struct ID3D12GraphicsCommandList;   // UIImage テクスチャの遅延ロード用

namespace dx12e
{
class ResourceManager;
class DescriptorHeap;
class EventBus;

// クリック確定（release-inside）した UIButton の保留イベント。
// Render 中に積み、次フレームの Update 冒頭（Lua OnUpdate より前）で EventBus へ流す。
struct UIPendingClick
{
    std::string  eventName;               // UIButton::onClickEvent
    entt::entity source = entt::null;     // ボタンのエンティティ
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
    void RenderAndUpdateInput(entt::registry& reg, ImDrawList* dl,
                              float ox, float oy, float vw, float vh,
                              ResourceManager* resources, DescriptorHeap* srvHeap,
                              ID3D12GraphicsCommandList* cmdList);

    // 前フレームの Render で確定したクリックを EventBus へ即時 Emit する。
    // Application::Update の Lua OnUpdate 呼び出しより前に呼ぶこと
    // （既存の即時 UI ボタンと同じ 1 フレーム遅延・フレーム境界配信）。
    void DispatchPendingClicks(entt::registry& reg, EventBus& bus);

    // 保留クリックと全 UIButton のランタイム状態を破棄（Play 終了・ランタイムシーン切替時）。
    void ResetRuntimeState(entt::registry& reg);

private:
    std::vector<UIPendingClick> m_pendingClicks;
};

} // namespace dx12e
