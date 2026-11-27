#pragma once

// ステータスバー（W1-A7 / GG-4）。Graphite のときだけ EditorLayer::RenderStatusBar が呼ぶ（旧テーマは従来の描画のまま）。
//   左: 状態（編集 / 再生中 / 一時停止）・「準備完了」または主ジョブの細型進捗（黄の指針 = 予算 08。呼び出しは StatusBar.cpp の 1 か所）・選択名
//   右: FPS / ms / VRAM のミニゲージ、MEM、obj / draws / culled、カメラ速度、DX12 · GPU、図番 UN-x.y.z
//   幅が足りないときは優先度の低い項目から落とす。--ui-deterministic では実測の値を「—」にして UI ゴールデンを揺らさない。

#include <entt/entt.hpp>

struct ID3D12Device;

namespace dx12e
{
class Scene;
class Camera;
class GameClock;
class EditorContext;
} // namespace dx12e


namespace dx12e::status
{

struct Input
{
    EditorContext* ctx = nullptr;
    Scene*         scene = nullptr;
    Camera*        camera = nullptr;
    GameClock*     clock = nullptr;
    bool           isPlaying = false;
};

// VRAM / GPU 名の問い合わせ先（EditorLayer::Render が cmdList->GetDevice で得たものを 1 度だけ渡す。所有しない）。
void SetDevice(ID3D12Device* device);

// 帯の高さ（物理 px）。Graphite は GgLayout::statusBarH、それ以外は kFallbackLogical を呼び出し側が使う。
float GraphiteHeightPx();

// ステータスバーの窓を描く（Graphite）。EditorLayer と同じ位置・フラグの ##StatusBar。
void RenderGraphite(const Input& in);

// カメラ速度ポップアップの中身（旧テーマの帯と共通）。BeginPopup の中で呼ぶ。
void CameraQuickPopupBody(Camera* camera, entt::registry& reg);

} // namespace dx12e::status
