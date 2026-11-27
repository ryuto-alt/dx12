// editor/FrameThrottle.h（W1-A11 / MOD-ED-11: 裏に回ったときのフレーム間引き）の状態遷移の検査。
// 窓・ImGui・GPU は要らない（純ロジック。時計は引数で進める）。
//   ・非アクティブ = 10fps / アクティブで 5 秒活動なし = 30fps / それ以外 = 制限なし
//   ・MCP の要求・再生・ジョブ（busy）があると即座に tier 0 へ戻り、その後 5 秒は戻らない
//   ・非アクティブな窓の入力は活動に数えない（アクティブな窓の入力だけ数える）
//   ・「常にリアルタイム」と allowed=false（ゲーム・UI テスト）は常に tier 0
//   ・fps 上限の合成（既存の上限との min）
//   ・Application.cpp に配線の要点が残っている（回帰: 要求を受けた瞬間に起きる / 応答待ちを間引かない）
#include "core/mcp/McpPendingReplies.h"
#include "editor/FrameThrottle.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace dx12e;
namespace fs = std::filesystem;

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECKF(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

struct State   // EditorContext::FrameThrottleState と同じフィールド
{
    int    tier            = 0;
    double lastActivitySec = 0.0;
    bool   windowActive    = true;
    bool   alwaysRealtime  = false;
};

throttle::Inputs In(double now, bool active, bool busy = false, bool input = false)
{
    throttle::Inputs i;
    i.nowSec = now; i.windowActive = active; i.busy = busy; i.input = input;
    return i;
}

void TestTransitions()
{
    const throttle::Config cfg;
    State s;
    // 初回は活動として数える（起動直後の読み込みを遅らせない）
    CHECK(throttle::Evaluate(s, In(100.0, true), cfg) == 0);
    CHECK(throttle::Evaluate(s, In(104.9, true), cfg) == 0);          // 5 秒未満
    CHECK(throttle::Evaluate(s, In(105.0, true), cfg) == 1);          // アクティブで 5 秒活動なし → 30fps
    CHECK(throttle::TierFps(1, cfg) == 30.0f);
    // 入力で即座に tier 0 へ、その後 5 秒は戻らない
    CHECK(throttle::Evaluate(s, In(110.0, true, false, true), cfg) == 0);
    CHECK(throttle::Evaluate(s, In(114.9, true), cfg) == 0);
    CHECK(throttle::Evaluate(s, In(115.0, true), cfg) == 1);
    // 非アクティブ → 10fps（活動の猶予が過ぎていれば）
    CHECK(throttle::Evaluate(s, In(116.0, false), cfg) == 2);
    CHECK(throttle::TierFps(2, cfg) == 10.0f);
    // 非アクティブな窓の入力は活動に数えない
    CHECK(throttle::Evaluate(s, In(117.0, false, false, true), cfg) == 2);
    // busy（MCP の要求・再生・ジョブ）は窓の状態によらず即 tier 0、その後 5 秒は戻らない
    CHECK(throttle::Evaluate(s, In(120.0, false, true), cfg) == 0);
    CHECK(throttle::Evaluate(s, In(124.9, false), cfg) == 0);
    CHECK(throttle::Evaluate(s, In(125.0, false), cfg) == 2);
    // 連続する busy は猶予を延ばし続ける（step_frames の最中に間引かれない）
    for (int i = 0; i < 100; ++i)
        CHECKF(throttle::Evaluate(s, In(200.0 + i, false, true), cfg) == 0, "busy 中は tier 0 (i=%d)", i);
    CHECK(throttle::Evaluate(s, In(305.0, false), cfg) == 2);
    // アクティブ → 非アクティブに変わっても、直近の活動から 5 秒以内なら tier 0
    State t;
    throttle::Evaluate(t, In(10.0, true), cfg);
    throttle::Evaluate(t, In(12.0, true, false, true), cfg);
    CHECK(throttle::Evaluate(t, In(13.0, false), cfg) == 0);
    CHECK(throttle::Evaluate(t, In(17.0, false), cfg) == 2);
    CHECK(t.windowActive == false && t.tier == 2);
}

void TestDisabled()
{
    const throttle::Config cfg;
    {   // 常にリアルタイム
        State s; throttle::Evaluate(s, In(1.0, false), cfg);
        throttle::Inputs i = In(500.0, false); i.alwaysRealtime = true;
        CHECK(throttle::Evaluate(s, i, cfg) == 0);
        CHECK(s.alwaysRealtime);
        i.alwaysRealtime = false;
        CHECK(throttle::Evaluate(s, i, cfg) == 2);   // 設定を戻せば間引く
    }
    {   // ゲーム・UI テスト・エディタ無し
        State s; throttle::Evaluate(s, In(1.0, false), cfg);
        throttle::Inputs i = In(500.0, false); i.allowed = false;
        CHECK(throttle::Evaluate(s, i, cfg) == 0);
    }
}

void TestCombine()
{
    CHECK(throttle::CombineFps(144.0f, 0.0f) == 144.0f);     // 間引きなし = 既存の上限
    CHECK(throttle::CombineFps(0.0f, 0.0f) == 0.0f);          // 無制限のまま
    CHECK(throttle::CombineFps(0.0f, 30.0f) == 30.0f);        // 無制限でも 30 に制限
    CHECK(throttle::CombineFps(144.0f, 30.0f) == 30.0f);
    CHECK(throttle::CombineFps(20.0f, 30.0f) == 20.0f);       // 既存の上限がもっと低ければそちら
    CHECK(throttle::CombineFps(60.0f, 10.0f) == 10.0f);
}

// 時間（フレーム数）が前後で変わらないことの代理: busy が続く限り、1 フレームあたりの上限は常に既存の上限のまま。
void TestStepFramesUnchanged()
{
    const throttle::Config cfg;
    State s;
    throttle::Evaluate(s, In(1.0, false), cfg);
    double t = 1000.0;   // 十分に idle にしてから要求が来る
    CHECK(throttle::Evaluate(s, In(t, false), cfg) == 2);
    // 要求を受けた（busy）→ その後 step_frames の 600 フレーム（60fps で 10 秒）は一度も間引かれない
    int throttled = 0;
    for (int f = 0; f < 600; ++f)
    {
        t += 1.0 / 60.0;
        if (throttle::Evaluate(s, In(t, false, true), cfg) != 0) ++throttled;
    }
    CHECKF(throttled == 0, "step_frames 中に間引かれたフレーム = %d", throttled);
}

std::string ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// 配線の回帰: Application.cpp が間引きの要点を保っている（文字列の存在だけを見る粗い検査）。
void TestWiring()
{
#ifdef UI_LINT_ROOT
    const std::string app = ReadAll(fs::path(UI_LINT_ROOT) / "src" / "core" / "Application.cpp");
    CHECK(!app.empty());
    CHECK(app.find("throttle::Evaluate(") != std::string::npos);
    CHECK(app.find("perf.alwaysRealtime") != std::string::npos);
    // 設定画面（エンジン設定）に同じキーの切り替えがある（FU-E-14）
    const std::string insp = ReadAll(fs::path(UI_LINT_ROOT) / "src" / "editor" / "panels" / "InspectorPanel.cpp");
    CHECK(!insp.empty());
    CHECK(insp.find("prefs::GetBool(\"perf.alwaysRealtime\"") != std::string::npos);
    CHECK(insp.find("prefs::SetBool(\"perf.alwaysRealtime\"") != std::string::npos);
    // 待ち中に MCP を Poll する（要求を受けた瞬間に起きる）
    CHECK(app.find("pollMcp(); if (mcpActivity) break;") != std::string::npos);
    // 応答待ち・step_frames・仮想入力の待ち・ジョブを busy に数える
    CHECK(app.find("m_mcpStepFramesLeft > 0 || m_mcpStepReply.client != 0") != std::string::npos);
    CHECK(app.find("!m_mcpVInputWaits.empty()") != std::string::npos);
    CHECK(app.find("IsSceneLoadJobActive()") != std::string::npos);
    // 遅延応答・ジョブ（5 秒を超える要求でも間引かない）
    CHECK(app.find("m_mcpFinalShot.reply.client != 0 || m_mcpFinalShot.pending") != std::string::npos);
    CHECK(app.find("m_mcpPerceive != nullptr") != std::string::npos);
    CHECK(app.find("!m_hwReads.empty()") != std::string::npos);
    CHECK(app.find("!m_mcpUndoRequests.empty()") != std::string::npos);
    CHECK(app.find("m_deterministicFramesLeft > 0") != std::string::npos);
    CHECK(app.find("m_benchReply.client != 0") != std::string::npos);
    CHECK(app.find("m_loading || m_scenePreloadJob != nullptr") != std::string::npos);
    CHECK(app.find("m_thumbRenderer->GetPendingCount() > 0") != std::string::npos);
    CHECK(app.find("m_ptHost && m_ptHost->Busy()") != std::string::npos);
    // FU-E-15: editor_command_run の待ちと、未応答の遅延応答の数え上げ
    CHECK(app.find("HasPendingEditorRunWaits()") != std::string::npos);
    CHECK(app.find("mcppending::Replies().Count(") != std::string::npos);
    const std::string mcp = ReadAll(fs::path(UI_LINT_ROOT) / "src" / "core" / "mcp" / "ApplicationMcp.cpp");
    CHECK(mcp.find("mcppending::Replies().Add(") != std::string::npos);
    CHECK(mcp.find("mcppending::Replies().Resolve(") != std::string::npos);
    const std::string eui = ReadAll(fs::path(UI_LINT_ROOT) / "src" / "core" / "mcp" / "ApplicationMcpEditorUi.cpp");
    CHECK(eui.find("bool HasPendingEditorRunWaits()") != std::string::npos);
    // ゲームと UI テストでは間引かない
    CHECK(app.find("m_editorCtx && !m_isGameMode && !m_uiTestsRequested") != std::string::npos);
#endif
}

// 未応答の遅延応答の数え上げ（FU-E-15）
void TestPendingReplies()
{
    mcppending::PendingReplies r;
    CHECK(r.Count(100.0) == 0);
    r.Add(1, 10, 100.0);
    r.Add(1, 11, 100.5);
    r.Add(2, 10, 101.0);                 // 別クライアントの同じ id は別項目
    CHECK(r.Count(101.0) == 3);
    r.Resolve(1, 10);
    CHECK(r.Count(101.0) == 2);
    r.Resolve(1, 10);                    // 二重の Resolve・未登録の Resolve は無害
    r.Resolve(9, 99);
    CHECK(r.Count(101.0) == 2);
    // 応答が出ないまま古くなった項目は数えない（永久に busy にしない）
    CHECK(r.Count(131.0, 30.0) == 1);    // (1,11) は 30.5 秒経過で掃除され (2,10) だけが残る
    CHECK(r.Count(132.0, 30.0) == 0);
    r.Add(3, 1, 200.0);
    r.Clear();
    CHECK(r.Count(200.0) == 0);

    // 未応答があるあいだは busy として tier 0 に保たれ、応答後は猶予（5 秒）を経て非アクティブは tier 2 へ
    State st;
    r.Add(1, 1, 0.0);
    double t = 1.0;
    for (; t < 20.0; t += 1.0)
        CHECK(throttle::Evaluate(st, In(t, false, r.Count(t) > 0)) == 0);
    r.Resolve(1, 1);
    CHECK(throttle::Evaluate(st, In(t, false, r.Count(t) > 0)) == 0);
    CHECK(throttle::Evaluate(st, In(t + 6.0, false, r.Count(t + 6.0) > 0)) == 2);
}
} // namespace

int main()
{
    TestTransitions();
    TestDisabled();
    TestCombine();
    TestStepFramesUnchanged();
    TestWiring();
    TestPendingReplies();
    if (g_fail) { std::printf("frame_throttle_test: %d failure(s)\n", g_fail); return 1; }
    std::printf("frame_throttle_test: OK\n");
    return 0;
}
