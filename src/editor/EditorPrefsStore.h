#pragma once

// ===== エディタ設定ストア(純ロジック。tests/editor_prefs_test.cpp が直接使う)=====
// %APPDATA%\DX12Engine\editor_state.json の "prefs" オブジェクトを、メモリにキャッシュして読み書きする。
//   ・読み書きの実体は Loader / Saver(std::function)で差し替え可能＝テストはメモリ上で完結する。
//     本番の配線は editor/EditorPrefs.cpp（ProjectManager の editor_state.json。他機能のキーは消さない）。
//   ・Set は値が変わった時だけ dirty にする。書き込みは Tick(now) が「最初に dirty になってから debounce 秒」後に 1 回だけ
//     行う（毎フレーム Set しても I/O は増えない。最大遅延も debounce 秒で頭打ち）。終了時は Flush()。
//   ・壊れた JSON / 型違いでも落ちない（既定値を返す。型は読み替える: 数値⇔真偽、整数⇔小数）。
//   ・スレッド安全（1 本のミューテックス）。
// 依存: nlohmann/json と標準ライブラリだけ。

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dx12e::prefs
{

class Store
{
public:
    using LoadFn  = std::function<std::string()>;                 // "prefs" オブジェクトの JSON 文字列を返す（無ければ空）
    using SaveFn  = std::function<void(const std::string&)>;      // "prefs" オブジェクトの JSON 文字列を保存する
    using ClockFn = std::function<double()>;                      // 秒（単調増加）

    Store() = default;

    // 入出力の差し替え。差し替えるとキャッシュは破棄され、次の Get で読み直す。
    void SetIO(LoadFn load, SaveFn save)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_load = std::move(load);
        m_save = std::move(save);
        m_loaded = false;
        m_dirty = false;
        m_obj = nlohmann::json::object();
    }
    void SetClock(ClockFn clock)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_clock = std::move(clock);
    }
    void SetDebounceSeconds(double s)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_debounce = s < 0.0 ? 0.0 : s;
    }
    // false の間は Tick / Flush が保存しない（メモリ上の値は変わる）。--background や決定論撮影で使う。
    void SetWriteEnabled(bool on)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_writeEnabled = on;
    }

    // ---- 読み ----
    bool GetBool(const char* key, bool def)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const nlohmann::json* v = Find(key);
        if (!v) return def;
        if (v->is_boolean()) return v->get<bool>();
        if (v->is_number())  return v->get<double>() != 0.0;
        return def;
    }
    int GetInt(const char* key, int def)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const nlohmann::json* v = Find(key);
        if (!v) return def;
        if (v->is_number_integer() || v->is_number_unsigned()) return static_cast<int>(v->get<long long>());
        if (v->is_number_float())
        {
            const double d = v->get<double>();
            if (!std::isfinite(d)) return def;
            return static_cast<int>(std::lround(d));
        }
        if (v->is_boolean()) return v->get<bool>() ? 1 : 0;
        return def;
    }
    float GetFloat(const char* key, float def)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const nlohmann::json* v = Find(key);
        if (!v) return def;
        if (v->is_number())  { const double d = v->get<double>(); return std::isfinite(d) ? static_cast<float>(d) : def; }
        if (v->is_boolean()) return v->get<bool>() ? 1.0f : 0.0f;
        return def;
    }
    std::string GetString(const char* key, const std::string& def)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const nlohmann::json* v = Find(key);
        if (v && v->is_string()) return v->get<std::string>();
        return def;
    }
    bool Has(const char* key)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return Find(key) != nullptr;
    }
    // prefix で始まるキーの一覧（名前つきレイアウトの列挙など）。並びはキーの辞書順。
    std::vector<std::string> KeysWithPrefix(const std::string& prefix)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        EnsureLoaded();
        std::vector<std::string> out;
        for (auto it = m_obj.begin(); it != m_obj.end(); ++it)
            if (it.key().compare(0, prefix.size(), prefix) == 0) out.push_back(it.key());
        return out;
    }

    // ---- 書き ----
    void SetBool(const char* key, bool v)               { SetValue(key, nlohmann::json(v)); }
    void SetInt(const char* key, int v)                 { SetValue(key, nlohmann::json(v)); }
    void SetFloat(const char* key, float v)             { SetValue(key, nlohmann::json(std::isfinite(v) ? v : 0.0f)); }
    void SetString(const char* key, const std::string& v) { SetValue(key, nlohmann::json(v)); }
    void Erase(const char* key)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        EnsureLoaded();
        if (m_obj.erase(key) > 0) MarkDirty();
    }

    // ---- 書き込み ----
    // 毎フレーム呼んでよい（dirty でなければ即 return）。debounce 秒経っていれば 1 回だけ保存する。
    void Tick()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_dirty) return;
        if (Now() - m_dirtySince < m_debounce) return;
        SaveLocked();
    }
    void Flush()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_dirty) SaveLocked();
    }
    bool IsDirty()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_dirty;
    }
    // 全消去（メモリ + 次の保存で "prefs" ごと空になる）。テスト用 / 「設定を初期化」用。
    void Clear()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        EnsureLoaded();
        if (!m_obj.empty()) { m_obj = nlohmann::json::object(); MarkDirty(); }
    }

private:
    double Now() const
    {
        if (m_clock) return m_clock();
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
    void MarkDirty()
    {
        if (!m_dirty) { m_dirty = true; m_dirtySince = Now(); }
    }
    void EnsureLoaded()
    {
        if (m_loaded) return;
        m_loaded = true;
        m_obj = nlohmann::json::object();
        if (!m_load) return;
        const std::string text = m_load();
        if (text.empty()) return;
        nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ false);
        if (!j.is_discarded() && j.is_object()) m_obj = std::move(j);   // 壊れていたら空から始める（次の保存で正常化）
    }
    const nlohmann::json* Find(const char* key)
    {
        EnsureLoaded();
        auto it = m_obj.find(key);
        return it == m_obj.end() ? nullptr : &*it;
    }
    void SetValue(const char* key, nlohmann::json v)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        EnsureLoaded();
        auto it = m_obj.find(key);
        if (it != m_obj.end() && *it == v) return;   // 同じ値なら何もしない（毎フレーム Set しても安い）
        m_obj[key] = std::move(v);
        MarkDirty();
    }
    void SaveLocked()
    {
        m_dirty = false;
        if (!m_writeEnabled || !m_save) return;
        m_save(m_obj.dump());
    }

    std::mutex     m_mutex;
    LoadFn         m_load;
    SaveFn         m_save;
    ClockFn        m_clock;
    nlohmann::json m_obj = nlohmann::json::object();
    bool           m_loaded = false;
    bool           m_dirty = false;
    bool           m_writeEnabled = true;
    double         m_dirtySince = 0.0;
    double         m_debounce = 1.5;
};

} // namespace dx12e::prefs
