#include "scene/ScenePartition.h"
#include "scene/SceneFormatV2.h"
#include "core/AtomicFileJson.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace dx12e::scenepart
{

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

unsigned ThreadBudget()
{
    const unsigned hw = (std::max)(1u, std::thread::hardware_concurrency());
    return (std::max)(1u, hw / 4);
}

// i = 0..n-1 を workers 本で回す（原子カウンタで取り合う）。例外は join の後で投げ直す
// （スレッドの中で漏れると std::terminate で落ちるため）。workers<=1 は呼び出しスレッドだけ。
template <typename Fn>
static void ParallelFor(size_t n, unsigned workers, Fn&& fn)
{
    if (n == 0) return;
    workers = static_cast<unsigned>((std::min)(static_cast<size_t>(workers), n));
    if (workers <= 1) { for (size_t i = 0; i < n; ++i) fn(i); return; }
    std::atomic<size_t> next{0};
    std::vector<std::exception_ptr> errors(workers);
    auto body = [&](unsigned w) {
        try { for (size_t i; (i = next.fetch_add(1)) < n;) fn(i); }
        catch (...) { errors[w] = std::current_exception(); }
    };
    std::vector<std::thread> threads;
    threads.reserve(workers - 1);
    for (unsigned w = 1; w < workers; ++w) threads.emplace_back(body, w);
    body(0);
    for (auto& t : threads) t.join();
    for (auto& e : errors) if (e) std::rethrow_exception(e);
}

std::string CellFileName(int x, int z)
{
    return "cell_" + std::to_string(x) + "_" + std::to_string(z) + ".json";
}

std::string PartsDirFor(const std::string& scenePath)
{
    fs::path p(scenePath);
    fs::path dir = p.parent_path();
    return (dir / (p.stem().string() + ".parts")).string();
}

// ======================================================================
//  割り当て
// ======================================================================
namespace
{

struct Mat3 { double m[3][3]; };

Mat3 Diag(double x, double y, double z) { return Mat3{{{x, 0, 0}, {0, y, 0}, {0, 0, z}}}; }

Mat3 Mul(const Mat3& a, const Mat3& b)
{
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}

// Transform::GetWorldMatrix と同じ規約（行ベクトル・XMMatrixRotationRollPitchYaw(pitch, yaw, roll) = Rz * Rx * Ry）。
Mat3 RotationPYR(double pitch, double yaw, double roll)
{
    const double cx = std::cos(pitch), sx = std::sin(pitch);
    const double cy = std::cos(yaw),   sy = std::sin(yaw);
    const double cz = std::cos(roll),  sz = std::sin(roll);
    const Mat3 rx{{{1, 0, 0}, {0, cx, sx}, {0, -sx, cx}}};
    const Mat3 ry{{{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}}};
    const Mat3 rz{{{cz, sz, 0}, {-sz, cz, 0}, {0, 0, 1}}};
    return Mul(Mul(rz, rx), ry);
}

struct Xf
{
    Mat3   lin = Diag(1, 1, 1);   // 行ベクトル: world = local * lin + p
    double p[3] = {0, 0, 0};
};

void ReadVec3(const json& t, const char* key, double def, double out[3])
{
    out[0] = out[1] = out[2] = def;
    const auto it = t.find(key);
    if (it == t.end() || !it->is_array() || it->size() < 3) return;
    for (int i = 0; i < 3; ++i)
        if ((*it)[static_cast<size_t>(i)].is_number()) out[i] = (*it)[static_cast<size_t>(i)].get<double>();
}

const json* TransformOf(const json& ej)
{
    const auto it = ej.find("transform");
    return (it != ej.end() && it->is_object()) ? &*it : nullptr;
}

void LocalTRS(const json& ej, double pos[3], double rot[3], double scl[3])
{
    static const json kEmpty = json::object();
    const json* t = TransformOf(ej);
    const json& tj = t ? *t : kEmpty;
    ReadVec3(tj, "position", 0.0, pos);
    ReadVec3(tj, "rotation", 0.0, rot);
    ReadVec3(tj, "scale",    1.0, scl);
}

// このエンティティのワールド変換（親のワールド変換 pw を受けて）。
Xf Compose(const json& ej, const Xf& pw)
{
    double p[3], r[3], s[3];
    LocalTRS(ej, p, r, s);
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    const Mat3 local = Mul(Diag(s[0], s[1], s[2]), RotationPYR(r[0] * kDeg, r[1] * kDeg, r[2] * kDeg));
    Xf out;
    out.lin = Mul(local, pw.lin);
    for (int j = 0; j < 3; ++j)
        out.p[j] = p[0] * pw.lin.m[0][j] + p[1] * pw.lin.m[1][j] + p[2] * pw.lin.m[2][j] + pw.p[j];
    return out;
}

bool Has(const json& ej, const char* key) { return ej.find(key) != ej.end(); }

bool IsPinnedRoot(const json& ej)
{
    const auto it = ej.find("partition");
    return it != ej.end() && it->is_string() && it->get<std::string>() == "root";
}

// 名前・guid・transform だけの入れ物か（親だけの空エンティティ。エディタ専用の印や無効フラグは許す）。
bool IsPureContainer(const json& ej)
{
    static const char* const kAllowed[] = {"guid", "name", "transform", "parentGuid", "parent", "siblingOrder",
                                           "editorFolder", "editorHidden", "editorLocked", "disabled"};
    for (auto it = ej.begin(); it != ej.end(); ++it)
    {
        bool ok = false;
        for (const char* k : kAllowed) if (it.key() == k) { ok = true; break; }
        if (!ok) return false;
    }
    return true;
}

// 空間に置かれる物（これらのどれかを持つ）。未知のキーは含めない = 迷ったら foo.json 側。
bool IsSpatial(const json& ej)
{
    static const char* const kSpatial[] = {"meshRenderer", "meshCollider", "convexHullCollider", "boxCollider",
                                           "sphereCollider", "capsuleCollider", "rigidBody", "instanceGroup",
                                           "primitive", "terrain", "waterBody", "foliageLayer", "decal",
                                           "sculpt", "virtualGeometry", "sprite2d"};
    for (const char* k : kSpatial) if (Has(ej, k)) return true;
    return false;
}

// ゲームの仕組み・ライト・カメラ・音（位置を持っていても foo.json 側に置く）。
bool IsLogicOrNonSpatial(const json& ej)
{
    static const char* const kLogic[] = {"luaScript", "trigger", "camera", "characterController", "brain",
                                         "networkIdentity", "networkTransform", "gimmick", "directionalLight",
                                         "pointLight", "spotLight", "audioSource", "audioReverbZone",
                                         "uiCanvas", "uiRect", "sequencePlayer"};
    for (const char* k : kLogic) if (Has(ej, k)) return true;
    return false;
}

} // namespace

Assignment AssignCells(const json& ents, double cellSize)
{
    Assignment a;
    const size_t n = ents.is_array() ? ents.size() : 0;
    if (n == 0) return a;
    if (!(cellSize > 0.0) || !std::isfinite(cellSize))
    {
        a.rootIdx.resize(n);
        for (size_t i = 0; i < n; ++i) a.rootIdx[i] = static_cast<int>(i);
        return a;
    }

    std::unordered_map<std::string, int> byGuid;
    byGuid.reserve(n * 2);
    for (size_t i = 0; i < n; ++i)
    {
        const json& ej = ents[i];
        if (!ej.is_object()) continue;
        const auto g = ej.find("guid");
        if (g != ej.end() && g->is_string() && !g->get_ref<const std::string&>().empty())
            byGuid.emplace(g->get<std::string>(), static_cast<int>(i));
    }
    std::vector<int> parent(n, -1);
    std::vector<std::vector<int>> kids(n);
    for (size_t i = 0; i < n; ++i)
    {
        const json& ej = ents[i];
        if (!ej.is_object()) continue;
        const auto pg = ej.find("parentGuid");
        if (pg == ej.end() || !pg->is_string()) continue;
        const auto f = byGuid.find(pg->get<std::string>());
        if (f == byGuid.end() || f->second == static_cast<int>(i)) continue;
        parent[i] = f->second;
        kids[static_cast<size_t>(f->second)].push_back(static_cast<int>(i));
    }

    // owner: -2 未割り当て / -1 foo.json / 0.. セルの番号
    std::vector<int> owner(n, -2);
    std::map<CellKey, int> cellIndex;
    std::vector<CellKey> cellKeys;

    auto assignSubtree = [&](int top, int ownerId) {
        std::vector<int> stack{top};
        while (!stack.empty())
        {
            const int i = stack.back();
            stack.pop_back();
            if (owner[static_cast<size_t>(i)] != -2) continue;
            owner[static_cast<size_t>(i)] = ownerId;
            for (int k : kids[static_cast<size_t>(i)]) stack.push_back(k);
        }
    };

    std::function<void(int, const Xf&, int)> visit = [&](int i, const Xf& pw, int depth) {
        const json& ej = ents[static_cast<size_t>(i)];
        if (!ej.is_object() || IsPinnedRoot(ej)) { assignSubtree(i, -1); return; }
        if (depth < 64 && !kids[static_cast<size_t>(i)].empty() && IsPureContainer(ej))
        {
            owner[static_cast<size_t>(i)] = -1;
            const Xf mine = Compose(ej, pw);
            for (int k : kids[static_cast<size_t>(i)]) visit(k, mine, depth + 1);
            return;
        }
        if (!IsSpatial(ej) || IsLogicOrNonSpatial(ej)) { assignSubtree(i, -1); return; }
        double lp[3], lr[3], ls[3];
        LocalTRS(ej, lp, lr, ls);
        double w[2];
        w[0] = lp[0] * pw.lin.m[0][0] + lp[1] * pw.lin.m[1][0] + lp[2] * pw.lin.m[2][0] + pw.p[0];
        w[1] = lp[0] * pw.lin.m[0][2] + lp[1] * pw.lin.m[1][2] + lp[2] * pw.lin.m[2][2] + pw.p[2];
        if (!std::isfinite(w[0]) || !std::isfinite(w[1])) { assignSubtree(i, -1); return; }
        auto cellOf = [&](double v) {
            const double c = std::floor(v / cellSize);
            return static_cast<int>((std::max)(-1.0e6, (std::min)(1.0e6, c)));
        };
        const CellKey key{cellOf(w[0]), cellOf(w[1])};
        auto [it, inserted] = cellIndex.emplace(key, static_cast<int>(cellKeys.size()));
        if (inserted) cellKeys.push_back(key);
        assignSubtree(i, it->second);
    };

    const Xf identity;
    for (size_t i = 0; i < n; ++i)
        if (parent[i] < 0) visit(static_cast<int>(i), identity, 0);
    for (size_t i = 0; i < n; ++i)
        if (owner[i] == -2) owner[i] = -1;   // 親の輪に入っていて辿れなかったもの（壊れた入力）

    // セルを (x,z) 昇順に並べ直す
    std::vector<int> order(cellKeys.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int l, int r) { return cellKeys[static_cast<size_t>(l)] < cellKeys[static_cast<size_t>(r)]; });
    std::vector<int> rank(cellKeys.size());
    for (size_t r = 0; r < order.size(); ++r) rank[static_cast<size_t>(order[r])] = static_cast<int>(r);
    a.cells.resize(cellKeys.size());
    for (size_t r = 0; r < order.size(); ++r) a.cells[r].first = cellKeys[static_cast<size_t>(order[r])];
    for (size_t i = 0; i < n; ++i)
    {
        if (owner[i] < 0) a.rootIdx.push_back(static_cast<int>(i));
        else a.cells[static_cast<size_t>(rank[static_cast<size_t>(owner[i])])].second.push_back(static_cast<int>(i));
    }
    return a;
}

