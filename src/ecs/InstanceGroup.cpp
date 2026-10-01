#include "ecs/InstanceGroup.h"
#include "ecs/Components.h"

#include <algorithm>
#include <charconv>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>

using namespace DirectX;

namespace dx12e::instgroup
{

namespace
{
std::atomic<u64> g_nextSetId{1};

inline u32 BitsOf(f32 f) { u32 u; std::memcpy(&u, &f, 4); return u; }
inline bool Bit3(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return BitsOf(a.x) == BitsOf(b.x) && BitsOf(a.y) == BitsOf(b.y) && BitsOf(a.z) == BitsOf(b.z);
}

thread_local SerializeCollector* t_collector = nullptr;
thread_local std::string        t_loadScenePath;

struct Store
{
    std::mutex mtx;
    std::unordered_map<u64, InstanceSetPtr> map;
    size_t totalInstances = 0;
};
Store& TheStore()
{
    static Store s;
    return s;
}
// 台帳の合計インスタンス数の上限。超えたら誰にも使われていない実体から捨てる（スナップショット 1 つぶんは数十 MB まで）。
constexpr size_t kStoreSoftLimit = 16u * 1024u * 1024u;

void AppendFloat(std::string& out, f32 v)
{
    char buf[48];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, r.ptr);
}
} // namespace

bool BitEqual(const InstanceTRS& a, const InstanceTRS& b)
{
    return Bit3(a.p, b.p) && Bit3(a.r, b.r) && Bit3(a.s, b.s);
}

InstanceSetPtr NewSet(std::vector<InstanceTRS>&& items)
{
    auto s = std::make_shared<InstanceSet>();
    s->items = std::move(items);
    s->id = g_nextSetId.fetch_add(1);
    return s;
}

InstanceSetPtr CloneSet(const InstanceSet& src)
{
    auto s = std::make_shared<InstanceSet>();
    s->items = src.items;
    s->id = g_nextSetId.fetch_add(1);
    return s;
}

XMMATRIX LocalMatrix(const InstanceTRS& t)
{
    // Transform::GetWorldMatrix と同じ式（Components.cpp）。変えるときは両方そろえること。
    const XMMATRIX s = XMMatrixScaling(t.s.x, t.s.y, t.s.z);
    const XMMATRIX r = XMMatrixRotationRollPitchYaw(
        XMConvertToRadians(t.r.x), XMConvertToRadians(t.r.y), XMConvertToRadians(t.r.z));
    const XMMATRIX tr = XMMatrixTranslation(t.p.x, t.p.y, t.p.z);
    return s * r * tr;
}

void ComputeBounds(const InstanceSet& s)
{
    if (s.boundsValid) return;
    if (s.items.empty())
    {
        s.boundsMin = s.boundsMax = XMFLOAT3{0, 0, 0};
    }
    else
    {
        XMFLOAT3 mn = s.items[0].p, mx = s.items[0].p;
        for (const auto& it : s.items)
        {
            mn.x = (std::min)(mn.x, it.p.x); mn.y = (std::min)(mn.y, it.p.y); mn.z = (std::min)(mn.z, it.p.z);
            mx.x = (std::max)(mx.x, it.p.x); mx.y = (std::max)(mx.y, it.p.y); mx.z = (std::max)(mx.z, it.p.z);
        }
        s.boundsMin = mn; s.boundsMax = mx;
    }
    s.boundsValid = true;
}

