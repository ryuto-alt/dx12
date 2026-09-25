// セーブファイル（core/save/SaveFile.h / UserData.h）のテスト。
//
// 守りたいこと（壊れると「プレイヤーの数十時間が消える」か「別のファイルを上書きする」）:
//  1) スロット名はファイル名にそのまま使うので、パス区切り・..・予約名を絶対に通さない
//  2) 書いた本体がそのまま読み戻せる（往復）。一覧は新しい順で、壊れたスロットも見える
//  3) ゲーム名のフォルダ化で Windows が黙って落とす文字（末尾のドット等）を残さない
//
// ファイル操作は IFileOps の偽物（メモリ上）で回す＝失敗の注入ができる。実ファイルでも 1 往復だけ確かめる。

#include "core/save/SaveFile.h"
#include "core/save/UserData.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <string>

namespace fs = std::filesystem;
using namespace dx12e;
using nlohmann::json;

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

// メモリ上のファイル群。failWriteOn に一致するパス（末尾一致）への書き込みを失敗させられる。
class FakeFileOps final : public save::IFileOps
{
public:
    std::map<std::wstring, std::string> files;
    std::wstring failWriteSuffix;     // これで終わるパスへの WriteAll は失敗（中途半端に半分だけ書く）
    int writes = 0;

    static std::wstring Key(const fs::path& p) { return p.lexically_normal().wstring(); }
    static bool EndsWith(const std::wstring& s, const std::wstring& suf)
    {
        return !suf.empty() && s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    }

    bool ReadAll(const fs::path& p, std::string& out) override
    {
        auto it = files.find(Key(p));
        if (it == files.end()) return false;
        out = it->second;
        return true;
    }
    bool WriteAll(const fs::path& p, std::string_view bytes) override
    {
        ++writes;
        if (EndsWith(Key(p), failWriteSuffix))
        {
            files[Key(p)] = std::string(bytes.substr(0, bytes.size() / 2));   // 途中で死んだ
            return false;
        }
        files[Key(p)] = std::string(bytes);
        return true;
    }
    bool Exists(const fs::path& p) override { return files.count(Key(p)) != 0; }
    bool Remove(const fs::path& p) override { files.erase(Key(p)); return true; }
    bool CreateDirs(const fs::path&) override { return true; }
    std::vector<std::string> ListFiles(const fs::path& dir) override
    {
        std::vector<std::string> out;
        const std::wstring d = Key(dir);
        for (const auto& [k, v] : files)
        {
            (void)v;
            const fs::path p(k);
            if (p.parent_path().lexically_normal().wstring() == d)
            {
                const std::u8string u8 = p.filename().u8string();
                out.emplace_back(u8.begin(), u8.end());
            }
        }
        return out;
    }
};

static json Body(const std::string& slot, long long t, json data)
{
    return json{{"engineFormat", 1}, {"slot", slot}, {"savedAtUnix", t}, {"playTime", 12.5},
                {"data", std::move(data)}, {"entities", json::array()}};
}

