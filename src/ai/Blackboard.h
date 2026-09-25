#pragma once

// ============================================================================
// 黒板（Blackboard）: Brain が「今わかっていること」を置く場所
// ============================================================================
// キー → 値（数値 / 真偽 / 文字列 / Vec3 / エンティティ）。
// ★std::map（キーの辞書順）で持つ＝列挙順が毎回同じ（MCP の JSON・デバッグ表示が揺れない）。
// ★ScriptEngine の saveNum/loadNum（シーンをまたぐ数値ストア）とは別物。こちらは Brain ごと。
// ============================================================================

#include <map>
#include <string>
#include <variant>

#include "core/Types.h"

namespace dx12e
{
namespace ai
{

struct BbVec3 { f32 x = 0, y = 0, z = 0; };
struct BbEntity { u32 id = 0xffffffffu; };   // entt のエンティティ id（null = 0xffffffff）

// monostate = 値なし
using BbValue = std::variant<std::monostate, f64, bool, std::string, BbVec3, BbEntity>;

enum class BbType : u8 { None = 0, Number, Bool, String, Vec3, Entity };
BbType      TypeOf(const BbValue& v);
const char* BbTypeName(BbType t);

class Blackboard
{
public:
    void Set(const std::string& key, BbValue v);
    void SetNumber(const std::string& key, f64 v) { Set(key, BbValue{ v }); }
    void SetBool(const std::string& key, bool v)  { Set(key, BbValue{ v }); }
    void SetVec3(const std::string& key, f32 x, f32 y, f32 z) { Set(key, BbValue{ BbVec3{ x, y, z } }); }
    void SetEntity(const std::string& key, u32 id) { Set(key, BbValue{ BbEntity{ id } }); }
    void SetString(const std::string& key, std::string v) { Set(key, BbValue{ std::move(v) }); }

    bool Has(const std::string& key) const;
    void Erase(const std::string& key);
    void Clear() { m_values.clear(); }
    const BbValue* Find(const std::string& key) const;

    // 数値として読む（bool は 1/0。数値でも bool でもなければ ok=false で def を返す）
    f64  Number(const std::string& key, f64 def, bool* ok = nullptr) const;
    bool Bool(const std::string& key, bool def) const;
    bool Vec3(const std::string& key, BbVec3& out) const;

    const std::map<std::string, BbValue>& All() const { return m_values; }
    size_t Size() const { return m_values.size(); }
    // 書き込みの通し番号（値が変わるたびに増える。デバッグ表示の更新判定用）
    u64 Revision() const { return m_revision; }

private:
    std::map<std::string, BbValue> m_values;
    u64 m_revision = 0;
};

} // namespace ai
} // namespace dx12e
