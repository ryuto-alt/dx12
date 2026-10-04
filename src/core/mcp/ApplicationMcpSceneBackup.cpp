// ===========================================================================
// MCP: シーンの世代つきバックアップ（core/SceneBackup.h。置き場は <プロジェクト>/.dx12/backups/）
//   scene_backups … op = list / restore / snapshot / settings
//     list     … そのシーンの世代の一覧（新しい順。id・時刻・大きさ・.parts/.inst/.nav の有無・ファイルの場所）
//     restore  … 世代 id でシーンを戻す（戻す前の版も 1 世代残す＝戻した操作も取り消せる）。戻したシーンは次のフレームで読み直す
//     snapshot … 今ディスクにある版を今すぐ 1 世代残す（間隔の制限を無視）
//     settings … 方針（有効 / 世代数 / 容量の上限 MB / 最短間隔 秒）の読み書き（プロジェクトの settings.json に保存）
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/SceneBackup.h"
#include "core/mcp/McpManifestBuild.h"

namespace dx12e
{
using namespace appdetail;
using namespace mcpdata;   // P(...)

namespace
{
using nlohmann::json;
namespace fs = std::filesystem;

std::string TimeText(int64_t unixTime)
{
    const std::time_t t = static_cast<std::time_t>(unixTime);
    std::tm tmv{};
    char buf[48] = {};
    if (localtime_s(&tmv, &t) == 0) std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

json GenJson(const scenebackup::Generation& g)
{
    return json{{"id", g.id}, {"time", TimeText(g.unixTime)}, {"bytes", g.bytes},
                {"hasParts", g.hasParts}, {"hasInst", g.hasInst}, {"hasNav", g.hasNav},
                {"file", g.root.string()}};
}

json PolicyJson()
{
    const scenebackup::Policy& p = scenebackup::GlobalPolicy();
    return json{{"enabled", p.enabled}, {"generations", p.generations},
                {"maxMb", p.maxTotalBytes / (1024ull * 1024ull)}, {"intervalSec", p.minIntervalSec}};
}
} // namespace

void Application::RegisterMcpSceneBackupMethods()
{
    McpMeta m;
    m.summary  = "シーンの世代つきバックアップ（<プロジェクト>/.dx12/backups/）の一覧・復元・設定。シーンを保存するたびに直前の版が世代として残る"
                 "（既定 10 世代・容量 1024MB・60 秒間隔。分割シーンの .parts・インスタンス群の .inst・.nav も一緒）。"
                 "op=list: 世代の一覧（新しい順）。op=restore: id の版へ戻す（戻す前の版も 1 世代残るので取り消せる。戻したシーンは次のフレームで読み直す）。"
                 "op=snapshot: 今ディスクにある版を今すぐ残す。op=settings: 方針の確認・変更。"
                 "シーンが開けない（壊れた・空）ときは保存を止めてあるので、ここから戻す";
    m.keywords = "scene backup restore generation history 以前の版 バックアップ 世代 復元 戻す 壊れた シーン 保存 消えた";
    m.category = "scene"; m.group = "scene_backups"; m.target = "scene_backups";
    m.effect = McpEffect::WriteScene; m.mode = "editor"; m.timeoutMs = 30000; m.idempotent = false;
    m.aliases = {"dx12_scene_backups"};
    m.params = {
        P("op", "enum", true, "list|restore|snapshot|settings", nullptr, nullptr, nullptr, "操作"),
        P("path", "string", false, nullptr, nullptr, nullptr, nullptr, "対象のシーン（assets 相対 例: scenes/main.json、または絶対パス）。省略 = いま開いているシーン（開けなかったシーンがあればそれ）"),
        P("id", "string", false, nullptr, nullptr, nullptr, nullptr, "restore: 戻す世代の id（list の id）"),
        P("enabled", "bool", false, nullptr, nullptr, nullptr, nullptr, "settings: バックアップを有効にする / 止める"),
        P("generations", "int", false, nullptr, "1", "200", nullptr, "settings: シーンごとに残す世代数"),
        P("maxMb", "int", false, nullptr, "16", "1048576", nullptr, "settings: 置き場の合計容量の上限（MB。超えたら古い世代から消す。各シーンの最新 1 世代は残す）"),
        P("intervalSec", "int", false, nullptr, "0", "86400", nullptr, "settings: 前の世代からこの秒数が経つまで次の世代を作らない（0 = 毎回）"),
    };
    m.next = {{"open_scene", "戻したシーンを確かめる"}, {"validate_scene", "戻した後の検査"}};
    m.examples = {{"{\"op\":\"list\"}", "いま開いているシーンの世代を見る"},
                  {"{\"op\":\"restore\",\"id\":\"main_20261002_101530\"}", "その版へ戻す"},
                  {"{\"op\":\"settings\",\"generations\":20}", "世代数を 20 にする"}};
    McpDefine("scene_backups", McpMeta(m), DX12E_MCP_HANDLER
        {
            const std::string op = params.value("op", std::string());
            const fs::path projectRoot = fs::path(PathResolver::BaseDir());

            auto resolveScene = [&]() -> fs::path {
                std::string p = params.value("path", std::string());
                if (p.empty()) p = m_editorCtx ? m_editorCtx->currentScenePath : std::string();
                if (p.empty() && m_editorCtx) p = m_editorCtx->sceneBackupTarget;
                if (p.empty())
                    throw McpError(McpErr::InvalidParam, "対象のシーンが分かりません", "path を渡してください（例: \"scenes/main.json\"）");
                fs::path fp(p);
                if (!fp.is_absolute()) fp = fs::path(PathResolver::AssetsDir()) / fp;
                return fp;
            };

            if (op == "settings")
            {
                bool changed = false;
                if (params.contains("enabled") && params["enabled"].is_boolean()) { PersistSet("backup_enabled", params["enabled"].get<bool>() ? 1.0 : 0.0); changed = true; }
                if (params.contains("generations")) { PersistSet("backup_generations", static_cast<double>(McpIntParam(params, "generations", 10, 1, 200))); changed = true; }
                if (params.contains("maxMb")) { PersistSet("backup_max_mb", static_cast<double>(McpIntParam(params, "maxMb", 1024, 16, 1048576))); changed = true; }
                if (params.contains("intervalSec")) { PersistSet("backup_interval_sec", static_cast<double>(McpIntParam(params, "intervalSec", 60, 0, 86400))); changed = true; }
                if (changed) ApplyBackupPolicyFromSettings();
                resp["ok"] = true;
                resp["result"] = {{"policy", PolicyJson()}, {"changed", changed},
                                  {"dir", scenebackup::BackupDir(projectRoot).string()}};
            }
            else if (op == "list")
            {
                const fs::path scene = resolveScene();
                json arr = json::array();
                for (const auto& g : scenebackup::List(projectRoot, scene)) arr.push_back(GenJson(g));
                resp["ok"] = true;
                resp["result"] = {{"scene", scene.string()}, {"dir", scenebackup::BackupDir(projectRoot).string()},
                                  {"policy", PolicyJson()}, {"count", arr.size()}, {"generations", std::move(arr)},
                                  {"loadFailed", m_editorCtx && !m_editorCtx->sceneLoadFailedPath.empty()}};
            }
            else if (op == "snapshot")
            {
                const fs::path scene = resolveScene();
                std::string why;
                const std::string id = scenebackup::Snapshot(projectRoot, scene, /*force=*/true, &why);
                resp["ok"] = true;
                resp["result"] = {{"created", !id.empty()}, {"id", id}, {"why", why}};
            }
            else if (op == "restore")
            {
                if (busyPlaying) throw McpError(McpErr::ModeConflict, "Play 中は戻せません", "先に dx12_stop で Editor へ戻してください");
                const std::string id = params.value("id", std::string());
                if (id.empty()) throw McpError(McpErr::InvalidParam, "id が要ります", "op:\"list\" で id を確かめる");
                const fs::path scene = resolveScene();
                std::string err;
                if (!RestoreSceneBackup(id, scene.string(), err))
                    throw McpError(McpErr::InvalidParam, "戻せませんでした: " + err, "op:\"list\" で id を確かめる");
                resp["ok"] = true;
                resp["result"] = {{"restored", id}, {"scene", scene.string()},
                                  {"note", "戻したシーンは次のフレームで読み直します（dx12_ping の sceneGeneration で確認）。戻す前の版も 1 世代残してあります"}};
            }
            else
            {
                throw McpError(McpErr::InvalidParam, "op は list / restore / snapshot / settings のどれか", "", {"list", "restore", "snapshot", "settings"});
            }
        });
}

} // namespace dx12e
