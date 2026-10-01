#pragma once
// ===========================================================================
// インスタンス群（InstanceGroup）の実体と、サイドカー（<シーン名>.inst/<guid>.jsonl）の純関数。
// 設計: docs/SCENE_FORMAT_DESIGN.md §4.1 の 4-1。
//
//   ・1 エンティティ = モデル 1 つ（そのエンティティの MeshRenderer）+ インスタンス配列（このファイルの InstanceSet）。
//   ・インスタンスは「グループの Transform からの相対 TRS」（位置 / Euler 度 / スケール。Transform と同じ意味・同じ行列計算）。
//   ・InstanceSet はコピーオンライト。編集は新しい実体を作って差し替える（複製・Undo・Play スナップショットは
//     ポインタを共有するだけで、配列の再直列化はしない）。実体を書き換えてはいけない（共有されている）。
//   ・GPU・シーン・ファイルシステムに依存しない（ctest から直接叩ける）。
// ===========================================================================
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include "core/Types.h"

namespace dx12e::instgroup
{

// 1 インスタンス（36 B）。rotation は Transform::rotation と同じ Euler 度（YXZ。XMMatrixRotationRollPitchYaw）。
struct InstanceTRS
{
    DirectX::XMFLOAT3 p{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 r{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 s{1.0f, 1.0f, 1.0f};
};
bool BitEqual(const InstanceTRS& a, const InstanceTRS& b);   // ビット一致（-0.0 と 0.0 は別）

struct InstanceSet
{
    std::vector<InstanceTRS> items;
    u64 id = 0;   // 実行時の一意番号（実体ごと。ファイルには出さない。メモリ内スナップショットの引き当てに使う）

    u32 Count() const { return static_cast<u32>(items.size()); }

    // ---- 派生キャッシュ（実体は不変なので、グループ側の行列が変わらない限り使い回せる）----
    // world = ((local * グループ) * 祖先 1) * 祖先 2 ... をグループ側の行列列のハッシュ（worldKey）つきで持つ。
    // 同じ実体を複数のグループが共有していて行列が違うと作り直しが交互に起きる（正しさには影響しない）。
    // ★描画（メインスレッド）専用。
    mutable std::vector<DirectX::XMFLOAT4X4> worldCache;
    mutable u64  worldKey   = 0;
    mutable bool worldValid = false;
    // 位置だけの AABB（インスタンスの原点の範囲）。bounds / 空間ソート用。
    mutable bool boundsValid = false;
    mutable DirectX::XMFLOAT3 boundsMin{0, 0, 0}, boundsMax{0, 0, 0};
};
using InstanceSetPtr = std::shared_ptr<InstanceSet>;

// 新しい実体（id を振る）。items は移動で受ける。
InstanceSetPtr NewSet(std::vector<InstanceTRS>&& items);
// 内容を複製した新しい実体（新しい id・キャッシュは空）。コピーオンライトの編集の入口。
InstanceSetPtr CloneSet(const InstanceSet& src);

// インスタンスのローカル行列。Transform::GetWorldMatrix（useQuaternion=false）と同じ式・同じ順序（S * R * T）。
DirectX::XMMATRIX LocalMatrix(const InstanceTRS& t);

// 位置の AABB（キャッシュ）。空なら 0。
void ComputeBounds(const InstanceSet& s);

// ---------------------------------------------------------------------------
// ワールド行列（描画・物理・ピッキング・Nav・bounds が同じ値を使う）。
//   world_i = ((local_i * 群) * 祖先1) * 祖先2 ...  … 普通のエンティティの ComputeWorldMatrix と同じ掛け順（丸めまで一致）。
// 群側の行列列が変わらない間は InstanceSet のキャッシュを返す（メインスレッド専用。実体は不変なので内容は読み取り専用）。
// outGroupWorld があれば群自身のワールド（ComputeWorldMatrix(reg, e) と同じ値）を返す。
// ---------------------------------------------------------------------------
const std::vector<DirectX::XMFLOAT4X4>& WorldMatrices(const entt::registry& reg, entt::entity group, const InstanceSet& set,
                                                      DirectX::XMFLOAT4X4* outGroupWorld = nullptr);

// 全インスタンスのワールド AABB（メッシュのローカル AABB 8 隅を各インスタンスのワールドへ移して合成）。空なら false。
bool ComputeWorldAabb(const entt::registry& reg, entt::entity group, const InstanceSet& set,
                      const DirectX::XMFLOAT3& localMin, const DirectX::XMFLOAT3& localMax,
                      DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax);

// ---------------------------------------------------------------------------
// サイドカー（テキスト）。1 行 1 インスタンスの数値配列:
//   [px,py,pz]  /  [px,py,pz,rx,ry,rz]  /  [px,py,pz,rx,ry,rz,sx,sy,sz]
// 末尾の既定値（回転 0・スケール 1）は省略できる（ビット一致のときだけ省く）。数値は最短の float（std::to_chars）。
// 空行は無視。壊れた行（数が 3/6/9 でない・数値でない・非有限）は読み飛ばして報告する（以降の行は読む）。
// ---------------------------------------------------------------------------
void AppendLine(std::string& out, const InstanceTRS& t);          // 1 行（末尾 '\n' 付き）を足す
std::string FormatSidecar(const InstanceSet& s);                  // 全行（改行は LF）
bool ParseLine(std::string_view line, InstanceTRS& out);          // 1 行（'\n' 抜き）。壊れていれば false

struct ParseReport
{
    size_t lines = 0;                  // 空行を除いた行数
    size_t ok    = 0;
    size_t bad   = 0;
    std::vector<size_t> badLineNumbers;   // 1 始まりの行番号（先頭 8 件まで）
};
InstanceSetPtr ParseSidecar(std::string_view text, ParseReport* report = nullptr);

// ---------------------------------------------------------------------------
// メモリ内スナップショット用の実体の台帳（Play のスナップショット・Undo の JSON・複製が id で実体を引く）。
//   直列化がファイルモードでないとき、JSON には {"count":N,"mem":id} を書き、実体はここへ強い参照で預ける。
//   ★台帳は強い参照を持つ（スナップショットの文字列は実体を持たないため）。合計インスタンス数が上限を超えたら、
//     どこからも参照されていない実体（use_count==1）から捨てる。
// ---------------------------------------------------------------------------
void StoreRegister(const InstanceSetPtr& s);
InstanceSetPtr StoreFind(u64 id);
size_t StoreEntryCount();             // テスト・診断用
void StoreClearForTests();

// ---------------------------------------------------------------------------
// 直列化の文脈（スレッドローカル）。SceneSerializer が Save / Load の間だけ張る。
// ---------------------------------------------------------------------------
struct SerializeCollector
{
    // (guid の 16 桁 hex, 実体)。ファイル保存の直列化中に積まれる。
    std::vector<std::pair<std::string, InstanceSetPtr>> sets;
};
class ScopedFileSerialize
{
public:
    explicit ScopedFileSerialize(SerializeCollector& c);
    ~ScopedFileSerialize();
    ScopedFileSerialize(const ScopedFileSerialize&) = delete;
    ScopedFileSerialize& operator=(const ScopedFileSerialize&) = delete;
private:
    SerializeCollector* m_prev;
};
SerializeCollector* CurrentCollector();   // nullptr = メモリモード（既定）

class ScopedLoadScene
{
public:
    explicit ScopedLoadScene(std::string scenePath);
    ~ScopedLoadScene();
    ScopedLoadScene(const ScopedLoadScene&) = delete;
    ScopedLoadScene& operator=(const ScopedLoadScene&) = delete;
private:
    std::string m_prev;
};
const std::string& CurrentLoadScenePath();   // 空 = サイドカーを読む先が無い

// <シーン>.inst フォルダと、その中の <guid>.jsonl のパス（scenePath は絶対パス。拡張子を .inst に替える）。
std::string SidecarDirFor(const std::string& scenePath);
std::string SidecarPathFor(const std::string& scenePath, const std::string& guidHex);

} // namespace dx12e::instgroup
