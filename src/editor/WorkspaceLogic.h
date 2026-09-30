#pragma once

// ===== ワークスペース / レイアウトの純ロジック（フェーズ 1b W）=====
// ドックの持ち方の方針（決定）:
//   ImGui の ini 文字列（SaveIniSettingsToMemory）を名前つきで保存する方式は採らず、ドックを【自前の小さな構造体】(Layout)で持つ。
//     ・ini は ImGui 内部のノード ID / 窓の位置（物理 px・DPI 換算マーカー付き）を含み、倍率や窓の増減で壊れやすい。
//       壊れた ini を読むと分割が崩れる（黙って既定へ戻す判断もできない）。
//     ・自前構造なら「分割比 4 本 + どの窓を開くか + 窓ごとの配置先 + 下部ドックのタブ」だけで決定論的に BuildDefaultLayout へ渡せ、
//       DPI に依存しない（比率で持つ）・壊れた JSON は検出して既定へ・単体テストできる。
//     ・フローティング窓の位置だけは ini に任せる（1a までの DPI マーカー処理がそのまま効く）。
// 依存: nlohmann と標準ライブラリだけ（ImGui も EditorContext も要らない）。tests/workspace_logic_test.cpp が直接使う。

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dx12e::ws
{

// ---- 配置先 ----
// RightTab   : 右カラム（インスペクターのタブ群）。既定
// RightSplit : 右カラムを縦に割った下側（インスペクターと並べて見る）
// BottomTab  : 中央下のドック（アセットブラウザ / コンソールのタブ群）
// Floating   : ビューポートの上に浮かぶ独立窓
enum class Slot : int { RightTab = 0, RightSplit = 1, BottomTab = 2, Floating = 3 };
inline constexpr int kSlotCount = 4;

inline const char* SlotKey(Slot s)
{
    switch (s)
    {
    case Slot::RightTab:   return "right";
    case Slot::RightSplit: return "rightSplit";
    case Slot::BottomTab:  return "bottom";
    case Slot::Floating:   return "float";
    }
    return "right";
}
inline const char* SlotLabel(Slot s)
{
    switch (s)
    {
    case Slot::RightTab:   return "右カラムのタブ";
    case Slot::RightSplit: return "右カラムを分割して並べる";
    case Slot::BottomTab:  return "下部ドックのタブ";
    case Slot::Floating:   return "フローティング";
    }
    return "";
}
inline bool SlotFromKey(const std::string& k, Slot& out)
{
    for (int i = 0; i < kSlotCount; ++i)
        if (k == SlotKey(static_cast<Slot>(i))) { out = static_cast<Slot>(i); return true; }
    return false;
}

// ---- 分割比 ----
// left       : 全幅に対するヒエラルキー列の割合
// right      : 「左を除いた幅」に対する右カラムの割合
// bottom     : 中央（左右を除いた領域）の高さに対する下部ドックの割合
// rightSplit : 右カラムを縦に割った時の下側（分割された窓）の割合
struct Ratios
{
    float left = 0.18f, right = 0.24f, bottom = 0.33f, rightSplit = 0.42f;
    bool operator==(const Ratios& o) const
    {
        return left == o.left && right == o.right && bottom == o.bottom && rightSplit == o.rightSplit;
    }
};
inline constexpr float kMinLeft = 0.10f,  kMaxLeft = 0.35f;
inline constexpr float kMinRight = 0.15f, kMaxRight = 0.45f;
inline constexpr float kMinBottom = 0.12f, kMaxBottom = 0.60f;
inline constexpr float kMinSplit = 0.20f, kMaxSplit = 0.75f;

inline float ClampOr(float v, float lo, float hi, float def)
{
    if (!std::isfinite(v)) return def;
    return (std::min)((std::max)(v, lo), hi);
}
inline Ratios Clamp(const Ratios& r)
{
    const Ratios d;
    Ratios o;
    o.left       = ClampOr(r.left, kMinLeft, kMaxLeft, d.left);
    o.right      = ClampOr(r.right, kMinRight, kMaxRight, d.right);
    o.bottom     = ClampOr(r.bottom, kMinBottom, kMaxBottom, d.bottom);
    o.rightSplit = ClampOr(r.rightSplit, kMinSplit, kMaxSplit, d.rightSplit);
    return o;
}
// 1/1000 に丸める（毎フレーム吸い上げた値が微小に揺れても保存が走らないように）
inline float Round3(float v) { return std::round(v * 1000.0f) / 1000.0f; }
inline Ratios Round(const Ratios& r) { return {Round3(r.left), Round3(r.right), Round3(r.bottom), Round3(r.rightSplit)}; }

// ---- レイアウト ----
struct SlotOverride
{
    std::string id;   // ツール窓 id（editor/ToolWindows.h の Desc::id）
    Slot        slot = Slot::RightTab;
    bool operator==(const SlotOverride& o) const { return id == o.id && slot == o.slot; }
};

struct Layout
{
    Ratios ratios;
    std::vector<std::string> open;          // 開いているツール窓の id
    std::vector<SlotOverride> slots;        // 既定と違う配置先だけ
    std::vector<std::string> bottomTabs;    // 開いている下部ドックの登録タブ id（並び順）
    std::string bottomActive;               // 最後に選んでいた下部ドックのタブ id（空 = 指定なし）

    bool operator==(const Layout& o) const
    {
        return ratios == o.ratios && open == o.open && slots == o.slots && bottomTabs == o.bottomTabs && bottomActive == o.bottomActive;
    }
};

inline bool Contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}
// 重複を除く（最初の出現を残す）。空文字も除く。
inline std::vector<std::string> Dedup(const std::vector<std::string>& v)
{
    std::vector<std::string> o;
    for (const auto& s : v)
        if (!s.empty() && !Contains(o, s)) o.push_back(s);
    return o;
}