// ---------------------------------------------------------------------------
// ワールド行列
// ---------------------------------------------------------------------------
const std::vector<XMFLOAT4X4>& WorldMatrices(const entt::registry& reg, entt::entity group, const InstanceSet& set,
                                             XMFLOAT4X4* outGroupWorld)
{
    static thread_local std::vector<XMFLOAT4X4> chain;
    static thread_local std::vector<XMMATRIX>    chainM;
    chain.clear();
    {
        entt::entity cur = group;
        int depth = 0;
        while (cur != entt::null && reg.valid(cur) && reg.all_of<Transform>(cur) && depth++ < 64)
        {
            const auto& t = reg.get<Transform>(cur);
            XMFLOAT4X4 m;
            XMStoreFloat4x4(&m, t.GetWorldMatrix());
            chain.push_back(m);
            cur = t.parent;
        }
    }
    u64 key = 1469598103934665603ull;
    for (const auto& m : chain)
    {
        const u8* q = reinterpret_cast<const u8*>(&m);
        for (size_t i = 0; i < sizeof(m); ++i) { key ^= q[i]; key *= 1099511628211ull; }
    }
    if (!set.worldValid || set.worldKey != key || set.worldCache.size() != set.items.size())
    {
        set.worldCache.resize(set.items.size());
        chainM.resize(chain.size());
        for (size_t k = 0; k < chain.size(); ++k) chainM[k] = XMLoadFloat4x4(&chain[k]);
        for (size_t i = 0; i < set.items.size(); ++i)
        {
            XMMATRIX w = LocalMatrix(set.items[i]);
            for (const auto& cm : chainM) w = w * cm;
            XMStoreFloat4x4(&set.worldCache[i], w);
        }
        set.worldKey   = key;
        set.worldValid = true;
    }
    if (outGroupWorld)
    {
        XMMATRIX gw = XMMatrixIdentity();
        for (const auto& m : chain) gw = gw * XMLoadFloat4x4(&m);
        XMStoreFloat4x4(outGroupWorld, gw);
    }
    return set.worldCache;
}

bool ComputeWorldAabb(const entt::registry& reg, entt::entity group, const InstanceSet& set,
                      const XMFLOAT3& localMin, const XMFLOAT3& localMax, XMFLOAT3& outMin, XMFLOAT3& outMax)
{
    if (set.items.empty()) return false;
    const auto& worlds = WorldMatrices(reg, group, set);
    XMVECTOR mn = XMVectorReplicate(FLT_MAX), mx = XMVectorReplicate(-FLT_MAX);
    for (const auto& wf : worlds)
    {
        const XMMATRIX w = XMLoadFloat4x4(&wf);
        for (int c = 0; c < 8; ++c)
        {
            const XMVECTOR p = XMVector3Transform(
                XMVectorSet((c & 1) ? localMax.x : localMin.x, (c & 2) ? localMax.y : localMin.y, (c & 4) ? localMax.z : localMin.z, 1.0f), w);
            mn = XMVectorMin(mn, p);
            mx = XMVectorMax(mx, p);
        }
    }
    XMStoreFloat3(&outMin, mn);
    XMStoreFloat3(&outMax, mx);
    return true;
}

// ---------------------------------------------------------------------------
// サイドカー
// ---------------------------------------------------------------------------
void AppendLine(std::string& out, const InstanceTRS& t)
{
    // 末尾の既定値の省略はビット一致のときだけ（-0.0 の回転は省かない＝G4 往復で JSON が変わらない）。
    const bool sDefault = BitsOf(t.s.x) == BitsOf(1.0f) && BitsOf(t.s.y) == BitsOf(1.0f) && BitsOf(t.s.z) == BitsOf(1.0f);
    const bool rDefault = BitsOf(t.r.x) == 0u && BitsOf(t.r.y) == 0u && BitsOf(t.r.z) == 0u;
    const int n = !sDefault ? 9 : (!rDefault ? 6 : 3);
    const f32 v[9] = {t.p.x, t.p.y, t.p.z, t.r.x, t.r.y, t.r.z, t.s.x, t.s.y, t.s.z};
    out.push_back('[');
    for (int i = 0; i < n; ++i)
    {
        if (i) out.push_back(',');
        AppendFloat(out, v[i]);
    }
    out.append("]\n");
}

std::string FormatSidecar(const InstanceSet& s)
{
    std::string out;
    out.reserve(s.items.size() * 40);
    for (const auto& it : s.items) AppendLine(out, it);
    return out;
}

