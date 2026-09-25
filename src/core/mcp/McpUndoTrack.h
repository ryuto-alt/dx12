#pragma once

// MCP の編集を「値のスナップショット」で Undo にする部品。
//
// ■ 使い方（ハンドラ側）
//     const auto e = ResolveMcpEntity(*m_scene, params);
//     McpUndo().Track<Transform>(e);   // ← 書き換える前に「どのコンポーネントを触るか」を申告
//     reg.get<Transform>(e).position = ...;
//   ディスパッチ（HandleMcpCommand）が呼び出しの前後で Begin / Finish を呼び、
//   値が実際に変わったエンティティだけを 1 個の Undo コマンドにまとめる（McpUndoRouter へ渡す）。
//
// ■ なぜ値のコピーか（JSON の往復にしない理由）
//   set_component は「消して作り直す」実装で、カメラは作り直すと isActive の重複防止が走る。
//   JSON で戻すと同じ副作用が戻す側でも起きて、戻したつもりが別の値になる。値のコピーなら
//   ビット単位で元に戻る。副作用の要る型（MeshRenderer の UV 焼き込み / LuaScript の実行状態）
//   だけ McpRestore を特殊化してある。
//
// ■ エンティティは guid でも引き直す
//   生成の取り消し → やり直し（SpawnEntityCommand::Redo）や削除の取り消しでは entity id が
//   変わる。id だけで持つと、同じエントリ（トランザクション）の後ろの編集が「消えた id」を
//   指して何もしなくなる（エディタの既存コマンドの既知の弱点）。追跡を始めた時点で
//   EntityGuid を確定させ、適用時に id が無効なら guid で引き直す。
//
// ■ 追跡しないもの（二重に積まないため）
//   地形 / スカルプトのストローク・グループ化・生成・削除・複製は既存のコマンドが自分で積み、
//   それを McpUndoRouter が横取りしてまとめる。ここで同じ物を追跡してはいけない。

#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>
#include <entt/entt.hpp>
#include "core/Types.h"
#include "editor/UndoCore.h"

namespace dx12e
{

class Scene;
struct MeshRenderer;
struct LuaScript;
struct Transform;

// 値を当てる。既定はそのまま置き換え。副作用が要る型だけ特殊化（定義は McpUndoTrack.cpp）。
template <class T>
struct McpRestore
{
    static void Apply(Scene& /*scene*/, entt::registry& reg, entt::entity e, const T& v)
    {
        reg.emplace_or_replace<T>(e, v);
    }
    static void Remove(entt::registry& reg, entt::entity e) { reg.remove<T>(e); }
};
template <> struct McpRestore<MeshRenderer>
{
    static void Apply(Scene& scene, entt::registry& reg, entt::entity e, const MeshRenderer& v);
    static void Remove(entt::registry& reg, entt::entity e);
};
template <> struct McpRestore<LuaScript>
{
    static void Apply(Scene& scene, entt::registry& reg, entt::entity e, const LuaScript& v);
    static void Remove(entt::registry& reg, entt::entity e);
};

// 1 エンティティ × 1 コンポーネント型の前後の値。
struct IMcpUndoSlot
{
    virtual ~IMcpUndoSlot() = default;
    virtual const void* TypeTag() const = 0;
    virtual void CaptureAfter(entt::registry& reg, entt::entity e) = 0;
    virtual void Apply(Scene& scene, entt::registry& reg, entt::entity e, bool after) = 0;
};

// Transform.parent を guid でも持つためのフック（Transform 以外は何もしない）。
// 親に guid が無ければここで振る（振らないと、親が生成の取り消し→やり直しで作り直されたとき
// 引き直せずに親子が外れる）。
void McpUndoCaptureParentGuid(entt::registry& reg, const Transform& t, u64& outGuid);
void McpUndoFixParent(const entt::registry& reg, Transform& t, u64 parentGuid);

template <class T>
struct McpUndoSlot final : IMcpUndoSlot
{
    static_assert(std::is_copy_constructible_v<T>,
                  "McpUndo().Track<T>: T は値でコピーできる型に限る（unique_ptr を持つ型は追跡できない）");

    std::optional<T> before, after;
    u64 parentGuidBefore = 0, parentGuidAfter = 0;   // T == Transform のときだけ使う

    static const void* Tag() { static const char tag = 0; return &tag; }
    const void* TypeTag() const override { return Tag(); }

