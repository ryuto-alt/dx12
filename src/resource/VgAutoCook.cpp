#include "resource/VgAutoCook.h"
#include "resource/TextureLoader.h"

#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/SafeRemove.h"
#include "core/vfs/Vfs.h"
#include "renderer/vg/VgeoFormat.h"
#include "renderer/vg/VgsrcFormat.h"
#include "VgeoCook.h"                   // tools/vgeo_cook（VgeoCookLib）

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>

#include <Windows.h>
#include <objbase.h>

namespace dx12e::vgauto
{
namespace fs = std::filesystem;

namespace
{
constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;
// FBX / OBJ は読むまで三角形数が分からない。これより小さいファイルは "auto" では裏の cook を始めない（20 万三角形 ≈ 10 MB 前後）。
constexpr uint64_t kMinNonGltfBytes = 8ull << 20;

uint64_t Fnv(uint64_t h, const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * kFnvPrime;
    return h;
}
uint64_t Fnv(uint64_t h, const std::string& s) { return Fnv(h, s.data(), s.size()); }

std::string Hex(uint64_t v)
{
    char b[20];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}
bool ParseHex(const std::string& s, uint64_t& v)
{
    if (s.empty() || s.size() > 16) return false;
    char* e = nullptr;
    v = std::strtoull(s.c_str(), &e, 16);
    return e && *e == '\0';
}

fs::path U8Path(const std::string& s) { return fs::path(PathResolver::Utf8ToWide(s)); }
std::string PathU8(const fs::path& p) { const std::u8string u = p.generic_u8string(); return std::string(u.begin(), u.end()); }

// 正規化（絶対・"/" 区切り・".." 無し）。ResourceManager::NormalizeModelKey と同じ式（weakly_canonical）。
std::string NormalizeModel(const fs::path& p)
{
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    if (ec || c.empty()) c = p.lexically_normal();
    return PathU8(c);
}
std::string LowerAscii(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
std::string LowerExt(const fs::path& p)
{
    std::string e = LowerAscii(PathU8(p.extension()));
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return e;
}
bool IsCandidateExt(const std::string& e) { return e == "glb" || e == "gltf" || e == "fbx" || e == "obj"; }

// <assets>/.modelcache/vg/（末尾 "/" なしの path）。配布ゲーム / 作れないときは空。
fs::path CacheDir()
{
    if (vfs::InGameMode()) return {};
    const fs::path dir = U8Path(PathResolver::AssetsDir() + ".modelcache/vg");
    std::error_code ec;
    if (!fs::exists(dir, ec)) fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) return {};
    return dir;
}

bool ReadWhole(const fs::path& p, std::string& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
bool WriteAtomically(const fs::path& p, const void* data, size_t n)
{
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

std::string ImportJsonOf(const fs::path& model)
{
    fs::path p = model;
    p += L".import.json";
    std::string s;
    ReadWhole(p, s);
    return s;
}

bool StampOf(const fs::path& model, uint64_t& size, int64_t& mtime)
{
    std::error_code ec;
    size = fs::file_size(model, ec);
    if (ec) return false;
    const auto t = fs::last_write_time(model, ec);
    if (ec) return false;
    mtime = static_cast<int64_t>(t.time_since_epoch().count());
    return true;
}

// cook パラメータ（自動 cook はいつも既定値。threads / lowPriority / collectStats は出力に影響しないので鍵に入らない）
vg::cook::CookOptions MakeCookOptions(const Config& cfg, const std::string& label)
{
    vg::cook::CookOptions co;
    const unsigned logical = std::max(1u, std::thread::hardware_concurrency());
    co.threads = cfg.threads ? std::min(cfg.threads, 12u) : std::clamp(logical / 4u, 1u, 12u);
    co.lowPriority = true;
    co.collectStats = false;
    co.label = label;
    return co;
}

// ── 覚え書き（パスごと: サイズ + 更新時刻 → 中身のハッシュ / 直近の鍵 / 性質）──
struct Memo
{
    uint64_t size = 0;
    int64_t  mtime = 0;
    uint64_t content = 0;
    std::string key;                   // 直近に作った .vgeo の鍵（古い物の掃除用）
    bool     hasFacts = false;
    Facts    facts;
};
fs::path MemoPath(const fs::path& dir, const std::string& norm)
{
    return dir / ("m" + Hex(Fnv(kFnvBasis, LowerAscii(norm))) + ".json");
}
bool ReadMemo(const fs::path& dir, const std::string& norm, Memo& m)
{
    std::string s;
    if (!ReadWhole(MemoPath(dir, norm), s)) return false;
    const nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
    if (!j.is_object()) return false;
    if (j.value("path", std::string()) != norm) return false;
    m.size  = j.value("size", uint64_t{0});
    m.mtime = j.value("mtime", int64_t{0});
    if (!ParseHex(j.value("content", std::string()), m.content)) return false;
    m.key = j.value("key", std::string());
    if (j.contains("facts") && j["facts"].is_object())
    {
        const auto& f = j["facts"];
        m.hasFacts = true;
        m.facts.triangles   = f.value("triangles", uint64_t{0});
        m.facts.skin        = f.value("skin", false);
        m.facts.animation   = f.value("animation", false);
        m.facts.vertexColor = f.value("vertexColor", false);
        m.facts.nonOpaque   = f.value("nonOpaque", false);
        m.facts.materials   = f.value("materials", 0u);
    }
    return true;
}
void WriteMemo(const fs::path& dir, const std::string& norm, const Memo& m)
{
    nlohmann::json j = {{"path", norm}, {"size", m.size}, {"mtime", m.mtime}, {"content", Hex(m.content)}, {"key", m.key},
                        {"version", kAutoCookVersion}};
    if (m.hasFacts)
        j["facts"] = {{"triangles", m.facts.triangles}, {"skin", m.facts.skin}, {"animation", m.facts.animation},
                      {"vertexColor", m.facts.vertexColor}, {"nonOpaque", m.facts.nonOpaque}, {"materials", m.facts.materials}};
    const std::string s = j.dump(1);
    WriteAtomically(MemoPath(dir, norm), s.data(), s.size());
}

std::string StampString(uint64_t size, int64_t mtime) { return std::to_string(size) + ":" + std::to_string(mtime); }

const char* SlotSuffix(int k) { static const char* s[] = {"a", "n", "r", "e"}; return s[k]; }
std::string SafeExt(std::string e)
{
    e = LowerAscii(e);
    if (e == "jpeg") e = "jpg";
    if (e.empty() || e.size() > 8) return "png";
    for (char c : e) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return "png";
    return e;
}
} // namespace

// ════════════════════════════════════════════════════════════════════════════
// 判定（純粋）
// ════════════════════════════════════════════════════════════════════════════
std::string DecideIneligible(ModelImportOptions::VgMode mode, const Config& cfg, const Facts& f)
{
    if (mode == ModelImportOptions::VgMode::Off) return "import.json の virtualGeometry が \"off\"";
    if (f.skin) return "スキンメッシュ（ボーンあり）は仮想ジオメトリの対象外";
    if (f.animation) return "アニメーションを持つモデルは仮想ジオメトリの対象外";
    if (f.triangles == 0) return "三角形がない";
    if (mode == ModelImportOptions::VgMode::On) return {};
    if (f.triangles < cfg.minTriangles)
        return "三角形数 " + std::to_string(f.triangles) + " < しきい値 " + std::to_string(cfg.minTriangles) + "（settings.json の vg_auto_min_triangles）";
    if (f.nonOpaque) return "アルファクリップ / 半透明の材質を含む（VG は不透明だけ。import.json の virtualGeometry:\"on\" なら NONVG に分けて cook する）";
    if (f.vertexColor) return "頂点カラーを持つ（VG は頂点カラーを持たない。virtualGeometry:\"on\" なら頂点カラーを捨てて cook する）";
    if (f.materials > cfg.maxMaterials)
        return "材質が " + std::to_string(f.materials) + " 個（自動 cook の上限 " + std::to_string(cfg.maxMaterials) + "。virtualGeometry:\"on\" で強制できる）";
    return {};
}

bool PrecheckGltfJson(const std::string& json, Facts& out, float bmin[3], float bmax[3], bool& hasBounds, std::string* err)
{
    out = Facts{};
    hasBounds = false;
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object()) { if (err) *err = "glTF の JSON が読めない"; return false; }
    const nlohmann::json empty = nlohmann::json::array();
    const auto& accessors = j.contains("accessors") && j["accessors"].is_array() ? j["accessors"] : empty;
    const auto& meshes    = j.contains("meshes") && j["meshes"].is_array() ? j["meshes"] : empty;
    const auto& nodes     = j.contains("nodes") && j["nodes"].is_array() ? j["nodes"] : empty;
    const auto& materials = j.contains("materials") && j["materials"].is_array() ? j["materials"] : empty;
    out.skin = j.contains("skins") && j["skins"].is_array() && !j["skins"].empty();
    out.animation = j.contains("animations") && j["animations"].is_array() && !j["animations"].empty();

    auto accCount = [&](const nlohmann::json& idx) -> uint64_t
    {
        if (!idx.is_number_unsigned() && !idx.is_number_integer()) return 0;
        const uint64_t i = idx.get<uint64_t>();
        if (i >= accessors.size() || !accessors[i].is_object()) return 0;
        return accessors[i].value("count", uint64_t{0});
    };
    std::vector<uint64_t> meshTris(meshes.size(), 0);
    std::set<int64_t> usedMats;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t mi = 0; mi < meshes.size(); ++mi)
    {
        const auto& m = meshes[mi];
        if (!m.is_object() || !m.contains("primitives") || !m["primitives"].is_array()) continue;
        for (const auto& p : m["primitives"])
        {
            if (!p.is_object()) continue;
            const int mode = p.value("mode", 4);
            const auto& attrs = p.contains("attributes") && p["attributes"].is_object() ? p["attributes"] : nlohmann::json::object();
            if (attrs.contains("COLOR_0")) out.vertexColor = true;
            const uint64_t nPos = attrs.contains("POSITION") ? accCount(attrs["POSITION"]) : 0;
            const uint64_t n = p.contains("indices") ? accCount(p["indices"]) : nPos;
            uint64_t t = 0;
            if (mode == 4) t = n / 3;
            else if (mode == 5 || mode == 6) t = n >= 3 ? n - 2 : 0;
            meshTris[mi] += t;
            if (t > 0) usedMats.insert(p.contains("material") && p["material"].is_number_integer() ? p["material"].get<int64_t>() : -1);
            if (attrs.contains("POSITION") && attrs["POSITION"].is_number_integer())
            {
                const uint64_t ai = attrs["POSITION"].get<uint64_t>();
                if (ai < accessors.size() && accessors[ai].is_object() && accessors[ai].contains("min") && accessors[ai].contains("max")
                    && accessors[ai]["min"].is_array() && accessors[ai]["max"].is_array() && accessors[ai]["min"].size() >= 3 && accessors[ai]["max"].size() >= 3)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        if (!accessors[ai]["min"][k].is_number() || !accessors[ai]["max"][k].is_number()) continue;
                        lo[k] = std::min(lo[k], accessors[ai]["min"][k].get<float>());
                        hi[k] = std::max(hi[k], accessors[ai]["max"][k].get<float>());
                    }
                    hasBounds = true;
                }
            }
        }
    }
    // ノードが参照するメッシュ（同じメッシュを複数ノードが使えば、その数だけ。ModelLoader のノード展開と同じ）
    std::vector<bool> referenced(meshes.size(), false);
    for (const auto& n : nodes)
    {
        if (!n.is_object()) continue;
        if (n.contains("skin")) out.skin = true;
        if (n.contains("mesh") && n["mesh"].is_number_integer())
        {
            const uint64_t mi = n["mesh"].get<uint64_t>();
            if (mi < meshes.size()) { out.triangles += meshTris[mi]; referenced[mi] = true; }
        }
    }
    for (size_t mi = 0; mi < meshes.size(); ++mi) if (!referenced[mi]) out.triangles += meshTris[mi];
    for (int64_t mi : usedMats)
    {
        if (mi < 0 || static_cast<uint64_t>(mi) >= materials.size() || !materials[static_cast<size_t>(mi)].is_object()) continue;
        const std::string am = materials[static_cast<size_t>(mi)].value("alphaMode", std::string("OPAQUE"));
        if (am == "MASK" || am == "BLEND") out.nonOpaque = true;
    }
    out.materials = static_cast<uint32_t>(usedMats.size());
    if (hasBounds) for (int k = 0; k < 3; ++k) { bmin[k] = lo[k]; bmax[k] = hi[k]; }
    return true;
}

