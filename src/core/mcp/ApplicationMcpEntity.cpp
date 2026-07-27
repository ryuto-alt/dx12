// ===========================================================================
// MCP: エンティティ / コンポーネント / シーン入出力
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// method の足し方は本ファイル内 McpDefine の並びに倣う（作法は ApplicationInternal.h の DX12E_MCP_HANDLER 付近）。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;


// ---- エンティティ / コンポーネント / シーン入出力 ----
void Application::RegisterMcpEntityMethods()
{
    using json = nlohmann::json;
    namespace fs = std::filesystem;

    McpDefine("list_entities", "component_type:string,name_prefix:string,verbose:bool", DX12E_MCP_HANDLER
        {
            const bool verbose = params.value("verbose", false);
            const std::string namePrefix = params.value("name_prefix", std::string());
            std::string typeFilter = params.value("component_type", std::string());
            json arr = json::array();
            auto& reg = m_scene->GetRegistry();
            auto view = reg.view<const NameTag>();
            for (auto e : view)
            {
                const std::string& nm = view.get<const NameTag>(e).name;
                if (!namePrefix.empty() && nm.rfind(namePrefix, 0) != 0) continue;
                json types;   // verbose か component_type 指定時のみ計算
                if (verbose || !typeFilter.empty()) types = McpComponentTypesOf(reg, e);
                if (!typeFilter.empty())
                {
                    bool has = false;
                    for (auto& t : types) if (t.get<std::string>() == typeFilter) { has = true; break; }
                    if (!has) continue;
                }
                json item{{"entityId", static_cast<u32>(e)}, {"id", static_cast<u32>(e)}, {"name", nm}};
                if (verbose) item["componentTypes"] = types;
                arr.push_back(std::move(item));
            }
            resp["ok"] = true;
            resp["result"] = {{"entities", arr}, {"count", arr.size()},
                              {"sceneGeneration", m_sceneGeneration}};
        });

    McpDefine("create_lua_component", "code:string,name:string", DX12E_MCP_HANDLER
        {
            const std::string name = params.value("name", std::string());
            const std::string code = params.value("code", std::string());
            if (name.empty()) throw std::runtime_error("missing 'name'");
            // ponytail: name はファイル名へ直結。パス区切り等を弾いて traversal を防ぐ。
            if (name.find_first_of("/\\:*?\"<>|") != std::string::npos)
                throw std::runtime_error("invalid component name");
            // 構文チェック(コンパイルのみ・実行しない)。不正なら書かずに AI へエラーを返す。
            std::string serr;
            if (!m_scriptEngine->CheckLuaSyntax(code, serr))
                throw std::runtime_error("Lua syntax error: " + serr);

            const std::string rel = "components/" + name + ".lua";
            const fs::path full = fs::path(PathResolver::AssetsDir()) / rel;
            fs::create_directories(full.parent_path());
            std::ofstream ofs(full, std::ios::binary | std::ios::trunc);
            if (!ofs) throw std::runtime_error("cannot write " + full.string());
            ofs.write(code.data(), static_cast<std::streamsize>(code.size()));
            resp["ok"] = true;
            resp["result"] = {{"path", rel}};
        });

    McpDefine("create_shader", "code:string,name:string", DX12E_MCP_HANDLER
        {
            // カスタムシェーダー(MeshRenderer::shaderPath 割当用)を assets/shaders/ に作成/上書きする。
            // Lua と違い、書く前の静的検証ができない(DXC はファイルからしかコンパイルできない)ため、
            // 先に書いてから即コンパイルを試み、成否をそのまま返す(失敗してもファイルは残す=
            // 反復修正前提。エンジン側も無効なカスタムシェーダーは既定 Forward へ安全にフォールバックする)。
            const std::string name = params.value("name", std::string());
            const std::string code = params.value("code", std::string());
            if (name.empty()) throw McpError(McpErr::InvalidParam, "missing 'name'");
            if (name.find_first_of("/\\:*?\"<>|") != std::string::npos)
                throw McpError(McpErr::InvalidParam, "invalid shader name");

            const std::string rel = name + ".hlsl";
            const fs::path full = fs::path(PathResolver::ProjectShaderDir()) / rel;
            fs::create_directories(full.parent_path());
            {
                std::ofstream ofs(full, std::ios::binary | std::ios::trunc);
                if (!ofs) throw McpError(McpErr::Internal, "cannot write " + full.string());
                ofs.write(code.data(), static_cast<std::streamsize>(code.size()));
            }

            bool compiled = false;
            std::string error;
            if (m_shaderManager)
            {
                compiled = m_shaderManager->CompileCustomShader(rel);
                if (!compiled) error = m_shaderManager->GetCustomShaderError(rel);
            }
            resp["ok"] = true;
            resp["result"] = {{"path", rel}, {"compiled", compiled}};
            if (!compiled) resp["result"]["error"] = error.empty() ? "shader manager unavailable" : error;
        });

    McpDefine("read_shader", "path:string", DX12E_MCP_HANDLER
        {
            const std::string rel = params.value("path", std::string());
            if (rel.empty()) throw McpError(McpErr::InvalidParam, "missing 'path'");
            if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                throw McpError(McpErr::InvalidParam, "invalid path (assets/shaders 相対のみ)");

            const fs::path full = fs::path(PathResolver::ProjectShaderDir()) / rel;
            if (!fs::exists(full)) throw McpError(McpErr::NotFound, "shader not found: " + rel);
            std::ifstream ifs(full, std::ios::binary);
            if (!ifs) throw McpError(McpErr::Internal, "cannot open " + full.string());
            std::ostringstream oss; oss << ifs.rdbuf();

            resp["ok"] = true;
            resp["result"] = {{"path", rel}, {"code", oss.str()},
                               {"compiled", m_shaderManager && m_shaderManager->HasValidCustomShader(rel)}};
        });

    McpDefine("set_mesh_shader", "alphaBlend:bool,entity:int,name:string,shaderPath:string", DX12E_MCP_HANDLER
        {
            // MeshRenderer::shaderPath の割当/解除。Inspector の「Shader」コンボと同じ操作を MCP から。
            // modelPath と違いメッシュ再ロードを伴わない(PSO 選択が変わるだけ)ので即時反映して安全。
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<MeshRenderer>(e)) throw McpError(McpErr::NotFound, "entity has no MeshRenderer");
            auto& mr = reg.get<MeshRenderer>(e);

            std::string rel = params.value("shaderPath", std::string());
            if (!rel.empty())
            {
                if (rel.rfind("shaders/", 0) == 0)
                    rel.erase(0, 8);  // assets相対表記("shaders/foo.hlsl")も受け付けて正規化
                if (rel.empty())     // 入力が "shaders/" ちょうどだと erase で空になる
                    throw McpError(McpErr::InvalidParam, "invalid shaderPath (assets/shaders 相対のみ)");
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw McpError(McpErr::InvalidParam, "invalid shaderPath (assets/shaders 相対のみ)");
                if (!fs::exists(fs::path(PathResolver::ProjectShaderDir()) / rel))
                    throw McpError(McpErr::NotFound, "shader not found: " + rel);
            }
            mr.shaderPath = rel;
            if (params.contains("alphaBlend"))
                mr.shaderAlphaBlend = params.value("alphaBlend", false);

            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"shaderPath", mr.shaderPath},
                               {"alphaBlend", mr.shaderAlphaBlend},
                               {"skinnedFallbackWarning", reg.all_of<SkeletalAnimation>(e) && !mr.shaderPath.empty()}};
        });

    McpDefine("set_sprite_shader", "alphaBlend:bool,entity:int,name:string,shaderPath:string", DX12E_MCP_HANDLER
        {
            // Sprite2D::shaderPath の割当/解除。set_mesh_shader と同型だが、対象はworld-spaceスプライトのみ
            // (ルートシグネチャ/頂点フォーマットがメッシュ用と異なる別キャッシュ。docs/AUTHORING.md参照)。
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<Sprite2D>(e)) throw McpError(McpErr::NotFound, "entity has no Sprite2D");
            auto& sp = reg.get<Sprite2D>(e);

            std::string rel = params.value("shaderPath", std::string());
            if (!rel.empty())
            {
                if (rel.rfind("shaders/", 0) == 0)
                    rel.erase(0, 8);  // assets相対表記("shaders/foo.hlsl")も受け付けて正規化
                if (rel.empty())     // 入力が "shaders/" ちょうどだと erase で空になる
                    throw McpError(McpErr::InvalidParam, "invalid shaderPath (assets/shaders 相対のみ)");
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw McpError(McpErr::InvalidParam, "invalid shaderPath (assets/shaders 相対のみ)");
                if (!fs::exists(fs::path(PathResolver::ProjectShaderDir()) / rel))
                    throw McpError(McpErr::NotFound, "shader not found: " + rel);
            }
            sp.shaderPath = rel;
            if (params.contains("alphaBlend"))
                sp.shaderAlphaBlend = params.value("alphaBlend", false);

            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"shaderPath", sp.shaderPath},
                               {"alphaBlend", sp.shaderAlphaBlend},
                               {"worldSpaceWarning", !sp.worldSpace && !sp.shaderPath.empty()}};
        });

    McpDefine("attach_lua_component", "entity:int,name:string,script:string", DX12E_MCP_HANDLER
        {
            const std::string script = params.value("script", std::string());
            if (script.empty()) throw std::runtime_error("missing 'script'");
            // assets 配下限定。絶対パス/ドライブレター/バックスラッシュ/".." を弾いて
            // assets ルート外の任意ファイルを Lua として読ませない(traversal 防止)。
            if (script.front() == '/' || script.find('\\') != std::string::npos ||
                script.find(':') != std::string::npos || script.find("..") != std::string::npos)
                throw std::runtime_error("invalid script path (assets 相対のみ)");
            const auto e = ResolveMcpEntity(*m_scene, params);
            m_scriptEngine->AttachScriptToEntity(e, script);
            m_scriptEngine->ReloadScript(e);
            resp["ok"] = true;
        });

    McpDefine("create_entity", "name:string,parent:any,parentName:any,position:any,type:string", DX12E_MCP_HANDLER
        {
            // 生成はメッシュ構築に cmdList が要るためフレーム境界で遅延処理。本物の entityId は
            // 生成後に SendToClient で返す(遅延同期)。Play 中は spawn キューが drain されないため拒否。
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot create entities while Playing; call dx12_stop first");
            const std::string type = params.value("type", std::string("box"));
            std::string name = params.value("name", std::string());
            const auto pos = params.value("position", std::vector<float>{0.0f, 0.0f, 0.0f});
            if (pos.size() != 3) throw McpError(McpErr::InvalidParam, "position must be [x,y,z]");
            std::string marker;
            if      (type == "box")    marker = "__primitive_box__";
            else if (type == "sphere") marker = "__primitive_sphere__";
            else if (type == "plane")  marker = "__primitive_plane__";
            else if (type == "empty")  marker = "__empty__";
            else if (type == "camera")            marker = "__camera__";
            else if (type == "light_directional") marker = "__directional_light__";
            else if (type == "light_point")       marker = "__point_light__";
            else if (type == "light_spot")        marker = "__spot_light__";
            else if (type == "particle_emitter")  marker = "__particle_emitter__";
            else if (type == "trigger")           marker = "__trigger__";
            else if (type == "decal")             marker = "__decal__";
            else if (type == "ui_canvas")     marker = "__ui_canvas__";
            else if (type == "ui_image")      marker = "__ui_image__";
            else if (type == "ui_text")       marker = "__ui_text__";
            else if (type == "ui_button")     marker = "__ui_button__";
            else if (type == "ui_slider")     marker = "__ui_slider__";
            else if (type == "ui_toggle")     marker = "__ui_toggle__";
            else if (type == "ui_scrollview") marker = "__ui_scrollview__";
            else throw McpError(McpErr::InvalidParam,
                "type must be one of: box, sphere, plane, empty, camera, light_directional, "
                "light_point, light_spot, particle_emitter, trigger, decal, ui_canvas, ui_image, "
                "ui_text, ui_button, ui_slider, ui_toggle, ui_scrollview");

            // UI 要素の親の明示指定(id か名前)。ui_canvas はルート生成なので対象外。
            entt::entity uiParentOverride = entt::null;
            if (type.rfind("ui_", 0) == 0 && type != "ui_canvas"
                && (params.contains("parent") || params.contains("parentName")))
            {
                auto& reg = m_scene->GetRegistry();
                if (params.contains("parent"))
                {
                    const auto pe = static_cast<entt::entity>(params["parent"].get<u32>());
                    if (!reg.valid(pe)) throw McpError(McpErr::NotFound, "invalid parent entity id");
                    uiParentOverride = pe;
                }
                else
                {
                    const std::string pname = params["parentName"].get<std::string>();
                    for (auto [pe, tag] : reg.view<const NameTag>().each())
                        if (tag.name == pname) { uiParentOverride = pe; break; }
                    if (uiParentOverride == entt::null)
                        throw McpError(McpErr::NotFound, "parentName not found: " + pname);
                }
            }
            if (name.empty())   // 既定名: 種別名を先頭大文字に
            {
                if (type.rfind("ui_", 0) == 0)
                {
                    // ui_scrollview → "UIScrollView" 等、UI は Pascal 風の既定名にする
                    if      (type == "ui_canvas")     name = "UICanvas";
                    else if (type == "ui_image")      name = "UIImage";
                    else if (type == "ui_text")       name = "UIText";
                    else if (type == "ui_button")     name = "UIButton";
                    else if (type == "ui_slider")     name = "UISlider";
                    else if (type == "ui_toggle")     name = "UIToggle";
                    else                              name = "UIScrollView";
                }
                else
                {
                    name = type;
                    if (name[0] >= 'a' && name[0] <= 'z') name[0] = static_cast<char>(name[0] - 'a' + 'A');
                }
            }
            // idempotency: 同 key で既に生成済みかつ有効ならそれを即返す(再試行の重複生成防止)。
            if (!deferred.idempotencyKey.empty())
            {
                auto it = m_mcpIdempotency.find(deferred.idempotencyKey);
                if (it != m_mcpIdempotency.end() &&
                    m_scene->GetRegistry().valid(static_cast<entt::entity>(it->second)))
                {
                    resp["ok"] = true;
                    resp["result"] = {{"entityId", it->second}, {"name", name},
                                      {"sceneGeneration", m_sceneGeneration}, {"idempotentReplay", true}};
                }
            }
            if (!resp.contains("result"))
            {
                PendingSpawnRequest sreq;
                sreq.modelPath = marker;
                sreq.position  = { pos[0], pos[1], pos[2] };
                sreq.name      = name;
                sreq.mcp       = deferred;
                sreq.parent    = uiParentOverride;
                m_editorCtx->pendingSpawns.push_back(std::move(sreq));
                isDeferred = true;
            }
        });

    McpDefine("delete_entity", "entity:int,name:string", DX12E_MCP_HANDLER
        {
            // 削除ドレイン(Render)は Editor モード限定。Play 中に積むと drain されず未応答ハングするため弾く。
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot delete while Playing; call dx12_stop first");
            const auto e = ResolveMcpEntity(*m_scene, params);
            // 子ごと削除+Undo はフレーム境界で処理し、deletedCount を遅延応答で返す。
            m_editorCtx->mcpDeletions.push_back(McpPendingDelete{ e, deferred });
            isDeferred = true;
        });

    McpDefine("set_transform", "entity:int,name:string,position:any,quaternion:any,rotation:any,scale:any", DX12E_MCP_HANDLER
        {
            const auto e = ResolveMcpEntity(*m_scene, params);   // 無効 id は "invalid entity id" を投げる
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<Transform>(e))
                throw McpError(McpErr::NotFound, "entity has no Transform");
            auto& t = reg.get<Transform>(e);
            if (params.contains("position"))
            {
                const auto p = params["position"].get<std::vector<float>>();
                if (p.size() != 3) throw McpError(McpErr::InvalidParam, "position must be [x,y,z]");
                t.position = { p[0], p[1], p[2] };
            }
            if (params.contains("rotation"))
            {
                const auto r = params["rotation"].get<std::vector<float>>();
                if (r.size() != 3) throw McpError(McpErr::InvalidParam, "rotation must be [x,y,z]");
                t.rotation = { r[0], r[1], r[2] };
                t.useQuaternion = false;   // Euler を反映(物理同期の quaternion に上書きされないように)
            }
            if (params.contains("quaternion"))
            {
                const auto q = params["quaternion"].get<std::vector<float>>();
                if (q.size() != 4) throw McpError(McpErr::InvalidParam, "quaternion must be [x,y,z,w]");
                t.quaternion = { q[0], q[1], q[2], q[3] };
                t.useQuaternion = true;   // set_component の transform 経路と同じ挙動に揃える
            }
            if (params.contains("scale"))
            {
                const auto s = params["scale"].get<std::vector<float>>();
                if (s.size() != 3) throw McpError(McpErr::InvalidParam, "scale must be [x,y,z]");
                t.scale = { s[0], s[1], s[2] };
            }
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}};
        });

    McpDefine("get_entity", "entity:int,name:string", DX12E_MCP_HANDLER
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            // 既存シリアライザを流用(リフレクション的に全コンポーネントを JSON 化)。
            std::string js = SceneSerializer::SerializeEntity(*m_scene, e, PathResolver::AssetsDir());
            json result = json::parse(js);
            result["entityId"] = static_cast<u32>(e);
            json types = McpComponentTypesOf(reg, e);
            // Lua スクリプトが entity.<key> で直接読めるコンポーネントだけを別出し(現状 transform のみ)。
            // MCP で見えても Lua では nil になる boxCollider 等との取り違えを防ぐ。
            json luaReadable = json::array();
            for (auto& k : types) if (LuaReadableComponent(k.get<std::string>())) luaReadable.push_back(k);
            result["luaReadable"] = std::move(luaReadable);
            result["componentTypes"] = std::move(types);
            result["sceneGeneration"] = m_sceneGeneration;
            resp["ok"] = true;
            resp["result"] = std::move(result);
        });

    McpDefine("save_scene", "path:string", DX12E_MCP_HANDLER
        {
            std::string rel = params.value("path", std::string());
            std::string full;
            if (rel.empty())
            {
                if (m_editorCtx->currentScenePath.empty())
                    throw std::runtime_error("no current scene; specify 'path'");
                full = m_editorCtx->currentScenePath;          // 既存は絶対パス
            }
            else
            {
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw std::runtime_error("invalid path (assets 相対のみ)");
                full = PathResolver::AssetsDir() + rel;        // 末尾 '/' 付き
                fs::create_directories(fs::path(full).parent_path());
            }
            if (!SceneSerializer::Save(*m_scene, full, PathResolver::AssetsDir()))
                throw std::runtime_error("save failed");
            resp["ok"] = true;
            resp["result"] = {{"path", rel.empty() ? m_editorCtx->currentScenePath : rel}};
        });

    McpDefine("open_scene", "path:string", DX12E_MCP_HANDLER
        {
            std::string rel = params.value("path", std::string());
            if (rel.empty()) throw McpError(McpErr::InvalidParam, "missing 'path'");
            if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
            // pendingLoadPath は Editor モードでのみ drain される(Play 中はロードしない)。
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot open scene while Playing; call dx12_stop first");
            // 単一スロット: 既に未処理の open_scene があれば 2件目を弾く(上書きで1件目が宙吊りになるのを防ぐ)。
            if (m_mcpLoadReply.client != 0 || !m_editorCtx->pendingLoadPath.empty())
                throw McpError(McpErr::ModeConflict, "a scene load is already in progress; retry after it completes");
            const std::string full = PathResolver::AssetsDir() + rel;
            if (!fs::exists(full)) throw McpError(McpErr::NotFound, "scene not found: " + rel);
            // 遅延ロード: フレーム境界の機構が pendingLoadPath を消費し SceneSerializer::Load を行う。
            // 完了後に m_mcpLoadReply 経由で sceneName/entityCount/sceneGeneration を返す(遅延同期)。
            m_editorCtx->pendingLoadPath = full;
            m_mcpLoadReply = deferred;
            isDeferred = true;
        });

    McpDefine("open_project", "path:string", DX12E_MCP_HANDLER
        {
            // プロジェクトを開く(ランチャーのクリックと同等)。path はプロジェクトルート絶対パス。
            // BeginProjectLoad は数フレームかけて非同期にロードする(スプラッシュ表示→シーン差替)ので、
            // ここでは開始だけして即応答する。完了確認は dx12_ping の currentScene で行える。
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot open project while Playing; call dx12_stop first");
            if (m_loading)
                throw McpError(McpErr::ModeConflict, "a project load is already in progress; retry after it completes");
            if (m_mcpLoadReply.client != 0 || !m_editorCtx->pendingLoadPath.empty())
                throw McpError(McpErr::ModeConflict, "a scene load is already in progress; retry after it completes");
            const std::string root = params.value("path", std::string());
            if (root.empty()) throw McpError(McpErr::InvalidParam, "missing 'path' (project root absolute path)");
            if (!fs::exists(root) || !fs::is_directory(root))
                throw McpError(McpErr::NotFound, "project folder not found: " + root);
            ProjectInfo info;
            if (!ProjectManager::ProjectFromFolder(root, info))
                throw McpError(McpErr::NotFound, "not a project folder: " + root);
            BeginProjectLoad(info, /*isNew=*/false);
            resp["ok"] = true;
            resp["result"] = {{"name", info.name}, {"rootDir", info.rootDir},
                              {"defaultScene", info.defaultScene}, {"loading", true}};
        });

    McpDefine("list_scenes", "", DX12E_MCP_HANDLER
        {
            json arr = json::array();
            const std::string root = PathResolver::AssetsDir();      // 末尾 '/'
            const fs::path scenesDir = fs::path(root) / "scenes";    // scenes/ 配下のみシーン扱い
            if (fs::exists(scenesDir))
            {
                for (const auto& de : fs::recursive_directory_iterator(scenesDir))
                {
                    if (!de.is_regular_file() || de.path().extension() != ".json") continue;
                    std::string relPath = fs::relative(de.path(), fs::path(root)).generic_string();
                    arr.push_back({{"path", relPath}, {"name", de.path().stem().string()}});
                }
            }
            resp["ok"] = true;
            resp["result"] = std::move(arr);
        });

    McpDefine("list_assets", "type:string", DX12E_MCP_HANDLER
        {
            const std::string filter = params.value("type", std::string());
            json arr = json::array();
            const std::string root = PathResolver::AssetsDir();
            // ext + 相対パスで分類。.json は scenes/ 配下だけ "scene"(game.json/sceneflow.json 等を除外)。
            auto classify = [](std::string ext, const std::string& relPath) -> std::string {
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj") return "model";
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".dds" ||
                    ext == ".tga" || ext == ".bmp") return "texture";
                if (ext == ".lua") return "script";
                if (ext == ".hlsl") return "shader";
                if (ext == ".wav" || ext == ".mp3" || ext == ".ogg") return "audio";
                if (ext == ".json")
                    return (relPath.rfind("scenes/", 0) == 0) ? "scene" : std::string();
                if (ext == ".prefab") return "prefab";
                return std::string();
            };
            if (fs::exists(fs::path(root)))
            {
                for (const auto& de : fs::recursive_directory_iterator(fs::path(root)))
                {
                    if (!de.is_regular_file()) continue;
                    std::string relPath = fs::relative(de.path(), fs::path(root)).generic_string();
                    std::string type = classify(de.path().extension().string(), relPath);
                    if (type.empty()) continue;
                    if (!filter.empty() && type != filter) continue;
                    arr.push_back({{"path", relPath}, {"type", type}, {"name", de.path().stem().string()}});
                }
            }
            resp["ok"] = true;
            resp["result"] = std::move(arr);
        });

    McpDefine("spawn_model", "name:string,path:string,position:any", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot spawn while Playing; call dx12_stop first");
            std::string path = params.value("path", std::string());
            if (path.empty()) throw McpError(McpErr::InvalidParam, "missing 'path'");
            if (path.front() == '/' || path.find('\\') != std::string::npos ||
                path.find(':') != std::string::npos || path.find("..") != std::string::npos)
                throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
            if (!fs::exists(PathResolver::AssetsDir() + path))
                throw McpError(McpErr::NotFound, "model not found: " + path);
            const auto pos = params.value("position", std::vector<float>{0.0f, 0.0f, 0.0f});
            if (pos.size() != 3) throw McpError(McpErr::InvalidParam, "position must be [x,y,z]");
            std::string name = params.value("name", std::string());
            if (name.empty()) name = fs::path(path).stem().string();
            // idempotency: 同 key で生成済みかつ有効ならそれを即返す。
            if (!deferred.idempotencyKey.empty())
            {
                auto it = m_mcpIdempotency.find(deferred.idempotencyKey);
                if (it != m_mcpIdempotency.end() &&
                    m_scene->GetRegistry().valid(static_cast<entt::entity>(it->second)))
                {
                    resp["ok"] = true;
                    resp["result"] = {{"entityId", it->second}, {"name", name},
                                      {"meshPath", path}, {"sceneGeneration", m_sceneGeneration},
                                      {"idempotentReplay", true}};
                }
            }
            if (!resp.contains("result"))
            {
                // 実モデルのロードは GPU を伴うため cmdList 有効なフレーム境界で遅延処理。
                // create_entity と同じ pendingSpawns に積む(marker でなく実パスを入れる)。
                PendingSpawnRequest sreq;
                sreq.modelPath = path;
                sreq.position  = { pos[0], pos[1], pos[2] };
                sreq.name      = name;
                sreq.mcp       = deferred;
                m_editorCtx->pendingSpawns.push_back(std::move(sreq));
                isDeferred = true;
            }
        });

    McpDefine("set_component", "component:string,data:any,entity:int,name:string,values:any", DX12E_MCP_HANDLER
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            const std::string comp = params.value("component", std::string());
            if (comp.empty()) throw McpError(McpErr::InvalidParam, "missing 'component'");
            // フィールドは 'data'（'values' も別名として許容）。どちらも無ければエラー。
            // 旧実装は無指定を空オブジェクト扱い＝全フィールドをデフォルトで再生成する事故になっていた
            // （onClickEvent 等が黙って消える）。
            json data;
            if (params.contains("data"))        data = params["data"];
            else if (params.contains("values")) data = params["values"];
            if (!data.is_object() || data.empty())
                throw McpError(McpErr::InvalidParam,
                    "missing component fields: pass a non-empty 'data' object");
            if (comp == "transform")
            {
                // コア不変: 専用処理(set_transform 相当)
                auto& t = reg.get_or_emplace<Transform>(e);
                if (data.contains("position"))
                {
                    auto p = data["position"].get<std::vector<float>>();
                    if (p.size() != 3) throw McpError(McpErr::InvalidParam, "position must be [x,y,z]");
                    t.position = { p[0], p[1], p[2] };
                }
                if (data.contains("rotation"))
                {
                    auto r = data["rotation"].get<std::vector<float>>();
                    if (r.size() != 3) throw McpError(McpErr::InvalidParam, "rotation must be [x,y,z]");
                    t.rotation = { r[0], r[1], r[2] };
                    t.useQuaternion = false;
                }
                if (data.contains("quaternion"))
                {
                    auto q = data["quaternion"].get<std::vector<float>>();
                    if (q.size() != 4) throw McpError(McpErr::InvalidParam, "quaternion must be [x,y,z,w]");
                    t.quaternion = { q[0], q[1], q[2], q[3] };
                    t.useQuaternion = true;
                }
                if (data.contains("scale"))
                {
                    auto s = data["scale"].get<std::vector<float>>();
                    if (s.size() != 3) throw McpError(McpErr::InvalidParam, "scale must be [x,y,z]");
                    t.scale = { s[0], s[1], s[2] };
                }
            }
            // orphan(レジストリ未登録)コンポーネントは専用適用(save/load 経路に触れない)。
            else if (ApplyOrphanComponent(reg, e, comp, data))
            {
                // 適用済み(emplace_or_replace)。
            }
            else
            {
                // 部分更新(マージ): 現在値を書き出してから data を被せる。未指定フィールドが
                // デフォルトへ戻らない(従来は data のみで再生成=丸ごと置換の罠だった)。
                json cur = json::object();
                RuntimeComponentRegistry::Get().ForEach([&](const RuntimeComponentInfo& info) {
                    if (info.serialize) info.serialize(reg, e, cur);
                });
                json merged = (cur.contains(comp) && cur[comp].is_object()) ? cur[comp] : json::object();
                merged.update(data);
                // deserialize に emplace-only の型があるため、"上書き(set)" 実現には
                // 既存を remove してから登録済みデシリアライザで再生成する。
                if (!RemoveRegisteredComponent(reg, e, comp))
                    throw McpError(McpErr::UnknownComponent,
                        "unknown/unsupported component: " + comp + " (call dx12_describe_components)");
                json ej;
                ej[comp] = merged;        // deserialize は ej.contains(jsonKey) を見る形
                RuntimeComponentRegistry::Get().ForEach([&](const RuntimeComponentInfo& info) {
                    if (info.deserialize) info.deserialize(reg, e, ej);
                });
            }
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"component", comp}};
        });

    McpDefine("remove_component", "component:string,entity:int,name:string", DX12E_MCP_HANDLER
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            const std::string comp = params.value("component", std::string());
            if (comp == "transform" || comp == "name")
                throw McpError(McpErr::InvalidParam, "cannot remove core component (transform/name)");
            if (!RemoveRegisteredComponent(reg, e, comp))
                throw McpError(McpErr::UnknownComponent,
                    "unknown/unsupported component: " + comp + " (call dx12_describe_components)");
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"removed", comp}};
        });

    McpDefine("ui_tree", "", DX12E_MCP_HANDLER
        {
            // ゲーム内 UI ツリーを丸ごと JSON で返す（AI が UI 構造を「見る」ための読み取り API）。
            // 座標はキャンバス空間 px（= uiRect で指定する単位。ビューポート非依存）。
            auto& reg = m_scene->GetRegistry();

            std::vector<UiResolvedRect> rects;
            UISystem::ResolveRects(reg, 0.0f, 0.0f, 1920.0f, 1080.0f, rects);
            auto findRect = [&rects](entt::entity e) -> const UiResolvedRect* {
                for (const auto& rr : rects) if (rr.e == e) return &rr;
                return nullptr;
            };

            // 兄弟順つき子リスト（UISystem::ResolveAndDrawCanvases と同じ規約）
            std::unordered_map<entt::entity, std::vector<entt::entity>> children;
            for (auto [e, t] : reg.view<const Transform>().each())
                if (t.parent != entt::null && reg.valid(t.parent))
                    children[t.parent].push_back(e);
            for (auto& [p, list] : children)
                std::stable_sort(list.begin(), list.end(),
                                 [&reg](entt::entity a, entt::entity b)
                                 {
                                     const auto* ra = reg.try_get<UIRect>(a);
                                     const auto* rb = reg.try_get<UIRect>(b);
                                     return (ra ? ra->order : 0) < (rb ? rb->order : 0);
                                 });

            std::function<json(entt::entity)> makeNode = [&](entt::entity e) -> json {
                json n;
                n["entityId"] = static_cast<u32>(e);
                if (const auto* tag = reg.try_get<NameTag>(e)) n["name"] = tag->name;
                json kinds = json::array();
                if (reg.all_of<UICanvas>(e))     kinds.push_back("uiCanvas");
                if (reg.all_of<UIImage>(e))      kinds.push_back("uiImage");
                if (reg.all_of<UIText>(e))       kinds.push_back("uiText");
                if (reg.all_of<UIButton>(e))     kinds.push_back("uiButton");
                if (reg.all_of<UISlider>(e))     kinds.push_back("uiSlider");
                if (reg.all_of<UIToggle>(e))     kinds.push_back("uiToggle");
                if (reg.all_of<UIScrollView>(e)) kinds.push_back("uiScrollView");
                if (reg.all_of<UILayout>(e))     kinds.push_back("uiLayout");
                if (reg.all_of<UIAnimator>(e))   kinds.push_back("uiAnimator");
                n["components"] = std::move(kinds);
                if (const auto* r = reg.try_get<UIRect>(e))
                {
                    n["uiRect"] = {{"anchorMin", {r->anchorMin.x, r->anchorMin.y}},
                                   {"anchorMax", {r->anchorMax.x, r->anchorMax.y}},
                                   {"pivot", {r->pivot.x, r->pivot.y}},
                                   {"offsetMin", {r->offsetMin.x, r->offsetMin.y}},
                                   {"offsetMax", {r->offsetMax.x, r->offsetMax.y}},
                                   {"order", r->order}, {"visible", r->visible},
                                   {"clipChildren", r->clipChildren}};
                    if (r->rotation != 0.0f) n["uiRect"]["rotation"] = r->rotation;
                    if (r->skewX != 0.0f)    n["uiRect"]["skewX"] = r->skewX;
                    if (const UiResolvedRect* rr = findRect(e))
                    {
                        // 解決済み矩形をキャンバス空間 px へ（[x, y, w, h]、キャンバス左上原点）
                        const float cs = (rr->canvasScale > 1e-6f) ? rr->canvasScale : 1.0f;
                        n["resolvedRect"] = {(rr->min.x - rr->canvasOrigin.x) / cs,
                                             (rr->min.y - rr->canvasOrigin.y) / cs,
                                             (rr->max.x - rr->min.x) / cs,
                                             (rr->max.y - rr->min.y) / cs};
                    }
                }
                // 品質監査が「矩形だけ」ではなく可読性・過装飾・入力領域まで判断できるよう、
                // UI の見た目に関わる値を ui_tree に含める。テクスチャ本体等の重いデータは返さない。
                if (const auto* img = reg.try_get<UIImage>(e))
                {
                    n["uiImage"] = {{"texturePath", img->texturePath},
                                    {"color", {img->color.x, img->color.y, img->color.z, img->color.w}},
                                    {"cornerRadius", img->cornerRadius},
                                    {"raycastBlock", img->raycastBlock},
                                    {"shape", img->shape}, {"fillAmount", img->fillAmount},
                                    {"gradientDir", img->gradientDir},
                                    {"gradientScrollSpeed", img->gradientScrollSpeed},
                                    {"outlineWidth", img->outlineWidth},
                                    {"outlineStyle", img->outlineStyle},
                                    {"shadowAlpha", img->shadowColor.w},
                                    {"shadowSoftness", img->shadowSoftness}};
                }
                if (const auto* txt = reg.try_get<UIText>(e))
                {
                    n["text"] = txt->text;
                    n["uiText"] = {{"fontSize", txt->fontSize},
                                   {"color", {txt->color.x, txt->color.y, txt->color.z, txt->color.w}},
                                   {"alignH", txt->alignH}, {"alignV", txt->alignV},
                                   {"wrap", txt->wrap}, {"fontPath", txt->fontPath},
                                   {"outlineWidth", txt->outlineWidth},
                                   {"shadowAlpha", txt->shadowColor.w},
                                   {"letterSpacing", txt->letterSpacing},
                                   {"charAnim", txt->charAnim}, {"rich", txt->rich}};
                }
                if (const auto* btn = reg.try_get<UIButton>(e))
                    n["uiButton"] = {{"onClickEvent", btn->onClickEvent},
                                     {"interactable", btn->interactable}};
                if (const auto* lay = reg.try_get<UILayout>(e))
                    n["uiLayout"] = {{"mode", lay->mode}, {"cellW", lay->cellW},
                                     {"cellH", lay->cellH}, {"spacing", lay->spacing},
                                     {"padding", {lay->padding.x, lay->padding.y,
                                                  lay->padding.z, lay->padding.w}},
                                     {"gridCols", lay->gridCols}};
                if (const auto* anim = reg.try_get<UIAnimator>(e))
                    n["uiAnimator"] = {{"showAnim", anim->showAnim},
                                       {"showDuration", anim->showDuration},
                                       {"showDelay", anim->showDelay},
                                       {"hoverScale", anim->hoverScale},
                                       {"pressScale", anim->pressScale},
                                       {"loopAnim", anim->loopAnim},
                                       {"loopAmount", anim->loopAmount}};
                auto it = children.find(e);
                if (it != children.end() && !it->second.empty())
                {
                    json kids = json::array();
                    for (entt::entity c : it->second) kids.push_back(makeNode(c));
                    n["children"] = std::move(kids);
                }
                return n;
            };

            struct Entry { entt::entity e; int order; };
            std::vector<Entry> canvases;
            for (auto [e, cv] : reg.view<const UICanvas>().each())
                canvases.push_back({e, cv.sortOrder});
            std::stable_sort(canvases.begin(), canvases.end(),
                             [](const Entry& a, const Entry& b) { return a.order < b.order; });

            json arr = json::array();
            for (const Entry& c : canvases)
            {
                const auto& cv = reg.get<UICanvas>(c.e);
                json cn = makeNode(c.e);
                cn["uiCanvas"] = {{"refWidth", cv.refWidth}, {"refHeight", cv.refHeight},
                                  {"scaleMode", cv.scaleMode}, {"sortOrder", cv.sortOrder},
                                  {"visible", cv.visible}};
                arr.push_back(std::move(cn));
            }
            resp["ok"] = true;
            resp["result"] = {{"canvases", std::move(arr)},
                              {"note", "coordinates are canvas-space px (same units as uiRect offsets); "
                                       "resolvedRect = [x, y, w, h] from canvas top-left"}};
        });

    McpDefine("describe_components", "component:string", DX12E_MCP_HANDLER
        {
            const std::string only = params.value("component", std::string());
            json all = McpComponentSchema();
            if (only.empty())
            {
                resp["ok"] = true;
                resp["result"] = std::move(all);
            }
            else
            {
                json filtered = json::array();
                for (auto& c : all["components"])
                    if (c.value("jsonKey", std::string()) == only) filtered.push_back(c);
                if (filtered.empty())
                    throw McpError(McpErr::UnknownComponent, "unknown component: " + only);
                resp["ok"] = true;
                resp["result"] = {{"components", std::move(filtered)}};
            }
        });

    McpDefine("describe_lua_api", "", DX12E_MCP_HANDLER
        {
            // Lua スクリプトから使えるバインディング一覧(静的)。MCP のコンポーネントと
            // Lua から読める API のズレ(entity.boxCollider は nil 等)を AI に伝えるため。
            resp["ok"] = true;
            resp["result"] = McpLuaApi();
        });

    McpDefine("set_parent", "entity:int,name:string,parent:any", DX12E_MCP_HANDLER
        {
            auto& reg = m_scene->GetRegistry();
            const auto child = ResolveMcpEntity(*m_scene, params);   // entity か name で子を指定
            const u32 pid = params.value("parent", 0xFFFFFFFFu);
            entt::entity parent = entt::null;
            if (pid != 0xFFFFFFFFu)
            {
                parent = static_cast<entt::entity>(pid);
                if (!reg.valid(parent)) throw std::runtime_error("invalid parent id");
                // サイクル検出: parent の祖先鎖に child が現れたら拒否(O(N))。
                // 祖先鎖自体が既に相互参照で壊れていても抜けられるよう深さ上限を置く。
                int depth = 0;
                for (entt::entity cur = parent; cur != entt::null && depth < 4096; ++depth)
                {
                    if (cur == child) throw std::runtime_error("would create cycle");
                    auto* t = reg.try_get<Transform>(cur);
                    cur = t ? t->parent : entt::null;
                }
                if (depth >= 4096) throw std::runtime_error("parent chain broken (cycle)");
            }
            auto& t = reg.get_or_emplace<Transform>(child);
            t.parent = parent;   // 階層は Transform.parent が駆動。SerializeEntity に自動反映。
            resp["ok"] = true;
        });

    McpDefine("group_entities", "entities:any,name:string,names:any", DX12E_MCP_HANDLER
        {
            // 複数エンティティを空の親(グループ)へまとめる。ヒエラルキーの Ctrl+G と同じ動作。
            // 親は原点・無回転・スケール1で作るので、子のワールド行列は変わらない＝見た目は動かない。
            auto& reg = m_scene->GetRegistry();

            // entities(id 配列) と names(名前配列) の両方を受ける
            std::vector<entt::entity> targets;
            if (params.contains("entities"))
                for (const auto& v : params["entities"])
                {
                    auto e = static_cast<entt::entity>(v.get<u32>());
                    if (!reg.valid(e)) throw McpError(McpErr::NotFound,
                        "invalid entity id: " + std::to_string(v.get<u32>()));
                    targets.push_back(e);
                }
            if (params.contains("names"))
                for (const auto& v : params["names"])
                {
                    const std::string want = v.get<std::string>();
                    entt::entity found = entt::null;
                    for (auto [oe, tag] : reg.view<NameTag>().each())
                        if (tag.name == want) { found = oe; break; }
                    if (found == entt::null) throw McpError(McpErr::NotFound, "entity not found: " + want);
                    targets.push_back(found);
                }
            if (targets.empty()) throw std::runtime_error("missing 'entities' or 'names'");

            // 祖先も対象に含まれている子は除く(親ごと動くので二重に付け替えない)
            auto inTargets = [&](entt::entity e) {
                return std::find(targets.begin(), targets.end(), e) != targets.end();
            };
            std::vector<std::pair<entt::entity, entt::entity>> members;   // (子, 元の親)
            for (entt::entity e : targets)
            {
                if (!reg.all_of<Transform>(e)) continue;
                if (reg.all_of<GridPlane>(e)) continue;    // 内部用グリッドは巻き込まない
                bool ancestorIncluded = false;
                entt::entity cur = reg.get<Transform>(e).parent;
                for (int d = 0; cur != entt::null && reg.valid(cur) && d < 4096; ++d)
                {
                    if (inTargets(cur)) { ancestorIncluded = true; break; }
                    auto* pt = reg.try_get<Transform>(cur);
                    cur = pt ? pt->parent : entt::null;
                }
                if (ancestorIncluded) continue;
                if (std::find_if(members.begin(), members.end(),
                        [&](const auto& m) { return m.first == e; }) != members.end()) continue;  // 重複指定
                members.emplace_back(e, reg.get<Transform>(e).parent);
            }
            if (members.empty()) throw std::runtime_error("no groupable entities (all nested under each other?)");

            // 名前は重複したら連番(後から name 指定で引けるように)
            std::string base = params.value("name", std::string("Group"));
            if (base.empty()) base = "Group";
            auto taken = [&](const std::string& s) {
                for (auto [oe, tag] : reg.view<NameTag>().each()) if (tag.name == s) return true;
                return false;
            };
            std::string gname = base;
            for (int n = 2; taken(gname); ++n) gname = base + "_" + std::to_string(n);

            // 元の親が全員同じならグループもそこへ入れる(階層の位置を保つ)
            const entt::entity commonParent = members.front().second;
            const bool sameParent = std::all_of(members.begin(), members.end(),
                [&](const auto& m) { return m.second == commonParent; });

            entt::entity group = reg.create();
            reg.emplace<NameTag>(group, NameTag{gname});
            Transform gt{};
            if (sameParent) gt.parent = commonParent;
            reg.emplace<Transform>(group, gt);
            for (const auto& m : members)
                reg.get<Transform>(m.first).parent = group;

            // エディタの Ctrl+G と同じく Undo 可能にする(AI がまとめた整理を取り消せる)
            if (m_editorCtx)
            {
                m_editorCtx->undoSystem.PushCommand(std::make_unique<GroupCommand>(
                    &reg, gname, group, sameParent ? commonParent : entt::null, members));
                m_editorCtx->Select(group);
            }

            resp["ok"] = true;
            resp["result"] = nlohmann::json{
                {"groupId", static_cast<u32>(group)},
                {"name", gname},
                {"count", members.size()},
                {"sceneGeneration", m_sceneGeneration},
            };
            Logger::Info("MCP group_entities: '{}' に {} 件をまとめました", gname, members.size());
        });

    McpDefine("rename_entity", "entity:any,name:string", DX12E_MCP_HANDLER
        {
            // ここの "name" は新しい名前。エンティティ指定は entity(id) のみ(name 引きは曖昧なので不可)。
            const auto e = static_cast<entt::entity>(params.value("entity", 0xFFFFFFFFu));
            auto& reg = m_scene->GetRegistry();
            if (!reg.valid(e)) throw McpError(McpErr::NotFound, "invalid entity id");
            if (!reg.all_of<NameTag>(e)) throw McpError(McpErr::NotFound, "entity has no NameTag");
            std::string base = params.value("name", std::string());
            if (base.empty()) throw std::runtime_error("missing 'name'");
            auto taken = [&](const std::string& s) {
                for (auto [oe, tag] : reg.view<NameTag>().each())
                    if (oe != e && tag.name == s) return true;
                return false;
            };
            std::string name = base;
            int n = 2;            // 重複時は連番付与(MakeUniqueName 相当をインライン)
            while (taken(name)) name = base + "_" + std::to_string(n++);
            reg.get<NameTag>(e).name = name;
            resp["ok"] = true;
            resp["result"] = {{"name", name}};
        });

    McpDefine("ping", "", DX12E_MCP_HANDLER
        {
            int entityCount = 0;
            for (auto e : m_scene->GetRegistry().view<NameTag>()) { (void)e; ++entityCount; }
            resp["ok"] = true;
            resp["result"] = {
                {"pong", true},
                {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"},
                {"entityCount", entityCount},
                {"sceneGeneration", m_sceneGeneration},
                {"currentScene", ToAssetRel(m_editorCtx->currentScenePath)},
                // ★TS 側はこれまで「エンジンログに混ざる絶対パス」から assets ディレクトリを
                //   推定していた（#20-3）。ここで正確に返すので推定は不要。
                {"assetsDir", PathResolver::AssetsDir()},
                {"scriptsDir", PathResolver::ScriptsDir()},
                {"baseDir", PathResolver::BaseDir()},
                {"projectShaderDir", PathResolver::ProjectShaderDir()},
                {"cwd", std::filesystem::current_path().string()},
                {"protocolVersion", 4}
            };
        });

    McpDefine("find_entity", "name:string", DX12E_MCP_HANDLER
        {
            const std::string name = params.value("name", std::string());
            if (name.empty()) throw McpError(McpErr::InvalidParam, "missing 'name'");
            auto ent = m_scene->FindEntity(name);
            resp["ok"] = true;
            if (ent.IsValid())
                resp["result"] = {{"entityId", static_cast<u32>(ent.GetHandle())}, {"name", name}};
            else
                resp["result"] = nullptr;
        });

    McpDefine("query_entities", "box:any,tag:string", DX12E_MCP_HANDLER
        {
            auto& reg = m_scene->GetRegistry();
            const std::string tag = params.value("tag", std::string());
            std::vector<entt::entity> hits;
            if (params.contains("box") && params["box"].is_array() && params["box"].size() == 4)
            {
                auto b = params["box"].get<std::vector<float>>();
                hits = m_scene->QueryInBox(b[0], b[1], b[2], b[3], tag);  // minX,minZ,maxX,maxZ
            }
            else if (!tag.empty())
            {
                hits = m_scene->QueryByTag(tag);
            }
            else
            {
                throw McpError(McpErr::InvalidParam, "provide 'tag' and/or 'box':[minX,minZ,maxX,maxZ]");
            }
            json arr = json::array();
            for (auto e : hits)
            {
                if (!reg.valid(e)) continue;
                std::string nm = reg.all_of<NameTag>(e) ? reg.get<NameTag>(e).name : std::string();
                arr.push_back({{"entityId", static_cast<u32>(e)}, {"name", nm}});
            }
            resp["ok"] = true;
            resp["result"] = {{"entities", arr}, {"count", arr.size()}};
        });

    McpDefine("select_entity", "entity:int,name:string", DX12E_MCP_HANDLER
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            m_editorCtx->Select(e);
            resp["ok"] = true;
            resp["result"] = {{"selected", static_cast<u32>(e)}};
        });

    McpDefine("focus_camera", "entity:int,name:string", DX12E_MCP_HANDLER
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<Transform>(e))
                throw McpError(McpErr::NotFound, "entity has no Transform");
            const auto& t = reg.get<Transform>(e);
            float dist = 8.0f;
            if (reg.all_of<MeshRenderer>(e))
            {
                const auto& mr = reg.get<MeshRenderer>(e);
                float maxExtent = 0.0f;
                for (const auto* mesh : mr.meshes)
                {
                    if (!mesh) continue;
                    auto mn = mesh->GetAABBMin();
                    auto mx = mesh->GetAABBMax();
                    maxExtent = std::max({maxExtent,
                        (mx.x - mn.x) * t.scale.x, (mx.y - mn.y) * t.scale.y, (mx.z - mn.z) * t.scale.z});
                }
                if (maxExtent > 0.0f) dist = std::clamp(maxExtent * 2.0f, 2.0f, 100.0f);
            }
            DirectX::XMFLOAT3 wpos = t.position;
            if (t.parent != entt::null && reg.valid(t.parent))
            {
                DirectX::XMFLOAT4X4 wf;
                DirectX::XMStoreFloat4x4(&wf, ComputeWorldMatrix(reg, e));
                wpos = { wf._41, wf._42, wf._43 };
            }
            auto fwd = m_camera->GetForward();
            DirectX::XMFLOAT3 camPos{ wpos.x - fwd.x * dist, wpos.y - fwd.y * dist, wpos.z - fwd.z * dist };
            m_camera->SetPosition(camPos);
            resp["ok"] = true;
            resp["result"] = {{"cameraPos", {camPos.x, camPos.y, camPos.z}},
                              {"target", {wpos.x, wpos.y, wpos.z}}, {"distance", dist}};
        });

    McpDefine("set_pbr", "entity:int,metallic:any,name:string,roughness:any,uvScaleU:any,uvScaleV:any", DX12E_MCP_HANDLER
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<MeshRenderer>(e))
                throw McpError(McpErr::NotFound, "entity has no MeshRenderer");
            auto& mr = reg.get<MeshRenderer>(e);
            if (params.contains("metallic"))  mr.overrideMetallic  = params["metallic"].get<float>();
            if (params.contains("roughness")) mr.overrideRoughness = params["roughness"].get<float>();
            if (params.contains("uvScaleU"))  mr.uvScaleU = params["uvScaleU"].get<float>();
            if (params.contains("uvScaleV"))  mr.uvScaleV = params["uvScaleV"].get<float>();
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)},
                              {"metallic", mr.overrideMetallic}, {"roughness", mr.overrideRoughness},
                              {"uvScaleU", mr.uvScaleU}, {"uvScaleV", mr.uvScaleV}};
        });

    McpDefine("duplicate_entity", "entity:int,name:string", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot duplicate while Playing; call dx12_stop first");
            const auto e = ResolveMcpEntity(*m_scene, params);
            m_editorCtx->mcpDuplications.push_back(McpPendingDelete{ e, deferred });  // .entity=複製元
            isDeferred = true;
        });

    McpDefine("undo", "", DX12E_MCP_HANDLER
        {
            m_editorCtx->pendingUndo = true;   // フレーム境界で適用
            resp["ok"] = true;
            resp["result"] = {{"queuedUndo", true}};
        });

    McpDefine("redo", "", DX12E_MCP_HANDLER
        {
            m_editorCtx->pendingRedo = true;
            resp["ok"] = true;
            resp["result"] = {{"queuedRedo", true}};
        });

    McpDefine("new_scene", "savePath:string", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot create a new scene while Playing");
            std::string rel = params.value("savePath", std::string());
            if (!rel.empty())
            {
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw McpError(McpErr::InvalidParam, "invalid savePath (assets 相対のみ)");
                m_editorCtx->pendingNewScenePath = PathResolver::AssetsDir() + rel;
            }
            m_editorCtx->pendingNewScene = true;   // フレーム境界で空シーン生成 + sceneGeneration++
            resp["ok"] = true;
            resp["result"] = {{"applied", "next frame"}};
        });

    McpDefine("spawn_prefab", "name:string,path:string,position:any", DX12E_MCP_HANDLER
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot spawn while Playing; call dx12_stop first");
            std::string path = params.value("path", std::string());
            if (path.empty()) throw McpError(McpErr::InvalidParam, "missing 'path'");
            if (path.front() == '/' || path.find('\\') != std::string::npos ||
                path.find(':') != std::string::npos || path.find("..") != std::string::npos)
                throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
            if (fs::path(path).extension() != ".prefab")
                throw McpError(McpErr::InvalidParam, "path must be a .prefab");
            if (!fs::exists(PathResolver::AssetsDir() + path))
                throw McpError(McpErr::NotFound, "prefab not found: " + path);
            const auto pos = params.value("position", std::vector<float>{0.0f, 0.0f, 0.0f});
            if (pos.size() != 3) throw McpError(McpErr::InvalidParam, "position must be [x,y,z]");
            // ★idempotency のリプレイ判定（#20-4。ここが無かったので実バグだった）。
            //   スポーン完了時に root id を m_mcpIdempotency へ記録してはいたが、
            //   **再送されたときに参照する側が無く、同じ key で何度でも重複生成できた**。
            //   create_entity / spawn_model と同じ規律に揃える。
            if (!deferred.idempotencyKey.empty())
            {
                auto& reg = m_scene->GetRegistry();
                auto it = m_mcpIdempotency.find(deferred.idempotencyKey);
                if (it != m_mcpIdempotency.end() &&
                    reg.valid(static_cast<entt::entity>(it->second)))
                {
                    const auto root = static_cast<entt::entity>(it->second);
                    json ids = json::array();
                    ids.push_back(static_cast<u32>(root));
                    for (auto c : reg.view<const Transform>())
                        if (c != root && McpIsDescendantOf(reg, c, root)) ids.push_back(static_cast<u32>(c));
                    resp["ok"] = true;
                    resp["result"] = {{"entityId", it->second}, {"rootEntityId", it->second},
                                      {"entityIds", ids},
                                      {"name", reg.all_of<NameTag>(root) ? reg.get<NameTag>(root).name
                                                                         : std::string()},
                                      {"sceneGeneration", m_sceneGeneration}, {"idempotentReplay", true}};
                    return;
                }
            }
            PendingSpawnRequest sreq;
            sreq.modelPath = path;          // 拡張子 .prefab で spawn ループが展開し root+ids を返す
            sreq.position  = { pos[0], pos[1], pos[2] };
            sreq.name      = params.value("name", std::string());
            sreq.mcp       = deferred;
            m_editorCtx->pendingSpawns.push_back(std::move(sreq));
            isDeferred = true;
        });
}



} // namespace dx12e