// ======================================================================
//  並び順（seq）
// ======================================================================
std::string EncodeSeq(const std::vector<int>& v)
{
    std::string s;
    size_t i = 0;
    while (i < v.size())
    {
        size_t j = i;
        while (j + 1 < v.size() && v[j + 1] == v[j] + 1) ++j;
        if (!s.empty()) s += ',';
        s += std::to_string(v[i]);
        if (j > i) { s += '-'; s += std::to_string(v[j]); }
        i = j + 1;
    }
    return s;
}

bool DecodeSeq(const std::string& s, size_t expectedCount, size_t total, std::vector<int>& out)
{
    out.clear();
    out.reserve(expectedCount);
    size_t pos = 0;
    auto readInt = [&](long long& v) -> bool {
        if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') return false;
        long long x = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
        {
            x = x * 10 + (s[pos] - '0');
            if (x > 100000000LL) return false;
            ++pos;
        }
        v = x;
        return true;
    };
    while (pos < s.size())
    {
        long long a = 0, b = 0;
        if (!readInt(a)) return false;
        b = a;
        if (pos < s.size() && s[pos] == '-') { ++pos; if (!readInt(b) || b < a) return false; }
        if (static_cast<size_t>(b) >= total) return false;
        if (out.size() + static_cast<size_t>(b - a + 1) > expectedCount) return false;
        for (long long k = a; k <= b; ++k) out.push_back(static_cast<int>(k));
        if (pos < s.size())
        {
            if (s[pos] != ',') return false;
            ++pos;
            if (pos >= s.size()) return false;
        }
    }
    return out.size() == expectedCount;
}