bool PrecheckGltfFile(const fs::path& path, Facts& out, float bmin[3], float bmax[3], bool& hasBounds, std::string* err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "開けない"; return false; }
    uint32_t hdr[3] = {0, 0, 0};
    f.read(reinterpret_cast<char*>(hdr), 12);
    std::string json;
    if (f && hdr[0] == 0x46546C67u)   // "glTF"（.glb）
    {
        uint32_t ch[2] = {0, 0};
        f.read(reinterpret_cast<char*>(ch), 8);
        if (!f || ch[1] != 0x4E4F534Au || ch[0] == 0 || ch[0] > (256u << 20)) { if (err) *err = ".glb の JSON チャンクが無い"; return false; }
        json.resize(ch[0]);
        f.read(json.data(), static_cast<std::streamsize>(ch[0]));
        if (!f) { if (err) *err = ".glb の JSON チャンクが切れている"; return false; }
    }
    else
    {
        f.clear();
        f.seekg(0);
        json.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    return PrecheckGltfJson(json, out, bmin, bmax, hasBounds, err);
}

uint64_t ComputeKey(uint64_t contentHash, const std::string& importJson, const std::string& cookParams)
{
    uint64_t h = kFnvBasis;
    h = Fnv(h, &contentHash, sizeof contentHash);
    const uint32_t ver = kAutoCookVersion;
    h = Fnv(h, &ver, sizeof ver);
    h = Fnv(h, std::string("|import|"));
    h = Fnv(h, importJson);
    h = Fnv(h, std::string("|cook|"));
    h = Fnv(h, cookParams);
    return h == 0 ? 1 : h;
}

