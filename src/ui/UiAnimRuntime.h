#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>

#include "animation/SpriteAnimAsset.h"
#include "animation/UiAnimAsset.h"
#include "core/Types.h"

namespace dx12e
{

// .uianim / .spranim の読み込みキャッシュと、ECS への適用。
//
// なぜ UISystem と別クラスなのか:
//   - SpriteAnimator は UI ではなく Sprite2D にも効く（Application::Update から駆動する）ので
//     UISystem（##GameUI 描画時のみ動く）に置くと 2D スプライトに届かない。
//   - エディタのタイムラインはクリップを「再生せずに任意時刻へスクラブ」したい。
//     再生状態と評価を分けておかないとプレビューが書けない。
class UiAnimRuntime
{
public:
    // 発火待ちイベント（クリップ内マーカー / 再生完了）。呼び出し側が EventBus へ流す。
    struct PendingEvent
    {
        entt::entity source = entt::null;
        std::string  name;
    };

    void SetAssetsDir(const std::string& dir) { m_assetsDir = dir; }

    // assets 相対パスでクリップ/シートを取得。読めなければ nullptr。
    // 失敗も負のキャッシュに覚えるので、パスミスがあっても毎フレーム I/O を叩かない。
    const UiAnimClip*      GetClip(const std::string& relPath);
    const SpriteAnimSheet* GetSheet(const std::string& relPath);

    // エディタ保存後のホットリロード用。相対パス指定（空なら全部）。
    void Invalidate(const std::string& relPath = {});

    // Play 開始/停止。playOnStart のものを頭出しし、停止時は再生状態を畳む。
    void OnPlayStart(entt::registry& reg);
    void OnPlayStop(entt::registry& reg);

    // 毎フレーム: UIAnimPlayer / SpriteAnimator を dt だけ進めてコンポーネントへ適用する。
    // outEvents にクリップイベントと完了イベントが積まれる（呼び出し側で Emit）。
    void Update(entt::registry& reg, f32 dt, std::vector<PendingEvent>& outEvents);

    // --- エディタ用（再生状態に触らず、指定時刻の姿勢を一発適用する）---
    // root を起点にトラックの target を解決する。プレビュー/スクラブ専用。
    void ApplyClipAt(entt::registry& reg, entt::entity root, const UiAnimClip& clip, f32 time);

    // トラックの target 名パス（"" = root 自身、"Panel/Title" = 子孫）を解決する。
    // 見つからなければ entt::null。同名の子が複数いる場合は最初に見つかったもの。
    static entt::entity ResolveTarget(const entt::registry& reg, entt::entity root,
                                      const std::string& path);

    // 逆引き: root から見た e の名前パスを作る（タイムラインでトラックを追加する時に使う）。
    // e が root 自身なら ""、root の子孫でなければ false を返す。
    static bool MakeTargetPath(const entt::registry& reg, entt::entity root, entt::entity e,
                               std::string& outPath);

    // 単一プロパティの現在値を読む（Record モードで「今の値」をキーにするため）。
    static f32 ReadProp(const entt::registry& reg, entt::entity e, int prop);

    // 単一プロパティを書く。ApplyClipAt / Update の中身と同じ経路を通る。
    static void WriteProp(entt::registry& reg, entt::entity e, int prop, f32 value);

private:
    // 「読み込み済みか」と「読み込みに失敗したか」を区別する（失敗も覚えて再試行しない）
    struct ClipSlot  { UiAnimClip      data; bool ok = false; };
    struct SheetSlot { SpriteAnimSheet data; bool ok = false; };

    std::string m_assetsDir;
    std::unordered_map<std::string, ClipSlot>  m_clips;
    std::unordered_map<std::string, SheetSlot> m_sheets;

    std::vector<uint8_t> ReadAssetBytes(const std::string& relPath) const;
};

} // namespace dx12e
