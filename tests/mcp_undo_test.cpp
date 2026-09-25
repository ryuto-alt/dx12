// MCP（AI）の Undo 規則のテスト（editor/McpUndoRouter.h + editor/UndoCore.h）。
//
// 見ているもの:
//   ・呼び出し 1 回ぶん（横取りした物 + 値スナップショット）が「AI: <method>」1 エントリになる
//   ・onlyAi（MCP の undo の既定）は一番上が人の操作なら戻さずに断る＝人の編集を戻さない
//   ・トランザクションは 1 エントリにまとまり、Undo 1 回で全部が逆順に戻る
//   ・rollback は begin 以降を「逆順に」戻してスタックに何も残さない
//   ・入れ子の begin / 開いていない commit はエラー、放置は確定扱いで閉じる
//
// エンジンも entt も要らない（依存ゼロ）。実行: ctest --output-on-failure -R McpUndo

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "editor/McpUndoRouter.h"

using namespace dx12e;

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

// 値を before ⇔ after に書き換え、呼ばれた順を log に残すだけのコマンド。
struct SetInt : IUndoCommand
{
    SetInt(std::vector<std::string>* log, std::string name, int* target, int before, int after)
        : m_log(log), m_name(std::move(name)), m_target(target), m_before(before), m_after(after) {}
    void Undo() override { *m_target = m_before; m_log->push_back("U:" + m_name); }
    void Redo() override { *m_target = m_after;  m_log->push_back("R:" + m_name); }
    const char* GetName() const override { return m_name.c_str(); }

    std::vector<std::string>* m_log;
    std::string m_name;
    int* m_target;
    int  m_before, m_after;
};

// 「編集」を 1 回行う: 値を書き換えて、そのコマンドを UndoSystem へ積む
// （横取り中なら AI の呼び出しへ、そうでなければ人の操作としてスタックへ入る）。
void Edit(UndoSystem& u, std::vector<std::string>& log, const char* name, int& target, int value)
{
    const int before = target;
    target = value;
    u.PushCommand(std::make_unique<SetInt>(&log, name, &target, before, value));
}

// AI の呼び出し 1 回（ハンドラの中で Edit が走る想定）
void AiCall(McpUndoRouter& r, UndoSystem& u, std::vector<std::string>& log,
            const char* method, int& target, int value, double now = 0.0)
{
    r.BeginCall(method);
    Edit(u, log, method, target, value);
    r.EndCall(now);
}

void TestCallBecomesOneAiEntry()
{
    std::printf("[1] 呼び出し 1 回 = 「AI: <method>」1 エントリ（横取り + スナップショットを束ねる）\n");
    UndoSystem u; McpUndoRouter r(u);
    std::vector<std::string> log;
    int a = 0, b = 0;

    r.BeginCall("set_transform");
    Edit(u, log, "cap", a, 1);                                                  // 既存の積み口（横取り）
    r.AddToCall(std::make_unique<SetInt>(&log, "snap", &b, 0, 2)); b = 2;        // 値スナップショット
    Check(u.UndoDepth() == 0, "呼び出し中はスタックへ直接入らない（横取りされる）");
    Check(r.EndCall(0.0), "EndCall が積んだと答える");
    Check(u.UndoDepth() == 1, "2 つのコマンドが 1 エントリにまとまる");
    Check(u.PeekUndoIsAi(), "AI の印が付く");
    Check(std::string(u.PeekUndoName()) == "AI: set_transform", "名前は「AI: <method>」");

    u.Undo();
    Check(a == 0 && b == 0, "Undo 1 回で両方戻る");
    Check(log.size() == 2 && log[0] == "U:snap" && log[1] == "U:cap", "Undo は積んだ順の逆");

    r.BeginCall("get_entity");   // 何も積まない呼び出し（読み取り系・失敗した検証）
    Check(!r.EndCall(0.0), "何も積まない呼び出しはエントリを作らない");
    Check(u.UndoDepth() == 0, "空エントリが積まれていない");
    Check(!u.Capturing(), "EndCall で横取りが外れている");
}