uint64_t HashFileContents(const fs::path& path, std::atomic<bool>* cancel)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    std::vector<char> buf(8u << 20);
    // 64bit 語ごとの乗算ハッシュ（FNV を 1 バイトずつ回すと 600 MB で数秒かかる）。端数は FNV で混ぜる。
    uint64_t h = kFnvBasis;
    uint64_t total = 0;
    while (f)
    {
        if (cancel && cancel->load()) return 0;
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const size_t n = static_cast<size_t>(f.gcount());
        if (n == 0) break;
        size_t i = 0;
        for (; i + 8 <= n; i += 8)
        {
            uint64_t w;
            std::memcpy(&w, buf.data() + i, 8);
            h = (h ^ w) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
        }
        h = Fnv(h, buf.data() + i, n - i);
        total += n;
    }
    h = Fnv(h, &total, sizeof total);
    return h == 0 ? 1 : h;
}

const char* StateName(State s)
{
    switch (s)
    {
    case State::NotCandidate: return "notCandidate";
    case State::Ready: return "ready";
    case State::Pending: return "pending";
    case State::Ineligible: return "ineligible";
    case State::Failed: return "failed";
    }
    return "?";
}

// ════════════════════════════════════════════════════════════════════════════
// Cooker
// ════════════════════════════════════════════════════════════════════════════
Cooker& Cooker::Get()
{
    // ★わざと解放しない（終了時に cook のスレッドを切り離すので、静的な破棄で mutex を壊すと切り離したスレッドが踏む）
    static Cooker* c = new Cooker();
    return *c;
}

