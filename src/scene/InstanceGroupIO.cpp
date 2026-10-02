#include "scene/InstanceGroupIO.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_set>

#include "core/Logger.h"
#include "core/vfs/Vfs.h"

namespace dx12e::instgroup
{

namespace fs = std::filesystem;

namespace
{
bool ReadFileBytes(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.clear();
    if (sz > 0)
    {
        out.resize(static_cast<size_t>(sz));
        f.read(out.data(), sz);
    }
    return true;
}
} // namespace

InstanceSetPtr LoadSidecar(const std::string& scenePath, const std::string& guidHex)
{
    const std::string path = SidecarPathFor(scenePath, guidHex);
    std::string text;
    {
        auto b = vfs::ReadAssetAbs(path);   // ゲームモード: pak から復号。エディタ: ディスク
        if (!b.empty()) text.assign(b.begin(), b.end());
        else if (!ReadFileBytes(path, text)) return nullptr;
    }
    ParseReport rep;
    InstanceSetPtr s = ParseSidecar(text, &rep);
    if (rep.bad > 0)
    {
        std::string nums;
        for (size_t n : rep.badLineNumbers) { if (!nums.empty()) nums += ","; nums += std::to_string(n); }
        Logger::Warn("インスタンス群: 壊れた行を {} 行読み飛ばしました（{}。行番号 {}{}）", rep.bad, path, nums,
                     rep.bad > rep.badLineNumbers.size() ? " ..." : "");
    }
    return s;
}

SaveStats StageSidecars(const std::string& scenePath, const SerializeCollector& collected, atomicfile::Batch& batch)
{
    SaveStats st;
    const fs::path dir = SidecarDirFor(scenePath);
    std::error_code ec;

    if (!collected.sets.empty())
    {
        fs::create_directories(dir, ec);
        if (ec && !fs::exists(dir, ec))
        {
            Logger::Error("インスタンス群のフォルダを作れません: {} ({})", dir.string(), ec.message());
            st.ok = false;
            return st;
        }
    }
    for (const auto& [guidHex, set] : collected.sets)
    {
        if (!set) continue;
        const fs::path p = dir / (guidHex + ".jsonl");
        const std::string text = FormatSidecar(*set);
        std::string cur;
        if (ReadFileBytes(p.string(), cur) && cur == text) { ++st.unchanged; continue; }
        // 一時ファイルへ書いて検証する（置き換えはコミットでまとめて。途中で落ちても既存ファイルは無傷）
        if (!batch.Add(p, text))
        {
            Logger::Error("インスタンス群のファイルを書けません: {} ({})", p.string(), batch.Error());
            st.ok = false;
            return st;
        }
        ++st.written;
    }
    return st;
}

int CleanupSidecars(const std::string& scenePath, const SerializeCollector& collected)
{
    const fs::path dir = SidecarDirFor(scenePath);
    std::error_code ec;
    std::unordered_set<std::string> keep;
    for (const auto& [guidHex, set] : collected.sets)
        if (set) keep.insert(guidHex + ".jsonl");

    // 孤児の掃除（削除したグループの残骸）。このフォルダの *.jsonl だけを対象にし、他のファイルには触らない。
    int removed = 0;
    if (fs::is_directory(dir, ec))
    {
        std::vector<fs::path> stale;
        for (const auto& ent : fs::directory_iterator(dir, ec))
        {
            if (!ent.is_regular_file(ec)) continue;
            const std::string name = ent.path().filename().string();
            if (name.size() > 6 && name.compare(name.size() - 6, 6, ".jsonl") == 0 && !keep.count(name))
                stale.push_back(ent.path());
        }
        for (const auto& p : stale) { fs::remove(p, ec); ++removed; }
        if (collected.sets.empty() && fs::is_empty(dir, ec)) fs::remove(dir, ec);
    }
    return removed;
}

SaveStats SaveSidecars(const std::string& scenePath, const SerializeCollector& collected)
{
    atomicfile::Batch batch(fs::path(scenePath).concat(".dx12txn"));
    SaveStats st = StageSidecars(scenePath, collected, batch);
    if (!st.ok) return st;
    if (!batch.Commit().ok) { st.ok = false; return st; }
    st.removed = CleanupSidecars(scenePath, collected);
    return st;
}

} // namespace dx12e::instgroup
