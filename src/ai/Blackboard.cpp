#include "ai/Blackboard.h"

namespace dx12e
{
namespace ai
{

BbType TypeOf(const BbValue& v)
{
    switch (v.index())
    {
    case 1: return BbType::Number;
    case 2: return BbType::Bool;
    case 3: return BbType::String;
    case 4: return BbType::Vec3;
    case 5: return BbType::Entity;
    default: return BbType::None;
    }
}

const char* BbTypeName(BbType t)
{
    switch (t)
    {
    case BbType::Number: return "number";
    case BbType::Bool:   return "bool";
    case BbType::String: return "string";
    case BbType::Vec3:   return "vec3";
    case BbType::Entity: return "entity";
    default:             return "none";
    }
}

void Blackboard::Set(const std::string& key, BbValue v)
{
    if (std::holds_alternative<std::monostate>(v)) { Erase(key); return; }
    auto it = m_values.find(key);
    if (it != m_values.end())
    {
        if (it->second.index() == v.index())
        {
            // 同じ値なら書かない（Revision を無駄に進めない）
            bool same = false;
            switch (v.index())
            {
            case 1: same = std::get<f64>(it->second) == std::get<f64>(v); break;
            case 2: same = std::get<bool>(it->second) == std::get<bool>(v); break;
            case 3: same = std::get<std::string>(it->second) == std::get<std::string>(v); break;
            case 4:
            {
                const BbVec3& a = std::get<BbVec3>(it->second);
                const BbVec3& b = std::get<BbVec3>(v);
                same = a.x == b.x && a.y == b.y && a.z == b.z;
                break;
            }
            case 5: same = std::get<BbEntity>(it->second).id == std::get<BbEntity>(v).id; break;
            default: break;
            }
            if (same) return;
        }
        it->second = std::move(v);
    }
    else
    {
        m_values.emplace(key, std::move(v));
    }
    ++m_revision;
}

bool Blackboard::Has(const std::string& key) const { return m_values.count(key) != 0; }

void Blackboard::Erase(const std::string& key)
{
    if (m_values.erase(key)) ++m_revision;
}

const BbValue* Blackboard::Find(const std::string& key) const
{
    auto it = m_values.find(key);
    return it == m_values.end() ? nullptr : &it->second;
}

f64 Blackboard::Number(const std::string& key, f64 def, bool* ok) const
{
    const BbValue* v = Find(key);
    if (v)
    {
        if (const f64* d = std::get_if<f64>(v)) { if (ok) *ok = true; return *d; }
        if (const bool* b = std::get_if<bool>(v)) { if (ok) *ok = true; return *b ? 1.0 : 0.0; }
    }
    if (ok) *ok = false;
    return def;
}

bool Blackboard::Bool(const std::string& key, bool def) const
{
    const BbValue* v = Find(key);
    if (!v) return def;
    if (const bool* b = std::get_if<bool>(v)) return *b;
    if (const f64* d = std::get_if<f64>(v)) return *d != 0.0;
    return def;
}

bool Blackboard::Vec3(const std::string& key, BbVec3& out) const
{
    const BbValue* v = Find(key);
    if (!v) return false;
    if (const BbVec3* p = std::get_if<BbVec3>(v)) { out = *p; return true; }
    return false;
}

} // namespace ai
} // namespace dx12e