inline Slot SlotFor(const Layout& l, const std::string& id, Slot def)
{
    for (const auto& s : l.slots) if (s.id == id) return s.slot;
    return def;
}
// 配置先を設定する。既定と同じなら上書きを消す（保存データを小さく保つ）。
inline void SetSlot(Layout& l, const std::string& id, Slot slot, Slot def)
{
    for (auto it = l.slots.begin(); it != l.slots.end(); ++it)
        if (it->id == id) { l.slots.erase(it); break; }
    if (slot != def) l.slots.push_back({id, slot});
}
inline void SetOpen(Layout& l, const std::string& id, bool on)
{
    auto it = std::find(l.open.begin(), l.open.end(), id);
    if (on && it == l.open.end()) l.open.push_back(id);
    if (!on && it != l.open.end()) l.open.erase(it);
}

// 知らない id（廃止・改名された窓）を取り除く。isKnown は「その id が今のレジストリに在るか」。
template <class Pred>
inline Layout Sanitize(Layout l, Pred isKnown)
{
    l.ratios = Clamp(l.ratios);
    l.open = Dedup(l.open);
    l.open.erase(std::remove_if(l.open.begin(), l.open.end(), [&](const std::string& s) { return !isKnown(s); }), l.open.end());
    l.slots.erase(std::remove_if(l.slots.begin(), l.slots.end(), [&](const SlotOverride& s) { return !isKnown(s.id); }), l.slots.end());
    l.bottomTabs = Dedup(l.bottomTabs);
    return l;
}

// ---- JSON ----
inline constexpr int kLayoutVersion = 1;

inline nlohmann::json ToJsonValue(const Layout& l)
{
    nlohmann::json j = nlohmann::json::object();
    j["v"] = kLayoutVersion;
    j["ratios"] = {{"left", l.ratios.left}, {"right", l.ratios.right}, {"bottom", l.ratios.bottom}, {"rightSplit", l.ratios.rightSplit}};
    j["open"] = l.open;
    nlohmann::json slots = nlohmann::json::object();
    for (const auto& s : l.slots) slots[s.id] = SlotKey(s.slot);
    j["slots"] = std::move(slots);
    j["bottom"] = {{"tabs", l.bottomTabs}, {"active", l.bottomActive}};
    return j;
}
inline std::string ToJson(const Layout& l) { return ToJsonValue(l).dump(); }

