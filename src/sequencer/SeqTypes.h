#pragma once
// シーケンサー(.dxseq)の基本型: ティック時刻・時間変換・ID・JSON 風の汎用値。
// 標準ライブラリだけに依存する(GPU / ECS / ImGui / nlohmann 非依存。docs/DXSEQ_FORMAT.md)。

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dx12e::seq
{

// ---------------------------------------------------------------------------
// 時刻: 整数ティック
// ---------------------------------------------------------------------------
// 既定 6000 tick/秒。24/25/30/48/50/60/100/120 fps が全て整数ティックに乗る。
// 23.976 / 29.97 は 24 / 30 で編集し、出力(ffmpeg)のレートだけ変える。
using Tick = std::int64_t;

constexpr std::int64_t kDefaultTicksPerSecond = 6000;
// 妥当な時刻の上限(2^42 ≒ 4.4e12 tick = 6000tps で約 23 年)。ここに収めれば t*fps 等が int64 で溢れない。
constexpr Tick kMaxTick = Tick(1) << 42;

// b > 0 の床除算/剰余(負の a でも数学的な floor / 非負の mod)
inline std::int64_t FloorDiv(std::int64_t a, std::int64_t b)
{
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
inline std::int64_t FloorMod(std::int64_t a, std::int64_t b)
{
    return a - FloorDiv(a, b) * b;
}

// --- 丸め規則(すべて決定的・整数演算。docs/DXSEQ_FORMAT.md §時間変換) ------------
//  秒 → ティック:      四捨五入(0.5 は 0 から遠い側へ = llround)。非有限値は 0。
//  ティック → 秒:      t / tps(double)。
//  フレーム → ティック: round-half-up(frame*tps/fps)。tps % fps == 0 なら厳密。
//  ティック → フレーム: Floor = t が属するフレーム(floor(t*fps/tps))
//                      Nearest = 最寄りフレーム(半分は上へ)
Tick   SecondsToTicks(double seconds, std::int64_t tps);
double TicksToSeconds(Tick t, std::int64_t tps);
Tick         FrameToTick(std::int64_t frame, int fps, std::int64_t tps);
std::int64_t TickToFrameFloor(Tick t, int fps, std::int64_t tps);
std::int64_t TickToFrameNearest(Tick t, int fps, std::int64_t tps);
// t を最寄りフレーム境界へ吸着させる(スナップ)。
Tick SnapToFrame(Tick t, int fps, std::int64_t tps);
// fps がティックに整数で乗るか(tps % fps == 0)。乗らないと FrameToTick は丸め誤差を持つ。
inline bool FrameRateIsExact(int fps, std::int64_t tps) { return fps > 0 && tps % fps == 0; }

// ---------------------------------------------------------------------------
// ID: 短い安定 ID(`<prefix>_<hex>`)。シーケンス内で全要素が 1 つの名前空間。
// ---------------------------------------------------------------------------
// 有効な ID: 1..40 文字の [A-Za-z0-9_.-]。手書き(c_01 など)も許す。
bool IsValidId(std::string_view id);
// 慣例の接頭辞: q=シーケンス b=バインディング t=トラック k=クリップ e=イベント m=マーカー c=カット
std::string FormatId(char prefix, std::uint32_t value);

// 決定論的な ID 発行器(SplitMix64)。同じ seed と同じ予約集合なら同じ列を返す。
// ops はできあがった ID を運ぶので、Undo/Redo/リプレイ/別ブランチで ID が食い違わない。
class IdAllocator
{
public:
    explicit IdAllocator(std::uint64_t seed = 0x9E3779B97F4A7C15ull) : m_state(seed) {}
    static std::uint64_t RandomSeed();                 // std::random_device 由来(非決定。本番の既定用)
    void Reserve(std::string_view id) { m_used.insert(std::string(id)); }
    bool IsUsed(std::string_view id) const { return m_used.count(std::string(id)) != 0; }
    void Release(std::string_view id) { m_used.erase(std::string(id)); }
    void Clear() { m_used.clear(); }
    std::string New(char prefix);                      // 衝突しない ID を発行して予約する
    std::size_t Size() const { return m_used.size(); }
private:
    std::uint64_t m_state;
    std::unordered_set<std::string> m_used;
};

// 64bit の FNV-1a(シード導出・テストのハッシュ用)
std::uint64_t Fnv1a64(std::string_view s, std::uint64_t h = 0xCBF29CE484222325ull);

// ---------------------------------------------------------------------------
// SeqValue: JSON 風の汎用値。予約種別(アニメ/音/VFX/…)のパラメータを型なしで運ぶ。
// オブジェクトのキーは常に辞書順(=正準形。シリアライズが決定的になる)。
// 数値は double。NaN / inf は持てない(シリアライズ不能なので Set 系で拒否する)。
// ---------------------------------------------------------------------------
class SeqValue
{
public:
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
    struct Member;

    SeqValue() = default;
    // ★Member は不完全型のうちに使うので、生成関数は SeqTypes.cpp に置く(in-class 定義だと sizeof を要求される)。
    static SeqValue MakeBool(bool v);
    static SeqValue MakeNumber(double v);      // -0.0 は 0.0 に正規化する
    static SeqValue MakeString(std::string v);
    static SeqValue MakeArray();
    static SeqValue MakeObject();

    bool IsNull() const   { return kind == Kind::Null; }
    bool IsBool() const   { return kind == Kind::Bool; }
    bool IsNumber() const { return kind == Kind::Number; }
    bool IsString() const { return kind == Kind::String; }
    bool IsArray() const  { return kind == Kind::Array; }
    bool IsObject() const { return kind == Kind::Object; }

    // オブジェクト操作(Kind が Object でなければ Find は null、Set は Object へ変換して入れる)
    const SeqValue* Find(std::string_view key) const;
    void Set(std::string key, SeqValue value);
    bool Erase(std::string_view key);
    std::size_t ObjectSize() const;

    double      GetNumber(std::string_view key, double def) const;
    bool        GetBool(std::string_view key, bool def) const;
    std::string GetString(std::string_view key, const std::string& def = {}) const;

    // 空の Object / Null なら true(「何も持たない」)
    bool IsEmptyContainer() const;
    // 数値が全て有限か(再帰)
    bool AllFinite() const;

    Kind kind = Kind::Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<SeqValue> arr;
    std::vector<Member> obj;   // key 昇順・キー一意
};

struct SeqValue::Member
{
    std::string key;
    SeqValue value;
};

bool operator==(const SeqValue& a, const SeqValue& b);
inline bool operator!=(const SeqValue& a, const SeqValue& b) { return !(a == b); }

} // namespace dx12e::seq
