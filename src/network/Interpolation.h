#pragma once
#include "core/Types.h"

#include <DirectXMath.h>
#include <deque>

namespace dx12e {

// 受信スナップショットのリングバッファ + 2点補間。エンティティ(netId)単位で1個持つ。
// ヘッダオンリー・GPU/entt非依存の純ロジック(単体テスト対象)。
// 「今より少し過去」を描画することでネットワークジッターを吸収する古典的な手法
// (Source engineのinterpolation delayと同じ考え方)。
class SnapshotBuffer
{
public:
    struct Sample
    {
        f32 time = 0.0f;   // 受信側ローカル時計での受信時刻(秒)
        DirectX::XMFLOAT3 position{ 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };   // クォータニオン(x,y,z,w)
        DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };
        bool hasPosition = false;
        bool hasRotation = false;
        bool hasScale    = false;
    };

    void Push(const Sample& s)
    {
        // 順序が乱れて届いた古いサンプルは補間の単調性を壊すので無視する。
        if (!m_samples.empty() && s.time < m_samples.back().time) return;
        m_samples.push_back(s);
        while (m_samples.size() > kMaxSamples) m_samples.pop_front();
    }

    bool Empty() const { return m_samples.empty(); }
    void Clear() { m_samples.clear(); }

    // renderTime 時点のサンプルを補間して返す。バッファが空なら false。
    // renderTime がバッファ範囲より古い/新しい場合は最も近い端のサンプルをそのまま返す
    // (外挿はしない。将来必要になれば追加する)。
    bool TrySample(f32 renderTime, Sample& out) const
    {
        if (m_samples.empty()) return false;

        if (renderTime <= m_samples.front().time) { out = m_samples.front(); return true; }
        if (renderTime >= m_samples.back().time)  { out = m_samples.back();  return true; }

        for (size_t i = 0; i + 1 < m_samples.size(); ++i)
        {
            const Sample& a = m_samples[i];
            const Sample& b = m_samples[i + 1];
            if (renderTime < a.time || renderTime > b.time) continue;

            const f32 span = b.time - a.time;
            const f32 t = (span > 1e-6f) ? (renderTime - a.time) / span : 0.0f;

            out = a;   // hasPosition/hasRotation/hasScale フラグは新しい方(b)を優先
            out.hasPosition = a.hasPosition && b.hasPosition;
            out.hasRotation = a.hasRotation && b.hasRotation;
            out.hasScale    = a.hasScale && b.hasScale;

            if (out.hasPosition)
                out.position = Lerp3(a.position, b.position, t);
            else if (b.hasPosition)
                out.position = b.position;

            if (out.hasRotation)
                out.rotation = Slerp(a.rotation, b.rotation, t);
            else if (b.hasRotation)
                out.rotation = b.rotation;

            if (out.hasScale)
                out.scale = Lerp3(a.scale, b.scale, t);
            else if (b.hasScale)
                out.scale = b.scale;

            return true;
        }
        // ここに来るのは理論上ない(前後の早期returnで尽くしているはず)が、保険で最新を返す。
        out = m_samples.back();
        return true;
    }

private:
    static constexpr size_t kMaxSamples = 16;

    static DirectX::XMFLOAT3 Lerp3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, f32 t)
    {
        using namespace DirectX;
        XMVECTOR va = XMLoadFloat3(&a), vb = XMLoadFloat3(&b);
        XMFLOAT3 out;
        XMStoreFloat3(&out, XMVectorLerp(va, vb, t));
        return out;
    }

    static DirectX::XMFLOAT4 Slerp(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b, f32 t)
    {
        using namespace DirectX;
        XMVECTOR va = XMLoadFloat4(&a), vb = XMLoadFloat4(&b);
        XMFLOAT4 out;
        XMStoreFloat4(&out, XMQuaternionSlerp(va, vb, t));
        return out;
    }

    std::deque<Sample> m_samples;
};

} // namespace dx12e