Cooker::~Cooker() { Shutdown(); }

void Cooker::SetConfig(const Config& c)
{
    std::lock_guard lk(m_mutex);
    m_cfg = c;
    m_ineligibleCache.clear();   // しきい値が変わったら判定し直す
}

Config Cooker::GetConfig() const
{
    std::lock_guard lk(m_mutex);
    return m_cfg;
}

uint32_t Cooker::ActiveJobs() const
{
    std::lock_guard lk(m_mutex);
    return static_cast<uint32_t>(m_active.size());
}

void Cooker::Shutdown()
{
    m_cancel = true;
    {
        std::lock_guard lk(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    // ★cook（数十秒）の途中では止められない。終了を待たせないよう切り離す（プロセスの終了で消える。書きかけは *.tmp のまま残り、
    //   次の cook が上書きする）。待ち行列だけの間なら即座に抜ける。
    if (m_worker.joinable()) m_worker.detach();
}

void Cooker::StartJobLocked(const std::string& model, bool manual, bool force)
{
    if (m_quit) return;
    auto it = m_active.find(model);
    if (it != m_active.end()) { if (manual) { it->second->manual = true; it->second->force |= force; } return; }
    auto j = std::make_shared<Job>();
    j->model = model;
    j->manual = manual;
    j->force = force;
    j->t0 = std::chrono::steady_clock::now();
    m_active.emplace(model, j);
    m_queue.push_back(j);
    if (!m_worker.joinable()) m_worker = std::thread([this] { WorkerMain(); });
    m_cv.notify_one();
}

QueryResult Cooker::Query(const fs::path& model)
{
    QueryResult q;
    Config cfg;
    {
        std::lock_guard lk(m_mutex);
        cfg = m_cfg;
        if (m_quit) return q;
    }
    if (!cfg.enabled || vfs::InGameMode()) return q;
    const std::string ext = LowerExt(model);
    if (!IsCandidateExt(ext)) return q;
    const ModelImportOptions opts = ModelImportOptions::LoadFor(model);
    if (opts.virtualGeometry == ModelImportOptions::VgMode::Off) { q.state = State::Ineligible; q.reason = DecideIneligible(opts.virtualGeometry, cfg, {}); return q; }
    const fs::path dir = CacheDir();
    if (dir.empty()) return q;
    const std::string norm = NormalizeModel(model);
    const bool isGltf = (ext == "glb" || ext == "gltf");
    auto boundsFromGltf = [&]()
    {
        if (!isGltf) return;
        Facts f;
        bool hb = false;
        float lo[3], hi[3];
        if (PrecheckGltfFile(model, f, lo, hi, hb, nullptr) && hb)
        {
            q.hasBounds = true;
            for (int k = 0; k < 3; ++k) { q.bmin[k] = lo[k]; q.bmax[k] = hi[k]; }
            if (opts.leftHanded) { const float a = -q.bmax[2], b = -q.bmin[2]; q.bmin[2] = a; q.bmax[2] = b; }   // MakeLeftHanded は z を反転する
        }
    };
    {
        std::lock_guard lk(m_mutex);
        if (m_active.count(norm)) { q.state = State::Pending; }
    }
    if (q.state == State::Pending) { boundsFromGltf(); return q; }

    uint64_t size = 0;
    int64_t mtime = 0;
    if (!StampOf(model, size, mtime)) return q;
    const std::string stamp = StampString(size, mtime);
    const std::string importJson = ImportJsonOf(model);
    const std::string params = MakeCookOptions(cfg, "x").CanonicalString();

    Memo memo;
    if (ReadMemo(dir, norm, memo) && memo.size == size && memo.mtime == mtime)
    {
        if (memo.hasFacts)
        {
            const std::string why = DecideIneligible(opts.virtualGeometry, cfg, memo.facts);
            if (!why.empty()) { q.state = State::Ineligible; q.reason = why; return q; }
        }
        const uint64_t key = ComputeKey(memo.content, importJson, params);
        const fs::path vgeo = dir / ("v" + Hex(key) + ".vgeo");
        std::error_code ec;
        if (fs::exists(vgeo, ec))
        {
            q.state = State::Ready;
            q.vgeoPath = PathU8(vgeo);
            return q;
        }
    }
    {
        std::lock_guard lk(m_mutex);
        auto it = m_ineligibleCache.find(norm);
        if (it != m_ineligibleCache.end() && it->second.first == stamp) { q.state = State::Ineligible; q.reason = it->second.second; return q; }
    }
    // まだ cook していない: 安く分かる範囲で判定してから裏で始める
    std::string why;
    if (isGltf)
    {
        Facts f;
        bool hb = false;
        float lo[3], hi[3];
        std::string perr;
        if (!PrecheckGltfFile(model, f, lo, hi, hb, &perr)) return q;   // 読めない glTF は通常の読み込みに任せる（そちらがエラーを出す）
        why = DecideIneligible(opts.virtualGeometry, cfg, f);
        if (why.empty() && hb)
        {
            q.hasBounds = true;
            for (int k = 0; k < 3; ++k) { q.bmin[k] = lo[k]; q.bmax[k] = hi[k]; }
            if (opts.leftHanded) { const float a = -q.bmax[2], b = -q.bmin[2]; q.bmin[2] = a; q.bmax[2] = b; }
        }
    }
    else if (opts.virtualGeometry == ModelImportOptions::VgMode::Auto && size < kMinNonGltfBytes)
    {
        why = "ファイルが小さい（" + std::to_string(size >> 10) + " KB < 8 MB。FBX / OBJ は読むまで三角形数が分からないので、小さい物は試さない）";
    }
    std::lock_guard lk(m_mutex);
    if (!why.empty())
    {
        m_ineligibleCache[norm] = {stamp, why};
        q.state = State::Ineligible;
        q.reason = why;
        return q;
    }
    StartJobLocked(norm, false, false);
    q.state = State::Pending;
    Logger::Info("仮想ジオメトリ: 自動 cook を裏で始めました（{}）。終わるまで仮の箱で表示します", norm);
    return q;
}

bool Cooker::Request(const fs::path& model, bool force, std::string* err)
{
    if (vfs::InGameMode()) { if (err) *err = "配布ゲームでは cook できない"; return false; }
    std::error_code ec;
    if (!fs::exists(model, ec)) { if (err) *err = "ファイルが無い: " + PathU8(model); return false; }
    if (!IsCandidateExt(LowerExt(model))) { if (err) *err = "glb / gltf / fbx / obj だけ cook できる"; return false; }
    if (CacheDir().empty()) { if (err) *err = "assets/.modelcache/vg を作れない"; return false; }
    const std::string norm = NormalizeModel(model);
    std::lock_guard lk(m_mutex);
    if (m_quit) { if (err) *err = "終了中"; return false; }
    m_ineligibleCache.erase(norm);
    StartJobLocked(norm, true, force);
    return true;
}

std::vector<std::string> Cooker::TakeFinished()
{
    std::lock_guard lk(m_mutex);
    std::vector<std::string> out;
    out.swap(m_finished);
    return out;
}

void Cooker::SetStage(Job& j, const char* s)
{
    std::lock_guard lk(m_mutex);
    j.stage = s;
}

void Cooker::WorkerMain()
{
    // ★PC の操作を奪わない: 低優先度。cook の並列は CookOptions.lowPriority / threads（論理コア / 4、上限 12）。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // テクスチャの先読み（WIC）が要る
    for (;;)
    {
        std::shared_ptr<Job> j;
        {
            std::unique_lock lk(m_mutex);
            m_cv.wait(lk, [&] { return m_quit || !m_queue.empty(); });
            if (m_quit) { if (SUCCEEDED(coHr)) CoUninitialize(); return; }
            j = m_queue.front();
            m_queue.pop_front();
            j->t0 = std::chrono::steady_clock::now();
        }
        try { RunJob(*j); }
        catch (const std::exception& e) { j->result = State::Failed; j->reason = std::string("例外: ") + e.what(); }
        catch (...) { j->result = State::Failed; j->reason = "例外"; }
        {
            std::lock_guard lk(m_mutex);
            j->done = true;
            j->stage = "done";
            j->tEnd = std::chrono::steady_clock::now();
            m_active.erase(j->model);
            m_history.push_back(j);
            if (m_history.size() > 32) m_history.erase(m_history.begin());
            if (!m_quit) m_finished.push_back(j->model);
        }
        const double sec = std::chrono::duration<double>(j->tEnd - j->t0).count();
        if (j->result == State::Ready)
            Logger::Info("仮想ジオメトリ: 自動 cook 完了 {}（{} 三角形・材質 {}・{:.1f} 秒 = ハッシュ {:.1f} / 読込 {:.1f} / cook {:.1f} / 書出 {:.1f}）→ {}",
                         j->model, j->triangles, j->materials, sec, j->secHash, j->secRead, j->secCook, j->secWrite, j->vgeoPath);
        else if (j->result == State::Ineligible)
            Logger::Info("仮想ジオメトリ: 自動 cook の対象外 {}（{}）。通常メッシュで描きます", j->model, j->reason);
        else
            Logger::Warn("仮想ジオメトリ: 自動 cook に失敗 {}（{}）。通常メッシュで描きます", j->model, j->reason);
    }
}

void Cooker::RunJob(Job& j)
{
    using clock = std::chrono::steady_clock;
    const auto since = [](clock::time_point t) { return std::chrono::duration<double>(clock::now() - t).count(); };
    const fs::path model = U8Path(j.model);
    const fs::path dir = CacheDir();
    if (dir.empty()) { j.result = State::Failed; j.reason = "キャッシュのフォルダを作れない"; return; }
    Config cfg = GetConfig();
    const ModelImportOptions opts = ModelImportOptions::LoadFor(model);
    const ModelImportOptions::VgMode mode = j.manual ? ModelImportOptions::VgMode::On : opts.virtualGeometry;

    // ── 1. 中身のハッシュ → 鍵 ──
    SetStage(j, "hash");
    auto t = clock::now();
    uint64_t size = 0;
    int64_t mtime = 0;
    if (!StampOf(model, size, mtime)) { j.result = State::Failed; j.reason = "ファイルの情報が取れない"; return; }
    Memo memo;
    const bool memoOk = ReadMemo(dir, j.model, memo) && memo.size == size && memo.mtime == mtime;
    const std::string prevKey = memo.key;
    uint64_t content = memoOk ? memo.content : 0;
    if (content == 0) content = HashFileContents(model, &m_cancel);
    if (content == 0) { j.result = State::Failed; j.reason = m_cancel ? "中止" : "読めない"; return; }
    memo.size = size; memo.mtime = mtime; memo.content = content;
    if (!memoOk) memo.hasFacts = false;
    j.secHash = since(t);
    const std::string label = PathU8(model.stem());
    const vg::cook::CookOptions co = MakeCookOptions(cfg, label);
    const uint64_t key = ComputeKey(content, ImportJsonOf(model), co.CanonicalString());
    const std::string keyHex = Hex(key);
    const fs::path vgeo = dir / ("v" + keyHex + ".vgeo");
    std::error_code ec;
    if (!j.force && fs::exists(vgeo, ec))
    {
        if (memo.hasFacts && !j.manual)
        {
            const std::string why = DecideIneligible(mode, cfg, memo.facts);
            if (!why.empty()) { j.result = State::Ineligible; j.reason = why; return; }
        }
        memo.key = keyHex;
        WriteMemo(dir, j.model, memo);
        j.result = State::Ready;
        j.vgeoPath = PathU8(vgeo);
        return;
    }

    // ── 2. エンジンと同じ読み込み（Assimp）→ 頂点・材質 ──
    SetStage(j, "read");
    t = clock::now();
    StaticModelCpu cpu;
    std::string err;
    if (!ModelLoader::ExtractStaticForVg(model, cpu, &err))
    {
        j.result = State::Failed;
        j.reason = err;
        return;
    }
    j.secRead = since(t);
    Facts f;
    f.triangles = cpu.triangles;
    f.skin = cpu.hasSkin;
    f.animation = cpu.hasAnimation;
    f.vertexColor = cpu.hasVertexColor;
    f.materials = static_cast<uint32_t>(cpu.materials.size());
    for (const auto& m : cpu.materials) if (m.alphaMode != AlphaMode::Opaque) f.nonOpaque = true;
    memo.hasFacts = true;
    memo.facts = f;
    WriteMemo(dir, j.model, memo);
    j.triangles = f.triangles;
    j.materials = f.materials;
    if (const std::string why = DecideIneligible(mode, cfg, f); !why.empty()) { j.result = State::Ineligible; j.reason = why; return; }
    if (cpu.hasVertexColor) Logger::Warn("仮想ジオメトリ: {} の頂点カラーは VG では使われません（virtualGeometry:\"on\"）", j.model);
    if (m_cancel) { j.result = State::Failed; j.reason = "中止"; return; }

    // ── 3. テクスチャを .vgeo の隣へ（.vgeo の材質は「.vgeo のフォルダ基準の相対パス」しか持てない）──
    SetStage(j, "textures");
    const fs::path texDir = dir / ("v" + keyHex);
    fs::create_directories(texDir, ec);
    vg::VgsrcData src;
    src.flags = vg::kVgsrcHasNormals | vg::kVgsrcHasUV0;
    src.coordSystem = vg::kCoordEngine;
    src.unitScaleToMeters = 1.0f;
    src.sourceHash = content;
    vg::StringPool pool;
    for (size_t mi = 0; mi < cpu.materials.size(); ++mi)
    {
        const StaticMaterialCpu& m = cpu.materials[mi];
        vg::MaterialRecord r = vg::MakeDefaultMaterial();
        r.nameOff = pool.Add(m.name.empty() ? ("material" + std::to_string(mi)) : m.name);
        const StaticTexSource* slots[4] = {&m.albedo, &m.normal, &m.metalRough, &m.emissive};
        uint32_t* offs[4] = {&r.albedoPathOff, &r.normalPathOff, &r.metalRoughPathOff, &r.emissivePathOff};
        bool have[4] = {false, false, false, false};
        for (int k = 0; k < 4; ++k)
        {
            const StaticTexSource& s = *slots[k];
            if (s.Empty()) continue;
            const std::string name = "m" + std::to_string(mi) + "_" + SlotSuffix(k) + "." + SafeExt(s.ext);
            const fs::path dst = texDir / name;
            bool ok = false;
            if (!s.bytes.empty()) ok = WriteAtomically(dst, s.bytes.data(), s.bytes.size());
            else
            {
                fs::copy_file(s.file, dst, fs::copy_options::overwrite_existing, ec);
                ok = !ec;
                if (!ok)
                {
                    // pak の中など、ディスクに無い元テクスチャ
                    const std::vector<uint8_t> b = vfs::ReadAssetAbs(s.file.wstring());
                    ok = !b.empty() && WriteAtomically(dst, b.data(), b.size());
                }
            }
            if (!ok) { Logger::Warn("仮想ジオメトリ: テクスチャを書き出せません（{}）", PathU8(dst)); continue; }
            *offs[k] = pool.Add("v" + keyHex + "/" + name);
            have[k] = true;
        }
        r.flags = 0;
        if (have[1]) r.flags |= vg::kMatHasNormalMap;
        if (have[2]) r.flags |= vg::kMatHasMetalRough;
        if (have[3]) r.flags |= vg::kMatHasEmissiveTex;
        if (m.doubleSided) r.flags |= vg::kMatDoubleSided;
        if (m.alphaMode == AlphaMode::Mask) r.flags |= vg::kMatAlphaTest;
        if (m.alphaMode == AlphaMode::Blend) r.flags |= vg::kMatBlend;
        if (m.occlusionInMR && have[2]) { r.flags |= vg::kMatAoInMR; r.aoStrength = std::clamp(m.aoStrength, 0.0f, 1.0f); }
        r.metallic = m.metallic;
        r.roughness = m.roughness;
        std::memcpy(r.emissiveColor, m.emissiveColor, sizeof r.emissiveColor);
        r.emissiveIntensity = m.emissiveIntensity;
        r.alphaCutoff = m.alphaCutoff;
        r.baseColorAlpha = m.baseColorAlpha;
        src.materials.push_back(r);
    }
    src.strings = pool.Data();
    // テクスチャの BC 圧縮キャッシュ（.texcache）を裏で作っておく。差し替えの瞬間（メインスレッド）に 4K の BC7 圧縮を
    //   何枚も踏むと数十秒固まるため。引数（sRGB / 用途）は VgeoProxyLoader・VirtualGeometryBindMaterials と同じ。
    SetStage(j, "texcache");
    for (const vg::MaterialRecord& r : src.materials)
    {
        const struct { uint32_t off; bool srgb; TextureUsage usage; } ts[4] = {
            {r.albedoPathOff, true, TextureUsage::BaseColor}, {r.normalPathOff, false, TextureUsage::Normal},
            {r.metalRoughPathOff, false, TextureUsage::NonColor}, {r.emissivePathOff, true, TextureUsage::BaseColor}};
        for (const auto& tx : ts)
        {
            if (tx.off == vg::kNone || tx.off >= src.strings.size() || m_cancel) continue;
            const fs::path p = (dir / U8Path(std::string(src.strings.data() + tx.off))).lexically_normal();
            TextureLoader::PrewarmCompressedCache(fs::path(p.generic_wstring()).wstring(), tx.srgb, tx.usage);
        }
    }
    for (const auto& s : cpu.sections) src.sections.push_back(vg::VgsrcSection{s.material, s.firstIndex, s.indexCount, 0u});
    src.positions = std::move(cpu.positions);
    src.normals = std::move(cpu.normals);
    src.uv0 = std::move(cpu.uvs);
    src.indices = std::move(cpu.indices);
    cpu = StaticModelCpu{};

    // ── 4. cook（tools/vgeo_cook と同じ）──
    SetStage(j, "cook");
    t = clock::now();
    vg::VgeoContent out;
    vg::cook::CookStats st;
    if (const vg::VgeoError e = vg::cook::Cook(std::move(src), co, out, &st); !e.ok())
    {
        j.result = State::Failed;
        j.reason = "cook 失敗: " + e.ToString();
        return;
    }
    j.secCook = since(t);
    j.clusters = st.clusters;
    j.levels = st.levels;
    if (m_cancel) { j.result = State::Failed; j.reason = "中止"; return; }

    // ── 5. 書き出し → 厳密検証 → 置き換え ──
    SetStage(j, "write");
    t = clock::now();
    fs::path tmp = vgeo;
    tmp += L".tmp";
    const std::string tmpU8 = PathU8(tmp);
    if (const vg::VgeoError e = vg::WriteVgeoToFile(out, tmpU8); !e.ok())
    {
        fs::remove(tmp, ec);
        j.result = State::Failed;
        j.reason = "書き出し失敗: " + e.ToString();
        return;
    }
    out = vg::VgeoContent{};
    {
        std::string verr;
        {
            vg::FileSource fsrc;
            if (!fsrc.Open(tmpU8)) verr = "書いたファイルを開けない";
            else
            {
                j.fileBytes = fsrc.Size();
                vg::ReadOptions ro;
                ro.strictReserved = true;
                ro.deep = false;  // 全頂点のデコード検査は cooker のテスト（VgeoCookTests）の仕事。ここは形・CRC・DAG だけ（数百 MB で数秒かかるため）
                const vg::ValidationReport rep = vg::ValidateVgeo(fsrc, ro);
                if (!rep.ok()) verr = "検証に失敗: " + rep.issues.front().ToString();
            }
        }   // ★ここで閉じる（開いたままだと消せない）
        if (!verr.empty())
        {
            fs::remove(tmp, ec);
            j.result = State::Failed;
            j.reason = verr;
            return;
        }
    }
    fs::rename(tmp, vgeo, ec);
    if (ec) { fs::remove(tmp, ec); j.result = State::Failed; j.reason = "置き換えに失敗"; return; }
    j.secWrite = since(t);

    // 同じモデルの古い鍵の出力を消す（モデルを編集するたびに数百 MB ずつ溜まらないように）
    if (!prevKey.empty() && prevKey != keyHex && prevKey.size() == 16)
    {
        fs::remove(dir / ("v" + prevKey + ".vgeo"), ec);
        const fs::path oldTex = dir / ("v" + prevKey);
        if (fs::is_directory(oldTex, ec)) saferm::RemoveAll(oldTex, ec, dir);
    }
    memo.key = keyHex;
    WriteMemo(dir, j.model, memo);
    j.result = State::Ready;
    j.vgeoPath = PathU8(vgeo);
}

nlohmann::json Cooker::StatusJson() const
{
    std::lock_guard lk(m_mutex);
    nlohmann::json j;
    j["enabled"] = m_cfg.enabled;
    j["minTriangles"] = m_cfg.minTriangles;
    j["maxMaterials"] = m_cfg.maxMaterials;
    const auto now = std::chrono::steady_clock::now();
    auto jobJson = [&](const Job& x)
    {
        const auto end = x.done ? x.tEnd : now;
        nlohmann::json o = {{"model", x.model}, {"stage", x.stage}, {"manual", x.manual},
                            {"elapsedSec", std::round(std::chrono::duration<double>(end - x.t0).count() * 10.0) / 10.0}};
        if (x.done)
        {
            o["result"] = StateName(x.result);
            if (!x.reason.empty()) o["reason"] = x.reason;
            if (!x.vgeoPath.empty()) o["vgeo"] = x.vgeoPath;
            o["triangles"] = x.triangles;
            o["materials"] = x.materials;
            o["clusters"] = x.clusters;
            o["levels"] = x.levels;
            o["fileMB"] = std::round(static_cast<double>(x.fileBytes) / 1048576.0 * 10.0) / 10.0;
            o["sec"] = {{"hash", std::round(x.secHash * 10) / 10}, {"read", std::round(x.secRead * 10) / 10},
                        {"cook", std::round(x.secCook * 10) / 10}, {"write", std::round(x.secWrite * 10) / 10}};
        }
        return o;
    };
    nlohmann::json active = nlohmann::json::array();
    for (const auto& [k, v] : m_active) active.push_back(jobJson(*v));
    nlohmann::json hist = nlohmann::json::array();
    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) hist.push_back(jobJson(**it));
    nlohmann::json inel = nlohmann::json::array();
    for (const auto& [k, v] : m_ineligibleCache) if (inel.size() < 16) inel.push_back({{"model", k}, {"reason", v.second}});
    j["active"] = active;
    j["recent"] = hist;
    j["skipped"] = inel;
    return j;
}

} // namespace dx12e::vgauto
