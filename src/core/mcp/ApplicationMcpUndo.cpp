// ===========================================================================
// Application: MCP の Undo / Redo / トランザクション
// ---------------------------------------------------------------------------
// 仕組みの全体は editor/McpUndoRouter.h の冒頭。ここは MCP の口とフレーム境界の実処理。
//
// ■ メソッド
//   undo {onlyAi?}            … 1 つ戻す（遅延応答）。onlyAi は既定 true（下の理由）
//   redo {onlyAi?}            … 1 つやり直す（遅延応答）
//   transaction_begin {label} … 以降の MCP 編集を 1 エントリ「AI: <label>」へまとめ始める
//   transaction_commit        … まとめた物を 1 エントリとして積む（遅延応答）
//   transaction_rollback      … begin 以降の MCP 編集を全部逆順に戻す（遅延応答）
//   transaction_status        … 開いているか / 中身 / 放置時間 / 直前に閉じた理由
//
// ■ 閉じ方の約束（明記しておく）
//   ・入れ子は禁止（begin 中の begin は MODE_CONFLICT）
//   ・Play 中は begin できない（MODE_CONFLICT）。開いている間の MCP play / open_scene /
//     new_scene / open_project も MODE_CONFLICT（Play/Stop とシーン読み込みは Undo 履歴を消すので、
//     開いたまま進むと rollback できなくなる）
//   ・人が Play / シーン切り替え / Ctrl+Z / Ctrl+Y をした、または最後の MCP 活動から
//     kMcpTxIdleTimeoutSec（600 秒）放置された → **確定扱いで自動的に閉じる**
//     （変更は残し、1 エントリとして積む＝Undo 1 回で丸ごと戻せる）。黙って巻き戻すと
//     画面に見えている作業（オートセーブで保存済みのこともある）を誰の指示もなく消すので、そちらにはしない。
//     閉じた理由は transaction_status の lastClosed と、次の commit / rollback のエラーで分かる
//   ・commit / rollback の応答が返る前に届いた書き込みは MODE_CONFLICT（内外どちらか決められない）
// ===========================================================================
#include "core/ApplicationInternal.h"

#include <chrono>

