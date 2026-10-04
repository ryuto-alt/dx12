// ===========================================================================
// Application: MCP ディスパッチ表の土台 + スクショ / 読み戻し / 診断
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// method の足し方は本ファイル内 McpDefine の並びに倣う（作法は ApplicationInternal.h の DX12E_MCP_HANDLER 付近）。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/mcp/FleetGuard.h"   // ping / manifest 以外の method を「活動」に数える（--idle-exit）
#include "core/mcp/McpSafety.h"    // M5: guarded ゲート / 冪等ストア / dryRun プレビュー / ファイルジャーナル
#include "renderer/PhotometricMath.h"   // Q2: 露出(EV100)とトーンマップの純関数（screenshot の CPU ミラーが共有）
#include "renderer/LinearCapturePass.h"  // Q2: 線形 HDR スクリーンショットの読み出し
#include "renderer/pt/PtImageIO.h"      // Q2: PFM / EXR の書き出し（パストレーサーの出力と同じ規約・同じ実装を共有）

#include <algorithm>
#include <unordered_set>

namespace dx12e
{
using namespace appdetail;


namespace
{
// シーンのデータを変えない method（未保存フラグ / トランザクション確定待ちの受付判定に使う）。
// ★読み取り専用リストの方を持つ（新しい書き込み系メソッドが増えたときに黙って漏れる側にしない）。
bool IsMcpReadOnlyMethod(const std::string& method)
{
    static const std::unordered_set<std::string> kReadOnly = {
        "ping", "get_mode", "get_log", "get_entity", "get_hierarchy", "list_entities",
        "list_scenes", "list_assets", "list_lights", "query_entities", "find_entity",
        "get_bounds", "get_editor_camera", "get_scene_settings", "get_post_process",
        "get_ssao", "get_ssr", "get_ssgi", "get_taa", "get_contact_shadow",
        "get_normal_filter",
        "get_shadow_pcss", "get_volumetric_fog", "get_dxr", "get_physics_state",
        "audio_state",
        "brain_state",
        // ビルドは出力フォルダへ書くだけでシーンのデータは変えない（未保存扱いにしない）。
        "build_game", "get_build_status", "cancel_build",
        "get_anim_state", "get_lua_component_state", "get_script_errors",
        "get_play_session", "read_lua_component", "read_shader", "describe_components",
        "describe_lua_api", "describe_anim_graph", "describe_mcp_params", "describe_mcp_manifest",
        "asset_info", "perf_stats", "diagnose", "validate_scene", "raycast",
        "raycast_precise", "overlap_box", "overlap_sphere", "pick",
        "project_world_to_screen", "screenshot", "screenshot_final",
        "screenshot_game_view", "read_texture", "preview_model", "ui_tree", "perceive",
        "ui_screenshot", "terrain_sample", "terrain_splat_info", "net_status",
        // トランザクションの開閉と状態はシーンのデータを変えない（rollback と undo/redo は変える）。
        "transaction_status", "transaction_begin", "transaction_commit",
        // play/stop はシーンを汚さない（Stop がスナップショットへ戻す）。
        // undo/redo は状態を変えるので入れない（安全側に倒す）。
        // アセット操作: シーンを汚すのは move_asset が参照を実際に書き換えたときだけで、
        // その場合はハンドラ内で MarkEdited を呼んでいる。ここで一律に汚すと
        // 「参照していないアセットを整理しただけ」で未保存扱いになる。
        "move_asset", "delete_asset", "import_asset",
        "benchmark", "step_frames", "play", "stop", "save_scene", "select_entity",
        "focus_camera", "look_at", "set_editor_camera", "key_down",
        "key_up", "key_press", "mouse_move", "render_debug", "eval_lua",
        // reload_scripts は env を作り直すだけでシーンのデータは変えない。
        // reload_assets も同じ（MeshRenderer の参照先を新しい実体へ差し替えるだけで、
        // シリアライズされる値は 1 つも変わらない＝未保存扱いにしてはいけない）。
        "reload_scripts", "reload_assets",
        // 仮想入力（imgui_*）は UI を触るだけでシーンのデータは変えない（編集は UI 経由で Undo に積まれる）。
        "imgui_virtual_input", "imgui_pointer", "imgui_key", "imgui_find", "imgui_screenshot",
        // M7: エディタ操作。実際の編集は UI と同じ経路（Undo に積まれ、EditSeq が進む）で起きるので、ここで一律に汚さない。
        //     editor_command_run が window.* を開いただけで「未保存」にしない。
        "editor_command_list", "editor_command_run", "editor_command_run_guarded", "editor_state", "editor_notify", "editor_select", "editor_modal",
        // M5: 安全性の method はシーンのメモリを変えない。★journal_restore を入れないと、戻したシーンファイルを
        //     自動保存がメモリ上のシーンで上書きしてしまう（復元が無かったことになる）。
        "guard_token", "journal_list", "journal_restore", "cancel",
        // シーケンサー: 文書(.dxseq)・エディタのスクラブ・Play の再生はシーンの保存されるデータを変えない
        // （スクラブは PreAnimatedState が保存の直前に必ず戻す）。sequence_autoplay だけはシーンを変えるので入れない（ハンドラ内で MarkEdited）。
        "sequence_list", "sequence_load", "sequence_save", "sequence_get", "sequence_eval",
        "sequence_scrub", "sequence_play", "sequence_stop", "sequence_apply_op",
    };
    return kReadOnly.find(method) != kReadOnly.end();
}

// Undo スタックそのものを操作する method。呼び出し 1 回ぶんの横取り（AI エントリ化）はしない。
bool IsMcpUndoControlMethod(const std::string& method)
{
    return method == "undo" || method == "redo" || method.rfind("transaction_", 0) == 0;
}
} // namespace

// names は "a" または "a|b"（get/set を 1 本のハンドラで捌く場合。本文が method を見て分ける）。
void Application::McpDefine(const char* names, const char* paramSpec, McpHandler fn)
{
    const std::string all(names);
    size_t pos = 0;
    for (;;)
    {
        const size_t bar = all.find('|', pos);
        const std::string one = all.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
        if (!one.empty())
        {
            if (!m_mcpMethods.emplace(one, McpMethodEntry{paramSpec, fn}).second)
                Logger::Error("MCP: duplicate method name '{}' (dispatch table)", one);
        }
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
}

// meta を直接渡す版。params は全部 enforce（ディスパッチャの中央検査の対象）にする。
// paramSpec（describe_mcp_params 互換の申告表）は meta.params から作る。
// const char* が指す文字列は起動中ずっと生きている必要があるので、内容ごとに 1 度だけ確保して使い回す。
void Application::McpDefine(const char* names, McpMeta meta, McpHandler fn)
{
    static std::unordered_set<std::string> s_specs;   // ノード型なので要素のアドレスは動かない
    for (McpParam& p : meta.params) p.enforce = true;
    const char* spec = s_specs.insert(McpParamsToSpec(meta.params)).first->c_str();

    const std::string all(names);
    size_t pos = 0;
    for (;;)
    {
        const size_t bar = all.find('|', pos);
        const std::string one = all.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
        if (!one.empty())
        {
            McpMethodEntry entry{spec, fn};
            entry.meta    = meta;
            entry.hasMeta = true;
            if (!m_mcpMethods.emplace(one, std::move(entry)).second)
                Logger::Error("MCP: duplicate method name '{}' (dispatch table)", one);
        }
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
}

// 表は初回の MCP コマンドで 1 度だけ組む（起動時コストをエディタ操作に持ち込まない）。
void Application::EnsureMcpMethodTable()
{
    if (!m_mcpMethods.empty()) return;
    m_mcpMethods.reserve(192);
    RegisterMcpEntityMethods();
    RegisterMcpEditorMethods();
    RegisterMcpRenderMethods();
    RegisterMcpToolingMethods();
    RegisterMcpAssetMethods();
    RegisterMcpTerrainMethods();
    RegisterMcpLightingMethods();
    RegisterMcpNavMethods();
    RegisterMcpGitMethods();
    RegisterMcpValidateMethods();
    RegisterMcpPerceiveMethods();
    RegisterMcpUndoMethods();
    RegisterMcpAudioMethods();
    RegisterMcpAiMethods();
    RegisterMcpPathTracerMethods();
    RegisterMcpWaterMethods();      // 水面 W1: water_apply_preset
    RegisterMcpFoliageMethods();    // 植生 F1: foliage_scatter / paint / clear / save / stats / get_wind / set_wind
    RegisterMcpInstanceGroupMethods();   // インスタンス群: instance_group（convert / explode / info / add / remove / set）
    RegisterMcpSceneBackupMethods();     // シーンの世代つきバックアップ: scene_backups（list / restore / settings）
    RegisterMcpImGuiMethods();      // 仮想入力モード（AI が OS の入力を奪わずエディタ UI を操作）
    RegisterMcpSequenceMethods();   // シーケンサー(.dxseq): sequence_list / load / save / get / eval / scrub / play / stop / apply_op / autoplay
    RegisterMcpMatGraphMethods();   // マテリアルグラフ(G2b): material_graph_get / edit / validate / compile / status / graphize / set_param / apply / nodes
    RegisterMcpHardwareMethods();   // 物理ハードウェア: hw_list_ports / hw_status / hw_connect / hw_read / hw_write / hw_simulate / hw_calibrate / hw_flash ほか
    RegisterMcpEditorUiMethods();   // M7: editor_command_list / run / run_guarded / editor_state / editor_notify / editor_select
    RegisterMcpManifestMethods();   // describe_mcp_manifest（meta を直接渡す最初の method）
    ApplyMcpManifest();            // 全 method へ meta を流し込み、manifestHash を 1 度だけ計算する

    // M5: 遅延応答（フレーム境界で返る method）の完了を冪等ストアへ伝える観測点。
    // CompleteMcp / FailMcp を触らず、McpBridge::SendToClient に出ていく行を見る（結び付けは dispatcher が積む）。
    mcpsafety::SendObserver() = [](uint64_t client, const std::string& line)
    {
        auto& st = mcpsafety::Idempotency();
        if (st.PendingRequests() == 0) return;
        const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer()) return;
        st.ResolveRequest(client, j["id"].get<long long>(), j.value("ok", false), line);
    };
}

namespace
{
// McpError の構造化フィールドを応答へ載せる（付いているものだけ。旧来のエラー形は変えない）。
void FillMcpErrorFields(nlohmann::json& resp, const McpError& e)
{
    using json = nlohmann::json;
    resp["ok"] = false;
    resp["error"] = e.what();
    resp["error_code"] = e.code;
    // 「次の一手」と有効値。付いているときだけ載せる(旧来のエラー形は変えない)。
    if (!e.hint.empty())        resp["error_hint"]   = e.hint;
    if (!e.validValues.empty()) resp["error_values"] = e.validValues;
    // ---- 加算フィールド（旧クライアントは無視する）----
    if (!e.name.empty())        resp["error_name"]   = e.name;
    if (!e.cause.empty())       resp["error_cause"]  = e.cause;
    if (!e.fix.empty())
    {
        json arr = json::array();
        for (const McpFix& f : e.fix)
        {
            json args = json::parse(f.argsJson, nullptr, /*allow_exceptions=*/false);
            if (args.is_discarded()) args = json::object();
            arr.push_back({{"tool", f.tool}, {"args", std::move(args)}, {"why", f.why}});
        }
        resp["error_fix"] = std::move(arr);
    }
    if (!e.didYouMean.empty())  resp["error_did_you_mean"] = e.didYouMean;
    if (!e.details.is_null())   resp["error_details"]      = e.details;
}
} // namespace


std::string Application::HandleMcpCommand(uint64_t client, const std::string& line)
{
    using json = nlohmann::json;

    json req;
    try { req = json::parse(line); }
    catch (const std::exception& e)
    {
        return json{{"id", nullptr}, {"ok", false}, {"error_code", McpErr::InvalidParam},
                    {"error", std::string("parse error: ") + e.what()}}.dump();
    }

    json resp;
    resp["id"] = req.value("id", json(nullptr));
    const std::string method = req.value("method", std::string());
    const json params = req.value("params", json::object());
    fleet::Instance().Touch(method);

    // 遅延応答(create/spawn/delete/open_scene/play/stop)の相関情報。
    // 該当ハンドラで deferred=true にし、保留キューへ mcp を積んで空文字列を返す。
    // M5: idempotency_key の別名 idempotencyKey も同義（文字列以外は無視）。
    std::string idemKeyIn;
    for (const char* kn : {"idempotency_key", "idempotencyKey"})
    {
        const auto ik = params.find(kn);
        if (idemKeyIn.empty() && ik != params.end() && ik->is_string()) idemKeyIn = ik->get<std::string>();
    }
    McpDeferred deferred{ client, req.value("id", 0LL), idemKeyIn };
    deferred.method = method;   // 遅延系が Undo に「AI: <method>」を付けるため
    bool isDeferred = false;

    // ---- MCP の編集を Undo に積む（editor/McpUndoRouter.h / core/mcp/McpUndoTrack.h）----
    // 呼び出し 1 回ぶんを「AI: <method>」1 エントリへまとめる（トランザクション中はそちらへ）。
    // ★Editor の間だけ。Play 中の変更は Stop で丸ごと捨てられ、Undo 履歴も Stop で消えるので積まない。
    // ★ハンドラが投げても、途中まで変わった分は積む（変更が起きたのに戻せない、を作らない）。
    const bool undoRec = m_editorCtx && m_scene && m_scriptEngine && !m_isGameMode
                      && m_engineMode == EngineMode::Editor && !IsMcpUndoControlMethod(method);
    bool undoEnded = false;
    // M5: 冪等キーの状態（dispatch 後に Complete / Bind / Abort する）と dryRun / 再送の印。
    std::string idemKey;
    bool        isDryRun = false, idemReplayed = false;
    {
        // ファイルジャーナルの置き場はいまのプロジェクト。トランザクションが人の操作などで閉じていたら確定扱いで閉じる。
        auto& jr = mcpjournal::Instance();
        jr.SetBaseDir(std::filesystem::path(PathResolver::BaseDir()));
        if (jr.TxActive() && (!m_editorCtx || !m_editorCtx->mcpUndo.TxOpen())) jr.CommitTx();
    }
    auto endUndo = [&]() -> bool
    {
        if (!undoRec || undoEnded) return false;
        undoEnded = true;
        std::vector<std::unique_ptr<IUndoCommand>> snaps;
        try { snaps = m_mcpUndoTrack.Finish(); }
        catch (const std::exception& ex) { Logger::Warn("MCP undo: スナップショットの確定に失敗: {}", ex.what()); }
        for (auto& snap : snaps) m_editorCtx->mcpUndo.AddToCall(std::move(snap));
        return m_editorCtx->mcpUndo.EndCall(McpNowSec());
    };
    if (m_editorCtx) m_editorCtx->mcpUndo.Touch(McpNowSec());   // 放置タイマー（読み取りも活動に数える）

    try
    {
        if (!m_scene || !m_scriptEngine)
            throw std::runtime_error("engine not ready");

        // 生成/削除/シーン系を弾く判定。Playing 中はもちろん、同一 Poll バッチで先に play が
        // 積まれた(モード遷移保留)場合も弾く＝そのフレームで spawn ドレインが skip されて
        // 遅延応答が宙吊り(クライアント timeout)になるのを防ぐ。
        const bool busyPlaying = (m_engineMode == EngineMode::Playing) ||
                                 (m_modeChangeRequested && m_pendingMode == EngineMode::Playing);

        // ---- トランザクションの約束事（ハンドラより前に弾く）----
        if (m_editorCtx)
        {
            // Play / シーン切り替えは Undo 履歴を消す（Stop とシーン読み込みがスタックを空にする）。
            // 開いたまま進むと rollback できなくなるので、先に閉じさせる。
            static const std::unordered_set<std::string> kClearsHistory = {
                "play", "open_scene", "new_scene", "open_project"};
            if (m_editorCtx->mcpUndo.TxOpen() && kClearsHistory.count(method))
                throw McpError(McpErr::ModeConflict,
                    "transaction '" + m_editorCtx->mcpUndo.TxLabel() + "' is open; " + method +
                    " would clear the undo history",
                    "先に transaction_commit（変更を残す）か transaction_rollback（begin 前へ戻す）で"
                    "閉じてから " + method + " を呼ぶこと");
            // commit / rollback はフレーム境界で処理する。その応答が返る前に届いた書き込みは、
            // トランザクションの内外どちらに入るべきか決められないので断る。
            const bool closing = std::any_of(m_mcpUndoRequests.begin(), m_mcpUndoRequests.end(),
                [](const McpUndoRequest& r) {
                    return r.kind == McpUndoRequest::Kind::Commit || r.kind == McpUndoRequest::Kind::Rollback;
                });
            if (closing && !IsMcpReadOnlyMethod(method) && !IsMcpUndoControlMethod(method))
                throw McpError(McpErr::ModeConflict,
                    "transaction commit/rollback is still being processed",
                    "transaction_commit / transaction_rollback の応答を待ってから次の編集を送ること"
                    "（1 フレームで返る）");
        }

        // ★ディスパッチ（旧: 118 本の else-if 連鎖 = C1061 の温床。N37 / N43）。
        //   表引きなので method を何本足しても入れ子は深くならない。
        EnsureMcpMethodTable();
        const auto it = m_mcpMethods.find(method);
        if (it != m_mcpMethods.end())
        {
            // ---- 全 method 共通の任意キー expectGeneration（古いシーンへの書き込みを弾く）----
            // entityId を取ってからシーンが切り替わった（open_scene / new_scene / Stop）後の呼び出しを、
            // ハンドラが動く前に E_STALE_SCENE で断る。読み取り専用 method にも適用する（一貫させるため）。
            if (const auto eg = params.find("expectGeneration");
                eg != params.end() && eg->is_number_integer() && eg->get<long long>() != m_sceneGeneration)
            {
                McpError err(McpErr::StaleScene,
                    "stale scene: expectGeneration " + std::to_string(eg->get<long long>()) +
                    " but the current sceneGeneration is " + std::to_string(m_sceneGeneration),
                    "list_entities を引き直して entityId を取り直す（シーンの切り替え / Stop で id は振り直される）");
                err.name  = "E_STALE_SCENE";
                err.cause = "expectGeneration が現在の sceneGeneration と違う。シーンが入れ替わった後の古い id を使っている";
                err.fix.push_back(MakeMcpFix("list_entities", json::object(), "現在のシーンの entityId を取り直す"));
                err.details = {{"expectGeneration", eg->get<long long>()}, {"sceneGeneration", m_sceneGeneration}};
                throw err;
            }
            // ---- 中央検査: meta を直接渡した method だけ（enforce=true の引数の必須 / 型 / 列挙 / 範囲）----
            if (it->second.hasMeta) McpValidateMeta(it->second.meta, params, method);

            // ================= M5: 副作用の安全性（docs/MCP.md §13）=================
            //   1) dryRun:true → 実行せずプレビュー（Read は無視して通常実行）
            //   2) 冪等キー → 前回の結果を返す / 衝突 / 処理中
            //   3) guarded → 有効な confirm_token が無ければ拒否（TS 側の E_GUARDED に加えたエンジン側の最終関門）
            const McpMeta& mmeta = it->second.meta;
            const bool isReadMethod = mmeta.effect == McpEffect::Read;
            // ハンドラへ渡す params: 共通キー（confirm_token / dryRun / idempotencyKey）を除く。無ければ元をそのまま。
            const nlohmann::json* hp = &params;
            json stripped;
            if (params.contains("confirm_token") || params.contains("dryRun") || params.contains("idempotencyKey"))
            {
                stripped = params;
                stripped.erase("confirm_token"); stripped.erase("dryRun"); stripped.erase("idempotencyKey");
                hp = &stripped;
            }
            const auto dryIt = params.find("dryRun");
            const bool wantDryRun = dryIt != params.end() && dryIt->is_boolean() && dryIt->get<bool>();
            bool skipRun = false;
            if (wantDryRun && !isReadMethod)
            {
                const auto& tbl = mcpsafety::PreviewTable();
                const auto pit = tbl.find(method);
                if (pit == tbl.end())
                {
                    std::vector<std::string> supported;
                    for (const auto& kv : tbl) supported.push_back(kv.first);
                    McpError err(McpErr::Unsupported, method + ": dryRun is not supported",
                                 "dryRun のプレビューがあるのは error_values の method だけ。他は実行前に dx12_tool_describe で影響を確かめる",
                                 supported);
                    err.name  = "E_UNSUPPORTED";
                    err.cause = "この method は dryRun のプレビューを持たない";
                    err.details = {{"method", method}, {"effect", McpEffectName(mmeta.effect)}};
                    throw err;
                }
                json preview = json::parse(pit->second(hp->dump()), nullptr, /*allow_exceptions=*/false);
                if (preview.is_discarded()) preview = json::object();
                resp["ok"] = true;
                resp["result"] = {{"dryRun", true}, {"executed", false}, {"method", method},
                                  {"effect", McpEffectName(mmeta.effect)}, {"preview", std::move(preview)}};
                isDryRun = true;
                skipRun = true;
                undoEnded = true;   // 何も積んでいない
            }
            std::string idemNewKey;
            uint64_t    idemNewHash = 0;
            if (!skipRun && !isReadMethod && !deferred.idempotencyKey.empty()
                && method != "create_entity" && method != "spawn_model" && method != "spawn_prefab"   // 既存の m_mcpIdempotency 実装を維持
                && method != "guard_token" && method != "cancel")
            {
                json h = params;
                for (const char* k : {"confirm_token", "dryRun", "idempotency_key", "idempotencyKey"}) h.erase(k);
                const uint64_t hash = McpFnv1a64(h.dump(-1, ' ', false, json::error_handler_t::replace));
                const auto f = mcpsafety::Idempotency().Find(deferred.idempotencyKey, method, hash);
                using K = mcpsafety::IdempotencyStore::Kind;
                if (f.kind == K::Conflict)
                {
                    McpError err(McpErr::InvalidParam,
                        "idempotency_key '" + deferred.idempotencyKey + "' was already used by a different request (" + f.existingMethod + ")",
                        "同じキーで別の操作・別の引数は送れない。別の操作ならキーを変える(前回の結果が欲しいなら前回と同じ method・引数で送る)");
                    err.name  = "E_IDEMPOTENCY_CONFLICT";
                    err.cause = "同じ idempotency_key が別の method か別の引数で使われている";
                    err.details = {{"key", deferred.idempotencyKey}, {"firstMethod", f.existingMethod}, {"method", method}, {"firstAtMs", f.firstAtMs}};
                    throw err;
                }
                if (f.kind == K::InFlight)
                {
                    McpError err(McpErr::Busy, "idempotency_key '" + deferred.idempotencyKey + "' is still being processed",
                                 "同じキーの処理中。少し待ってから同じ内容で再送する(完了していれば前回の結果が返る)");
                    err.name  = "E_IDEMPOTENCY_IN_FLIGHT";
                    err.cause = "同じキーの前回の要求がまだ完了していない";
                    err.details = {{"key", deferred.idempotencyKey}, {"method", method}, {"firstAtMs", f.firstAtMs}};
                    throw err;
                }
                if (f.kind == K::Replay)
                {
                    json cached = json::parse(f.resp, nullptr, /*allow_exceptions=*/false);
                    if (cached.is_object())
                    {
                        const json myId = resp["id"];
                        resp = std::move(cached);
                        resp["id"] = myId;
                        if (!resp.contains("result") || !resp["result"].is_object())
                            resp["result"] = json{{"value", resp.contains("result") ? resp["result"] : json(nullptr)}};
                        resp["result"]["idempotentReplay"] = true;
                        resp["result"]["idempotency"] = {{"key", deferred.idempotencyKey}, {"firstAtMs", f.firstAtMs}};
                        idemReplayed = true;
                        skipRun = true;
                        undoEnded = true;
                    }
                }
                if (!skipRun)
                {
                    // 状態遷移の method は実行前にストアを空にする（自分の Done は完了後に積まれる）
                    if (method == "open_scene" || method == "new_scene" || method == "open_project" || method == "play" || method == "stop")
                        mcpsafety::Idempotency().Clear();
                    idemNewKey  = deferred.idempotencyKey;
                    idemNewHash = hash;
                }
            }
            if (!skipRun && mmeta.effect == McpEffect::Guarded)
            {
                using R = mcpsafety::GuardTokens::Result;
                const auto tk = params.find("confirm_token");
                const std::string token = (tk != params.end() && tk->is_string()) ? tk->get<std::string>() : std::string();
                const R r = token.empty() ? R::Unknown : mcpsafety::Guard().Consume(token, method);
                if (r != R::Ok)
                {
                    McpError err(McpErr::Guarded, method + " is a guarded method: a valid confirm_token is required",
                        "取り返しの付かない/外部へ出る操作。guard_token {method} で 1 回限りのトークンを得て confirm_token に入れる"
                        "(TS の dx12_call_guarded / confirm 経路は自動で行う)。実行せず影響だけ見るなら dryRun:true");
                    err.name  = "E_GUARDED";
                    err.cause = token.empty() ? "confirm_token が無い"
                              : r == R::Expired ? "confirm_token の有効期限(既定 60 秒)が切れている"
                              : r == R::WrongMethod ? "confirm_token は別の method 用(トークンは method 束縛)"
                              : "confirm_token が未知か使用済み(1 回限り)";
                    err.fix.push_back(MakeMcpFix("guard_token", json{{"method", method}},
                        "1 回限りの確認トークンを得る(TS の dx12_call_guarded / confirm 経路が自動で行う)"));
                    err.details = {{"method", method}, {"gate", "engine"}};
                    throw err;
                }
            }
            if (!skipRun && !idemNewKey.empty())
            {
                mcpsafety::Idempotency().BeginInFlight(idemNewKey, method, idemNewHash);
                idemKey = idemNewKey;
            }

            bool undoRecorded = false;
            if (!skipRun)
            {
                // ファイルジャーナル: journal 対応 method は 1 呼び出し = 1 エントリ（トランザクション中は tx のエントリへ入る）。
                struct JournalScope
                {
                    bool own = false;
                    ~JournalScope() { if (own) mcpjournal::Instance().EndCall(); }
                } jscope;
                if (mmeta.journal) jscope.own = mcpjournal::Instance().BeginCall(method);

                if (undoRec)
                {
                    m_editorCtx->mcpUndo.BeginCall(method);
                    m_mcpUndoTrack.Begin(m_scene.get(), PathResolver::AssetsDir());
                }
                it->second.fn(*hp, resp, method, deferred, isDeferred, busyPlaying);
                undoRecorded = endUndo();
            }

            // ★ハンドラが resp["result"] を作らず、resp へ直接キーを書く流儀のものを救う。
            //
            //   Node 側の engineClient は `msg.result ?? null` を返す契約なので、
            //   result を作らないハンドラは **MCP から見ると戻り値ゼロ**になる。
            //   これで navmesh 系 8 本（info / build / path / sample / raycast / clear /
            //   debug / settings）が長らく null を返していた＝AI はナビメッシュを焼けても
            //   ポリゴン数も経路も一切読めなかった（2026-09-10 に dx12_navmesh_info が
            //   {"result":null} を返すのを見て発覚）。
            //   ハンドラを 8 本書き換えるより、ここで 1 回畳む方が漏れない。
            if (!isDeferred && resp.value("ok", true) && !resp.contains("result"))
            {
                static const std::unordered_set<std::string> kReserved = {
                    "id", "ok", "error", "error_code", "error_hint", "error_values", "result",
                };
                json extra = json::object();
                for (auto it2 = resp.begin(); it2 != resp.end(); ++it2)
                    if (!kReserved.count(it2.key())) extra[it2.key()] = it2.value();
                if (!extra.empty())
                {
                    for (auto it2 = extra.begin(); it2 != extra.end(); ++it2) resp.erase(it2.key());
                    resp["result"] = std::move(extra);
                }
            }

            // 積んだら応答に名前を載せる（AI が「今のは戻せるか / 何という名前で積まれたか」を知る）。
            // ★上の「result を作らないハンドラを救う」より後に置く（先に result を作ると救済が走らない）。
            if (undoRecorded && !isDeferred && resp.value("ok", true))
            {
                if (!resp.contains("result") || resp["result"].is_null()) resp["result"] = json::object();
                if (resp["result"].is_object())
                {
                    resp["result"]["undoEntry"] = "AI: " + method;
                    if (m_editorCtx->mcpUndo.TxOpen())   // トランザクションの中身として積まれた
                        resp["result"]["undoTransaction"] = m_editorCtx->mcpUndo.TxLabel();
                }
            }

            // ★シーンに属する描画設定は Play 中に変えても Stop で捨てられる
            //   （Stop は Play 開始時のシーン JSON から丸ごと復元する）。
            //   これらのセッターは Playing でも素通しなので、応答は成功、get も新しい値、
            //   なのに Stop した瞬間に巻き戻る＝**呼んだ側からは検知しようがなかった**。
            //   エンティティ系ツールのように弾いてしまうと「動いている絵を見ながら詰める」
            //   という正当な使い方まで潰れるので、捨てられることを応答に書く。
            static const std::unordered_set<std::string> kSceneRenderSetters = {
                "set_post_process", "set_ssao", "set_ssr", "set_ssgi", "set_taa",
                "set_volumetric_fog", "set_shadow_pcss", "set_contact_shadow",
                "set_dxr", "set_sun", "apply_lighting_preset", "set_scene_settings",
            };
            if (busyPlaying && kSceneRenderSetters.count(method)
                && resp.contains("result") && resp["result"].is_object())
            {
                resp["result"]["discardedOnStop"] = true;
                resp["result"]["note"] =
                    "★Play 中の変更は dx12_stop で破棄される（Stop は Play 開始時のシーンを"
                    "丸ごと復元する）。残したいなら先に dx12_stop してから設定すること。";
            }
        }
        else
        {
            // ★error 文字列は従来のまま。error_code だけ 2(InvalidParam) → 8(UnknownMethod) に変えた。
            //   近い method 名（編集距離 + 別名 + 前方/部分一致）と、一覧の取り方を加算で返す。
            McpError err(McpErr::UnknownMethod, "unknown method: " + method);
            err.name  = "E_UNKNOWN_TOOL";
            err.cause = "この名前の method は無い。名前は dx12_ 接頭辞なし（例 set_transform）で、綴りは完全一致";
            err.didYouMean = SuggestMcpMethodNames(method);
            err.fix.push_back(MakeMcpFix("describe_mcp_manifest",
                nlohmann::json{{"brief", true}}, "全 method の名前・カテゴリ・要約を引いて正しい名前を探す"));
            FillMcpErrorFields(resp, err);
        }
        if (isDeferred) resp["ok"] = true;   // パネル表示用: dispatch 成功(本応答は遅延)
    }
    catch (const McpError& e)
    {
        endUndo();
        FillMcpErrorFields(resp, e);
        isDeferred = false;
    }
    catch (const std::exception& e)
    {
        endUndo();
        resp["ok"] = false;
        resp["error"] = e.what();
        resp["error_code"] = McpErr::InvalidParam;   // 大半は引数検証エラー
        isDeferred = false;
    }
    // M5: 冪等ストアへ確定。非遅延は成功なら Done・失敗なら InFlight を消す。遅延は応答が出るとき（SendToClient の観測点）に確定する。
    if (!idemKey.empty())
    {
        auto& st = mcpsafety::Idempotency();
        if (isDeferred)                 st.BindRequest(client, deferred.requestId, idemKey);
        else if (resp.value("ok", false)) st.Complete(idemKey, resp.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        else                            st.Abort(idemKey);
    }
    // パネル(MCP / AI Bridge)用にコマンド結果を記録（メインスレッドからのみ）。
    if (m_mcpBridge)
        m_mcpBridge->RecordCommand(method, resp.value("ok", false), resp.value("error", std::string()));

    // 未保存フラグ: MCP でシーンを変えた分も拾う。
    // MCP ハンドラは 100 個以上あり、Undo を積むのは group_entities だけなので、
    // ここ 1 箇所で名前から判定する。★読み取り専用リストの方を持つ（新しい書き込み系
    // メソッドが増えたときに黙って漏れる側にしない）。誤検出しても「保存しますか」と
    // 余計に聞くだけで済むが、取りこぼすと黙って作業が消える。
    if (m_editorCtx && resp.value("ok", false) && !method.empty() && !isDryRun && !idemReplayed)
    {
        // eval_lua と render_debug は「シーンを変えうる」が、変えないことの方が多い。
        // 変えた場合は設定フィンガープリント（Run ループの定期比較）か、
        // ハンドラ内で積まれる Undo の側で拾われる。
        if (!IsMcpReadOnlyMethod(method))
        {
            m_editorCtx->undoSystem.MarkEdited();
            // 最後の書き込みから kMcpAutoSaveDelay 秒アイドルしたらディスクへ本保存する
            // （UpdateMcpAutoSave）。1 コールごとに書くと数百体の配置で I/O が詰まるので、
            // 撃たれるたびにここで待ち時間を延ばし直す＝連続編集の間は書かない。
            // ★これが「未保存の警告を必ず出さない」の本体。フラグを黙らせているのではなく、
            //   未保存の状態そのものを残さないようにしている。
            m_editorCtx->mcpSaveCountdown = kMcpAutoSaveDelay;
        }
    }
    // 遅延応答は今は送らない(フレーム境界で SendToClient が送る)。Poll が空文字列をスキップ。
    if (isDeferred) return std::string();
    // 不正 UTF-8(例: CP932 のモデル名由来の NameTag)で dump() が例外を投げないよう置換。
    return resp.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// BGRA8(tightly packed, w*4 stride)を PNG ファイルへ。WIC(OS 標準)で書くので外部依存なし。

bool Application::ReadbackSceneBgra(std::vector<u8>& outBgra, u32& outW, u32& outH, std::string& err)
{
    using Microsoft::WRL::ComPtr;

    if (!m_sceneRT || !m_sceneRT->GetResource()) { err = "scene RT not ready"; return false; }
    if (!m_commandQueue || !m_frameResources)    { err = "gpu not ready";     return false; }

    auto* dev    = m_graphicsDevice->GetDevice();
    auto* srcTex = m_sceneRT->GetResource();
    const D3D12_RESOURCE_DESC texDesc = srcTex->GetDesc();
    const UINT w = static_cast<UINT>(texDesc.Width);
    const UINT h = texDesc.Height;
    if (w == 0 || h == 0) { err = "scene size is 0"; return false; }

    // readback バッファのレイアウト(行ピッチは 256B アライン)を取得
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT   rowCount   = 0;
    UINT64 rowSize    = 0;
    UINT64 totalBytes = 0;
    dev->GetCopyableFootprints(&texDesc, 0, 1, 0, &fp, &rowCount, &rowSize, &totalBytes);

    ComPtr<ID3D12Resource> readback;
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = totalBytes;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        { err = "readback alloc failed"; return false; }
    }

    // 前フレームの GPU 完了を待ってから sceneRT(単一リソース＝内容確定)をコピー
    m_commandQueue->WaitIdle();
    auto* cmd = m_frameResources->BeginFrame(*m_commandQueue);

    const D3D12_RESOURCE_STATES prev = m_sceneRT->GetState();
    auto barrier = [&](D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER br{};
        br.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource   = srcTex;
        br.Transition.StateBefore = a;
        br.Transition.StateAfter  = b;
        br.Transition.Subresource = 0;
        cmd->ResourceBarrier(1, &br);
    };
    const bool needBarrier = (prev != D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (needBarrier) barrier(prev, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = srcTex;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource       = readback.Get();
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    if (needBarrier) barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, prev);  // 元の状態へ戻す(エンジンの追跡と一致)

    if (FAILED(cmd->Close())) { m_frameResources->EndFrame(*m_commandQueue); err = "cmd close failed"; return false; }
    m_commandQueue->ExecuteCommandList(cmd);
    m_commandQueue->WaitIdle();
    m_frameResources->EndFrame(*m_commandQueue);

    // R16G16B16A16_FLOAT(リニアHDR) → 表示変換 → BGRA8。
    //
    // ここは PostProcess.hlsl の ToneMapGamma と *同じ分岐* を辿らせること。
    // 以前は ACES 決め打ちだったので、シーンのトーンマップを AgX / なし にしていると
    // スクリーンショットだけ別物の絵になり、色やコントラストの判断を誤らせていた
    // (2D ゲームは「なし(ガンマのみ)」を使うので特に事故りやすい)。
    // 露出も同様に効かせる。それ以外のグレーディングは掛けない(掛けたければ PostProcess を CPU で
    // 丸ごと再実装することになる)ので、色を厳密に見るときはビューポートを信じること。
    void* mapped = nullptr;
    D3D12_RANGE rr{ 0, static_cast<SIZE_T>(totalBytes) };
    if (FAILED(readback->Map(0, &rr, &mapped))) { err = "readback map failed"; return false; }

    static const PostProcessSettings kDefaultPost{};
    const PostProcessSettings& pp = m_scene ? m_scene->GetPostSettings() : kDefaultPost;
    const int   tonemapper = (pp.tonemapper >= 0 && pp.tonemapper < photo::kTmCount) ? pp.tonemapper : 0;
    // Q2: 露出モード 1（手動 EV100）は係数を掛ける。2（自動）は GPU の適応値なので CPU からは分からない（掛けない）。
    const float exposure   = (pp.exposureMode == 1)
        ? ((pp.enabled && pp.exposureOn ? pp.exposure : 1.0f) * photo::ManualExposureScale(pp.ev100, pp.evComp))
        : (pp.exposureOn ? pp.exposure : 1.0f);
    const photo::FilmParams film{pp.filmSlope, pp.filmToe, pp.filmShoulder, pp.filmBlackClip, pp.filmWhiteClip};

    auto sat = [](float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };

    // 以下 3 つは PostProcess.hlsl の ACESFilm / AgXContrast / TonemapAgX / ToneMapGamma の写し。
    // 片方を直したらもう片方も直すこと（ずれるとスクショだけ違う絵になる）。
    auto aces1 = [&](float x)
    {
        const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
        return sat((x * (a * x + b)) / (x * (c * x + d) + e));
    };
    auto agxContrast = [](float x)
    {
        const float x2 = x * x, x4 = x2 * x2;
        return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4
             - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
    };
    auto toneMapGamma = [&](float rgb[3])
    {
        if (photo::IsNewToneMapper(tonemapper))   // Q2: UE Filmic / 線形 / PBR Neutral（PhotometricMath.h。GPU と同じ式）
        {
            const photo::Rgb o = photo::ToneMapNewDisplay(tonemapper, {rgb[0], rgb[1], rgb[2]}, film);
            rgb[0] = o.r; rgb[1] = o.g; rgb[2] = o.b;
            return;
        }
        if (tonemapper == 2)   // トーンマップなし（ガンマのみ）
        {
            for (int i = 0; i < 3; ++i) rgb[i] = std::pow((std::max)(rgb[i], 0.0f), 1.0f / 2.2f);
            return;
        }
        if (tonemapper == 0)   // ACES
        {
            for (int i = 0; i < 3; ++i) rgb[i] = std::pow(aces1(rgb[i]), 1.0f / 2.2f);
            return;
        }
        // AgX（行列 → log2 → コントラスト曲線 → 逆行列。出力は既にガンマ空間）
        static const float kAgx[9] = {
            0.842479062253094f, 0.0784335999999992f, 0.0792237451477643f,
            0.0423282422610123f, 0.878468636469772f, 0.0791661274605434f,
            0.0423756549057051f, 0.0784336f,         0.879142973793104f };
        static const float kAgxInv[9] = {
             1.19687900512017f,  -0.0980208811401368f, -0.0990297440797205f,
            -0.0528968517574562f, 1.15190312990417f,   -0.0989611768448433f,
            -0.0529716355144438f,-0.0980434501171241f,  1.15107367264116f };
        const float minEv = -12.47393f, maxEv = 4.026069f;

        auto mul3 = [](const float m[9], float v[3])
        {
            const float a = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
            const float b = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
            const float c = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
            v[0] = a; v[1] = b; v[2] = c;
        };
        for (int i = 0; i < 3; ++i) rgb[i] = (std::max)(rgb[i], 0.0f);
        mul3(kAgx, rgb);
        for (int i = 0; i < 3; ++i)
        {
            const float l = std::log2((std::max)(rgb[i], 1e-10f));
            rgb[i] = ((l < minEv ? minEv : (l > maxEv ? maxEv : l)) - minEv) / (maxEv - minEv);
            rgb[i] = agxContrast(rgb[i]);
        }
        mul3(kAgxInv, rgb);
        for (int i = 0; i < 3; ++i) rgb[i] = sat(rgb[i]);
    };

    auto toByte = [&](float c)
    {
        const int v = static_cast<int>(sat(c) * 255.0f + 0.5f);
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };

    outBgra.assign(static_cast<size_t>(w) * h * 4, 0);
    const auto* base = static_cast<const uint8_t*>(mapped);
    for (UINT y = 0; y < h; ++y)
    {
        const auto* in  = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(fp.Footprint.RowPitch) * y);
        uint8_t*    out = &outBgra[static_cast<size_t>(w) * 4 * y];
        for (UINT x = 0; x < w; ++x)
        {
            // ★render debug 中はトーンマップも露出も掛けない（RenderDebug.hlsl が
            //   「表示したい色」をそのまま書いているので、加工すると偽色の意味が壊れる）。
            const float ex = m_renderDebugRawReadback ? 1.0f : exposure;
            float rgb[3] = {
                DirectX::PackedVector::XMConvertHalfToFloat(in[x * 4 + 0]) * ex,
                DirectX::PackedVector::XMConvertHalfToFloat(in[x * 4 + 1]) * ex,
                DirectX::PackedVector::XMConvertHalfToFloat(in[x * 4 + 2]) * ex };
            if (!m_renderDebugRawReadback) toneMapGamma(rgb);
            out[x * 4 + 0] = toByte(rgb[2]);   // BGRA 順
            out[x * 4 + 1] = toByte(rgb[1]);
            out[x * 4 + 2] = toByte(rgb[0]);
            out[x * 4 + 3] = 255;
        }
    }
    D3D12_RANGE wr{ 0, 0 };
    readback->Unmap(0, &wr);

    outW = w;
    outH = h;
    return true;
}

// MCP のスクショ系が共有する出力先の解決。
//   省略 → CWD の defName（＝従来の挙動。dx12_engine.log と同じ場所へ上書き）
//   相対 → CWD 基準 / 絶対 → そのまま。拡張子が .png でなければ足す。
// ".." は弾く（ブリッジは localhost 専用だが、AI が誤って上位ディレクトリへ書くのを防ぐ）。
static std::filesystem::path McpScreenshotPath(const std::string& rel, const char* defName)
{
    namespace fs = std::filesystem;
    if (rel.empty()) return fs::absolute(defName);
    fs::path p(rel);
    for (const auto& part : p)
        if (part == "..")
            throw McpError(McpErr::InvalidParam, "path must not contain '..'",
                           "CWD からの相対パスか絶対パスで指定する");
    if (p.extension() != ".png") p += ".png";
    p = fs::absolute(p);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    return p;
}

std::string Application::CaptureSceneScreenshot(std::string& err, const std::string& outPathRel)
{
    namespace fs = std::filesystem;

    std::vector<u8> bgra;
    u32 w = 0, h = 0;
    if (!ReadbackSceneBgra(bgra, w, h, err)) return {};

    const fs::path outPath = McpScreenshotPath(outPathRel, "mcp_screenshot.png");
    if (!WriteBgraPng(outPath.wstring(), bgra.data(), w, h, err)) return {};
    return outPath.string();
}

// ---------------------------------------------------------------------------
// screenshot_final — バックバッファ（ポスト適用後の最終画）の読み戻し（§6 B5 の根治）
// ---------------------------------------------------------------------------
// ★なぜ別経路が要るのか
//   ReadbackSceneBgra が読む m_sceneRT は **ポストプロセスの入力** なので、
//   グレーディング / ブルーム / ゴッドレイ / ビネット / LUT / FXAA / デバンド、
//   そして TAA の解決結果（履歴 RT 側に出る）が 1 つも写らない。
//   MCP の測定と目視が食い違う根本原因がこれ。バックバッファを読めば全部解決する。
//
// ★撮る位置
//   Render() の中、**ImGui フレームを始める前**。この時点のバックバッファには
//   「ポスト適用後のシーン + エディタアイコン + ゲーム内 UI 画像」が入っており、
//   ImGui のパネル / ギズモ / オーバーレイはまだ 1 ピクセルも乗っていない。
//   ＝エディタで撮ってもゲームと同じ絵になる。パネル込みが欲しいときは ui_screenshot。
void Application::CaptureFinalBackBufferRegion(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backBuffer,
                                               u32 vpX, u32 vpY, u32 vpW, u32 vpH, bool afterImGui)
{
    if (!m_mcpFinalShot.pending || !cmd || !backBuffer) return;
    // ImGui の前（従来）と後（dx12_imgui_screenshot）のどちらで撮る要求かで、呼び出し位置を分ける。
    if (m_mcpFinalShot.withImGui != afterImGui) return;
    m_mcpFinalShot.pending  = false;
    m_mcpFinalShot.captured = false;

    // ★どの早期 return でも必ずここを通す。通さないと遅延応答が宙吊りになり、
    //   決定論モードの「時間を固定したまま」状態がエディタに残り続ける（＝時間が止まって見える）。
    auto bail = [&](const char* why)
    {
        FailMcp(m_mcpBridge.get(), m_mcpFinalShot.reply, McpErr::Internal, why);
        m_mcpFinalShot = {};
        m_deterministicCapture = false;
    };

    auto* dev = m_graphicsDevice ? m_graphicsDevice->GetDevice() : nullptr;
    if (!dev) { bail("graphics device not ready"); return; }

    const D3D12_RESOURCE_DESC bbDesc = backBuffer->GetDesc();
    const u32 fullW = static_cast<u32>(bbDesc.Width);
    const u32 fullH = bbDesc.Height;
    // ビューポートがバックバッファをはみ出していたらクランプ（リサイズ直後の 1 フレームで起きる）。
    if (vpW == 0 || vpH == 0 || fullW == 0 || fullH == 0 || vpX >= fullW || vpY >= fullH)
    { bail("viewport rect is empty (window minimized or resizing?)"); return; }
    vpW = (std::min)(vpW, fullW - vpX);
    vpH = (std::min)(vpH, fullH - vpY);

    // コピー先のレイアウトは「切り出す矩形と同じサイズのテクスチャ」で計算する。
    D3D12_RESOURCE_DESC regionDesc = bbDesc;
    regionDesc.Width  = vpW;
    regionDesc.Height = vpH;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT   rowCount = 0;
    UINT64 rowSize = 0, totalBytes = 0;
    dev->GetCopyableFootprints(&regionDesc, 0, 1, 0, &fp, &rowCount, &rowSize, &totalBytes);

    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = totalBytes;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.Format           = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    { bail("readback alloc failed"); return; }

    // バックバッファはこの時点で RENDER_TARGET（呼び出し側の契約）。COPY_SOURCE へ往復させる。
    auto barrier = [&](D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER br{};
        br.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource   = backBuffer;
        br.Transition.StateBefore = a;
        br.Transition.StateAfter  = b;
        br.Transition.Subresource = 0;
        cmd->ResourceBarrier(1, &br);
    };
    barrier(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = backBuffer;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource       = readback.Get();
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    const D3D12_BOX box{ vpX, vpY, 0, vpX + vpW, vpY + vpH, 1 };
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    m_mcpFinalShot.readback = readback;
    m_mcpFinalShot.w        = vpW;
    m_mcpFinalShot.h        = vpH;
    m_mcpFinalShot.rowPitch = fp.Footprint.RowPitch;
    m_mcpFinalShot.bytes    = totalBytes;
    m_mcpFinalShot.format   = static_cast<u32>(bbDesc.Format);
    m_mcpFinalShot.captured = true;
}

void Application::FinishFinalScreenshot()
{
    if (!m_mcpFinalShot.captured) return;
    m_mcpFinalShot.captured = false;
    const bool wasDeterministic = m_mcpFinalShot.deterministic;
    const bool hidGizmos        = m_mcpFinalShot.hideGizmos;
    m_deterministicCapture = false;   // 撮り終わったら必ず通常の時間へ戻す

    McpDeferred reply = m_mcpFinalShot.reply;
    m_mcpFinalShot.reply = {};
    auto readback = m_mcpFinalShot.readback;
    m_mcpFinalShot.readback.Reset();
    if (!readback || reply.client == 0) return;

    // コピーは Present と同じコマンドリストに積んである。読む前に GPU の完了を待つ。
    m_commandQueue->WaitIdle();

    const u32 w = m_mcpFinalShot.w, h = m_mcpFinalShot.h, pitch = m_mcpFinalShot.rowPitch;
    // バックバッファは既に表示色（トーンマップ + ガンマ済み）。並びだけ BGRA へ揃える。
    const bool bgraSrc = (m_mcpFinalShot.format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                          m_mcpFinalShot.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

    void* mapped = nullptr;
    // ★read range は必ず GetCopyableFootprints の totalBytes。pitch*h を渡すと最終行の
    //   パディング分だけリソースを超えて E_INVALIDARG になる（実際に踏んだ）。
    D3D12_RANGE rr{ 0, static_cast<SIZE_T>(m_mcpFinalShot.bytes) };
    if (FAILED(readback->Map(0, &rr, &mapped)))
    {
        FailMcp(m_mcpBridge.get(), reply, McpErr::Internal, "readback map failed");
        return;
    }
    std::vector<u8> bgra(static_cast<size_t>(w) * h * 4);
    const auto* base = static_cast<const uint8_t*>(mapped);
    for (u32 y = 0; y < h; ++y)
    {
        const uint8_t* in  = base + static_cast<size_t>(pitch) * y;
        uint8_t*       out = &bgra[static_cast<size_t>(w) * 4 * y];
        for (u32 x = 0; x < w; ++x)
        {
            const uint8_t c0 = in[x * 4 + 0], c1 = in[x * 4 + 1], c2 = in[x * 4 + 2];
            out[x * 4 + 0] = bgraSrc ? c0 : c2;   // B
            out[x * 4 + 1] = c1;                  // G
            out[x * 4 + 2] = bgraSrc ? c2 : c0;   // R
            out[x * 4 + 3] = 255;
        }
    }
    D3D12_RANGE wr{ 0, 0 };
    readback->Unmap(0, &wr);

    std::string err;
    std::filesystem::path outPath;
    // ★Q2: path の拡張子は .png / .pfm / .exr のどれでも受け、形式ごとに同じ基準名で書く（例: a/b.png + pfm → a/b.pfm）。
    std::string pathArg = m_mcpFinalShot.path;
    {
        const std::filesystem::path pp(pathArg);
        std::string ext = pp.extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".pfm" || ext == ".exr") pathArg = (pp.parent_path() / pp.stem()).string();
    }
    try { outPath = McpScreenshotPath(pathArg,
        m_mcpFinalShot.withImGui ? "mcp_imgui_screenshot.png" : "mcp_screenshot_final.png"); }
    catch (const std::exception& e)
    {
        FailMcp(m_mcpBridge.get(), reply, McpErr::InvalidParam, e.what());
        return;
    }
    const bool q2Png = m_mcpFinalShot.withImGui || m_mcpFinalShot.wantPng;
    if (q2Png && !WriteBgraPng(outPath.wstring(), bgra.data(), w, h, err))
    {
        FailMcp(m_mcpBridge.get(), reply, McpErr::Internal, err.empty() ? "png write failed" : err);
        return;
    }
    // ---- Q2: 線形 HDR（トーンマップ前・露出前の float）----
    nlohmann::json q2Files = nlohmann::json::object();
    nlohmann::json q2Linear = nullptr;
    std::string primaryPath = outPath.string();
    if (q2Png) q2Files["png"] = outPath.string();
    if (!m_mcpFinalShot.withImGui && m_mcpFinalShot.WantLinear())
    {
        std::vector<float> lin;
        u32 lw = 0, lh = 0;
        std::string lerr = m_mcpFinalShot.linearErr;
        if (!m_mcpFinalShot.linearRecorded || !m_linearCapture || !m_linearCapture->Fetch(lin, lw, lh, lerr))
        {
            FailMcp(m_mcpBridge.get(), reply, McpErr::Internal,
                    "linear HDR capture failed: " + (lerr.empty() ? std::string("not recorded (the main view's post chain did not run)") : lerr));
            return;
        }
        std::filesystem::path linBase = outPath; linBase.replace_extension("");
        auto writeLin = [&](const char* ext, bool exr) -> bool
        {
            const std::filesystem::path p = linBase.string() + ext;
            const bool ok = exr ? pt::io::WriteExr(p, lw, lh, lin.data()) : pt::io::WritePfm(p, lw, lh, lin.data());
            if (ok) q2Files[exr ? "exr" : "pfm"] = p.string();
            return ok;
        };
        if ((m_mcpFinalShot.wantPfm && !writeLin(".pfm", false)) || (m_mcpFinalShot.wantExr && !writeLin(".exr", true)))
        {
            FailMcp(m_mcpBridge.get(), reply, McpErr::Internal, "linear HDR write failed (disk full or path not writable)");
            return;
        }
        if (!q2Png) primaryPath = q2Files.contains("pfm") ? q2Files["pfm"].get<std::string>() : q2Files["exr"].get<std::string>();
        // 値の意味（比較ツール tools/parity が読む規約）: シーン参照の線形 Rec.709 RGB / トーンマップ前 / 露出前
        const PostProcessSettings& lpp = m_scene->GetPostSettings();
        q2Linear = {{"width", lw}, {"height", lh},
                    {"space", "scene-linear Rec.709 RGB"}, {"tonemapped", false}, {"exposureApplied", false},
                    {"units", lpp.lightingUnits == 1 ? "nit (1.0 = 1 cd/m^2)" : "engine units (legacy lighting)"},
                    {"rowOrder", "top-to-bottom in memory; PFM is written bottom-up as the format requires"},
                    {"sameAsPathTracer", "same PFM/EXR writer as render_reference"}};
    }
    if (m_mcpFinalShot.withImGui)
    {
        // dx12_imgui_screenshot: ImGui（パネル / ギズモ / 仮想カーソル）込みの最終画。
        CompleteMcp(m_mcpBridge.get(), reply,
            nlohmann::json{{"path", outPath.string()},
                           {"width", w}, {"height", h},
                           {"source", "backbuffer+imgui"},
                           {"virtualCursor", vinput::Enabled() && vinput::Global().State().hasPos},
                           {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"},
                           {"note", "ImGui を描いた後・Present 直前のバックバッファ全面。窓が背面/画面外/最小化"
                                    "でも撮れる（PrintWindow を使わない）。仮想入力モード ON なら仮想カーソル"
                                    "（矢印 + クリックの波紋）も写る。座標はこの画像のピクセル = "
                                    "dx12_imgui_pointer のクライアント座標"}});
        return;
    }
    const PostProcessSettings& pp = m_scene->GetPostSettings();
    CompleteMcp(m_mcpBridge.get(), reply,
        nlohmann::json{{"path", primaryPath},
                       {"files", q2Files}, {"linear", q2Linear},
                       {"offscreen", m_mcpFinalShot.offW > 0},
                       {"width", w}, {"height", h},
                       {"source", m_mcpFinalShot.offW > 0 ? "offscreen" : "backbuffer"},
                       {"postApplied", pp.enabled},
                       {"deterministic", wasDeterministic},
                       {"gizmos", !hidGizmos},
                       {"taa", m_scene->GetTaaSettings().enabled},
                       {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"},
                       {"note", "ポスト適用後のバックバッファ。ImGui を描く前に撮るので"
                                "エディタのパネル/ギズモは写らない。dx12_screenshot（ポスト前の "
                                "m_sceneRT）とは別物なので、見た目の判断はこちらを使う"}});
}

Application::DiagRenderInfo Application::GetDiagRenderInfo() const
{
    DiagRenderInfo info;
    info.backBufferFormat = m_swapChain ? static_cast<u32>(m_swapChain->GetFormat()) : 0u;
    info.sceneColorFormat = m_sceneRT   ? static_cast<u32>(m_sceneRT->GetFormat())   : 0u;
    info.depthFormat      = static_cast<u32>(DXGI_FORMAT_D32_FLOAT);
    if (m_scene)
    {
        const PostProcessSettings& pp = m_scene->GetPostSettings();
        info.tonemapper  = pp.tonemapper;
        info.postEnabled = pp.enabled;
        info.exposureOn  = pp.exposureOn;
        info.exposure    = pp.exposure;
    }
    return info;
}

Application::DiagRenderHealth Application::GetDiagRenderHealth() const
{
    DiagRenderHealth h;
    h.renderDebugMode = m_renderDebugMode;
    h.renderDebugName = m_renderDebugModeName;
    if (m_srvHeap)
    {
        h.srvHeapCapacity = m_srvHeap->GetCapacity();
        h.srvHeapFree     = m_srvHeap->GetFreeCount();
    }
    h.renderW = m_renderW;
    h.renderH = m_renderH;
    u32 vx = 0, vy = 0;
    GetDisplayViewport(vx, vy, h.viewportW, h.viewportH);
    h.atLauncher       = m_showLauncher;
    h.cameraOverridden = m_mcpCameraOverride;
    if (m_camera)
    {
        const DirectX::XMFLOAT3 p = m_camera->GetPosition();
        h.cameraFinite = std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
        if (h.cameraFinite) h.cameraDistance = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    }
    return h;
}

Application::DiagOcclusionInfo Application::GetDiagOcclusionInfo() const
{
    DiagOcclusionInfo o;
    o.enabled = m_occlusionCulling;
    o.ready   = m_hiZPass && m_hiZPass->IsReady() && m_occlusionCull && m_occlusionCull->IsReady();
    o.active  = m_diagOcclusionActive;
    o.prepassNeededAnyway = m_diagPrepassWithoutHiZ;
    if (m_hiZPass && m_hiZPass->IsReady())
    {
        o.pyramidW    = m_hiZPass->GetWidth();
        o.pyramidH    = m_hiZPass->GetHeight();
        o.pyramidMips = m_hiZPass->GetMipCount();
    }
    if (m_occlusionCull)
    {
        o.tested   = m_occlusionCull->GetTested();
        o.occluded = m_occlusionCull->GetOccluded();
    }
    o.batches = static_cast<u32>(m_drawBatches.size());
    o.predicatedDraws = m_statPredicated;
    o.drawItems       = static_cast<u32>(m_drawItems.size());
    return o;
}

Application::DiagDxrInfo Application::GetDiagDxrInfo() const
{
    DiagDxrInfo d;
    d.supported = m_dxrEnabled;
    if (m_graphicsDevice)
    {
        d.tier        = static_cast<int>(m_graphicsDevice->GetRaytracingTier());
        d.shaderModel = static_cast<int>(m_graphicsDevice->GetHighestShaderModel());
        d.inlineRt    = m_graphicsDevice->SupportsInlineRaytracing();
    }
    if (m_scene)
    {
        const RtSettings& r = m_scene->GetRtSettings();
        d.shadowEnabled = r.shadowEnabled;
        d.aoEnabled     = r.aoEnabled;
    }
    d.shadowActive = m_rtShadowActiveThisFrame;
    if (m_rtScene)
    {
        const auto& s = m_rtScene->GetStats();
        d.tlasReady          = m_rtScene->IsReady();
        d.instances          = s.instances;
        d.blasCount          = s.blasCount;
        d.skippedSkinned     = s.skippedSkinned;
        d.skippedTransparent = s.skippedTransparent;
        d.skippedAlphaTest   = s.skippedAlphaTest;
        d.droppedOverLimit   = s.droppedOverLimit;
        d.skinnedInstances   = s.skinnedInstances;
        d.skinnedRebuilds    = s.skinnedRebuilds;
        d.skinnedStale       = s.skinnedStale;
        d.skinnedTriangles   = s.skinnedTriangles;
        d.blasBytes          = s.blasBytes + s.skinnedBlasBytes;
        d.blasTriangles      = s.blasTriangles;
        d.tlasBytes          = s.tlasBytes;
        d.scratchBytes       = s.scratchBytes;
        d.instanceDescBytes  = s.instanceDescBytes;
    }
    return d;
}

Application::DiagFrameStats Application::TakeDiagnosticFrameStats()
{
    const DiagFrameStats out = m_diagFrameStats;
    m_diagFrameStats = {};
    return out;
}



} // namespace dx12e
