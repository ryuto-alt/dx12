// W05-02（BUG-06）: 人の未保存の変更を自動保存が本体へ上書きしないことの検査。
//
//   ・編集の起点（EditOrigin）と UndoSystem::LastHumanEditSeq（editor/UndoCore.h）
//   ・自動保存の守り（core/AutosaveGuard.h）: 人の変更が有れば本体を書かず退避、MCP だけなら本体へ保存
//   ・退避の実体（core/SceneBackup.cpp の SnapshotExternal）: 本体は不変・理由つきの世代が 1 件・Restore で 1 操作で戻る
//
// エンジンも entt も要らない。実行: ctest --output-on-failure -R AutosaveOrigin

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "core/AutosaveGuard.h"
#include "core/SafeRemove.h"
#include "core/SceneBackup.h"
#include "editor/McpUndoRouter.h"

using namespace dx12e;
namespace fs = std::filesystem;

namespace
{
int g_failures = 0;
int g_checks   = 0;

void Check(bool cond, const char* label)
{
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::printf("  NG  %s\n", label);
}

struct SetInt : IUndoCommand
{
    SetInt(int* t, int b, int a) : m_t(t), m_b(b), m_a(a) {}
    void Undo() override { *m_t = m_b; }
    void Redo() override { *m_t = m_a; }
    const char* GetName() const override { return "SetInt"; }
    int* m_t; int m_b, m_a;
};

void Edit(UndoSystem& u, int& v, int value)
{
    const int before = v;
    v = value;
    u.PushCommand(std::make_unique<SetInt>(&v, before, value));
}

// EditorContext::IsSceneHumanDirty と同じ式（EditorContext はエンジンに依存するのでここでは写す）
bool HumanDirty(const UndoSystem& u, u64 savedSeq, bool humanSettingsDirty = false)
{
    return u.LastHumanEditSeq() > savedSeq || humanSettingsDirty;
}

struct Counters { int saveMain = 0; int backup = 0; };

autosaveguard::Ops MakeOps(Counters& c, bool backupOk = true, bool saveOk = true)
{
    autosaveguard::Ops ops;
    ops.saveMain = [&c, saveOk] { ++c.saveMain; return saveOk; };
    ops.backup = [&c, backupOk](std::string* err) -> std::string {
        ++c.backup;
        if (!backupOk) { if (err) *err = "disk full"; return {}; }
        return "gen_1";
    };
    return ops;
}

void TestOriginAndGuard()
{
    std::printf("[1] 人の Push は本体に書かない / MCP だけなら書く\n");
    using autosaveguard::Outcome;
    int v = 0;
    {   // (a) Human の Push
        UndoSystem u; Counters c;
        Edit(u, v, 1);
        Check(HumanDirty(u, 0), "(a) Human の Push は人の未保存");
        const auto r = autosaveguard::ProtectOrSave(HumanDirty(u, 0), MakeOps(c));
        Check(r.outcome == Outcome::Protected && c.saveMain == 0 && c.backup == 1, "(a) 本体に書かず退避が 1 回");
    }
    {   // (b) MCP の Push だけ
        UndoSystem u; Counters c;
        { EditOriginScope s(EditOrigin::Mcp); Edit(u, v, 2); }
        Check(!HumanDirty(u, 0), "(b) MCP の Push だけは人の未保存ではない");
        const auto r = autosaveguard::ProtectOrSave(HumanDirty(u, 0), MakeOps(c));
        Check(r.outcome == Outcome::SavedMain && c.saveMain == 1 && c.backup == 0, "(b) 本体へ保存");
    }
    {   // (c) Human の後に保存 → 次の MCP の Push は本体に書く
        UndoSystem u; Counters c;
        Edit(u, v, 3);
        const u64 saved = u.EditSeq();   // 保存した
        Check(!HumanDirty(u, saved), "(c) 保存の直後は人の未保存ではない");
        { EditOriginScope s(EditOrigin::Mcp); Edit(u, v, 4); }
        Check(!HumanDirty(u, saved), "(c) 保存の後の MCP の Push は人の未保存ではない");
        const auto r = autosaveguard::ProtectOrSave(HumanDirty(u, saved), MakeOps(c));
        Check(r.outcome == Outcome::SavedMain && c.saveMain == 1, "(c) 本体へ保存");
    }
    {   // (d) 入れ子と例外安全
        Check(CurrentEditOrigin() == EditOrigin::Human, "(d) 既定は Human");
        {
            EditOriginScope a(EditOrigin::Mcp);
            { EditOriginScope b(EditOrigin::Human); Check(CurrentEditOrigin() == EditOrigin::Human, "(d) 内 Human"); }
            Check(CurrentEditOrigin() == EditOrigin::Mcp, "(d) 外 Mcp");
        }
        try { EditOriginScope a(EditOrigin::Mcp); throw 1; } catch (int) {}
        Check(CurrentEditOrigin() == EditOrigin::Human, "(d) 例外でも復元");
    }
    {   // (e) 仮想入力は Human（MCP の呼び出しの中でも Human のスコープを張れば Human）
        UndoSystem u;
        EditOriginScope mcp(EditOrigin::Mcp);
        { EditOriginScope virt(EditOrigin::Human); Edit(u, v, 5); }
        Check(HumanDirty(u, 0), "(e) 仮想入力の編集は Human");
    }
    {   // (f) McpUndoRouter の捕捉中の Push は Mcp
        UndoSystem u;
        McpUndoRouter r(u);
        r.BeginCall("set_x");
        Edit(u, v, 6);
        r.EndCall(0.0);
        Check(!HumanDirty(u, 0), "(f) 横取り中の Push は Mcp");
    }
    {   // (g) MarkEdited: Human は LastHumanEditSeq に入り、Mcp は入らない
        UndoSystem u;
        { EditOriginScope s(EditOrigin::Mcp); u.MarkEdited(); }
        Check(!HumanDirty(u, 0), "(g) Mcp の MarkEdited");
        u.MarkEdited();
        Check(HumanDirty(u, 0), "(g) Human の MarkEdited");
    }
    {   // 退避に失敗したときは本体へ書き戻さない
        UndoSystem u; Counters c;
        Edit(u, v, 7);
        const auto r = autosaveguard::ProtectOrSave(HumanDirty(u, 0), MakeOps(c, /*backupOk=*/false));
        Check(r.outcome == Outcome::ProtectFailed && c.saveMain == 0 && !r.error.empty(), "退避失敗でも本体は書かない");
    }
    {   // 保存の失敗は SaveFailed
        Counters c;
        const auto r = autosaveguard::ProtectOrSave(false, MakeOps(c, true, /*saveOk=*/false));
        Check(r.outcome == Outcome::SaveFailed, "本体への保存の失敗");
    }
    {   // humanSettingsDirty だけでも守る
        UndoSystem u; Counters c;
        const auto r = autosaveguard::ProtectOrSave(HumanDirty(u, 0, /*humanSettingsDirty=*/true), MakeOps(c));
        Check(r.outcome == Outcome::Protected && c.saveMain == 0, "humanSettingsDirty だけでも本体に書かない");
    }
    {   // 実入力の人 / 仮想入力の判定（ConfirmDiscardScene と同じ式）。起点不明（reason 無し）は確認を出さない。
        struct PD { bool known; EditOrigin origin; bool virt; };
        auto real = [](PD p, bool vinput) { return p.known && p.origin == EditOrigin::Human && !p.virt && !vinput; };
        Check(real({true, EditOrigin::Human, false}, false), "実入力の人の閉じる要求は確認を出す");
        Check(!real({true, EditOrigin::Human, true}, false), "仮想入力の閉じる要求は確認を出さない");
        Check(!real({true, EditOrigin::Human, false}, true), "仮想入力モード中は確認を出さない");
        Check(!real({true, EditOrigin::Mcp, false}, false), "MCP 由来は確認を出さない");
        Check(!real({false, EditOrigin::Human, false}, false), "起点不明は確認を出さない");
    }
}

std::string ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
void WriteAll(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

void TestBackupRoundtrip()
{
    std::printf("[2] 退避の実体: 本体は不変・理由つきの世代・1 操作で戻る\n");
    const fs::path root = fs::temp_directory_path() / ("autosave_origin_test_" + std::to_string(std::time(nullptr)));
    { std::error_code e0; saferm::RemoveAll(root, e0, fs::temp_directory_path()); }
    const fs::path scene   = root / "assets" / "scenes" / "main.json";
    const fs::path staging = root / ".dx12" / "protect" / "main.json";
    WriteAll(scene, "{\"v\":\"DISK\"}");
    WriteAll(staging, "{\"v\":\"HUMAN\"}");
    scenebackup::GlobalPolicy() = scenebackup::Policy{};

    std::string why;
    const std::string id = scenebackup::SnapshotExternal(root, scene, staging, "protect-human-edits:Quit", &why);
    Check(!id.empty(), "退避の世代ができる");
    Check(ReadAll(scene) == "{\"v\":\"DISK\"}", "本体は不変");
    const auto gens = scenebackup::List(root, scene);
    Check(gens.size() == 1, "世代は 1 件");
    Check(!gens.empty() && gens[0].reason == "protect-human-edits:Quit", "理由が付く");
    Check(scenebackup::ReadReason(root, id) == "protect-human-edits:Quit", "ReadReason");

    // 同じ内容をもう一度 → 世代は増えず、同じ id が返る
    const std::string id2 = scenebackup::SnapshotExternal(root, scene, staging, "protect-human-edits:Quit", &why);
    Check(id2 == id && scenebackup::List(root, scene).size() == 1, "同じ内容では世代を増やさない");

    // 1 操作で戻る（scene_backups restore / 「以前の版に戻す」と同じ Restore）
    std::string pre;
    const auto r = scenebackup::Restore(root, scene, id, &pre);
    Check(r.ok, "Restore が成功");
    Check(ReadAll(scene) == "{\"v\":\"HUMAN\"}", "人の編集が戻る");

    // 退避先が名前違い・外のときは作らない（本体は触らない）
    WriteAll(root / ".dx12" / "protect" / "other.json", "x");
    Check(scenebackup::SnapshotExternal(root, scene, root / ".dx12" / "protect" / "other.json", "r").empty(), "名前が違う退避は断る");
    scenebackup::WaitIdle();
    std::error_code ec;
    saferm::RemoveAll(root, ec, fs::temp_directory_path());
}
} // namespace

int main()
{
    TestOriginAndGuard();
    TestBackupRoundtrip();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