// ======================================================================
//  保存
// ======================================================================
void SplitAndDump(json&& root, double cellSize, SplitOutput& out)
{
    out = SplitOutput{};
    json::array_t* arr = root.is_object() && root.contains("entities") ? root["entities"].get_ptr<json::array_t*>() : nullptr;
    if (!arr) { out.rootText = scenefmt::DumpSceneV2(root); return; }

    const Assignment a = AssignCells(root["entities"], cellSize);
    if (a.cells.empty()) { out.rootText = scenefmt::DumpSceneV2(root); return; }

    std::vector<json> docs(a.cells.size());
    out.parts.resize(a.cells.size());
    json partsList = json::array();
    for (size_t c = 0; c < a.cells.size(); ++c)
    {
        const CellKey key = a.cells[c].first;
        const std::vector<int>& idx = a.cells[c].second;
        json::array_t pe;
        pe.reserve(idx.size());
        for (int i : idx) pe.push_back(std::move((*arr)[static_cast<size_t>(i)]));
        json doc = json::object();
        doc["version"] = scenefmt::kSceneVersionV2;
        doc["seq"] = EncodeSeq(idx);
        doc["entities"] = std::move(pe);
        docs[c] = std::move(doc);

        PartText& pt = out.parts[c];
        pt.name  = CellFileName(key.x, key.z);
        pt.cell  = key;
        pt.count = idx.size();
        auto num = [&](double v) { return (v == std::floor(v) && std::fabs(v) < 1.0e9) ? json(static_cast<long long>(v)) : json(v); };
        partsList.push_back({{"file", pt.name}, {"count", pt.count}, {"cell", json::array({key.x, key.z})},
                             {"bounds", json::array({num(key.x * cellSize), num(key.z * cellSize),
                                                     num((key.x + 1) * cellSize), num((key.z + 1) * cellSize)})}});
    }
    json::array_t rootEnts;
    rootEnts.reserve(a.rootIdx.size());
    for (int i : a.rootIdx) rootEnts.push_back(std::move((*arr)[static_cast<size_t>(i)]));
    root["entities"] = std::move(rootEnts);
    root["parts"] = std::move(partsList);

    // セルの文字列化は並列（各セルは独立）。DumpSceneV2 の中ではスレッドを重ねない。
    ParallelFor(docs.size(), ThreadBudget(), [&](size_t c) {
        out.parts[c].text = scenefmt::DumpSceneV2(docs[c], /*allowThreads=*/false);
    });
    out.rootText = scenefmt::DumpSceneV2(root);
}

