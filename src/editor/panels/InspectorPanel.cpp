#include "editor/panels/InspectorPanel.h"
#include "editor/EditorContext.h"
#include "editor/PropertyGrid.h"
#include "editor/UiEditUtil.h"
#include "editor/UndoSystem.h"
#include "editor/AssetDrop.h"
#include "ecs/Components.h"
#include "renderer/Camera.h"
#include "renderer/Material.h"
#include "renderer/Mesh.h"
#include "audio/AudioSystem.h"
#include "physics/PhysicsDebugRenderer.h"
#include "core/GameClock.h"
#include "scene/Scene.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"
#include "animation/NodeAnimator.h"
#include "animation/SkinningBuffer.h"
#include "animation/AnimGraphRuntime.h"
#include "scripting/ScriptEngine.h"
#include "resource/ShaderRegistry.h"
#include "resource/ShaderDiagnostics.h"
#include "resource/ShaderParams.h"
#include "resource/MaterialAssetIO.h"
#include "editor/panels/AssetBrowserPanel.h"

#include <imgui_internal.h>   // BeginDragDropTargetCustom（ウィンドウ全体をドロップ先に）
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

#pragma warning(push)
#pragma warning(disable: 4100)
#include <Windows.h>
#include <shellapi.h>
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <DirectXMath.h>

namespace
{

// 「この項目は今の設定では効かない」を出す注記行（pg テーブルの中で使う）。
//
// ★ImGui::TextColored の 1 行では**パネル幅で右端が切れて読めない**（実際に切れていた）。
//   値セル側（col 1）は幅が広く、そこで折り返せば全文が出る。ラベル列は空にする。
// ★色はこの用途で既に使っていた橙（1.0, 0.65, 0.2）に統一。
void WarnTextV(const char* fmt, va_list args)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.2f, 1.0f));
    ImGui::PushTextWrapPos(0.0f);   // 現在の作業矩形の右端で折り返す
    ImGui::TextV(fmt, args);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// pg テーブルの中で使う（値セル側は幅が広いので col 1 に置く）。
void WarnRow(const char* fmt, ...)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    va_list args; va_start(args, fmt); WarnTextV(fmt, args); va_end(args);
}

// pg::End() の後（テーブル外）で使う。
void WarnText(const char* fmt, ...)
{
    va_list args; va_start(args, fmt); WarnTextV(fmt, args); va_end(args);
}


// ===== カスタムシェーダーの「名前付きパラメーター」を描く =====
//
// HLSL の cbuffer b0 の自由枠(オフセット 128..159 = float 8 個)に書かれた変数を、
// DXIL リフレクションで拾った【本人が付けた名前】のまま並べる（resource/ShaderParams.h）。
//
//     float  _Glow;        // @range(0,4)   → 0..4 のスライダー「_Glow」
//     float3 _TintColor;   // @color        → カラーピッカー「_TintColor」（名前でも自動判定）
//
// 値の置き場は従来と同じ MeshRenderer の 8 float（CustomParamBase() が先頭）なので、
// ルート定数もシーン JSON も変わらない。リフレクションが取れないとき（未コンパイル /
// 配布ビルドで .cso しか無い等）は false を返し、呼び出し側が従来の固定 2 行へ落ちる。
bool DrawNamedShaderParams(const std::string& shaderRel, float* base)
{
    // この無名 namespace は dx12e の外なので、エンジン側の名前は明示的に引く。
    namespace sp = dx12e::shaderparams;
    namespace pg = dx12e::pg;

    const std::vector<sp::Param> params = sp::Get(dx12e::shaderdiag::NormalizeKey(shaderRel));
    if (params.empty()) return false;

    for (const sp::Param& p : params)
    {
        float* v = base + p.Index();
        const char* label = p.name.c_str();
        // どの宣言から生えた行なのかが一目で分かるようにしておく（HLSL を見に行かなくて済む）。
        char tip[192];
        std::snprintf(tip, sizeof(tip), "HLSL: %s %s;  (cbuffer b0 の +%u バイト)",
                      p.typeName.empty() ? "?" : p.typeName.c_str(), label, p.offset);

        const float mn = p.hasRange ? p.minV : 0.0f;
        const float mx = p.hasRange ? p.maxV : 0.0f;   // mn==mx==0 で DragFloat は無制限になる
        switch (p.kind)
        {
        case sp::Kind::Float:
            if (p.hasRange) pg::SliderFloat(label, v, p.minV, p.maxV, "%.3f", nullptr, tip);
            else            pg::Float(label, v, 0.005f, 0.0f, 0.0f, "%.3f", nullptr, tip);
            break;
        case sp::Kind::Float2: pg::Float2(label, v, 0.01f, mn, mx, "%.3f", nullptr, tip); break;
        case sp::Kind::Float3: pg::Float3(label, v, 0.01f, mn, mx, "%.3f", nullptr, tip); break;
        case sp::Kind::Float4: pg::Float4(label, v, 0.01f, mn, mx, "%.3f", nullptr, tip); break;
        case sp::Kind::Color3: pg::Color3(label, v); break;
        case sp::Kind::Color4: pg::Color4(label, v); break;
        default:
            // int / bool / 行列 / 配列。値の実体が float なので編集させない（黙って壊すより出す）。
            pg::Text(label, "%s は編集非対応（float / float2 / float3 / float4 のみ）",
                     p.typeName.empty() ? "この型" : p.typeName.c_str());
            break;
        }
    }
    return true;
}

