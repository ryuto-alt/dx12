#pragma once

// Undo が「値」として持ってよい形（W05-11 恒久策 / EXEC §0.3b）。
// UndoSystem.h（人の編集）と core/mcp/McpUndoTrack.h（AI の編集）の両方が使う。

#include <string>
#include <vector>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include "core/Types.h"
#include "ecs/Components.h"

namespace dx12e
{

// ── Undo が値として持ってよい形（W05-11 恒久策 / EXEC §0.3b）──
// Undo コマンドは「ランタイムが所有するポインタ・ハンドル」（Mesh* / Material* / 物理ボディ ID / スクリプトの環境 /
// ボイス番号など）を保持してはいけない。Stop（Scene::Clear）や再読み込みで持ち主が消えると、Undo が解放済みの値を書き戻してしまう。
//   Capture : コンポーネントをコマンドへ預けるときに、実行時ハンドルを空にした値（= 純データ）へ変える
//   Merge   : Undo / Redo のとき、**いま生きているコンポーネント**の実行時ハンドルはそのまま残し、
//             純データの部分だけを書き戻す（= ハンドルは「戻すたびに生きている側から解決し直す」）
// 既定は値コピー / 代入（ハンドルを持たない型）。ハンドルを持つ型は下で特殊化する。
// ★新しいコンポーネントが実行時ハンドルを持つなら、ここに特殊化を足すこと。
//   「どの型が特殊化済みか」は tests/undo_handle_free_test.cpp が 1 型ずつ確かめる（特殊化が無い型を自動で見つける仕組みは無い。
//   型を足すときの確認項目）。Terrain / SculptMesh / InstanceGroup の shared_ptr は共有実体（コピーオンライト）で、
//   汎用の ComponentEditCommand は通らない（専用のストロークコマンド）ので特殊化していない。
// kSupportsEmplace = false の型は AddComponentCommand / RemoveComponentCommand に使えない（static_assert）。
template<typename T>
struct UndoValueTraits
{
    static constexpr bool kSupportsEmplace = true;
    static T    Capture(const T& v) { return v; }
    static void Merge(T& live, const T& stored) { live = stored; }
};

template<>
struct UndoValueTraits<MeshRenderer>
{
    // 純データの値だけではモデル（meshes）を作れないので、無い所へ付け直す Add / Remove は禁止（見えないエンティティになる）。
    static constexpr bool kSupportsEmplace = false;
    static MeshRenderer Capture(const MeshRenderer& v)
    {
        MeshRenderer c = v;
        c.meshes.clear();
        c.materials.clear();
        c.meshNodeTransforms.clear();
        c.vgAutoPath.clear();
        return c;
    }
    static void Merge(MeshRenderer& live, const MeshRenderer& stored)
    {
        // モデル本体（modelPath とそこから解決した Mesh* / Material* / ノード行列）は生きている側のまま。
        // モデルの差し替えは ModelSwapCommand（エンティティごと作り直す）の担当で、ここでは戻さない。
        std::vector<Mesh*>               meshes   = std::move(live.meshes);
        std::vector<Material*>           mats     = std::move(live.materials);
        std::vector<DirectX::XMFLOAT4X4> nodeXf   = std::move(live.meshNodeTransforms);
        std::string                      modelPath = std::move(live.modelPath);
        std::string                      vgAuto    = std::move(live.vgAutoPath);
        live = stored;
        live.meshes             = std::move(meshes);
        live.materials          = std::move(mats);
        live.meshNodeTransforms = std::move(nodeXf);
        live.modelPath          = std::move(modelPath);
        live.vgAutoPath         = std::move(vgAuto);
        live._remapKey          = 0;   // 再解決させる（materialRemap / materialAsset が変わった可能性）
    }
};

template<>
struct UndoValueTraits<RigidBody>
{
    static constexpr bool kSupportsEmplace = true;
    static RigidBody Capture(const RigidBody& v) { RigidBody c = v; c.bodyId = kInvalidBodyId; return c; }
    static void Merge(RigidBody& live, const RigidBody& stored)
    {
        const uint32_t bodyId = live.bodyId;   // Jolt のボディは生きている側のもの
        live = stored;
        live.bodyId = bodyId;
    }
};

template<>
struct UndoValueTraits<LuaScript>
{
    static constexpr bool kSupportsEmplace = true;
    static LuaScript Capture(const LuaScript& v)
    {
        LuaScript c = v;
        c.env.reset(); c.self.reset();
        c.started = false; c.loadError = false; c.errorMessage.clear();
        return c;
    }
    static void Merge(LuaScript& live, const LuaScript& stored)
    {
        // 実行状態（環境・self・started・loadError・errorMessage）はひとまとめで生きている側のまま
        // （ScriptEngine::Shutdown が env と一緒にリセットする組。片方だけ戻すと食い違う）。
        auto env  = std::move(live.env);
        auto self = std::move(live.self);
        const bool started = live.started, loadError = live.loadError;
        std::string errorMessage = std::move(live.errorMessage);
        live = stored;
        live.env = std::move(env);
        live.self = std::move(self);
        live.started = started;
        live.loadError = loadError;
        live.errorMessage = std::move(errorMessage);
    }
};

// ボイス番号・Play 中の状態は音声システムの持ち物。
template<>
struct UndoValueTraits<AudioSource>
{
    static constexpr bool kSupportsEmplace = true;
    static AudioSource Capture(const AudioSource& v) { AudioSource c = v; c.runtimeSlot = -1; c.startedThisPlay = false; return c; }
    static void Merge(AudioSource& live, const AudioSource& stored)
    {
        const i32 slot = live.runtimeSlot; const bool started = live.startedThisPlay;
        live = stored;
        live.runtimeSlot = slot; live.startedThisPlay = started;
    }
};

// CharacterVirtual は PhysicsSystem 内の map が持つ（_registered はそれと対）。
template<>
struct UndoValueTraits<CharacterController>
{
    static constexpr bool kSupportsEmplace = true;
    static CharacterController Capture(const CharacterController& v)
    {
        CharacterController c = v;
        c._desiredVel = {0.0f, 0.0f, 0.0f}; c._verticalVel = 0.0f; c._jumpQueued = false;
        c._jumpOverride = -1.0f; c._grounded = false; c._registered = false;
        return c;
    }
    static void Merge(CharacterController& live, const CharacterController& stored)
    {
        const CharacterController keep = live;
        live = stored;
        live._desiredVel = keep._desiredVel; live._verticalVel = keep._verticalVel; live._jumpQueued = keep._jumpQueued;
        live._jumpOverride = keep._jumpOverride; live._grounded = keep._grounded; live._registered = keep._registered;
    }
};

// ネットワーク上の ID・所有者は NetworkSystem が割り当てる。
template<>
struct UndoValueTraits<NetworkIdentity>
{
    static constexpr bool kSupportsEmplace = true;
    static NetworkIdentity Capture(const NetworkIdentity& v) { NetworkIdentity c = v; c._netId = 0; c._owner = 0; c._isLocalOwner = false; return c; }
    static void Merge(NetworkIdentity& live, const NetworkIdentity& stored)
    {
        const NetworkIdentity keep = live;
        live = stored;
        live._netId = keep._netId; live._owner = keep._owner; live._isLocalOwner = keep._isLocalOwner;
    }
};

// _set（インスタンスの実体）は shared_ptr のコピーオンライトで、「古い実体へ戻す」ことが Undo の仕事（メモリ安全）。
// 持ち歩いてはいけないのは _diskSet（生ポインタ。解放後に同じ番地へ別の実体が来ると「ディスクと一致」と誤判定して保存が飛ぶ）。
// Merge は _diskSet を生きている側のままにする（戻した _set と食い違えば、保存が必要と判定される）。
template<>
struct UndoValueTraits<FoliageLayer>
{
    static constexpr bool kSupportsEmplace = true;
    static FoliageLayer Capture(const FoliageLayer& v) { FoliageLayer c = v; c._diskSet = nullptr; return c; }
    static void Merge(FoliageLayer& live, const FoliageLayer& stored)
    {
        const void* disk = live._diskSet;
        live = stored;
        live._diskSet = disk;
    }
};

// 純データの値から、コンポーネントを付ける（既にあれば Merge で書き戻す。無ければ純データをそのまま付ける）。
template<typename T>
void EmplaceValue(entt::registry& reg, entt::entity e, const T& stored)
{
    static_assert(UndoValueTraits<T>::kSupportsEmplace,
                  "この型は純データの値から付け直せない（MeshRenderer は meshes を解決し直せない）。専用のコマンドを使うこと");
    if (reg.all_of<T>(e)) { UndoValueTraits<T>::Merge(reg.get<T>(e), stored); return; }
    reg.emplace<T>(e, stored);   // stored はハンドルを持たない純データ（ハンドルは空のまま付く）
}

} // namespace dx12e