void TestOnlyAiDoesNotUndoHuman()
{
    std::printf("[2] onlyAi: 人の編集は戻さない\n");
    UndoSystem u; McpUndoRouter r(u);
    std::vector<std::string> log;
    int ai = 0, human = 0;

    AiCall(r, u, log, "set_transform", ai, 5);
    Edit(u, log, "gizmo", human, 7);        // 人の操作（呼び出しの外）
    Check(!u.PeekUndoIsAi(), "人の操作には AI の印が付かない");

    std::string name;
    Check(r.Undo(true, &name) == McpUndoRouter::StepStatus::TopIsHuman, "一番上が人なら断る");
    Check(human == 7 && ai == 5, "断ったときは何も戻していない");
    Check(u.UndoDepth() == 2, "スタックも触っていない");

    Check(r.Undo(false, &name) == McpUndoRouter::StepStatus::Ok && name == "gizmo",
          "onlyAi:false なら人の操作も戻せる（明示したときだけ）");
    Check(human == 0, "人の編集が戻った");
    Check(r.Redo(true, &name) == McpUndoRouter::StepStatus::TopIsHuman, "redo も人の操作なら断る");
    Check(r.Redo(false, &name) == McpUndoRouter::StepStatus::Ok && human == 7, "redo（明示）");

    // 順序が逆（人 → AI）なら AI の分だけ戻る
    UndoSystem u2; McpUndoRouter r2(u2);
    int h2 = 0, a2 = 0;
    Edit(u2, log, "inspector", h2, 3);
    AiCall(r2, u2, log, "set_pbr", a2, 9);
    Check(r2.Undo(true, &name) == McpUndoRouter::StepStatus::Ok && name == "AI: set_pbr" && a2 == 0,
          "一番上が AI なら戻す");
    Check(r2.Undo(true, &name) == McpUndoRouter::StepStatus::TopIsHuman && h2 == 3,
          "その下の人の編集までは戻さない");
    UndoSystem u3; McpUndoRouter r3(u3);
    Check(r3.Undo(true, &name) == McpUndoRouter::StepStatus::Empty, "空なら Empty");
}

void TestTransactionIsOneEntry()
{
    std::printf("[3] トランザクション: begin〜commit が 1 エントリ、Undo 1 回で全部逆順に戻る\n");
    UndoSystem u; McpUndoRouter r(u);
    std::vector<std::string> log;
    int x = 0, y = 0, z = 0;

    Check(r.Begin("room layout", 0.0) == McpUndoRouter::TxStatus::Ok, "begin");
    AiCall(r, u, log, "set_transform", x, 1);
    AiCall(r, u, log, "set_pbr", y, 2);
    AiCall(r, u, log, "set_color", z, 3);
    Check(u.UndoDepth() == 0, "開いている間はスタックへ積まない");
    Check(r.TxCalls() == 3, "中身は呼び出し 3 回ぶん");
    const auto names = r.TxCallNames();
    Check(names.size() == 3 && names[0] == "AI: set_transform" && names[2] == "AI: set_color",
          "中身の名前が呼び出し順に並ぶ");

    std::string undone;
    Check(r.Undo(true, &undone) == McpUndoRouter::StepStatus::TxOpen, "開いている間の undo は断る");

    McpUndoRouter::TxResult res;
    Check(r.Commit(1.0, &res) == McpUndoRouter::TxStatus::Ok, "commit");
    Check(res.pushed && res.calls == 3 && res.entryName == "AI: room layout", "結果に件数と名前");
    Check(u.UndoDepth() == 1 && u.PeekUndoIsAi(), "1 エントリ（AI の印つき）");
    Check(std::string(u.PeekUndoName()) == "AI: room layout", "名前はラベル");
    Check(!r.TxOpen(), "閉じた");

    log.clear();
    Check(r.Undo(true, &undone) == McpUndoRouter::StepStatus::Ok, "undo 1 回");
    Check(x == 0 && y == 0 && z == 0, "3 つとも戻る");
    Check(log.size() == 3 && log[0] == "U:set_color" && log[1] == "U:set_pbr" && log[2] == "U:set_transform",
          "戻す順は後に積んだ方から");
    log.clear();
    Check(r.Redo(true, &undone) == McpUndoRouter::StepStatus::Ok && x == 1 && y == 2 && z == 3,
          "redo 1 回で 3 つとも戻る");
    Check(log.size() == 3 && log[0] == "R:set_transform" && log[2] == "R:set_color", "やり直しは積んだ順");

    // 空のトランザクションは何も積まない
    r.Begin("nothing", 2.0);
    McpUndoRouter::TxResult empty;
    r.Commit(3.0, &empty);
    Check(!empty.pushed && empty.calls == 0 && u.UndoDepth() == 1, "空の commit はエントリを作らない");
}