// 壊れた / 未知バージョン / 型違いは false（out は既定のまま）。部分的に欠けたキーは既定で補う。
inline bool FromJsonValue(const nlohmann::json& j, Layout& out)
{
    out = Layout{};
    if (!j.is_object()) return false;
    if (!j.contains("v") || !j["v"].is_number_integer() || j["v"].get<int>() != kLayoutVersion) return false;

    Layout l;
    if (j.contains("ratios") && j["ratios"].is_object())
    {
        const auto& r = j["ratios"];
        auto num = [&](const char* k, float def) {
            return (r.contains(k) && r[k].is_number()) ? r[k].get<float>() : def;
        };
        l.ratios.left       = num("left", l.ratios.left);
        l.ratios.right      = num("right", l.ratios.right);
        l.ratios.bottom     = num("bottom", l.ratios.bottom);
        l.ratios.rightSplit = num("rightSplit", l.ratios.rightSplit);
    }
    l.ratios = Clamp(l.ratios);
    auto strings = [](const nlohmann::json& a) {
        std::vector<std::string> v;
        if (a.is_array())
            for (const auto& e : a) if (e.is_string()) v.push_back(e.get<std::string>());
        return Dedup(v);
    };
    if (j.contains("open")) l.open = strings(j["open"]);
    if (j.contains("slots") && j["slots"].is_object())
        for (auto it = j["slots"].begin(); it != j["slots"].end(); ++it)
        {
            Slot s = Slot::RightTab;
            if (it.value().is_string() && SlotFromKey(it.value().get<std::string>(), s)) l.slots.push_back({it.key(), s});
        }
    if (j.contains("bottom") && j["bottom"].is_object())
    {
        const auto& b = j["bottom"];
        if (b.contains("tabs")) l.bottomTabs = strings(b["tabs"]);
        if (b.contains("active") && b["active"].is_string()) l.bottomActive = b["active"].get<std::string>();
    }
    out = std::move(l);
    return true;
}
inline bool FromJson(const std::string& text, Layout& out)
{
    out = Layout{};
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ false);
    if (j.is_discarded()) return false;
    return FromJsonValue(j, out);
}

// ---- 名前つきレイアウト ----
inline constexpr size_t kMaxLayoutNameBytes = 60;
// 前後の空白を除く。空 / 長すぎる / 制御文字を含む名前は空文字を返す（＝不正）。
inline std::string NormalizeLayoutName(std::string n)
{
    auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!n.empty() && isSpace(static_cast<unsigned char>(n.front()))) n.erase(n.begin());
    while (!n.empty() && isSpace(static_cast<unsigned char>(n.back()))) n.pop_back();
    if (n.empty() || n.size() > kMaxLayoutNameBytes) return {};
    for (unsigned char c : n) if (c < 0x20 || c == 0x7f) return {};
    return n;
}
inline const char* NamedLayoutPrefix() { return "layout.named."; }

// ---- ワークスペース（プリセット + 切替で状態を保つ）----
struct Preset
{
    const char* id;
    const char* title;
    const char* keywords;   // パレット検索用の別名
    Layout      layout;
};

inline const std::vector<Preset>& Presets()
{
    static const std::vector<Preset> p = [] {
        std::vector<Preset> v;
        {   // レベル編集: 既定（ツール窓は全部閉じる。右カラム = インスペクター）
            Layout l;
            v.push_back({"level", "レベル編集", "level 既定 default", l});
        }
        {   // マテリアル: マテリアルエディタ + グラフ（浮かぶ窓。ライブラリ（Poly Haven）は必要な時に足す）
            Layout l;
            l.open = {"material", "materialGraph"};
            v.push_back({"material", "マテリアル", "material shader グラフ", l});
        }
        {   // ライティング: ライティング窓 + ポスト + 空（右タブ）
            Layout l;
            l.open = {"lighting", "postProcess", "skybox", "ssao"};
            v.push_back({"lighting", "ライティング", "lighting light sun ポスト 空", l});
        }
        {   // アニメ・シーケンサー: 下部ドックを厚くしてタイムライン（ダミー / シーケンサー）。大きな浮かぶ窓で下部ドックを隠さないよう、窓は開かない
            Layout l;
            l.ratios.bottom = 0.40f;
            l.bottomTabs = {"timeline"};
            l.bottomActive = "timeline";
            v.push_back({"animation", "アニメ・シーケンサー", "animation sequencer timeline アニメ", l});
        }
        {   // VFX: パーティクルエディタ
            Layout l;
            l.open = {"particle"};
            v.push_back({"vfx", "VFX", "particle effect エフェクト パーティクル", l});
        }
        {   // UI: UI エディタ（UI アニメは大きな独立窓なので必要な時に足す）
            Layout l;
            l.open = {"uiEditor"};
            v.push_back({"ui", "UI", "ui canvas ゲーム内UI", l});
        }
        return v;
    }();
    return p;
}
inline const Preset* FindPreset(const std::string& id)
{
    for (const auto& p : Presets()) if (id == p.id) return &p;
    return nullptr;
}

