// ===========================================================================
// MCP の副作用の安全性（M5）の method とプレビュー表。
// ---------------------------------------------------------------------------
// ★このファイルは ApplicationMcpManifest.cpp の Application::RegisterMcpManifestMethods() の【本体の中】に
//   #include される（Application.h を触らずに Application のメンバ / this を使うため。データ表 .inc と同じ流儀）。
//   json / McpDefine / P() はその関数の文脈のものをそのまま使う。
//
//   guard_token     … guarded な method を実行するための 1 回限りの確認トークンを発行する
//   journal_list    … ファイル書き込みジャーナルの一覧（.dx12/journal/）
//   journal_restore … ジャーナルのエントリを書き戻す
//   cancel          … 実行中の benchmark / step_frames を切り詰めて、次のフレームで自然に完了させる（M6）
//   プレビュー表    … dryRun:true で「実際に何が起こるか」を実行せず返す（McpPreviewMethods() の全 method）
// 仕様は docs/MCP.md §13。
// ===========================================================================
{
    namespace fs = std::filesystem;
    using mcpsafety::Guard;

    // ---------------------------------------------------------------- guard_token
    {
        McpMeta m;
        m.summary   = "guarded な method(git_push / eval_lua / delete_asset / build_game など)を実行するための 1 回限りの確認トークン(60 秒有効・method 束縛)を発行する";
        m.keywords  = "guard token confirm guarded 確認 トークン 危険操作 ゲート";
        m.category  = "safety";
        m.effect    = McpEffect::Runtime;
        m.timeoutMs = 5000;
        m.params    = {P("method", "string", true, nullptr, nullptr, nullptr, nullptr, "トークンを使う guarded な method 名(例 eval_lua)")};
        m.next      = {{"eval_lua", "confirm_token に入れて実行する"}};
        m.examples  = {{"{\"method\":\"eval_lua\"}", "eval_lua 用のトークンを得る"}};
        McpDefine("guard_token", m, DX12E_MCP_HANDLER
            {
                const std::string target = params.value("method", std::string());
                std::vector<std::string> guarded;
                for (const auto& kv : m_mcpMethods)
                    if (kv.second.meta.effect == McpEffect::Guarded) guarded.push_back(kv.first);
                std::sort(guarded.begin(), guarded.end());
                const auto it = m_mcpMethods.find(target);
                if (it == m_mcpMethods.end() || it->second.meta.effect != McpEffect::Guarded)
                {
                    McpError err(McpErr::InvalidParam, "guard_token: '" + target + "' is not a guarded method",
                                 "guarded な method 名を渡す(error_values 参照)", guarded);
                    err.name       = "E_INVALID_PARAM";
                    err.cause      = it == m_mcpMethods.end() ? "この名前の method は無い" : "この method は guarded ではない(トークンは要らない)";
                    err.didYouMean = McpSuggest(target, guarded, 3);
                    throw err;
                }
                const std::string tok = Guard().Issue(target);
                resp["ok"]     = true;
                resp["result"] = {{"token", tok}, {"method", target}, {"ttlSec", Guard().TtlSec()},
                                  {"note", "1 回限り。この method の confirm_token に入れて撃つ。別の method には使えない"}};
            });
    }

    // ---------------------------------------------------------------- journal_list
    {
        McpMeta m;
        m.summary   = "ファイル書き込みジャーナル(<project>/.dx12/journal/)のエントリ一覧(新しい順)。journal_restore で戻せる";
        m.keywords  = "journal ジャーナル 履歴 バックアップ ファイル 復元 ロールバック list";
        m.category  = "safety";
        m.effect    = McpEffect::Read;
        m.timeoutMs = 8000;
        m.idempotent = true;
        m.params    = {P("limit", "int", false, nullptr, "1", "200", "20", "返す件数(既定 20)")};
        m.next      = {{"journal_restore", "エントリを書き戻す"}};
        McpDefine("journal_list", m, DX12E_MCP_HANDLER
            {
                const size_t limit = static_cast<size_t>((std::max)(1, (std::min)(200, params.value("limit", 20))));
                auto& jr = mcpjournal::Instance();
                json arr = json::array();
                for (const auto& e : jr.List(limit))
                    arr.push_back({{"id", e.id}, {"method", e.method}, {"label", e.label}, {"state", e.state},
                                   {"createdAt", e.createdAt}, {"fileCount", e.files.size()}, {"complete", e.complete},
                                   {"txLabel", e.hasTx ? json(e.txLabel) : json(nullptr)},
                                   {"note", e.note}});
                resp["ok"]     = true;
                resp["result"] = {{"entries", std::move(arr)}, {"dir", mcpjournal::Utf8FromPath(jr.Root())},
                                  {"total", jr.EntryCount()}};
            });
    }

    // ---------------------------------------------------------------- journal_restore
    {
        McpMeta m;
        m.summary   = "ジャーナルのエントリを書き戻す(上書きされたファイルは元の内容に、新規作成されたファイルは削除)。復元自体も 1 エントリとして残る";
        m.keywords  = "journal restore ジャーナル 復元 元に戻す ファイル ロールバック 巻き戻し";
        m.category  = "safety";
        m.effect    = McpEffect::WriteFile;
        m.timeoutMs = 20000;
        m.dryRun    = "preview";
        m.journal   = true;
        m.params    = {P("id", "string", true, nullptr, nullptr, nullptr, nullptr, "journal_list の id(例 000012-save_scene)")};
        m.next      = {{"open_scene", "シーンを戻した後は開き直す"}};
        McpDefine("journal_restore", m, DX12E_MCP_HANDLER
            {
                const std::string id = params.value("id", std::string());
                auto& jr = mcpjournal::Instance();
                mcpjournal::EntryInfo info;
                if (!jr.Load(id, info))
                {
                    McpError err(McpErr::NotFound, "journal entry not found: " + id, "journal_list で id を確かめる");
                    err.name  = "E_NOT_FOUND";
                    err.cause = "この id のジャーナルエントリが無い(50 件を超えると古い物から消える)";
                    std::vector<std::string> ids;
                    for (const auto& e : jr.List(200)) ids.push_back(e.id);
                    err.didYouMean = McpSuggest(id, ids, 3);
                    err.fix.push_back(MakeMcpFix("journal_list", json::object(), "実在する id の一覧"));
                    throw err;
                }
                const auto r = jr.Restore(id, /*backupFirst=*/true);
                resp["ok"]     = true;
                resp["result"] = {{"id", id}, {"restored", r.restored}, {"unchanged", r.unchanged},
                                  {"missing", r.missing}, {"warnings", r.warnings}, {"complete", r.complete},
                                  {"note", "ファイルだけを戻した。開いているシーンのメモリには反映されない(シーンを戻したなら open_scene で開き直す)"}};
            });
    }

    // ---------------------------------------------------------------- cancel（M6）
    {
        McpMeta m;
        m.summary   = "実行中の benchmark / step_frames を残り 1 フレームへ切り詰め、次のフレームで自然に完了(部分結果)させる";
        m.keywords  = "cancel 中断 キャンセル benchmark step_frames 止める 打ち切り";
        m.category  = "meta";
        m.effect    = McpEffect::Runtime;
        m.timeoutMs = 5000;
        m.idempotent = true;
        m.params    = {P("target", "enum", false, "benchmark|step_frames|all", nullptr, nullptr, "\"all\"", "中断する対象(既定 all)")};
        McpDefine("cancel", m, DX12E_MCP_HANDLER
            {
                const std::string target = params.value("target", std::string("all"));
                json cancelled = json::array();
                json left = json::object();
                if ((target == "all" || target == "benchmark") && m_benchFramesLeft > 0)
                {
                    left["benchmark"] = m_benchFramesLeft;
                    if (m_benchFramesLeft > 1) m_benchFramesLeft = 1;
                    cancelled.push_back("benchmark");
                }
                if ((target == "all" || target == "step_frames") && m_mcpStepFramesLeft > 0)
                {
                    left["step_frames"] = m_mcpStepFramesLeft;
                    if (m_mcpStepFramesLeft > 1) m_mcpStepFramesLeft = 1;
                    cancelled.push_back("step_frames");
                }
                resp["ok"]     = true;
                resp["result"] = {{"cancelled", std::move(cancelled)}, {"framesLeft", std::move(left)},
                                  {"note", "遅延応答は次フレームで完了する(benchmark は途中までの統計、step_frames は simulatedSec が要求より短い)。framesLeft は切り詰める前の残りフレーム数"}};
            });
    }

    // ================================================================ dryRun プレビュー表
    using mcpsafety::PreviewTable;
    // 例外は「実行すると失敗する」プレビューにする（dryRun 自体は失敗させない）。
    auto addPreview = [](const char* name, std::function<json(const json&)> fn)
    {
        PreviewTable()[name] = [fn, nm = std::string(name)](const std::string& paramsJson) -> std::string
        {
            json p = json::parse(paramsJson, nullptr, /*allow_exceptions=*/false);
            if (p.is_discarded() || !p.is_object()) p = json::object();
            json out;
            try { out = fn(p); }
            catch (const McpError& e)
            {
                out = {{"summary", std::string("実行すると失敗する: ") + e.what()}, {"willFail", true}, {"reason", e.what()},
                       {"targets", json::array()}, {"count", 0}, {"destructive", false},
                       {"undoable", "変更なし(失敗する)"}, {"files", json::array()}, {"notes", json::array()}};
                if (!e.hint.empty()) out["hint"] = e.hint;
                if (!e.didYouMean.empty()) out["didYouMean"] = e.didYouMean;
            }
            catch (const std::exception& e)
            {
                out = {{"summary", std::string("実行すると失敗する: ") + e.what()}, {"willFail", true}, {"reason", e.what()},
                       {"targets", json::array()}, {"count", 0}, {"destructive", false},
                       {"undoable", "変更なし(失敗する)"}, {"files", json::array()}, {"notes", json::array()}};
            }
            (void)nm;
            return out.dump(-1, ' ', false, json::error_handler_t::replace);
        };
    };
    auto mk = [](const std::string& summary, bool destructive, const std::string& undoable) -> json
    {
        return {{"summary", summary}, {"targets", json::array()}, {"count", 0}, {"destructive", destructive},
                {"undoable", undoable}, {"files", json::array()}, {"notes", json::array()}};
    };
    static const char* const kUndoScene = "Undo 1 エントリ(AI: <method>)で戻る(Editor モードのみ)";
    static const char* const kUndoFile  = "ファイルを書く。Undo では戻らない(journal_restore で戻せる)";
    static const char* const kNoUndo    = "戻せない";

    auto nameOf = [this](entt::entity e) -> std::string
    {
        auto& reg = m_scene->GetRegistry();
        return reg.all_of<NameTag>(e) ? reg.get<NameTag>(e).name : std::string();
    };
    auto subtreeOf = [this](entt::entity root) -> std::vector<entt::entity>
    {
        auto& reg = m_scene->GetRegistry();
        std::vector<entt::entity> subtree{root};
        for (size_t i = 0; i < subtree.size(); ++i)
            for (auto [c, t] : reg.view<const Transform>().each())
                if (t.parent == subtree[i]) subtree.push_back(c);
        return subtree;
    };
    // entity か name で指されたエンティティ。無ければ McpError（addPreview が willFail にする）。
    auto targetJson = [this, nameOf](const json& p, entt::entity& out) -> json
    {
        out = ResolveMcpEntity(*m_scene, p);
        return {{"kind", "entity"}, {"exists", true}, {"id", static_cast<u32>(out)}, {"name", nameOf(out)}};
    };
    auto fileJson = [](const fs::path& full, const std::string& shown, const char* action) -> json
    {
        std::error_code ec;
        const bool ex = fs::exists(full, ec);
        json f = {{"path", shown}, {"exists", ex}, {"action", action}};
        if (ex && fs::is_regular_file(full, ec)) f["bytes"] = static_cast<u64>(fs::file_size(full, ec));
        return f;
    };
    auto dirStats = [](const fs::path& p) -> std::pair<u64, u64>   // {ファイル数, 総バイト}
    {
        std::error_code ec;
        u64 n = 0, b = 0;
        if (fs::is_regular_file(p, ec)) return {1, static_cast<u64>(fs::file_size(p, ec))};
        for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec)) { ++n; b += static_cast<u64>(it->file_size(ec)); }
        return {n, b};
    };
    auto vec3j = [](float x, float y, float z) { return json::array({x, y, z}); };
    auto editorNote = [this](json& pv)
    {
        if (m_engineMode == EngineMode::Playing)
            pv["notes"].push_back("いまは Play 中。この操作は Editor モードでのみ実行できる(dx12_stop が先)");
    };

    // ---- create_entity / spawn_model / spawn_prefab ----
    addPreview("create_entity", [=, this](const json& p) -> json
    {
        static const char* const kTypes[] = {"box", "sphere", "plane", "empty", "camera", "light_directional", "light_point",
            "light_spot", "particle_emitter", "trigger", "decal", "ui_canvas", "ui_image", "ui_text", "ui_button", "ui_slider",
            "ui_toggle", "ui_scrollview"};
        const std::string type = p.value("type", std::string("box"));
        bool ok = false;
        for (const char* t : kTypes) if (type == t) ok = true;
        if (!ok) throw McpError(McpErr::InvalidParam, "type '" + type + "' は使えない", "type は box / sphere / plane / empty / camera / light_* / ui_* など");
        const std::string name = p.value("name", std::string());
        json pv = mk("エンティティを 1 つ生成する(" + type + ")", false, kUndoScene);
        json t = {{"kind", "entity"}, {"exists", false}, {"wouldCreate", true}, {"type", type},
                  {"name", name.empty() ? json("(自動: 種別名。重複時は連番)") : json(name)}};
        if (p.contains("position")) t["position"] = p["position"];
        if (p.contains("parent") || p.contains("parentName")) t["parent"] = p.contains("parent") ? p["parent"] : p["parentName"];
        pv["targets"].push_back(std::move(t));
        pv["count"] = 1;
        editorNote(pv);
        return pv;
    });
    auto modelPreview = [=, this](const json& p, bool prefab) -> json
    {
        const std::string path = p.value("path", std::string());
        if (path.empty()) throw McpError(McpErr::InvalidParam, "missing 'path'", "assets 相対のパスを渡す");
        ValidateMcpAssetRelPath(path);
        const fs::path full = fs::path(PathResolver::AssetsDir()) / path;
        std::error_code ec;
        const bool ex = fs::exists(full, ec);
        json pv = mk(std::string(prefab ? "プレハブ " : "モデル ") + path + " を生成する", false, kUndoScene);
        json f = fileJson(full, path, "read");
        pv["files"].push_back(f);
        std::string name = p.value("name", std::string());
        if (name.empty()) name = fs::path(path).stem().string();
        pv["targets"].push_back({{"kind", "entity"}, {"exists", false}, {"wouldCreate", true}, {"name", name}, {"source", path}, {"sourceExists", ex}});
        pv["count"] = 1;
        if (!ex) { pv["willFail"] = true; pv["notes"].push_back("ファイルが assets 以下に無い。実行すると E_NOT_FOUND_ASSET で失敗する"); }
        if (prefab) pv["notes"].push_back("プレハブは階層ぶんのエンティティが生成される(件数は実行するまで分からない)");
        editorNote(pv);
        return pv;
    };
    addPreview("spawn_model", [=](const json& p) { return modelPreview(p, false); });
    addPreview("spawn_prefab", [=](const json& p) { return modelPreview(p, true); });

    // ---- delete_entity / duplicate_entity ----
    addPreview("delete_entity", [=, this](const json& p) -> json
    {
        entt::entity e{};
        json t = targetJson(p, e);
        const auto sub = subtreeOf(e);
        json names = json::array();
        for (size_t i = 0; i < sub.size() && i < 20; ++i) names.push_back(nameOf(sub[i]));
        t["descendants"] = sub.size() - 1;
        json pv = mk("エンティティ " + nameOf(e) + " を子孫ごと削除する(" + std::to_string(sub.size()) + " 体)", true, kUndoScene);
        pv["targets"].push_back(std::move(t));
        pv["count"] = sub.size();
        pv["names"] = names;
        if (sub.size() > 20) pv["notes"].push_back("名前は先頭 20 件だけ");
        editorNote(pv);
        return pv;
    });
    addPreview("duplicate_entity", [=, this](const json& p) -> json
    {
        entt::entity e{};
        json t = targetJson(p, e);
        const auto sub = subtreeOf(e);
        json pv = mk("エンティティ " + nameOf(e) + " を子孫ごと複製する(" + std::to_string(sub.size()) + " 体増える)", false, kUndoScene);
        pv["targets"].push_back(std::move(t));
        pv["count"] = sub.size();
        editorNote(pv);
        return pv;
    });

    // ---- set_transform / set_component / remove_component / set_parent / rename_entity ----
    addPreview("set_transform", [=, this](const json& p) -> json
    {
        entt::entity e{};
        json t = targetJson(p, e);
        auto& reg = m_scene->GetRegistry();
        if (!reg.all_of<Transform>(e)) throw McpError(McpErr::NotFound, "entity has no Transform", "Transform を持つエンティティを指す");
        const Transform& tr = reg.get<Transform>(e);
        json changes = json::array();
        auto add = [&](const char* field, json from) { if (p.contains(field)) changes.push_back({{"field", field}, {"from", from}, {"to", p[field]}}); };
        add("position", vec3j(tr.position.x, tr.position.y, tr.position.z));
        add("rotation", vec3j(tr.rotation.x, tr.rotation.y, tr.rotation.z));
        add("scale", vec3j(tr.scale.x, tr.scale.y, tr.scale.z));
        add("quaternion", json::array({tr.quaternion.x, tr.quaternion.y, tr.quaternion.z, tr.quaternion.w}));
        t["changes"] = changes;
        json pv = mk("Transform の " + std::to_string(changes.size()) + " 項目を変更する", false, kUndoScene);
        pv["targets"].push_back(std::move(t));
        pv["count"] = 1;
        return pv;
    });
    addPreview("set_component", [=, this](const json& p) -> json
    {
        entt::entity e{};
        json t = targetJson(p, e);
        const std::string comp = p.value("component", std::string());
        if (comp.empty()) throw McpError(McpErr::InvalidParam, "missing 'component'", "component に jsonKey を渡す");
        json data;
        if (p.contains("data")) data = p["data"]; else if (p.contains("values")) data = p["values"];
        if (!data.is_object() || data.empty()) throw McpError(McpErr::InvalidParam, "missing component fields: pass a non-empty 'data' object", "data に変えたいフィールドを渡す");
        json cur = json::parse(SceneSerializer::SerializeEntity(*m_scene, e, PathResolver::AssetsDir()), nullptr, false);
        const bool exists = comp == "transform" || (cur.is_object() && cur.contains(comp));
        json fields = json::array(), overwritten = json::array();
        for (auto it = data.begin(); it != data.end(); ++it)
        {
            fields.push_back(it.key());
            if (exists && cur.is_object() && cur.contains(comp) && cur[comp].is_object() && cur[comp].contains(it.key()))
                overwritten.push_back({{"field", it.key()}, {"from", cur[comp][it.key()]}, {"to", it.value()}});
        }
        t["component"] = comp;
        t["componentExists"] = exists;
        t["action"] = exists ? "merge(既存コンポーネントへ指定フィールドだけ上書き)" : "add(コンポーネントを新規追加)";
        t["fields"] = fields;
        t["overwritten"] = overwritten;
        json pv = mk(std::string("コンポーネント ") + comp + (exists ? " を更新する" : " を追加する"), false, kUndoScene);
        pv["targets"].push_back(std::move(t));
        pv["count"] = 1;
        return pv;
    });
    addPreview("remove_component", [=, this](const json& p) -> json
    {
        entt::entity e{};
        json t = targetJson(p, e);
        const std::string comp = p.value("component", std::string());
        if (comp == "transform" || comp == "name") throw McpError(McpErr::InvalidParam, "cannot remove core component (transform/name)", "transform と name は外せない");
        json cur = json::parse(SceneSerializer::SerializeEntity(*m_scene, e, PathResolver::AssetsDir()), nullptr, false);
        const bool exists = cur.is_object() && cur.contains(comp);
        t["component"] = comp;
        t["componentExists"] = exists;
        json pv = mk("コンポーネント " + comp + " を外す", true, kUndoScene);
        pv["targets"].push_back(std::move(t));
        pv["count"] = exists ? 1 : 0;
        if (!exists) pv["notes"].push_back("このコンポーネントは付いていない(実行しても変わらない、または UNKNOWN_COMPONENT)");
        return pv;
    });
    addPreview("set_parent", [=, this](const json& p) -> json
    {
        entt::entity child{};
        json t = targetJson(p, child);
        auto& reg = m_scene->GetRegistry();
        const u32 pid = p.value("parent", 0xFFFFFFFFu);
        json from = nullptr, to = nullptr;
        if (const auto* tr = reg.try_get<Transform>(child); tr && tr->parent != entt::null && reg.valid(tr->parent)) from = nameOf(tr->parent);
        json pv = mk("親子関係を変更する", false, kUndoScene);
        if (pid != 0xFFFFFFFFu)
        {
            const auto parent = static_cast<entt::entity>(pid);
            if (!reg.valid(parent)) throw McpError(McpErr::NotFound, "invalid parent id", "親の entityId を取り直す");
            int depth = 0;
            for (entt::entity cur = parent; cur != entt::null && depth < 4096; ++depth)
            {
                if (cur == child) throw McpError(McpErr::InvalidParam, "would create cycle", "子孫を親にはできない");
                auto* tt = reg.try_get<Transform>(cur);
                cur = tt ? tt->parent : entt::null;
            }
            to = nameOf(parent);
        }
        t["parentFrom"] = from;
        t["parentTo"] = to;
        pv["targets"].push_back(std::move(t));
        pv["count"] = 1;
        pv["notes"].push_back("set_parent はワールド座標を保持しない(子はローカル座標として解釈される)");
        return pv;
    });
    addPreview("rename_entity", [=, this](const json& p) -> json
    {
        const auto e = static_cast<entt::entity>(p.value("entity", 0xFFFFFFFFu));
        auto& reg = m_scene->GetRegistry();
        if (!reg.valid(e)) throw McpError(McpErr::NotFound, "invalid entity id", "entity に今のシーンの entityId を渡す");
        if (!reg.all_of<NameTag>(e)) throw McpError(McpErr::NotFound, "entity has no NameTag", "名前を持つエンティティを指す");
        const std::string base = p.value("name", std::string());
        if (base.empty()) throw McpError(McpErr::InvalidParam, "missing 'name'", "新しい名前を渡す");
        auto taken = [&](const std::string& s) { for (auto [oe, tag] : reg.view<NameTag>().each()) if (oe != e && tag.name == s) return true; return false; };
        std::string name = base;
        int n = 2;
        while (taken(name)) name = base + "_" + std::to_string(n++);
        json pv = mk("エンティティ名を変更する", false, kUndoScene);
        pv["targets"].push_back({{"kind", "entity"}, {"exists", true}, {"id", static_cast<u32>(e)}, {"nameFrom", nameOf(e)}, {"nameTo", name}});
        pv["count"] = 1;
        if (name != base) pv["notes"].push_back("同名があるので連番が付く");
        pv["notes"].push_back("名前で結ばれた参照(Lua の entity プロパティ / Trigger の filter・target)も追従して書き換わる");
        return pv;
    });

    // ---- save_scene / open_scene ----
    addPreview("save_scene", [=, this](const json& p) -> json
    {
        const std::string rel = p.value("path", std::string());
        fs::path full;
        std::string shown;
        if (rel.empty())
        {
            if (!m_editorCtx || m_editorCtx->currentScenePath.empty()) throw McpError(McpErr::InvalidParam, "no current scene; specify 'path'", "path を指定する");
            full = fs::path(m_editorCtx->currentScenePath);
            shown = full.generic_string();
        }
        else
        {
            if (rel.front() == '/' || rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)", "path は assets 相対");
            full = fs::path(PathResolver::AssetsDir() + rel);
            shown = rel;
        }
        std::error_code ec;
        const bool ex = fs::exists(full, ec);
        json pv = mk(ex ? "シーン " + shown + " を上書き保存する" : "シーン " + shown + " を新規に保存する", ex, kUndoFile);
        pv["files"].push_back(fileJson(full, shown, ex ? "overwrite" : "create"));
        pv["targets"].push_back({{"kind", "scene"}, {"path", shown}, {"exists", ex}, {"entityCount", m_scene->GetRegistry().view<NameTag>().size()},
                                 {"sceneDirty", m_editorCtx ? m_editorCtx->IsSceneDirty() : false}});
        pv["count"] = 1;
        if (m_engineMode == EngineMode::Playing) pv["notes"].push_back("Play 中に保存すると欠けたシーンが書かれることがある(dx12_stop してから)");
        return pv;
    });
    addPreview("open_scene", [=, this](const json& p) -> json
    {
        const std::string rel = p.value("path", std::string());
        if (rel.empty()) throw McpError(McpErr::InvalidParam, "missing 'path'", "assets 相対のシーンを渡す");
        ValidateMcpAssetRelPath(rel);
        const fs::path full = fs::path(PathResolver::AssetsDir() + rel);
        std::error_code ec;
        const bool ex = fs::exists(full, ec);
        const bool dirty = m_editorCtx ? m_editorCtx->IsSceneDirty() : false;
        json pv = mk("シーン " + rel + " を開く(現在のシーンを閉じる)", dirty, "現在のシーンは閉じる。Undo 履歴も消える(未保存の変更は自動保存の範囲だけ残る)");
        pv["targets"].push_back({{"kind", "scene"}, {"path", rel}, {"exists", ex}, {"currentScene", m_editorCtx ? m_editorCtx->currentScenePath : std::string()}, {"sceneDirty", dirty}});
        pv["files"].push_back(fileJson(full, rel, "read"));
        pv["count"] = 1;
        if (!ex) { pv["willFail"] = true; pv["notes"].push_back("シーンが無い。実行すると E_NOT_FOUND_SCENE で失敗する"); }
        if (dirty) pv["notes"].push_back("未保存の変更がある(sceneDirty:true)。先に save_scene するか、意図した変更か確認する");
        if (m_engineMode == EngineMode::Playing) pv["notes"].push_back("Play 中は開けない(dx12_stop が先)");
        return pv;
    });

    // ---- create_lua_component / create_shader ----
    addPreview("create_lua_component", [=, this](const json& p) -> json
    {
        const std::string name = p.value("name", std::string());
        const std::string code = p.value("code", std::string());
        if (name.empty()) throw McpError(McpErr::InvalidParam, "missing 'name'", "name にコンポーネント名を渡す");
        if (name.find_first_of("/\\:*?\"<>|") != std::string::npos) throw McpError(McpErr::InvalidParam, "invalid component name", "ファイル名に使えない文字を除く");
        std::string serr;
        const bool syntaxOk = m_scriptEngine->CheckLuaSyntax(code, serr);
        const std::string rel = "components/" + name + ".lua";
        const fs::path full = fs::path(PathResolver::AssetsDir()) / rel;
        std::error_code ec;
        const bool ex = fs::exists(full, ec);
        json pv = mk(ex ? "Lua コンポーネント " + rel + " を上書きする" : "Lua コンポーネント " + rel + " を新規作成する", ex, kUndoFile);
        pv["files"].push_back(fileJson(full, rel, ex ? "overwrite" : "create"));
        pv["files"].back()["newBytes"] = code.size();
        pv["targets"].push_back({{"kind", "file"}, {"path", rel}, {"exists", ex}, {"syntaxOk", syntaxOk}});
        pv["count"] = 1;
        if (!syntaxOk) { pv["willFail"] = true; pv["notes"].push_back("Lua の構文エラー: " + serr + "(実行すると書かずに失敗する)"); }
        return pv;
    });
    addPreview("create_shader", [=, this](const json& p) -> json
    {
        const std::string name = p.value("name", std::string());
        const std::string code = p.value("code", std::string());
        const std::string tmpl = p.value("template", std::string());
        if (name.empty()) throw McpError(McpErr::InvalidParam, "missing 'name'", "name にシェーダ名を渡す");
        if (name.find_first_of("/\\:*?\"<>|") != std::string::npos) throw McpError(McpErr::InvalidParam, "invalid shader name", "ファイル名に使えない文字を除く");
        if (code.empty() && tmpl.empty()) throw McpError(McpErr::InvalidParam, "missing 'code' (または template を指定する)", "code か template を渡す");
        const std::string rel = name + ".hlsl";
        const fs::path full = fs::path(PathResolver::ProjectShaderDir()) / rel;
        std::error_code ec;
        const bool ex = fs::exists(full, ec);
        json pv = mk(ex ? "シェーダ " + rel + " を上書きする" : "シェーダ " + rel + " を新規作成する", ex, kUndoFile);
        pv["files"].push_back(fileJson(full, "shaders/" + rel, ex ? "overwrite" : "create"));
        pv["targets"].push_back({{"kind", "file"}, {"path", "shaders/" + rel}, {"exists", ex}, {"fromTemplate", code.empty() ? json(tmpl) : json(nullptr)}});
        pv["count"] = 1;
        pv["notes"].push_back("書いた後にコンパイルを試みる(失敗してもファイルは残る)");
        return pv;
    });

    // ---- move_asset / delete_asset / import_asset ----
    addPreview("move_asset", [=, this](const json& p) -> json
    {
        const std::string fromRel = ValidateMcpAssetRelPath(p.value("from", std::string()), "from");
        const std::string toRel = ValidateMcpAssetRelPath(p.value("to", std::string()), "to");
        const fs::path root(PathResolver::AssetsDir());
        const fs::path fromP = root / fromRel, toP = root / toRel;
        std::error_code ec;
        if (!fs::exists(fromP, ec)) throw McpError(McpErr::NotFound, "asset not found: " + fromRel, "from は今ある assets 相対パス");
        const bool toEx = fs::exists(toP, ec);
        const bool overwrite = p.value("overwrite", false);
        const auto st = dirStats(fromP);
        json pv = mk("アセット " + fromRel + " を " + toRel + " へ移動する", toEx, kUndoFile);
        pv["files"].push_back(fileJson(fromP, fromRel, "move"));
        pv["files"].push_back(fileJson(toP, toRel, toEx ? "overwrite" : "create"));
        pv["targets"].push_back({{"kind", "file"}, {"path", fromRel}, {"exists", true}, {"files", st.first}, {"bytes", st.second}, {"isDirectory", fs::is_directory(fromP, ec)}});
        pv["count"] = st.first;
        if (toEx && !overwrite) { pv["willFail"] = true; pv["notes"].push_back("移動先が既にある(overwrite:true が要る)"); }
        if (p.value("updateFiles", true)) pv["notes"].push_back("参照を持つ他のシーン/プレハブ等のファイルも書き換わる(それらはジャーナルに退避されない。journal は complete:false になる)");
        return pv;
    });
    addPreview("delete_asset", [=, this](const json& p) -> json
    {
        const std::string rel = ValidateMcpAssetRelPath(p.value("path", std::string()));
        const fs::path full = fs::path(PathResolver::AssetsDir()) / rel;
        std::error_code ec;
        if (!fs::exists(full, ec)) throw McpError(McpErr::NotFound, "asset not found: " + rel, "path は assets 相対");
        const bool isDir = fs::is_directory(full, ec);
        const auto st = dirStats(full);
        std::vector<std::string> who;
        const int refCount = SceneSerializer::CountAssetPathRefs(*m_scene, rel, &who);
        json pv = mk("アセット " + rel + (isDir ? " をフォルダごと削除する(" + std::to_string(st.first) + " ファイル)" : " を削除する"), true, kUndoFile);
        pv["files"].push_back(fileJson(full, rel, "delete"));
        pv["targets"].push_back({{"kind", "file"}, {"path", rel}, {"exists", true}, {"isDirectory", isDir}, {"files", st.first}, {"bytes", st.second},
                                 {"stillReferencedCount", refCount}, {"stillReferencedBy", who}});
        pv["count"] = st.first;
        if (isDir && !p.value("recursive", false)) { pv["willFail"] = true; pv["notes"].push_back("フォルダは recursive:true が要る"); }
        if (refCount > 0) pv["notes"].push_back("開いているシーンがまだ参照している(消すと出ない/鳴らないだけでエラーは出ない)");
        pv["notes"].push_back("guarded な操作。実行には確認トークン(guard_token)が要る。ジャーナルから journal_restore で復元できる");
        return pv;
    });
    addPreview("import_asset", [=, this](const json& p) -> json
    {
        const std::string src = p.value("sourcePath", std::string());
        if (src.empty()) throw McpError(McpErr::InvalidParam, "missing 'sourcePath'", "取り込む元の絶対パスを渡す");
        const std::string destRel = ValidateMcpAssetRelPath(p.value("destPath", std::string()), "destPath");
        const fs::path srcP(src), dest = fs::path(PathResolver::AssetsDir()) / destRel;
        std::error_code ec;
        if (!fs::exists(srcP, ec)) throw McpError(McpErr::NotFound, "source not found: " + src, "sourcePath は取り込み元の絶対パス");
        const auto st = dirStats(srcP);
        const bool destEx = fs::exists(dest, ec);
        const bool overwrite = p.value("overwrite", false);
        json pv = mk("assets/" + destRel + " へ " + std::to_string(st.first) + " ファイル(" + std::to_string(st.second) + " バイト)を取り込む", destEx && overwrite, kUndoFile);
        pv["files"].push_back(fileJson(dest, destRel, destEx ? "overwrite" : "create"));
        pv["targets"].push_back({{"kind", "file"}, {"path", destRel}, {"exists", destEx}, {"sourceFiles", st.first}, {"sourceBytes", st.second}});
        pv["count"] = st.first;
        if (destEx && !overwrite) { pv["willFail"] = true; pv["notes"].push_back("取り込み先が既にある(overwrite:true が要る)"); }
        return pv;
    });

    // ---- journal_restore ----
    addPreview("journal_restore", [=](const json& p) -> json
    {
        const std::string id = p.value("id", std::string());
        auto& jr = mcpjournal::Instance();
        mcpjournal::EntryInfo info;
        if (!jr.Load(id, info)) throw McpError(McpErr::NotFound, "journal entry not found: " + id, "journal_list で id を確かめる");
        json pv = mk("ジャーナル " + id + "(" + info.method + ")を書き戻す", true, kUndoFile);
        size_t willChange = 0;
        for (const auto& f : info.files)
        {
            fs::path target = mcpjournal::PathFromUtf8(f.path);
            if (!target.is_absolute()) target = jr.BaseDir() / target;
            std::error_code ec;
            const bool ex = fs::exists(target, ec);
            const char* action = !f.existed ? (ex ? "delete" : "unchanged") : (f.backup.empty() ? "missing" : "overwrite");
            if (std::string(action) != "unchanged" && std::string(action) != "missing") ++willChange;
            pv["files"].push_back({{"path", f.path}, {"exists", ex}, {"action", action}});
        }
        pv["targets"].push_back({{"kind", "journal"}, {"id", id}, {"state", info.state}, {"complete", info.complete}, {"files", info.files.size()}});
        pv["count"] = willChange;
        if (!info.complete) pv["notes"].push_back("このエントリは不完全(退避できなかったファイルがある)。戻せる範囲だけ戻る");
        pv["notes"].push_back("現在の内容と同じファイルは何もしない(willChange は差があるものの上限)");
        return pv;
    });
}
