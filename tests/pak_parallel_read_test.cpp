// AUD-05: PakArchive::Read のスレッドセーフ検証（GPU 非依存）。
//   - 圧縮・暗号化・非圧縮の混在 pak を作り、直列の読み取り結果を基準にする。
//   - 8 スレッド x 1000 回のランダムな読み取りが、全エントリで基準と一致すること。
//   - ファイル末尾をまたぐ破損 pak（切り詰め）は失敗を返し、クラッシュしないこと。

#include "core/vfs/PakArchive.h"
#include "core/vfs/PakFormat.h"
#include "core/vfs/PakWriter.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;
} // namespace

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static uint64_t Fnv(const std::vector<uint8_t>& v)
{
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : v)
    {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h ^ v.size();
}

int main()
{
    const fs::path dir = fs::temp_directory_path() / "dx12_pak_parallel_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string pakPath = (dir / "t.pak").string();

    // エントリ生成: 圧縮される（反復）/ されにくい（乱数）/ 小さい、を混ぜる。暗号化は全エントリ既定。
    constexpr int kEntries = 48;
    std::vector<std::string> names;
    {
        std::mt19937 rng(12345);
        vfs::PakWriter w;
        CHECK(w.Open(pakPath));
        for (int i = 0; i < kEntries; ++i)
        {
            std::vector<uint8_t> data;
            const std::size_t n = 1 + (rng() % 70000);
            data.resize(n);
            if (i % 3 == 0)
                for (std::size_t k = 0; k < n; ++k) data[k] = static_cast<uint8_t>((k / 7) & 0xFF); // 圧縮向き
            else
                for (std::size_t k = 0; k < n; ++k) data[k] = static_cast<uint8_t>(rng());           // 非圧縮向き
            const std::string rel = "data/e" + std::to_string(i) + ".bin";
            CHECK(w.AddBlob(rel, data.data(), data.size()));
            names.push_back(rel);
        }
        CHECK(w.Finish(/*stripStrings=*/true));
    }

    vfs::PakArchive pak;
    CHECK(pak.Mount(pakPath));

    // 直列の基準。
    std::vector<uint64_t> ref(kEntries);
    for (int i = 0; i < kEntries; ++i)
    {
        std::vector<uint8_t> out;
        CHECK(pak.Read(names[i], out));
        ref[i] = Fnv(out);
    }

    // 並列。
    constexpr int kThreads = 8, kIters = 1000;
    std::atomic<int> mismatches{0};
    std::vector<std::thread> th;
    for (int t = 0; t < kThreads; ++t)
        th.emplace_back([&, t]() {
            std::mt19937 rng(1000u + static_cast<unsigned>(t));
            std::vector<uint8_t> out;
            for (int it = 0; it < kIters; ++it)
            {
                const int i = static_cast<int>(rng() % kEntries);
                if (!pak.Read(names[i], out) || Fnv(out) != ref[i])
                    ++mismatches;
            }
        });
    for (auto& x : th) x.join();
    CHECK(mismatches.load() == 0);

    // 末尾をまたぐ読み取り: pak を途中で切り詰める（フッタが失われるので Mount 自体が失敗する）。
    {
        const std::string cut = (dir / "cut.pak").string();
        std::ifstream in(pakPath, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::ofstream o(cut, std::ios::binary);
        o.write(bytes.data(), static_cast<std::streamsize>(bytes.size() / 2));
        o.close();
        vfs::PakArchive bad;
        CHECK(!bad.Mount(cut));
        std::vector<uint8_t> out;
        CHECK(!bad.Read(names[0], out));
    }

    fs::remove_all(dir, ec);
    std::printf("PakParallelRead: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
