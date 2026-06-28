// VFS / Crypto / Compression / Pak の単体テスト（GPU 非依存）。
//
// 外部テストフレームワークに依存しない（ctest が終了コードで合否判定）。
// テスト:
//   - AES-256-GCM encrypt -> decrypt 往復
//   - ciphertext 1 バイト改竄 -> decrypt が false
//   - XPRESS_HUFF compress -> decompress 往復
//   - PakWriter -> PakArchive 往復（temp dir に数ファイル書いて pack -> mount -> 全エントリ照合）

#include "core/vfs/Crypto.h"
#include "core/vfs/Compression.h"
#include "core/vfs/PakWriter.h"
#include "core/vfs/PakArchive.h"
#include "core/vfs/PakFormat.h"
#include "core/vfs/Vfs.h"
#include "core/PathResolver.h"

#include <array>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <string>
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

static std::vector<uint8_t> MakeBytes(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

static void Test_AesRoundTrip()
{
    std::array<uint8_t, vfs::kKeyLen> key{};
    vfs::AssembleKey(key);

    std::vector<uint8_t> plain = MakeBytes("Hello GMPK! AES-256-GCM round trip test payload 0123456789");
    uint8_t nonce[vfs::kNonceLen] = {};
    uint8_t tag[vfs::kTagLen]     = {};
    std::vector<uint8_t> cipher;
    CHECK(vfs::AesGcmEncrypt(key.data(), plain.data(), plain.size(), nonce, tag, cipher));
    CHECK(cipher.size() == plain.size()); // GCM は平文長を変えない
    CHECK(cipher != plain);

    std::vector<uint8_t> out;
    CHECK(vfs::AesGcmDecrypt(key.data(), cipher.data(), cipher.size(), nonce, tag, out));
    CHECK(out == plain);
}

static void Test_AesTamperFails()
{
    std::array<uint8_t, vfs::kKeyLen> key{};
    vfs::AssembleKey(key);

    std::vector<uint8_t> plain = MakeBytes("tamper detection target bytes ----------------------------");
    uint8_t nonce[vfs::kNonceLen] = {};
    uint8_t tag[vfs::kTagLen]     = {};
    std::vector<uint8_t> cipher;
    CHECK(vfs::AesGcmEncrypt(key.data(), plain.data(), plain.size(), nonce, tag, cipher));
    CHECK(!cipher.empty());

    // ciphertext 1 バイトを反転 -> tag mismatch で false になるべき。
    cipher[cipher.size() / 2] ^= 0xFF;
    std::vector<uint8_t> out;
    CHECK(!vfs::AesGcmDecrypt(key.data(), cipher.data(), cipher.size(), nonce, tag, out));

    // tag 改竄でも false。
    std::vector<uint8_t> cipher2;
    CHECK(vfs::AesGcmEncrypt(key.data(), plain.data(), plain.size(), nonce, tag, cipher2));
    tag[0] ^= 0x01;
    std::vector<uint8_t> out2;
    CHECK(!vfs::AesGcmDecrypt(key.data(), cipher2.data(), cipher2.size(), nonce, tag, out2));
}

static void Test_XpressRoundTrip()
{
    // 圧縮しやすい入力（繰り返し）。
    std::string s;
    for (int i = 0; i < 4096; ++i)
        s += "ABCDEFGH";
    std::vector<uint8_t> plain = MakeBytes(s);

    std::vector<uint8_t> compressed;
    CHECK(vfs::XpressCompress(plain.data(), plain.size(), compressed));
    CHECK(compressed.size() < plain.size()); // 厳密に小さい

    std::vector<uint8_t> out;
    CHECK(vfs::XpressDecompress(compressed.data(), compressed.size(), plain.size(), out));
    CHECK(out == plain);

    // 展開後サイズが食い違えば false。
    std::vector<uint8_t> bad;
    CHECK(!vfs::XpressDecompress(compressed.data(), compressed.size(), plain.size() + 7, bad));
}

static void Test_PakRoundTrip()
{
    fs::path dir = fs::temp_directory_path() / "dx12_vfs_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "src", ec);

    // ソースファイルを用意（圧縮しやすいもの・しにくいもの両方）。
    struct Item { std::string rel; std::vector<uint8_t> bytes; };
    std::vector<Item> items;

    {
        std::string compr;
        for (int i = 0; i < 2000; ++i)
            compr += "the quick brown fox 0123 ";
        items.push_back({"scripts/game.lua", MakeBytes(compr)});
    }
    {
        // 擬似ランダム（圧縮しにくい -> store パスを通す）。
        std::vector<uint8_t> rnd(5000);
        uint32_t st = 0x12345678u;
        for (auto& b : rnd) { st = st * 1664525u + 1013904223u; b = static_cast<uint8_t>(st >> 24); }
        items.push_back({"textures/noise.bin", rnd});
    }
    {
        items.push_back({"scenes/title.json", MakeBytes("{\"name\":\"title\",\"entities\":[]}")});
    }

    // ディスクへ書く（fN.dat という名前で、index で pack 時に再導出）。
    int n = 0;
    for (const auto& it : items)
    {
        fs::path p = dir / "src" / ("f" + std::to_string(n++) + ".dat");
        std::ofstream o(p, std::ios::binary);
        o.write(reinterpret_cast<const char*>(it.bytes.data()),
                static_cast<std::streamsize>(it.bytes.size()));
    }

    // pack。
    fs::path pakPath = dir / "test.pak";
    {
        vfs::PakWriter w;
        CHECK(w.Open(pakPath.string()));
        int k = 0;
        for (auto& it : items)
        {
            fs::path p = dir / "src" / ("f" + std::to_string(k++) + ".dat");
            CHECK(w.AddFile(p.string(), it.rel));
        }
        // blob（__manifest__ 相当）。
        std::string manifest = "{\"title\":\"T\",\"startScene\":\"scenes/title.json\"}";
        CHECK(w.AddBlob("__manifest__",
                        reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()));
        CHECK(w.Finish(/*stripStrings=*/false));
    }
    CHECK(fs::exists(pakPath));

    // mount して全エントリ照合。
    {
        vfs::PakArchive a;
        CHECK(a.Mount(pakPath.string()));
        for (auto& it : items)
        {
            std::vector<uint8_t> got;
            CHECK(a.Has(it.rel));
            CHECK(a.Read(it.rel, got));
            CHECK(got == it.bytes);
        }
        // パス正規化が writer/runtime で一致（先頭 assets/ 剥がし、大文字でもヒット）。
        std::vector<uint8_t> g2;
        CHECK(a.Read("SCRIPTS/GAME.LUA", g2));
        CHECK(g2 == items[0].bytes);
        CHECK(a.Read("assets/textures/noise.bin", g2)); // assets/ プレフィックスは剥がれる
        CHECK(g2 == items[1].bytes);

        std::vector<uint8_t> manifestOut;
        CHECK(a.Read("__manifest__", manifestOut));
        CHECK(!manifestOut.empty());

        std::vector<uint8_t> miss;
        CHECK(!a.Read("does/not/exist.png", miss));
    }

    fs::remove_all(dir, ec);
}

// 実行時の VFS ファサード（pak マウント時）が、ローダの渡す絶対パスを正しく
// pak キーへ解決することを検証する。特に assets/ の外に在る scripts/・shaders/ を
// BaseDir 相対で引けること（= ビルドしたゲームで game.lua とシェーダが読めること）。
static void Test_ReadAssetAbs_GameMode()
{
    fs::path dir = fs::temp_directory_path() / "dx12_vfs_abs_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "src", ec);

    // PathResolver を testDir に向ける: BaseDir=dir/, AssetsDir=dir/assets/, ScriptsDir=dir/scripts/。
    PathResolver::Initialize(false);
    PathResolver::SetProjectRoot(dir.generic_string());

    struct Item { std::string rel; std::vector<uint8_t> bytes; };
    std::vector<Item> items = {
        { "scripts/game.lua",       MakeBytes("function OnUpdate(dt) end") },
        { "shaders/Forward_VS.cso", MakeBytes("DXBC fake bytecode 0123456789") },
        { "textures/foo.png",       MakeBytes("\x89PNG fake texture bytes") },
    };

    int n = 0;
    for (auto& it : items)
    {
        fs::path p = dir / "src" / ("f" + std::to_string(n++) + ".dat");
        std::ofstream o(p, std::ios::binary);
        o.write(reinterpret_cast<const char*>(it.bytes.data()),
                static_cast<std::streamsize>(it.bytes.size()));
    }

    fs::path pakPath = dir / "game.pak";
    {
        vfs::PakWriter w;
        CHECK(w.Open(pakPath.string()));
        int k = 0;
        for (auto& it : items)
        {
            fs::path p = dir / "src" / ("f" + std::to_string(k++) + ".dat");
            CHECK(w.AddFile(p.string(), it.rel));
        }
        CHECK(w.Finish(/*stripStrings=*/true));
    }

    CHECK(vfs::MountPak(pakPath.string()));
    CHECK(vfs::InGameMode());

    const std::string base   = PathResolver::BaseDir();    // dir/
    const std::string assets = PathResolver::AssetsDir();  // dir/assets/

    // 相対キー（既存パス）。
    CHECK(vfs::ReadAsset("scripts/game.lua") == items[0].bytes);

    // 絶対パス -> BaseDir 相対キー解決（バグ#1: game.lua、目標#3: shaders の実行時経路）。
    CHECK(vfs::ReadAssetAbs(base + "scripts/game.lua")       == items[0].bytes);
    CHECK(vfs::ReadAssetAbs(base + "shaders/Forward_VS.cso") == items[1].bytes);

    // 絶対パス -> AssetsDir 相対キー解決（テクスチャ等の既存経路が壊れていない）。
    CHECK(vfs::ReadAssetAbs(assets + "textures/foo.png")     == items[2].bytes);

    // pak に無いものは空（ゲームモードでディスクへフォールバックして漏れない）。
    CHECK(vfs::ReadAssetAbs(base + "scripts/missing.lua").empty());

    vfs::Unmount();
    CHECK(!vfs::InGameMode());

    fs::remove_all(dir, ec);
}

int main()
{
    Test_AesRoundTrip();
    Test_AesTamperFails();
    Test_XpressRoundTrip();
    Test_PakRoundTrip();
    Test_ReadAssetAbs_GameMode();

    std::printf("Vfs: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