namespace dx12e
{
using namespace appdetail;
using json = nlohmann::json;

double Application::McpNowSec() const
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void Application::McpUndoAutoClose(const char* reason)
{
    if (!m_editorCtx) return;
    auto& router = m_editorCtx->mcpUndo;
    if (!router.TxOpen()) return;
    const std::string label = router.TxLabel();
    const size_t      calls = router.TxCalls();
    if (router.AutoClose(reason, McpNowSec()))
        Logger::Warn("MCP トランザクション『{}』（{} 件）を確定扱いで閉じました（理由: {}）。"
                     "Undo 1 回で丸ごと戻せます", label, calls, reason);
}

namespace
{
nlohmann::json UndoTopsJson(const UndoSystem& u)
{
    auto top = [](const char* name, bool ai) -> nlohmann::json {
        if (!name) return nullptr;
        return {{"name", name}, {"ai", ai}};
    };
    return {{"undo", top(u.PeekUndoName(), u.PeekUndoIsAi())},
            {"redo", top(u.PeekRedoName(), u.PeekRedoIsAi())}};
}

std::string LastClosedHint(const McpUndoRouter& r)
{
    const McpTxClosed* lc = r.LastClosed();
    if (!lc) return "transaction_begin で開いてから呼ぶこと";
    if (lc->reason == "commit" || lc->reason == "rollback")
        return "直前のトランザクション『" + lc->label + "』は既に " + lc->reason +
               " 済み。続けて編集をまとめたいなら transaction_begin からやり直すこと";
    return "直前のトランザクション『" + lc->label + "』は " + lc->reason +
           " で確定扱いに自動で閉じられた（変更は残っていて Undo 1 回で丸ごと戻せる）。"
           "戻したいなら undo、まとめ直すなら transaction_begin";
}
} // namespace

void Application::RegisterMcpUndoMethods()
{
    // ★onlyAi の既定は true。
    //   AI が undo を呼ぶのはほぼ「自分の直前の変更を取り消したい」とき。一番上が人の編集だった
    //   場合にそれを黙って戻すと、人は自分の作業が消えた理由を知る手段が無い（画面の外で AI が
    //   やったこと）。戻せないことはエラー + ヒントで AI には伝わり、人の編集ごと戻したいなら
    //   onlyAi:false を明示すればよい＝壊れる側に倒さない。
    //   以前は既定の undo が人の編集を戻していた（MCP の編集がほぼ Undo に積まれなかったので、
    //   「直前の set_transform を戻す」つもりの undo がその下の人の操作に当たっていた）。
    // ★実処理はフレーム境界（ProcessMcpUndoRequests）。生成・削除の遅延処理が先に積む物を
    //   含めて判定するためと、削除の取り消しがモデルの再読み込み（cmdList）を要するため。
    McpDefine("undo", "onlyAi:bool", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot undo while Playing",
                    "dx12_stop してから呼ぶこと（★Stop で Undo 履歴は消え、Play 開始時の状態へ戻る）");
            if (m_editorCtx->mcpUndo.TxOpen())
                throw McpError(McpErr::ModeConflict,
                    "transaction '" + m_editorCtx->mcpUndo.TxLabel() + "' is open",
                    "begin 以降を全部戻すなら transaction_rollback、残すなら transaction_commit してから undo");
            m_mcpUndoRequests.push_back({McpUndoRequest::Kind::Undo, params.value("onlyAi", true), deferred});
            isDeferred = true;
        });

    McpDefine("redo", "onlyAi:bool", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot redo while Playing",
                    "dx12_stop してから呼ぶこと（★Stop で Undo 履歴は消える）");
            if (m_editorCtx->mcpUndo.TxOpen())
                throw McpError(McpErr::ModeConflict,
                    "transaction '" + m_editorCtx->mcpUndo.TxLabel() + "' is open",
                    "transaction_commit か transaction_rollback で閉じてから redo");
            m_mcpUndoRequests.push_back({McpUndoRequest::Kind::Redo, params.value("onlyAi", true), deferred});
            isDeferred = true;
        });

    McpDefine("transaction_begin", "label:string", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot begin a transaction while Playing",
                    "dx12_stop してから begin すること（Play 中の変更は Stop で捨てられ、Undo 履歴も消える）");
            auto& router = m_editorCtx->mcpUndo;
            std::string label = params.value("label", std::string());
            if (label.empty()) label = "transaction";
            if (router.Begin(label, McpNowSec()) == McpUndoRouter::TxStatus::AlreadyOpen)
                throw McpError(McpErr::ModeConflict,
                    "transaction '" + router.TxLabel() + "' is already open (nesting is not allowed)",
                    "入れ子は不可。transaction_status で中身を確かめ、transaction_commit か "
                    "transaction_rollback で閉じてから begin すること");
            resp["ok"] = true;
            resp["result"] = {
                {"open", true}, {"label", label},
                {"entryName", "AI: " + label},
                {"undoDepth", m_editorCtx->undoSystem.UndoDepth()},
                {"idleTimeoutSec", kMcpTxIdleTimeoutSec},
                {"note", "以降の MCP の編集（生成・削除・複製・Transform・コンポーネント・親子・名前・"
                         "色/PBR/テクスチャ・Lua プロパティ・地形/スカルプト）は 1 エントリにまとまる。"
                         "transaction_commit で積む / transaction_rollback で begin 前へ戻す。"
                         "Play・シーン切り替えは閉じるまで断る。放置 600 秒か、人の Play / シーン切り替え / "
                         "Ctrl+Z で確定扱いに自動で閉じる"}};
        });

    McpDefine("transaction_commit", "", DX12E_MCP_HANDLER
        {
            if (!m_editorCtx->mcpUndo.TxOpen())
                throw McpError(McpErr::ModeConflict, "no open transaction", LastClosedHint(m_editorCtx->mcpUndo));
            m_mcpUndoRequests.push_back({McpUndoRequest::Kind::Commit, true, deferred});
            isDeferred = true;
        });

    McpDefine("transaction_rollback", "", DX12E_MCP_HANDLER
        {
            if (!m_editorCtx->mcpUndo.TxOpen())
                throw McpError(McpErr::ModeConflict, "no open transaction", LastClosedHint(m_editorCtx->mcpUndo));
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot roll back while Playing",
                    "dx12_stop してから（★ただし Stop で Undo 履歴は消えるので、Play 前に閉じるのが正しい順）");
            m_mcpUndoRequests.push_back({McpUndoRequest::Kind::Rollback, true, deferred});
            isDeferred = true;
        });

    McpDefine("transaction_status", "", DX12E_MCP_HANDLER
        {
            const auto& router = m_editorCtx->mcpUndo;
            const double now = McpNowSec();
            json r;
            r["open"] = router.TxOpen();
            if (router.TxOpen())
            {
                r["label"] = router.TxLabel();
                r["calls"] = router.TxCalls();
                json names = json::array();
                const auto all = router.TxCallNames();
                for (size_t i = 0; i < all.size() && i < 64; ++i) names.push_back(all[i]);
                r["callNames"] = std::move(names);   // 多いときは先頭 64 件
                r["ageSec"]  = now - router.TxOpenedAt();
                r["idleSec"] = now - router.TxLastActivity();
                r["autoCloseInSec"] = (std::max)(0.0, kMcpTxIdleTimeoutSec - (now - router.TxLastActivity()));
                r["humanEditsDuringTransaction"] = router.TxHumanPushes();
            }
            r["idleTimeoutSec"] = kMcpTxIdleTimeoutSec;
            r["closePending"] = std::any_of(m_mcpUndoRequests.begin(), m_mcpUndoRequests.end(),
                [](const McpUndoRequest& q) {
                    return q.kind == McpUndoRequest::Kind::Commit || q.kind == McpUndoRequest::Kind::Rollback;
                });
            if (const McpTxClosed* lc = router.LastClosed())
                r["lastClosed"] = {{"label", lc->label}, {"reason", lc->reason},
                                   {"calls", lc->calls}, {"agoSec", now - lc->at}};
            else
                r["lastClosed"] = nullptr;
            r["top"] = UndoTopsJson(m_editorCtx->undoSystem);
            r["undoDepth"] = m_editorCtx->undoSystem.UndoDepth();
            r["redoDepth"] = m_editorCtx->undoSystem.RedoDepth();
            r["mode"] = (m_engineMode == EngineMode::Playing) ? "Playing" : "Editor";
            resp["ok"] = true;
            resp["result"] = std::move(r);
        });

    // ★ヘッドレス専用の検証口（人の編集を模す）。ギズモのドラッグと同じ TransformCommand を
    //   AI の印なしでスタックへ積む。「人の編集の後に AI の undo が人の編集を戻さない」を
    //   人のいない CI で確かめるためだけにある。人が使うエディタ（窓あり）には登録しない。
    if (m_headless)
    {
        McpDefine("debug_human_edit", "entity:int,name:string,position:any", DX12E_MCP_HANDLER
            {
                const auto e = ResolveMcpEntity(*m_scene, params);
                auto& reg = m_scene->GetRegistry();
                if (!reg.all_of<Transform>(e))
                    throw McpError(McpErr::NotFound, "entity has no Transform",
                                   "Transform を持つエンティティ（dx12_list_entities で見える物）を指定する");
                const auto p = params.value("position", std::vector<float>{});
                if (p.size() != 3)
                    throw McpError(McpErr::InvalidParam, "position must be [x,y,z]", "例: [1.0, 0.0, 2.5]");
                const Transform before = reg.get<Transform>(e);
                reg.get<Transform>(e).position = {p[0], p[1], p[2]};
                // 横取り（AI エントリ化）を通さずに直接積む＝人の操作として積まれる
                m_editorCtx->undoSystem.PushUncaptured(
                    std::make_unique<TransformCommand>(&reg, e, before, reg.get<Transform>(e)));
                resp["ok"] = true;
                resp["result"] = {{"entityId", static_cast<u32>(e)}, {"pushedAsHuman", true},
                                  {"top", UndoTopsJson(m_editorCtx->undoSystem)}};
            });
        // 人の Ctrl+Z を模す（メニュー / ショートカットと同じ pendingUndo を立てるだけ）。
        // AI のトランザクションが開いていれば確定扱いで閉じてから戻る、を CI で確かめる用。
        McpDefine("debug_human_undo", "", DX12E_MCP_HANDLER
            {
                m_editorCtx->pendingUndo = true;
                resp["ok"] = true;
                resp["result"] = {{"queued", true}, {"willUndo", m_editorCtx->undoSystem.PeekUndoName()
                                      ? std::string(m_editorCtx->undoSystem.PeekUndoName()) : std::string()}};
            });
    }
}

