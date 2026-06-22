#pragma once
//
// ActionMap — データ駆動の入力アクションマップ（Phase 4 能力カタログ）。
//
// "move" などの抽象アクションに (キー → 寄与ベクトル) を束ねる。リバインド/設定可能入力・
// 機種非依存入力の土台。キー状態の取得は呼び出し側の述語(predicate)へ委ねるので
// InputSystem に非依存で、ヘッドレスにテストできる。ジャンル語彙を含まない中立ヘッダ。
//
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>

namespace dx12e
{

class ActionMap
{
public:
    using KeyDownFn = std::function<bool(int)>;

    // action に (key → 寄与ベクトル) を追加。移動なら方向ベクトル、ボタンなら任意。
    void Bind(const std::string& action, int key, const DirectX::XMFLOAT3& contribution)
    {
        m_actions[action].push_back(Binding{key, contribution});
    }
    // ボタン用（寄与 (1,0,0)）。
    void Bind(const std::string& action, int key)
    {
        Bind(action, key, DirectX::XMFLOAT3{1.0f, 0.0f, 0.0f});
    }

    void ClearAction(const std::string& action) { m_actions.erase(action); }
    void Clear() { m_actions.clear(); }

    // down(key) が真のキーの寄与を合算して返す（移動ベクトル等）。
    DirectX::XMFLOAT3 Evaluate(const std::string& action, const KeyDownFn& down) const
    {
        DirectX::XMFLOAT3 r{0.0f, 0.0f, 0.0f};
        auto it = m_actions.find(action);
        if (it == m_actions.end()) return r;
        for (const auto& b : it->second)
            if (down(b.key)) { r.x += b.c.x; r.y += b.c.y; r.z += b.c.z; }
        return r;
    }

    // いずれかのキーが押されているか（ボタン/トリガ判定）。
    bool Active(const std::string& action, const KeyDownFn& down) const
    {
        auto it = m_actions.find(action);
        if (it == m_actions.end()) return false;
        for (const auto& b : it->second)
            if (down(b.key)) return true;
        return false;
    }

    std::size_t BindingCount(const std::string& action) const
    {
        auto it = m_actions.find(action);
        return it == m_actions.end() ? 0 : it->second.size();
    }

private:
    struct Binding { int key; DirectX::XMFLOAT3 c; };
    std::unordered_map<std::string, std::vector<Binding>> m_actions;
};

} // namespace dx12e
