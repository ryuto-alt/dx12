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
        // ★同じ (action, key) は積み増さない。
        //   m_actionMap はアプリ寿命なのに、Play / Stop / ランタイムのシーン切替のたびに
        //   スクリプトが読み直されて bind が再実行される。重複を許すと寄与が合算され、
        //   **2 回目の Play で移動速度が 2 倍、3 回目で 3 倍**になる（エラーもログも無し）。
        //   配布ゲームでもタイトル↔ステージを往復するたびに 1 ずつ増えていた。
        auto& list = m_actions[action];
        for (Binding& b : list)
            if (b.key == key) { b.c = contribution; return; }   // 寄与だけ更新
        list.push_back(Binding{key, contribution});
    }

    // その action に 1 つでも割り当てがあるか（「既定値は上書きしない」判定用）。
    bool HasAction(const std::string& action) const
    {
        const auto it = m_actions.find(action);
        return it != m_actions.end() && !it->second.empty();
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

    struct Binding { int key; DirectX::XMFLOAT3 c; };

    // 全バインドの読み取り。保存（設定ファイルへの書き出し）と設定画面の一覧表示用。
    // ここも「入力の実体を知らない」まま済ませたいので、シリアライズ自体は呼び出し側の仕事。
    const std::unordered_map<std::string, std::vector<Binding>>& Bindings() const { return m_actions; }

private:
    std::unordered_map<std::string, std::vector<Binding>> m_actions;
};

} // namespace dx12e
