// ============================================================================
// PreviewLogic.h — マテリアルグラフ G2c の純ロジック（D3D12 / ImGui / DXC 非依存。tests/matgraph_g2c_test.cpp が直接検証する）
//
//   ・Debouncer        : 編集 → 250 ms のデバウンス → コンパイル要求。値のみの変更はここを通さない（1 フレーム以内に反映する）。
//   ・StageGate        : 段階コンパイル（-Od の高速版 → 最適化版）の結果が順不同に届いても、古い / 低い段階が新しい / 高い段階を上書きしない。
//   ・PriorityJobQueue : ワーカーのジョブキュー。段階の低い（= ユーザーが待っている）ジョブを先に・同順位は FIFO。
//                        条件つきの取り出し（最適化ジョブの同時実行数の上限）と、条件に合うジョブの取り消し（古い編集の間引き）。
//   ・PreviewSettings  : プレビューの設定（形状・環境・カメラ・ライト）。グラフ（.dxmg）ではなくエディタ設定へ保存する。文字列の往復つき。
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace dx12e::matgraph
{

// ---------------------------------------------------------------------------
// デバウンス
//   Edit(now) で編集を登録し、毎フレーム Poll(now) を呼ぶ。true が返ったらコンパイルを要求する。
//   quietSec  : 最後の編集からこの秒数だけ静かならトレーリングで発火する（既定 0.25 = 250 ms）
//   leading   : true なら「直前の quietSec 以内に編集が無かった最初の編集」はすぐ（次の Poll で）発火する。
//               その後の連続した編集はトレーリングで 1 回にまとまる（孤立した 1 回の編集は待たない）
//   maxWaitSec: 編集が続いていても、最初の未処理の編集からこの秒数で必ず 1 回は発火する（0 = 無効）
// ---------------------------------------------------------------------------
class Debouncer
{
public:
    struct Config
    {
        double quietSec = 0.25;
        bool   leading = false;
        double maxWaitSec = 0.0;
    };

    Debouncer() = default;
    explicit Debouncer(const Config& c) : m_c(c) {}
    void SetConfig(const Config& c) { m_c = c; }
    const Config& GetConfig() const { return m_c; }

    void Edit(double now)
    {
        const bool idle = !m_everEdited || (now - m_lastEdit >= m_c.quietSec);
        if (!m_hasPending) m_firstPending = now;
        m_lastEdit = now;
        m_everEdited = true;
        if (m_c.leading && idle && !m_hasPending)
        {
            m_fireNow = true;
            m_hasTrailing = false;
            m_hasPending = true;
        }
        else
        {
            m_hasTrailing = true;
            m_hasPending = true;
        }
    }

    // true = 今コンパイルを要求する
    bool Poll(double now)
    {
        if (m_fireNow)
        {
            m_fireNow = false;
            if (!m_hasTrailing) m_hasPending = false;
            ++m_fires;
            return true;
        }
        if (m_hasTrailing)
        {
            const bool quiet = now - m_lastEdit >= m_c.quietSec;
            const bool overdue = m_c.maxWaitSec > 0.0 && now - m_firstPending >= m_c.maxWaitSec;
            if (quiet || overdue)
            {
                m_hasTrailing = false;
                m_hasPending = false;
                ++m_fires;
                return true;
            }
        }
        return false;
    }

    // まだ発火していない編集があるか（UI の「生成待ち」表示用）
    bool Pending() const { return m_hasPending || m_fireNow; }
    // 次のトレーリング発火までの残り秒（Pending でなければ 0）
    double RemainingSec(double now) const
    {
        if (m_fireNow) return 0.0;
        if (!m_hasTrailing) return 0.0;
        return std::max(0.0, m_c.quietSec - (now - m_lastEdit));
    }
    void Cancel() { m_hasPending = m_hasTrailing = m_fireNow = false; }
    uint32_t Fires() const { return m_fires; }

private:
    Config   m_c;
    bool     m_everEdited = false, m_hasPending = false, m_hasTrailing = false, m_fireNow = false;
    double   m_lastEdit = 0.0, m_firstPending = 0.0;
    uint32_t m_fires = 0;
};

// ---------------------------------------------------------------------------
// 段階の順序保証
//   level: 0 = 未完成 / 1 = -Od の高速版 / 2 = 最適化版。結果は届いた順ではなく段階で採否を決める。
// ---------------------------------------------------------------------------
struct StageGate
{
    int level = 0;
    // 受理したら true（現在の段階より高い結果だけ）。false = 捨てる（古い / 同じ / 低い段階）
    bool Accept(int incoming)
    {
        if (incoming <= level) return false;
        level = incoming;
        return true;
    }
};

// ---------------------------------------------------------------------------
// 優先度つきジョブキュー（スレッド安全ではない。呼び出し側が排他する）
// ---------------------------------------------------------------------------
template <class Job>
class PriorityJobQueue
{
public:
    // priority が小さいほど先。同じ priority は投入順（FIFO）
    void Push(Job j, int priority)
    {
        m_items.push_back(Item{std::move(j), priority, m_seq++});
    }
    // allow(job) が真のジョブの中で最優先のものを取り出す。無ければ false
    bool Pop(Job& out, const std::function<bool(const Job&)>& allow = {})
    {
        int best = -1;
        for (size_t i = 0; i < m_items.size(); ++i)
        {
            const Item& it = m_items[i];
            if (allow && !allow(it.job)) continue;
            if (best < 0 || it.priority < m_items[static_cast<size_t>(best)].priority ||
                (it.priority == m_items[static_cast<size_t>(best)].priority && it.seq < m_items[static_cast<size_t>(best)].seq))
                best = static_cast<int>(i);
        }
        if (best < 0) return false;
        out = std::move(m_items[static_cast<size_t>(best)].job);
        m_items.erase(m_items.begin() + best);
        return true;
    }
    // 条件に合うジョブを取り除く（古い編集の間引き）。取り除いた数を返す
    size_t CancelIf(const std::function<bool(const Job&)>& pred)
    {
        size_t n = 0;
        for (size_t i = 0; i < m_items.size();)
        {
            if (pred(m_items[i].job)) { m_items.erase(m_items.begin() + static_cast<ptrdiff_t>(i)); ++n; }
            else ++i;
        }
        return n;
    }
    size_t Size() const { return m_items.size(); }
    bool Empty() const { return m_items.empty(); }
    void Clear() { m_items.clear(); }

private:
    struct Item { Job job; int priority; uint64_t seq; };
    std::vector<Item> m_items;
    uint64_t m_seq = 0;
};

// ---------------------------------------------------------------------------
// プレビュー設定（エディタ設定に保存。グラフの内容ハッシュ / .dxmg には入れない）
// ---------------------------------------------------------------------------
enum class PreviewShape : int { Sphere = 0, Plane = 1, Cylinder = 2, Cube = 3 };
enum class PreviewEnv : int { Studio = 0, Outdoor = 1, Dark = 2 };

inline const char* PreviewShapeName(PreviewShape s)
{
    switch (s)
    {
    case PreviewShape::Sphere:   return "sphere";
    case PreviewShape::Plane:    return "plane";
    case PreviewShape::Cylinder: return "cylinder";
    case PreviewShape::Cube:     return "cube";
    }
    return "sphere";
}
inline const char* PreviewEnvName(PreviewEnv e)
{
    switch (e)
    {
    case PreviewEnv::Studio:  return "studio";
    case PreviewEnv::Outdoor: return "outdoor";
    case PreviewEnv::Dark:    return "dark";
    }
    return "studio";
}
inline bool ParsePreviewShape(const std::string& s, PreviewShape& out)
{
    for (int i = 0; i < 4; ++i)
        if (s == PreviewShapeName(static_cast<PreviewShape>(i))) { out = static_cast<PreviewShape>(i); return true; }
    return false;
}
inline bool ParsePreviewEnv(const std::string& s, PreviewEnv& out)
{
    for (int i = 0; i < 3; ++i)
        if (s == PreviewEnvName(static_cast<PreviewEnv>(i))) { out = static_cast<PreviewEnv>(i); return true; }
    return false;
}

struct PreviewSettings
{
    PreviewShape shape = PreviewShape::Sphere;
    PreviewEnv   env = PreviewEnv::Studio;
    float yaw = 0.55f;          // ラジアン。オブジェクトの回転（オービット）
    float pitch = 0.22f;
    float dist = 3.6f;          // カメラ距離（オブジェクトの外接球の半径 ≒ 1）
    float lightYaw = -0.75f;    // ライトの向き（ラジアン。ワールド）
    float lightPitch = 0.85f;
    bool  autoRotate = false;
    float autoRotateSpeed = 0.35f;   // rad / 秒

    void Clamp()
    {
        auto fin = [](float v, float d) { return std::isfinite(v) ? v : d; };
        yaw = fin(yaw, 0.55f);
        pitch = std::min(1.45f, std::max(-1.45f, fin(pitch, 0.22f)));
        dist = std::min(8.0f, std::max(1.8f, fin(dist, 3.6f)));
        lightYaw = fin(lightYaw, -0.75f);
        lightPitch = std::min(1.5f, std::max(-0.2f, fin(lightPitch, 0.85f)));
        autoRotateSpeed = std::min(3.0f, std::max(0.0f, fin(autoRotateSpeed, 0.35f)));
        // 角度は 2π に畳む（保存値が増え続けない）
        const float tp = 6.28318530718f;
        yaw = std::fmod(yaw, tp);
        lightYaw = std::fmod(lightYaw, tp);
    }

    // "shape=sphere;env=studio;yaw=0.55;..." 形式（エディタ設定 1 キーに入る）
    std::string ToString() const
    {
        char b[320];
        std::snprintf(b, sizeof(b), "shape=%s;env=%s;yaw=%.4f;pitch=%.4f;dist=%.4f;lyaw=%.4f;lpitch=%.4f;auto=%d;aspd=%.3f",
                      PreviewShapeName(shape), PreviewEnvName(env), static_cast<double>(yaw), static_cast<double>(pitch), static_cast<double>(dist),
                      static_cast<double>(lightYaw), static_cast<double>(lightPitch), autoRotate ? 1 : 0, static_cast<double>(autoRotateSpeed));
        return b;
    }
    // 壊れた文字列 / 未知のキーがあっても、読めたものだけ反映する（既定へ落ちる）。1 個でも読めたら true
    bool FromString(const std::string& s)
    {
        bool any = false;
        size_t p = 0;
        while (p < s.size())
        {
            size_t e = s.find(';', p);
            if (e == std::string::npos) e = s.size();
            const std::string kv = s.substr(p, e - p);
            p = e + 1;
            const size_t q = kv.find('=');
            if (q == std::string::npos) continue;
            const std::string k = kv.substr(0, q), v = kv.substr(q + 1);
            auto num = [&](float& dst) { char* end = nullptr; const float f = std::strtof(v.c_str(), &end); if (end != v.c_str()) { dst = f; any = true; } };
            if (k == "shape") { PreviewShape t; if (ParsePreviewShape(v, t)) { shape = t; any = true; } }
            else if (k == "env") { PreviewEnv t; if (ParsePreviewEnv(v, t)) { env = t; any = true; } }
            else if (k == "yaw") num(yaw);
            else if (k == "pitch") num(pitch);
            else if (k == "dist") num(dist);
            else if (k == "lyaw") num(lightYaw);
            else if (k == "lpitch") num(lightPitch);
            else if (k == "auto") { autoRotate = std::atoi(v.c_str()) != 0; any = true; }
            else if (k == "aspd") num(autoRotateSpeed);
        }
        Clamp();
        return any;
    }
    bool operator==(const PreviewSettings& o) const { return ToString() == o.ToString(); }
};

} // namespace dx12e::matgraph
