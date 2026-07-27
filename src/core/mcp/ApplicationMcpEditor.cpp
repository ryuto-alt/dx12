// ===========================================================================
// MCP: エディタ操作（設定 / Play / 入力 / スクショ / 計測）
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// method の足し方は本ファイル内 McpDefine の並びに倣う（作法は ApplicationInternal.h の DX12E_MCP_HANDLER 付近）。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;


// ---- エディタ操作（設定・Play/Stop・入力・スクショ・計測・物理クエリ） ----
void Application::RegisterMcpEditorMethods()
{
    using json = nlohmann::json;
    namespace fs = std::filesystem;

    // ディスパッチ表そのものを機械可読で返す（#20-7）。TS 側が Application.cpp を
    // テキスト解析しなくてもスキーマのドリフトを検出できるようにするための唯一の入口。
    // 表が正なので「実装にあるのに describe に出ない」は原理的に起きない。
    McpDefine("describe_mcp_params", "method:string", DX12E_MCP_HANDLER
        {
            const std::string only = params.value("method", std::string());
            json methods = json::object();
            for (const auto& [name, entry] : m_mcpMethods)
            {
                if (!only.empty() && name != only) continue;
                json keys = json::array();
                const std::string spec(entry.paramSpec ? entry.paramSpec : "");
                size_t pos = 0;
                while (pos <= spec.size() && !spec.empty())
                {
                    const size_t comma = spec.find(',', pos);
                    const std::string one =
                        spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                    if (!one.empty())
                    {
                        const size_t colon = one.find(':');
                        keys.push_back({{"key",  one.substr(0, colon)},
                                        {"type", colon == std::string::npos ? std::string("any")
                                                                            : one.substr(colon + 1)}});
                    }
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
                methods[name] = std::move(keys);
            }
            if (!only.empty() && methods.empty())
                throw McpError(McpErr::InvalidParam, "unknown method: " + only,
                               "method を省くと全 method を返す");
            resp["ok"] = true;
            resp["result"] = {{"methods", methods},
                              {"count", methods.size()},
                              {"globalKeys", json::array({"idempotency_key"})},
                              {"note", "type は bool/int/number/string/vec3/object/any。"
                                       "\"親.子\" は入れ子オブジェクトのキー（例 skybox.envMapPath）。"
                                       "any は C++ 側で型を静的に決められなかったもので、値の型制約が無い意味ではない"}};
        });

    McpDefine("get_scene_settings", "", DX12E_MCP_HANDLER
        {
            const auto& sky = m_scene->GetSkyboxSettings();
            resp["ok"] = true;
            resp["result"] = {{"skybox", {
                                  {"envMapPath", sky.envMapPath}, {"iblIntensity", sky.iblIntensity},
                                  {"skyboxIntensity", sky.skyboxIntensity}, {"drawSkybox", sky.drawSkybox}}},
                              {"note", "post-process は dx12_get_post_process、SSAO は dx12_get_ssao、SSR は dx12_get_ssr、SSGI は dx12_get_ssgi、ボリュメトリックフォグは dx12_get_volumetric_fog を使う"}};
        });

    McpDefine("set_scene_settings", "skybox:object,skybox.drawSkybox:any,skybox.envMapPath:any,skybox.iblIntensity:any,"
              "skybox.skyboxIntensity:any", DX12E_MCP_HANDLER
        {
            const json sky = params.value("skybox", json::object());
            auto& s = m_scene->GetSkyboxSettings();
            bool envChanged = false;
            if (sky.contains("envMapPath"))
            {
                std::string p = sky["envMapPath"].get<std::string>();
                if (!p.empty() && (p.front() == '/' || p.find('\\') != std::string::npos ||
                    p.find(':') != std::string::npos || p.find("..") != std::string::npos))
                    throw McpError(McpErr::InvalidParam, "invalid envMapPath (assets 相対のみ)");
                if (p != s.envMapPath) { s.envMapPath = p; envChanged = true; }
            }
            if (sky.contains("iblIntensity"))    s.iblIntensity    = sky["iblIntensity"].get<float>();
            if (sky.contains("skyboxIntensity")) s.skyboxIntensity = sky["skyboxIntensity"].get<float>();
            if (sky.contains("drawSkybox"))      s.drawSkybox      = sky["drawSkybox"].get<bool>();
            if (envChanged) { m_loadedSkyboxPath.clear(); m_skyboxDirty = true; }  // 環境マップ再ベイク要求
            resp["ok"] = true;
            resp["result"] = {{"applied", true}, {"envMapRebake", envChanged}};
        });

    McpDefine("play", "", DX12E_MCP_HANDLER
        {
            if (m_engineMode == EngineMode::Playing)
            {
                resp["ok"] = true;
                resp["result"] = {{"mode", "Playing"}, {"sceneGeneration", m_sceneGeneration}};
            }
            else
            {
                // 単一スロット: 既にモード遷移待ちなら 2件目を弾く(上書きで1件目が宙吊りになるのを防ぐ)。
                if (m_mcpModeReply.client != 0)
                    throw McpError(McpErr::ModeConflict, "a mode change is already pending; retry shortly");
                // 実切替は EnterPlayMode(snapshot/script init/GPU) を伴うためフレーム境界で遅延。
                // 遷移確定後に Run() のモード応答ブロックが本物のモード(or 失敗)を返す。
                m_pendingMode = EngineMode::Playing;
                m_modeChangeRequested = true;
                m_mcpModeReply = deferred;
                isDeferred = true;
            }
        });

    McpDefine("stop", "", DX12E_MCP_HANDLER
        {
            if (m_engineMode == EngineMode::Editor)
            {
                resp["ok"] = true;
                resp["result"] = {{"mode", "Editor"}, {"sceneGeneration", m_sceneGeneration}};
            }
            else
            {
                if (m_mcpModeReply.client != 0)
                    throw McpError(McpErr::ModeConflict, "a mode change is already pending; retry shortly");
                m_pendingMode = EngineMode::Editor;
                m_modeChangeRequested = true;     // 次フレームで EnterEditorMode()(snapshot 復元)
                m_mcpModeReply = deferred;
                isDeferred = true;
            }
        });

    McpDefine("get_mode", "", DX12E_MCP_HANDLER
        {
            resp["ok"] = true;
            resp["result"] = {{"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"}};
        });

    McpDefine("get_log", "lines:int", DX12E_MCP_HANDLER
        {
            int lines = params.value("lines", 50);
            if (lines < 1) lines = 1;
            // Logger は CWD の "dx12_engine.log" へ出力。末尾 N 行を返すだけ(リングは足さない)。
            std::ifstream ifs("dx12_engine.log", std::ios::binary);
            json arr = json::array();
            if (ifs)
            {
                std::vector<std::string> all;
                std::string ln;
                while (std::getline(ifs, ln))
                {
                    if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                    all.push_back(ln);
                }
                size_t start = all.size() > static_cast<size_t>(lines) ? all.size() - static_cast<size_t>(lines) : 0;
                for (size_t i = start; i < all.size(); ++i) arr.push_back(all[i]);
            }
            resp["ok"] = true;
            resp["result"] = arr;   // ファイル無しは空配列(grace)
        });

    McpDefine("screenshot", "deterministic:bool,path:string,settleFrames:int", DX12E_MCP_HANDLER
        {
            // 直近フレームのシーン描画を PNG にして絶対パスを返す。AI 側はそのパスを画像として読む。
            // ★これは【ポスト前】の m_sceneRT。グレーディング/ブルーム/ビネット/TAA は写らない（§6 B5）。
            //   見た目を判断したいときは dx12_screenshot_final を使うこと。
            if (params.value("deterministic", false))
            {
                if (m_deterministicCapture || m_mcpFinalShot.reply.client != 0)
                    throw McpError(McpErr::ModeConflict,
                        "another deterministic screenshot is in flight; retry shortly");
                m_mcpFinalShot = {};
                m_mcpFinalShot.path          = params.value("path", std::string());
                m_mcpFinalShot.reply         = deferred;
                m_mcpFinalShot.wantSceneRt   = true;
                m_mcpFinalShot.deterministic = true;
                // ★履歴を捨ててから固定フレーム数だけ回す（#31）。
                //   位相を固定しただけでは「ピンポンの偶奇」と「撮る前に何フレーム回っていたか」で
                //   結果がわずかに残るので（実測 240 フレーム回しても 1% 残った）、
                //   **同じ初期状態 + 同じフレーム数** にして完全に再現させる。
                InvalidateTemporalHistory();
                m_deterministicFramesLeft = std::clamp(params.value("settleFrames", 8), 1, 240);
                m_deterministicCapture    = true;
                isDeferred = true;
                return;
            }
            std::string serr;
            const std::string path = CaptureSceneScreenshot(serr, params.value("path", std::string()));
            if (path.empty()) throw std::runtime_error(serr.empty() ? "screenshot failed" : serr);
            resp["ok"] = true;
            resp["result"] = {{"path", path},
                              {"width", m_sceneRT->GetWidth()},
                              {"height", m_sceneRT->GetHeight()},
                              {"source", "sceneRT(pre-post)"},
                              // ★#16: シーン RT は「レンダー解像度」そのもの。renderScale<1 なら
                              //   表示解像度より小さい絵が返る（拡大前）。表示解像度で見たいなら final。
                              {"renderScale", m_renderScale},
                              {"note", "ポストプロセス前のシーン RT（レンダー解像度）。グレーディング/"
                                       "ブルーム/ビネット/TAA は写らない。最終画は dx12_screenshot_final"}};
        });

    // ★§6 B5 の根治。バックバッファ（ポスト適用後の最終画）のビューポート矩形を撮る。
    //   ImGui を描く前にコピーするのでエディタのパネルは写らない＝ゲームと同じ絵になる。
    //   1 フレーム描いてから撮るので遅延応答。
    McpDefine("screenshot_final", "deterministic:bool,path:string,settleFrames:int", DX12E_MCP_HANDLER
        {
            if (m_mcpFinalShot.reply.client != 0 || m_mcpFinalShot.pending || m_deterministicCapture)
                throw McpError(McpErr::ModeConflict,
                    "a final screenshot is already pending; retry shortly");
            const bool det = params.value("deterministic", false);
            m_mcpFinalShot = {};
            m_mcpFinalShot.path          = params.value("path", std::string());
            m_mcpFinalShot.reply         = deferred;
            m_mcpFinalShot.deterministic = det;
            if (det)
            {
                // ★#31: time / TAA ジッタ / フォグ・SSGI の位相を固定して N フレーム回し、
                //   時間蓄積が収束してから撮る。pending は収束後に Run ループが立てる。
                // ★履歴を捨ててから固定フレーム数だけ回す（#31）。
                //   位相を固定しただけでは「ピンポンの偶奇」と「撮る前に何フレーム回っていたか」で
                //   結果がわずかに残るので（実測 240 フレーム回しても 1% 残った）、
                //   **同じ初期状態 + 同じフレーム数** にして完全に再現させる。
                InvalidateTemporalHistory();
                m_deterministicFramesLeft = std::clamp(params.value("settleFrames", 8), 1, 240);
                m_deterministicCapture    = true;
            }
            else
            {
                m_mcpFinalShot.pending = true;   // 次に描くフレームの ImGui 直前でコピーされる
            }
            isDeferred = true;
        });

    McpDefine("ui_screenshot", "", DX12E_MCP_HANDLER
        {
            // エディタウィンドウ全体(ImGui パネル込み = UIエディタ/ゲーム内 UI プレビューが写る)を
            // PNG にして返す。scene RT には ImGui 描画が乗らないため screenshot とは別経路。
            std::string serr;
            const std::string path = CaptureWindowScreenshot(m_window ? m_window->GetHwnd() : nullptr, serr);
            if (path.empty()) throw std::runtime_error(serr.empty() ? "ui_screenshot failed" : serr);
            RECT rc{};
            GetClientRect(m_window->GetHwnd(), &rc);
            resp["ok"] = true;
            resp["result"] = {{"path", path},
                              {"width", rc.right - rc.left}, {"height", rc.bottom - rc.top},
                              {"note", "editor window capture (includes UI editor panel & game UI preview)"}};
        });

    McpDefine("project_world_to_screen", "entity:int,name:string", DX12E_MCP_HANDLER
        {
            // エンティティのワールド座標を、今シーンビューを描いているカメラ(m_camera)の
            // ビュー*射影で画面ピクセルへ投影する。Playing 中は m_camera = アクティブなゲームカメラ
            // なので「ゲーム画面で player が中央/画面内か」を数値で確認できる(screenshot と整合)。
            using namespace DirectX;
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            XMVECTOR wpos = XMVectorSetW(ComputeWorldMatrix(reg, e).r[3], 1.0f);
            XMVECTOR clip = XMVector4Transform(wpos, m_camera->GetViewProjMatrix());
            const float w = XMVectorGetW(clip);
            const float ndcX = (w != 0.0f) ? XMVectorGetX(clip) / w : 0.0f;
            const float ndcY = (w != 0.0f) ? XMVectorGetY(clip) / w : 0.0f;
            const float ndcZ = (w != 0.0f) ? XMVectorGetZ(clip) / w : 0.0f;
            // ★#16: シーンは RT 全面に描かれるので、RT サイズ＝スクリーン空間そのもの
            //   （かつては RT がウィンドウ全面でシーンはサブ矩形だったため、
            //     エディタでは vpLeft ぶん系統的にずれていた）。
            const float vw = static_cast<float>(m_sceneRT->GetWidth());
            const float vh = static_cast<float>(m_sceneRT->GetHeight());
            const float px = (ndcX * 0.5f + 0.5f) * vw;
            const float py = (0.5f - ndcY * 0.5f) * vh;   // NDC +Y up → ピクセル +Y down
            const bool visible = (w > 0.0f) && ndcX >= -1.0f && ndcX <= 1.0f &&
                                 ndcY >= -1.0f && ndcY <= 1.0f && ndcZ >= 0.0f && ndcZ <= 1.0f;
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"x", px}, {"y", py},
                              {"visible", visible}, {"depth", ndcZ}, {"w", w},
                              {"width", m_sceneRT->GetWidth()}, {"height", m_sceneRT->GetHeight()},
                              {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"}};
        });

    McpDefine("get_lua_component_state", "entity:int,name:string", DX12E_MCP_HANDLER
        {
            // LuaScript の現在のプロパティ値(オーバーライド+スキーマ既定)を全部返す。
            // get_entity は保存済みオーバーライドしか出さないので、スキーマを基準に既定も含めて出す。
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<LuaScript>(e)) throw McpError(McpErr::NotFound, "entity has no LuaScript");
            const auto& ls = reg.get<LuaScript>(e);
            const auto& schema = m_scriptEngine->GetPropertySchema(ls.scriptPath);
            auto typeStr = [](ScriptPropType t) -> const char* {
                switch (t) {
                    case ScriptPropType::Int:    return "int";
                    case ScriptPropType::Bool:   return "bool";
                    case ScriptPropType::String: return "string";
                    case ScriptPropType::Vec3:   return "vec3";
                    case ScriptPropType::Color:  return "color";
                    case ScriptPropType::Entity: return "entity";
                    default:                     return "float";
                } };
            auto emitVal = [](json& pj, const ScriptProp& v) {
                switch (v.type) {
                    case ScriptPropType::Int:    pj["value"] = static_cast<long long>(v.num); break;
                    case ScriptPropType::Bool:   pj["value"] = v.b; break;
                    case ScriptPropType::String:
                    case ScriptPropType::Entity: pj["value"] = v.str; break;
                    case ScriptPropType::Vec3:
                    case ScriptPropType::Color:  pj["value"] = json::array({v.vec.x, v.vec.y, v.vec.z}); break;
                    default:                     pj["value"] = v.num; break;
                } };
            json props = json::array();
            for (const auto& d : schema)
            {
                const ScriptProp* ov = nullptr;
                for (const auto& p : ls.props) if (p.name == d.name) { ov = &p; break; }
                ScriptProp v = ov ? *ov : d.def;
                v.type = d.type;   // オーバーライドの型がズレてても schema を正とする
                json pj{{"name", d.name}, {"type", typeStr(d.type)}, {"isOverride", ov != nullptr}};
                emitVal(pj, v);
                props.push_back(std::move(pj));
            }
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"scriptPath", ls.scriptPath},
                              {"enabled", ls.enabled}, {"started", ls.started},
                              {"loadError", ls.loadError}, {"errorMessage", ls.errorMessage},
                              {"properties", std::move(props)}};
        });

    McpDefine("set_lua_property", "entity:int,key:string,name:string,value:any", DX12E_MCP_HANDLER
        {
            // LuaScript のプロパティを1つ書き換える。スキーマで型を確認して検証。
            // 実行中(Playing)なら ReloadScript で再注入、Editor では保存だけ(次 Play で反映)。
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<LuaScript>(e)) throw McpError(McpErr::NotFound, "entity has no LuaScript");
            const std::string key = params.value("key", std::string());
            if (key.empty()) throw McpError(McpErr::InvalidParam, "missing 'key'");
            if (!params.contains("value")) throw McpError(McpErr::InvalidParam, "missing 'value'");
            const json& value = params["value"];
            auto& ls = reg.get<LuaScript>(e);
            const auto& schema = m_scriptEngine->GetPropertySchema(ls.scriptPath);
            const ScriptPropDef* def = nullptr;
            for (const auto& d : schema) if (d.name == key) { def = &d; break; }
            if (!def) throw McpError(McpErr::InvalidParam,
                "unknown property '" + key + "' (script の properties に未宣言。dx12_get_lua_component_state で確認)");
            ScriptProp* p = nullptr;
            for (auto& ex : ls.props) if (ex.name == key) { p = &ex; break; }
            if (!p) { ls.props.push_back(def->def); p = &ls.props.back(); }
            p->type = def->type;
            switch (def->type)
            {
            case ScriptPropType::Float:
            case ScriptPropType::Int:
                if (!value.is_number()) throw McpError(McpErr::InvalidParam, "value must be a number");
                p->num = value.get<double>(); break;
            case ScriptPropType::Bool:
                if (!value.is_boolean()) throw McpError(McpErr::InvalidParam, "value must be a bool");
                p->b = value.get<bool>(); break;
            case ScriptPropType::String:
            case ScriptPropType::Entity:
                if (!value.is_string()) throw McpError(McpErr::InvalidParam, "value must be a string");
                p->str = value.get<std::string>(); break;
            case ScriptPropType::Vec3:
            case ScriptPropType::Color:
            {
                if (!value.is_array() || value.size() != 3)
                    throw McpError(McpErr::InvalidParam, "value must be [x,y,z]");
                auto a = value.get<std::vector<float>>();
                p->vec = { a[0], a[1], a[2] }; break;
            }
            }
            if (ls.started) m_scriptEngine->ReloadScript(e);  // 実行中のみ再注入(Editor は保存のみ)
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"key", key}, {"reloaded", ls.started}};
        });

    McpDefine("key_down", "key:string", DX12E_MCP_HANDLER
        {
            // 合成キー押下(押しっぱなし)。次フレーム以降の Lua input:isKeyDown / keyDown() が true になる。
            // key_up を呼ぶまで保持。Playing 中の移動などの確認用(isAsyncKeyDown 系には効かない)。
            const int vk = ParseMcpVk(params);
            m_inputSystem->OnKeyDown(vk);
            resp["ok"] = true;
            resp["result"] = {{"key", vk}, {"down", true}};
        });

    McpDefine("key_up", "key:string", DX12E_MCP_HANDLER
        {
            const int vk = ParseMcpVk(params);
            m_inputSystem->OnKeyUp(vk);
            resp["ok"] = true;
            resp["result"] = {{"key", vk}, {"down", false}};
        });

    McpDefine("key_press", "key:string", DX12E_MCP_HANDLER
        {
            // 1フレームだけ押す(isKeyPressed が立つ)。ジャンプ等のタップ操作の確認用。
            const int vk = ParseMcpVk(params);
            m_inputSystem->InjectKeyPress(vk);
            resp["ok"] = true;
            resp["result"] = {{"key", vk}, {"pressed", true}};
        });

    McpDefine("render_debug", "depthRange:number,exposure:number,frames:int,gain:number,mode:string", DX12E_MCP_HANDLER
        {
            // 中間バッファ可視化。mode を受けて必要な機能を一時的に ON にし、N フレーム描いてから
            // スクリーンショットを撮って返し、設定を元へ戻す（＝呼ぶ前と完全に同じ状態に戻る）。
            //
            // ★実装は 2 系統に分かれる（重複実装を避けるため）:
            //   (A) 専用フルスクリーンパス RenderDebugPass … 可視化が無かったバッファ
            //   (B) 既存のデバッグ表示トグルへの振り分け     … 既に実装済みのもの
            //       shadowCascade = shadowParams.w / lightComplexity,clusterGrid,decalCount =
            //       clusterExtra.z / fog* = FogParams.gMisc.z
            if (m_mcpRenderDebugReply.client != 0)
                throw McpError(McpErr::ModeConflict, "a render_debug capture is already pending; retry shortly");
            if (!m_scene) throw McpError(McpErr::Internal, "no scene");

            const std::string mode = params.value("mode", std::string());
            // (mode, パスモード(0=既存トグル), 説明)
            struct DbgEntry { const char* name; u32 passMode; };
            static const DbgEntry kEntries[] = {
                {"off",              0},
                {"normal",           static_cast<u32>(RenderDebugMode::Normal)},
                {"roughness",        static_cast<u32>(RenderDebugMode::Roughness)},
                {"metallic",         static_cast<u32>(RenderDebugMode::Metallic)},
                {"depth",            static_cast<u32>(RenderDebugMode::Depth)},
                {"ao",               static_cast<u32>(RenderDebugMode::Ao)},
                {"contactShadow",    static_cast<u32>(RenderDebugMode::ContactShadow)},
                {"velocity",         static_cast<u32>(RenderDebugMode::Velocity)},
                {"ssr",              static_cast<u32>(RenderDebugMode::Ssr)},
                {"ssgi",             static_cast<u32>(RenderDebugMode::Ssgi)},
                {"rt",               static_cast<u32>(RenderDebugMode::RtHit)},
                {"rtDiff",           static_cast<u32>(RenderDebugMode::RtDiff)},
                {"shadowCascade",    0},
                {"lightComplexity",  0},
                {"clusterGrid",      0},
                {"decalCount",       0},
                {"fogScattering",    0},
                {"fogTransmittance", 0},
                {"fogSlice",         0},
            };
            const DbgEntry* entry = nullptr;
            for (const DbgEntry& e : kEntries)
                if (mode == e.name) { entry = &e; break; }
            if (!entry)
            {
                std::string list;
                for (const DbgEntry& e : kEntries) { if (!list.empty()) list += ", "; list += e.name; }
                throw McpError(McpErr::InvalidParam, "unknown mode '" + mode + "'",
                    "有効な mode: " + list +
                    "。albedo と overdraw は非対応（前方レンダラなので albedo の G-Buffer が無く、"
                    "overdraw は加算カウント用の専用パスが要るため）");
            }

            // ---- 現在の状態を退避（返す直前に必ず戻す）----
            auto& taaS  = m_scene->GetTaaSettings();
            auto& ssaoS = m_scene->GetSSAOSettings();
            auto& csS   = m_scene->GetContactShadowSettings();
            auto& ssrS  = m_scene->GetSsrSettings();
            auto& ssgiS = m_scene->GetSsgiSettings();
            auto& fogS  = m_scene->GetVolumetricFogSettings();
            m_renderDebugRestore.valid         = true;
            m_renderDebugRestore.taa           = taaS.enabled;
            m_renderDebugRestore.ssao          = ssaoS.enabled;
            m_renderDebugRestore.contactShadow = csS.enabled;
            m_renderDebugRestore.ssr           = ssrS.enabled;
            m_renderDebugRestore.ssgi          = ssgiS.enabled;
            m_renderDebugRestore.clusterDebug  = m_editorCtx ? m_editorCtx->clusterDebugMode : 0u;
            m_renderDebugRestore.cascadeDebug  = m_showCascadeDebug;
            m_renderDebugRestore.fogDebug      = fogS.debugMode;

            // ---- 必要な機能を一時的に ON ----
            json warn = json::array();
            m_renderDebugMode        = entry->passMode;
            m_renderDebugModeName    = mode;
            m_renderDebugGain        = static_cast<f32>(params.value("gain", 1.0));
            m_renderDebugDepthRange  = McpFloatParam(params, "depthRange", 100.0f, 0.1f, 100000.0f);
            m_renderDebugExposure    = McpFloatParam(params, "exposure", 1.0f, 0.001f, 1000.0f);
            m_renderDebugRawReadback = (entry->passMode != 0);

            if (mode == "normal" || mode == "roughness" || mode == "metallic" || mode == "velocity")
            {
                // G-Buffer / 速度バッファは深度プリパスの MRT にしか書かれない
                //（＝TAA か SSR/SSGI が有効なときだけ）。
                if (!taaS.enabled && !ssrS.enabled && !ssgiS.enabled)
                {
                    taaS.enabled = true;
                    warn.push_back("G-Buffer/速度は速度プリパスでしか書かれないので TAA を一時的に ON にした");
                }
            }
            else if (mode == "ao")
            {
                if (!ssaoS.enabled) { ssaoS.enabled = true; warn.push_back("SSAO を一時的に ON にした"); }
            }
            else if (mode == "contactShadow")
            {
                if (!csS.enabled) { csS.enabled = true; warn.push_back("コンタクトシャドウを一時的に ON にした"); }
            }
            else if (mode == "ssr")
            {
                if (!ssrS.enabled) { ssrS.enabled = true; warn.push_back("SSR を一時的に ON にした（時間蓄積があるので frames を増やすと安定する）"); }
            }
            else if (mode == "ssgi")
            {
                if (!ssgiS.enabled) { ssgiS.enabled = true; warn.push_back("SSGI を一時的に ON にした（同上）"); }
            }
            else if (mode == "rt" || mode == "rtDiff")
            {
                // TLAS を建てさせる（RT 影 / RT-AO が両方 OFF でも見られるようにする）。
                // forceBuildTlas はシーン JSON に保存しない一時トグル。
                if (!m_dxrEnabled)
                    warn.push_back("この GPU では inline raytracing が使えないので何も出ない"
                                   "（要 DXR Tier 1.1 / Shader Model 6.5）");
                m_renderDebugRestore.rtForceTlas = m_scene->GetRtSettings().forceBuildTlas;
                m_scene->GetRtSettings().forceBuildTlas = true;
                if (mode == "rtDiff")
                    warn.push_back("スキンドと半透明は TLAS に入らない仕様なので、そこはマゼンタになる");
            }
            else if (mode == "shadowCascade")
            {
                m_showCascadeDebug = true;
            }
            else if (mode == "lightComplexity" || mode == "clusterGrid" || mode == "decalCount")
            {
                if (!m_editorCtx)
                    throw McpError(McpErr::Internal, "editor context not available");
                m_editorCtx->clusterDebugMode = (mode == "lightComplexity") ? 1u
                                              : (mode == "clusterGrid")     ? 2u : 3u;
                if (!m_clusteredEnabled)
                    warn.push_back("クラスタードライティングが無効（settings.json render_clustered=0 / 正射カメラ）なので何も出ない");
                if (mode == "decalCount")
                {
                    u32 nDecals = 0;
                    for (auto de : m_scene->GetRegistry().view<const DecalComponent>()) { (void)de; ++nDecals; }
                    if (nDecals == 0)
                        warn.push_back("シーンにデカールが 1 枚も無いので全面がほぼ黒になる");
                }
            }
            else if (mode == "fogScattering" || mode == "fogTransmittance" || mode == "fogSlice")
            {
                if (!fogS.enabled)
                    warn.push_back("ボリュメトリックフォグが無効なので何も出ない（先に dx12_set_volumetric_fog で ON にすること）");
                fogS.debugMode = (mode == "fogScattering") ? 1 : (mode == "fogTransmittance") ? 2 : 3;
            }
            else if (mode == "off")
            {
                // 何も ON にしない（退避した値をそのまま書き戻して終わる）
            }

            int frames = params.value("frames", 3);
            frames = std::clamp(frames, 1, 120);
            m_mcpRenderDebugFramesLeft = frames;
            m_mcpRenderDebugReply      = deferred;
            m_renderDebugWarnings      = warn.dump();
            isDeferred = true;
        });

    McpDefine("step_frames", "frames:any,n:int", DX12E_MCP_HANDLER
        {
            // N フレーム進めてから応答する同期バリア(遅延応答)。key_down/press の後に呼ぶと
            // 入力がシミュレーションに効いてから get_entity/project_world_to_screen で結果を見られる。
            // ※ 真の決定論ステッパではない(各フレーム dt は実時間)。エンジンは常時実時間で回る。
            int n = params.value("frames", params.value("n", 1));
            if (n < 1) n = 1;
            if (n > 600) n = 600;   // ~10s 上限(クライアント timeout 対策)
            if (m_mcpStepReply.client != 0)
                throw McpError(McpErr::ModeConflict, "a step is already pending; retry shortly");
            m_mcpStepFramesLeft = n;
            m_mcpStepReply = deferred;
            isDeferred = true;
        });

    McpDefine("perf_stats", "window:int", DX12E_MCP_HANDLER
        {
            // 直近 window フレームのリングバッファを平均して即答（ベンチ不要の現状把握用）。
            const u32 have = static_cast<u32>((std::min<u64>)(m_perfTotalFrames, kPerfHistory));
            if (have == 0) throw McpError(McpErr::Internal, "no frames recorded yet");
            const int windowReq = params.value("window", 60);
            const u32 n = static_cast<u32>(std::clamp(windowReq, 1, static_cast<int>(have)));

            PerfSummary s{};
            f64 mainDraws = 0, mainTris = 0, shadowDraws = 0, shadowTris = 0;
            std::vector<f32> fm;
            fm.reserve(n);
            for (u32 i = 0; i < n; ++i)
            {
                const PerfFrame& f = m_perfHistory[static_cast<size_t>((m_perfTotalFrames - 1 - i) % kPerfHistory)];
                if (f.frameMs > 0.0f) fm.push_back(f.frameMs);   // 起動直後の未計測 0 は除外
                s.workMs += f.workMs; s.fenceWaitMs += f.fenceWaitMs; s.presentMs += f.presentMs;
                s.draws += f.draws; s.culled += f.culled; s.tris += f.tris;
                mainDraws += f.passMain.draws; mainTris += f.passMain.tris;
                shadowDraws += f.passShadow.draws; shadowTris += f.passShadow.tris;
                for (u32 g = 0; g < GpuTimer::Count; ++g) s.gpuMs[g] += f.gpuMs[g];
                for (u32 c = 0; c < CpuScopeCount; ++c)   s.cpuMs[c] += f.cpuMs[c];
            }
            const double inv = 1.0 / static_cast<double>(n);
            s.workMs *= inv; s.fenceWaitMs *= inv; s.presentMs *= inv;
            s.draws *= inv; s.culled *= inv; s.tris *= inv;
            for (u32 g = 0; g < GpuTimer::Count; ++g) s.gpuMs[g] *= inv;
            for (u32 c = 0; c < CpuScopeCount; ++c)   s.cpuMs[c] *= inv;
            s.samples = static_cast<int>(fm.size());
            if (!fm.empty())
            {
                double sum = 0; for (f32 v : fm) sum += v;
                s.frameMs = sum / static_cast<double>(fm.size());
                s.fps = (s.frameMs > 0.0) ? 1000.0 / s.frameMs : 0.0;
                s.frameMsP95 = PerfPercentile(fm, 0.95);   // 内部でソート
                s.frameMsMin = fm.front();
                s.frameMsMax = fm.back();
            }
            nlohmann::json rep = PerfReportJson(s, m_useVsync, m_fpsLimit);
            rep["instancing"] = m_instancingEnabled;   // settings.json "render_instancing" で A/B 可
            // クラスタードライティング（Forward+）。settings.json "render_clustered" で A/B 可。
            // OFF / 正射カメラのときは「先頭 64 灯を総当たり」フォールバックで走る。
            rep["clustered"]  = m_clusteredEnabled;
            // 内部解像度スケール（#16）。GPU 時間を読むときは必ずこれも見ること
            // （renderScale=0.5 なら画素数が 1/4 になっているので単純比較できない）。
            {
                u32 dvx = 0, dvy = 0, dvw = 0, dvh = 0;
                GetDisplayViewport(dvx, dvy, dvw, dvh);
                rep["renderScale"]       = m_renderScale;
                rep["depthPrepass"]      = m_forceDepthPrepass;   // 計画10 A2 の A/B スイッチ
                rep["renderResolution"]  = {{"width", m_renderW}, {"height", m_renderH}};
                rep["displayResolution"] = {{"width", dvw},       {"height", dvh}};
            }

            // パス別内訳（平均/フレーム）。other = 深度プリパス/エディタプレビュー等
            rep["passes"] = {
                {"main",   {{"draws", mainDraws / n},   {"tris", static_cast<u64>(mainTris / n)}}},
                {"shadow", {{"draws", shadowDraws / n}, {"tris", static_cast<u64>(shadowTris / n)}}},
                {"other",  {{"draws", s.draws - (mainDraws + shadowDraws) / n},
                            {"tris",  static_cast<u64>(s.tris - (mainTris + shadowTris) / n)}}},
            };

            // シーン構成（描いてる物の内訳。ボトルネック解析の材料）
            auto& reg = m_scene->GetRegistry();
            int ents = 0, meshEnts = 0, skinned = 0, ptL = 0, spL = 0, emitters = 0;
            for (auto e : reg.view<const NameTag>()) { (void)e; ++ents; }
            for (auto e : reg.view<const MeshRenderer>()) { (void)e; ++meshEnts; }
            for (auto e : reg.view<const SkeletalAnimation>()) { (void)e; ++skinned; }
            for (auto e : reg.view<const PointLight>()) { (void)e; ++ptL; }
            for (auto e : reg.view<const SpotLight>()) { (void)e; ++spL; }
            for (auto e : reg.view<const ParticleEmitter>()) { (void)e; ++emitters; }
            rep["scene"] = {
                {"entities", ents},
                {"meshRenderers", meshEnts},
                {"drawItems", static_cast<int>(m_drawItems.size())},
                {"skinned", skinned},
                {"pointLights", ptL},
                {"spotLights", spL},
                {"particleEmitters", emitters},
                {"shadowsEnabled", m_scene->GetShadowsEnabled()},
                {"ssaoEnabled", m_scene->GetSSAOSettings().enabled},
                {"ssrEnabled",  m_scene->GetSsrSettings().enabled},
                {"ssgiEnabled", m_scene->GetSsgiSettings().enabled},
                {"contactShadowEnabled", m_scene->GetContactShadowSettings().enabled},
                {"taaEnabled", m_scene->GetTaaSettings().enabled},
                {"gpuTimerValid", m_gpuTimer && m_gpuTimer->IsValid()},
                {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"},
            };
            resp["ok"] = true;
            resp["result"] = rep;
        });

    McpDefine("benchmark", "frames:int,uncap:bool", DX12E_MCP_HANDLER
        {
            // N フレーム計測してから応答する遅延同期。カメラ/シーンは呼び出し側が事前に整えること。
            int n = params.value("frames", 300);
            if (n < 30) n = 30;
            if (n > 3600) n = 3600;
            if (m_benchFramesLeft > 0 || m_benchReply.client != 0)
                throw McpError(McpErr::ModeConflict, "a benchmark is already running; wait for it to finish");
            m_benchSamples.clear();
            m_benchSamples.reserve(static_cast<size_t>(n));
            m_benchDraws = m_benchCulled = m_benchTris = 0;
            m_benchWork = m_benchFence = m_benchPresent = 0;
            for (auto& g : m_benchGpu) g = 0;
            for (auto& c : m_benchCpu) c = 0;
            // 既定で FPS 上限/VSync を計測中だけ外す(=真のスループットを測る)。
            // uncap:false を渡せば普段の設定のまま測れる。終了時に必ず戻す。
            if (params.value("uncap", true))
            {
                m_benchRestore = true;
                m_benchSavedFpsLimit = m_fpsLimit;
                m_benchSavedVsync    = m_useVsync;
                m_fpsLimit = 0.0f;
                m_useVsync = false;
            }
            m_benchFramesLeft = static_cast<u32>(n);
            m_benchReply = deferred;
            isDeferred = true;
        });

    McpDefine("set_color", "color:any,entity:int,name:string", DX12E_MCP_HANDLER
        {
            // メッシュの頂点色(基本色の乗算)を設定する。scene:setColor(Lua) と同じ。
            // 足場やコインの色付けに。色は [r,g,b](0..1)。
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<MeshRenderer>(e)) throw McpError(McpErr::NotFound, "entity has no MeshRenderer");
            auto c = params.value("color", std::vector<float>{1.0f, 1.0f, 1.0f});
            if (c.size() != 3) throw McpError(McpErr::InvalidParam, "color must be [r,g,b]");
            auto* device = m_scene->GetDevice();
            if (!device) throw McpError(McpErr::Internal, "no graphics device");
            auto& mr = reg.get<MeshRenderer>(e);
            mr.colorTint    = {c[0], c[1], c[2], 1.0f};   // シーン保存で色指定が消えないよう記録
            mr.hasColorTint = true;
            for (auto* mesh : mr.meshes) if (mesh) mesh->SetVertexColor(*device, c[0], c[1], c[2], 1.0f);
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"color", {c[0], c[1], c[2]}}};
        });

    McpDefine("screenshot_game_view", "", DX12E_MCP_HANDLER
        {
            // アクティブな CameraComponent 視点でシーンを1フレーム描いて撮る(遅延応答)。
            // Editor 中でもゲームカメラの画角を確認できる。Playing 中は通常 screenshot と同じ絵。
            auto& reg = m_scene->GetRegistry();
            bool hasActiveCam = false;
            for (auto [e, cam] : reg.view<const CameraComponent>().each())
                if (cam.isActive) { hasActiveCam = true; break; }
            if (!hasActiveCam && m_engineMode != EngineMode::Playing)
                throw McpError(McpErr::NotFound,
                    "no active CameraComponent (camera.isActive=true にするか dx12_screenshot を使う)");
            if (m_mcpGameViewReply.client != 0)
                throw McpError(McpErr::ModeConflict, "a game-view screenshot is already pending; retry shortly");
            m_mcpGameViewReply = deferred;   // フレーム境界で描画→撮影→応答(Run ループ側)
            isDeferred = true;
        });

    // ════════════════════════════════════════════════════════════
    //  ランタイム物理検証(raycast/overlap/velocity) — 全て同期・読み取り系。
    //  bodies は Play 中のみ登録される(RegisterBody は Play 開始/loadScene 時)。
    //  Editor 中に呼んでもエラーにはせず hit=false / entities=[] / velocity=[0,0,0] を返す。
    // ════════════════════════════════════════════════════════════
    McpDefine("raycast", "direction:any,maxDistance:number,origin:any", DX12E_MCP_HANDLER
        {
            auto originV = params.value("origin", std::vector<float>{});
            auto dirV = params.value("direction", std::vector<float>{});
            if (originV.size() != 3 || dirV.size() != 3)
                throw McpError(McpErr::InvalidParam, "origin and direction must be [x,y,z]");
            const float maxDist = params.value("maxDistance", 1000.0f);
            RaycastHit hit = m_physicsSystem->Raycast(
                {originV[0], originV[1], originV[2]}, {dirV[0], dirV[1], dirV[2]}, maxDist);
            json result{{"hit", hit.hit}};
            if (hit.hit)
            {
                result["distance"] = hit.distance;
                result["point"]    = {hit.point.x, hit.point.y, hit.point.z};
                // 法線は近似(常に up 向き。PhysicsSystem::Raycast の既知の制約)。厳密な面法線は未対応。
                result["normal"]   = {hit.normal.x, hit.normal.y, hit.normal.z};
                auto& reg = m_scene->GetRegistry();
                entt::entity ent = m_physicsSystem->EntityForBody(hit.bodyId);
                if (ent != entt::null && reg.valid(ent))
                {
                    result["entityId"] = static_cast<u32>(ent);
                    if (reg.all_of<NameTag>(ent)) result["name"] = reg.get<NameTag>(ent).name;
                }
            }
            resp["ok"] = true;
            resp["result"] = std::move(result);
        });

    McpDefine("overlap_box|overlap_sphere", "center:any,halfExtents:any,maxResults:int,radius:number", DX12E_MCP_HANDLER
        {
            int maxResults = params.value("maxResults", 32);
            if (maxResults < 1) maxResults = 1;
            if (maxResults > 256) maxResults = 256;
            std::vector<entt::entity> buf(static_cast<size_t>(maxResults));
            size_t n = 0;
            if (method == "overlap_box")
            {
                auto centerV = params.value("center", std::vector<float>{});
                auto halfV = params.value("halfExtents", std::vector<float>{});
                if (centerV.size() != 3 || halfV.size() != 3)
                    throw McpError(McpErr::InvalidParam, "center and halfExtents must be [x,y,z]");
                n = m_physicsSystem->OverlapBox({centerV[0], centerV[1], centerV[2]},
                                                 {halfV[0], halfV[1], halfV[2]}, buf.data(), buf.size());
            }
            else
            {
                auto centerV = params.value("center", std::vector<float>{});
                if (centerV.size() != 3)
                    throw McpError(McpErr::InvalidParam, "center must be [x,y,z]");
                const float radius = params.value("radius", 1.0f);
                n = m_physicsSystem->OverlapSphere({centerV[0], centerV[1], centerV[2]}, radius,
                                                    buf.data(), buf.size());
            }
            json arr = json::array();
            auto& reg = m_scene->GetRegistry();
            for (size_t i = 0; i < n; ++i)
            {
                json item{{"entityId", static_cast<u32>(buf[i])}};
                if (reg.valid(buf[i]) && reg.all_of<NameTag>(buf[i])) item["name"] = reg.get<NameTag>(buf[i]).name;
                arr.push_back(std::move(item));
            }
            resp["ok"] = true;
            resp["result"] = {{"entities", arr}, {"count", arr.size()}};
        });

    McpDefine("get_physics_state", "entity:int,name:string", DX12E_MCP_HANDLER
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            json result{{"entityId", static_cast<u32>(e)},
                        {"hasRigidBody", false}, {"velocity", {0.0f, 0.0f, 0.0f}},
                        {"hasCharacterController", false}, {"isGrounded", false}};
            if (reg.all_of<RigidBody>(e))
            {
                const auto& rb = reg.get<RigidBody>(e);
                result["hasRigidBody"] = true;
                if (rb.bodyId != kInvalidBodyId)
                {
                    auto v = m_physicsSystem->GetLinearVelocity(rb.bodyId);
                    result["velocity"] = {v.x, v.y, v.z};
                }
            }
            if (reg.all_of<CharacterController>(e))
            {
                result["hasCharacterController"] = true;
                result["isGrounded"] = reg.get<CharacterController>(e)._grounded;
            }
            resp["ok"] = true;
            resp["result"] = std::move(result);
        });

    // ════════════════════════════════════════════════════════════
    //  コンテンツ制作ヘルパー拡充
    // ════════════════════════════════════════════════════════════
    McpDefine("read_lua_component", "path:string", DX12E_MCP_HANDLER
        {
            std::string rel = params.value("path", std::string());
            if (rel.empty()) throw McpError(McpErr::InvalidParam, "missing 'path'");
            if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
            const fs::path full = fs::path(PathResolver::AssetsDir()) / rel;
            if (!fs::exists(full)) throw McpError(McpErr::NotFound, "script not found: " + rel);
            std::ifstream ifs(full, std::ios::binary);
            if (!ifs) throw McpError(McpErr::Internal, "cannot open " + full.string());
            std::ostringstream ss; ss << ifs.rdbuf();
            resp["ok"] = true;
            resp["result"] = {{"path", rel}, {"code", ss.str()}};
        });

    McpDefine("create_prefab", "entity:int,name:string,path:string", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot create a prefab while Playing; call dx12_stop first");
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            std::string rel = params.value("path", std::string());
            fs::path file;
            if (rel.empty())
            {
                std::string base = reg.all_of<NameTag>(e) ? reg.get<NameTag>(e).name : std::string("Prefab");
                if (base.empty()) base = "Prefab";
                fs::path dir = fs::path(PathResolver::AssetsDir()) / "prefabs";
                fs::create_directories(dir);
                file = dir / (base + ".prefab");
                for (int n = 1; fs::exists(file); ++n)
                    file = dir / (base + " (" + std::to_string(n) + ").prefab");
                rel = "prefabs/" + file.filename().string();
            }
            else
            {
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
                if (fs::path(rel).extension() != ".prefab")
                    throw McpError(McpErr::InvalidParam, "path must end with .prefab");
                file = fs::path(PathResolver::AssetsDir()) / rel;
                fs::create_directories(file.parent_path());
            }
            if (!SceneSerializer::SavePrefab(*m_scene, e, file.string(), PathResolver::AssetsDir()))
                throw McpError(McpErr::Internal, "failed to save prefab");
            resp["ok"] = true;
            resp["result"] = {{"path", rel}, {"entityId", static_cast<u32>(e)}};
        });
}



} // namespace dx12e