static bool ReadWhole(const fs::path& p, std::string& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.clear();
    if (sz > 0) { out.resize(static_cast<size_t>(sz)); f.read(out.data(), sz); }
    return static_cast<bool>(f) || sz == 0;
}

// 内容が同じなら触らない。違うなら Batch へ（一時ファイルに書いて検証。置き換えは Commit でまとめて）。
// 戻り値: 0 = 変更なし / 1 = 書いた（ステージした） / -1 = 失敗
static int StageIfChanged(atomicfile::Batch& batch, const fs::path& p, const std::string& text)
{
    std::error_code ec;
    if (fs::exists(p, ec))
    {
        const auto sz = fs::file_size(p, ec);
        if (!ec && sz == text.size())
        {
            std::string cur;
            if (ReadWhole(p, cur) && cur == text) return 0;
        }
    }
    return batch.Add(p, text, atomicfile::JsonVerifier()) ? 1 : -1;
}

static bool IsCellFileName(const std::string& n)
{
    return n.size() > 10 && n.compare(0, 5, "cell_") == 0 && n.compare(n.size() - 5, 5, ".json") == 0;
}

WriteStats StageSplit(const std::string& scenePath, const SplitOutput& out, atomicfile::Batch& batch, bool stageRoot)
{
    WriteStats st;
    st.files = static_cast<int>(out.parts.size());
    const fs::path dir = PartsDirFor(scenePath);
    std::error_code ec;

    if (!out.parts.empty())
    {
        fs::create_directories(dir, ec);
        if (ec && !fs::exists(dir, ec)) { st.ok = false; return st; }
    }
    // セルは並列に比べて・書く（数百のセルを 1 つずつ flush・検証すると待ちが積み重なる）。内容が同じものは触らない。
    {
        std::vector<atomicfile::Batch::Entry> es;
        es.reserve(out.parts.size());
        const atomicfile::Verifier verify = atomicfile::JsonVerifier();
        for (const PartText& pt : out.parts)
        {
            st.maxBytes = (std::max)(st.maxBytes, pt.text.size());
            st.totalBytes += pt.text.size();
            es.push_back({dir / pt.name, std::string_view(pt.text), verify});
        }
        std::vector<char> wrote;
        if (!batch.AddMany(es, ThreadBudget(), /*skipIdentical=*/true, &wrote)) { st.ok = false; return st; }
        for (char w : wrote) { if (w) ++st.written; else ++st.unchanged; }
    }
    // foo.json は最後（セルが全部書けた後。コミットも追加順に置き換えるので、途中で落ちても「新しいルート＋古いセル」にならない）
    if (st.ok && stageRoot)
    {
        st.maxBytes = (std::max)(st.maxBytes, out.rootText.size());
        st.totalBytes += out.rootText.size();
        const int r = StageIfChanged(batch, scenePath, out.rootText);
        if (r < 0) st.ok = false; else if (r == 0) ++st.unchanged; else ++st.written;
    }
    return st;
}