bool ParseLine(std::string_view line, InstanceTRS& out)
{
    size_t i = 0;
    const size_t n = line.size();
    auto skipWs = [&] { while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i; };
    skipWs();
    if (i >= n || line[i] != '[') return false;
    ++i;
    f32 v[9];
    int cnt = 0;
    for (;;)
    {
        skipWs();
        if (i >= n) return false;
        if (cnt >= 9) return false;
        f32 x = 0.0f;
        const auto r = std::from_chars(line.data() + i, line.data() + n, x);
        if (r.ec != std::errc{} || r.ptr == line.data() + i) return false;
        if (!std::isfinite(x)) return false;
        v[cnt++] = x;
        i = static_cast<size_t>(r.ptr - line.data());
        skipWs();
        if (i >= n) return false;
        if (line[i] == ',') { ++i; continue; }
        if (line[i] == ']') { ++i; break; }
        return false;
    }
    skipWs();
    if (i != n) return false;
    if (cnt != 3 && cnt != 6 && cnt != 9) return false;
    InstanceTRS t;
    t.p = {v[0], v[1], v[2]};
    if (cnt >= 6) t.r = {v[3], v[4], v[5]};
    if (cnt >= 9) t.s = {v[6], v[7], v[8]};
    out = t;
    return true;
}

InstanceSetPtr ParseSidecar(std::string_view text, ParseReport* report)
{
    ParseReport rep;
    std::vector<InstanceTRS> items;
    items.reserve(text.size() / 40 + 1);
    size_t pos = 0, lineNo = 0;
    while (pos < text.size())
    {
        size_t e = text.find('\n', pos);
        if (e == std::string_view::npos) e = text.size();
        std::string_view line = text.substr(pos, e - pos);
        pos = e + 1;
        ++lineNo;
        // 空行（空白のみを含む）は無視
        size_t k = 0;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t' || line[k] == '\r')) ++k;
        if (k == line.size()) continue;
        ++rep.lines;
        InstanceTRS t;
        if (ParseLine(line, t)) { items.push_back(t); ++rep.ok; }
        else
        {
            ++rep.bad;
            if (rep.badLineNumbers.size() < 8) rep.badLineNumbers.push_back(lineNo);
        }
    }
    if (report) *report = std::move(rep);
    return NewSet(std::move(items));
}

// ---------------------------------------------------------------------------
// 台帳
// ---------------------------------------------------------------------------
void StoreRegister(const InstanceSetPtr& s)
{
    if (!s) return;
    Store& st = TheStore();
    std::lock_guard<std::mutex> lk(st.mtx);
    auto ins = st.map.emplace(s->id, s);
    if (!ins.second) return;
    st.totalInstances += s->items.size();
    if (st.totalInstances > kStoreSoftLimit)
    {
        for (auto it = st.map.begin(); it != st.map.end() && st.totalInstances > kStoreSoftLimit / 2;)
        {
            if (it->second.use_count() == 1 && it->first != s->id)
            {
                st.totalInstances -= it->second->items.size();
                it = st.map.erase(it);
            }
            else ++it;
        }
    }
}

InstanceSetPtr StoreFind(u64 id)
{
    Store& st = TheStore();
    std::lock_guard<std::mutex> lk(st.mtx);
    auto it = st.map.find(id);
    return it == st.map.end() ? nullptr : it->second;
}

size_t StoreEntryCount()
{
    Store& st = TheStore();
    std::lock_guard<std::mutex> lk(st.mtx);
    return st.map.size();
}

void StoreClearForTests()
{
    Store& st = TheStore();
    std::lock_guard<std::mutex> lk(st.mtx);
    st.map.clear();
    st.totalInstances = 0;
}

// ---------------------------------------------------------------------------
// 文脈
// ---------------------------------------------------------------------------
ScopedFileSerialize::ScopedFileSerialize(SerializeCollector& c) : m_prev(t_collector) { t_collector = &c; }
ScopedFileSerialize::~ScopedFileSerialize() { t_collector = m_prev; }
SerializeCollector* CurrentCollector() { return t_collector; }

ScopedLoadScene::ScopedLoadScene(std::string scenePath) : m_prev(std::move(t_loadScenePath)) { t_loadScenePath = std::move(scenePath); }
ScopedLoadScene::~ScopedLoadScene() { t_loadScenePath = std::move(m_prev); }
const std::string& CurrentLoadScenePath() { return t_loadScenePath; }

std::string SidecarDirFor(const std::string& scenePath)
{
    namespace fs = std::filesystem;
    return fs::path(scenePath).replace_extension(".inst").string();
}

std::string SidecarPathFor(const std::string& scenePath, const std::string& guidHex)
{
    namespace fs = std::filesystem;
    return (fs::path(SidecarDirFor(scenePath)) / (guidHex + ".jsonl")).string();
}

} // namespace dx12e::instgroup