// ===== カスタムシェーダーの「なぜ効かないか」をその場に出す =====
//
// ★従来はコンパイルエラーも PSO 生成失敗も dx12_engine.log にしか出ず、シェーダーを
//   割り当てた本人には「割り当てたのに何も起きない」としか見えなかった。
//   割り当てた場所（Inspector）に、原因と使えるレジスタ＝書式をそのまま出す。
//
// shaderRel が空なら何も描かない。issue はエンジン側（ShaderManager / PSO 生成）が積む。
void ShaderIssueBox(const std::string& shaderRel, const char* contractId, const char* idSuffix)
{
    namespace sd = dx12e::shaderdiag;

    const std::string issue = shaderRel.empty() ? std::string() : sd::GetIssue(shaderRel);
    const std::string help  = sd::GetHelp(contractId);
    if (issue.empty() && help.empty()) return;

    if (!issue.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.38f, 1.0f));
        ImGui::TextUnformatted("このシェーダーは効いていません（既定の描画に戻しています）");
        ImGui::PopStyleColor();

        // ★読み取り専用の複数行テキストにする＝マウスでドラッグ選択して Ctrl+C できる。
        //   （TextUnformatted だと目で読むしかなく、行番号やレジスタ名を写経する羽目になる）
        //   バッファは ImGui が書き換えないので const_cast で渡してよい（コンソールの
        //   詳細ペインと同じ流儀）。
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.16f, 0.10f, 0.10f, 1.0f));
        const float h = ImGui::GetTextLineHeightWithSpacing() * 12.0f;
        ImGui::InputTextMultiline((std::string("##shaderIssue") + idSuffix).c_str(),
                                  const_cast<char*>(issue.c_str()), issue.size() + 1,
                                  ImVec2(-1.0f, h), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();

        if (ImGui::Button((std::string("全文をコピー##") + idSuffix).c_str()))
            ImGui::SetClipboardText(issue.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("上の枠内はドラッグで部分選択 → Ctrl+C でもコピーできます");
        if (!help.empty()) ImGui::SameLine();
    }

    if (help.empty()) return;

    const std::string popupId = std::string("シェーダーの書式##") + idSuffix;
    if (ImGui::Button((std::string("書式を見る##") + idSuffix).c_str()))
        ImGui::OpenPopup(popupId.c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("このシェーダーで宣言してよいレジスタと約束事の一覧");

    ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(popupId.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        // ここも読み取り専用テキスト＝レジスタ名をそのまま選択コピーして HLSL へ貼れる。
        ImGui::InputTextMultiline("##helpBody", const_cast<char*>(help.c_str()), help.size() + 1,
                                  ImVec2(-1.0f, -ImGui::GetFrameHeightWithSpacing()),
                                  ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("全文をコピー", ImVec2(140.0f, 0.0f)))
            ImGui::SetClipboardText(help.c_str());
        ImGui::SameLine();
        if (ImGui::Button("閉じる", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// CollapsingHeader を Unreal 風カテゴリ帯（濃いグレー地＋左端アクセントストライプ）で描くヘルパ。
// tex 指定でラベル頭に種別アイコンを重ね描きする。
bool IconHeader(const dx12e::EditorUiIcons* ic, dx12e::u64 tex, const char* label,
                ImGuiTreeNodeFlags flags = 0)
{
    namespace th = dx12e::theme;
    ImGui::PushStyleColor(ImGuiCol_Header,        th::GroupBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, th::Hex(0x272831));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  th::Hex(0x2b2c36));
    ImGui::PushStyleColor(ImGuiCol_Text,          th::TextHi);

    bool open;
    if (!ic || !tex)
    {
        open = ImGui::CollapsingHeader(label, flags);
    }
    else
    {
        const float h = ImGui::GetTextLineHeight();
        const float spaceW = ImGui::CalcTextSize(" ").x;
        const int pad = (spaceW > 0.0f) ? static_cast<int>((h + 6.0f) / spaceW) + 1 : 3;
        std::string padded(static_cast<size_t>(pad), ' ');
        padded += label;
        open = ImGui::CollapsingHeader(padded.c_str(), flags);
    }
    ImGui::PopStyleColor(4);

    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    // 左端のアクセントストライプ（カテゴリの目印。Unreal の Details 風）
    ImGui::GetWindowDrawList()->AddRectFilled(
        mn, ImVec2(mn.x + 3.0f, mx.y), ImGui::GetColorU32(th::Accent));

    if (ic && tex)
    {
        const float h = ImGui::GetTextLineHeight();
        const float cy = (mn.y + mx.y) * 0.5f;
        const float x  = mn.x + ImGui::GetTreeNodeToLabelSpacing();
        ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(tex),
            ImVec2(x, cy - h * 0.5f), ImVec2(x + h, cy + h * 0.5f));
    }
    return open;
}

// エンティティの構成から代表アイコンを選ぶ（Inspector 上部の見出し用）
dx12e::u64 PickEntityIcon(entt::registry& reg, entt::entity e, const dx12e::EditorUiIcons& ic)
{
    using namespace dx12e;
    if (reg.all_of<CameraComponent>(e))                       return ic.entCamera;
    if (reg.any_of<PointLight, DirectionalLight, SpotLight>(e)) return ic.entLight;
    if (reg.any_of<UICanvas, UIRect, UIImage, UIText, UIButton, UIAnimator>(e)) return ic.entUi;
    if (reg.all_of<MeshRenderer>(e))                          return ic.entMesh;
    if (reg.all_of<AudioSource>(e))                           return ic.entAudio;
    if (reg.any_of<RigidBody, BoxCollider, SphereCollider,
                   CapsuleCollider, ConvexHullCollider, CharacterController>(e))   return ic.entPhysics;
    if (reg.all_of<LuaScript>(e))                             return ic.entScript;
    return ic.entEmpty;
}

// assets 配下の .lua（= スクリプトコンポーネント候補）を列挙する。
// game.lua は除外。戻り値は assets 相対パス（スラッシュ区切り、例 "components/Spike.lua"）。
// メニュー/ポップアップが開いている間だけ呼ばれる想定（開いてる時しか走らないので軽い）。
std::vector<std::string> ScanScriptComponents(const std::string& assetsDir)
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    fs::path base(assetsDir);
    if (base.empty() || !fs::exists(base, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(base, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) break;
        const auto& p = it->path();
        if (!fs::is_regular_file(p, ec)) continue;
        if (p.extension() != ".lua") continue;
        if (p.filename() == "game.lua") continue;
        std::string rel = fs::relative(p, base, ec).generic_string();
        if (!rel.empty()) out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// スクリプト選択 UI。assets の .lua を列挙し、クリックで pendingScriptAttachments に積む
// （フレーム境界で Undo 付きアタッチ）。選んだら true。
bool ScriptPicker(dx12e::EditorContext& ctx, entt::entity e, const std::string& assetsDir)
{
    bool picked = false;
    auto scripts = ScanScriptComponents(assetsDir);
    if (scripts.empty())
    {
        ImGui::TextDisabled("assets/components に .lua がありません");
        ImGui::TextDisabled("（AssetBrowser の「新規スクリプト」で作成）");
        return false;
    }
    for (const auto& rel : scripts)
    {
        if (ImGui::Selectable(rel.c_str()))
        {
            ctx.pendingScriptAttachments.push_back({ e, rel });
            picked = true;
        }
    }
    return picked;
}

// スキーマに合わせて ls.props を並べ替え/補完する（既存値は名前で引き継ぎ、
// 宣言から消えたプロパティは捨てる）。これで Inspector は常にスキーマ通りに描ける。
void SyncScriptProps(dx12e::LuaScript& ls, const std::vector<dx12e::ScriptPropDef>& schema)
{
    std::vector<dx12e::ScriptProp> next;
    next.reserve(schema.size());
    for (const auto& d : schema)
    {
        dx12e::ScriptProp p;
        p.name = d.name;
        p.type = d.type;
        const dx12e::ScriptProp* old = nullptr;
        for (auto& ex : ls.props)
            if (ex.name == d.name && ex.type == d.type) { old = &ex; break; }
        if (old) { p.num = old->num; p.b = old->b; p.str = old->str; p.vec = old->vec; }
        else     { p.num = d.def.num; p.b = d.def.b; p.str = d.def.str; p.vec = d.def.vec; }
        next.push_back(std::move(p));
    }
    ls.props = std::move(next);
}

void DrawLuaScriptSection(entt::registry& reg,
                          entt::entity e,
                          dx12e::EditorContext& ctx,
                          dx12e::ScriptEngine* scriptEngine,
                          const std::string& assetsDir)
{
    const bool hasLua = reg.all_of<dx12e::LuaScript>(e);

    bool open = IconHeader(nullptr, 0, "Lua Script",
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

    // ヘッダ右の状態表示: 付いてたら緑、無ければグレー（テキスト幅から右寄せ位置を計算）
    {
        const char* status = hasLua ? "ATTACHED" : "(none)";
        ImGui::SameLine(ImGui::GetWindowWidth()
                        - ImGui::CalcTextSize(status).x
                        - ImGui::GetStyle().WindowPadding.x - 24.0f);
        if (hasLua)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), status);
        else
            ImGui::TextDisabled(status);
    }

    if (!open) return;

    if (!hasLua)
    {
        if (ImGui::Button("＋ スクリプトを付ける", ImVec2(-1, 0)))
            ImGui::OpenPopup("PickScriptPopup");
        ImGui::TextDisabled("または AssetBrowser から .lua をドラッグ");
        if (ImGui::BeginPopup("PickScriptPopup"))
        {
            ImGui::TextDisabled("付けるスクリプトを選ぶ:");
            ImGui::Separator();
            if (ScriptPicker(ctx, e, assetsDir)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        return;
    }

    auto& ls = reg.get<dx12e::LuaScript>(e);

    // Script path (read-only)
    char pathBuf[256];
    std::memset(pathBuf, 0, sizeof(pathBuf));
    strncpy_s(pathBuf, sizeof(pathBuf), ls.scriptPath.c_str(), _TRUNCATE);
    if (dx12e::pg::Begin("LuaScriptHead"))
    {
        dx12e::pg::InputText("Script", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_ReadOnly);
        dx12e::pg::Label("有効 Enabled");
        // ★戻り値を捨てていたので Undo にも積まれず未保存フラグも立たなかった。
        //   ls.enabled はシリアライズされるのに、切り替えてもタイトルに `*` が出ず、
        //   閉じる/別シーンを開くと確認も無しに元へ戻っていた。
        {
            const bool before = ls.enabled;
            if (ImGui::Checkbox("##lsEnabled", &ls.enabled))
            {
                dx12e::LuaScript snapshot = ls;
                snapshot.enabled = before;
                ctx.undoSystem.PushCommand(std::make_unique<dx12e::ComponentEditCommand<dx12e::LuaScript>>(
                    &reg, e, snapshot, ls, "Script Enabled"));
            }
        }
        ImGui::SameLine();
        ImGui::TextColored(
            ls.started ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            ls.started ? "RUNNING" : "IDLE");
        dx12e::pg::End();
    }

    if (ImGui::Button("Reload##LuaScript"))
    {
        if (scriptEngine) scriptEngine->ReloadScript(e);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open in Editor##LuaScript"))
    {
        namespace fs = std::filesystem;
        fs::path abs = fs::path(assetsDir) / ls.scriptPath;
        ShellExecuteA(nullptr, "open", abs.string().c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("\xe5\xa4\x89\xe6\x9b\xb4##LuaScript"))  // 変更（別スクリプトへ差し替え）
        ImGui::OpenPopup("ChangeScriptPopup");
    if (ImGui::BeginPopup("ChangeScriptPopup"))
    {
        ImGui::TextDisabled("\xe5\x88\xa5\xe3\x81\xae\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88\xe3\x81\xb8\xe5\xa4\x89\xe6\x9b\xb4:");  // 別のスクリプトへ変更:
        ImGui::Separator();
        if (ScriptPicker(ctx, e, assetsDir)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Detach##LuaScript"))
    {
        // ★Undo を積んでいなかった。押すと LuaScript が公開プロパティごと消え、
        //   Ctrl+Z で戻らない（しかも代わりに 1 つ前の別操作が巻き戻る）。
        //   DetachScriptCommand は前から定義済みなのにどこからも使われていなかった。
        ctx.undoSystem.PushCommand(std::make_unique<dx12e::DetachScriptCommand>(
            &reg, e, ls.scriptPath, ls.enabled, ls.props));
        if (scriptEngine) scriptEngine->DetachScriptFromEntity(e);
        return;
    }

    if (ls.loadError)
    {
        // errorMessage は sol2 の traceback 込み。ログを見に行かせず、ここで本文を出す。
        // 長いので折り返し + コピーボタン（そのまま AI に貼れるように）。
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Lua エラー");
        if (!ls.errorMessage.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.5f, 1.0f));
            ImGui::TextWrapped("%s", ls.errorMessage.c_str());
            ImGui::PopStyleColor();
            if (ImGui::SmallButton("エラーをコピー##LuaScript"))
                ImGui::SetClipboardText(ls.errorMessage.c_str());
            ImGui::SameLine();
        }
        ImGui::TextDisabled(".lua を保存し直すと自動で読み直します");
    }

    // --- 公開プロパティ（.lua の properties 宣言からスキーマ駆動で自動生成）---
    // ここを編集すると各エンティティ個別の値としてシーンに保存され、Play 時に
    // self.<name> としてスクリプトへ注入される（巨大コントローラ不要の部品化）。
    if (scriptEngine)
    {
        const auto& schema = scriptEngine->GetPropertySchema(ls.scriptPath);
        if (!schema.empty())
        {
            SyncScriptProps(ls, schema);
            ImGui::SeparatorText("プロパティ");
            // ★Lua の公開プロパティはシーン JSON に保存されるのに、この経路は
            //   Undo も MarkEdited も呼んでいなかった。UndoSystem.h の MarkEdited は
            //   「Undo を積まない変更経路（Lua プロパティ等）から呼ぶ」契約なのに
            //   その呼び出しが無く、**編集しても未保存フラグが立たない**＝
            //   確認ダイアログ無しで閉じると消える。まず契約に合わせる。
            bool propsEdited = false;
            if (dx12e::pg::Begin("LuaProps"))
            {
                for (size_t i = 0; i < schema.size(); ++i)
                {
                    const auto& d = schema[i];
                    auto& p = ls.props[i];
                    const char* lbl = d.label.empty() ? d.name.c_str() : d.label.c_str();
                    ImGui::PushID(static_cast<int>(i));
                    switch (d.type)
                    {
                    case dx12e::ScriptPropType::Float:
                    {
                        float v = static_cast<float>(p.num);
                        bool ch = d.hasRange ? dx12e::pg::SliderFloat(lbl, &v, d.minVal, d.maxVal)
                                             : dx12e::pg::Float(lbl, &v, 0.01f);
                        if (ch) { p.num = v; propsEdited = true; }
                        break;
                    }
                    case dx12e::ScriptPropType::Int:
                    {
                        int v = static_cast<int>(p.num);
                        bool ch = d.hasRange ? dx12e::pg::SliderInt(lbl, &v, static_cast<int>(d.minVal), static_cast<int>(d.maxVal))
                                             : dx12e::pg::Int(lbl, &v);
                        if (ch) { p.num = v; propsEdited = true; }
                        break;
                    }
                    case dx12e::ScriptPropType::Bool:
                        if (dx12e::pg::Checkbox(lbl, &p.b)) propsEdited = true;
                        break;
                    case dx12e::ScriptPropType::String:
                    {
                        char buf[256];
                        std::memset(buf, 0, sizeof(buf));
                        strncpy_s(buf, sizeof(buf), p.str.c_str(), _TRUNCATE);
                        if (dx12e::pg::InputText(lbl, buf, sizeof(buf)))
                        { p.str = buf; propsEdited = true; }
                        break;
                    }
                    case dx12e::ScriptPropType::Vec3:
                        if (dx12e::pg::Float3(lbl, &p.vec.x, 0.01f)) propsEdited = true;
                        break;
                    case dx12e::ScriptPropType::Color:
                        if (dx12e::pg::Color3(lbl, &p.vec.x)) propsEdited = true;
                        break;
                    case dx12e::ScriptPropType::Entity:
                    {
                        // シーン内のエンティティから選ぶコンボ。
                        // ★参照の正は guid。名前だけ書き換えると古い guid が勝って
                        //   コンボの操作が黙って無視されるので、必ず両方を同時に書く。
                        const auto setEntRef = [&reg](entt::entity picked, dx12e::ScriptProp& pp) {
                            if (picked == entt::null) { pp.str.clear(); pp.guid = 0; return; }
                            const auto* n = reg.try_get<dx12e::NameTag>(picked);
                            pp.str = n ? n->name : std::string{};
                            const auto* g = reg.try_get<dx12e::EntityGuid>(picked);
                            pp.guid = g ? g->value : 0;   // 保存前なら 0（次の保存で付く）
                        };
                        const char* cur = p.str.empty() ? "(なし)" : p.str.c_str();
                        dx12e::pg::Label(lbl);
                        if (ImGui::BeginCombo("##ent", cur))
                        {
                            if (ImGui::Selectable("(なし)", p.str.empty()))
                            { setEntRef(entt::null, p); propsEdited = true; }
                            auto nameView = reg.view<dx12e::NameTag>();
                            for (auto ne : nameView)
                            {
                                const auto& nm = nameView.get<dx12e::NameTag>(ne).name;
                                if (nm.empty()) continue;
                                if (ImGui::Selectable(nm.c_str(), nm == p.str))
                                { setEntRef(ne, p); propsEdited = true; }
                            }
                            ImGui::EndCombo();
                        }
                        break;
                    }
                    }
                    ImGui::PopID();
                }
                dx12e::pg::End();
            }
            if (ImGui::SmallButton("規定値に戻す"))
            {
                for (size_t i = 0; i < schema.size(); ++i)
                {
                    auto& p = ls.props[i]; const auto& d = schema[i];
                    p.num = d.def.num; p.b = d.def.b; p.str = d.def.str; p.vec = d.def.vec;
                }
                propsEdited = true;   // 1 クリックで全プロパティが変わる。ここが一番失いやすい
            }
            if (propsEdited) ctx.undoSystem.MarkEdited();
        }
        else
        {
            ImGui::TextDisabled("properties = {...} を書くとここに編集欄が出ます");
        }
    }
}

} // anonymous namespace

namespace dx12e
{

namespace
{

// ── コンポーネント編集 Undo 追跡ヘルパー ──
// 描画前に BeginEdit でスナップショットを取り、描画後に EndEdit で
// 「即時変更 (checkbox/combo)」または「ドラッグ終了」を検出して Undo に積む
template<typename T>
void BeginEdit(entt::registry& reg, entt::entity e, InspectorPanel::EditState<T>& state)
{
    if (!state.editing)
        state.snapshot = reg.get<T>(e);
}

template<typename T>
void EndEdit(entt::registry& reg, EditorContext& ctx, entt::entity e,
             InspectorPanel::EditState<T>& state,
             bool changed, bool active, const char* name)
{
    auto push = [&]()
    {
        const T& cur = reg.get<T>(e);
        // ★operator== があるならそちらを使う。std::string / std::vector を含む
        //   コンポーネント（Trigger 等）は memcmp が中身ではなくポインタを見るので、
        //   変わっていなくても「変わった」と判定して Undo エントリを量産する。
        bool same;
        if constexpr (requires (const T& a, const T& b) { { a == b } -> std::convertible_to<bool>; })
            same = (state.snapshot == cur);
        else
            same = (std::memcmp(&state.snapshot, &cur, sizeof(T)) == 0);
        if (!same)
        {
            ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<T>>(
                &reg, e, state.snapshot, cur, name));
        }
    };

    if (active)
    {
        state.editing = true;          // ドラッグ継続中
    }
    else if (changed)
    {
        push();                        // 即時変更 (checkbox/combo/単発編集)
        state.editing = false;
    }
    else if (state.editing && !ImGui::IsAnyItemActive())
    {
        push();                        // ドラッグ終了
        state.editing = false;
    }
}

// ── ヘッダ右クリックでコンポーネント削除（Undo 対応）。削除したら true ──
template<typename T>
bool ComponentRemoveMenu(entt::registry& reg, EditorContext& ctx,
                         entt::entity e, const char* name)
{
    bool removed = false;
    ImGui::PushID(name);
    if (ImGui::BeginPopupContextItem("##RemoveComponent"))
    {
        // コンポーネント削除
        if (ImGui::MenuItem("\xe3\x82\xb3\xe3\x83\xb3\xe3\x83\x9d\xe3\x83\xbc\xe3\x83\x8d\xe3\x83\xb3\xe3\x83\x88\xe5\x89\x8a\xe9\x99\xa4"))
        {
            ctx.undoSystem.PushCommand(std::make_unique<RemoveComponentCommand<T>>(
                &reg, e, reg.get<T>(e), name));
            reg.remove<T>(e);
            removed = true;
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return removed;
}

// ── 光の照らす方向を編集する UI（DragFloat3 + プリセット + 正規化） ──
// changed を返し、ドラッグ中は active を立てる（Undo 追跡用）。
bool DirectionEditor(const char* id, DirectX::XMFLOAT3& dir, bool& active)
{
    bool changed = false;
    ImGui::PushID(id);
    ImGui::TextUnformatted("照らす方向 Direction");
    ImGui::SetNextItemWidth(-FLT_MIN);
    changed |= ImGui::DragFloat3("##dir", &dir.x, 0.01f, -1.0f, 1.0f, "%.2f");
    active  |= ImGui::IsItemActive();

    struct Preset { const char* label; float x, y, z; };
    static const Preset presets[] = {
        {"真下", 0.0f, -1.0f,  0.0f},
        {"斜め", -0.4f, -1.0f, -0.4f},
        {"横",   1.0f,  0.0f,  0.0f},
        {"前",   0.0f,  0.0f,  1.0f},
    };
    for (int i = 0; i < 4; ++i)
    {
        if (i > 0) ImGui::SameLine();
        if (ImGui::SmallButton(presets[i].label))
        {
            dir = {presets[i].x, presets[i].y, presets[i].z};
            changed = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("正規化"))
    {
        DirectX::XMVECTOR v = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&dir));
        DirectX::XMStoreFloat3(&dir, v);
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

// ── Add Component メニュー項目（未所持のものだけ表示、Undo 対応） ──
template<typename T>
void AddComponentMenuItem(entt::registry& reg, EditorContext& ctx,
                          entt::entity e, const char* label, const T& initial = T{})
{
    if (reg.all_of<T>(e)) return;
    if (ImGui::MenuItem(label))
    {
        reg.emplace<T>(e, initial);
        ctx.undoSystem.PushCommand(std::make_unique<AddComponentCommand<T>>(
            &reg, e, initial, label));
    }
}

// 直前に描いたウィジェットをアセットパスの D&D 受け口にする。
// wantExt（例 ".uianim"）に一致する拡張子だけ受理し、assets ルートからの相対パスへ直して書く。
// 一致しないものを無視するのは、ドロップ先を間違えた時に黙って壊れるのを防ぐため。
inline bool AcceptAssetPathDrop(std::string& outRelPath, const char* wantExt,
                                const std::string& assetsDir)
{
    if (!ImGui::BeginDragDropTarget()) return false;
    bool changed = false;
    if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
    {
        namespace fs = std::filesystem;
        const char* dropped = static_cast<const char*>(payload->Data);
        std::string ext = fs::path(dropped).extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == wantExt)
        {
            std::string abs  = fs::path(dropped).lexically_normal().string();
            std::string base = fs::path(assetsDir).lexically_normal().string();
            std::replace(abs.begin(), abs.end(), '\\', '/');
            std::replace(base.begin(), base.end(), '\\', '/');
            outRelPath = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;
            changed = true;
        }
    }
    ImGui::EndDragDropTarget();
    return changed;
}

// ※ UIRect の「親矩形」解決（ResolveUiParentRectPx）は SceneView / UIエディタと共用のため
//   editor/UiEditUtil.h（uiedit 名前空間）へ移設した。

} // anonymous namespace

bool InspectorPanel::RevealMatches(const std::string& shaderPath) const
{
    if (m_revealShaderKey.empty() || shaderPath.empty()) return false;
    return shaderdiag::NormalizeKey(shaderPath) == m_revealShaderKey;
}

void InspectorPanel::Render(entt::registry& reg,
                            EditorContext& ctx,
                            Scene* scene)
{
    // ---- コンソールのシェーダーエラー行から飛んできた場合の受け口 ----
    // そのシェーダーを使っているエンティティを選び直し、該当セクションを開く指示を
    // このフレームだけ立てる（開く/スクロールは各セクションの描画箇所で行う）。
    m_revealShaderKey.clear();
    if (!ctx.revealShaderIssue.empty())
    {
        m_revealShaderKey = ctx.revealShaderIssue;
        ctx.revealShaderIssue.clear();

        // 既に選んでいるエンティティがそのシェーダーを使っているなら選択は動かさない
        // （複数が同じシェーダーを共有している時に選択が飛び回るのを避ける）。
        auto usesIt = [&](entt::entity e)
        {
            if (e == entt::null || !reg.valid(e)) return false;
            if (auto* mr = reg.try_get<MeshRenderer>(e))
                if (RevealMatches(mr->shaderPath)) return true;
            if (auto* sp = reg.try_get<Sprite2D>(e))
                if (RevealMatches(sp->shaderPath)) return true;
            if (auto* cc = reg.try_get<CameraComponent>(e))
                if (RevealMatches(cc->screenShaderPath)) return true;
            return false;
        };
        if (!usesIt(ctx.selectedEntity))
        {
            entt::entity found = entt::null;
            for (auto [e, mr] : reg.view<MeshRenderer>().each())
                if (RevealMatches(mr.shaderPath)) { found = e; break; }
            if (found == entt::null)
                for (auto [e, sp] : reg.view<Sprite2D>().each())
                    if (RevealMatches(sp.shaderPath)) { found = e; break; }
            if (found == entt::null)
                for (auto [e, cc] : reg.view<CameraComponent>().each())
                    if (RevealMatches(cc.screenShaderPath)) { found = e; break; }
            if (found != entt::null) ctx.Select(found);
            else m_revealShaderKey.clear();   // 誰も使っていない＝開く先が無い
        }
    }

    ImGui::Begin("\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xb9\xe3\x83\x9a\xe3\x82\xaf\xe3\x82\xbf\xe3\x83\xbc");  // Inspector

    // --- Selected Entity properties ---
    if (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity))
    {
        const EditorUiIcons* ic = ctx.icons;

        // NameTag（種別アイコン + 名前）
        if (reg.all_of<NameTag>(ctx.selectedEntity))
        {
            auto& tag = reg.get<NameTag>(ctx.selectedEntity);
            if (ic)
            {
                u64 tex = PickEntityIcon(reg, ctx.selectedEntity, *ic);
                if (tex)
                {
                    float s = ImGui::GetTextLineHeight() * 1.3f;
                    ImGui::Image(static_cast<ImTextureID>(tex), ImVec2(s, s));
                    ImGui::SameLine(0.0f, 6.0f);
                    ImGui::AlignTextToFramePadding();
                }
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.39f, 0.58f, 0.93f, 1.0f));
            ImGui::Text("%s", tag.name.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        // --- プレハブインスタンス（PrefabLink 持ちのルートだけ）---
        // 変更点の一覧と 適用 / 元に戻す を最上段に出す。Unity のプレハブヘッダー相当で、
        // 「今どのプレハブの実体を触っているのか」を触る前に分からせるのが狙い。
        if (scene && reg.all_of<PrefabLink>(ctx.selectedEntity))
            RenderPrefabHeader(reg, ctx, *scene, ctx.selectedEntity);

        // 種別専用インスペクター（ライト/オーディオは専用UIを最前面に。共通部品はこの下）
        if (reg.any_of<PointLight, DirectionalLight, SpotLight>(ctx.selectedEntity))
            RenderLightHero(reg, ctx, ctx.selectedEntity);
        if (reg.all_of<AudioSource>(ctx.selectedEntity))
            RenderAudioHero(reg, ctx, ctx.selectedEntity);

        // Transform
        if (reg.all_of<Transform>(ctx.selectedEntity))
        {
            if (IconHeader(ic, ic ? ic->entEmpty : 0, "Transform", ImGuiTreeNodeFlags_DefaultOpen))
            {
                auto& t = reg.get<Transform>(ctx.selectedEntity);

                // 編集開始前にスナップショットを取る（毎フレーム、非編集中のみ）
                if (!m_transformEditing)
                    m_transformSnapshot = t;

                bool anyActive = false;
                if (pg::Begin("Transform"))
                {
                    pg::Float3("位置 Position", &t.position.x, 0.1f,  0, 0, "%.3f", &anyActive);
                    pg::Float3("回転 Rotation", &t.rotation.x, 1.0f,  0, 0, "%.2f", &anyActive);
                    pg::Float3("拡縮 Scale",    &t.scale.x,    0.01f, 0, 0, "%.3f", &anyActive);
                    pg::End();
                }
                if (anyActive)
                    m_transformEditing = true;
            }

            // Transform 編集中 → 全ウィジェットが非アクティブになったら Undo に積む
            if (m_transformEditing && !ImGui::IsAnyItemActive())
            {
                auto& t = reg.get<Transform>(ctx.selectedEntity);
                bool changed =
                    m_transformSnapshot.position.x != t.position.x ||
                    m_transformSnapshot.position.y != t.position.y ||
                    m_transformSnapshot.position.z != t.position.z ||
                    m_transformSnapshot.rotation.x != t.rotation.x ||
                    m_transformSnapshot.rotation.y != t.rotation.y ||
                    m_transformSnapshot.rotation.z != t.rotation.z ||
                    m_transformSnapshot.scale.x    != t.scale.x ||
                    m_transformSnapshot.scale.y    != t.scale.y ||
                    m_transformSnapshot.scale.z    != t.scale.z;
                if (changed)
                {
                    ctx.undoSystem.PushCommand(std::make_unique<TransformCommand>(
                        &reg, ctx.selectedEntity, m_transformSnapshot, t));
                }
                m_transformEditing = false;
            }
        }

        // MeshRenderer
        if (reg.all_of<MeshRenderer>(ctx.selectedEntity))
        {
            if (IconHeader(ic, ic ? ic->entMesh : 0, "MeshRenderer"))
            {
                auto& r = reg.get<MeshRenderer>(ctx.selectedEntity);
                if (pg::Begin("MeshRenderer"))
                {
                    pg::Text("Meshes",    "%d", static_cast<int>(r.meshes.size()));
                    pg::Text("Materials", "%d", static_cast<int>(r.materials.size()));
                    pg::End();
                }

                // ── モデル差し替え ──
                // 現在のモデルパス表示 + assets 内モデル一覧コンボ + アセットブラウザからの
                // D&D 受け。選択/ドロップは pendingModelSwaps に積み、フレーム境界で
                // SwapEntityModel が全コンポーネント・親子関係を維持したまま差し替える。
                {
                    namespace fs = std::filesystem;

                    // 現在パスを assets 相対で表示（プリミティブはマーカー名のまま）
                    std::string cur = r.modelPath;
                    {
                        std::string abs = fs::path(cur).lexically_normal().string();
                        std::string base = fs::path(m_assetsDir).lexically_normal().string();
                        std::replace(abs.begin(), abs.end(), '\\', '/');
                        std::replace(base.begin(), base.end(), '\\', '/');
                        if (abs.rfind(base, 0) == 0) cur = abs.substr(base.size());
                    }

                    if (pg::Begin("ModelSwap"))
                    {
                        pg::Label("モデル Model");
                        if (ImGui::BeginCombo("##swapModel", cur.empty() ? "(none)" : cur.c_str()))
                        {
                            std::vector<std::string> options;
                            std::error_code ec;
                            fs::path root(m_assetsDir);
                            if (fs::exists(root, ec))
                            {
                                fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
                                fs::recursive_directory_iterator end;
                                for (; !ec && it != end; it.increment(ec))
                                {
                                    std::error_code fec;
                                    if (!it->is_regular_file(fec) || fec) continue;
                                    std::string ext = it->path().extension().string();
                                    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                    if (AssetBrowserPanel::ClassifyExtension(ext) != AssetBrowserPanel::AssetType::Model)
                                        continue;
                                    fs::path rel = fs::relative(it->path(), root, fec);
                                    if (fec) continue;
                                    options.push_back(rel.generic_string());
                                }
                            }
                            std::sort(options.begin(), options.end());
                            for (const auto& opt : options)
                            {
                                if (ImGui::Selectable(opt.c_str(), opt == cur) && opt != cur)
                                    ctx.pendingModelSwaps.push_back(
                                        {ctx.selectedEntity, m_assetsDir + opt});
                            }
                            ImGui::EndCombo();
                        }
                        // アセットブラウザからモデルファイルを直接ドロップして差し替え
                        if (ImGui::BeginDragDropTarget())
                        {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
                            {
                                const char* droppedPath = static_cast<const char*>(payload->Data);
                                std::string ext = fs::path(droppedPath).extension().string();
                                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                if (AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Model)
                                    ctx.pendingModelSwaps.push_back(
                                        {ctx.selectedEntity, std::string(droppedPath)});
                            }
                            ImGui::EndDragDropTarget();
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup))
                            ImGui::SetTooltip("クリックで assets 内のモデル一覧から選択\n"
                                              "アセットブラウザからモデルをドロップしても差し替え可能\n"
                                              "Transform・スクリプト・物理などは維持されます");
                        pg::End();
                    }
                }
            }
        }

        // Sprite2D（ワールド/HUD スプライト）
        if (reg.all_of<Sprite2D>(ctx.selectedEntity))
        {
            // コンソールのエラー行から飛んできたら、このコンポーネントと下の Shader 節を開く
            const bool revealSprite =
                RevealMatches(reg.get<Sprite2D>(ctx.selectedEntity).shaderPath);
            if (revealSprite) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "Sprite2D");
            if (revealSprite) ImGui::SetScrollHereY(0.15f);
            bool removed = ComponentRemoveMenu<Sprite2D>(reg, ctx, ctx.selectedEntity, "Sprite2D");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_spriteEdit);
                auto& sp = reg.get<Sprite2D>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("Sprite2D"))
                {
                    char buf[256] = {};
                    size_t n = sp.texturePath.copy(buf, sizeof(buf) - 1);
                    buf[n] = '\0';
                    if (pg::InputText("テクスチャ Texture", buf, sizeof(buf), 0, &active))
                    { sp.texturePath = buf; changed = true; }

                    changed |= pg::Int("描画順 Layer", &sp.layer, 1.0f, -1000, 1000, &active);
                    changed |= pg::Float2("サイズ Size", &sp.size.x, 0.01f, 0.0f, 100.0f, "%.2f", &active);
                    changed |= pg::Float2("UV Min", &sp.uvMin.x, 0.005f, 0.0f, 1.0f, "%.3f", &active);
                    changed |= pg::Float2("UV Max", &sp.uvMax.x, 0.005f, 0.0f, 1.0f, "%.3f", &active);
                    // ★同じ注記が「アニメ」グループの下(animFrames の子項目の後)にもあるが、
                    //   UV Min/Max を触っている人はそこまでスクロールしない。効かない値の
                    //   すぐ隣に出す（ApplicationRender.cpp:854 が uvMin/uvMax を丸ごと上書きする）。
                    if (sp.animFrames > 0)
                        WarnRow("animFrames>0 のため UV Min/Max は無視されます"
                                "（下の連番設定が UV を決めます）");
                    changed |= pg::Color4("色 Color", &sp.color.x);

                    pg::Group("配置");
                    changed |= pg::Checkbox("ワールド空間 World Space", &sp.worldSpace,
                        "OFF: HUD 表示（画面固定）。位置は Transform の値をピクセル扱い");
                    if (sp.worldSpace)
                    {
                        changed |= pg::Checkbox("常にカメラを向く Billboard", &sp.billboard,
                            "OFF: Transform の回転どおりに固定表示（ギズモの R で回転可）\n"
                            "ON : 常にアクティブカメラの方を向く（回転は無視される）");
                    }

                    // フリップブック（スプライトシート）/ UVスクロール。エディタ中もプレビュー再生される
                    // ※ ここでテーブルを閉じないこと。閉じた状態で pg::Group() を呼ぶと
                    //   ImGui::TableNextRow() が null テーブルを触って即クラッシュする
                    //   （Sprite2D を選ぶだけでエディタが落ちていた。超詳細診断以前の UI 自動テストで発覚）。
                    pg::Group("アニメ");
                    changed |= pg::Int("フレーム数 animFrames", &sp.animFrames, 1.0f, 0, 1024, &active);
                    if (sp.animFrames > 0)
                    {
                        changed |= pg::Float("速度 animFps", &sp.animFps, 0.1f, 0.0f, 120.0f, "%.1f", &active);
                        changed |= pg::Int("列数 animCols", &sp.animCols, 1.0f, 0, 1024, &active);
                        changed |= pg::Int("開始行 animRow", &sp.animRow, 1.0f, 0, 1024, &active);
                        changed |= pg::Int("総行数 animRows", &sp.animRows, 1.0f, 0, 1024, &active);
                        {
                            static const char* animModes[] = {"ループ", "単発(最後で停止)", "往復(ピンポン)"};
                            changed |= pg::Combo("再生モード animMode", &sp.animMode, animModes,
                                IM_ARRAYSIZE(animModes),
                                "単発=爆発/被弾など1回きりの演出。往復=0→末尾→0 の呼吸アニメ");
                        }
                        WarnRow("UV Min/Max は無視され、上の連番設定から自動で決まります");
                    }
                    else
                    {
                        changed |= pg::Float("スクロールU scrollU", &sp.scrollU, 0.01f, -100.0f, 100.0f, "%.2f", &active);
                        changed |= pg::Float("スクロールV scrollV", &sp.scrollV, 0.01f, -100.0f, 100.0f, "%.2f", &active);
                    }
                    pg::End();
                }

                // カスタムシェーダー割当（worldSpace のスプライトのみ有効。MeshRendererの仕組みを踏襲するが
                // ルートシグネチャ/頂点フォーマットが異なるので互換性は無い＝別キャッシュ）。
                if (revealSprite) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                if (IconHeader(nullptr, 0, "Shader##Sprite2D") && pg::Begin("Sprite2DShader"))
                {
                    namespace fs = std::filesystem;
                    std::string currentLabel = sp.shaderPath.empty() ? "既定 (Sprite)" : sp.shaderPath;
                    pg::Label("シェーダー");
                    if (ImGui::BeginCombo("##sprShader", currentLabel.c_str()))
                    {
                        std::vector<std::string> options;
                        std::error_code ec;
                        fs::path root(m_assetsDir + "shaders/");
                        if (fs::exists(root, ec))
                        {
                            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
                            fs::recursive_directory_iterator end;
                            for (; !ec && it != end; it.increment(ec))
                            {
                                std::error_code fec;
                                if (!it->is_regular_file(fec) || fec) continue;
                                if (it->path().extension() != L".hlsl") continue;
                                fs::path rel = fs::relative(it->path(), root, fec);
                                if (fec) continue;
                                std::string relStr = rel.generic_string();
                                if (FindShaderSourceByRelPath(relStr) != nullptr)
                                    continue;  // Registry一致=上書き用途。個別割当の選択肢からは除外
                                options.push_back(relStr);
                            }
                        }

                        if (ImGui::Selectable("既定 (Sprite)", sp.shaderPath.empty()))
                        { sp.shaderPath.clear(); changed = true; }
                        for (const auto& opt : options)
                        {
                            if (ImGui::Selectable(opt.c_str(), sp.shaderPath == opt))
                            { sp.shaderPath = opt; changed = true; }
                        }
                        if (options.empty())
                            ImGui::TextDisabled("(assets/shaders/ に自作シェーダーなし)");
                        ImGui::EndCombo();
                    }
                    {   // アセットブラウザから .hlsl を直接ドロップして割り当てる
                        std::string dropped;
                        if (assetdrop::AcceptShader(dropped, m_assetsDir + "shaders/"))
                        { sp.shaderPath = dropped; changed = true; }
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(".hlsl をここへドロップでも割り当てられます");
                    if (!sp.shaderPath.empty())
                    {
                        changed |= pg::Checkbox("アルファブレンド有効", &sp.shaderAlphaBlend,
                            "ON: シェーダーの alpha 出力を SrcAlpha/InvSrcAlpha でブレンド(DepthWrite OFF)。"
                            "OFF(既定): 不透明固定で alpha は無視される");
                        changed |= pg::Float("エフェクト値 effectValue", &sp.effectValue, 0.005f, 0.0f, 1.0f, "%.3f", &active,
                            "シェーダーへ渡す汎用の進捗/強度値(意味はシェーダー依存)。"
                            "Luaの scene:setSpriteEffect(e, value) で実行時にも変更可");
                        changed |= pg::Float4("パラメーター shaderParams", &sp.shaderParams.x, 0.01f, 0.0f, 0.0f, "%.3f", &active,
                            "シェーダーへ渡す汎用パラメーター4つ(意味はシェーダー依存)。HLSL側は VSIn/PSIn に "
                            "float4 params : TEXCOORD2; を足して読む。Luaの scene:setSpriteParams(e, x,y,z,w) でも変更可");
                    }
                    pg::End();
                    ShaderIssueBox(sp.shaderPath, dx12e::shaderdiag::kIdSprite, "Sprite2D");
                    if (!sp.worldSpace && !sp.shaderPath.empty())
                        WarnText("HUD スプライトはカスタムシェーダー未対応(worldSpaceのみ)");
                }

                EndEdit(reg, ctx, ctx.selectedEntity, m_spriteEdit, changed, active, "Sprite2D");
            }
        }

        // UICanvas（ゲーム内UIのルート。子孫の UIRect ツリーを Play 中に描画する）
        if (reg.all_of<UICanvas>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "UICanvas");
            bool removed = ComponentRemoveMenu<UICanvas>(reg, ctx, ctx.selectedEntity, "UICanvas");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiCanvasEdit);
                auto& cv = reg.get<UICanvas>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UICanvas"))
                {
                    changed |= pg::Float("基準幅 Ref Width", &cv.refWidth, 1.0f, 1.0f, 16384.0f, "%.0f", &active,
                        "レイアウトの基準解像度。UI はこの解像度で設計し、実行時に画面へ合わせる");
                    changed |= pg::Float("基準高さ Ref Height", &cv.refHeight, 1.0f, 1.0f, 16384.0f, "%.0f", &active);
                    static const char* scaleModes[] = {
                        "等比スケール（中央寄せレターボックス）",
                        "実ピクセル（左上原点・等倍）",
                        "引き伸ばし（画面全体・余白なし）",
                    };
                    changed |= pg::Combo("スケールモード Scale Mode", &cv.scaleMode, scaleModes, 3,
                        "等比スケール: 基準解像度をアスペクト比を保って画面に収める（縦横比が違う画面では余白ができる）\n"
                        "実ピクセル: スケールせず左上原点の実ピクセルで配置する\n"
                        "引き伸ばし: 縦横を個別に伸縮して画面いっぱいに敷き詰める（余白ゼロ。HUD向け）");
                    changed |= pg::Int("描画順 Sort Order", &cv.sortOrder, 1.0f, -100, 100, &active,
                        "キャンバス間の描画順（小さいほど奥）");
                    changed |= pg::Checkbox("表示 Visible", &cv.visible,
                        "OFF: このキャンバス配下の UI をすべて描画しない");
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiCanvasEdit, changed, active, "UICanvas");
            }
        }

        // UIRect（UI レイアウトノード。アンカー＋オフセットで親矩形に追従する）
        if (reg.all_of<UIRect>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "UIRect");
            bool removed = ComponentRemoveMenu<UIRect>(reg, ctx, ctx.selectedEntity, "UIRect");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiRectEdit);
                auto& ur = reg.get<UIRect>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UIRect"))
                {
                    // ── アンカープリセット（Unity 風 4x4 = 9方位＋ストレッチ）──
                    // 選択時は解決済み矩形（見た目の位置）を維持するよう offset を補正してから
                    // anchorMin/Max を差し替える。親矩形サイズはキャンバス基準解像度で解決する。
                    pg::Label("アンカー Anchor",
                        "親矩形のどこに追従するか。プリセット選択時は見た目の位置を保ったまま\n"
                        "アンカーとオフセットを再計算する。最下行/最右列はストレッチ");
                    {
                        // 軸ごとの候補: {anchorMin, anchorMax}。0=左/上 0.5=中央 1=右/下 {0,1}=ストレッチ
                        static const float kAxis[4][2] = {
                            {0.0f, 0.0f}, {0.5f, 0.5f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
                        static const char* kColName[4] = {"左", "中央", "右", "横ストレッチ"};
                        static const char* kRowName[4] = {"上", "中央", "下", "縦ストレッチ"};

                        DirectX::XMFLOAT2 pMin{0.0f, 0.0f};
                        DirectX::XMFLOAT2 pMax{1920.0f, 1080.0f};   // UI ツリー外は既定値で近似
                        uiedit::ResolveUiParentRectPx(reg, ctx.selectedEntity, pMin, pMax);
                        const float pw = pMax.x - pMin.x;
                        const float ph = pMax.y - pMin.y;

                        ImGui::PushID("AnchorPreset");
                        auto* dl = ImGui::GetWindowDrawList();
                        const float cell = ImGui::GetFrameHeight();
                        for (int row = 0; row < 4; ++row)
                        {
                            for (int col = 0; col < 4; ++col)
                            {
                                if (col) ImGui::SameLine(0.0f, 3.0f);
                                ImGui::PushID(row * 4 + col);
                                const bool current =
                                    std::fabs(ur.anchorMin.x - kAxis[col][0]) < 1e-4f &&
                                    std::fabs(ur.anchorMax.x - kAxis[col][1]) < 1e-4f &&
                                    std::fabs(ur.anchorMin.y - kAxis[row][0]) < 1e-4f &&
                                    std::fabs(ur.anchorMax.y - kAxis[row][1]) < 1e-4f;
                                if (ImGui::Button("##anchor", ImVec2(cell, cell)))
                                {
                                    // 見た目の位置を維持: 新旧アンカー差 × 親サイズぶん offset を補正
                                    ur.offsetMin.x += pw * (ur.anchorMin.x - kAxis[col][0]);
                                    ur.offsetMax.x += pw * (ur.anchorMax.x - kAxis[col][1]);
                                    ur.offsetMin.y += ph * (ur.anchorMin.y - kAxis[row][0]);
                                    ur.offsetMax.y += ph * (ur.anchorMax.y - kAxis[row][1]);
                                    ur.anchorMin = {kAxis[col][0], kAxis[row][0]};
                                    ur.anchorMax = {kAxis[col][1], kAxis[row][1]};
                                    changed = true;
                                }
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip("%s・%s", kRowName[row], kColName[col]);

                                // ミニ図: 外枠=親矩形、塗り=アンカーの位置/範囲
                                const ImVec2 bmin = ImGui::GetItemRectMin();
                                const ImVec2 bmax = ImGui::GetItemRectMax();
                                dl->AddRect(ImVec2(bmin.x + 2.0f, bmin.y + 2.0f),
                                            ImVec2(bmax.x - 2.0f, bmax.y - 2.0f),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled));
                                const float innerW = bmax.x - bmin.x - 6.0f;
                                const float innerH = bmax.y - bmin.y - 6.0f;
                                float x0 = bmin.x + 3.0f + innerW * kAxis[col][0];
                                float x1 = bmin.x + 3.0f + innerW * kAxis[col][1];
                                float y0 = bmin.y + 3.0f + innerH * kAxis[row][0];
                                float y1 = bmin.y + 3.0f + innerH * kAxis[row][1];
                                const float dot = 2.0f;   // 点アンカーの最低表示幅（半分）
                                if (x1 - x0 < dot * 2.0f) { const float c = (x0 + x1) * 0.5f; x0 = c - dot; x1 = c + dot; }
                                if (y1 - y0 < dot * 2.0f) { const float c = (y0 + y1) * 0.5f; y0 = c - dot; y1 = c + dot; }
                                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                                    ImGui::GetColorU32(current ? theme::Accent
                                                               : ImGui::GetStyleColorVec4(ImGuiCol_Text)));
                                ImGui::PopID();
                            }
                        }
                        ImGui::PopID();
                    }

                    // ── 配置テンプレ（9 方位）: アンカー + 位置を同時スナップ ──
                    // アンカープリセットが「アンカーだけ変える（見た目は保つ）」のに対し、こちらは
                    // 要素そのものを親矩形のその方位へピッタリ寄せる（Unity の Alt+プリセット相当）。
                    pg::Label("配置 Placement",
                        "要素を親矩形の 9 方位へスナップ配置する。アンカーも同時に設定されるので\n"
                        "解像度が変わってもその方位に追従する（サイズは維持）");
                    {
                        static const char* kPlaceLabels[3][3] = {
                            {"\xe2\x86\x96", "\xe2\x86\x91", "\xe2\x86\x97"},   // ↖ ↑ ↗
                            {"\xe2\x86\x90", "\xe2\x97\x8f", "\xe2\x86\x92"},   // ← ● →
                            {"\xe2\x86\x99", "\xe2\x86\x93", "\xe2\x86\x98"},   // ↙ ↓ ↘
                        };
                        static const char* kPlaceNames[3][3] = {
                            {"左上", "上", "右上"},
                            {"左",   "中央", "右"},
                            {"左下", "下", "右下"},
                        };
                        ImGui::PushID("UiPlacement");
                        const float cell = ImGui::GetFrameHeight() + 4.0f;
                        for (int row = 0; row < 3; ++row)
                        {
                            for (int col = 0; col < 3; ++col)
                            {
                                if (col) ImGui::SameLine(0.0f, 3.0f);
                                ImGui::PushID(row * 3 + col);
                                if (ImGui::Button(kPlaceLabels[row][col], ImVec2(cell, cell)))
                                {
                                    if (uiedit::ApplyUiPlacement(reg, ctx.selectedEntity, col, row))
                                        changed = true;
                                }
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip("%s に配置", kPlaceNames[row][col]);
                                ImGui::PopID();
                            }
                        }
                        ImGui::PopID();
                    }

                    changed |= pg::Float2("アンカー Min", &ur.anchorMin.x, 0.005f, 0.0f, 1.0f, "%.3f", &active,
                        "親矩形内の正規化位置（0=左/上 1=右/下）。直接編集では位置補正しない");
                    changed |= pg::Float2("アンカー Max", &ur.anchorMax.x, 0.005f, 0.0f, 1.0f, "%.3f", &active);
                    changed |= pg::Float2("ピボット Pivot", &ur.pivot.x, 0.005f, 0.0f, 1.0f, "%.3f", &active,
                        "位置 Position が指す自分の基準点（0.5,0.5=中心）");

                    const bool anchorsMatch =
                        std::fabs(ur.anchorMax.x - ur.anchorMin.x) < 1e-4f &&
                        std::fabs(ur.anchorMax.y - ur.anchorMin.y) < 1e-4f;
                    if (anchorsMatch)
                    {
                        // アンカー一致: 位置(px)＋サイズ(px)で編集（offset へ換算して保持）
                        DirectX::XMFLOAT2 size{ur.offsetMax.x - ur.offsetMin.x,
                                               ur.offsetMax.y - ur.offsetMin.y};
                        DirectX::XMFLOAT2 pos{ur.offsetMin.x + size.x * ur.pivot.x,
                                              ur.offsetMin.y + size.y * ur.pivot.y};
                        bool rectCh = false;
                        rectCh |= pg::Float2("位置 Position", &pos.x, 1.0f, 0.0f, 0.0f, "%.0f", &active,
                            "アンカー点からのピボット位置(px)");
                        rectCh |= pg::Float2("サイズ Size", &size.x, 1.0f, 0.0f, 32768.0f, "%.0f", &active);
                        if (rectCh)
                        {
                            ur.offsetMin = {pos.x - size.x * ur.pivot.x,
                                            pos.y - size.y * ur.pivot.y};
                            ur.offsetMax = {pos.x + size.x * (1.0f - ur.pivot.x),
                                            pos.y + size.y * (1.0f - ur.pivot.y)};
                            changed = true;
                        }
                    }
                    else
                    {
                        // ストレッチ: アンカー辺からのオフセット(px)を直接編集
                        changed |= pg::Float2("オフセット Min", &ur.offsetMin.x, 1.0f, 0.0f, 0.0f, "%.0f", &active,
                            "アンカー Min 点からのオフセット(px)");
                        changed |= pg::Float2("オフセット Max", &ur.offsetMax.x, 1.0f, 0.0f, 0.0f, "%.0f", &active,
                            "アンカー Max 点からのオフセット(px)");
                    }

                    changed |= pg::Checkbox("表示 Visible", &ur.visible,
                        "OFF: 自分と子孫を描画しない（ボタンも反応しない）");
                    changed |= pg::Float("回転 Rotation", &ur.rotation, 0.5f, -360.0f, 360.0f,
                        "%.1f", &active,
                        "視覚回転（度・時計回り）。ピボット点回りに掛かり、子孫も一緒に回る。\n"
                        "レイアウトは軸平行のまま（回転中はリサイズ/アンカーハンドル非表示）。\n"
                        "UIScrollView ノード自身では無視される");
                    changed |= pg::Float("スキュー Skew X", &ur.skewX, 0.5f, -85.0f, 85.0f,
                        "%.1f", &active,
                        "横方向の傾き（度）。平行四辺形のバナー/ボタンに（ペルソナ風の斜めUI）");
                    changed |= pg::Checkbox("子をマスク Clip Children", &ur.clipChildren,
                        "ON: 子ツリーをこの矩形でクリップ（はみ出しを隠す）。ワイプ公開・\n"
                        "マーキー・ゲージ内スクロール用。このノード自身の回転/スキューは無効になる");

                    pg::Label("視覚変形 Visual",
                        "レイアウト矩形は変えずに見た目だけ変える。回転/スキューと同じ扱いで、\n"
                        "UIAnimator や tween の実行時アニメとは掛け算で合流する");
                    changed |= pg::Float("スケール X", &ur.scaleX, 0.005f, 0.0f, 10.0f, "%.3f", &active,
                        "ピボット点回りの拡縮。ピボット(0,0) なら左上を固定して伸びる（ゲージ向き）");
                    changed |= pg::Float("スケール Y", &ur.scaleY, 0.005f, 0.0f, 10.0f, "%.3f", &active);
                    changed |= pg::Float("アルファ Alpha", &ur.alpha, 0.005f, 0.0f, 1.0f, "%.3f", &active,
                        "自分と子孫にまとめて掛かるグループ透過。UIImage/UIText の色アルファとは別枠");
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiRectEdit, changed, active, "UIRect");
            }
        }

        // UIImage（UI の画像/単色矩形。UIButton があれば状態色が乗算される）
        if (reg.all_of<UIImage>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "UIImage");
            bool removed = ComponentRemoveMenu<UIImage>(reg, ctx, ctx.selectedEntity, "UIImage");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiImageEdit);
                auto& img = reg.get<UIImage>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UIImage"))
                {
                    char buf[256] = {};
                    size_t n = img.texturePath.copy(buf, sizeof(buf) - 1);
                    buf[n] = '\0';
                    if (pg::InputText("テクスチャ Texture", buf, sizeof(buf), 0, &active,
                        "assets 相対パス。空なら単色塗り矩形。アセットブラウザから D&D で割当可。\n"
                        "ロードに失敗した場合は単色矩形で代替表示される"))
                    { img.texturePath = buf; changed = true; }

                    // アセットブラウザからの D&D（テクスチャのみ受理、assets 相対パスへ変換）
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload =
                                ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
                        {
                            const char* droppedPath = static_cast<const char*>(payload->Data);
                            namespace fs = std::filesystem;
                            std::string ext = fs::path(droppedPath).extension().string();
                            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                            if (AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Texture)
                            {
                                std::string abs = fs::path(droppedPath).lexically_normal().string();
                                std::string base = fs::path(m_assetsDir).lexically_normal().string();
                                std::replace(abs.begin(), abs.end(), '\\', '/');
                                std::replace(base.begin(), base.end(), '\\', '/');
                                img.texturePath = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;
                                changed = true;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    changed |= pg::Color4("色 Color", &img.color.x);
                    {
                        static const char* shapes[] = {"矩形", "楕円", "リング(枠円)", "ダイヤ",
                                                       "六角形", "三角形(上向き)"};
                        changed |= pg::Combo("形状 Shape", &img.shape, shapes, IM_ARRAYSIZE(shapes),
                            "矩形以外は角丸/9スライス無効。テクスチャは形で切り抜かれる\n"
                            "（丸アイコン等）。リングは単色専用の帯円（円形ゲージ向き）");
                    }
                    if (img.shape == 2)
                        changed |= pg::Float("リング太さ Ring Thickness", &img.ringThickness,
                            0.25f, 1.0f, 512.0f, "%.1f", &active, "帯の太さ（キャンバスpx）");
                    changed |= pg::Float2("UV Min", &img.uvMin.x, 0.005f, -64.0f, 64.0f, "%.3f", &active);
                    changed |= pg::Float2("UV Max", &img.uvMax.x, 0.005f, -64.0f, 64.0f, "%.3f", &active,
                        "1 を超えるとタイル繰り返し（パターン背景。ストライプ/ドット等）");
                    changed |= pg::Float2("UVスクロール UV Scroll", &img.uvScroll.x, 0.01f,
                        -32.0f, 32.0f, "%.2f", &active,
                        "uv/秒。タイル(UV Max>1)と併用で流れるパターンに\n"
                        "（警告帯/コンベア/背景ストライプ）。9スライスでは無効");
                    // ★UISystem.cpp:1050 の分岐をそのまま画面に出す。
                    //   これまで注記は animFrames のツールチップにしか無く、
                    //   UV を触っている人は animFrames にホバーしない。
                    {
                        const bool nineSlice = img.sliceBorder.x > 0.0f || img.sliceBorder.y > 0.0f
                                            || img.sliceBorder.z > 0.0f || img.sliceBorder.w > 0.0f;
                        const bool flipbookLive = img.animFrames > 0 && !nineSlice && img.shape == 0;
                        if (flipbookLive)
                            WarnRow("連番アニメ中のため UV Min/Max・UVスクロールは無視されます");
                        else if (img.animFrames > 0)
                            WarnRow("連番アニメは %s では動きません（UV はそのまま使われます）",
                                    nineSlice ? "9スライス有効中" : "矩形以外の形状");
                        else if (nineSlice && (img.uvScroll.x != 0.0f || img.uvScroll.y != 0.0f))
                            WarnRow("9スライス有効中は UVスクロールが効きません");
                    }
                    // 連番アニメ（スプライトシート）。エディタ中もプレビュー再生される
                    changed |= pg::Int("連番フレーム数 animFrames", &img.animFrames, 1.0f, 0, 1024, &active,
                        "0=なし。1以上でテクスチャをコマ送り再生（爆発/ローディング/アイコン点滅）。\n"
                        "有効中は UV Min/Max と UVスクロールは無視される。9スライス/矩形以外の形状では無効");
                    if (img.animFrames > 0)
                    {
                        changed |= pg::Float("速度 animFps", &img.animFps, 0.1f, 0.0f, 120.0f, "%.1f", &active);
                        changed |= pg::Int("列数 animCols", &img.animCols, 1.0f, 0, 1024, &active,
                            "0=animFrames と同じ（横1行ストリップ）");
                        changed |= pg::Int("開始行 animRow", &img.animRow, 1.0f, 0, 1024, &active);
                        changed |= pg::Int("総行数 animRows", &img.animRows, 1.0f, 0, 1024, &active,
                            "0=自動（開始行 + ceil(animFrames/animCols)）");
                        static const char* uiAnimModes[] = {"ループ", "単発(最後で停止)", "往復(ピンポン)"};
                        changed |= pg::Combo("再生モード animMode", &img.animMode, uiAnimModes,
                            IM_ARRAYSIZE(uiAnimModes),
                            "単発=1回きりの演出。往復=0→末尾→0 の呼吸アニメ");
                    }
                    changed |= pg::FloatN("9スライス境界 Slice Border", &img.sliceBorder.x, 4, 0.5f,
                        0.0f, 4096.0f, "%.0f", &active,
                        "元テクスチャ上の境界px（左,上,右,下）。全0で無効。\n"
                        "角は固定サイズのまま、辺と中央だけ引き伸ばして描画する");
                    changed |= pg::Float("角丸半径 Corner Radius", &img.cornerRadius, 0.5f, 0.0f, 1024.0f,
                        "%.0f", &active, "単色矩形（テクスチャ未指定）のときのみ有効");
                    changed |= pg::Checkbox("クリックを遮る Raycast Block", &img.raycastBlock,
                        "ON: この画像が背後にあるボタンへのクリック/ホバーを遮る（Unity の raycastTarget 相当）。\n"
                        "OFF: クリックを素通しする（装飾用オーバーレイ向け）");
                    changed |= pg::Float("表示割合 Fill Amount", &img.fillAmount, 0.005f, 0.0f, 1.0f,
                        "%.3f", &active,
                        "0〜1 の割合だけ表示する（HPバー/ゲージ用）。1=全表示、0=非表示。\n"
                        "Lua からは scene:setUiFill(entity, amount) で更新できる");
                    {
                        static const char* fillDirs[] = {"左から", "右から", "下から", "上から",
                                                         "放射(時計回り)", "放射(反時計回り)"};
                        changed |= pg::Combo("Fill 方向 Fill Dir", &img.fillDir, fillDirs, IM_ARRAYSIZE(fillDirs),
                            "Fill Amount が増えるとき、どの端から現れていくか。\n"
                            "放射=中心からの角度掃引（クールダウン円。矩形にも効く）");
                    }
                    if (img.fillDir >= 4 || img.shape == 2)
                        changed |= pg::Float("Fill 開始角 Fill Origin", &img.fillOrigin, 1.0f,
                            -360.0f, 360.0f, "%.0f", &active,
                            "放射 fill の開始角（度）。0=真上、時計回り正");
                    if (img.shape == 0 && img.fillDir <= 3)
                    {
                        changed |= pg::Int("分割数 Segments", &img.segments, 0.1f, 0, 64, &active,
                            "0以外で fill 軸と直交する区切り線を重ねて「n分割チャンクゲージ」に\n"
                            "（スタミナ/弾数）。矩形+線形 fill 専用");
                        if (img.segments > 1)
                        {
                            changed |= pg::Float("区切り太さ Segment Gap", &img.segmentGap,
                                0.25f, 0.5f, 64.0f, "%.1f", &active, "キャンバスpx");
                            changed |= pg::Color4("区切り色 Segment Color", &img.segmentColor.x);
                        }
                    }

                    pg::Label("グラデーション Gradient",
                        "色 Color → 終端色 の線形グラデーション。テクスチャ/9スライス/角丸にも掛かる");
                    {
                        static const char* gradDirs[] = {"なし", "横(左→右)", "縦(上→下)",
                                                         "斜め(左上→右下)", "放射(中心→外)"};
                        changed |= pg::Combo("方向 Gradient Dir", &img.gradientDir,
                                             gradDirs, IM_ARRAYSIZE(gradDirs));
                    }
                    if (img.gradientDir > 0)
                    {
                        changed |= pg::Color4("終端色 Gradient Color 2", &img.gradientColor2.x);
                        if (img.gradientDir != 4)
                            changed |= pg::Float("グロス速度 Scroll Speed", &img.gradientScrollSpeed,
                                0.05f, -10.0f, 10.0f, "%.2f", &active,
                                "0以外で静的グラデの代わりに終端色の光帯がグラデ方向へ流れる\n"
                                "（ガチャボタンの光沢流し）。周回数/秒。負値で逆方向、0 で静的グラデ");
                    }

                    pg::Label("縁取り Outline", "枠線。角丸にも追従する");
                    changed |= pg::Float("太さ Outline Width", &img.outlineWidth, 0.25f, 0.0f, 64.0f,
                        "%.1f", &active, "キャンバスpx。0 で無効");
                    if (img.outlineWidth > 0.0f)
                    {
                        changed |= pg::Color4("縁取り色 Outline Color", &img.outlineColor.x);
                        if (img.shape == 0)
                        {
                            static const char* olStyles[] = {"実線", "破線",
                                                             "コーナーブラケット(四隅)"};
                            changed |= pg::Combo("スタイル Outline Style", &img.outlineStyle,
                                olStyles, IM_ARRAYSIZE(olStyles),
                                "破線/ブラケットは矩形専用・角丸無視。ブラケットは SF/照準 HUD の定番");
                            if (img.outlineStyle != 0)
                                changed |= pg::Float("破線長/腕長 Dash", &img.outlineDash,
                                    0.5f, 2.0f, 256.0f, "%.0f", &active,
                                    "破線=1区切りの長さ / ブラケット=腕の長さ（キャンバスpx）");
                        }
                    }

                    pg::Label("影 Drop Shadow",
                        "矩形近似のドロップシャドウ（テクスチャの形は反映しない）。色のαが 0 で無効");
                    changed |= pg::Color4("影色 Shadow Color", &img.shadowColor.x);
                    if (img.shadowColor.w > 0.0f)
                    {
                        changed |= pg::Float2("影オフセット Shadow Offset", &img.shadowOffset.x,
                                              0.25f, 0.0f, 0.0f, "%.1f", &active, "キャンバスpx");
                        changed |= pg::Float("影ぼかし Shadow Softness", &img.shadowSoftness,
                                             0.25f, 0.0f, 64.0f, "%.1f", &active,
                                             "外へ広がるぼかし量(px)。0 でシャープな影");
                    }
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiImageEdit, changed, active, "UIImage");
            }
        }

        // UIText（UI テキスト。ImGui 共有フォントのスケール描画）
        if (reg.all_of<UIText>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "UIText");
            bool removed = ComponentRemoveMenu<UIText>(reg, ctx, ctx.selectedEntity, "UIText");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiTextEdit);
                auto& txt = reg.get<UIText>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UIText"))
                {
                    char buf[1024] = {};
                    size_t n = txt.text.copy(buf, sizeof(buf) - 1);
                    buf[n] = '\0';
                    pg::Label("テキスト Text");
                    if (ImGui::InputTextMultiline("##uiTextValue", buf, sizeof(buf),
                                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f)))
                    { txt.text = buf; changed = true; }
                    active |= ImGui::IsItemActive();

                    changed |= pg::Float("フォントサイズ Font Size", &txt.fontSize, 0.5f, 4.0f, 512.0f,
                                         "%.0f", &active, "キャンバス基準解像度でのpx。実行時にスケールされる");
                    changed |= pg::Color4("色 Color", &txt.color.x);
                    static const char* alignHItems[] = {"左", "中央", "右"};
                    static const char* alignVItems[] = {"上", "中央", "下"};
                    changed |= pg::Combo("水平整列 Align H", &txt.alignH, alignHItems, 3);
                    changed |= pg::Combo("垂直整列 Align V", &txt.alignV, alignVItems, 3);
                    changed |= pg::Checkbox("折り返し Wrap", &txt.wrap, "ON: UIRect の幅で折り返す");

                    // カスタムフォント（assets 相対 .ttf/.otf。アセットブラウザから D&D 可）
                    {
                        char fbuf[256] = {};
                        size_t fn = txt.fontPath.copy(fbuf, sizeof(fbuf) - 1);
                        fbuf[fn] = '\0';
                        if (pg::InputText("フォント Font", fbuf, sizeof(fbuf), 0, &active,
                            "assets 相対の .ttf/.otf。空なら既定フォント（Yu Gothic）。\n"
                            "アセットブラウザから D&D で割当可。ロード失敗は既定フォントで表示"))
                        { txt.fontPath = fbuf; changed = true; }
                        if (ImGui::BeginDragDropTarget())
                        {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
                            {
                                const char* droppedPath = static_cast<const char*>(payload->Data);
                                namespace fs = std::filesystem;
                                std::string ext = fs::path(droppedPath).extension().string();
                                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                if (ext == ".ttf" || ext == ".otf")
                                {
                                    std::string abs = fs::path(droppedPath).lexically_normal().string();
                                    std::string base = fs::path(m_assetsDir).lexically_normal().string();
                                    std::replace(abs.begin(), abs.end(), '\\', '/');
                                    std::replace(base.begin(), base.end(), '\\', '/');
                                    txt.fontPath = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;
                                    changed = true;
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                    }

                    pg::Label("縁取り Outline", "8方位の重ね描き。ゲームUIの可読性の要");
                    changed |= pg::Float("太さ Outline Width", &txt.outlineWidth, 0.25f, 0.0f, 16.0f,
                        "%.1f", &active, "キャンバスpx。0 で無効");
                    if (txt.outlineWidth > 0.0f)
                        changed |= pg::Color4("縁取り色 Outline Color", &txt.outlineColor.x);

                    pg::Label("影 Shadow", "1オフセットの落ち影。色のαが 0 で無効");
                    changed |= pg::Color4("影色 Shadow Color", &txt.shadowColor.x);
                    if (txt.shadowColor.w > 0.0f)
                        changed |= pg::Float2("影オフセット Shadow Offset", &txt.shadowOffset.x,
                                              0.25f, 0.0f, 0.0f, "%.1f", &active, "キャンバスpx");

                    changed |= pg::Float("タイプライター Typewriter", &txt.typewriterSpeed,
                        0.5f, 0.0f, 200.0f, "%.0f", &active,
                        "文字/秒。0 で無効(即全表示)。Play 中に1文字ずつ現れる(日本語も1文字ずつ)。\n"
                        "Lua: scene:setUiText で文字列を変えると先頭から再生し直す。\n"
                        "scene:setUiTypewriter(e,速度) / scene:isUiTypewriterDone(e) も使える");

                    changed |= pg::Float("字間 Letter Spacing", &txt.letterSpacing, 0.1f,
                        -32.0f, 64.0f, "%.1f", &active,
                        "文字の間隔(px)。負で詰める。0以外で1文字ずつ描くモードに\n"
                        "（折り返し Wrap とは非両立 = Wrap 優先）。タイトルの字間広げに");
                    {
                        static const char* charAnims[] = {"なし", "ウェーブ(上下うねり)",
                                                          "ジッター(ガタガタ)", "レインボー(色相回転)"};
                        changed |= pg::Combo("文字アニメ Char Anim", &txt.charAnim,
                            charAnims, IM_ARRAYSIZE(charAnims),
                            "1文字ずつ動く/色が変わる にぎやかしテキスト。Wrap とは非両立");
                    }
                    if (txt.charAnim == 1 || txt.charAnim == 2)
                        changed |= pg::Float("アニメ振幅 Amount", &txt.charAnimAmount, 0.1f,
                            0.0f, 64.0f, "%.1f", &active, "上下/ガタつきの振幅(px)");
                    if (txt.charAnim != 0)
                        changed |= pg::Float("アニメ速度 Speed", &txt.charAnimSpeed, 0.05f,
                            0.0f, 20.0f, "%.2f", &active, "周波数(Hz)");
                    {
                        static const char* tGradDirs[] = {"なし", "横(左→右)", "縦(上→下)"};
                        changed |= pg::Combo("グラデ Text Gradient", &txt.gradientDir,
                            tGradDirs, IM_ARRAYSIZE(tGradDirs),
                            "本体のみの2色グラデ（縁取り/影には掛からない）。金色タイトル等");
                    }
                    if (txt.gradientDir > 0)
                        changed |= pg::Color4("グラデ終端色 Gradient Color 2", &txt.gradientColor2.x);

                    changed |= pg::Checkbox("リッチテキスト Rich", &txt.rich,
                        "ON: テキスト内のタグをスパン装飾として解釈（入れ子なし。閉じ忘れは文末まで）:\n"
                        "  [c=RRGGBB]色[/c]  [wave]うねり[/wave]  [shake]震え[/shake]  [rainbow]虹[/rainbow]\n"
                        "アニメの振幅/速度は上の Char Anim 設定を流用。不正・未知のタグはそのまま表示。\n"
                        "Wrap とは非両立（Wrap 優先で無効）。テキストグラデは rich では無効");
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiTextEdit, changed, active, "UIText");
            }
        }

        // UIButton（クリックで events へ emit。同一エンティティの UIImage を状態色でティント）
        if (reg.all_of<UIButton>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "UIButton");
            bool removed = ComponentRemoveMenu<UIButton>(reg, ctx, ctx.selectedEntity, "UIButton");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiButtonEdit);
                auto& btn = reg.get<UIButton>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UIButton"))
                {
                    char buf[128] = {};
                    size_t n = btn.onClickEvent.copy(buf, sizeof(buf) - 1);
                    buf[n] = '\0';
                    if (pg::InputText("クリックイベント名 On Click", buf, sizeof(buf), 0, &active,
                        "クリック確定時に events へ emit するイベント名。空なら発火しない。\n"
                        "Lua 側は events:on(\"イベント名\", function(e) ... end) で受ける"))
                    { btn.onClickEvent = buf; changed = true; }

                    changed |= pg::Color4("通常色 Normal", &btn.normalColor.x);
                    changed |= pg::Color4("ホバー色 Hover", &btn.hoverColor.x);
                    changed |= pg::Color4("押下色 Pressed", &btn.pressedColor.x);
                    changed |= pg::Checkbox("操作可能 Interactable", &btn.interactable,
                        "OFF: 入力を受け付けず通常色で固定");

                    pg::Group("効果音");
                    {
                        char sbuf[256] = {};
                        size_t sn = btn.hoverSound.copy(sbuf, sizeof(sbuf) - 1);
                        sbuf[sn] = '\0';
                        if (pg::InputText("ホバー音 Hover SFX", sbuf, sizeof(sbuf), 0, &active,
                            "カーソル/フォーカスが乗った瞬間に 1 回鳴らす wav\n"
                            "(AudioSource のクリップと同じ assets 相対パス。空=鳴らさない)"))
                        { btn.hoverSound = sbuf; changed = true; }
                        sn = btn.clickSound.copy(sbuf, sizeof(sbuf) - 1);
                        sbuf[sn] = '\0';
                        if (pg::InputText("クリック音 Click SFX", sbuf, sizeof(sbuf), 0, &active,
                            "クリック確定(ボタン上で離した)時に 1 回鳴らす wav"))
                        { btn.clickSound = sbuf; changed = true; }
                    }
                    pg::End();
                }
                if (!reg.all_of<UIImage>(ctx.selectedEntity))
                    WarnText("状態色の反映には同一エンティティの UIImage が必要");
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiButtonEdit, changed, active, "UIButton");
            }
        }

        // UISlider（トラック+つまみを自前描画。値変更で onChangeEvent を emit）
        if (reg.all_of<UISlider>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entUi : 0, "UISlider");
            bool removed = ComponentRemoveMenu<UISlider>(reg, ctx, ctx.selectedEntity, "UISlider");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiSliderEdit);
                auto& sld = reg.get<UISlider>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UISlider"))
                {
                    changed |= pg::Float("値 Value", &sld.value, 0.005f,
                                         std::min(sld.minValue, sld.maxValue),
                                         std::max(sld.minValue, sld.maxValue), "%.3f", &active,
                        "現在値(実値)。Play 中はドラッグ操作で変わる。\n"
                        "Lua: scene:getUiSlider(e) / scene:setUiSlider(e, v)");
                    changed |= pg::Float("最小 Min", &sld.minValue, 0.01f, 0.0f, 0.0f, "%.3f", &active);
                    changed |= pg::Float("最大 Max", &sld.maxValue, 0.01f, 0.0f, 0.0f, "%.3f", &active);
                    changed |= pg::Float("刻み Step", &sld.step, 0.01f, 0.0f, 0.0f, "%.3f", &active,
                        "0 = 連続。0.1 なら 0.1 刻みにスナップ");

                    char buf[128] = {};
                    size_t n = sld.onChangeEvent.copy(buf, sizeof(buf) - 1);
                    buf[n] = '\0';
                    if (pg::InputText("変更イベント名 On Change", buf, sizeof(buf), 0, &active,
                        "値が変わった時に events へ emit するイベント名(空なら発火しない)。\n"
                        "Lua 側: events:on(\"名前\", function(e) e.value が実値 end)"))
                    { sld.onChangeEvent = buf; changed = true; }

                    changed |= pg::Color4("トラック色 Track", &sld.trackColor.x);
                    changed |= pg::Color4("塗り色 Fill", &sld.fillColor.x);
                    changed |= pg::Color4("つまみ色 Knob", &sld.knobColor.x);
                    changed |= pg::Checkbox("操作可能 Interactable", &sld.interactable);
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiSliderEdit, changed, active, "UISlider");
            }
        }

        // UIScrollView（子をクリップ + ホイール/ドラッグスクロール。子は階層ツリーでぶら下げる）
        if (reg.all_of<UIScrollView>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entUi : 0, "UIScrollView");
            bool removed = ComponentRemoveMenu<UIScrollView>(reg, ctx, ctx.selectedEntity, "UIScrollView");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiScrollEdit);
                auto& sv = reg.get<UIScrollView>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UIScrollView"))
                {
                    changed |= pg::Checkbox("縦スクロール Vertical", &sv.vertical);
                    changed |= pg::Checkbox("横スクロール Horizontal", &sv.horizontal);
                    changed |= pg::Float("位置 Y Scroll", &sv.scrollY, 1.0f, 0.0f, 100000.0f, "%.0f", &active,
                        "現在のスクロール量(px)。Play 中はホイールで変わる。\n"
                        "コンテンツ量に合わせて自動でクランプされる");
                    if (sv.horizontal)
                        changed |= pg::Float("位置 X Scroll", &sv.scrollX, 1.0f, 0.0f, 100000.0f, "%.0f", &active);
                    changed |= pg::Float("ホイール速度 Wheel", &sv.wheelSpeed, 1.0f, 4.0f, 400.0f, "%.0f", &active,
                        "ホイール1ノッチで進む量(px)");
                    changed |= pg::Checkbox("ドラッグ操作 Drag Scroll", &sv.dragScroll,
                        "ON: ドラッグ/フリック(慣性)でもスクロールできる(タッチUI風)。\n"
                        "6px 超えてドラッグするとリスト内ボタンの押下はキャンセルされ、\n"
                        "スクロールしてもクリック誤発火しない");
                    changed |= pg::Float("慣性減衰 Flick Decay", &sv.flickDecay, 0.1f, 0.0f, 20.0f, "%.1f", &active,
                        "フリック慣性の指数減衰率(/秒)。大きいほどすぐ止まる。\n"
                        "0 = 慣性なし(離した瞬間停止)");
                    changed |= pg::Checkbox("バー表示 Show Bar", &sv.showBar);
                    changed |= pg::Color4("バー色 Bar Color", &sv.barColor.x);
                    pg::End();
                }
                ImGui::TextDisabled("コンテンツ実測: %.0f x %.0f px", sv._contentW, sv._contentH);
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiScrollEdit, changed, active, "UIScrollView");
            }
        }

        // UILayout（自動レイアウト: 直下の子へセル矩形を順に配る）
        if (reg.all_of<UILayout>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entUi : 0, "UILayout");
            bool removed = ComponentRemoveMenu<UILayout>(reg, ctx, ctx.selectedEntity, "UILayout");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiLayoutEdit);
                auto& lay = reg.get<UILayout>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UILayout"))
                {
                    static const char* modes[] = {"縦積み VBox", "横並び HBox", "グリッド Grid"};
                    changed |= pg::Combo("モード Mode", &lay.mode, modes, IM_ARRAYSIZE(modes),
                        "直下の子(UIRect 持ち)へセル矩形を順番に配る。手動 offset 計算なしで\n"
                        "メニュー列/ツールバー/インベントリが組める。子はセル内でアンカー解決\n"
                        "（全面ストレッチの子はセルいっぱいに広がる）");
                    changed |= pg::Float("セル幅 Cell W", &lay.cellW, 1.0f, 0.0f, 4096.0f,
                        "%.0f", &active, "px。縦積み(VBox)では 0 = 親の内側いっぱい");
                    changed |= pg::Float("セル高 Cell H", &lay.cellH, 1.0f, 0.0f, 4096.0f,
                        "%.0f", &active, "px。横並び(HBox)では 0 = 親の内側いっぱい");
                    changed |= pg::Float("間隔 Spacing", &lay.spacing, 0.5f, 0.0f, 512.0f,
                        "%.0f", &active, "セル間の隙間(px)");
                    changed |= pg::FloatN("余白 Padding", &lay.padding.x, 4, 0.5f, 0.0f, 512.0f,
                        "%.0f", &active, "内側余白(左,上,右,下 px)");
                    if (lay.mode == 2)
                        changed |= pg::Int("列数 Grid Cols", &lay.gridCols, 0.1f, 1, 64, &active,
                            "グリッドの列数（行優先で左上から埋まる）");
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiLayoutEdit, changed, active, "UILayout");
            }
        }

        // UIToggle（チェックボックス。クリックで isOn 反転 + onChangeEvent を emit）
        if (reg.all_of<UIToggle>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entUi : 0, "UIToggle");
            bool removed = ComponentRemoveMenu<UIToggle>(reg, ctx, ctx.selectedEntity, "UIToggle");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiToggleEdit);
                auto& tgl = reg.get<UIToggle>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UIToggle"))
                {
                    changed |= pg::Checkbox("オン Is On", &tgl.isOn,
                        "現在の状態。Lua: scene:getUiToggle(e) / scene:setUiToggle(e, on)");

                    char buf[128] = {};
                    size_t n = tgl.onChangeEvent.copy(buf, sizeof(buf) - 1);
                    buf[n] = '\0';
                    if (pg::InputText("変更イベント名 On Change", buf, sizeof(buf), 0, &active,
                        "切替時に events へ emit するイベント名(空なら発火しない)。\n"
                        "Lua 側: events:on(\"名前\", function(e) e.value が 1/0 end)"))
                    { tgl.onChangeEvent = buf; changed = true; }

                    changed |= pg::Color4("箱色 Box", &tgl.boxColor.x);
                    changed |= pg::Color4("チェック色 Check", &tgl.checkColor.x);
                    changed |= pg::Checkbox("操作可能 Interactable", &tgl.interactable);
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiToggleEdit, changed, active, "UIToggle");
            }
        }

        // UIAnimator（UI の出現/ホバー/ループアニメ。Play 中のみ再生。効果は自分と子孫に掛かる）
        if (reg.all_of<UIAnimator>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entUi : 0, "UIAnimator");
            bool removed = ComponentRemoveMenu<UIAnimator>(reg, ctx, ctx.selectedEntity, "UIAnimator");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiAnimatorEdit);
                auto& an = reg.get<UIAnimator>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UIAnimator"))
                {
                    static const char* showAnims[] = {"なし", "フェード", "ポップ(拡大)",
                                                      "左から", "右から", "上から", "下から",
                                                      "スピン(回転入場)", "バウンド落下",
                                                      "フリップ(縦)", "シェイク入場",
                                                      "フリップ(横=扉/カード)"};
                    static const char* easings[] = {"リニア", "イーズイン", "イーズアウト",
                                                    "イン/アウト", "バック(勢い)", "バウンス", "弾性",
                                                    "エクスポ(鋭い減速)", "インバック(溜め)",
                                                    "イン/アウトバック", "クイント(強い減速)",
                                                    "サイン(ゆったり)"};
                    static const char* loops[] = {"なし", "浮遊(上下)", "パルス(拡縮)", "点滅",
                                                  "スピン(連続回転)", "スウィング(揺れ)"};

                    pg::Label("出現 Show",
                        "Play 開始時と Lua scene:showUi() で再生される出現アニメ。\n"
                        "scene:hideUi() は逆再生で消す");
                    changed |= pg::Combo("出現アニメ Show Anim", &an.showAnim,
                                         showAnims, IM_ARRAYSIZE(showAnims));
                    changed |= pg::Float("時間 Duration", &an.showDuration, 0.01f, 0.05f, 5.0f,
                                         "%.2f", &active);
                    changed |= pg::Float("遅延 Delay", &an.showDelay, 0.01f, 0.0f, 10.0f, "%.2f", &active,
                        "再生開始までの秒。複数の UI をずらして順番に出すのに使う");
                    changed |= pg::Combo("イージング Easing", &an.showEasing,
                                         easings, IM_ARRAYSIZE(easings));
                    changed |= pg::Float("スライド距離 Slide", &an.slideOffset, 1.0f, 0.0f, 2000.0f,
                                         "%.0f", &active,
                                         "「〜から」系の移動距離 / バウンド落下の落下距離 / "
                                         "シェイク入場の振幅基準(px)");

                    pg::Label("ホバー/押下 Hover",
                        "同一エンティティに UIButton がある時だけ効く（ボタンの気持ちよさ用）");
                    changed |= pg::Float("ホバー拡大 Hover Scale", &an.hoverScale, 0.005f, 0.5f, 2.0f,
                                         "%.2f", &active);
                    changed |= pg::Float("押下縮小 Press Scale", &an.pressScale, 0.005f, 0.5f, 2.0f,
                                         "%.2f", &active);
                    changed |= pg::Float("追従速度 Speed", &an.hoverSpeed, 0.1f, 1.0f, 40.0f,
                                         "%.0f", &active, "大きいほどキビキビ反応する");

                    pg::Label("ループ Loop", "常時再生されるループアニメ（注目させたい UI に）");
                    changed |= pg::Combo("ループアニメ Loop Anim", &an.loopAnim,
                                         loops, IM_ARRAYSIZE(loops));
                    changed |= pg::Float("速さ Speed (Hz)", &an.loopSpeed, 0.01f, 0.05f, 10.0f,
                                         "%.2f", &active);
                    changed |= pg::Float("量 Amount", &an.loopAmount, 0.1f, 0.0f, 200.0f,
                                         "%.2f", &active, "浮遊=px / パルス・点滅=割合(0.05 = ±5%)");
                    pg::End();
                }
                ImGui::TextDisabled("アニメは Play 中に再生されます（エディタ上は最終ポーズ）");
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiAnimatorEdit, changed, active, "UIAnimator");
            }
        }

        // UIAnimPlayer（タイムラインで作った .uianim クリップの再生器）
        if (reg.all_of<UIAnimPlayer>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entUi : 0, "UIAnimPlayer");
            bool removed = ComponentRemoveMenu<UIAnimPlayer>(reg, ctx, ctx.selectedEntity, "UIAnimPlayer");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiAnimPlayerEdit);
                auto& pl = reg.get<UIAnimPlayer>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UIAnimPlayer"))
                {
                    changed |= pg::InputTextStr("クリップ Clip", pl.clipPath, &active,
                        "assets 相対の .uianim パス（例 uianim/menu_open.uianim）。\n"
                        "アセットブラウザからドラッグしてもええ");
                    changed |= AcceptAssetPathDrop(pl.clipPath, ".uianim", m_assetsDir);
                    changed |= pg::Checkbox("開始時に再生 Play On Start", &pl.playOnStart);
                    changed |= pg::Checkbox("ループ Loop", &pl.loop,
                        "ON: クリップ側の設定に関わらずループする");
                    changed |= pg::Float("速度 Speed", &pl.speed, 0.01f, -8.0f, 8.0f, "%.2f", &active,
                        "負の値で逆再生、0 で一時停止");
                    changed |= pg::InputTextStr("完了イベント Finish Event", pl.finishEvent, &active,
                        "再生完了時に EventBus へ発火するイベント名（空=発火しない）");
                    pg::End();
                }
                if (ImGui::Button("UIアニメーションを開く"))
                {
                    // ★フラグだけ立てると「前回開いていたクリップ」が出たままになる。
                    //   気づかず編集して保存すると**無関係な .uianim を上書きする**。
                    //   同じファイルの Material の Edit ボタン(:2844 付近)は
                    //   pendingOpenMaterialPath を正しく渡しているので、これは設計ではなく漏れ。
                    ctx.showAnimEditor = true;
                    if (!pl.clipPath.empty())
                        ctx.pendingOpenUiAnimPath = m_assetsDir + pl.clipPath;
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiAnimPlayerEdit, changed, active,
                        "UIAnimPlayer");
            }
        }

        // SpriteAnimator（.spranim シートの連番アニメ。Sprite2D / UIImage の UV を駆動する）
        if (reg.all_of<SpriteAnimator>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "SpriteAnimator");
            bool removed = ComponentRemoveMenu<SpriteAnimator>(reg, ctx, ctx.selectedEntity,
                                                               "SpriteAnimator");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_spriteAnimatorEdit);
                auto& sa = reg.get<SpriteAnimator>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("SpriteAnimator"))
                {
                    changed |= pg::InputTextStr("シート Sheet", sa.sheetPath, &active,
                        "assets 相対の .spranim パス（例 spriteanim/player.spranim）");
                    changed |= AcceptAssetPathDrop(sa.sheetPath, ".spranim", m_assetsDir);
                    changed |= pg::InputTextStr("シーケンス Sequence", sa.currentSeq, &active,
                        "再生するシーケンス名（空 = シートの先頭）。Lua は entity:playSprite(\"run\")");
                    changed |= pg::Checkbox("開始時に再生 Play On Start", &sa.playOnStart);
                    changed |= pg::Float("速度 Speed", &sa.speed, 0.01f, 0.0f, 8.0f, "%.2f", &active);
                    changed |= pg::Checkbox("テクスチャも反映 Apply Texture", &sa.applyTexture,
                        "ON: シートの texture を Sprite2D/UIImage の texturePath へも書き込む");
                    pg::End();
                }
                if (!reg.any_of<Sprite2D, UIImage>(ctx.selectedEntity))
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                                       "Sprite2D か UIImage が無いと何も表示されへん");
                if (ImGui::Button("スプライトシートを開く"))
                {
                    ctx.showSpriteSheetEditor = true;   // ↑と同じ理由でパスも渡す
                    if (!sa.sheetPath.empty())
                        ctx.pendingOpenSpriteSheetPath = m_assetsDir + sa.sheetPath;
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_spriteAnimatorEdit, changed, active,
                        "SpriteAnimator");
            }
        }

        // SkeletalAnimation
        if (reg.all_of<SkeletalAnimation>(ctx.selectedEntity))
        {
            auto& skelAnim = reg.get<SkeletalAnimation>(ctx.selectedEntity);
            if (skelAnim.animator && IconHeader(ic, ic ? ic->entMesh : 0, "SkeletalAnimation"))
            {
                if (pg::Begin("SkeletalAnimation"))
                {
                    pg::Text("Bones", "%d",
                        static_cast<int>(skelAnim.skeleton ? skelAnim.skeleton->GetBoneCount() : 0));
                    pg::Text("Clips", "%d", static_cast<int>(skelAnim.clips.size()));
                    pg::End();
                }

                for (i32 i = 0; i < static_cast<i32>(skelAnim.clips.size()); ++i)
                {
                    const auto& clip = skelAnim.clips[i];
                    std::string label = clip->GetName().empty()
                        ? ("Clip " + std::to_string(i))
                        : clip->GetName();
                    if (ImGui::Selectable(label.c_str()))
                        skelAnim.animator->CrossFadeTo(clip.get(), 0.3f);
                }
            }
        }

        // AnimatorController（.animfsm ステートマシン）。
        // 編集できるのはパスとパラメータだけ。グラフの構造は JSON アセット側にあるので
        // ここは「現在の状態を見せるライブ表示」に徹する（決定 2: グラフエディタは作らない）。
        if (reg.all_of<AnimatorController>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "AnimatorController");
            bool removed = ComponentRemoveMenu<AnimatorController>(reg, ctx, ctx.selectedEntity,
                                                                   "AnimatorController");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_animatorControllerEdit);
                auto& ac = reg.get<AnimatorController>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("AnimatorController"))
                {
                    changed |= pg::InputTextStr("グラフ Graph", ac.graphPath, &active,
                        "assets 相対の .animfsm パス（例 animfsm/humanoid_locomotion.animfsm）。"
                        "中身はテキストエディタで編集する");
                    changed |= AcceptAssetPathDrop(ac.graphPath, ".animfsm", m_assetsDir);
                    changed |= pg::Checkbox("開始時に再生 Play On Start", &ac.playOnStart);
                    changed |= pg::Float("速度 Speed", &ac.speed, 0.01f, 0.0f, 4.0f, "%.2f", &active,
                        "グラフ全体の再生速度倍率");
                    changed |= pg::InputTextStr("イベント接頭辞 Event Channel", ac.eventChannel, &active,
                        "クリップイベント名の前に付ける文字列（空 = 素の名前で EventBus へ）");
                    pg::End();
                }

                if (!reg.all_of<SkeletalAnimation>(ctx.selectedEntity))
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                                       "SkeletalAnimation が無いと動きません");

                // --- 読み取り専用のライブ表示 ---
                if (ac._failed)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                       ".animfsm の読み込みに失敗（ログを確認）");
                }
                else if (ac._state && ac._state->valid && reg.all_of<SkeletalAnimation>(ctx.selectedEntity))
                {
                    auto& sa = reg.get<SkeletalAnimation>(ctx.selectedEntity);
                    if (pg::Begin("実行状態 Runtime"))
                    {
                        for (size_t li = 0; li < ac._state->layers.size(); ++li)
                        {
                            const auto& lr  = ac._state->layers[li];
                            const auto& def = ac._state->asset.layers[li];
                            const char* stateName =
                                (lr.curState >= 0 && lr.curState < static_cast<i32>(def.states.size()))
                                ? def.states[static_cast<size_t>(lr.curState)].name.c_str() : "-";
                            pg::Text(def.name.c_str(), "%s  (w=%.2f, t=%.2f)%s",
                                     stateName, lr.weight,
                                     anim_graph::NormalizedTime(*ac._state, static_cast<u32>(li), sa.clips),
                                     lr.inTransition ? "  →遷移中" : "");
                        }
                        for (const auto& [name, v] : ac._state->params)
                        {
                            if (v.type == AnimParamType::Float) pg::Text(name.c_str(), "%.3f", v.f);
                            else                                pg::Text(name.c_str(), "%s", v.b ? "true" : "false");
                        }
                        pg::End();
                    }
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_animatorControllerEdit, changed, active,
                        "AnimatorController");
            }
        }

        // FootIK（接地補正）
        if (reg.all_of<FootIK>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "FootIK");
            bool removed = ComponentRemoveMenu<FootIK>(reg, ctx, ctx.selectedEntity, "FootIK");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_footIkEdit);
                auto& ik = reg.get<FootIK>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("FootIK"))
                {
                    changed |= pg::Checkbox("有効 Enabled", &ik.enabled);
                    changed |= pg::Float("効き Weight", &ik.weight, 0.01f, 0.0f, 1.0f, "%.2f", &active,
                        "0 で無効、1 で完全に接地補正する");
                    changed |= pg::Float("足首の高さ Foot Height", &ik.footHeight, 0.005f, 0.0f, 0.5f,
                        "%.3f", &active, "レストポーズでの足首の地面からの高さ(m)");
                    changed |= pg::Float("レイ開始オフセット Ray Up", &ik.rayUpOffset, 0.01f, 0.0f, 2.0f,
                        "%.2f", &active, "足首から何 m 上からレイを打つか");
                    changed |= pg::Float("レイ長さ Ray Length", &ik.rayLength, 0.01f, 0.05f, 5.0f,
                        "%.2f", &active);
                    changed |= pg::Float("腰下げ上限 Max Pelvis Drop", &ik.maxPelvisDrop, 0.01f, 0.0f, 2.0f,
                        "%.2f", &active, "これを超える段差は諦める(m)");
                    changed |= pg::Checkbox("面法線に合わせる Align To Normal", &ik.alignToNormal,
                        "斜面で足を寝かせる");
                    changed |= pg::Float("傾きの上限 Max Foot Pitch", &ik.maxFootPitchDeg, 1.0f, 0.0f, 90.0f,
                        "%.0f", &active, "急斜面で足が不自然にねじれるのを防ぐ(度)");
                    changed |= pg::Float("平滑時間 Smooth Time", &ik.smoothTime, 0.005f, 0.0f, 0.5f,
                        "%.3f", &active, "段差でレイが飛ぶのをならす時定数(秒)");
                    changed |= pg::Float("フェード時間 Fade Out", &ik.fadeOutTime, 0.005f, 0.0f, 1.0f,
                        "%.3f", &active, "非接地(ジャンプ中など)で IK を切る時間(秒)");
                    pg::End();
                }

                if (pg::Begin("ボーン指定 Bones（空 = 自動推定）"))
                {
                    changed |= pg::InputTextStr("左 股関節 L Hip",  ik.leftHipBone,  &active);
                    changed |= pg::InputTextStr("左 膝 L Knee",     ik.leftKneeBone, &active);
                    changed |= pg::InputTextStr("左 足首 L Foot",   ik.leftFootBone, &active);
                    changed |= pg::InputTextStr("右 股関節 R Hip",  ik.rightHipBone,  &active);
                    changed |= pg::InputTextStr("右 膝 R Knee",     ik.rightKneeBone, &active);
                    changed |= pg::InputTextStr("右 足首 R Foot",   ik.rightFootBone, &active);
                    changed |= pg::InputTextStr("腰 Pelvis",        ik.pelvisBone,    &active);
                    pg::End();
                }

                if (!reg.all_of<SkeletalAnimation>(ctx.selectedEntity))
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                                       "SkeletalAnimation が無いと動きません");
                else if (ik._resolveFailed)
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                       "足ボーンを特定できません。上のボーン名を明示指定してください"
                                       "（候補はログに出ています）");

                // 読み取り専用のライブ表示（Play 中のみ動く）
                if (ik._resolved)
                {
                    auto& sa = reg.get<SkeletalAnimation>(ctx.selectedEntity);
                    auto boneName = [&](i32 b) -> const char* {
                        return (sa.skeleton && b >= 0 && static_cast<u32>(b) < sa.skeleton->GetBoneCount())
                             ? sa.skeleton->GetBone(static_cast<u32>(b)).name.c_str() : "-";
                    };
                    if (pg::Begin("解決結果 / 実行状態"))
                    {
                        pg::Text("左脚", "%s / %s / %s",
                                 boneName(ik._lHip), boneName(ik._lKnee), boneName(ik._lFoot));
                        pg::Text("右脚", "%s / %s / %s",
                                 boneName(ik._rHip), boneName(ik._rKnee), boneName(ik._rFoot));
                        pg::Text("腰", "%s", boneName(ik._pelvis));
                        pg::Text("接地", "L=%s(w %.2f)  R=%s(w %.2f)",
                                 ik._lContact ? "○" : "×", ik._lWeight,
                                 ik._rContact ? "○" : "×", ik._rWeight);
                        pg::Text("補正量", "L %+.3fm  R %+.3fm  腰 %+.3fm",
                                 ik._lLift, ik._rLift, ik._pelvisDrop);
                        pg::End();
                    }
                    ImGui::TextDisabled("※ Play 中のみ動きます（物理ボディが要るため）");
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_footIkEdit, changed, active, "FootIK");
            }
        }

        // NodeAnimation
        if (reg.all_of<NodeAnimationComp>(ctx.selectedEntity))
        {
            auto& nodeAnim = reg.get<NodeAnimationComp>(ctx.selectedEntity);
            if (nodeAnim.nodeAnimator && IconHeader(ic, ic ? ic->entMesh : 0, "NodeAnimation"))
            {
                ImGui::Text("Clips: %d", static_cast<int>(nodeAnim.clips.size()));
                for (i32 i = 0; i < static_cast<i32>(nodeAnim.clips.size()); ++i)
                {
                    const auto& clip = nodeAnim.clips[i];
                    std::string label = clip->GetName().empty()
                        ? ("Clip " + std::to_string(i))
                        : clip->GetName();
                    if (ImGui::Selectable(label.c_str()))
                        nodeAnim.nodeAnimator->CrossFadeTo(clip.get(), 0.3f);
                }
            }
        }

        // GridPlane
        if (reg.all_of<GridPlane>(ctx.selectedEntity))
        {
            if (IconHeader(ic, ic ? ic->entEmpty : 0, "GridPlane"))
            {
                auto& gp = reg.get<GridPlane>(ctx.selectedEntity);
                if (pg::Begin("GridPlane"))
                {
                    pg::Checkbox("有効 Enabled", &gp.enabled);
                    pg::End();
                }
            }
        }

        // ライト（Point / Directional / Spot）は上部の専用ヒーローカード（RenderLightHero）で編集する。

        // Gimmick（ステージギミック: 時間で動く/塞ぐ部品。動きは Lua が駆動）
        if (reg.all_of<Gimmick>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "Gimmick");
            bool removed = ComponentRemoveMenu<Gimmick>(reg, ctx, ctx.selectedEntity, "Gimmick");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_gimmickEdit);
                auto& gm = reg.get<Gimmick>(ctx.selectedEntity);
                bool changed = false, active = false;
                const char* kinds[] = { "Static Wall（動かない壁）", "Spike Pulse（上下するトゲ）",
                                        "Slide X（左右に動く）", "Slide Z（前後に動く）" };
                if (pg::Begin("Gimmick"))
                {
                    changed |= pg::Combo("種類 Kind", &gm.kind, kinds, IM_ARRAYSIZE(kinds));
                    if (gm.kind < 0) gm.kind = 0; if (gm.kind > 3) gm.kind = 3;
                    if (gm.kind != 0)  // Static 以外は動きパラメータを表示
                    {
                        changed |= pg::Float("周期 Period(s)", &gm.period, 0.05f, 0.2f, 30.0f, "%.2f", &active);
                        changed |= pg::SliderFloat("位相 Phase", &gm.phase, 0.0f, 1.0f, "%.2f", &active);
                        changed |= pg::Float("振幅 Amplitude", &gm.amplitude, 0.05f, 0.0f, 30.0f, "%.2f", &active);
                    }
                    if (gm.kind == 1)  // SpikePulse 固有
                    {
                        changed |= pg::SliderFloat("塞ぐ閾値 Threshold", &gm.threshold, 0.0f, 1.0f, "%.2f", &active);
                        changed |= pg::Checkbox("直撃死 Deadly", &gm.deadly);
                    }
                    changed |= pg::Checkbox("当たり判定 Solid", &gm.solid);
                    pg::End();
                }
                ImGui::TextDisabled("基準位置=Transform。動き/早送りはゲーム側が駆動します");
                EndEdit(reg, ctx, ctx.selectedEntity, m_gimmickEdit, changed, active, "Gimmick");
            }
        }

        // ParticleEmitter（配置できるエフェクト部品）
        if (reg.all_of<ParticleEmitter>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "Particle Emitter");
            bool removed = ComponentRemoveMenu<ParticleEmitter>(reg, ctx, ctx.selectedEntity, "Particle Emitter");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_emitterEdit);
                auto& pe = reg.get<ParticleEmitter>(ctx.selectedEntity);
                bool changed = false, active = false;
                const char* kinds[] = { "Glow", "Fire", "Smoke", "Spark", "Magic", "Electric", "Ring", "Star" };
                const char* blends[] = { "加算 Additive", "アルファ Alpha" };
                if (pg::Begin("ParticleEmitter"))
                {
                    changed |= pg::Combo("見た目 Kind", &pe.kind, kinds, IM_ARRAYSIZE(kinds));
                    changed |= pg::Combo("合成 Blend", &pe.blend, blends, IM_ARRAYSIZE(blends));
                    {
                        const char* orients[] = { "ビルボード(カメラ正対)", "水平(地面向き)", "垂直(+Z正対)" };
                        changed |= pg::Combo("向き Orient", &pe.orient, orients, IM_ARRAYSIZE(orients),
                            "粒子クアッドの向き。ビルボード=常にカメラを向く(従来)。\n"
                            "水平=XZ平面(リング/衝撃波/魔法陣)。垂直=XY平面固定。\n"
                            "stretch>0 の速度ストレッチ時と GPUパーティクルでは無効");
                    }
                    {
                        // ★static バッファ + 確定時コミットだった。バッファが全エンティティで
                        //   共有なので、パスを打ってから Enter を押さずにヒエラルキーで別の
                        //   エンティティをクリックすると（Hierarchy が Inspector より前に走る）、
                        //   入力が捨てられるか **選び直した方の放出器へ書き込まれる**。
                        //   Sprite2D 側と同じ「ローカルバッファ + 打鍵ごとにコミット」に揃える。
                        char texBuf[260] = {};
                        const size_t tn = pe.texturePath.copy(texBuf, sizeof(texBuf) - 1);
                        texBuf[tn] = '\0';
                        pg::Label("テクスチャ", "assetsからの相対パス。空=プロシージャル質感");
                        if (ImGui::InputTextWithHint("##peTex", "空=プロシージャル質感", texBuf, sizeof(texBuf)))
                        { pe.texturePath = texBuf; changed = true; }
                    }
                    // ★無効になるのは GpuParticleSystem::EmitRequest に無いものが全部。
                    //   ここを増やしたら EmitRequest と突き合わせて更新すること。
                    //   ツールチップだけだとホバーしないと読まれないので、本文にも出す
                    //   （「設定したのに変わらない」で悩ませないため）。
                    changed |= pg::Checkbox("GPUパーティクル", &pe.gpu,
                        "compute シムで最大 131072 粒子（加算専用・大量粒子向け）。\n"
                        "CPU 専用の項目は無効: 中間色 / 中間サイズ / 乱流の細かさ / 画面歪み /\n"
                        "ライト放出 / 明滅 / テクスチャ / 向き / 合成（加算固定）");
                    if (pe.gpu)
                        WarnRow("GPU では無効: 中間色・中間サイズ・乱流の細かさ・画面歪み・"
                                "ライト放出・明滅・テクスチャ・向き（合成は加算固定）");
                    changed |= pg::Float("放出レート Rate(/s)", &pe.rate, 0.5f, 0.0f, pe.gpu ? 100000.0f : 500.0f, "%.1f", &active);
                    changed |= pg::Checkbox("Play開始で放出", &pe.playOnStart);
                    changed |= pg::Checkbox("ループ Looping", &pe.looping);
                    if (!pe.looping)
                        changed |= pg::Float("継続秒 Duration", &pe.duration, 0.05f, 0.0f, 60.0f, "%.2f", &active);

                    pg::Group("見た目");
                    changed |= pg::Color3("開始色 Color", &pe.color.x);
                    changed |= pg::Checkbox("中間色を使う", &pe.hasColorMid);
                    if (pe.hasColorMid)
                        changed |= pg::Color3("中間色 ColorMid", &pe.colorMid.x);
                    changed |= pg::Color3("終了色 ColorEnd", &pe.colorEnd.x);
                    changed |= pg::Float("輝度 Intensity", &pe.intensity, 0.05f, 0.0f, 30.0f, "%.2f", &active);
                    changed |= pg::Float("サイズ Size", &pe.size, 0.01f, 0.0f, 10.0f, "%.3f", &active);
                    changed |= pg::Float("中間サイズ SizeMid", &pe.sizeMid, 0.01f, -1.0f, 10.0f, "%.3f", &active,
                        "-1で無効。0以上で 開始→中間→終了 の3キーサイズカーブ");
                    changed |= pg::Float("終了サイズ SizeEnd", &pe.sizeEnd, 0.01f, 0.0f, 10.0f, "%.3f", &active);
                    changed |= pg::Float("寿命 Life(s)", &pe.life, 0.01f, 0.01f, 30.0f, "%.2f", &active);
                    changed |= pg::SliderFloat("寿命ばらつき", &pe.lifeVar, 0.0f, 1.0f, "%.2f", &active);

                    pg::Group("動き");
                    changed |= pg::Float3("方向 Dir", &pe.dir.x, 0.01f, 0, 0, "%.2f", &active);
                    changed |= pg::SliderFloat("拡がり Spread", &pe.spread, 0.0f, 1.0f, "%.2f", &active);
                    changed |= pg::Float("速度 Speed", &pe.speed, 0.02f, 0.0f, 50.0f, "%.2f", &active);
                    changed |= pg::SliderFloat("速度ばらつき", &pe.speedVar, 0.0f, 1.0f, "%.2f", &active);
                    changed |= pg::Float("重力 Gravity", &pe.gravity, 0.02f, -50.0f, 50.0f, "%.2f", &active);
                    changed |= pg::Float("抵抗 Drag", &pe.drag, 0.02f, 0.0f, 10.0f, "%.2f", &active);
                    changed |= pg::Float("上向き Up", &pe.up, 0.02f, 0.0f, 10.0f, "%.2f", &active);
                    changed |= pg::Float("ストレッチ Stretch", &pe.stretch, 0.02f, 0.0f, 10.0f, "%.2f", &active);
                    changed |= pg::Float("乱流 Turbulence", &pe.turbStrength, 0.01f, 0.0f, 10.0f, "%.2f", &active,
                        ">0 でカールノイズによる有機的な揺らぎ（煙/炎向け）");
                    if (pe.turbStrength > 0.0f)
                        changed |= pg::Float("乱流の細かさ", &pe.turbFreq, 0.01f, 0.01f, 10.0f, "%.2f", &active);

                    pg::Group("特殊効果");
                    changed |= pg::SliderFloat("画面歪み Distort", &pe.distort, 0.0f, 3.0f, "%.2f", &active,
                        ">0 で色でなく画面の歪みを描く（熱ゆらぎ。Kind=Ring で衝撃波）");
                    changed |= pg::Checkbox("ライト放出 Light", &pe.light,
                        "明るい粒子の上位数個が実ポイントライトになり周囲を照らす（炎/魔法）");
                    if (pe.light)
                        changed |= pg::Float("光の距離 Range", &pe.lightRange, 0.05f, 0.1f, 50.0f, "%.2f", &active);
                    changed |= pg::SliderFloat("明滅 Flicker", &pe.flicker, 0.0f, 1.0f, "%.2f", &active);
                    if (pe.flicker > 0.0f)
                        changed |= pg::Float("明滅の速さ", &pe.flickerFreq, 0.2f, 0.1f, 60.0f, "%.1f", &active);
                    pg::End();
                }
                ImGui::TextDisabled("エディタでもプレビュー表示されます。詳細な作成は ツール > パーティクルエディタ が便利です");
                EndEdit(reg, ctx, ctx.selectedEntity, m_emitterEdit, changed, active, "Particle Emitter");
            }
        }

        // TrailRenderer（軌跡リボン: 剣の残像/弾道/魔法の尾）
        if (reg.all_of<TrailRenderer>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "Trail Renderer");
            bool removed = ComponentRemoveMenu<TrailRenderer>(reg, ctx, ctx.selectedEntity, "Trail Renderer");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_trailEdit);
                auto& tr = reg.get<TrailRenderer>(ctx.selectedEntity);
                bool changed = false, active = false;
                const char* tblends[] = { "加算 Additive", "アルファ Alpha" };
                if (pg::Begin("TrailRenderer"))
                {
                    changed |= pg::Checkbox("記録中 Emitting", &tr.emitting);
                    changed |= pg::Float("幅 Width", &tr.width, 0.01f, 0.01f, 10.0f, "%.3f", &active);
                    changed |= pg::Float("寿命 Life(s)", &tr.life, 0.02f, 0.05f, 10.0f, "%.2f", &active);
                    changed |= pg::Color3("先頭色 Color", &tr.color.x);
                    changed |= pg::Color3("尾の色 ColorEnd", &tr.colorEnd.x);
                    changed |= pg::Float("輝度 Intensity", &tr.intensity, 0.05f, 0.0f, 30.0f, "%.2f", &active);
                    changed |= pg::Combo("合成 Blend", &tr.blend, tblends, IM_ARRAYSIZE(tblends));
                    changed |= pg::Float("最小移動 MinDist", &tr.minDist, 0.005f, 0.001f, 2.0f, "%.3f", &active);
                    pg::End();
                }
                ImGui::TextDisabled("エンティティを動かすと軌跡の帯が出ます（エディタでもプレビュー）");
                EndEdit(reg, ctx, ctx.selectedEntity, m_trailEdit, changed, active, "Trail Renderer");
            }
        }

        // Decal（投影デカール: 弾痕/血/汚れ/水たまり）
        if (reg.all_of<DecalComponent>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "Decal");
            bool removed = ComponentRemoveMenu<DecalComponent>(reg, ctx, ctx.selectedEntity, "Decal");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_decalEdit);
                auto& dc = reg.get<DecalComponent>(ctx.selectedEntity);
                bool changed = false, active = false;
                if (pg::Begin("Decal"))
                {
                    pg::Group("アトラス");
                    changed |= pg::Float4("カラー矩形 UV", &dc.atlasUV.x, 0.005f, 0.0f, 1.0f, "%.4f", &active,
                        "シーン設定のデカールアトラスの中の (u0, v0, 幅, 高さ)。既定は全面");
                    changed |= pg::Float4("法線矩形 UV", &dc.atlasUVNormal.x, 0.005f, 0.0f, 1.0f, "%.4f", &active,
                        "同アトラス内の法線マップ領域。高さ(4番目)を 0 にすると法線ブレンド無し");

                    pg::Group("見た目");
                    changed |= pg::Color3("色 Tint", &dc.tint.x);
                    changed |= pg::SliderFloat("不透明度 Opacity", &dc.opacity, 0.0f, 1.0f, "%.2f", &active);
                    changed |= pg::Color3("自己発光 Emissive", &dc.emissive.x);
                    changed |= pg::SliderFloat("法線の強さ", &dc.normalStrength, 0.0f, 2.0f, "%.2f", &active);
                    changed |= pg::Float("粗さ上書き Roughness", &dc.roughness, 0.01f, -1.0f, 1.0f, "%.2f", &active,
                        "-1 で「変更しない」。0..1 で受け面のラフネスを上書きする（濡れた床など）");
                    changed |= pg::Float("金属度上書き Metallic", &dc.metallic, 0.01f, -1.0f, 1.0f, "%.2f", &active,
                        "-1 で「変更しない」");

                    pg::Group("投影");
                    changed |= pg::SliderFloat("角度フェード(度)", &dc.angleFadeDeg, 0.0f, 89.0f, "%.0f", &active);
                    changed |= pg::SliderFloat("縁フェード", &dc.fadeEdge, 0.001f, 0.5f, "%.3f", &active);
                    changed |= pg::Int("重ね順 SortOrder", &dc.sortOrder, 1.0f, -999, 999, &active,
                        "小さいものから下に重なる");
                    pg::End();
                }
                ImGui::TextDisabled("Transform の scale が投影ボックスの大きさ。ローカル -Y 方向へ投影します");
                ImGui::TextDisabled("アトラスは「ツール > ライティング」ではなくシーン設定 decalAtlas で指定します");
                EndEdit(reg, ctx, ctx.selectedEntity, m_decalEdit, changed, active, "Decal");
            }
        }

        // NetworkIdentity（マルチプレイ複製対象の印。netId/owner はランタイム表示のみ）
        if (reg.all_of<NetworkIdentity>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "Network Identity");
            bool removed = ComponentRemoveMenu<NetworkIdentity>(reg, ctx, ctx.selectedEntity, "Network Identity");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_netIdEdit);
                auto& ni = reg.get<NetworkIdentity>(ctx.selectedEntity);
                bool changed = false, active = false;
                if (pg::Begin("NetworkIdentity"))
                {
                    changed |= pg::Float("関連半径 InterestRadius", &ni.interestRadius, 0.5f, 0.0f, 1000.0f, "%.1f", &active,
                        "0 = 常に全クライアントへ複製。>0 にすると観測者からこの距離を超えた相手へは"
                        "スナップショットを送らない（★実装済み・稼働中）");
                    // ★予約フィールド。src/network/ に読者が 1 箇所も無い（権限判定は
                    //   _isLocalOwner + NetworkTransform::syncMode で行っている）。
                    //   触れると「設定したのに何も起きない」になるので触らせない。
                    ImGui::BeginDisabled(true);
                    ImGui::Checkbox("サーバー権威（予約・未実装）", &ni.serverAuthority);
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("クライアント権威エンティティ用の予約枠。"
                                          "現状どこからも読まれないので、変えても挙動は変わらない");
                    pg::Text("netId（実行時割当）", "%d", static_cast<int>(ni._netId));
                    pg::Text("owner clientId", "%d", static_cast<int>(ni._owner));
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_netIdEdit, changed, active, "Network Identity");
            }
        }

        // NetworkTransform（Transformのスナップショット複製設定。NetworkIdentityと併用）
        if (reg.all_of<NetworkTransform>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "Network Transform");
            bool removed = ComponentRemoveMenu<NetworkTransform>(reg, ctx, ctx.selectedEntity, "Network Transform");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_netTfEdit);
                auto& nt = reg.get<NetworkTransform>(ctx.selectedEntity);
                bool changed = false, active = false;
                const char* modes[] = { "補間 Interpolated", "オーナー予測 Predicted" };
                if (pg::Begin("NetworkTransform"))
                {
                    changed |= pg::Combo("同期モード SyncMode", &nt.syncMode, modes, IM_ARRAYSIZE(modes));
                    pg::Label("同期する要素");
                    changed |= ImGui::Checkbox("位置", &nt.syncPosition);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("回転", &nt.syncRotation);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("拡縮", &nt.syncScale);
                    changed |= pg::Float("補間遅延 (ms)", &nt.interpDelayMs, 1.0f, 0.0f, 1000.0f, "%.0f", &active,
                        "受信スナップショットをこの時間だけ遅らせて補間する(ジッター吸収)");
                    changed |= pg::Float("テレポート距離", &nt.snapDistance, 0.1f, 0.0f, 100.0f, "%.1f", &active);
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_netTfEdit, changed, active, "Network Transform");
            }
        }

        // Trigger（イベント: 範囲に入る/出る/居る で宣言アクションを実行）
        if (reg.all_of<Trigger>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "Trigger");
            bool removed = ComponentRemoveMenu<Trigger>(reg, ctx, ctx.selectedEntity, "Trigger");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_triggerEdit);
                auto& tr = reg.get<Trigger>(ctx.selectedEntity);
                const char* shapes[] = { "Box", "Sphere" };
                // ★参照は guid が正。名前だけ書き換えると古い guid が勝って
                //   コンボの操作が黙って無視されるので、必ず両方を同時に書く。
                //   まだ guid が振られていないエンティティ（保存前）は 0 のままにし、
                //   次の保存で付いた値を読み込み時の昇格が拾う。
                const auto setRef = [&reg](entt::entity picked, std::string& name, uint64_t& guid) {
                    if (picked == entt::null) { name.clear(); guid = 0; return; }
                    const auto* n = reg.try_get<dx12e::NameTag>(picked);
                    name = n ? n->name : std::string{};
                    const auto* g = reg.try_get<dx12e::EntityGuid>(picked);
                    guid = g ? g->value : 0;
                };
                if (pg::Begin("Trigger"))
                {
                    pg::Combo("形 Shape", &tr.shape, shapes, IM_ARRAYSIZE(shapes));
                    if (tr.shape == 0) pg::Float3("半径 HalfExtents", &tr.halfExtents.x, 0.05f, 0.0f, 1000.0f, "%.2f");
                    else               pg::Float("半径 Radius", &tr.radius, 0.05f, 0.0f, 1000.0f, "%.2f");
                    pg::Float3("オフセット Offset", &tr.offset.x, 0.05f, 0, 0, "%.2f");
                    {
                        const char* cur = tr.filter.empty() ? "Player（既定）" : tr.filter.c_str();
                        pg::Label("対象 Filter");
                        if (ImGui::BeginCombo("##trFilter", cur))
                        {
                            if (ImGui::Selectable("Player（既定）", tr.filter.empty()))
                                setRef(entt::null, tr.filter, tr.filterGuid);
                            auto nv = reg.view<dx12e::NameTag>();
                            for (auto ne : nv)
                            { const auto& nm = nv.get<dx12e::NameTag>(ne).name; if (nm.empty()) continue;
                              if (ImGui::Selectable(nm.c_str(), nm == tr.filter))
                                  setRef(ne, tr.filter, tr.filterGuid); }
                            ImGui::EndCombo();
                        }
                    }
                    pg::Checkbox("一度だけ Once", &tr.once);
                    pg::End();
                }
                ImGui::SeparatorText("アクション");
                int removeIdx = -1;
                for (size_t i = 0; i < tr.actions.size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    auto& a = tr.actions[i];
                    if (pg::Begin("TriggerAction"))
                    {
                        const char* whens[] = { "入った時 Enter", "出た時 Exit", "居る間 Stay" };
                        pg::Combo("いつ When", &a.when, whens, IM_ARRAYSIZE(whens));
                        const char* types[] = { "Enable", "Disable", "Destroy", "Move", "PlayEffect",
                                                "StopEffect", "PlaySound", "LoadScene", "FadeToScene",
                                                "SetProperty", "EmitEvent" };
                        pg::Combo("何を Type", &a.type, types, IM_ARRAYSIZE(types));
                        {
                            const char* cur = a.target.empty() ? "(なし=Filter対象)" : a.target.c_str();
                            pg::Label("対象 Target");
                            if (ImGui::BeginCombo("##actTarget", cur))
                            {
                                if (ImGui::Selectable("(なし=Filter対象)", a.target.empty()))
                                    setRef(entt::null, a.target, a.targetGuid);
                                auto nv = reg.view<dx12e::NameTag>();
                                for (auto ne : nv)
                                { const auto& nm = nv.get<dx12e::NameTag>(ne).name; if (nm.empty()) continue;
                                  if (ImGui::Selectable(nm.c_str(), nm == a.target))
                                      setRef(ne, a.target, a.targetGuid); }
                                ImGui::EndCombo();
                            }
                        }
                        char buf[256];
                        if (a.type == 3) pg::Float3("移動量 Vec", &a.vec.x, 0.05f, 0, 0, "%.2f");
                        if (a.type == 7 || a.type == 8)
                        { std::memset(buf, 0, sizeof(buf)); strncpy_s(buf, sizeof(buf), a.str.c_str(), _TRUNCATE);
                          if (pg::InputText("シーン Path", buf, sizeof(buf))) a.str = buf; }
                        if (a.type == 8)
                        { float d = static_cast<float>(a.num); if (pg::Float("秒 Dur", &d, 0.05f, 0.0f, 10.0f, "%.2f")) a.num = d; }
                        if (a.type == 9 || a.type == 10)
                        { std::memset(buf, 0, sizeof(buf)); strncpy_s(buf, sizeof(buf), a.str.c_str(), _TRUNCATE);
                          const char* hint = a.type == 9 ? "プロパティ名 Prop" : "イベント名 Event";
                          if (pg::InputText(hint, buf, sizeof(buf))) a.str = buf;
                          float v = static_cast<float>(a.num); if (pg::Float("値 Value", &v, 0.05f)) a.num = v; }
                        pg::End();
                    }
                    if (ImGui::SmallButton("このアクションを削除")) removeIdx = static_cast<int>(i);
                    ImGui::Separator();
                    ImGui::PopID();
                }
                if (removeIdx >= 0) tr.actions.erase(tr.actions.begin() + removeIdx);
                if (ImGui::Button("＋ アクション追加")) tr.actions.push_back(dx12e::TriggerAction{});
                ImGui::TextDisabled("Play 中に評価。Target 空のアクションは Filter 対象に作用");

                // ★この節はウィジェットが多く changed を全部拾うと差分が大きいので、
                //   スナップショットとの比較そのものを changed として渡す（operator== がある）。
                //   ドラッグ中は active が優先されるので、離した時に 1 エントリだけ積まれる。
                const bool trChanged = !(m_triggerEdit.snapshot == tr);
                EndEdit(reg, ctx, ctx.selectedEntity, m_triggerEdit,
                        trChanged, ImGui::IsAnyItemActive(), "Trigger");
            }
        }

        // CameraComponent
        if (reg.all_of<CameraComponent>(ctx.selectedEntity))
        {
            // コンソールのエラー行から飛んできたら、Camera と下の「画面シェーダー」節を開く
            const bool revealCamScreen =
                RevealMatches(reg.get<CameraComponent>(ctx.selectedEntity).screenShaderPath);
            if (revealCamScreen) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            bool open = IconHeader(ic, ic ? ic->entCamera : 0, "Camera");
            if (revealCamScreen) ImGui::SetScrollHereY(0.15f);
            bool removed = ComponentRemoveMenu<CameraComponent>(reg, ctx, ctx.selectedEntity, "Camera");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_camEdit);
                auto& cam = reg.get<CameraComponent>(ctx.selectedEntity);
                bool changed = false, active = false;

                // 投影方式（透視 / 正射）。正射＝平行投影は距離で大きさが変わらない（2D/見下ろし向け）。
                // 近づくとスプライト/メッシュを大きく見せたいなら「透視」を選ぶ。
                if (pg::Begin("Camera"))
                {
                    int projIdx = (cam.projection == CameraProjection::Orthographic) ? 1 : 0;
                    const char* projItems[] = { "Perspective (透視)", "Orthographic (正射)" };
                    if (pg::Combo("投影 Projection", &projIdx, projItems, 2,
                                  "正射は距離で大きさが変わりません（2D/見下ろし向け）。\n"
                                  "近づくと大きくしたいなら透視を選びます。"))
                    {
                        cam.projection = (projIdx == 1) ? CameraProjection::Orthographic
                                                        : CameraProjection::Perspective;
                        changed = true;
                    }

                    // 透視は FOV、正射は Ortho Size（ビュー縦の半分の世界単位）を出す。
                    if (cam.projection == CameraProjection::Perspective)
                        changed |= pg::Float("視野角 FOV", &cam.fovDegrees, 1.0f, 1.0f, 179.0f, "%.1f", &active);
                    else
                        changed |= pg::Float("Ortho Size", &cam.orthoSize, 0.1f, 0.01f, 1000.0f, "%.2f", &active);
                    changed |= pg::Float("Near", &cam.nearClip, 0.01f, 0.001f, 100.0f, "%.3f", &active);
                    changed |= pg::Float("Far", &cam.farClip, 10.0f, 1.0f, 100000.0f, "%.0f", &active);
                    if (pg::Checkbox("アクティブ Active", &cam.isActive))
                    {
                        changed = true;
                        // アクティブカメラは常に1つだけ
                        if (cam.isActive)
                        {
                            for (auto [oe, oc] : reg.view<CameraComponent>().each())
                                if (oe != ctx.selectedEntity) oc.isActive = false;
                        }
                    }
                    pg::End();
                }

                // ── 画面全体のカスタムシェーダー（スクリーンシェーダー）──
                // ポストプロセスが終わった【完成した絵】をこの .hlsl が受け取って書き換える。
                // MeshRenderer の shaderPath が「1 個のモデルの描き方」を差し替えるのに対し、
                // こちらは「画面そのもの」を差し替える。
                if (revealCamScreen) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                if (IconHeader(nullptr, 0, "\xe7\x94\xbb\xe9\x9d\xa2\xe3\x82\xb7\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\x80\xe3\x83\xbc##CamScreenShader"))
                {
                    namespace fs = std::filesystem;
                    if (pg::Begin("CameraScreenShader"))
                    {
                        pg::Label("\xe3\x82\xb7\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\x80\xe3\x83\xbc");  // シェーダー
                        std::string label = cam.screenShaderPath.empty()
                            ? "\xe3\x81\xaa\xe3\x81\x97\xef\xbc\x88\xe9\x80\x9a\xe5\xb8\xb8\xe6\x8f\x8f\xe7\x94\xbb\xef\xbc\x89"  // なし（通常描画）
                            : cam.screenShaderPath;
                        if (ImGui::BeginCombo("##camScreenShader", label.c_str()))
                        {
                            if (ImGui::Selectable("\xe3\x81\xaa\xe3\x81\x97\xef\xbc\x88\xe9\x80\x9a\xe5\xb8\xb8\xe6\x8f\x8f\xe7\x94\xbb\xef\xbc\x89",
                                                  cam.screenShaderPath.empty()))
                            { cam.screenShaderPath.clear(); changed = true; }

                            std::error_code ec;
                            fs::path root(m_assetsDir + "shaders/");
                            if (fs::exists(root, ec))
                            {
                                fs::recursive_directory_iterator it(root,
                                    fs::directory_options::skip_permission_denied, ec);
                                fs::recursive_directory_iterator end;
                                for (; !ec && it != end; it.increment(ec))
                                {
                                    std::error_code fec;
                                    if (!it->is_regular_file(fec) || fec) continue;
                                    if (it->path().extension() != L".hlsl") continue;
                                    fs::path rel = fs::relative(it->path(), root, fec);
                                    if (fec) continue;
                                    std::string relStr = rel.generic_string();
                                    if (FindShaderSourceByRelPath(relStr) != nullptr) continue;
                                    if (ImGui::Selectable(relStr.c_str(), cam.screenShaderPath == relStr))
                                    { cam.screenShaderPath = relStr; changed = true; }
                                }
                            }
                            ImGui::EndCombo();
                        }
                        {   // ★.hlsl をここへ D&D するだけで画面全体のシェーダーになる
                            std::string dropped;
                            if (assetdrop::AcceptShader(dropped, m_assetsDir + "shaders/"))
                            { cam.screenShaderPath = dropped; changed = true; }
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(".hlsl \xe3\x82\x92\xe3\x81\x93\xe3\x81\x93\xe3\x81\xb8\xe3\x83\x89\xe3\x83\xad\xe3\x83\x83\xe3\x83\x97"
                                              "\xe3\x81\xa7\xe7\x94\xbb\xe9\x9d\xa2\xe5\x85\xa8\xe4\xbd\x93\xe3\x81\xab\xe9\x81\xa9\xe7\x94\xa8\xe3\x81\xa7\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x99");

                        if (!cam.screenShaderPath.empty())
                        {
                            changed |= pg::Checkbox("\xe6\x9c\x89\xe5\x8a\xb9 Enabled", &cam.screenShaderEnabled,
                                "\xe5\x89\xb2\xe3\x82\x8a\xe5\xbd\x93\xe3\x81\xa6\xe3\x81\x9f\xe3\x81\xbe\xe3\x81\xbe\xe4\xb8\x80\xe6\x99\x82\xe7\x9a\x84\xe3\x81\xab\xe5\x88\x87\xe3\x82\x8b");
                            changed |= pg::Float4("\xe3\x83\x91\xe3\x83\xa9\xe3\x83\xa1\xe3\x83\xbc\xe3\x82\xbf\xe3\x83\xbc params",
                                &cam.screenShaderParams.x, 0.01f, 0.0f, 0.0f, "%.3f", &active,
                                "HLSL \xe5\x81\xb4\xe3\x81\xaf cbuffer ScreenShaderCB \xe3\x81\xae params \xe3\x81\xa7\xe8\xaa\xad\xe3\x82\x81\xe3\x81\xbe\xe3\x81\x99");
                        }
                        pg::End();
                    }
                    ShaderIssueBox(cam.screenShaderPath, dx12e::shaderdiag::kIdScreen, "CamScreen");
                    if (!cam.isActive && !cam.screenShaderPath.empty())
                        WarnText("\xe3\x82\xa2\xe3\x82\xaf\xe3\x83\x86\xe3\x82\xa3\xe3\x83\x96\xe3\x81\xaa\xe3\x82\xab\xe3\x83\xa1\xe3\x83\xa9\xe3\x81\xa0\xe3\x81\x91\xe3\x81\xab\xe9\x81\xa9\xe7\x94\xa8\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xbe\xe3\x81\x99");
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_camEdit, changed, active, "Camera");
            }
        }

        // オーディオ（AudioSource）は上部の専用ヒーローカード（RenderAudioHero）で編集する。

        // --- Physics ---
        {
            bool hasRb = reg.all_of<RigidBody>(ctx.selectedEntity);
            if (ImGui::Checkbox("Physics", &hasRb))
            {
                if (hasRb)
                {
                    // Add physics: auto collider + RigidBody
                    if (reg.all_of<MeshRenderer>(ctx.selectedEntity))
                    {
                        auto* mr = &reg.get<MeshRenderer>(ctx.selectedEntity);
                        auto* tf = &reg.get<Transform>(ctx.selectedEntity);
                        std::vector<DirectX::XMFLOAT3> allPoints;
                        for (const auto* mesh : mr->meshes)
                        {
                            if (!mesh) continue;
                            for (const auto& p : mesh->GetPositions())
                                allPoints.push_back({
                                    p.x * tf->scale.x,
                                    p.y * tf->scale.y,
                                    p.z * tf->scale.z });
                        }
                        constexpr size_t kMax = 256;
                        if (allPoints.size() > kMax)
                        {
                            size_t step = allPoints.size() / kMax;
                            std::vector<DirectX::XMFLOAT3> sampled;
                            for (size_t i = 0; i < allPoints.size() && sampled.size() < kMax; i += step)
                                sampled.push_back(allPoints[i]);
                            allPoints = std::move(sampled);
                        }
                        if (!allPoints.empty())
                        {
                            ConvexHullCollider col;
                            col.points = std::move(allPoints);
                            reg.emplace_or_replace<ConvexHullCollider>(ctx.selectedEntity, col);
                            ctx.undoSystem.PushCommand(std::make_unique<AddComponentCommand<ConvexHullCollider>>(
                                &reg, ctx.selectedEntity, std::move(col), "Convex Hull Collider"));
                        }
                    }
                    reg.emplace_or_replace<RigidBody>(ctx.selectedEntity);
                    ctx.undoSystem.PushCommand(std::make_unique<AddComponentCommand<RigidBody>>(
                        &reg, ctx.selectedEntity, reg.get<RigidBody>(ctx.selectedEntity), "RigidBody"));
                }
                else
                {
                    auto pushRemove = [&]<typename T>(const char* name)
                    {
                        if (reg.all_of<T>(ctx.selectedEntity))
                        {
                            ctx.undoSystem.PushCommand(std::make_unique<RemoveComponentCommand<T>>(
                                &reg, ctx.selectedEntity, reg.get<T>(ctx.selectedEntity), name));
                            reg.remove<T>(ctx.selectedEntity);
                        }
                    };
                    pushRemove.template operator()<RigidBody>("RigidBody");
                    pushRemove.template operator()<ConvexHullCollider>("Convex Hull Collider");
                    pushRemove.template operator()<BoxCollider>("Box Collider");
                    pushRemove.template operator()<SphereCollider>("Sphere Collider");
                    pushRemove.template operator()<CapsuleCollider>("Capsule Collider");
                }
            }

            if (reg.all_of<RigidBody>(ctx.selectedEntity))
            {
                BeginEdit(reg, ctx.selectedEntity, m_rbEdit);
                auto& rb = reg.get<RigidBody>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("RigidBody"))
                {
                    const char* motionTypes[] = { "Static", "Kinematic", "Dynamic" };
                    int motionIdx = static_cast<int>(rb.motionType);
                    if (pg::Combo("挙動 Motion", &motionIdx, motionTypes, 3))
                    {
                        rb.motionType = static_cast<MotionType>(motionIdx);
                        changed = true;
                    }
                    changed |= pg::Float("質量 Mass", &rb.mass, 0.5f, 0.0f, 10000.0f, "%.1f", &active);
                    changed |= pg::Float("摩擦 Friction", &rb.friction, 0.01f, 0.0f, 2.0f, "%.2f", &active);
                    changed |= pg::Float("反発 Bounce", &rb.restitution, 0.01f, 0.0f, 1.0f, "%.2f", &active);
                    changed |= pg::Checkbox("重力 Gravity", &rb.useGravity);
                    changed |= pg::Checkbox("すり抜け防止 CCD", &rb.continuousCollision);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("弾丸や投擲物のように 1 フレームで自分の厚みより長く動く物だけ ON。\n"
                                          "OFF だと薄い壁をすり抜ける。ON はコストが高いので普通の物には不要。");
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_rbEdit, changed, active, "RigidBody");
            }
        }

        // --- Colliders ---
        if (reg.all_of<BoxCollider>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entCollider : 0, "Box Collider");
            bool removed = ComponentRemoveMenu<BoxCollider>(reg, ctx, ctx.selectedEntity, "Box Collider");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_boxColEdit);
                auto& col = reg.get<BoxCollider>(ctx.selectedEntity);
                bool changed = false, active = false;
                if (pg::Begin("BoxCollider"))
                {
                    changed |= pg::Float3("半径 Half Extents", &col.halfExtents.x, 0.05f, 0.01f, 1000.0f, "%.2f", &active);
                    changed |= pg::Float3("オフセット Offset", &col.offset.x, 0.05f, 0, 0, "%.2f", &active);
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_boxColEdit, changed, active, "Box Collider");
            }
        }

        if (reg.all_of<SphereCollider>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entCollider : 0, "Sphere Collider");
            bool removed = ComponentRemoveMenu<SphereCollider>(reg, ctx, ctx.selectedEntity, "Sphere Collider");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_sphereColEdit);
                auto& col = reg.get<SphereCollider>(ctx.selectedEntity);
                bool changed = false, active = false;
                if (pg::Begin("SphereCollider"))
                {
                    changed |= pg::Float("半径 Radius", &col.radius, 0.05f, 0.01f, 1000.0f, "%.2f", &active);
                    changed |= pg::Float3("オフセット Offset", &col.offset.x, 0.05f, 0, 0, "%.2f", &active);
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_sphereColEdit, changed, active, "Sphere Collider");
            }
        }

        if (reg.all_of<CapsuleCollider>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entCollider : 0, "Capsule Collider");
            bool removed = ComponentRemoveMenu<CapsuleCollider>(reg, ctx, ctx.selectedEntity, "Capsule Collider");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_capsuleColEdit);
                auto& col = reg.get<CapsuleCollider>(ctx.selectedEntity);
                bool changed = false, active = false;
                if (pg::Begin("CapsuleCollider"))
                {
                    changed |= pg::Float("半径 Radius", &col.radius, 0.05f, 0.01f, 1000.0f, "%.2f", &active);
                    changed |= pg::Float("半分高さ Half Height", &col.halfHeight, 0.05f, 0.01f, 1000.0f, "%.2f", &active);
                    changed |= pg::Float3("オフセット Offset", &col.offset.x, 0.05f, 0, 0, "%.2f", &active);
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_capsuleColEdit, changed, active, "Capsule Collider");
            }
        }

        if (reg.all_of<CharacterController>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entPhysics : 0, "Character Controller");
            bool removed = ComponentRemoveMenu<CharacterController>(reg, ctx, ctx.selectedEntity, "Character Controller");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_ccEdit);
                auto& cc = reg.get<CharacterController>(ctx.selectedEntity);
                bool changed = false, active = false;
                if (pg::Begin("CharacterController"))
                {
                    changed |= pg::Float("半径 Radius",       &cc.radius,       0.02f, 0.05f, 5.0f,    "%.2f", &active);
                    changed |= pg::Float("半分高さ Half Height", &cc.halfHeight, 0.02f, 0.05f, 5.0f,    "%.2f", &active);
                    changed |= pg::Float3("オフセット Offset", &cc.offset.x,     0.02f, 0, 0,           "%.2f", &active);
                    changed |= pg::Float("質量 Mass",         &cc.mass,         1.0f,  1.0f, 1000.0f,  "%.0f", &active);
                    changed |= pg::Float("最大斜度 (deg)",     &cc.maxSlopeDeg,  0.5f,  0.0f, 89.0f,    "%.1f", &active);
                    changed |= pg::Float("段差高さ Step",      &cc.stepHeight,   0.01f, 0.0f, 2.0f,     "%.2f", &active);
                    changed |= pg::Float("ジャンプ速度",       &cc.jumpSpeed,    0.1f,  0.0f, 50.0f,    "%.1f", &active);
                    changed |= pg::Float("重力スケール",       &cc.gravityScale, 0.05f, 0.0f, 5.0f,     "%.2f", &active);
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_ccEdit, changed, active, "Character Controller");
            }
        }

        if (reg.all_of<ConvexHullCollider>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entCollider : 0, "Convex Hull Collider");
            bool removed = ComponentRemoveMenu<ConvexHullCollider>(reg, ctx, ctx.selectedEntity, "Convex Hull Collider");
            if (open && !removed)
            {
                const auto& col = reg.get<ConvexHullCollider>(ctx.selectedEntity);
                if (pg::Begin("ConvexHull"))
                {
                    pg::Text("頂点数 Points", "%d", static_cast<int>(col.points.size()));
                    pg::End();
                }
                ImGui::TextDisabled("(メッシュから自動生成)");
            }
        }

        // --- Material (PBR) ---
        if (reg.all_of<MeshRenderer>(ctx.selectedEntity))
        {
            auto& mr = reg.get<MeshRenderer>(ctx.selectedEntity);
            if (!mr.meshes.empty() && mr.meshes[0] && mr.meshes[0]->GetMaterial())
            {
                const auto* mat = mr.meshes[0]->GetMaterial();
                if (IconHeader(ic, ic ? ic->entMesh : 0, "Material"))
                {
                    // ★ここは以前「ヘッダを開いた瞬間に -1(継承) を mat の既定値で潰す」
                    //   だった。描画側は override >= 0 を materialAsset より優先するので
                    //   （ApplicationRender.cpp の matAsset 分岐）、.dxmat を割り当てた
                    //   メッシュでも **Material 節を一度展開しただけで .dxmat の
                    //   metallic/roughness が恒久的に無視される**。しかも保存されるので戻らない
                    //   （-1 を書き戻す UI はどこにも無かった）。
                    //   表示用のローカルへ入れ、実際に動かされたときだけ override を書く。
                    const bool inheritMetal = (mr.overrideMetallic  < 0.0f);
                    const bool inheritRough = (mr.overrideRoughness < 0.0f);
                    float uiMetal = inheritMetal ? mat->defaultMetallic  : mr.overrideMetallic;
                    float uiRough = inheritRough ? mat->defaultRoughness : mr.overrideRoughness;

                    // PBR 編集前スナップショット（metallic / roughness / 透明をひとまとめ）
                    if (!m_pbrEditing)
                        m_pbrSnapshot = PbrOverrides::From(mr);

                    // 透明の実効値（モデル焼き込み × エンティティ上書き）。
                    // ★合成規則は renderer/Material.h の ResolveAlphaParams が正。ここで
                    //   別の規則を書くと「インスペクタの表示と実際の絵が食い違う」になる。
                    const AlphaParams effAlpha = ResolveAlphaParams(mat, mr.alphaModeOverride,
                                                                    mr.alphaCutoffOverride, mr.opacity);
                    int   alphaIdx  = mr.alphaModeOverride + 1;   // -1..2 → 0..3（0 = 継承）
                    float uiCutoff  = effAlpha.cutoff;
                    float uiOpacity = mr.opacity;

                    bool cutoffActive = false, opacityActive = false;
                    bool metalActive = false, roughActive = false;
                    bool hasNormal = mat->normalMapTexture != nullptr;
                    bool hasMR2 = mat->metalRoughnessTexture != nullptr;
                    if (pg::Begin("MaterialPBR"))
                    {
                        if (pg::SliderFloat("金属感 Metallic", &uiMetal, 0.0f, 1.0f, "%.3f", &metalActive))
                            mr.overrideMetallic = uiMetal;   // 動かしたときだけ上書きにする
                        if (pg::SliderFloat("粗さ Roughness", &uiRough, 0.0f, 1.0f, "%.3f", &roughActive))
                            mr.overrideRoughness = uiRough;
                        pg::Text("Normal Map", "%s", hasNormal ? "あり" : "なし");
                        pg::Text("MetalRough Map", "%s", hasMR2 ? "あり" : "なし");

                        // ---- 透明（アルファクリップ / アルファブレンド）----
                        // 既定は「継承」＝モデルの glTF alphaMode に従う。ここを触ると
                        // エンティティ単位の上書きになり、シーン JSON の material ブロックに載る。
                        pg::Group("透明 Transparency");
                        static const char* const kAlphaModes[4] = {
                            "継承 (モデルに従う)", "不透明 Opaque",
                            "アルファクリップ Mask", "半透明 Blend" };
                        if (pg::Combo("扱い Alpha Mode", &alphaIdx, kAlphaModes, 4,
                                      "継承 = glTF の alphaMode。Mask は葉・フェンス・角膜の抜き"
                                      "（影も同じ形に抜ける）。Blend は不透明の後に奥から順に描く"))
                        {
                            // コンボは押した瞬間に確定する＝下のドラッグ終了検出には拾われない
                            const PbrOverrides before = PbrOverrides::From(mr);
                            mr.alphaModeOverride = alphaIdx - 1;
                            ctx.undoSystem.PushCommand(std::make_unique<PBRCommand>(
                                &reg, ctx.selectedEntity, before, PbrOverrides::From(mr)));
                        }
                        if (effAlpha.mode == AlphaMode::Mask)
                        {
                            if (pg::SliderFloat("しきい値 Cutoff", &uiCutoff, 0.0f, 1.0f, "%.2f", &cutoffActive))
                                mr.alphaCutoffOverride = uiCutoff;
                        }
                        if (pg::SliderFloat("不透明度 Opacity", &uiOpacity, 0.0f, 1.0f, "%.2f", &opacityActive))
                            mr.opacity = uiOpacity;
                        if (effAlpha.mode == AlphaMode::Blend)
                            WarnRow("半透明は深度を書かず影も落としません"
                                    "（エンティティ単位で奥から手前へ描画）");

                        if (inheritMetal || inheritRough)
                            WarnRow("継承中（この値はモデル/マテリアル側の既定。動かすと上書きになります）");
                        if (!inheritMetal || !inheritRough || mr.alphaModeOverride >= 0
                            || mr.alphaCutoffOverride >= 0.0f || mr.opacity != 1.0f)
                        {
                            pg::Label("");
                            if (ImGui::SmallButton("継承に戻す"))
                            {
                                // ★Undo に積む。m_pbrEditing はスライダー操作でしか立たないので、
                                //   このボタンだけは下のドラッグ終了検出に拾われず、
                                //   Undo にも未保存フラグにも一切残らなかった
                                //   （overrideMetallic/Roughness は material ブロックの出力を左右する）。
                                // ★透明の上書きもここで一緒に落とす（節の中の全上書きが対象）。
                                const PbrOverrides before = PbrOverrides::From(mr);
                                PbrOverrides{}.ApplyTo(mr);   // 既定 = 全部「継承」/ opacity 1
                                if (!(before == PbrOverrides::From(mr)))
                                {
                                    ctx.undoSystem.PushCommand(std::make_unique<PBRCommand>(
                                        &reg, ctx.selectedEntity, before, PbrOverrides::From(mr)));
                                }
                            }
                        }
                        pg::End();
                    }

                    const bool pbrActive = metalActive || roughActive || cutoffActive || opacityActive;
                    if (pbrActive)
                        m_pbrEditing = true;

                    if (m_pbrEditing && !pbrActive && !ImGui::IsAnyItemActive())
                    {
                        if (!(m_pbrSnapshot == PbrOverrides::From(mr)))
                        {
                            ctx.undoSystem.PushCommand(std::make_unique<PBRCommand>(
                                &reg, ctx.selectedEntity, m_pbrSnapshot, PbrOverrides::From(mr)));
                        }
                        m_pbrEditing = false;
                    }

                    // テクスチャ上書き（アセットブラウザからテクスチャをドラッグ&ドロップして割当。
                    // Unity/Unreal 風）。サブメッシュ単位。Material 自体は同一モデルパスの全インスタンスで
                    // 共有されているため直接書き換えず、MeshRenderer にインスタンス単位で保持する
                    // (描画側 Application::EnsureMaterialOverrideSrv が専用SRVブロックを合成する)。
                    ImGui::Separator();
                    ImGui::TextDisabled("\xe3\x83\x86\xe3\x82\xaf\xe3\x82\xb9\xe3\x83\x81\xe3\x83\xa3\xe4\xb8\x8a\xe6\x9b\xb8\xe3\x81\x8d"
                        "(\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x96\xe3\x83\xa9\xe3\x82\xa6\xe3\x82\xb6\xe3\x81\x8b\xe3\x82\x89 D&D)");

                    // 割当/解除を1箇所にまとめる(ドラッグ&ドロップ・ピッカー・xボタンの全経路から呼ぶ)。
                    auto applyOverride = [&](std::vector<std::string>& slotVec, u32 smi, const std::string& rel)
                    {
                        MeshRenderer before = mr;
                        MeshRenderer::SetOverride(slotVec, smi, rel);
                        ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<MeshRenderer>>(
                            &reg, ctx.selectedEntity, before, mr, "Material Texture"));
                    };

                    constexpr float kThumbSize = 64.0f;

                    auto drawTextureOverrideSlot = [&](const char* label, std::vector<std::string>& slotVec, u32 smi)
                    {
                        ImGui::PushID(label);
                        ImGui::PushID(static_cast<int>(smi));

                        const std::string& cur = MeshRenderer::SafeGetOverride(slotVec, smi);
                        bool hasTex = !cur.empty();

                        pg::Label(label);

                        // サムネイル(実テクスチャ画像)。AssetBrowserPanel と同じキャッシュを使い回す
                        // (GetOrQueueThumbnail、無ければロードキューに積んで0=読込中を返す)。
                        u64 gpuHandle = 0;
                        if (hasTex && m_assetBrowser)
                            gpuHandle = m_assetBrowser->GetOrQueueThumbnail(m_assetsDir + cur);

                        bool clicked;
                        if (gpuHandle != 0)
                        {
                            clicked = ImGui::ImageButton("##slotThumb", static_cast<ImTextureID>(gpuHandle),
                                                          ImVec2(kThumbSize, kThumbSize));
                        }
                        else
                        {
                            clicked = ImGui::Button(hasTex ? "..." : "(default)", ImVec2(kThumbSize * 3.0f, kThumbSize));
                        }
                        if (clicked)
                            ImGui::OpenPopup("TexturePicker");

                        // ドラッグ&ドロップ(アセットブラウザから直接)
                        if (ImGui::BeginDragDropTarget())
                        {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
                            {
                                const char* droppedPath = static_cast<const char*>(payload->Data);
                                namespace fs = std::filesystem;
                                std::string ext = fs::path(droppedPath).extension().string();
                                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                                if (AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Texture)
                                {
                                    std::string abs = fs::path(droppedPath).lexically_normal().string();
                                    std::string base = fs::path(m_assetsDir).lexically_normal().string();
                                    std::replace(abs.begin(), abs.end(), '\\', '/');
                                    std::replace(base.begin(), base.end(), '\\', '/');
                                    std::string rel = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;
                                    applyOverride(slotVec, smi, rel);
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }

                        if (hasTex)
                        {
                            ImGui::SameLine();
                            ImGui::BeginGroup();
                            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 160.0f);
                            ImGui::TextWrapped("%s", cur.c_str());
                            ImGui::PopTextWrapPos();
                            if (ImGui::SmallButton("x"))
                                applyOverride(slotVec, smi, "");
                            ImGui::EndGroup();
                        }

                        // クリックで開く一覧ダイアログ(Unity風: プロジェクト内の全テクスチャから選択)
                        if (ImGui::BeginPopup("TexturePicker"))
                        {
                            ImGui::TextDisabled("Select Texture");
                            ImGui::Separator();
                            if (ImGui::Selectable("(None)", !hasTex))
                            {
                                applyOverride(slotVec, smi, "");
                                ImGui::CloseCurrentPopup();
                            }

                            namespace fs = std::filesystem;
                            std::error_code ec;
                            fs::path root(m_assetsDir);
                            if (fs::exists(root, ec))
                            {
                                fs::recursive_directory_iterator dirIt(
                                    root, fs::directory_options::skip_permission_denied, ec);
                                fs::recursive_directory_iterator dirEnd;
                                for (; !ec && dirIt != dirEnd; dirIt.increment(ec))
                                {
                                    std::error_code fec;
                                    if (!dirIt->is_regular_file(fec) || fec) continue;
                                    std::string rowExt = dirIt->path().extension().string();
                                    for (char& c : rowExt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                    if (AssetBrowserPanel::ClassifyExtension(rowExt) != AssetBrowserPanel::AssetType::Texture)
                                        continue;

                                    fs::path relPath = fs::relative(dirIt->path(), root, fec);
                                    if (fec) continue;
                                    std::string relStr = relPath.generic_string();

                                    ImGui::PushID(relStr.c_str());
                                    u64 rowThumb = m_assetBrowser
                                        ? m_assetBrowser->GetOrQueueThumbnail(dirIt->path().string()) : 0;
                                    if (rowThumb != 0)
                                    {
                                        ImGui::Image(static_cast<ImTextureID>(rowThumb), ImVec2(20.0f, 20.0f));
                                        ImGui::SameLine();
                                    }
                                    if (ImGui::Selectable(relStr.c_str(), cur == relStr))
                                    {
                                        applyOverride(slotVec, smi, relStr);
                                        ImGui::CloseCurrentPopup();
                                    }
                                    ImGui::PopID();
                                }
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::PopID();
                        ImGui::PopID();
                    };

                    // マテリアルアセット(assets/materials/*.dxmat、Unrealのマテリアルインスタンス相当)。
                    // 割当があれば上記3スロットのテクスチャ個別上書きより優先される(描画側 Application 参照)。
                    auto drawMaterialAssetSlot = [&](u32 smi)
                    {
                        ImGui::PushID("MaterialAsset");
                        ImGui::PushID(static_cast<int>(smi));

                        const std::string& cur = MeshRenderer::SafeGetOverride(mr.materialAsset, smi);
                        bool hasMat = !cur.empty();

                        pg::Label("Material Asset");

                        // サムネイルは .dxmat の albedo テクスチャを軽量パースして使い回す
                        // (AssetBrowserPanel::Refresh と同じ方式。JSON数百バイトなので毎フレーム読んでも軽い)。
                        u64 gpuHandle = 0;
                        if (hasMat && m_assetBrowser)
                        {
                            std::ifstream ifs(m_assetsDir + cur, std::ios::binary);
                            if (ifs)
                            {
                                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                                MaterialAssetData data;
                                if (ParseMaterialAsset(bytes, data) && !data.albedoPath.empty())
                                    gpuHandle = m_assetBrowser->GetOrQueueThumbnail(m_assetsDir + data.albedoPath);
                            }
                        }

                        bool clicked;
                        if (gpuHandle != 0)
                        {
                            clicked = ImGui::ImageButton("##matThumb", static_cast<ImTextureID>(gpuHandle),
                                                          ImVec2(kThumbSize, kThumbSize));
                        }
                        else
                        {
                            clicked = ImGui::Button(hasMat ? "..." : "(none)", ImVec2(kThumbSize * 3.0f, kThumbSize));
                        }
                        if (clicked)
                            ImGui::OpenPopup("MaterialPicker");

                        // ドラッグ&ドロップ(アセットブラウザから .dxmat のみ受理)
                        if (ImGui::BeginDragDropTarget())
                        {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
                            {
                                const char* droppedPath = static_cast<const char*>(payload->Data);
                                namespace fs = std::filesystem;
                                std::string ext = fs::path(droppedPath).extension().string();
                                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                                if (AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Material)
                                {
                                    std::string abs = fs::path(droppedPath).lexically_normal().string();
                                    std::string base = fs::path(m_assetsDir).lexically_normal().string();
                                    std::replace(abs.begin(), abs.end(), '\\', '/');
                                    std::replace(base.begin(), base.end(), '\\', '/');
                                    std::string rel = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;
                                    applyOverride(mr.materialAsset, smi, rel);

                                    // 割当時: UVタイリングが既定値(1.0)のままなら.dxmatの値を初期値としてコピーする
                                    // (描画時に合成はしない。Mesh::ApplyUVScaleは頂点焼き込みのため)。
                                    if (mr.uvScaleU == 1.0f && mr.uvScaleV == 1.0f)
                                    {
                                        std::ifstream ifs2(m_assetsDir + rel, std::ios::binary);
                                        if (ifs2)
                                        {
                                            std::vector<uint8_t> bytes2((std::istreambuf_iterator<char>(ifs2)),
                                                                         std::istreambuf_iterator<char>());
                                            MaterialAssetData data2;
                                            if (ParseMaterialAsset(bytes2, data2) && mr.meshes[smi])
                                            {
                                                mr.uvScaleU = data2.uvTilingU;
                                                mr.uvScaleV = data2.uvTilingV;
                                                if (scene)
                                                    for (auto* mesh : mr.meshes)
                                                        if (mesh) mesh->ApplyUVScale(*scene->GetDevice(), mr.uvScaleU, mr.uvScaleV);
                                            }
                                        }
                                    }
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }

                        if (hasMat)
                        {
                            ImGui::SameLine();
                            ImGui::BeginGroup();
                            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 160.0f);
                            ImGui::TextWrapped("%s", cur.c_str());
                            ImGui::PopTextWrapPos();
                            if (ImGui::SmallButton("x"))
                                applyOverride(mr.materialAsset, smi, "");
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Edit"))
                            {
                                ctx.pendingOpenMaterialPath = m_assetsDir + cur;
                                ctx.showMaterialEditor = true;
                            }
                            ImGui::EndGroup();
                        }

                        // クリックで開く一覧ダイアログ(TexturePickerの.dxmat版)
                        if (ImGui::BeginPopup("MaterialPicker"))
                        {
                            ImGui::TextDisabled("Select Material");
                            ImGui::Separator();
                            if (ImGui::Selectable("(None)", !hasMat))
                            {
                                applyOverride(mr.materialAsset, smi, "");
                                ImGui::CloseCurrentPopup();
                            }

                            namespace fs = std::filesystem;
                            std::error_code ec;
                            fs::path root(m_assetsDir);
                            if (fs::exists(root, ec))
                            {
                                fs::recursive_directory_iterator dirIt(
                                    root, fs::directory_options::skip_permission_denied, ec);
                                fs::recursive_directory_iterator dirEnd;
                                for (; !ec && dirIt != dirEnd; dirIt.increment(ec))
                                {
                                    std::error_code fec;
                                    if (!dirIt->is_regular_file(fec) || fec) continue;
                                    std::string rowExt = dirIt->path().extension().string();
                                    for (char& c : rowExt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                    if (AssetBrowserPanel::ClassifyExtension(rowExt) != AssetBrowserPanel::AssetType::Material)
                                        continue;

                                    fs::path relPath = fs::relative(dirIt->path(), root, fec);
                                    if (fec) continue;
                                    std::string relStr = relPath.generic_string();

                                    if (ImGui::Selectable(relStr.c_str(), cur == relStr))
                                    {
                                        applyOverride(mr.materialAsset, smi, relStr);
                                        ImGui::CloseCurrentPopup();
                                    }
                                }
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::PopID();
                        ImGui::PopID();
                    };

                    if (pg::Begin("MaterialTex"))
                    {
                        for (u32 smi = 0; smi < static_cast<u32>(mr.meshes.size()); ++smi)
                        {
                            if (!mr.meshes[smi]) continue;
                            if (mr.meshes.size() > 1)
                            {
                                char sub[32];
                                snprintf(sub, sizeof(sub), "Submesh %u", smi);
                                pg::Group(sub);
                            }
                            drawMaterialAssetSlot(smi);

                            bool matAssigned = mr.HasMaterialAsset(smi);
                            if (matAssigned)
                            {
                                ImGui::BeginDisabled();
                                ImGui::TextDisabled("\xe3\x83\x9e\xe3\x83\x86\xe3\x83\xaa\xe3\x82\xa2\xe3\x83\xab\xe5\x89\xb2\xe5\xbd\x93\xe4\xb8\xad\xe3\x81\xaf\xe7\x84\xa1\xe5\x8a\xb9"); // マテリアル割当中は無効
                            }
                            drawTextureOverrideSlot("Albedo", mr.overrideAlbedoTexture, smi);
                            drawTextureOverrideSlot("Normal", mr.overrideNormalTexture, smi);
                            drawTextureOverrideSlot("MetalRoughness", mr.overrideMetalRoughnessTexture, smi);
                            if (matAssigned)
                                ImGui::EndDisabled();
                        }
                        pg::End();
                    }
                }
            }

            // UV タイリング / UVスクロール / 連番アニメ
            // ★UV & Anim とシェーダーの 2 節は長らく Undo に積まれていなかった。
            //   普通の ComponentEditCommand では戻せない（uvScale は頂点へ焼くので
            //   値だけ戻すと「値は新しいのに絵は古い」になる）ため、
            //   焼き直しまで面倒を見る MeshRendererLookCommand を使う。
            //   節をまたぐので、スナップショットは両方の外側で取る。
            // ★毎フレーム取り直してはいけない。ドラッグ中は push を抑止するので、
            //   離したフレームには「今フレームの値」と一致してしまい差分が消える
            //   ＝ドラッグ編集が一度も積まれない。選択が変わったときだけ取り直す。
            if (m_lookEntity != ctx.selectedEntity)
            {
                m_lookEntity   = ctx.selectedEntity;
                m_lookSnapshot = MeshRendererLook::From(mr);
            }
            const MeshRendererLook& lookBefore = m_lookSnapshot;

            if (IconHeader(nullptr, 0, "UV & Anim"))
            {
                bool uvChanged = false;
                if (pg::Begin("UVTiling"))
                {
                    uvChanged |= pg::Float("U Scale", &mr.uvScaleU, 0.1f, 0.01f, 100.0f, "%.2f");
                    uvChanged |= pg::Float("V Scale", &mr.uvScaleV, 0.1f, 0.01f, 100.0f, "%.2f");
                    pg::End();
                }
                // U,V を連動させるボタン
                if (ImGui::Button("U=V"))
                {
                    mr.uvScaleV = mr.uvScaleU;
                    uvChanged = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset 1x"))
                {
                    mr.uvScaleU = 1.0f;
                    mr.uvScaleV = 1.0f;
                    uvChanged = true;
                }
                if (uvChanged)
                {
                    // 全メッシュに UV スケールを適用
                    for (auto* mesh : mr.meshes)
                    {
                        if (mesh)
                            mesh->ApplyUVScale(*scene->GetDevice(), mr.uvScaleU, mr.uvScaleV);
                    }
                }

                // UVスクロール（頂点は触らずシェーダー側で流す。滝/溶岩/コンベア）と
                // 連番アニメ（有効中はタイリング/スクロールより優先）
                if (pg::Begin("UVAnim"))
                {
                    pg::Group("アニメ");
                    pg::Float("UVスクロールU uvScrollU", &mr.uvScrollU, 0.01f, -100.0f, 100.0f,
                              "%.2f", nullptr,
                              "uv/秒。頂点は触らないので VB 再生成なし（タイリングと併用可）");
                    pg::Float("UVスクロールV uvScrollV", &mr.uvScrollV, 0.01f, -100.0f, 100.0f,
                              "%.2f");
                    pg::Int("連番フレーム数 animFrames", &mr.animFrames, 1.0f, 0, 1024, nullptr,
                            "0=なし。1以上でアルベドをコマ送り再生（炎/水しぶき/アニメする看板）。\n"
                            "有効中は UV タイリング/スクロールより優先される");
                    if (mr.animFrames > 0)
                    {
                        pg::Float("速度 animFps", &mr.animFps, 0.1f, 0.0f, 120.0f, "%.1f");
                        pg::Int("列数 animCols", &mr.animCols, 1.0f, 0, 1024, nullptr,
                                "0=animFrames と同じ（横1行ストリップ）");
                        pg::Int("開始行 animRow", &mr.animRow, 1.0f, 0, 1024);
                        pg::Int("総行数 animRows", &mr.animRows, 1.0f, 0, 1024, nullptr,
                                "0=自動（開始行 + ceil(animFrames/animCols)）");
                        static const char* meshAnimModes[] = {"ループ", "単発(最後で停止)", "往復(ピンポン)"};
                        pg::Combo("再生モード animMode", &mr.animMode, meshAnimModes,
                                  IM_ARRAYSIZE(meshAnimModes),
                                  "単発=1回きりの演出。往復=0→末尾→0 の呼吸アニメ");
                    }
                    pg::End();
                }
            }

            // カスタムシェーダー割当（静的メッシュのみ有効。スキンド/インスタンシングは既定へフォールバック）。
            // 一覧はプロジェクト assets/shaders/ 配下の .hlsl のうち、Registry(エンジン組み込み)と
            // 一致しないもの＝自作シェーダーだけ(一致するものは全体に効く「上書き」用途なので個別割当から除外)。
            // コンソールのエラー行から飛んできたら、この節を開いてそこまでスクロールする
            const bool revealMesh = RevealMatches(mr.shaderPath);
            if (revealMesh) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            const bool meshShaderOpen = IconHeader(nullptr, 0, "Shader");
            if (revealMesh) ImGui::SetScrollHereY(0.15f);
            if (meshShaderOpen)
            {
                namespace fs = std::filesystem;
                std::string currentLabel = mr.shaderPath.empty() ? "\xe6\x97\xa2\xe5\xae\x9a (Forward)" : mr.shaderPath;
                bool shaderGrid = pg::Begin("MeshShader");
                if (shaderGrid)
                    pg::Label("\xe3\x82\xb7\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\x80\xe3\x83\xbc");  // シェーダー
                if (ImGui::BeginCombo("##meshShader", currentLabel.c_str()))
                {
                    std::vector<std::string> options;
                    std::error_code ec;
                    fs::path root(m_assetsDir + "shaders/");
                    if (fs::exists(root, ec))
                    {
                        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
                        fs::recursive_directory_iterator end;
                        for (; !ec && it != end; it.increment(ec))
                        {
                            std::error_code fec;
                            if (!it->is_regular_file(fec) || fec) continue;
                            if (it->path().extension() != L".hlsl") continue;
                            fs::path rel = fs::relative(it->path(), root, fec);
                            if (fec) continue;
                            std::string relStr = rel.generic_string();
                            if (FindShaderSourceByRelPath(relStr) != nullptr)
                                continue;  // Registry一致=上書き用途。個別割当の選択肢からは除外
                            options.push_back(relStr);
                        }
                    }

                    if (ImGui::Selectable("\xe6\x97\xa2\xe5\xae\x9a (Forward)", mr.shaderPath.empty()))
                        mr.shaderPath.clear();
                    for (const auto& opt : options)
                    {
                        if (ImGui::Selectable(opt.c_str(), mr.shaderPath == opt))
                            mr.shaderPath = opt;
                    }
                    if (options.empty())
                        ImGui::TextDisabled("(assets/shaders/ \xe3\x81\xab\xe8\x87\xaa\xe4\xbd\x9c\xe3\x82\xb7\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\x80\xe3\x83\xbc\xe3\x81\xaa\xe3\x81\x97)");
                    ImGui::EndCombo();
                }
                // ★アセットブラウザから .hlsl を直接ドロップして割り当てられる
                //   （コンボを開いて探さなくてよい。落とせる先は Mesh / Sprite2D / Camera）。
                {
                    std::string dropped;
                    if (assetdrop::AcceptShader(dropped, m_assetsDir + "shaders/"))
                        mr.shaderPath = dropped;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(".hlsl \xe3\x82\x92\xe3\x81\x93\xe3\x81\x93\xe3\x81\xb8\xe3\x83\x89\xe3\x83\xad\xe3\x83\x83\xe3\x83\x97\xe3\x81\xa7\xe3\x82\x82\xe5\x89\xb2\xe3\x82\x8a\xe5\xbd\x93\xe3\x81\xa6\xe3\x82\x89\xe3\x82\x8c\xe3\x81\xbe\xe3\x81\x99");
                // カスタムシェーダー割当時のみ意味を持つ: PSが書く float4 の alpha を実際に
                // ブレンドに使うかどうか。既定 OFF(不透明、DepthWrite=ON)のままだと
                // シェーダー側でどれだけ alpha を作り込んでも画面には反映されない。
                if (shaderGrid)
                {
                    if (!mr.shaderPath.empty())
                    {
                        pg::Checkbox("アルファブレンド有効", &mr.shaderAlphaBlend,
                            "ON: シェーダーの alpha 出力を SrcAlpha/InvSrcAlpha でブレンド(DepthWrite OFF)。"
                            "OFF(既定): 不透明固定で alpha は無視される");
                        // ★HLSL に書いた名前のままパラメーターを並べる。cbuffer b0 の自由枠に
                        //   `float _Glow;` と足して保存すれば、ホットリロードでこの欄に「_Glow」が生える。
                        //   リフレクションが取れないとき(未コンパイル / 配布ビルドで .cso しか無い)は
                        //   従来どおりの汎用 2 行へ落ちるので、既存シェーダーの操作感は変わらない。
                        if (!DrawNamedShaderParams(mr.shaderPath, mr.CustomParamBase()))
                        {
                            pg::Float("エフェクト値 effectValue", &mr.effectValue, 0.005f, 0.0f, 1.0f, "%.3f", nullptr,
                                "シェーダーへ渡す汎用の進捗/強度値(意味はシェーダー依存)。"
                                "Luaの scene:setMeshEffect(e, value) で実行時にも変更可");
                            pg::Float4("パラメーター shaderParams", &mr.shaderParams.x, 0.01f, 0.0f, 0.0f, "%.3f", nullptr,
                                "シェーダーへ渡す汎用パラメーター4つ(意味はシェーダー依存)。HLSL側は cbuffer の "
                                "effectValue の後ろに float4 shaderParams; を足して読む。"
                                "Luaの scene:setMeshParams(e, x,y,z,w) でも変更可");
                        }
                    }
                    pg::End();
                }
                ShaderIssueBox(mr.shaderPath, dx12e::shaderdiag::kIdMesh, "MeshRenderer");
                if (reg.all_of<SkeletalAnimation>(ctx.selectedEntity) && !mr.shaderPath.empty())
                    WarnText("スキンドメッシュは既定シェーダーへフォールバック");
            }

            // 上の 2 節で見た目まわりが変わっていたら 1 エントリだけ積む。
            // ドラッグ中（IsAnyItemActive）は積まない＝離した時にまとめて 1 回。
            {
                const MeshRendererLook lookAfter = MeshRendererLook::From(mr);
                if (!(lookBefore == lookAfter) && !ImGui::IsAnyItemActive())
                {
                    ctx.undoSystem.PushCommand(std::make_unique<MeshRendererLookCommand>(
                        scene, &reg, ctx.selectedEntity, lookBefore, lookAfter));
                    m_lookSnapshot = lookAfter;   // 次の編集の基準を更新（同じ差分で積み続けない）
                }
            }
        }

        // LuaScript
        DrawLuaScriptSection(reg, ctx.selectedEntity, ctx, m_scriptEngine, m_assetsDir);

        // --- Add Component ---
        ImGui::Separator();
        // ✚ コンポーネント追加
        if (ImGui::Button("\xe2\x9c\x9a \xe3\x82\xb3\xe3\x83\xb3\xe3\x83\x9d\xe3\x83\xbc\xe3\x83\x8d\xe3\x83\xb3\xe3\x83\x88\xe8\xbf\xbd\xe5\x8a\xa0", ImVec2(-1, 0)))
            ImGui::OpenPopup("AddComponentPopup");

        if (ImGui::BeginPopup("AddComponentPopup"))
        {
            AddComponentMenuItem<PointLight>(reg, ctx, ctx.selectedEntity, "Point Light");
            AddComponentMenuItem<DirectionalLight>(reg, ctx, ctx.selectedEntity, "Directional Light");
            AddComponentMenuItem<SpotLight>(reg, ctx, ctx.selectedEntity, "Spot Light");
            AddComponentMenuItem<CameraComponent>(reg, ctx, ctx.selectedEntity, "Camera");
            AddComponentMenuItem<AudioSource>(reg, ctx, ctx.selectedEntity, "Audio Source");
            AddComponentMenuItem<Gimmick>(reg, ctx, ctx.selectedEntity, "Gimmick");
            AddComponentMenuItem<ParticleEmitter>(reg, ctx, ctx.selectedEntity, "Particle Emitter");
            AddComponentMenuItem<TrailRenderer>(reg, ctx, ctx.selectedEntity, "Trail Renderer");
            AddComponentMenuItem<DecalComponent>(reg, ctx, ctx.selectedEntity, "Decal");
            AddComponentMenuItem<Trigger>(reg, ctx, ctx.selectedEntity, "Trigger");
            ImGui::Separator();
            AddComponentMenuItem<UICanvas>(reg, ctx, ctx.selectedEntity, "UI Canvas");
            AddComponentMenuItem<UIRect>(reg, ctx, ctx.selectedEntity, "UI Rect");
            AddComponentMenuItem<UIImage>(reg, ctx, ctx.selectedEntity, "UI Image");
            AddComponentMenuItem<UIText>(reg, ctx, ctx.selectedEntity, "UI Text");
            AddComponentMenuItem<UIButton>(reg, ctx, ctx.selectedEntity, "UI Button");
            AddComponentMenuItem<UISlider>(reg, ctx, ctx.selectedEntity, "UI Slider");
            AddComponentMenuItem<UIToggle>(reg, ctx, ctx.selectedEntity, "UI Toggle");
            AddComponentMenuItem<UIScrollView>(reg, ctx, ctx.selectedEntity, "UI Scroll View");
            AddComponentMenuItem<UILayout>(reg, ctx, ctx.selectedEntity, "UI Layout (VBox/HBox/Grid)");
            AddComponentMenuItem<UIAnimator>(reg, ctx, ctx.selectedEntity, "UI Animator");
            AddComponentMenuItem<UIAnimPlayer>(reg, ctx, ctx.selectedEntity,
                                               "UI Anim Player (.uianim クリップ)");
            AddComponentMenuItem<SpriteAnimator>(reg, ctx, ctx.selectedEntity,
                                                 "Sprite Animator (.spranim シート)");
            AddComponentMenuItem<AnimatorController>(reg, ctx, ctx.selectedEntity,
                                                     "Animator Controller (.animfsm ステートマシン)");
            AddComponentMenuItem<FootIK>(reg, ctx, ctx.selectedEntity,
                                         "Foot IK (接地補正・Play 中のみ)");
            ImGui::Separator();
            AddComponentMenuItem<RigidBody>(reg, ctx, ctx.selectedEntity, "RigidBody");
            AddComponentMenuItem<BoxCollider>(reg, ctx, ctx.selectedEntity, "Box Collider");
            AddComponentMenuItem<SphereCollider>(reg, ctx, ctx.selectedEntity, "Sphere Collider");
            AddComponentMenuItem<CapsuleCollider>(reg, ctx, ctx.selectedEntity, "Capsule Collider");
            AddComponentMenuItem<CharacterController>(reg, ctx, ctx.selectedEntity, "Character Controller");
            ImGui::Separator();
            AddComponentMenuItem<NetworkIdentity>(reg, ctx, ctx.selectedEntity, "Network Identity");
            AddComponentMenuItem<NetworkTransform>(reg, ctx, ctx.selectedEntity, "Network Transform");
            ImGui::Separator();
            // スクリプト（.lua）をクリックでアタッチ — ドラッグ不要
            if (ImGui::BeginMenu("\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88"))  // スクリプト
            {
                ScriptPicker(ctx, ctx.selectedEntity, m_assetsDir);
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
    }
    else
    {
        ImGui::TextDisabled("\xe3\x82\xa8\xe3\x83\xb3\xe3\x83\x86\xe3\x82\xa3\xe3\x83\x86\xe3\x82\xa3\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84");  // Select an entity
    }

    // Inspector ウィンドウ全体を .lua とテクスチャのドロップ先にする（どこにドロップしても付く）。
    // 個別スロット(テクスチャ上書きUI等)の上では小さいターゲットが優先される(ImGui は面積最小を採用)。
    if (ctx.HasSelection())
    {
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        if (win && ImGui::BeginDragDropTargetCustom(win->Rect(), win->ID))
        {
            namespace fs = std::filesystem;
            // ドロップパスを assets 相対へ（.lua/テクスチャ共通）
            auto toAssetsRel = [this](const char* pathCStr) {
                auto abs  = fs::path(pathCStr).lexically_normal().string();
                auto base = fs::path(m_assetsDir).lexically_normal().string();
                std::replace(abs.begin(),  abs.end(),  '\\', '/');
                std::replace(base.begin(), base.end(), '\\', '/');
                return (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;
            };

            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_SCRIPT"))
            {
                std::string rel = toAssetsRel(static_cast<const char*>(payload->Data));
                for (auto ent : ctx.selectedEntities)
                    ctx.pendingScriptAttachments.push_back({ent, rel});
            }

            // テクスチャ: 選択エンティティの Albedo に割当（全サブメッシュ、Undo対応）。
            // スロットに正確に落とさなくても「オブジェクトを選んで Inspector に投げる」だけで貼れる。
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
            {
                const char* pathCStr = static_cast<const char*>(payload->Data);
                std::string ext = fs::path(pathCStr).extension().string();
                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Texture)
                {
                    std::string rel = toAssetsRel(pathCStr);
                    for (auto ent : ctx.selectedEntities)
                    {
                        auto* mr = reg.try_get<MeshRenderer>(ent);
                        if (!mr) continue;
                        MeshRenderer before = *mr;
                        const u32 n = mr->meshes.empty() ? 1u : static_cast<u32>(mr->meshes.size());
                        for (u32 smi = 0; smi < n; ++smi)
                            MeshRenderer::SetOverride(mr->overrideAlbedoTexture, smi, rel);
                        ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<MeshRenderer>>(
                            &reg, ent, before, *mr, "Material Texture"));
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    ImGui::End();
}

// グローバルなエンジン設定（カメラ速度/シャドウ/オーディオ/VSync/ビルド）。
// Inspector から分離して独立ウィンドウ「エンジン設定」に描く（下ドックに置く）。
// ── ライト専用インスペクター（明るさ/色/距離/コーン/方向）。種別を判定して該当UIを出す ──
void InspectorPanel::RenderPrefabHeader(entt::registry& reg, EditorContext& ctx, Scene& scene,
                                        entt::entity e)
{
    const auto& link = reg.get<PrefabLink>(e);

    // 差分キャッシュの更新（選択が変わった時 + 0.3 秒ごと）
    m_prefabDiffTimer -= ImGui::GetIO().DeltaTime;
    if (m_prefabDiffEntity != e || m_prefabDiffTimer <= 0.0f)
    {
        m_prefabDiffEntity = e;
        m_prefabDiffTimer  = 0.3f;
        m_prefabDiffOk = SceneSerializer::ComputePrefabOverrides(scene, e, m_assetsDir, m_prefabDiff);
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.20f, 0.26f, 1.0f));
    ImGui::BeginChild("##PrefabHeader", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);

    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.95f, 1.0f), "プレハブ");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", link.sourcePath.c_str());

    if (!m_prefabDiffOk)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f),
                           "元の .prefab が見つからへん（移動/削除された？）");
    }

    const bool dirty = m_prefabDiffOk && !m_prefabDiff.empty();

    // ★Play 中は全部無効化する。これらのボタンは pending キューへ積むだけで、
    //   消化側（ApplicationRender）が Editor モードでしか回らない。押せてしまうと
    //   要求は誰にも読まれずログにもエラーにも出ないまま消える（MCP 側は ModeConflict）。
    if (ctx.isPlaying)
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                           "Play 中はプレハブを編集できません（Stop してください）");

    if (!m_prefabDiffOk || ctx.isPlaying) ImGui::BeginDisabled();
    if (ImGui::Button("適用 Apply"))
        ctx.pendingPrefabApply.push_back(e);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("このインスタンスの今の姿を元の .prefab へ書き戻し、同じプレハブの\n"
                          "他のインスタンスへも反映する。各インスタンスの手直し（置いた位置、\n"
                          "差し替えたマテリアル、手で足した子）はそのまま残る");
    ImGui::SameLine();
    if (ImGui::Button("元に戻す Revert"))
        ctx.pendingPrefabRevert.push_back(e);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("元の .prefab の状態へ戻す（外側の親だけ維持。この操作は作り直しなので\n"
                          "エンティティ ID が変わる）");
    ImGui::SameLine();
    if (ImGui::Button("他を強制リセット"))
        ctx.pendingPrefabPropagate.push_back(e);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("同じ .prefab から作った他のインスタンスを、保存済みの .prefab の内容で\n"
                          "まるごと作り直す。★各インスタンスの手直しは消える。\n"
                          ".prefab をエディタの外で直接編集したときの手動リセット用。\n"
                          "普通に直して配りたいだけなら「適用」を押せばよい（手直しが残る）");
    ImGui::SameLine();
    if (ImGui::Button("リンクを外す"))
    {
        // ★Undo に積む。以前はその場で remove していたので、隣の「他を強制リセット」と
        //   押し間違えると Ctrl+Z でも戻せず、シーン JSON を手で直すしかなかった。
        if (auto* pl = reg.try_get<PrefabLink>(e))
        {
            ctx.undoSystem.PushCommand(
                std::make_unique<RemoveComponentCommand<PrefabLink>>(
                    &reg, e, *pl, "Unlink Prefab"));
            reg.remove<PrefabLink>(e);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ただのコピーに戻す（以後 適用/元に戻す は出えへん）");
    if (!m_prefabDiffOk || ctx.isPlaying) ImGui::EndDisabled();

    if (dirty)
    {
        char label[96];
        std::snprintf(label, sizeof(label), "変更点 %d 件###PrefabOverrides",
                      static_cast<int>(m_prefabDiff.size()));
        if (ImGui::TreeNode(label))
        {
            // 件数が多い時に Inspector を占領しないよう、スクロール枠に閉じ込める
            ImGui::BeginChild("##OvrList", ImVec2(0.0f, 120.0f), ImGuiChildFlags_Borders);
            for (const auto& o : m_prefabDiff)
                ImGui::BulletText("[%s] %s . %s", o.entityName.c_str(), o.component.c_str(),
                                  o.field.c_str());
            ImGui::EndChild();
            ImGui::TreePop();
        }
    }
    else if (m_prefabDiffOk)
    {
        ImGui::TextDisabled("元の .prefab と同じ状態");
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void InspectorPanel::RenderLightHero(entt::registry& reg, EditorContext& ctx, entt::entity e)
{
    const EditorUiIcons* ic = ctx.icons;
    const ImVec4 amber(0.96f, 0.72f, 0.25f, 1.0f);

    // ヘッダ（アイコン + 「ライト」 + 種別名）
    if (ic && ic->entLight)
    {
        float s = ImGui::GetTextLineHeight() * 1.3f;
        ImGui::Image(static_cast<ImTextureID>(ic->entLight), ImVec2(s, s));
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::AlignTextToFramePadding();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, amber);
    ImGui::TextUnformatted("ライト");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextDisabled("%s",
        reg.all_of<DirectionalLight>(e) ? "Directional — 太陽光（全体を照らす）" :
        reg.all_of<SpotLight>(e)        ? "Spot — スポット（円錐状）"          :
                                          "Point — 点光源（全方向）");

    ImGui::Spacing();

    // 色 × 明るさ の結果を帯でプレビュー（実際の光の見え方の目安）
    auto previewSwatch = [&](const DirectX::XMFLOAT3& col, float intensity)
    {
        float k = intensity > 1.0f ? 1.0f : intensity;
        ImVec4 c(col.x * k, col.y * k, col.z * k, 1.0f);
        ImGui::ColorButton("##lightpreview", c,
            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
            ImVec2(ImGui::GetContentRegionAvail().x, 14.0f));
        ImGui::Spacing();
    };

    if (reg.all_of<PointLight>(e))
    {
        BeginEdit(reg, e, m_plEdit);
        auto& pl = reg.get<PointLight>(e);
        bool changed = false, active = false;
        previewSwatch(pl.color, pl.intensity);
        if (pg::Begin("PointLight"))
        {
            changed |= pg::Color3("色 Color", &pl.color.x, ImGuiColorEditFlags_NoInputs);
            active  |= ImGui::IsItemActive();
            changed |= pg::SliderFloat("明るさ Brightness", &pl.intensity, 0.0f, 20.0f, "%.2f", &active);
            changed |= pg::SliderFloat("距離 Range", &pl.range, 0.1f, 100.0f, "%.1f", &active,
                "位置は Transform で決まります");
            changed |= pg::Checkbox("影を落とす Shadows", &pl.castShadows,
                "同時に影を落とせるポイントライトは最大2灯（カメラに近い順で優先）。\n"
                "超えた分は影なしにフォールバックします。");
            pg::End();
        }
        EndEdit(reg, ctx, e, m_plEdit, changed, active, "PointLight");
    }
    if (reg.all_of<DirectionalLight>(e))
    {
        BeginEdit(reg, e, m_dlEdit);
        auto& dl = reg.get<DirectionalLight>(e);
        bool changed = false, active = false;
        previewSwatch(dl.color, dl.intensity);
        if (pg::Begin("DirectionalLight"))
        {
            changed |= pg::Color3("色 Color", &dl.color.x, ImGuiColorEditFlags_NoInputs);
            active  |= ImGui::IsItemActive();
            changed |= pg::SliderFloat("明るさ Brightness", &dl.intensity, 0.0f, 10.0f, "%.2f", &active);
            changed |= pg::SliderFloat("環境光 Ambient", &dl.ambient, 0.0f, 1.0f, "%.2f", &active);
            pg::End();
        }
        changed |= DirectionEditor("dlDir", dl.direction, active);
        ImGui::TextDisabled("このライトの向きが影の向きになります");
        EndEdit(reg, ctx, e, m_dlEdit, changed, active, "DirectionalLight");
    }
    if (reg.all_of<SpotLight>(e))
    {
        BeginEdit(reg, e, m_slEdit);
        auto& sl = reg.get<SpotLight>(e);
        bool changed = false, active = false;
        previewSwatch(sl.color, sl.intensity);
        if (pg::Begin("SpotLight"))
        {
            changed |= pg::Color3("色 Color", &sl.color.x, ImGuiColorEditFlags_NoInputs);
            active  |= ImGui::IsItemActive();
            changed |= pg::SliderFloat("明るさ Brightness", &sl.intensity, 0.0f, 30.0f, "%.2f", &active);
            changed |= pg::SliderFloat("距離 Range", &sl.range, 0.1f, 100.0f, "%.1f", &active);
            changed |= pg::SliderFloat("内側コーン Inner", &sl.innerConeDeg, 1.0f, 80.0f, "%.0f°", &active);
            changed |= pg::SliderFloat("外側コーン Outer", &sl.outerConeDeg, 1.0f, 89.0f, "%.0f°", &active);
            if (sl.innerConeDeg > sl.outerConeDeg) sl.innerConeDeg = sl.outerConeDeg;
            changed |= pg::Checkbox("影を落とす Shadows", &sl.castShadows,
                "同時に影を落とせるスポットライトは最大4灯（カメラに近い順で優先）。\n"
                "超えた分は影なしにフォールバックします。");
            pg::End();
        }
        changed |= DirectionEditor("slDir", sl.direction, active);
        ImGui::TextDisabled("位置は Transform、向きは上の方向で決まります");
        EndEdit(reg, ctx, e, m_slEdit, changed, active, "SpotLight");
    }

    ImGui::Separator();
}

// ── オーディオ専用インスペクター（クリップ/音量/再生/空間化） ──
void InspectorPanel::RenderAudioHero(entt::registry& reg, EditorContext& ctx, entt::entity e)
{
    const EditorUiIcons* ic = ctx.icons;
    const ImVec4 green(0.37f, 0.78f, 0.49f, 1.0f);

    auto& as = reg.get<AudioSource>(e);

    // ヘッダ（アイコン + 「オーディオ」 + 2D/3D 種別）
    if (ic && ic->entAudio)
    {
        float s = ImGui::GetTextLineHeight() * 1.3f;
        ImGui::Image(static_cast<ImTextureID>(ic->entAudio), ImVec2(s, s));
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::AlignTextToFramePadding();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, green);
    ImGui::TextUnformatted("オーディオ");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextDisabled("%s", as.spatial ? "3D 空間音" : "2D サウンド");

    ImGui::Spacing();

    BeginEdit(reg, e, m_audioEdit);
    bool changed = false, active = false;

    if (pg::Begin("AudioSource"))
    {
        char buf[256] = {};
        size_t n = as.clipPath.copy(buf, sizeof(buf) - 1);
        buf[n] = '\0';
        if (pg::InputText("クリップ Clip", buf, sizeof(buf), 0, &active))
        { as.clipPath = buf; changed = true; }

        changed |= pg::SliderFloat("音量 Volume", &as.volume, 0.0f, 1.0f, "%.2f", &active);

        pg::Group("再生");
        changed |= pg::Checkbox("開始時に再生 Play On Start", &as.playOnStart);
        changed |= pg::Checkbox("ループ Loop", &as.loop);

        pg::Group("空間化");
        changed |= pg::Checkbox("3D 空間音にする Spatial", &as.spatial,
            "空間化はモノラル wav のみ。位置は Transform。");
        if (as.spatial)
        {
            changed |= pg::Float("最小距離 Min", &as.minDistance, 0.1f, 0.0f, 1000.0f, "%.1f", &active);
            changed |= pg::Float("最大距離 Max", &as.maxDistance, 0.5f, 0.1f, 5000.0f, "%.1f", &active);
        }
        pg::End();
    }

    EndEdit(reg, ctx, e, m_audioEdit, changed, active, "Audio Source");
    ImGui::Separator();
}

void InspectorPanel::RenderEngineSettings(EditorContext& ctx,
                                          Camera* camera,
                                          AudioSystem* audioSystem,
                                          PhysicsDebugRenderer* physicsDebugRenderer,
                                          bool& physicsDebugDraw,
                                          bool& useVsync,
                                          i32& shadowQualityIndex,
                                          u32& shadowMapSize,
                                          bool& shadowMapDirty,
                                          f32& cascadeSplitLambda,
                                          f32& cascadeBlendBand,
                                          bool& showCascadeDebug,
                                          GameClock* /*clock*/)  // ビルド節を移設して未使用に
{
    ImGui::Begin("\xe3\x82\xa8\xe3\x83\xb3\xe3\x82\xb8\xe3\x83\xb3\xe8\xa8\xad\xe5\xae\x9a");  // エンジン設定

    // --- Camera ---
    if (IconHeader(ctx.icons, ctx.icons ? ctx.icons->entCamera : 0,
                   "\xe3\x82\xab\xe3\x83\xa1\xe3\x83\xa9", ImGuiTreeNodeFlags_DefaultOpen))  // Camera
    {
        if (pg::Begin("EngineCam"))
        {
            auto camPos = camera->GetPosition();
            pg::Text("位置", "%.1f, %.1f, %.1f", camPos.x, camPos.y, camPos.z);
            f32 moveSpeed = camera->GetMoveSpeed();
            if (pg::SliderFloat("移動速度", &moveSpeed, 1.0f, 50.0f, "%.1f"))
                camera->SetMoveSpeed(moveSpeed);
            pg::End();
        }
    }

    // --- Gizmo ---
    // ギズモのスナップ量。従来は SceneViewPanel にハードコード（移動1.0 / 回転15 / スケール0.1）
    // やったのでここに出す。既定値は同じなので、触らなければ挙動は変わらない。
    if (IconHeader(nullptr, 0, "ギズモ"))
    {
        if (pg::Begin("EngineGizmo"))
        {
            pg::Float("移動スナップ", &ctx.snapTranslate, 0.05f, 0.001f, 1000.0f, "%.3f m",
                      nullptr, "Ctrl ドラッグ中に位置がこの刻みへ吸着する");
            pg::Float("回転スナップ", &ctx.snapRotateDeg, 0.5f, 0.1f, 180.0f, "%.1f deg",
                      nullptr, "Ctrl ドラッグ中に角度がこの刻みへ吸着する");
            pg::Float("スケールスナップ", &ctx.snapScale, 0.01f, 0.001f, 100.0f, "%.3f",
                      nullptr, "Ctrl ドラッグ中に倍率がこの刻みへ吸着する");
            pg::Checkbox("常にスナップ", &ctx.snapAlways,
                         "ON にすると Ctrl を押さなくても常にスナップする");
            pg::End();
        }
    }

    // --- Shadow quality ---
    if (IconHeader(nullptr, 0, "シャドウ"))
    {
        const char* qualities[] = {"1024 (Low)", "2048 (Medium)", "4096 (High)", "8192 (Ultra)"};
        const u32 sizes[] = {1024, 2048, 4096, 8192};
        if (pg::Begin("EngineShadow"))
        {
            if (pg::Combo("解像度", &shadowQualityIndex, qualities, 4))
            {
                shadowMapSize = sizes[shadowQualityIndex];
                shadowMapDirty = true;
            }
            pg::Text("実サイズ", "%ux%u", shadowMapSize, shadowMapSize);

            pg::Group("CSM (4分割)");
            pg::SliderFloat("分割λ", &cascadeSplitLambda, 0.0f, 1.0f, "%.2f");
            pg::SliderFloat("境界ブレンド", &cascadeBlendBand, 0.0f, 5.0f, "%.2f");
            pg::Checkbox("カスケード可視化", &showCascadeDebug);
            pg::End();
        }
    }

    // --- Audio ---
    if (IconHeader(ctx.icons, ctx.icons ? ctx.icons->entAudio : 0,
                   "\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xaa"))  // Audio
    {
        f32 masterVol = audioSystem->GetMasterVolume();
        f32 bgmVol    = audioSystem->GetBGMVolume();
        f32 sfxVol    = audioSystem->GetSFXVolume();
        if (pg::Begin("EngineAudio"))
        {
            if (pg::SliderFloat("マスター", &masterVol, 0.0f, 1.0f, "%.2f"))
                audioSystem->SetMasterVolume(masterVol);
            if (pg::SliderFloat("BGM", &bgmVol, 0.0f, 1.0f, "%.2f"))
                audioSystem->SetBGMVolume(bgmVol);
            if (pg::SliderFloat("SE", &sfxVol, 0.0f, 1.0f, "%.2f"))
                audioSystem->SetSFXVolume(sfxVol);
            pg::End();
        }

        const auto& bgmList = audioSystem->GetBGMList();
        for (const auto& bgm : bgmList)
        {
            std::string fn = std::filesystem::path(bgm).filename().string();
            ImGui::PushID(bgm.c_str());
            if (ImGui::Button("\xe2\x96\xb6")) audioSystem->PlayBGM(bgm);
            ImGui::SameLine(); ImGui::Text("%s", fn.c_str());
            ImGui::PopID();
        }
        if (!bgmList.empty() && ImGui::Button("BGM \xe5\x81\x9c\xe6\xad\xa2"))
            audioSystem->StopBGM();

        const auto& sfxList = audioSystem->GetSFXList();
        for (const auto& sfx : sfxList)
        {
            std::string fn = std::filesystem::path(sfx).filename().string();
            ImGui::PushID(sfx.c_str());
            if (ImGui::Button("\xe2\x96\xb6")) audioSystem->PlaySFX(sfx);
            ImGui::SameLine(); ImGui::Text("%s", fn.c_str());
            ImGui::PopID();
        }
        if (!sfxList.empty() && ImGui::Button("SE \xe5\x85\xa8\xe5\x81\x9c\xe6\xad\xa2"))
            audioSystem->StopAllSFX();
    }

    // --- Settings ---
    if (IconHeader(nullptr, 0, "設定"))
    {
        if (pg::Begin("EngineMisc"))
        {
            pg::Checkbox("VSync", &useVsync);

            bool debugDraw = physicsDebugRenderer->IsEnabled();
            if (pg::Checkbox("Physics Debug", &debugDraw))
            {
                physicsDebugRenderer->SetEnabled(debugDraw);
                physicsDebugDraw = debugDraw;
            }
            pg::End();
        }
    }

    // --- Build ---
    if (IconHeader(ctx.icons, ctx.icons ? ctx.icons->build : 0,
                   "\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89"))  // Build
    {
        // ビルドは専用の「ビルド設定」パネルで構成・開始シーン・出力先を決めてから実行する。
        if (ImGui::Button("ビルド設定を開く"))
            ctx.showBuildSettings = true;
    }

    ImGui::End();
}

} // namespace dx12e