int FinishSplit(const std::string& scenePath, const SplitOutput& out)
{
    const fs::path dir = PartsDirFor(scenePath);
    std::error_code ec;
    std::unordered_set<std::string> keep;
    for (const PartText& pt : out.parts) keep.insert(pt.name);
    int removed = 0;
    // 空になったセルの掃除（このフォルダの cell_*.json だけを対象にする）
    if (fs::is_directory(dir, ec))
    {
        std::vector<fs::path> stale;
        for (const auto& ent : fs::directory_iterator(dir, ec))
        {
            if (!ent.is_regular_file(ec)) continue;
            const std::string n = ent.path().filename().string();
            if (IsCellFileName(n) && !keep.count(n)) stale.push_back(ent.path());
        }
        for (const auto& p : stale) { fs::remove(p, ec); ++removed; }
        if (out.parts.empty() && fs::is_empty(dir, ec)) fs::remove(dir, ec);
    }
    return removed;
}

WriteStats WriteSplit(const std::string& scenePath, const SplitOutput& out)
{
    atomicfile::Batch batch(fs::path(scenePath).concat(".dx12txn"));
    WriteStats st = StageSplit(scenePath, out, batch, /*stageRoot=*/true);
    if (!st.ok) return st;   // Batch のデストラクタが一時ファイルを消す（何も置き換えていない）
    if (!batch.Commit().ok) { st.ok = false; return st; }
    st.removed = FinishSplit(scenePath, out);
    return st;
}

