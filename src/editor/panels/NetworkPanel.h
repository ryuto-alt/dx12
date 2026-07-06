#pragma once
#include <entt/entt.hpp>

namespace dx12e
{

class NetworkSystem;
class EditorContext;

// マルチプレイの状態モニタパネル（エディタ専用・読み取り専用）。
// ロール/tick・複製エンティティ数・接続一覧(RTT/送受信バイト概算)を表示する。
// 設定(ポート/tickRate等)は NetworkSettingsPanel(フェーズ⑨)側。
class NetworkPanel
{
public:
    // ツール窓トグル（ctx.showNetworkStatus）が ON のとき毎フレーム呼ぶ。
    static void Render(NetworkSystem& net, entt::registry& reg, EditorContext& ctx);
};

} // namespace dx12e
