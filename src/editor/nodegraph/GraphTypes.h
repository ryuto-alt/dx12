#pragma once

// ===== 汎用ノードグラフ UI: 基本型（ImGui にも DirectX にも依存しない純データ）=====
// マテリアルグラフ G0（docs/MATERIAL_GRAPH_DESIGN.md §2 / §6 / §7 G0）。
// このヘッダ以下の nodegraph/ の「純ロジック」層は std だけに依存し、tests/nodegraph_test.cpp が単体で検証する。
//
// 座標系: グラフ座標（float）。100% 表示・DPI 100% で 1 単位 = 論理 1px。
// 画面座標 = origin + (g - pan) * zoom * dpi（GraphMath.h の Viewport）。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace dx12e::ng
{

struct Vec2
{
    float x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(float ax, float ay) : x(ax), y(ay) {}
    friend Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
    friend Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
    friend Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
    friend bool operator==(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
    friend bool operator!=(Vec2 a, Vec2 b) { return !(a == b); }
};

struct Rect
{
    Vec2 min, max;
    Rect() = default;
    Rect(Vec2 a, Vec2 b) : min(a), max(b) {}
    static Rect FromPosSize(Vec2 p, Vec2 s) { return Rect(p, p + s); }
    float Width() const { return max.x - min.x; }
    float Height() const { return max.y - min.y; }
    Vec2  Size() const { return max - min; }
    Vec2  Center() const { return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f}; }
    bool  Contains(Vec2 p) const { return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y; }
    bool  Contains(const Rect& r) const { return r.min.x >= min.x && r.max.x <= max.x && r.min.y >= min.y && r.max.y <= max.y; }
    bool  Overlaps(const Rect& r) const { return min.x <= r.max.x && max.x >= r.min.x && min.y <= r.max.y && max.y >= r.min.y; }
    Rect  Expanded(float d) const { return Rect({min.x - d, min.y - d}, {max.x + d, max.y + d}); }
    Rect  Translated(Vec2 d) const { return Rect(min + d, max + d); }
    void  Add(const Rect& r)
    {
        min.x = std::min(min.x, r.min.x); min.y = std::min(min.y, r.min.y);
        max.x = std::max(max.x, r.max.x); max.y = std::max(max.y, r.max.y);
    }
    friend bool operator==(const Rect& a, const Rect& b) { return a.min == b.min && a.max == b.max; }
    friend bool operator!=(const Rect& a, const Rect& b) { return !(a == b); }
};

// 空の（何も含まない）矩形。Add で広げていく用。
inline Rect EmptyRect()
{
    const float big = 1e30f;
    return Rect({big, big}, {-big, -big});
}
inline bool IsEmptyRect(const Rect& r) { return r.min.x > r.max.x || r.min.y > r.max.y; }

using NodeId    = uint32_t;   // 0 = 無効。モデルが採番する安定 ID（保存・Undo・コピペで保たれる）
using CommentId = uint32_t;   // 0 = 無効
using PinType   = int;        // モデルが決める型 ID（0 以上）

// ピンの参照。ピンは (ノード, 入力/出力, その側の添字) で一意（モデルは固定ピン数のノードだけを持つ）。
struct PinRef
{
    NodeId   node   = 0;
    uint16_t index  = 0;
    bool     output = false;
    bool Valid() const { return node != 0; }
    friend bool operator==(const PinRef& a, const PinRef& b) { return a.node == b.node && a.index == b.index && a.output == b.output; }
    friend bool operator!=(const PinRef& a, const PinRef& b) { return !(a == b); }
    friend bool operator<(const PinRef& a, const PinRef& b)
    {
        if (a.node != b.node) return a.node < b.node;
        if (a.output != b.output) return a.output < b.output;
        return a.index < b.index;
    }
    uint64_t Key() const { return (static_cast<uint64_t>(node) << 17) | (static_cast<uint64_t>(index) << 1) | (output ? 1u : 0u); }
};

// 接続。出力ピン → 入力ピン。入力ピンは接続を 1 本しか持てない（同じ入力へ繋ぎ直すと置き換わる）ので、
// 接続の同一性は「to」で決まる。モデルの Edges() は to の昇順（=決定的な順序）。
struct Edge
{
    PinRef from;   // output == true
    PinRef to;     // output == false
    friend bool operator==(const Edge& a, const Edge& b) { return a.from == b.from && a.to == b.to; }
    friend bool operator!=(const Edge& a, const Edge& b) { return !(a == b); }
};

// ---- ピンの見た目・値 ----
enum class PinShape : uint8_t
{
    Circle,        // F1 など
    CircleLine1,   // 円 + 内側線 1
    CircleLine2,   // 円 + 内側線 2
    Diamond,       // ダイヤ（4 成分など）
    RoundedSquare, // テクスチャ等のオブジェクト
    Square,        // Bool 等
    Pentagon,      // 関数入出力
    Triangle,
};

// 入力ピンに付く「インライン定数」の種別。None は値欄なし。
enum class ValueKind : uint8_t { None, Float, Vec2, Vec3, Vec4, Color, Bool, Enum };

inline int ValueComponentCount(ValueKind k)
{
    switch (k)
    {
    case ValueKind::Float: case ValueKind::Bool: case ValueKind::Enum: return 1;
    case ValueKind::Vec2:  return 2;
    case ValueKind::Vec3:  return 3;
    case ValueKind::Vec4:  case ValueKind::Color: return 4;
    default: return 0;
    }
}

struct PinValue
{
    ValueKind kind = ValueKind::None;
    float     f[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // Bool は f[0] != 0、Enum は (int)f[0]
    friend bool operator==(const PinValue& a, const PinValue& b)
    {
        return a.kind == b.kind && a.f[0] == b.f[0] && a.f[1] == b.f[1] && a.f[2] == b.f[2] && a.f[3] == b.f[3];
    }
    friend bool operator!=(const PinValue& a, const PinValue& b) { return !(a == b); }
};

struct PinTypeDesc
{
    std::string name;        // "Float" / "Vec3" / "Texture" ...（ツールチップ・パレットの型表示）
    int         colorSlot = 0;   // GraphTokens の型色スロット（0..7）。実色はテーマから決まる
    PinShape    shape = PinShape::Circle;
};

struct PinDesc
{
    std::string name;
    PinType     type = 0;
    // 入力ピンだけ: 値欄。
    ValueKind   valueKind = ValueKind::None;
    PinValue    defaultValue;
    std::vector<std::string> enumOptions;   // Enum の選択肢
    float       rangeMin = -1e9f, rangeMax = 1e9f;   // ドラッグ編集の範囲
    float       dragSpeed = 0.01f;
    // ★true のとき「ピンのない値欄（プロパティ行）」。接続できない（定数ノードの値など）。
    bool        propertyOnly = false;
};

struct NodeTypeDesc
{
    std::string id;           // "add"（保存・貼り付けで使う安定名）
    std::string title;        // 表示名
    std::string category;     // パレットの分類
    int         categorySlot = 0;   // ヘッダ色スロット（0..9）
    std::string keywords;     // 検索の別名（日本語・英語）
    std::string description;
    std::string icon;         // 任意（アイコングリフの UTF-8。空なら無し）
    char        hotkey = 0;   // UE 風ワンキー（'A' 等。0 = 無し）。押しながらクリック位置へ置く
    std::vector<PinDesc> inputs;
    std::vector<PinDesc> outputs;
    bool        hasPreview = false;   // 本体下部にプレビュー領域を予約する（テクスチャサムネ等）
    bool        isReroute  = false;   // 小さな丸いリルート点として描く
};

// ノードのデータ（1 個ぶんの丸ごとのスナップショット）。Undo・コピペ・保存が使う。
// values は入力ピンの数だけ（値欄のないピンは kind == None）。
struct NodeData
{
    NodeId      id = 0;
    std::string type;
    Vec2        pos;
    std::vector<PinValue> values;
    const NodeTypeDesc* desc = nullptr;   // モデルが返す参照（比較・保存の対象外）
};

struct ConnectCheck
{
    bool        ok = false;
    std::string reason;   // ok でないときの理由（ツールチップに出す。日本語）
    bool        replaces = false;   // ok のとき、既存の接続を置き換える
};

// 見た目のスロット数
inline constexpr int kPinTypeSlots  = 8;
inline constexpr int kCategorySlots = 10;
inline constexpr int kCommentColors = 6;

} // namespace dx12e::ng
