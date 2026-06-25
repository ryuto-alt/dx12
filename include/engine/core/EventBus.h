#pragma once
//
// EventBus — エンジン汎用イベントバス（Phase 3 中核）。
//
// 閉じた TriggerActionType enum（「何が起きるか」をエンジンが固定列挙）を廃し、
// 「Trigger は領域 overlap を判定して events:emit するだけ。何が起きるかはゲーム側(Lua/データ)」
// へ転換するための土台。C++ サブシステム（衝突/トリガ/シーンライフサイクル/ターン）も
// 同じバスへ流す。ジャンル語彙を一切含まない中立ヘッダ。
//
// ヘッダオンリー・GPU 非依存。Lua の events テーブルはこの薄いバインドにする（後続）。
//
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <entt/entity/entity.hpp>   // entt::entity / entt::null

namespace dx12e
{

// 1 イベント。name + 任意の source/other エンティティ + 小さなキー値ペイロード。
struct EngineEvent
{
    using Value = std::variant<double, std::string, bool>;
    struct KV { std::string key; Value val; };

    std::string  name;
    entt::entity source = entt::null;
    entt::entity other  = entt::null;
    std::vector<KV> data;

    void set(const std::string& k, double v)             { data.push_back({k, v}); }
    void set(const std::string& k, bool v)               { data.push_back({k, v}); }
    void set(const std::string& k, const std::string& v) { data.push_back({k, v}); }

    double num(const std::string& k, double def = 0.0) const
    {
        for (const auto& kv : data)
            if (kv.key == k) if (auto* p = std::get_if<double>(&kv.val)) return *p;
        return def;
    }
    bool boolean(const std::string& k, bool def = false) const
    {
        for (const auto& kv : data)
            if (kv.key == k) if (auto* p = std::get_if<bool>(&kv.val)) return *p;
        return def;
    }
    std::string str(const std::string& k, const std::string& def = {}) const
    {
        for (const auto& kv : data)
            if (kv.key == k) if (auto* p = std::get_if<std::string>(&kv.val)) return *p;
        return def;
    }
};

class EventBus
{
public:
    using Handler = std::function<void(const EngineEvent&)>;

    // name 購読。戻り値の購読IDは Off に使う。
    std::uint32_t On(const std::string& name, Handler h)
    {
        const std::uint32_t id = ++m_nextId;
        m_handlers[name].push_back({id, std::move(h)});
        return id;
    }

    // entity スコープ購読。scope エンティティが OnEntityDestroyed に渡されたら
    // 同 scope の購読を全 name から自動 Off する（ハンドラ寿命をエンティティに束ねる）。
    std::uint32_t On(const std::string& name, entt::entity scope, Handler h)
    {
        const std::uint32_t id = ++m_nextId;
        m_scopedHandlers[name].push_back({id, scope, std::move(h)});
        return id;
    }

    // entt の on_destroy シグナル等から呼ぶ。scope == e の購読を全 name から削除。
    void OnEntityDestroyed(entt::entity e)
    {
        for (auto& kv : m_scopedHandlers)
        {
            auto& list = kv.second;
            list.erase(
                std::remove_if(list.begin(), list.end(),
                    [e](const ScopedEntry& se){ return se.scope == e; }),
                list.end());
        }
    }

    // 購読解除（全 name を走査して id 一致を除去）。scoped も走査する。
    void Off(std::uint32_t id)
    {
        for (auto& kv : m_handlers)
        {
            auto& list = kv.second;
            for (auto it = list.begin(); it != list.end(); ++it)
                if (it->id == id) { list.erase(it); break; }
        }
        for (auto& kv : m_scopedHandlers)
        {
            auto& list = kv.second;
            for (auto it = list.begin(); it != list.end(); ++it)
                if (it->id == id) { list.erase(it); break; }
        }
    }

    // 即時発火。ハンドラ列のコピーを回すので、ハンドラ内で On/Off しても安全。
    void Emit(const EngineEvent& e) const
    {
        auto it = m_handlers.find(e.name);
        if (it != m_handlers.end())
        {
            const auto snapshot = it->second;
            for (const auto& entry : snapshot)
                if (entry.fn) entry.fn(e);
        }

        // entity スコープ購読も同名で発火（同じ EngineEvent を渡す）。
        auto it2 = m_scopedHandlers.find(e.name);
        if (it2 != m_scopedHandlers.end())
        {
            const auto snapshot2 = it2->second;
            for (const auto& se : snapshot2)
                if (se.fn) se.fn(e);
        }
    }

    // フレーム末まで遅延発火（列挙中の破壊や再入を避けたいとき）。
    void Post(EngineEvent e) { m_queue.push_back(std::move(e)); }

    // 遅延キューを発火。Flush 中の Post も同フレームで処理する。
    void Flush()
    {
        while (!m_queue.empty())
        {
            std::vector<EngineEvent> batch;
            batch.swap(m_queue);
            for (const auto& e : batch) Emit(e);
        }
    }

    // 全購読（scoped 含む）と遅延キューを破棄（Play 開始時など）。
    void Clear() { m_handlers.clear(); m_scopedHandlers.clear(); m_queue.clear(); }

    // name の購読数（通常 + scoped の合算）。
    std::size_t HandlerCount(const std::string& name) const
    {
        std::size_t n = 0;
        auto it = m_handlers.find(name);
        if (it != m_handlers.end()) n += it->second.size();
        auto it2 = m_scopedHandlers.find(name);
        if (it2 != m_scopedHandlers.end()) n += it2->second.size();
        return n;
    }

private:
    struct Entry       { std::uint32_t id; Handler fn; };
    struct ScopedEntry { std::uint32_t id; entt::entity scope; Handler fn; };
    std::unordered_map<std::string, std::vector<Entry>>       m_handlers;
    std::unordered_map<std::string, std::vector<ScopedEntry>> m_scopedHandlers;
    std::vector<EngineEvent> m_queue;
    std::uint32_t m_nextId = 0;
};

} // namespace dx12e
