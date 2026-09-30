#include "renderer/matgraph/ShaderCache.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace dx12e::matgraph
{
namespace
{

std::string ReadWholeFile(const fs::path& p, bool& ok)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) { ok = false; return {}; }
    std::ostringstream ss;
    ss << f.rdbuf();
    ok = true;
    return ss.str();
}

std::string EnvVar(const char* name)
{
#ifdef _WIN32
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf) return {};
    std::string s(buf);
    std::free(buf);
    return s;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

struct Scanner
{
    const std::vector<std::string>& dirs;
    std::set<std::string>           visited;
    IncludeScan&                    out;

    void Visit(const std::string& text, const fs::path& includerDir)
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            const size_t h = line.find("#include");
            if (h == std::string::npos) continue;
            if (line.find_first_not_of(" \t") != h) continue;          // コメントアウトを拾わない
            const size_t q0 = line.find('"', h);
            if (q0 == std::string::npos) continue;                     // <...> は対象外
            const size_t q1 = line.find('"', q0 + 1);
            if (q1 == std::string::npos) continue;
            const std::string name = line.substr(q0 + 1, q1 - q0 - 1);

            std::error_code ec;
            fs::path found;
            std::vector<fs::path> cands;
            if (!includerDir.empty()) cands.push_back(includerDir / name);
            for (const std::string& d : dirs) cands.push_back(fs::path(d) / name);
            for (const fs::path& c : cands)
            {
                if (fs::is_regular_file(c, ec)) { found = c; break; }
            }
            if (found.empty()) { out.missing.push_back(name); continue; }

            const std::string canon = fs::weakly_canonical(found, ec).generic_string();
            if (!visited.insert(canon).second) continue;
            bool ok = false;
            const std::string body = ReadWholeFile(found, ok);
            if (!ok) { out.missing.push_back(name); continue; }
            out.files.push_back(canon);
            // パスは全体ではなく「見つけた include 名」で混ぜる（リポジトリの置き場所が違ってもキーが揃う）
            out.hash = Fnv1a64(name, out.hash);
            out.hash = Fnv1a64(body, out.hash);
            Visit(body, found.parent_path());
        }
    }
};

constexpr uint32_t kMagic = 0x43534755u;   // "UGSC"
constexpr uint32_t kFormat = 1;

#pragma pack(push, 1)
struct FileHeader
{
    uint32_t magic;
    uint32_t format;
    uint64_t key;
    uint64_t payloadHash;
    uint32_t payloadSize;
    uint32_t reserved;
};
#pragma pack(pop)

} // namespace

uint64_t Fnv1a64(const void* data, size_t n, uint64_t seed)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

std::string Hex16(uint64_t v)
{
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

IncludeScan ScanIncludes(const std::string& sourceText, const std::vector<std::string>& searchDirs)
{
    IncludeScan r;
    r.hash = 1469598103934665603ull;
    Scanner sc{searchDirs, {}, r};
    sc.Visit(sourceText, fs::path());
    return r;
}

uint64_t MakeShaderKey(const ShaderKeyInput& in)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const std::string& s) {
        h = Fnv1a64(s, h);
        const unsigned char sep = 0x1f;
        h = Fnv1a64(&sep, 1, h);
    };
    mix(in.kind);
    mix(in.engineVersion);
    mix(in.dxcVersion);
    mix(in.extra);
    mix(Hex16(in.includeHash));
    mix(in.hlsl);
    return h;
}

// ---- ディスクキャッシュ -------------------------------------------------------------------
std::string ShaderDiskCache::DefaultDir(const std::string& engineVersion)
{
    std::string base = EnvVar("UNO_SHADERCACHE_DIR");
    if (!base.empty()) return (fs::path(base) / (engineVersion.empty() ? "dev" : engineVersion)).generic_string();
    std::string local = EnvVar("LOCALAPPDATA");
    if (local.empty()) local = (fs::temp_directory_path()).generic_string();
    return (fs::path(local) / "UnoEngine" / "shadercache" / (engineVersion.empty() ? "dev" : engineVersion)).generic_string();
}

ShaderDiskCache::ShaderDiskCache(std::string dir, const std::string& engineVersion)
    : m_dir(dir.empty() ? DefaultDir(engineVersion) : std::move(dir))
{
}

std::string ShaderDiskCache::PathFor(uint64_t key, const std::string& kind) const
{
    return (fs::path(m_dir) / (Hex16(key) + "." + kind + ".dxil")).generic_string();
}

bool ShaderDiskCache::Load(uint64_t key, const std::string& kind, std::vector<uint8_t>& out)
{
    const std::string path = PathFor(key, kind);
    std::ifstream f(path, std::ios::binary);
    if (!f) { ++misses; return false; }
    FileHeader h{};
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!f || h.magic != kMagic || h.format != kFormat || h.key != key || h.payloadSize == 0 || h.payloadSize > (256u << 20))
    {
        ++corrupt; ++misses;
        return false;
    }
    std::vector<uint8_t> bytes(h.payloadSize);
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f || Fnv1a64(bytes.data(), bytes.size()) != h.payloadHash)
    {
        ++corrupt; ++misses;
        return false;
    }
    out = std::move(bytes);
    ++hits;
    return true;
}

bool ShaderDiskCache::Store(uint64_t key, const std::string& kind, const std::vector<uint8_t>& bytes)
{
    if (bytes.empty()) return false;
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    if (ec) return false;
    const std::string path = PathFor(key, kind);
    // 一時ファイル名は pid 相当の乱数（別プロセスと衝突しない）+ rename
    static std::atomic<uint32_t> counter{0};
    const std::string tmp = path + ".tmp" + std::to_string(reinterpret_cast<uintptr_t>(&counter) & 0xffffu) + "_" + std::to_string(++counter);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        FileHeader h{};
        h.magic = kMagic;
        h.format = kFormat;
        h.key = key;
        h.payloadHash = Fnv1a64(bytes.data(), bytes.size());
        h.payloadSize = static_cast<uint32_t>(bytes.size());
        f.write(reinterpret_cast<const char*>(&h), sizeof(h));
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!f) { f.close(); fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, path, ec);
    if (ec)
    {
        // 既存ファイルがあって上書きできない（同時書き込み）場合は、既存が有効なので成功扱い
        fs::remove(tmp, ec);
        std::error_code ec2;
        if (!fs::exists(path, ec2)) return false;
    }
    ++stores;
    return true;
}

} // namespace dx12e::matgraph