struct WorkspaceState
{
    std::string current = "level";
    std::map<std::string, Layout> saved;   // ワークスペースごとの「最後の状態」
};

// 現在のワークスペースの状態(snapshot)を記憶し、切替先の状態を返す（記憶が無ければプリセット）。
// 未知の id なら state を変えず false。
inline bool SwitchTo(WorkspaceState& st, const std::string& toId, const Layout& snapshot, Layout& outLayout)
{
    const Preset* p = FindPreset(toId);
    if (!p) return false;
    st.saved[st.current] = snapshot;
    auto it = st.saved.find(toId);
    outLayout = (it != st.saved.end()) ? it->second : p->layout;
    st.current = toId;
    return true;
}

inline std::string StateToJson(const WorkspaceState& st)
{
    nlohmann::json j = nlohmann::json::object();
    j["v"] = 1;
    j["current"] = st.current;
    nlohmann::json saved = nlohmann::json::object();
    for (const auto& kv : st.saved) saved[kv.first] = ToJsonValue(kv.second);
    j["saved"] = std::move(saved);
    return j.dump();
}
inline bool StateFromJson(const std::string& text, WorkspaceState& out)
{
    out = WorkspaceState{};
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ false);
    if (j.is_discarded() || !j.is_object()) return false;
    WorkspaceState st;
    if (j.contains("current") && j["current"].is_string() && FindPreset(j["current"].get<std::string>()))
        st.current = j["current"].get<std::string>();
    if (j.contains("saved") && j["saved"].is_object())
        for (auto it = j["saved"].begin(); it != j["saved"].end(); ++it)
        {
            Layout l;
            if (FindPreset(it.key()) && FromJsonValue(it.value(), l)) st.saved[it.key()] = l;
        }
    out = std::move(st);
    return true;
}

// ---- 下部ドックの最大化 ----
// 最大化中でもゲーム絵の矩形が 0 にならない下限を設ける。centerH = 中央領域（左右を除いた）の高さ(px)、
// minViewportPx = ビューポート帯 + ゲーム絵として最低限残す高さ(px)。返り値は bottom 比（clamp 済み。通常の kMaxBottom を超えてよい）。
inline float MaximizedBottomRatio(float centerH, float minViewportPx)
{
    if (!(centerH > 1.0f)) return kMaxBottom;
    const float r = 1.0f - minViewportPx / centerH;
    return (std::min)((std::max)(r, kMinBottom), 0.92f);
}

// ---- フローティング窓の整列 ----
struct Pt { float x = 0.0f, y = 0.0f; };
// index 番目の窓の左上。領域(ax,ay,aw,ah)の左上から step ずつずらして重ね、はみ出さないよう領域内へ収める。
inline Pt CascadePos(int index, float ax, float ay, float aw, float ah, float winW, float winH, float step)
{
    const int n = (std::max)(index, 0);
    float x = ax + step * static_cast<float>(n % 6) + step * 0.5f * static_cast<float>(n / 6);
    float y = ay + step * static_cast<float>(n % 6);
    x = (std::max)(ax, (std::min)(x, ax + (std::max)(0.0f, aw - winW)));
    y = (std::max)(ay, (std::min)(y, ay + (std::max)(0.0f, ah - winH)));
    return {x, y};
}

} // namespace dx12e::ws