    void CaptureBefore(entt::registry& reg, entt::entity e)
    {
        if (const T* v = reg.try_get<T>(e))
        {
            before = *v;
            if constexpr (std::is_same_v<T, Transform>) McpUndoCaptureParentGuid(reg, *v, parentGuidBefore);
        }
    }
    void CaptureAfter(entt::registry& reg, entt::entity e) override
    {
        if (const T* v = reg.try_get<T>(e))
        {
            after = *v;
            if constexpr (std::is_same_v<T, Transform>) McpUndoCaptureParentGuid(reg, *v, parentGuidAfter);
        }
    }
    void Apply(Scene& scene, entt::registry& reg, entt::entity e, bool useAfter) override
    {
        const std::optional<T>& v = useAfter ? after : before;
        if (!v)
        {
            if (reg.all_of<T>(e)) McpRestore<T>::Remove(reg, e);
            return;
        }
        if constexpr (std::is_same_v<T, Transform>)
        {
            T fixed = *v;
            McpUndoFixParent(reg, fixed, useAfter ? parentGuidAfter : parentGuidBefore);
            McpRestore<T>::Apply(scene, reg, e, fixed);
        }
        else
        {
            McpRestore<T>::Apply(scene, reg, e, *v);
        }
    }
};

// シーン全体の設定（ポスト等）1 個ぶんの前後の値。Scene のメンバを指すポインタで持つ
// （Scene 自体はシーンを開き直しても同じ物で、Undo 履歴はシーン読み込みで消えるので寿命が合う）。
struct IMcpSceneSlot
{
    virtual ~IMcpSceneSlot() = default;
    virtual const void* Ptr() const = 0;
    virtual void CaptureAfter() = 0;
    virtual void Apply(bool after) = 0;
};

template <class T>
struct McpSceneSlot final : IMcpSceneSlot
{
    T* ptr = nullptr;
    T  before{}, after{};
    const void* Ptr() const override { return ptr; }
    void CaptureAfter() override { after = *ptr; }
    void Apply(bool useAfter) override { *ptr = useAfter ? after : before; }
};

class McpUndoTracker
{
public:
    McpUndoTracker();
    ~McpUndoTracker();
    McpUndoTracker(const McpUndoTracker&) = delete;
    McpUndoTracker& operator=(const McpUndoTracker&) = delete;

    // 呼び出し 1 回ぶんの開始 / 終了（HandleMcpCommand が呼ぶ）。
    void Begin(Scene* scene, std::string assetsDir);
    // 値が変わったエンティティ / シーン設定だけを Undo コマンドにして返す（無ければ空）。追跡は終わる。
    std::vector<std::unique_ptr<IUndoCommand>> Finish();
    bool Active() const { return m_scene != nullptr; }

    // 書き換える前に呼ぶ。同じ (エンティティ, 型) を 2 回呼んでも最初の値だけを持つ。
    // 追跡していない（Editor 以外 / MCP の外）ときは何もしない＝ハンドラは気にせず呼んでよい。
    template <class T>
    void Track(entt::entity e)
    {
        if (!Active()) return;
        Rec* rec = RecordFor(e);
        if (!rec) return;
        for (const auto& s : rec->slots)
            if (s->TypeTag() == McpUndoSlot<T>::Tag()) return;
        auto slot = std::make_unique<McpUndoSlot<T>>();
        slot->CaptureBefore(Registry(), e);
        rec->slots.push_back(std::move(slot));
    }

    // シーン全体の設定（m_scene->GetPostSettings() など）を書き換える前に呼ぶ。
    // 変わったかは SceneSettingsFingerprint で比べる（未保存判定と同じ物差し）。
    template <class T>
    void TrackSceneValue(T& ref)
    {
        if (!Active()) return;
        if (!m_sceneFpTaken) { m_sceneFpBefore = SceneFingerprint(); m_sceneFpTaken = true; }
        for (const auto& s : m_sceneSlots)
            if (s->Ptr() == &ref) return;
        auto slot = std::make_unique<McpSceneSlot<T>>();
        slot->ptr    = &ref;
        slot->before = ref;
        m_sceneSlots.push_back(std::move(slot));
    }

    // set_component / remove_component の jsonKey から型を引いて Track する（McpUndoTrack.cpp）。
    // 知らないキーなら false（ハンドラ側がどのみち UNKNOWN_COMPONENT で断る）。
    bool TrackByJsonKey(entt::entity e, const std::string& jsonKey);

private:
    struct Rec
    {
        entt::entity e = entt::null;
        u64 guid = 0;
        std::string beforeFp;
        std::vector<std::unique_ptr<IMcpUndoSlot>> slots;
    };
    Rec* RecordFor(entt::entity e);
    entt::registry& Registry();
    std::string Fingerprint(entt::entity e);

    u64 SceneFingerprint() const;

    Scene*           m_scene = nullptr;
    std::string      m_assetsDir;
    std::vector<Rec> m_recs;
    std::vector<std::unique_ptr<IMcpSceneSlot>> m_sceneSlots;
    u64              m_sceneFpBefore = 0;
    bool             m_sceneFpTaken  = false;
};

} // namespace dx12e
