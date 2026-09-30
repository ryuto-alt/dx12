#pragma once

// ===========================================================================
// ビューポート専用ツールバー + ビューキューブ（フェーズ 1a）
// ---------------------------------------------------------------------------
// ★ゲーム画面の中には何も重ねない。3D ビューポートの「上端の別帯」として、中央ノードの上に高さ
//   kBarHeight の帯を置き、EditorLayer が 3D の矩形をその分だけ縮める（帯は 3D の外）。
//   ビューキューブも帯の右端に収める＝ゲームの絵にも決定論スクショにも一切写らない。
// ★Play 中も帯は同じ高さで出す（3D の矩形を Editor ⇄ Play で揺らさない）。中身は「Play 中」の表示だけ。
//
// 帯の中身（左から）:
//   移動 / 回転 / 拡縮 / ローカル・ワールド | スナップ（量つきポップオーバー）| ビューモード | 表示フラグ |
//   カメラ（速度・視野角）| アスペクト | ブックマーク 1〜9 | （一時表示: スナップ量・カメラ速度）| ビューキューブ
// 純ロジック（スナップ / アスペクト / キューブの向き / 補間 / ブックマーク / 輪郭）は ViewportLogic.h。
// ===========================================================================

#include "core/Types.h"
#include "editor/ViewportLogic.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <string>

namespace dx12e
{

class EditorContext;
class Camera;
class Scene;

class ViewportToolbar
{
public:
    static constexpr f32 kBarHeight = 38.0f;   // 論理 px（100% 表示）

    // 帯の高さ（物理 px）。帯を隠している時は 0（＝従来と同じ 3D の矩形）。
    static f32 HeightPx(const EditorContext& ctx);

    // プロジェクトの assets フォルダ（ブックマークの保存先 <project>/.dx12/viewport_bookmarks.txt の決定に使う）。
    void SetAssetsDir(const std::string& assetsDir);

    // 毎フレーム 1 回（帯を描く前）。カメラ補間の駆動・ブックマークの保存 / 呼び出し・
    // スナップ量 / カメラ速度の一時表示（HUD）の更新。
    void Update(EditorContext& ctx, Camera* camera, Scene* scene, bool isPlaying, f32 dt);

    // 帯の描画。nodePos / nodeW = 中央ノードの上端と幅（帯はここから barH の高さ）。
    void Render(EditorContext& ctx, Camera* camera, Scene* scene, bool& physicsDebugDraw, bool isPlaying,
                ImVec2 nodePos, f32 nodeW, f32 barH);

    // ビューキューブのクリックなどでなめらかに動かしている最中か（手動操作が始まったら止める判定用）
    bool IsTweening() const { return m_tween.running; }

private:
    void DrawViewCube(EditorContext& ctx, Camera* camera, Scene* scene, ImVec2 cubeMin, f32 size);
    void GoToFace(EditorContext& ctx, Camera* camera, Scene* scene, vp::CubeFace face);
    void SaveBookmark(EditorContext& ctx, Camera* camera, int slot);
    void JumpBookmark(EditorContext& ctx, Camera* camera, int slot);
    void LoadBookmarksIfNeeded();
    void PersistBookmarks() const;
    vp::CameraPose CurrentPose(const Camera* camera) const;
    static void ApplyPose(Camera* camera, const vp::CameraPose& p);
    void Hud(EditorContext& ctx, const std::string& text, f32 seconds = 1.6f) const;

    vp::PoseTween    m_tween;
    vp::BookmarkSet  m_bookmarks;
    std::string      m_bookmarkPath;
    bool             m_bookmarksLoaded = false;

    // 変化検出（HUD の一時表示用）
    f32 m_lastSpeed = -1.0f;
    f32 m_lastSnapT = -1.0f, m_lastSnapR = -1.0f, m_lastSnapS = -1.0f;
    bool m_lastSnapAlways = false;
    bool m_haveLast = false;
    int  m_hoverFace = -1;
};

} // namespace dx12e