int RemoveParts(const std::string& scenePath)
{
    const fs::path dir = PartsDirFor(scenePath);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    int n = 0;
    std::vector<fs::path> stale;
    for (const auto& ent : fs::directory_iterator(dir, ec))
    {
        if (!ent.is_regular_file(ec)) continue;
        if (IsCellFileName(ent.path().filename().string())) stale.push_back(ent.path());
    }
    for (const auto& p : stale) { fs::remove(p, ec); ++n; }
    if (fs::is_empty(dir, ec)) fs::remove(dir, ec);
    return n;
}

// ======================================================================
//  読み込み
// ======================================================================
bool IsSafePartName(const std::string& name)
{
    if (name.empty() || name.size() > 200) return false;
    if (name == "." || name == "..") return false;
    for (char c : name)
        if (c == '/' || c == '\\' || c == ':' || c == '\0') return false;
    return name.find("..") == std::string::npos;
}

std::vector<std::string> PartNames(const json& root)
{
    std::vector<std::string> names;
    const auto it = root.is_object() ? root.find("parts") : root.end();
    if (it == root.end() || !it->is_array()) return names;
    for (const auto& p : *it)
    {
        if (!p.is_object()) continue;
        const auto f = p.find("file");
        if (f == p.end() || !f->is_string()) continue;
        const std::string n = f->get<std::string>();
        if (IsSafePartName(n)) names.push_back(n);
    }
    return names;
}

