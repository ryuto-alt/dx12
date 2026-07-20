#pragma once

#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "animation/UiAnimAsset.h"
#include "core/Types.h"

namespace dx12e
{

class EditorContext;
class UiAnimRuntime;

// UI アニメーション(.uianim)のタイムラインエディタ。
// Unity の Animation ウィンドウ / Unreal のシーケンサ相当を最小構成で実装したもの:
//   - 左: トラック一覧（対象パス + プロパティ）
//   - 右: タイムライン（キーのドラッグ / 追加 / 削除 / イージング指定 / 再生ヘッド）
//   - 下: 選択キーの数値編集 + 区間カーブのプレビュー
//
// 「対象ルート」に選んだエンティティを起点に、トラックの target（名前パス）で子孫を指す。
// スクラブ・再生中は実際のコンポーネント値を書き換えるので、UIエディタと3Dビューの
// プレビューにそのまま反映される（= 見たままが保存される）。
//
// 破壊的な点として、スクラブすると対象のコンポーネント値が動く。パネルは開いた時点の
// 値をスナップショットしておき、閉じる/対象を変える/「元に戻す」を押した時に書き戻す。
class AnimationEditorPanel
{
public:
    // ImGui ウィンドウ本体。ctx.showAnimEditor が false なら即 return。
    // runtime はクリップのキャッシュ無効化（保存後のホットリロード）に使う。null 可。
    void RenderWindow(entt::registry& reg, EditorContext& ctx, const std::string& assetsDir,
                      UiAnimRuntime* runtime);

private:
    // --- アセット IO（VfxEditorPanel と同じ New/Load/Save の3点セット）---
    void RefreshAssetList(const std::string& assetsDir);
    void NewAsset();
    bool LoadAsset(const std::string& path);
    bool SaveAsset(const std::string& path, UiAnimRuntime* runtime);

    // --- 描画パーツ ---
    void DrawToolbar(entt::registry& reg, EditorContext& ctx, const std::string& assetsDir,
                     UiAnimRuntime* runtime);
    void DrawTrackList(entt::registry& reg, EditorContext& ctx);
    void DrawTimeline(entt::registry& reg, EditorContext& ctx);
    void DrawKeyInspector();
    void DrawAddTrackPopup(entt::registry& reg, EditorContext& ctx);

    // --- 編集操作 ---
    void PushUndo();                 // 変更前に呼ぶ（クリップ丸ごとのスナップショット）
    void Undo();
    void Redo();
    // トラック t の time 位置へ「対象の現在値」をキーとして打つ。既存キーがあれば上書き。
    void SetKeyAt(entt::registry& reg, size_t trackIdx, f32 time, f32 value);
    // 全トラックを time で評価して対象へ適用する（スクラブ）。
    void ApplyAtTime(entt::registry& reg, f32 time);
    // 録画中: 対象の現在値がクリップの評価値とずれていたらキーを打つ。
    void CaptureRecordedEdits(entt::registry& reg);

    // 対象ルートのスナップショット（スクラブで壊した値を戻すため）。
    void SnapshotTargets(entt::registry& reg);
    void RestoreTargets(entt::registry& reg);

    // --- 状態 ---
    UiAnimClip  m_clip;
    std::string m_currentPath;        // 空 = 未保存の新規クリップ
    std::vector<std::string> m_assetNames;
    bool m_assetListLoaded = false;
    char m_nameBuf[128] = "NewUiAnim";

    entt::entity m_root = entt::null;  // 対象ルート（トラックの target を解決する起点）

    f32  m_time     = 0.0f;           // 再生ヘッド位置（秒）
    bool m_playing  = false;
    bool m_recording = false;
    f32  m_playSpeed = 1.0f;
    f32  m_zoom      = 1.0f;          // 横方向ズーム（1.0 = クリップ全体が窓幅に収まる）
    f32  m_scroll    = 0.0f;          // 横スクロール（秒）
    f32  m_snapStep  = 0.05f;         // キー/再生ヘッドのスナップ間隔（秒。0 = スナップ無し）

    int m_selTrack = -1;              // 選択中のトラック（-1 = 無し）
    int m_selKey   = -1;              // 選択中のキー（m_selTrack 内の index）
    int m_dragTrack = -1;             // ドラッグ中のキー
    int m_dragKey   = -1;
    bool m_draggingPlayhead = false;

    // クリップ丸ごとの Undo（ECS の UndoSystem とは別勘定。パネル内 Ctrl+Z 専用）。
    // クリップは大きくても数 KB なのでスナップショット方式で十分。
    std::vector<UiAnimClip> m_undoStack;
    std::vector<UiAnimClip> m_redoStack;
    static constexpr size_t kUndoLimit = 64;

    // スクラブで書き換える前の値（対象エンティティ × プロパティ）。
    struct PropSnapshot { entt::entity e = entt::null; int prop = 0; f32 value = 0.0f; };
    std::vector<PropSnapshot> m_snapshot;
    bool m_snapshotValid = false;

    std::string m_statusMsg;
    f32 m_statusFlash = 0.0f;
};

} // namespace dx12e