void TestRollback()
{
    std::printf("[4] rollback: begin 以降を逆順に戻し、スタックに何も残さない\n");
    UndoSystem u; McpUndoRouter r(u);
    std::vector<std::string> log;
    int before = 0, x = 10;

    AiCall(r, u, log, "create_entity", before, 1);   // トランザクション前の AI 操作（残るべき）
    const size_t depth = u.UndoDepth();

    r.Begin("try", 0.0);
    AiCall(r, u, log, "move1", x, 11);
    AiCall(r, u, log, "move2", x, 12);
    AiCall(r, u, log, "move3", x, 13);
    Check(x == 13, "3 回変えた");

    log.clear();
    McpUndoRouter::TxResult res;
    Check(r.Rollback(1.0, &res) == McpUndoRouter::TxStatus::Ok && res.calls == 3, "rollback");
    Check(x == 10, "begin 前の値へ戻る（同じ値を 3 回変えても最初の値）");
    Check(log.size() == 3 && log[0] == "U:move3" && log[1] == "U:move2" && log[2] == "U:move1",
          "逆順（move3 → move2 → move1）で戻す。順を間違えると途中の値が残る");
    Check(u.UndoDepth() == depth && std::string(u.PeekUndoName()) == "AI: create_entity",
          "スタックはトランザクション前のまま");
    Check(before == 1, "トランザクション前の変更は残る");
    Check(!u.CanRedo(), "rollback は redo にも何も残さない");
    Check(r.LastClosed() && r.LastClosed()->reason == "rollback", "閉じた理由が残る");
    Check(r.Rollback(2.0, &res) == McpUndoRouter::TxStatus::NotOpen, "2 回目は NotOpen");
}

void TestNestingAndStates()
{
    std::printf("[5] 入れ子の禁止 / 人の編集が挟まった件数 / 放置は確定扱い\n");
    UndoSystem u; McpUndoRouter r(u);
    std::vector<std::string> log;
    int v = 0, h = 0;

    Check(r.Commit(0.0, nullptr) == McpUndoRouter::TxStatus::NotOpen, "開いていない commit は NotOpen");
    Check(r.Begin("outer", 0.0) == McpUndoRouter::TxStatus::Ok, "begin");
    Check(r.Begin("inner", 0.0) == McpUndoRouter::TxStatus::AlreadyOpen, "入れ子の begin は AlreadyOpen");
    Check(r.TxLabel() == "outer", "外側のラベルのまま");

    AiCall(r, u, log, "set_transform", v, 1, 10.0);
    Edit(u, log, "human", h, 1);   // 開いている間の人の操作はそのままスタックへ
    Check(u.UndoDepth() == 1 && !u.PeekUndoIsAi(), "人の操作はトランザクションに入らない");
    Check(r.TxHumanPushes() == 1, "挟まった人の操作を数える");

    Check(!r.CloseIfIdle(500.0, 600.0), "最後の活動（t=10）から 490 秒ならまだ閉じない");
    r.Touch(400.0);
    Check(!r.CloseIfIdle(900.0, 600.0), "Touch で放置タイマーが延びる");
    Check(r.CloseIfIdle(1001.0, 600.0), "600 秒を超えたら閉じる");
    Check(r.LastClosed() && r.LastClosed()->reason == "idle_timeout" && r.LastClosed()->label == "outer",
          "理由 idle_timeout");
    Check(u.UndoDepth() == 2 && u.PeekUndoIsAi() && std::string(u.PeekUndoName()) == "AI: outer",
          "確定扱い: 変更は残り、1 エントリとして人の操作の上に積まれる");
    Check(v == 1, "値は変えたまま（黙って消さない）");

    r.Begin("again", 2000.0);
    Check(r.AutoClose("scene_changed", 2001.0), "シーン切り替えでも確定扱いで閉じる");
    Check(!r.AutoClose("scene_changed", 2002.0), "開いていなければ何もしない");
}

void TestCaptureOutsideCallIsHuman()
{
    std::printf("[6] 呼び出しの外で積まれた物は人の操作（横取りしない）\n");
    UndoSystem u; McpUndoRouter r(u);
    std::vector<std::string> log;
    int v = 0;
    r.BeginCall("a");
    r.BeginCall("nested");            // 入れ子は外側へまとめる
    Edit(u, log, "x", v, 1);
    Check(!r.EndCall(0.0), "内側の EndCall では積まない");
    Check(r.EndCall(0.0), "外側の EndCall で積む");
    Check(u.UndoDepth() == 1 && std::string(u.PeekUndoName()) == "AI: a", "外側の名前で 1 エントリ");
    Edit(u, log, "y", v, 2);
    Check(u.UndoDepth() == 2 && !u.PeekUndoIsAi(), "外で積んだ物は人の操作");
}
} // namespace

int main()
{
    std::printf("MCP の Undo（AI の印 / トランザクション / onlyAi）\n");
    TestCallBecomesOneAiEntry();
    TestOnlyAiDoesNotUndoHuman();
    TestTransactionIsOneEntry();
    TestRollback();
    TestNestingAndStates();
    TestCaptureOutsideCallIsHuman();
    std::printf("%s: %d checks / %d failures\n", g_failures == 0 ? "OK" : "NG", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