bool MergeParts(json& root, const ReadPartFn& read, MergeStats& stats, std::string& err)
{
    stats = MergeStats{};
    if (!root.is_object()) return true;
    const auto pit = root.find("parts");
    if (pit == root.end() || !pit->is_array()) return true;

    // 目次の検査: 1 つでも不正な名前があれば、黙って読み飛ばさず失敗にする（保存で消えるのを避ける）
    std::vector<std::string> names;
    for (const auto& p : *pit)
    {
        const auto f = p.is_object() ? p.find("file") : p.end();
        if (!p.is_object() || f == p.end() || !f->is_string() || !IsSafePartName(f->get<std::string>()))
        {
            err = "foo.json の parts に不正な項目があります（file は分割フォルダ内のファイル名だけ）";
            return false;
        }
        names.push_back(f->get<std::string>());
    }
    stats.files = names.size();
    if (!root.contains("entities") || !root["entities"].is_array()) root["entities"] = json::array();

    // 1. 読む（順番に。pak の復号は主スレッドで）
    const auto tRead = Clock::now();
    std::vector<std::string> texts(names.size());
    size_t totalBytes = 0;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (!read(names[i], texts[i]))
        {
            err = "分割ファイルを読めません: " + names[i];
            return false;
        }
        totalBytes += texts[i].size();
    }
    stats.readMs = MsSince(tRead);

    // 2. パース（並列）。小さいときはスレッドを起こさない。
    const auto tParse = Clock::now();
    std::vector<json> docs(names.size());
    std::vector<std::string> perr(names.size());
    stats.threads = totalBytes < 256 * 1024 ? 1u : (std::min)(ThreadBudget(), static_cast<unsigned>(names.size()));
    ParallelFor(names.size(), stats.threads, [&](size_t i) {
        docs[i] = json::parse(texts[i], nullptr, /*allow_exceptions=*/false);
        std::string().swap(texts[i]);   // 本文は要らない（メモリを早く返す）
        if (docs[i].is_discarded() || !docs[i].is_object()) perr[i] = "JSON として読めません";
        else if (!docs[i].contains("entities") || !docs[i]["entities"].is_array()) perr[i] = "entities 配列がありません";
    });
    stats.parseMs = MsSince(tParse);
    for (size_t i = 0; i < names.size(); ++i)
        if (!perr[i].empty()) { err = "分割ファイル " + names[i] + ": " + perr[i]; return false; }

    // 3. 統合（並び順 seq どおり）
    const auto tMerge = Clock::now();
    json::array_t& rootEnts = *root["entities"].get_ptr<json::array_t*>();
    size_t total = rootEnts.size();
    for (const auto& d : docs) total += d["entities"].size();

    std::vector<std::vector<int>> seqs(docs.size());
    bool seqOk = true;
    for (size_t i = 0; i < docs.size() && seqOk; ++i)
    {
        const auto s = docs[i].find("seq");
        if (s == docs[i].end() || !s->is_string() ||
            !DecodeSeq(s->get<std::string>(), docs[i]["entities"].size(), total, seqs[i]))
            seqOk = false;
    }
    std::vector<char> used;
    if (seqOk)
    {
        used.assign(total, 0);
        for (const auto& sq : seqs)
            for (int v : sq)
            {
                if (used[static_cast<size_t>(v)]) { seqOk = false; break; }   // 重複 = 壊れている
                used[static_cast<size_t>(v)] = 1;
            }
    }
    json::array_t merged(total);
    if (seqOk)
    {
        for (size_t i = 0; i < docs.size(); ++i)
        {
            json::array_t& pe = *docs[i]["entities"].get_ptr<json::array_t*>();
            for (size_t k = 0; k < pe.size(); ++k) merged[static_cast<size_t>(seqs[i][k])] = std::move(pe[k]);
        }
        size_t r = 0;
        for (size_t slot = 0; slot < total && r < rootEnts.size(); ++slot)
            if (!used[slot]) merged[slot] = std::move(rootEnts[r++]);
    }
    else
    {
        // つなぎ順: foo.json → セル（parts の並び）
        size_t w = 0;
        for (auto& e : rootEnts) merged[w++] = std::move(e);
        for (auto& d : docs)
        {
            json::array_t& pe = *d["entities"].get_ptr<json::array_t*>();
            for (auto& e : pe) merged[w++] = std::move(e);
        }
    }
    rootEnts = std::move(merged);
    root.erase("parts");
    stats.seqUsed = seqOk;
    stats.mergeMs = MsSince(tMerge);
    return true;
}

bool ReadSceneFileMerged(const std::string& absPath, json& root, std::string& err)
{
    std::string text;
    if (!ReadWhole(absPath, text)) { err = "シーンファイルを開けません: " + absPath; return false; }
    root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) { err = "JSON の解析に失敗しました: " + absPath; return false; }
    return MergePartsFromDisk(root, absPath, err);
}

bool MergePartsFromDisk(json& root, const std::string& scenePath, std::string& err)
{
    const std::string dir = PartsDirFor(scenePath);
    MergeStats ms;
    return MergeParts(root, [&](const std::string& name, std::string& bytes) {
        return ReadWhole(fs::path(dir) / name, bytes);
    }, ms, err);
}

} // namespace dx12e::scenepart
