// ===========================================================================
// MCP: ビルド検証 / Lua / テクスチャ / アニメ / マルチプレイ
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// method の足し方は本ファイル内 McpDefine の並びに倣う（作法は ApplicationInternal.h の DX12E_MCP_HANDLER 付近）。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;


// ---- ビルド検証 / Lua / テクスチャ / アニメーション / マルチプレイ ----
void Application::RegisterMcpToolingMethods()
{
    using json = nlohmann::json;
    namespace fs = std::filesystem;

    // ════════════════════════════════════════════════════════════
    //  ビルド/検証パイプライン連携
    // ════════════════════════════════════════════════════════════
    McpDefine("validate_scene", "path:string", DX12E_MCP_HANDLER
        {
            std::string rel = params.value("path", std::string());
            fs::path scenePath;
            if (rel.empty())
            {
                if (m_editorCtx->currentScenePath.empty())
                    throw McpError(McpErr::InvalidParam, "no scene currently open and 'path' not given");
                scenePath = m_editorCtx->currentScenePath;
            }
            else
            {
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
                scenePath = fs::path(PathResolver::AssetsDir()) / rel;
            }
            if (!fs::exists(scenePath)) throw McpError(McpErr::NotFound, "scene not found: " + scenePath.string());

            wchar_t exeBuf[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
            std::string exePath(fs::path(exeBuf).string());

            // 呼び出しごとに専用の作業ディレクトリで実行(validate_report.txt の競合/汚染回避)。
            static int s_validateSeq = 0;
            fs::path workDir = fs::temp_directory_path() /
                ("dx12_validate_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++s_validateSeq));
            std::error_code ec;
            fs::create_directories(workDir, ec);

            const std::string args = "--validate \"" + scenePath.string() + "\"";
            const int code = RunEngineSubprocessAndWait(exePath, args, workDir.string(), 30000);

            std::string report;
            std::ifstream rf(workDir / "validate_report.txt", std::ios::binary);
            if (rf) { std::ostringstream ss; ss << rf.rdbuf(); report = ss.str(); }
            fs::remove_all(workDir, ec);

            resp["ok"] = true;
            resp["result"] = {{"pass", code == 0}, {"exitCode", code}, {"report", report},
                              {"scenePath", scenePath.string()}};
        });

    McpDefine("build_game", "", DX12E_MCP_HANDLER
        {
            const bool ok = BuildGame();
            json result{{"success", ok}};
            if (m_editorCtx)
            {
                result["outputDir"] = m_editorCtx->buildConfig.outputDir.empty()
                    ? (PathResolver::BaseDir() + "build/game") : m_editorCtx->buildConfig.outputDir;
                if (!ok) result["error"] = m_editorCtx->buildErrorMsg;
            }
            resp["ok"] = true;
            resp["result"] = std::move(result);
        });

    // ════════════════════════════════════════════════════════════
    //  Lua 即時実行(eval) — デバッグ用。globals フォールバック環境で実行するため
    //  scene/physics/camera/audio 等の既存グローバルバインディングがそのまま使える。
    //  print() は Logger へ差し替え済みなので log() と同じく dx12_get_log で見える。
    // ════════════════════════════════════════════════════════════
    McpDefine("eval_lua", "code:string", DX12E_MCP_HANDLER
        {
            const std::string code = params.value("code", std::string());
            if (code.empty()) throw McpError(McpErr::InvalidParam, "missing 'code'");
            std::string resultStr, err;
            const bool ok = m_scriptEngine->EvalLua(code, resultStr, err);
            if (!ok) throw McpError(McpErr::Internal, "Lua error: " + err);
            resp["ok"] = true;
            resp["result"] = {{"result", resultStr}};
        });

    // ════════════════════════════════════════════════════════════
    //  マテリアルテクスチャ上書き(Inspector の D&D 割当と同じ経路)
    // ════════════════════════════════════════════════════════════
    McpDefine("set_texture", "entity:int,name:string,path:string,slot:string,submesh:int", DX12E_MCP_HANDLER
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<MeshRenderer>(e))
                throw McpError(McpErr::InvalidParam, "entity has no meshRenderer");
            const std::string slot = params.value("slot", std::string("albedo"));
            const u32 submesh = params.value("submesh", 0u);
            std::string rel = params.value("path", std::string());
            if (!rel.empty())
            {
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
                if (!fs::exists(fs::path(PathResolver::AssetsDir()) / rel))
                    throw McpError(McpErr::NotFound, "texture not found: " + rel);
            }
            auto& mr = reg.get<MeshRenderer>(e);
            // Material は同一モデルの全インスタンスで共有されるため直接触らず、
            // インスタンス単位の override に書く(描画側 EnsureMaterialOverrideSrv が合成)。
            if      (slot == "albedo")         MeshRenderer::SetOverride(mr.overrideAlbedoTexture, submesh, rel);
            else if (slot == "normal")         MeshRenderer::SetOverride(mr.overrideNormalTexture, submesh, rel);
            else if (slot == "metalRoughness") MeshRenderer::SetOverride(mr.overrideMetalRoughnessTexture, submesh, rel);
            else throw McpError(McpErr::InvalidParam, "slot must be albedo|normal|metalRoughness");
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"slot", slot},
                              {"submesh", submesh}, {"path", rel}};
        });

    // ════════════════════════════════════════════════════════════
    //  スケルタルアニメーション制御(Lua playAnim/playAnimByName と同じ経路)
    // ════════════════════════════════════════════════════════════
    McpDefine("play_anim", "blend:number,clip:int,clipName:any,entity:int,layer:int,loop:any,name:string,"
              "speed:any,state:any", DX12E_MCP_HANDLER
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<SkeletalAnimation>(e))
                throw McpError(McpErr::InvalidParam, "entity has no skeletalAnimation");
            auto& sa = reg.get<SkeletalAnimation>(e);
            if (!sa.animator) throw McpError(McpErr::Internal, "animator not initialized");
            const float blend = params.value("blend", 0.3f);

            // state を渡された場合は FSM の遷移（AnimatorController が必要）。
            // 渡さなければ従来どおり clip / clipName の CrossFadeTo（完全後方互換）。
            if (params.contains("state"))
            {
                const std::string want = params["state"].get<std::string>();
                if (!reg.all_of<AnimatorController>(e))
                    throw McpError(McpErr::InvalidParam, "entity has no animatorController");
                auto& ac = reg.get<AnimatorController>(e);
                if (!ac._state || !ac._state->valid)
                    throw McpError(McpErr::Internal,
                        "animatorController graph not loaded (graphPath='" + ac.graphPath + "')");
                const u32 layer = params.value("layer", 0u);
                if (!anim_graph::PlayState(*ac._state, layer, want, blend))
                    throw McpError(McpErr::NotFound,
                        "no state named '" + want + "' on layer " + std::to_string(layer)
                        + " (dx12_describe_anim_graph で一覧を確認)");
                resp["ok"] = true;
                resp["result"] = {{"entityId", static_cast<u32>(e)}, {"state", want},
                                  {"layer", layer}, {"blend", blend}};
            }
            else
            {
                int idx = -1;
                if (params.contains("clipName"))
                {
                    const std::string want = params["clipName"].get<std::string>();
                    for (int i = 0; i < static_cast<int>(sa.clips.size()); ++i)
                        if (sa.clips[i]->GetName() == want) { idx = i; break; }
                    if (idx < 0) throw McpError(McpErr::NotFound, "no clip named '" + want + "' (dx12_get_anim_state で一覧を確認)");
                }
                else
                {
                    idx = params.value("clip", 0);
                    if (idx < 0 || idx >= static_cast<int>(sa.clips.size()))
                        throw McpError(McpErr::InvalidParam, "clip index out of range (0.." +
                            std::to_string(sa.clips.empty() ? 0 : sa.clips.size() - 1) + ")");
                }
                sa.animator->CrossFadeTo(sa.clips[idx].get(), blend);
                if (params.contains("loop")) sa.animator->SetLooping(params["loop"].get<bool>());
                if (params.contains("speed")) sa.animator->SetSpeed(params["speed"].get<float>());
                resp["ok"] = true;
                resp["result"] = {{"entityId", static_cast<u32>(e)}, {"clip", idx},
                                  {"clipName", sa.clips[idx]->GetName()}, {"blend", blend},
                                  {"speed", sa.animator->GetSpeed()}};
            }
        });

    McpDefine("get_anim_state", "entity:int,name:string", DX12E_MCP_HANDLER
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            json result;
            result["hasSkeletalAnimation"] = reg.all_of<SkeletalAnimation>(e);
            json clips = json::array();
            if (reg.all_of<SkeletalAnimation>(e))
            {
                auto& sa = reg.get<SkeletalAnimation>(e);
                for (const auto& c : sa.clips) clips.push_back(c->GetName());
                if (sa.skeleton) result["boneCount"] = sa.skeleton->GetBoneCount();
                if (sa.animator)
                {
                    result["clipTime"]   = sa.animator->GetClipTime();
                    result["speed"]      = sa.animator->GetSpeed();
                    result["looping"]    = sa.animator->GetLooping();
                    result["blending"]   = sa.animator->IsBlending();
                    if (sa.animator->GetClip())
                        result["currentClip"] = sa.animator->GetClip()->GetName();
                }
            }
            result["clips"] = std::move(clips);

            // ---- AnimatorController（.animfsm）の状態 ----
            const bool hasCtrl = reg.all_of<AnimatorController>(e);
            result["hasController"] = hasCtrl;
            if (hasCtrl)
            {
                auto& ac = reg.get<AnimatorController>(e);
                result["graphPath"] = ac.graphPath;
                result["graphLoaded"] = (ac._state && ac._state->valid);
                if (ac._failed) result["graphError"] = "load or parse failed (see engine log)";
                if (ac._state && ac._state->valid)
                {
                    auto& sa = reg.get<SkeletalAnimation>(e);
                    json layers = json::array();
                    for (size_t li = 0; li < ac._state->layers.size(); ++li)
                    {
                        const auto& lr  = ac._state->layers[li];
                        const auto& def = ac._state->asset.layers[li];
                        json lj;
                        lj["name"]   = def.name;
                        lj["weight"] = lr.weight;
                        lj["state"]  = (lr.curState >= 0 && lr.curState < static_cast<i32>(def.states.size()))
                                     ? def.states[static_cast<size_t>(lr.curState)].name : std::string();
                        lj["normalizedTime"] = anim_graph::NormalizedTime(*ac._state, static_cast<u32>(li), sa.clips);
                        lj["transitioning"]  = lr.inTransition;
                        if (lr.inTransition && lr.transTo >= 0 && lr.transTo < static_cast<i32>(def.states.size()))
                        {
                            lj["transitionTo"] = def.states[static_cast<size_t>(lr.transTo)].name;
                            lj["transitionProgress"] = (lr.transDuration > 0.0f)
                                                     ? (lr.transElapsed / lr.transDuration) : 1.0f;
                        }
                        lj["masked"] = !lr.maskWeights.empty();
                        layers.push_back(std::move(lj));
                    }
                    result["layers"] = std::move(layers);

                    json ps = json::object();
                    for (const auto& [name, v] : ac._state->params)
                    {
                        if (v.type == AnimParamType::Float) ps[name] = v.f;
                        else                                ps[name] = v.b;
                    }
                    result["parameters"] = std::move(ps);
                }
            }

            // ---- FootIK（接地の破綻をスクショ無しで検知できるようにする）----
            if (reg.all_of<FootIK>(e))
            {
                const auto& ik = reg.get<FootIK>(e);
                json fj;
                fj["enabled"]       = ik.enabled;
                fj["weight"]        = ik.weight;
                fj["resolved"]      = ik._resolved;
                fj["resolveFailed"] = ik._resolveFailed;
                fj["bones"] = {{"leftHip", ik._lHip}, {"leftKnee", ik._lKnee}, {"leftFoot", ik._lFoot},
                               {"rightHip", ik._rHip}, {"rightKnee", ik._rKnee}, {"rightFoot", ik._rFoot},
                               {"pelvis", ik._pelvis}};
                if (ik._resolved && reg.all_of<SkeletalAnimation>(e))
                {
                    const auto& sk = reg.get<SkeletalAnimation>(e).skeleton;
                    auto boneName = [&](i32 b) -> std::string {
                        return (sk && b >= 0 && static_cast<u32>(b) < sk->GetBoneCount())
                             ? sk->GetBone(static_cast<u32>(b)).name : std::string();
                    };
                    fj["boneNames"] = {{"leftHip", boneName(ik._lHip)}, {"leftKnee", boneName(ik._lKnee)},
                                       {"leftFoot", boneName(ik._lFoot)}, {"rightHip", boneName(ik._rHip)},
                                       {"rightKnee", boneName(ik._rKnee)}, {"rightFoot", boneName(ik._rFoot)},
                                       {"pelvis", boneName(ik._pelvis)}};
                }
                fj["leftContact"]  = ik._lContact;
                fj["rightContact"] = ik._rContact;
                fj["leftWeight"]   = ik._lWeight;
                fj["rightWeight"]  = ik._rWeight;
                fj["leftLift"]     = ik._lLift;
                fj["rightLift"]    = ik._rLift;
                fj["pelvisOffset"] = ik._pelvisDrop;
                fj["leftNormal"]   = json::array({ik._lNormal.x, ik._lNormal.y, ik._lNormal.z});
                fj["rightNormal"]  = json::array({ik._rNormal.x, ik._rNormal.y, ik._rNormal.z});
                result["footIK"] = std::move(fj);
            }

            resp["ok"] = true;
            resp["result"] = std::move(result);
        });

    McpDefine("set_anim_param", "entity:int,name:string,param:string,trigger:bool,value:any", DX12E_MCP_HANDLER
        {
            // ★このメソッドだけ 'name' が二重の意味を持っていた（エンティティ名 / FSM パラメータ名）。
            //   ResolveMcpEntity は params["name"] を最優先でエンティティ名として引くので、
            //   {entity:7, name:"Speed"} が必ず "no entity named 'Speed'" で落ちていた。
            //   → パラメータ名は 'param' を正とし、'name' は後方互換のフォールバックにする。
            //     エンティティ解決は「entity(id) が有効ならそれを使い、無ければ従来どおり
            //     ResolveMcpEntity（= name をエンティティ名として引く）」。
            //     こうすると {entity, name} も {name, param} も {entity, param} も全部通る。
            auto& reg = m_scene->GetRegistry();
            entt::entity e = entt::null;
            {
                const auto idParam = static_cast<entt::entity>(params.value("entity", 0xFFFFFFFFu));
                if (params.contains("entity") && reg.valid(idParam)) e = idParam;
                else                                                e = ResolveMcpEntity(*m_scene, params);
            }
            if (!reg.all_of<AnimatorController>(e))
                throw McpError(McpErr::InvalidParam, "entity has no animatorController");
            auto& ac = reg.get<AnimatorController>(e);
            if (!ac._state || !ac._state->valid)
                throw McpError(McpErr::Internal,
                    "animatorController graph not loaded (graphPath='" + ac.graphPath + "')");
            std::string name = params.value("param", std::string());
            if (name.empty()) name = params.value("name", std::string());
            if (name.empty())
                throw McpError(McpErr::InvalidParam, "missing 'param' (FSM パラメータ名)",
                    "param にパラメータ名、entity か name でエンティティを指定する");
            auto it = ac._state->params.find(name);
            if (it == ac._state->params.end())
                throw McpError(McpErr::NotFound,
                    "no parameter named '" + name + "' (dx12_describe_anim_graph で一覧を確認)");

            if (params.value("trigger", false))
            {
                it->second.b = true;
            }
            else if (params.contains("value"))
            {
                const json& v = params["value"];
                if (v.is_boolean())     it->second.b = v.get<bool>();
                else if (v.is_number()) it->second.f = v.get<float>();
                else throw McpError(McpErr::InvalidParam, "value must be a number or a boolean");
            }
            else
            {
                throw McpError(McpErr::InvalidParam, "either 'value' or 'trigger' is required");
            }

            json out;
            out["entityId"] = static_cast<u32>(e);
            out["param"] = name;
            out["name"]  = name;   // 後方互換（TS 側が 'param' へ移るまで）
            if (it->second.type == AnimParamType::Float) out["value"] = it->second.f;
            else                                         out["value"] = it->second.b;
            resp["ok"] = true;
            resp["result"] = std::move(out);
        });

    McpDefine("describe_anim_graph", "entity:int,name:string,path:any", DX12E_MCP_HANDLER
        {
            // entity 指定ならロード済みのグラフを、path 指定なら .animfsm を直接読んで返す。
            AnimGraphAsset asset;
            std::string source;
            if (params.contains("path"))
            {
                source = params["path"].get<std::string>();
                const std::vector<uint8_t> bytes = vfs::ReadAsset(source);
                if (bytes.empty()) throw McpError(McpErr::NotFound, "cannot read " + source);
                std::string err;
                if (!ParseAnimGraphAsset(bytes, asset, err))
                    throw McpError(McpErr::InvalidParam, "invalid .animfsm: " + err);
            }
            else
            {
                const auto e = ResolveMcpEntity(*m_scene, params);
                auto& reg = m_scene->GetRegistry();
                if (!reg.all_of<AnimatorController>(e))
                    throw McpError(McpErr::InvalidParam, "entity has no animatorController");
                auto& ac = reg.get<AnimatorController>(e);
                if (!ac._state || !ac._state->valid)
                    throw McpError(McpErr::Internal,
                        "animatorController graph not loaded (graphPath='" + ac.graphPath + "')");
                asset  = ac._state->asset;
                source = ac.graphPath;
            }
            json result;
            result["source"] = source;
            result["graph"]  = json::parse(SerializeAnimGraphAsset(asset));
            resp["ok"] = true;
            resp["result"] = std::move(result);
        });

    // ════════════════════════════════════════════════════════════
    //  マルチプレイヤー(フェーズ⑨のローカルテストループを AI から回す)
    // ════════════════════════════════════════════════════════════
    McpDefine("net_status", "", DX12E_MCP_HANDLER
        {
            json result;
            result["available"] = (m_networkSystem != nullptr);
            if (m_networkSystem)
            {
                const char* role = m_networkSystem->IsServer() ? "Host"
                                 : m_networkSystem->IsClient() ? "Client" : "Offline";
                result["role"] = role;
                result["isConnected"] = m_networkSystem->IsConnected();
                result["localClientId"] = m_networkSystem->LocalClientId();
                result["tick"] = m_networkSystem->CurrentTick();
                result["syncedEntityCount"] = m_networkSystem->SyncedEntityCount(m_scene->GetRegistry());
                json players = json::array();
                for (const auto& p : m_networkSystem->Players())
                    players.push_back({{"id", p.id}, {"rttMs", p.rttMs},
                                       {"bytesSent", p.bytesSent}, {"bytesReceived", p.bytesReceived}});
                result["players"] = std::move(players);
                const auto& cfg = m_networkSystem->Config();
                result["config"] = {{"tickRate", cfg.tickRate}, {"snapshotRate", cfg.snapshotRate},
                                    {"maxPlayers", cfg.maxPlayers}, {"defaultPort", cfg.defaultPort}};
            }
            const char* testRole = m_editorCtx->netTestRole == NetTestRole::Host ? "host"
                                 : m_editorCtx->netTestRole == NetTestRole::Client ? "client" : "offline";
            result["testRole"] = testRole;
            result["testJoinAddress"] = m_editorCtx->netTestJoinAddress;
            resp["ok"] = true;
            resp["result"] = std::move(result);
        });

    McpDefine("net_setup", "address:any,port:int,role:string", DX12E_MCP_HANDLER
        {
            // ツールバーの Play ロールドロップダウンと同じ状態を書く。次の play で
            // EnterPlayMode が Host/Join を自動実行する(直接 Host/Join は EnterPlayMode の
            // イベント順序保証を壊すのでやらない)。
            const std::string role = params.value("role", std::string());
            if (role == "host")         m_editorCtx->netTestRole = NetTestRole::Host;
            else if (role == "client")  m_editorCtx->netTestRole = NetTestRole::Client;
            else if (role == "offline") m_editorCtx->netTestRole = NetTestRole::Offline;
            else throw McpError(McpErr::InvalidParam, "role must be host|client|offline");
            if (params.contains("address")) m_editorCtx->netTestJoinAddress = params["address"].get<std::string>();
            const int port = params.value("port", 0);
            if (port < 0 || port > 65535) throw McpError(McpErr::InvalidParam, "port must be 0..65535");
            m_editorCtx->netTestJoinPort = static_cast<u16>(port);
            resp["ok"] = true;
            resp["result"] = {{"testRole", role}, {"address", m_editorCtx->netTestJoinAddress},
                              {"port", m_editorCtx->netTestJoinPort}};
        });

    McpDefine("net_launch_test_client", "", DX12E_MCP_HANDLER
        {
            if (!m_networkSystem || !m_networkSystem->IsServer())
                throw McpError(McpErr::ModeConflict,
                    "not hosting (dx12_net_setup role=host → dx12_play してからテストクライアントを起動する)");
            // ツールバーの「テストクライアント起動」ボタンと同じ: フレーム境界で CreateProcess。
            m_editorCtx->netTestLaunchClientRequested = true;
            resp["ok"] = true;
            resp["result"] = {{"requested", true},
                              {"note", "second engine process launches at next frame boundary and auto-joins 127.0.0.1"}};
        });
}



} // namespace dx12e
