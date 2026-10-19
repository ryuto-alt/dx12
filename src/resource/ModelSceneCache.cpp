#include "resource/ModelSceneCache.h"
#include "core/AtomicFile.h"
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/vfs/Vfs.h"

#include <assimp/Importer.hpp>
#include <assimp/Exporter.hpp>
#include <assimp/scene.h>
#include <assimp/version.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <windows.h>   // SetThreadPriority（裏の書き出し）

namespace dx12e::modelcache
{
namespace
{
namespace fs = std::filesystem;

// 形式の版。中身の並び・キーの作り方・ModelLoader の後処理フラグの扱いを変えたら上げる（古いものは自然に外れる）。
constexpr uint32_t kSceneFormat = 1;
constexpr uint32_t kLodFormat   = 1;
constexpr char     kSceneMagic[4] = { 'D', 'X', 'M', 'C' };
constexpr char     kLodMagic[4]   = { 'D', 'X', 'L', 'C' };

uint64_t Fnv(uint64_t h, const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
uint64_t Fnv(uint64_t h, std::string_view s) { return Fnv(h, s.data(), s.size()); }
template <class T> uint64_t FnvPod(uint64_t h, const T& v) { return Fnv(h, &v, sizeof(v)); }
constexpr uint64_t kFnvSeed = 1469598103934665603ull;

std::string Hex(uint64_t v)
{
    char buf[20];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

// 置き場（末尾 "/" 付き）。配布ゲーム・作れないときは空＝キャッシュしない。
std::string CacheDir()
{
    if (vfs::InGameMode()) return {};
    const std::string dir = PathResolver::AssetsDir() + ".modelcache/";
    std::error_code ec;
    if (!fs::exists(dir, ec)) fs::create_directories(dir, ec);
    if (ec || !fs::exists(dir, ec)) return {};
    return dir;
}

std::string NormalizedPath(const fs::path& p)
{
    std::error_code ec;
    std::string s = fs::weakly_canonical(p, ec).generic_string();
    if (ec || s.empty()) s = p.lexically_normal().generic_string();
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');   // Windows は大小を区別しない
    return s;
}

// 0 = キーを作れない（ファイルが無い等）＝キャッシュしない。
uint64_t SceneKey(const fs::path& modelPath, unsigned int flags)
{
    std::error_code ec;
    const auto size  = fs::file_size(modelPath, ec);
    if (ec) return 0;
    const auto mtime = fs::last_write_time(modelPath, ec);
    if (ec) return 0;
    uint64_t h = kFnvSeed;
    h = Fnv(h, NormalizedPath(modelPath));
    h = FnvPod(h, static_cast<uint64_t>(size));
    h = FnvPod(h, static_cast<int64_t>(mtime.time_since_epoch().count()));
    h = FnvPod(h, flags);
    h = FnvPod(h, kSceneFormat);
    const unsigned ver[4] = { aiGetVersionMajor(), aiGetVersionMinor(), aiGetVersionPatch(), aiGetVersionRevision() };
    h = Fnv(h, ver, sizeof(ver));
    return h == 0 ? 1 : h;
}

// ---- 小さなバイト列の読み書き ----
void PutU32(std::string& out, uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }
void PutU64(std::string& out, uint64_t v) { out.append(reinterpret_cast<const char*>(&v), 8); }
void PutStr(std::string& out, std::string_view s) { PutU32(out, static_cast<uint32_t>(s.size())); out.append(s); }

struct Reader
{
    const char* p;
    const char* end;
    bool ok = true;
    bool Take(void* dst, size_t n)
    {
        if (!ok || static_cast<size_t>(end - p) < n) { ok = false; return false; }
        std::memcpy(dst, p, n);
        p += n;
        return true;
    }
    uint32_t U32() { uint32_t v = 0; Take(&v, 4); return v; }
    uint64_t U64() { uint64_t v = 0; Take(&v, 8); return v; }
    std::string Str()
    {
        const uint32_t n = U32();
        if (!ok || static_cast<size_t>(end - p) < n) { ok = false; return {}; }
        std::string s(p, n);
        p += n;
        return s;
    }
};

// 解析済みシーンのファイル名 = s<パスのハッシュ>_<キー>.dxmc。パスの部分で「同じモデルの古い版」を見分けて消す
// （モデルを編集するたびに更新時刻＝キーが変わり、Bistro だと 237MB ずつ溜まっていくため）。
std::string ScenePrefix(const fs::path& modelPath) { return "s" + Hex(Fnv(kFnvSeed, NormalizedPath(modelPath))) + "_"; }

bool ReadWholeFile(const std::string& path, std::string& out)
{
    std::ifstream f(PathResolver::Utf8ToWide(path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size <= 0) return false;
    out.resize(static_cast<size_t>(size));
    f.seekg(0);
    return static_cast<bool>(f.read(out.data(), size));
}

// ---- 裏の書き出し（終了時に待つ）----
struct PendingSaves
{
    std::mutex               mtx;
    std::vector<std::thread> threads;
    ~PendingSaves() { JoinAll(); }
    void JoinAll()
    {
        std::vector<std::thread> ts;
        { std::lock_guard lk(mtx); ts.swap(threads); }
        for (auto& t : ts) if (t.joinable()) t.join();
    }
};
PendingSaves& Saves() { static PendingSaves s; return s; }

} // namespace

bool IsSceneCacheable(const aiScene* scene)
{
    if (!scene || !scene->mRootNode || scene->mNumMeshes == 0) return false;
    for (unsigned i = 0; i < scene->mNumMeshes; ++i)
        if (scene->mMeshes[i]->HasBones()) return false;   // スキンは UnitScaleFactor（assbin に残らない）を使う
    return true;
}

const aiScene* TryLoadScene(Assimp::Importer& importer, const fs::path& modelPath, unsigned int flags)
{
    const std::string dir = CacheDir();
    if (dir.empty()) return nullptr;
    const uint64_t key = SceneKey(modelPath, flags);
    if (key == 0) return nullptr;
    const std::string file = dir + ScenePrefix(modelPath) + Hex(key) + ".dxmc";
    std::error_code ec;
    if (!fs::exists(file, ec)) return nullptr;

    const auto t0 = std::chrono::steady_clock::now();
    std::string bytes;
    if (!ReadWholeFile(file, bytes)) return nullptr;

    Reader r{ bytes.data(), bytes.data() + bytes.size() };
    char magic[4] = {};
    r.Take(magic, 4);
    const uint32_t format = r.U32();
    const uint64_t storedKey = r.U64();
    if (!r.ok || std::memcmp(magic, kSceneMagic, 4) != 0 || format != kSceneFormat || storedKey != key)
        return nullptr;
    std::vector<std::string> meshNames(r.U32());
    for (auto& s : meshNames) s = r.Str();
    std::vector<std::string> texNames(r.ok ? r.U32() : 0);
    for (auto& s : texNames) s = r.Str();
    const uint64_t blobSize = r.U64();
    if (!r.ok || static_cast<uint64_t>(r.end - r.p) != blobSize)
    {
        Logger::Warn("モデルのキャッシュが壊れているので読み直します: {}", file);
        fs::remove(file, ec);
        return nullptr;
    }

    const aiScene* scene = importer.ReadFileFromMemory(r.p, static_cast<size_t>(blobSize), 0, "assbin");
    if (!scene || !scene->mRootNode || scene->mNumMeshes != meshNames.size() || scene->mNumTextures != texNames.size())
    {
        Logger::Warn("モデルのキャッシュを読めなかったので元のファイルから読みます: {}", file);
        importer.FreeScene();
        fs::remove(file, ec);
        return nullptr;
    }
    // assbin に残らない名前を戻す（メッシュ名は get_bounds の perSubmesh 等、テクスチャ名は GetEmbeddedTexture が使う）
    aiScene* mut = const_cast<aiScene*>(scene);
    for (unsigned i = 0; i < mut->mNumMeshes; ++i) mut->mMeshes[i]->mName.Set(meshNames[i]);
    for (unsigned i = 0; i < mut->mNumTextures; ++i) mut->mTextures[i]->mFilename.Set(texNames[i]);

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Logger::Info("解析済みのモデルをキャッシュから読みました（{}）: {:.0f} ms", modelPath.filename().string(), ms);
    return scene;
}

void SaveSceneAsync(aiScene* scene, const fs::path& modelPath, unsigned int flags)
{
    if (!scene) return;
    const std::string dir = CacheDir();
    const uint64_t key = SceneKey(modelPath, flags);
    if (dir.empty() || key == 0) { delete scene; return; }
    const std::string prefix = ScenePrefix(modelPath);
    const std::string file   = dir + prefix + Hex(key) + ".dxmc";
    const std::string name   = modelPath.filename().string();

    std::thread t([scene, dir, prefix, file, name, key]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);   // 人の操作を邪魔しない
        const auto t0 = std::chrono::steady_clock::now();
        try
        {
            std::string out;
            out.append(kSceneMagic, 4);
            PutU32(out, kSceneFormat);
            PutU64(out, key);
            PutU32(out, scene->mNumMeshes);
            for (unsigned i = 0; i < scene->mNumMeshes; ++i) PutStr(out, scene->mMeshes[i]->mName.C_Str());
            PutU32(out, scene->mNumTextures);
            for (unsigned i = 0; i < scene->mNumTextures; ++i) PutStr(out, scene->mTextures[i]->mFilename.C_Str());

            Assimp::Exporter exporter;
            const aiExportDataBlob* blob = exporter.ExportToBlob(scene, "assbin");
            if (!blob || blob->next)
            {
                Logger::Warn("モデルのキャッシュを書き出せませんでした（{}）: {}", name, exporter.GetErrorString());
            }
            else
            {
                PutU64(out, blob->size);
                out.append(static_cast<const char*>(blob->data), blob->size);
                const auto res = atomicfile::WriteFile(PathResolver::Utf8ToWide(file), out);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (res)
                {
                    Logger::Info("解析済みのモデルをキャッシュへ書き出しました（{}）: {:.1f} MB / {:.0f} ms（裏で）",
                                 name, static_cast<double>(out.size()) / (1024.0 * 1024.0), ms);
                    // 同じモデルの古い版（編集前の更新時刻のキー）を消す
                    std::error_code ec;
                    const std::string keep = fs::path(file).filename().string();
                    for (const auto& e : fs::directory_iterator(dir, ec))
                    {
                        const std::string fn = e.path().filename().string();
                        if (fn != keep && fn.rfind(prefix, 0) == 0 && e.path().extension() == ".dxmc")
                            fs::remove(e.path(), ec);
                    }
                }
                else
                {
                    Logger::Warn("モデルのキャッシュを保存できませんでした（{}）: {}", name, res.error);
                }
            }
        }
        catch (const std::exception& e)
        {
            Logger::Warn("モデルのキャッシュを書き出せませんでした（{}）: {}", name, e.what());
        }
        delete scene;
    });
    std::lock_guard lk(Saves().mtx);
    Saves().threads.push_back(std::move(t));
}

void WaitPendingSaves() { Saves().JoinAll(); }

// ============================================================================
// LOD キャッシュ
// ============================================================================
struct LodCacheScope::Impl
{
    std::string file;
    Impl*       prev = nullptr;
    std::unordered_map<uint64_t, std::vector<std::vector<u32>>> loaded;   // ファイルにあって、まだ使っていない
    std::unordered_map<uint64_t, std::vector<std::vector<u32>>> used;     // 今回使った / 作った（書き出すのはこれだけ）
    bool added = false;
};

namespace { thread_local LodCacheScope::Impl* t_lod = nullptr; }

LodCacheScope::LodCacheScope(const fs::path& modelPath)
{
    const std::string dir = CacheDir();
    if (dir.empty()) return;
    m_impl = std::make_unique<Impl>();
    m_impl->file = dir + "l" + Hex(Fnv(kFnvSeed, NormalizedPath(modelPath))) + ".lod";

    std::string bytes;
    if (ReadWholeFile(m_impl->file, bytes))
    {
        Reader r{ bytes.data(), bytes.data() + bytes.size() };
        char magic[4] = {};
        r.Take(magic, 4);
        if (r.ok && std::memcmp(magic, kLodMagic, 4) == 0 && r.U32() == kLodFormat)
        {
            const uint32_t count = r.U32();
            for (uint32_t i = 0; i < count && r.ok; ++i)
            {
                const uint64_t key = r.U64();
                std::vector<std::vector<u32>> lods(r.U32());
                for (auto& lod : lods)
                {
                    const uint32_t n = r.U32();
                    if (!r.ok || static_cast<size_t>(r.end - r.p) / sizeof(u32) < n) { r.ok = false; break; }
                    lod.resize(n);
                    r.Take(lod.data(), n * sizeof(u32));
                }
                if (r.ok) m_impl->loaded.emplace(key, std::move(lods));
            }
            if (!r.ok) m_impl->loaded.clear();   // 壊れていたら使わない（作り直して上書きされる）
        }
    }
    m_impl->prev = t_lod;
    t_lod = m_impl.get();
}

LodCacheScope::~LodCacheScope()
{
    if (!m_impl) return;
    t_lod = m_impl->prev;
    // 新しく作った / 使われなかった古い項目があるときだけ書き直す（毎回同じなら触らない）
    if (!m_impl->added && m_impl->loaded.empty()) return;
    if (m_impl->used.empty()) return;

    std::string out;
    out.append(kLodMagic, 4);
    PutU32(out, kLodFormat);
    PutU32(out, static_cast<uint32_t>(m_impl->used.size()));
    for (const auto& [key, lods] : m_impl->used)
    {
        PutU64(out, key);
        PutU32(out, static_cast<uint32_t>(lods.size()));
        for (const auto& lod : lods)
        {
            PutU32(out, static_cast<uint32_t>(lod.size()));
            out.append(reinterpret_cast<const char*>(lod.data()), lod.size() * sizeof(u32));
        }
    }
    const auto res = atomicfile::WriteFile(PathResolver::Utf8ToWide(m_impl->file), out);
    if (!res) Logger::Warn("LOD のキャッシュを保存できませんでした: {}", res.error);
}

bool LodCacheActive() { return t_lod != nullptr; }

bool FindLods(uint64_t key, std::vector<std::vector<u32>>& lods)
{
    if (!t_lod) return false;
    if (auto it = t_lod->used.find(key); it != t_lod->used.end()) { lods = it->second; return true; }
    auto it = t_lod->loaded.find(key);
    if (it == t_lod->loaded.end()) return false;
    lods = it->second;
    t_lod->used.emplace(key, std::move(it->second));
    t_lod->loaded.erase(it);
    return true;
}

void StoreLods(uint64_t key, const std::vector<std::vector<u32>>& lods)
{
    if (!t_lod) return;
    t_lod->used[key] = lods;
    t_lod->added = true;
}

} // namespace dx12e::modelcache
