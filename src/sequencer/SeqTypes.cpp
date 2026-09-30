#include "sequencer/SeqTypes.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace dx12e::seq
{

// ---------------------------------------------------------------------------
// 時間変換
// ---------------------------------------------------------------------------
Tick SecondsToTicks(double seconds, std::int64_t tps)
{
    if (!std::isfinite(seconds)) return 0;
    const double v = seconds * static_cast<double>(tps);
    // 範囲外は kMaxTick に頭打ち(llround の未定義動作を避ける)
    if (v >= static_cast<double>(kMaxTick))  return kMaxTick;
    if (v <= -static_cast<double>(kMaxTick)) return -kMaxTick;
    return static_cast<Tick>(std::llround(v));
}

double TicksToSeconds(Tick t, std::int64_t tps)
{
    return static_cast<double>(t) / static_cast<double>(tps);
}

namespace
{
// round-half-up(a / b)  (b > 0)
std::int64_t RoundHalfUpDiv(std::int64_t a, std::int64_t b)
{
    return FloorDiv(2 * a + b, 2 * b);
}
} // namespace

Tick FrameToTick(std::int64_t frame, int fps, std::int64_t tps)
{
    if (fps <= 0) return 0;
    return RoundHalfUpDiv(frame * tps, fps);
}

std::int64_t TickToFrameFloor(Tick t, int fps, std::int64_t tps)
{
    if (fps <= 0 || tps <= 0) return 0;
    return FloorDiv(t * fps, tps);
}

std::int64_t TickToFrameNearest(Tick t, int fps, std::int64_t tps)
{
    if (fps <= 0 || tps <= 0) return 0;
    return RoundHalfUpDiv(t * fps, tps);
}

Tick SnapToFrame(Tick t, int fps, std::int64_t tps)
{
    return FrameToTick(TickToFrameNearest(t, fps, tps), fps, tps);
}

// ---------------------------------------------------------------------------
// ID
// ---------------------------------------------------------------------------
bool IsValidId(std::string_view id)
{
    if (id.empty() || id.size() > 40) return false;
    for (const char c : id)
    {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                        || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string FormatId(char prefix, std::uint32_t value)
{
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(10);
    s.push_back(prefix);
    s.push_back('_');
    for (int i = 7; i >= 0; --i) s.push_back(hex[(value >> (i * 4)) & 0xF]);
    return s;
}

std::uint64_t IdAllocator::RandomSeed()
{
    std::random_device rd;
    return (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
}

std::string IdAllocator::New(char prefix)
{
    for (;;)
    {
        // SplitMix64
        m_state += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = m_state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= (z >> 31);
        std::string id = FormatId(prefix, static_cast<std::uint32_t>(z));
        if (m_used.insert(id).second) return id;
    }
}

std::uint64_t Fnv1a64(std::string_view s, std::uint64_t h)
{
    for (const char c : s)
    {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x100000001B3ull;
    }
    return h;
}

// ---------------------------------------------------------------------------
// SeqValue
// ---------------------------------------------------------------------------
SeqValue SeqValue::MakeBool(bool v)          { SeqValue r; r.kind = Kind::Bool; r.b = v; return r; }
SeqValue SeqValue::MakeNumber(double v)      { SeqValue r; r.kind = Kind::Number; r.n = (v == 0.0) ? 0.0 : v; return r; }
SeqValue SeqValue::MakeString(std::string v) { SeqValue r; r.kind = Kind::String; r.s = std::move(v); return r; }
SeqValue SeqValue::MakeArray()               { SeqValue r; r.kind = Kind::Array; return r; }
SeqValue SeqValue::MakeObject()              { SeqValue r; r.kind = Kind::Object; return r; }

const SeqValue* SeqValue::Find(std::string_view key) const
{
    if (kind != Kind::Object) return nullptr;
    auto it = std::lower_bound(obj.begin(), obj.end(), key,
                               [](const Member& m, std::string_view k) { return std::string_view(m.key) < k; });
    if (it == obj.end() || it->key != key) return nullptr;
    return &it->value;
}

void SeqValue::Set(std::string key, SeqValue value)
{
    if (kind != Kind::Object)
    {
        *this = MakeObject();
    }
    auto it = std::lower_bound(obj.begin(), obj.end(), key,
                               [](const Member& m, const std::string& k) { return m.key < k; });
    if (it != obj.end() && it->key == key)
    {
        it->value = std::move(value);
        return;
    }
    Member m;
    m.key = std::move(key);
    m.value = std::move(value);
    obj.insert(it, std::move(m));
}

bool SeqValue::Erase(std::string_view key)
{
    if (kind != Kind::Object) return false;
    auto it = std::lower_bound(obj.begin(), obj.end(), key,
                               [](const Member& m, std::string_view k) { return std::string_view(m.key) < k; });
    if (it == obj.end() || it->key != key) return false;
    obj.erase(it);
    return true;
}

std::size_t SeqValue::ObjectSize() const { return kind == Kind::Object ? obj.size() : 0; }

double SeqValue::GetNumber(std::string_view key, double def) const
{
    const SeqValue* v = Find(key);
    return (v && v->IsNumber()) ? v->n : def;
}
bool SeqValue::GetBool(std::string_view key, bool def) const
{
    const SeqValue* v = Find(key);
    return (v && v->IsBool()) ? v->b : def;
}
std::string SeqValue::GetString(std::string_view key, const std::string& def) const
{
    const SeqValue* v = Find(key);
    return (v && v->IsString()) ? v->s : def;
}

bool SeqValue::IsEmptyContainer() const
{
    return kind == Kind::Null || (kind == Kind::Object && obj.empty());
}

bool SeqValue::AllFinite() const
{
    switch (kind)
    {
    case Kind::Number: return std::isfinite(n);
    case Kind::Array:
        for (const auto& v : arr) if (!v.AllFinite()) return false;
        return true;
    case Kind::Object:
        for (const auto& m : obj) if (!m.value.AllFinite()) return false;
        return true;
    default: return true;
    }
}

bool operator==(const SeqValue& a, const SeqValue& b)
{
    if (a.kind != b.kind) return false;
    switch (a.kind)
    {
    case SeqValue::Kind::Null:   return true;
    case SeqValue::Kind::Bool:   return a.b == b.b;
    case SeqValue::Kind::Number: return a.n == b.n;
    case SeqValue::Kind::String: return a.s == b.s;
    case SeqValue::Kind::Array:  return a.arr == b.arr;
    case SeqValue::Kind::Object:
        if (a.obj.size() != b.obj.size()) return false;
        for (std::size_t i = 0; i < a.obj.size(); ++i)
            if (a.obj[i].key != b.obj[i].key || !(a.obj[i].value == b.obj[i].value)) return false;
        return true;
    }
    return false;
}

} // namespace dx12e::seq