int main()
{
    // ---- 1) スロット名 ----
    {
        CHECK(save::IsValidSlotName("1"), "数字");
        CHECK(save::IsValidSlotName("auto_save-2"), "英数 _ -");
        CHECK(!save::IsValidSlotName(""), "空は不可");
        CHECK(!save::IsValidSlotName("../x"), "親ディレクトリへ出られない");
        CHECK(!save::IsValidSlotName("a/b"), "スラッシュ不可");
        CHECK(!save::IsValidSlotName("a\\b"), "バックスラッシュ不可");
        CHECK(!save::IsValidSlotName("C:x"), "ドライブ指定不可");
        CHECK(!save::IsValidSlotName("a b"), "空白不可");
        CHECK(!save::IsValidSlotName("con"), "予約名 CON（小文字でも）");
        CHECK(!save::IsValidSlotName("COM1"), "予約名 COM1");
        CHECK(save::IsValidSlotName("CONSOLE"), "予約名を含むだけの名前は可");
        CHECK(!save::IsValidSlotName(std::string(65, 'a')), "65 文字は長すぎ");
        std::string why;
        save::IsValidSlotName("a/b", &why);
        CHECK(!why.empty(), "理由が返る");
    }

    // ---- 3) フォルダ名 ----
    {
        CHECK(save::SanitizeFolderName("My Game") == "My Game", "普通の名前はそのまま");
        CHECK(save::SanitizeFolderName("A:B/C?") == "ABC", "禁止文字を落とす");
        CHECK(save::SanitizeFolderName("Game. . ") == "Game", "末尾のドットと空白を落とす");
        CHECK(save::SanitizeFolderName("") == "DX12Game", "空なら既定名");
        CHECK(save::SanitizeFolderName("..") == "DX12Game", "ドットだけも既定名");
        CHECK(save::SanitizeFolderName("\xE9\x9B\xA8\xE3\x81\xAE\xE6\x97\xA5") == "\xE9\x9B\xA8\xE3\x81\xAE\xE6\x97\xA5",
              "日本語（UTF-8）はそのまま");
    }

    // ---- 2) 往復と一覧 ----
    {
        FakeFileOps ops;
        const fs::path dir = L"C:/fake/saves";
        const json data = {{"coins", 12}, {"name", "\xE5\x8B\x87\xE8\x80\x85"}, {"pos", {1.5, 2, 3}},
                           {"flags", {{"door1", true}}}};
        auto w = save::WriteSlot(ops, dir, "1", save::EncodeSaveFile(Body("1", 100, data)));
        CHECK(w.ok, "書ける");
        auto r = save::ReadSlot(ops, dir, "1");
        CHECK(r.ok(), "読める");
        CHECK(r.decoded.body["data"] == data, "data が往復で変わらない");
        CHECK(r.decoded.body["playTime"].get<double>() == 12.5, "playTime が往復");

        CHECK(!save::WriteSlot(ops, dir, "../evil", "x").ok, "不正なスロット名では書かない");
        CHECK(save::ReadSlot(ops, dir, "nope").decoded.status == save::DecodeStatus::Missing, "無いスロットは Missing");

        save::WriteSlot(ops, dir, "old", save::EncodeSaveFile(Body("old", 50, json::object())));
        save::WriteSlot(ops, dir, "new", save::EncodeSaveFile(Body("new", 900, json::object())));
        ops.files[FakeFileOps::Key(save::SlotPath(dir, "broken"))] = "{ this is not json";
        auto list = save::ListSlots(ops, dir);
        CHECK(list.size() == 4, "スロット 4 本（壊れたものも含む）");
        if (list.size() == 4)
        {
            CHECK(list[0].slot == "new", "新しい順 1: new");
            CHECK(list[1].slot == "1", "新しい順 2: 1");
            CHECK(list[2].slot == "old", "新しい順 3: old");
            CHECK(list[3].slot == "broken" && list[3].status != save::DecodeStatus::Ok, "壊れたものは最後 + 状態付き");
        }
        CHECK(save::DeleteSlot(ops, dir, "old"), "消せる");
        CHECK(save::ReadSlot(ops, dir, "old").decoded.status == save::DecodeStatus::Missing, "消えた");
    }

    // ---- 実ファイルで 1 往復（Win32 のワイド API 経路。日本語のフォルダ名）----
    {
        std::error_code ec;
        const fs::path root = fs::temp_directory_path() / L"dx12_save_test_\u96e8";
        fs::remove_all(root, ec);
        auto& ops = save::RealFileOps();
        const json data = {{"hp", 3}, {"list", {1, 2, 3}}};
        auto w = save::WriteSlot(ops, root, "slot_a", save::EncodeSaveFile(Body("slot_a", 1, data)));
        CHECK(w.ok, "実ファイルに書ける（日本語フォルダ）");
        auto r = save::ReadSlot(ops, root, "slot_a");
        CHECK(r.ok() && r.decoded.body["data"] == data, "実ファイルから読み戻せる");
        CHECK(save::ListSlots(ops, root).size() == 1, "実ファイルの一覧");
        fs::remove_all(root, ec);
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