void Application::ProcessMcpUndoRequests(ID3D12GraphicsCommandList* cmdList)
{
    if (!m_editorCtx || !m_scene) return;
    auto& router = m_editorCtx->mcpUndo;
    auto& undo   = m_editorCtx->undoSystem;
    const double now = McpNowSec();

    // 放置されたトランザクションは確定扱いで閉じる（McpUndoAutoClose と同じ文面でログ）
    if (router.TxOpen() && now - router.TxLastActivity() >= kMcpTxIdleTimeoutSec)
        McpUndoAutoClose("idle_timeout");

    if (m_mcpUndoRequests.empty()) return;
    auto reqs = std::move(m_mcpUndoRequests);
    m_mcpUndoRequests.clear();

    // Undo（削除の取り消し）はモデルの再読み込みを伴うので、今フレームの cmdList を渡しておく
    // （pendingUndo の処理と同じ手順）。
    bool sceneBound = false;
    auto bindScene = [&]()
    {
        if (sceneBound) return;
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(), m_srvHeap.get(), cmdList);
        sceneBound = true;
    };
    bool touchedScene = false;

    for (auto& r : reqs)
    {
        using Kind = McpUndoRequest::Kind;
        const bool editor = (m_engineMode == EngineMode::Editor);
        switch (r.kind)
        {
        case Kind::Undo:
        case Kind::Redo:
        {
            const bool isUndo = (r.kind == Kind::Undo);
            if (!editor)
            {
                FailMcp(m_mcpBridge.get(), r.reply, McpErr::ModeConflict,
                        std::string("cannot ") + (isUndo ? "undo" : "redo") + " while Playing",
                        "dx12_stop してから呼ぶこと（★Stop で Undo 履歴は消える）");
                break;
            }
            bindScene();
            std::string name;
            const auto st = isUndo ? router.Undo(r.onlyAi, &name) : router.Redo(r.onlyAi, &name);
            if (st == McpUndoRouter::StepStatus::Ok)
            {
                touchedScene = true;
                json res{{isUndo ? "undone" : "redone", name},
                         {"wasAi", name.rfind("AI: ", 0) == 0},
                         {"onlyAi", r.onlyAi},
                         // 旧来の応答キー（互換）。今は実行した後に返すので will* = 実際に戻した物
                         {isUndo ? "queuedUndo" : "queuedRedo", true},
                         {isUndo ? "undoable" : "redoable", true},
                         {isUndo ? "willUndo" : "willRedo", name},
                         {"next", UndoTopsJson(undo)},
                         {"sceneGeneration", m_sceneGeneration}};
                CompleteMcp(m_mcpBridge.get(), r.reply, std::move(res));
            }
            else if (st == McpUndoRouter::StepStatus::Empty)
            {
                // 戻す物が無いのはエラーにしない（旧来どおり ok + undoable:false）
                CompleteMcp(m_mcpBridge.get(), r.reply,
                    json{{isUndo ? "undone" : "redone", nullptr},
                         {isUndo ? "queuedUndo" : "queuedRedo", true},
                         {isUndo ? "undoable" : "redoable", false},
                         {isUndo ? "willUndo" : "willRedo", ""},
                         {"onlyAi", r.onlyAi},
                         {"note", isUndo ? "Undo 履歴が空（シーンを開いた / Stop した直後は空になる）"
                                         : "やり直す物が無い（新しい編集が入ると redo は消える）"}});
            }
            else if (st == McpUndoRouter::StepStatus::TopIsHuman)
            {
                const char* top = isUndo ? undo.PeekUndoName() : undo.PeekRedoName();
                FailMcp(m_mcpBridge.get(), r.reply, McpErr::ModeConflict,
                    std::string("the top of the ") + (isUndo ? "undo" : "redo") + " stack is a human edit ('" +
                        (top ? top : "?") + "'); refused because onlyAi is true",
                    std::string("人の操作は戻さない。AI の操作はその下にあるので、人の操作を飛ばしては戻せない"
                                "（スタックは順番どおり）。本当に人の操作ごと戻すなら onlyAi:false を明示する。"
                                "自分の変更だけ取り消したいなら、同じ set_* を元の値で呼び直すのが安全"));
            }
            else   // TxOpen（受付後に開かれた）
            {
                FailMcp(m_mcpBridge.get(), r.reply, McpErr::ModeConflict,
                    "a transaction was opened before this request was processed",
                    "transaction_commit か transaction_rollback で閉じてから呼ぶこと");
            }
            break;
        }
        case Kind::Commit:
        {
            McpUndoRouter::TxResult res;
            if (router.Commit(now, &res) != McpUndoRouter::TxStatus::Ok)
            {
                FailMcp(m_mcpBridge.get(), r.reply, McpErr::ModeConflict,
                        "no open transaction (it was closed before commit was processed)", LastClosedHint(router));
                break;
            }
            json out{{"committed", true}, {"label", res.label}, {"calls", res.calls},
                     {"pushed", res.pushed}, {"entryName", res.pushed ? json(res.entryName) : json(nullptr)},
                     {"humanEditsDuringTransaction", res.humanPushes},
                     {"top", UndoTopsJson(undo)}};
            if (!res.pushed)
                out["note"] = "中身が空だったので何も積んでいない（読み取りだけ / 値が変わらない設定だけだった）";
            else if (res.humanPushes > 0)
                out["note"] = "開いている間に人の編集が挟まった。このエントリはその上に積まれたので、"
                              "undo 1 回でまずこのトランザクションが丸ごと戻る";
            CompleteMcp(m_mcpBridge.get(), r.reply, std::move(out));
            break;
        }
        case Kind::Rollback:
        {
            if (!editor)
            {
                FailMcp(m_mcpBridge.get(), r.reply, McpErr::ModeConflict, "cannot roll back while Playing",
                        "Stop すると Undo 履歴が消えるので rollback はもうできない。変更は Stop で Play 前へ戻る");
                break;
            }
            bindScene();
            McpUndoRouter::TxResult res;
            if (router.Rollback(now, &res) != McpUndoRouter::TxStatus::Ok)
            {
                FailMcp(m_mcpBridge.get(), r.reply, McpErr::ModeConflict,
                        "no open transaction (it was closed before rollback was processed)", LastClosedHint(router));
                break;
            }
            touchedScene = true;
            CompleteMcp(m_mcpBridge.get(), r.reply,
                json{{"rolledBack", true}, {"label", res.label}, {"calls", res.calls},
                     {"humanEditsDuringTransaction", res.humanPushes},
                     {"top", UndoTopsJson(undo)},
                     {"sceneGeneration", m_sceneGeneration},
                     {"note", res.humanPushes > 0
                         ? "begin 以降の AI の編集を逆順に戻した。開いている間の人の編集は戻していない"
                           "（同じ物を人も触っていた場合は、人の値が AI の begin 前の値で上書きされうる）"
                         : "begin 以降の AI の編集を逆順に戻した（スタックにも redo にも残さない）"}});
            break;
        }
        }
    }

    // 戻した結果いなくなったエンティティを選択から外す（削除の遅延処理と同じ後始末）
    if (touchedScene)
    {
        auto& reg = m_scene->GetRegistry();
        auto& sel = m_editorCtx->selectedEntities;
        sel.erase(std::remove_if(sel.begin(), sel.end(),
                  [&](entt::entity e) { return !reg.valid(e); }), sel.end());
        if (m_editorCtx->selectedEntity != entt::null && !reg.valid(m_editorCtx->selectedEntity))
            m_editorCtx->selectedEntity = sel.empty() ? entt::null : sel.back();
    }
}

} // namespace dx12e
