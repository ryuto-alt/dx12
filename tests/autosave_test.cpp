// オートセーブ退避スロット（core/AutosaveSlot.h）のテスト。
//
// 守りたいこと（どちらも壊れると作業が消える or 永久に聞かれ続ける）:
//  1) 明示保存 / 破棄をしたシーンの退避は確実に消える
//     → 消していなかったせいで「Ctrl+S で保存して終えても、次に開くたびに
//        『保存されていない自動保存が見つかりました』が出る」不具合になっていた。
//  2) 別シーンの退避は絶対に消さない（そこにしか無い未保存作業だから）
//  3) 中身が同じなら復旧の必要は無い（更新時刻の逆転で誤って聞かないための最後の砦）

#include "core/AutosaveSlot.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace dx12e;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);      \
            ++g_failed;                                                        \
        }                                                                      \
    } while (0)

static void WriteFile(const std::string& path, const std::string& body)
{
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << body;
}

// dir/ に originPath=origin の退避一式（scene.json / scene.nav / meta.json）を作る。
// meta は nlohmann で組む（Windows のパスは '\' を含むので手書きの JSON だと壊れる）。
static void MakeSlot(const std::string& dir, const std::string& origin,
                     const std::string& body = "{\"entities\":[]}")
{
    const nlohmann::json meta{
        {"originPath", origin}, {"engineVersion", "1.9.0"}, {"savedAtUnix", 1},
    };
    WriteFile(autosave::ScenePath(dir), body);
    WriteFile(autosave::NavPath(dir),   "NAVBINARY");
    WriteFile(autosave::MetaPath(dir),  meta.dump(2));
}

static bool SlotExists(const std::string& dir)
{
    std::error_code ec;
    return fs::exists(autosave::ScenePath(dir), ec) || fs::exists(autosave::MetaPath(dir), ec);
}

int main()
{
    std::error_code ec;
    const std::string root = (fs::temp_directory_path(ec) / "dx12_autosave_test").string() + "/";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const std::string dir     = root + ".autosave/";
    const std::string sceneA  = root + "levelA.json";
    const std::string sceneB  = root + "levelB.json";
    WriteFile(sceneA, "{\"entities\":[]}");
    WriteFile(sceneB, "{\"entities\":[1]}");

    // ---- OriginPath ----
    {
        MakeSlot(dir, sceneA);
        CHECK(autosave::OriginPath(dir) == sceneA, "originPath を読めること");
    }

    // ---- 同じシーンなら消える（保存 / 破棄の直後にここへ来る）----
    {
        MakeSlot(dir, sceneA);
        CHECK(autosave::DiscardIfFor(dir, sceneA), "同じシーンの退避は破棄される");
        CHECK(!SlotExists(dir), "scene.json / meta.json が消えている");
        std::error_code e2;
        CHECK(!fs::exists(autosave::NavPath(dir), e2), "サイドカー(.nav)も道連れに消える");
    }

    // ---- ★別シーンの退避は消さない（消すと未保存作業が失われる）----
    {
        MakeSlot(dir, sceneA);
        CHECK(!autosave::DiscardIfFor(dir, sceneB), "別シーンの退避は破棄しない");
        CHECK(SlotExists(dir), "別シーンの退避はディスクに残る");
        CHECK(autosave::OriginPath(dir) == sceneA, "残った退避は元のまま");
    }

    // ---- 区切り文字 / 相対の揺れを吸収する ----
    {
        std::string mixed = sceneA;
        for (auto& c : mixed) if (c == '/') c = '\\';
        MakeSlot(dir, mixed);
        CHECK(autosave::DiscardIfFor(dir, sceneA), "区切り文字が違っても同一シーンと判定する");
    }

    // ---- 退避が無ければ何もしない / 空パスは無視 ----
    {
        fs::remove_all(dir, ec);
        CHECK(!autosave::DiscardIfFor(dir, sceneA), "退避が無ければ false");
        CHECK(autosave::OriginPath(dir).empty(), "退避が無ければ originPath は空");
        MakeSlot(dir, sceneA);
        CHECK(!autosave::DiscardIfFor(dir, ""), "空パスでは何も消さない");
        CHECK(SlotExists(dir), "空パスの後も退避は残っている");
    }

    // ---- 壊れた meta.json は「無い」と同じ扱い（例外を投げない）----
    {
        MakeSlot(dir, sceneA);
        WriteFile(autosave::MetaPath(dir), "{ this is not json");
        CHECK(autosave::OriginPath(dir).empty(), "壊れた meta は空を返す");
        CHECK(!autosave::DiscardIfFor(dir, sceneA), "壊れた meta では消さない（判断できないため）");
    }

    // ---- SameBytes ----
    {
        const std::string p1 = root + "same1.bin";
        const std::string p2 = root + "same2.bin";
        const std::string p3 = root + "diff.bin";
        const std::string p4 = root + "shorter.bin";
        std::string big(200 * 1024, 'x');          // チャンク境界(64KB)をまたぐ長さ
        big[150 * 1024] = 'y';
        WriteFile(p1, big);
        WriteFile(p2, big);
        std::string other = big; other[199 * 1024] = 'z';   // 最終チャンクだけ違う
        WriteFile(p3, other);
        WriteFile(p4, big.substr(0, 100 * 1024));

        CHECK(autosave::SameBytes(p1, p2), "同じ中身なら true（64KB 境界をまたいでも）");
        CHECK(!autosave::SameBytes(p1, p3), "最後のチャンクだけ違っても false");
        CHECK(!autosave::SameBytes(p1, p4), "長さが違えば false");
        CHECK(!autosave::SameBytes(p1, root + "nope.bin"), "存在しないファイルは false");
        CHECK(autosave::SameBytes(p1, p1), "同一ファイル同士は true");
    }

    fs::remove_all(root, ec);

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
