#include "editor/UiWidgets.h"
#include "editor/panels/LightingPanel.h"

#include "editor/EditorContext.h"
#include "editor/EditorIcons.h"
#include "editor/EditorTheme.h"
#include "editor/GiMigrationCommand.h"
#include "editor/LightMath.h"
#include "editor/LightingPresets.h"
#include "editor/PropertyGrid.h"
#include "editor/UndoSystem.h"
#include "ecs/Components.h"
#include "scene/Scene.h"
#include "renderer/atmosphere/AtmosphereMath.h"   // 大気の読み取り表示（太陽の高度・方位・照度）

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dx12e
{

using namespace DirectX;
namespace lm = lightmath;

namespace
{

// クラスタードライティング（Forward+）の灯数バジェット。
// 「点 8 / スポット 8」の個別上限は撤廃済みで、今は point + spot 合計 1024 灯。
// 1 クラスタあたりは 128 灯（超えた分は無言で消えるが CPU からは検出できない）。
// 実体は editor/LightingPresets.h（MCP の dx12_list_lights と同じ数字を出すため 1 箇所に集約）。
constexpr int kMaxTotalLights  = kLightBudgetTotal;
constexpr int kMaxPerCluster   = kLightBudgetPerCluster;

// ── InspectorPanel の IconHeader と同じ見出し帯（実体は ui::SectionHeader）──
// tex は従来の PNG アイコンの有無（0 = アイコン無し）。あるときはライトのグリフ（琥珀）を出す。
bool SectionHeader(const EditorUiIcons* ic, u64 tex, const char* label,
                   ImGuiTreeNodeFlags flags = 0)
{
    const bool hasIcon = ic && tex;
    return ui::SectionHeader(label, hasIcon ? ICON_T_LIGHT : nullptr,
                             hasIcon ? &theme::TypeLight : nullptr, flags);
}

// ── コンポーネント編集の Undo 追跡（InspectorPanel の BeginEdit/EndEdit と同じ流儀）──
template<typename T>
struct EditTracker
{
    bool editing = false;
    T    snapshot{};
};

template<typename T>
void BeginTrack(entt::registry& reg, entt::entity e, EditTracker<T>& tr)
{
    if (!tr.editing) tr.snapshot = reg.get<T>(e);
}

template<typename T>
void EndTrack(entt::registry& reg, EditorContext& ctx, entt::entity e,
              EditTracker<T>& tr, bool changed, bool active, const char* name)
{
    auto push = [&]()
    {
        const T& cur = reg.get<T>(e);
        if (std::memcmp(&tr.snapshot, &cur, sizeof(T)) != 0)
        {
            ctx.undoSystem.PushCommand(
                std::make_unique<ComponentEditCommand<T>>(&reg, e, tr.snapshot, cur, name));
        }
    };

    if (active)                                        tr.editing = true;
    else if (changed)                                { push(); tr.editing = false; }
    else if (tr.editing && !ImGui::IsAnyItemActive()) { push(); tr.editing = false; }
}

// ── ミュート（目玉アイコン）: 元の強度を覚えて 0 にする。Undo 1 エントリ ──
using MuteList = std::vector<std::pair<entt::entity, f32>>;

int FindMuted(const MuteList& list, entt::entity e)
{
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].first == e) return static_cast<int>(i);
    return -1;
}

template<typename T>
void ToggleMute(entt::registry& reg, EditorContext& ctx, entt::entity e, MuteList& muted)
{
    if (!reg.all_of<T>(e)) return;
    const T before = reg.get<T>(e);
    T& cur = reg.get<T>(e);

    const int idx = FindMuted(muted, e);
    if (idx >= 0)
    {
        cur.intensity = muted[static_cast<size_t>(idx)].second;
        muted.erase(muted.begin() + idx);
    }
    else
    {
        muted.emplace_back(e, cur.intensity);
        cur.intensity = 0.0f;
    }
    ctx.undoSystem.PushCommand(
        std::make_unique<ComponentEditCommand<T>>(&reg, e, before, cur, "ライトのミュート"));
}

template<typename T>
void UnmuteOne(entt::registry& reg, EditorContext& ctx, entt::entity e, f32 restore)
{
    if (!reg.valid(e) || !reg.all_of<T>(e)) return;
    const T before = reg.get<T>(e);
    reg.get<T>(e).intensity = restore;
    ctx.undoSystem.PushCommand(
        std::make_unique<ComponentEditCommand<T>>(&reg, e, before, reg.get<T>(e),
                                                  "ライトのミュート解除"));
}

// ── ライティング・プリセット（太陽 + ポストをまとめて置き換え。Undo は 1 エントリ）──
class LightingPresetCommand : public IUndoCommand
{
public:
    LightingPresetCommand(entt::registry* reg, entt::entity sun, Scene* scene,
                          const DirectionalLight& sunBefore, const DirectionalLight& sunAfter,
                          const PostProcessSettings& postBefore,
                          const PostProcessSettings& postAfter)
        : m_reg(reg), m_sun(sun), m_scene(scene),
          m_sunBefore(sunBefore), m_sunAfter(sunAfter),
          m_postBefore(postBefore), m_postAfter(postAfter) {}

    void Undo() override { Apply(m_sunBefore, m_postBefore); }
    void Redo() override { Apply(m_sunAfter,  m_postAfter); }
    const char* GetName() const override { return "ライティング・プリセット"; }

private:
    void Apply(const DirectionalLight& dl, const PostProcessSettings& pp)
    {
        if (m_sun != entt::null && m_reg->valid(m_sun) && m_reg->all_of<DirectionalLight>(m_sun))
            m_reg->get<DirectionalLight>(m_sun) = dl;
        if (m_scene) m_scene->GetPostSettings() = pp;
    }

    entt::registry*     m_reg;
    entt::entity        m_sun;
    Scene*              m_scene;
    DirectionalLight    m_sunBefore, m_sunAfter;
    PostProcessSettings m_postBefore, m_postAfter;
};

// ── 3 点ライト設置（Undo で消す / Redo で作り直す）──
// 中身は NameTag + Transform + PointLight だけなのでモデルロードを伴わない。
// UndoSystem の GroupCommand と同じく、その場で create/destroy してよい種類の操作。
class SpawnStudioLightsCommand : public IUndoCommand
{
public:
    struct Rec
    {
        std::string name;
        Transform   tf;
        PointLight  pl;
    };

    SpawnStudioLightsCommand(entt::registry* reg, std::vector<Rec> recs,
                             std::vector<entt::entity> created)
        : m_reg(reg), m_recs(std::move(recs)), m_entities(std::move(created)) {}

    void Undo() override
    {
        for (auto e : m_entities)
            if (m_reg->valid(e)) m_reg->destroy(e);
        m_entities.clear();
    }

    void Redo() override
    {
        m_entities.clear();
        for (const auto& r : m_recs)
        {
            const entt::entity e = m_reg->create();
            m_reg->emplace<NameTag>(e, NameTag{r.name});
            m_reg->emplace<Transform>(e, r.tf);
            m_reg->emplace<PointLight>(e, r.pl);
            m_entities.push_back(e);
        }
    }

    const char* GetName() const override { return "3点ライト"; }

private:
    entt::registry*           m_reg;
    std::vector<Rec>          m_recs;
    std::vector<entt::entity> m_entities;
};

// ── プリセット定義は editor/LightingPresets.h（MCP の dx12_apply_lighting_preset と共有）──

// 太陽ロールの DirectionalLight を選ぶ規則は Application と同じ「最初の 1 灯」
entt::entity FindSun(entt::registry& reg)
{
    for (auto e : reg.view<DirectionalLight>())
        return e;
    return entt::null;
}

// ── GI モードの切り替え（旧 ⇄ 新）。構成は scene/GiMigration.h（MCP の migrate_gi / 新規シーンと同じ）。
//   gi・DDGI・SSGI・RT 影・太陽の ambient をまとめて書き換え、Undo は 1 エントリ。
enum class GiAction { ToNew, ToLegacy, RefitGrid };

void RunGiAction(Scene* scene, EditorContext& ctx, GiAction act)
{
    const gi::State before = gi::Capture(*scene);
    gi::Result r;
    const char* undoName = "GI";
    switch (act)
    {
    case GiAction::ToNew:
    {
        gi::Options o;
        o.dxrSupported = ctx.dxrSupported;
        o.refitGrid    = !before.ddgi.enabled;   // 手で置いた DDGI の格子が ON ならそのまま使う
        r = gi::ApplyNew(*scene, o);
        undoName = "GI を新しい方式へ切り替え";
        break;
    }
    case GiAction::ToLegacy:
        r = gi::ApplyLegacy(*scene);
        undoName = "GI を旧へ戻す";
        break;
    case GiAction::RefitGrid:
        r = gi::RefitGrid(*scene);
        undoName = "DDGI の範囲をシーンに合わせる";
        break;
    }
    if (!r.applied)
    {
        ctx.Notify(ui::ToastKind::Warn, r.reason.empty() ? "GI を切り替えられませんでした" : r.reason);
        return;
    }
    ctx.undoSystem.PushCommand(
        std::make_unique<GiMigrationCommand>(scene, before, gi::Capture(*scene), undoName));
    if (act == GiAction::ToNew)
        ctx.Notify(ui::ToastKind::Success, "新しい GI に切り替えました（Ctrl+Z で戻せます）");
    else if (act == GiAction::ToLegacy)
        ctx.Notify(ui::ToastKind::Success, "GI を旧に戻しました（Ctrl+Z でやり直せます）");
    else
        ctx.Notify(ui::ToastKind::Success, "DDGI の範囲をシーンに合わせました");
}

ImVec4 SwatchColor(const XMFLOAT3& col, f32 intensity)
{
    const f32 k = (intensity > 1.0f) ? 1.0f : ((intensity < 0.0f) ? 0.0f : intensity);
    return ImVec4(col.x * k, col.y * k, col.z * k, 1.0f);
}

} // namespace

void RenderLightingPanel(Scene* scene,
                         EditorContext& ctx,
                         i32& shadowQualityIndex,
                         u32& shadowMapSize,
                         bool& shadowMapDirty,
                         f32& cascadeSplitLambda,
                         f32& cascadeBlendBand,
                         bool& showCascadeDebug)
{
    if (!ctx.showLighting || !scene) return;

    ImGui::SetNextWindowSize(ui::Px(430.0f, 760.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ライティング###LightingPanelFloating",
                      &ctx.showLighting, ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                               | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem
                               | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
        ctx.floatingToolWindowHoveredThisFrame = true;

    // ★Play 中の変更は Stop で捨てられる（Stop は Play 開始時のシーン JSON から復元する）。
    //   この窓は Play 中も編集できるので、黙って巻き戻る前に言っておく。
    if (ctx.isPlaying)
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
            "Play 中の変更は Stop で破棄されます（残すなら Stop してから調整）");

    entt::registry& reg = scene->GetRegistry();
    const EditorUiIcons* ic = ctx.icons;
    const entt::entity sun = FindSun(reg);

    // ミュート中のライト（強度 0 にして退避した元の値）。エディタ内だけの一時状態。
    // エンティティが消えた / 強度が 0 でなくなった（Undo で戻した・シーンを入れ替えて
    // 同じ ID が別のライトに再利用された）エントリはここで落とす＝嘘の「OFF」を出さない。
    static MuteList s_muted;
    for (size_t i = s_muted.size(); i-- > 0; )
    {
        const entt::entity me = s_muted[i].first;
        bool stale = !reg.valid(me);
        if (!stale)
        {
            f32 cur = -1.0f;
            if (reg.all_of<PointLight>(me))            cur = reg.get<PointLight>(me).intensity;
            else if (reg.all_of<SpotLight>(me))        cur = reg.get<SpotLight>(me).intensity;
            else if (reg.all_of<DirectionalLight>(me)) cur = reg.get<DirectionalLight>(me).intensity;
            stale = (cur != 0.0f);
        }
        if (stale) s_muted.erase(s_muted.begin() + static_cast<std::ptrdiff_t>(i));
    }

    // =====================================================================
    // シーンのライト一覧 + 灯数の上限警告
    // =====================================================================
    if (SectionHeader(ic, ic ? ic->entLight : 0, "シーンのライト",
                      ImGuiTreeNodeFlags_DefaultOpen))
    {
        struct Row { entt::entity e; int kind; int slot; };   // kind 0=Dir 1=Point 2=Spot
        std::vector<Row> rows;
        int dirCount = 0, pointCount = 0, spotCount = 0;

        for (auto [e, dl, tf] : reg.view<const DirectionalLight, const Transform>().each())
        {
            (void)dl; (void)tf;
            rows.push_back({e, 0, dirCount++});
        }
        for (auto [e, pl, tf] : reg.view<const PointLight, const Transform>().each())
        {
            (void)pl; (void)tf;
            rows.push_back({e, 1, pointCount++});
        }
        for (auto [e, sl, tf] : reg.view<const SpotLight, const Transform>().each())
        {
            (void)sl; (void)tf;
            rows.push_back({e, 2, spotCount++});
        }

        // 灯数表示。クラスタードライティングで個別上限は消えたので「合計 / 1024」を出す。
        // 1 クラスタ 128 灯の切り捨ては CPU からは見えないので、ヒートマップへ誘導する。
        {
            const int totalPunctual = pointCount + spotCount;
            const bool over = totalPunctual > kMaxTotalLights;
            ImGui::TextUnformatted("ライト");
            ImGui::SameLine(0.0f, ui::Px(4.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, over ? theme::Bad : theme::TextDim);
            ImGui::Text("%d / %d", totalPunctual, kMaxTotalLights);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("クラスタードライティング（Forward+）。\n"
                                  "点光源とスポットに個別の上限はなく、合計 %d 灯まで GPU へ送れます。\n"
                                  "ただし画面を分割したクラスタ 1 マスあたりで評価できるのは %d 灯まで。\n"
                                  "ライトが密集して超えた所は無言で切り捨てられます（下の\n"
                                  "「クラスタデバッグ表示 > ライト複雑度」で白くなる所がそれ）。\n"
                                  "パーティクルの発光ライトも枠を使います。",
                                  kMaxTotalLights, kMaxPerCluster);
            if (over)
            {
                ImGui::SameLine(0.0f, ui::Px(6.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad);
                ImGui::TextUnformatted("超過");
                ImGui::PopStyleColor();
            }
            ImGui::SameLine(0.0f, ui::Px(18.0f));
            ImGui::TextDisabled("点 %d / スポット %d", pointCount, spotCount);
        }
        ImGui::SameLine(0.0f, ui::Px(18.0f));
        ImGui::TextDisabled("平行光 %d（太陽は先頭の1灯のみ有効）", dirCount);

        if (!s_muted.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn);
            ImGui::Text("ミュート中 %d 灯（強度 0 のまま保存されます）",
                        static_cast<int>(s_muted.size()));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton("全部戻す"))
            {
                MuteList copy = s_muted;
                s_muted.clear();
                for (const auto& m : copy)
                {
                    UnmuteOne<PointLight>(reg, ctx, m.first, m.second);
                    UnmuteOne<SpotLight>(reg, ctx, m.first, m.second);
                    UnmuteOne<DirectionalLight>(reg, ctx, m.first, m.second);
                }
            }
        }

        ImGui::BeginChild("##LightList", ui::Px(0.0f, 150.0f), ImGuiChildFlags_Borders);
        if (rows.empty())
        {
            ImGui::TextDisabled("ライトがありません（Inspector の「コンポーネント追加」から足せます）");
        }
        for (const Row& r : rows)
        {
            // 1 エンティティが複数種のライトを持つ場合もあるので種別も ID に混ぜる
            ImGui::PushID(static_cast<int>(static_cast<u32>(r.e)));
            ImGui::PushID(r.kind);

            // 目玉トグル。日本語フォントのグリフ範囲(ImGui の GetGlyphRangesJapanese)には
            // ● ○ のような記号が入っていないので、確実に出る ON/OFF の文字で表す。
            const bool muted = FindMuted(s_muted, r.e) >= 0;
            if (ImGui::SmallButton(muted ? "OFF" : "ON"))
            {
                if (r.kind == 0)      ToggleMute<DirectionalLight>(reg, ctx, r.e, s_muted);
                else if (r.kind == 1) ToggleMute<PointLight>(reg, ctx, r.e, s_muted);
                else                  ToggleMute<SpotLight>(reg, ctx, r.e, s_muted);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("一時ミュート（強度を 0 にして退避。もう一度押すと戻ります）");

            // 色 × 明るさのスウォッチ
            XMFLOAT3 col{1.0f, 1.0f, 1.0f};
            f32 intensity = 0.0f;
            const char* kindLabel = "D";
            if (r.kind == 0)
            { const auto& c = reg.get<DirectionalLight>(r.e); col = c.color; intensity = c.intensity; }
            else if (r.kind == 1)
            { const auto& c = reg.get<PointLight>(r.e);       col = c.color; intensity = c.intensity; kindLabel = "P"; }
            else
            { const auto& c = reg.get<SpotLight>(r.e);        col = c.color; intensity = c.intensity; kindLabel = "S"; }

            ImGui::SameLine(ui::Px(42.0f));   // ON/OFF で幅が変わっても以降の桁が揃うよう絶対位置で置く
            ImGui::ColorButton("##sw", SwatchColor(col, intensity),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                               ui::Px(14.0f, 14.0f));

            ImGui::SameLine(0.0f, ui::Px(6.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, theme::TypeLight);
            ImGui::TextUnformatted(kindLabel);
            ImGui::PopStyleColor();

            ImGui::SameLine(0.0f, ui::Px(6.0f));
            const char* name = reg.all_of<NameTag>(r.e)
                ? reg.get<NameTag>(r.e).name.c_str() : "(no name)";
            if (ImGui::Selectable(name, ctx.IsSelected(r.e), 0, ui::Px(180.0f, 0.0f)))
                ctx.Select(r.e);

            ImGui::SameLine();
            // 個別上限は撤廃済み。合計 1024 灯を超えた分だけが「無言で消える」。
            // Application は「点光源を先、スポットを後」の順で 1 本の配列へ積むので、
            // スポットの通し番号は pointCount + slot になる。
            const int combinedIdx = (r.kind == 1) ? r.slot
                                  : (r.kind == 2) ? pointCount + r.slot : -1;
            const bool over = (combinedIdx >= kMaxTotalLights);
            ImGui::PushStyleColor(ImGuiCol_Text, over ? theme::Bad : theme::TextFaint);
            if (over) ImGui::Text("%.2f  (上限超過)", static_cast<double>(intensity));
            else      ImGui::Text("%.2f", static_cast<double>(intensity));
            ImGui::PopStyleColor();

            ImGui::PopID();   // r.kind
            ImGui::PopID();   // r.e
        }
        ImGui::EndChild();
    }

    // =====================================================================
    // 太陽（最初の DirectionalLight）
    // =====================================================================
    if (SectionHeader(ic, ic ? ic->entLight : 0, "太陽 Sun", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (sun == entt::null)
        {
            ImGui::TextDisabled("DirectionalLight がありません。");
            ImGui::TextDisabled("Inspector の「コンポーネント追加」から足してください。");
        }
        else
        {
            static EditTracker<DirectionalLight> s_sunTrack;
            static f32 s_hour = 12.0f;

            BeginTrack(reg, sun, s_sunTrack);
            auto& dl = reg.get<DirectionalLight>(sun);
            bool changed = false, active = false;

            ImGui::ColorButton("##sunpreview", SwatchColor(dl.color, dl.intensity),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                               ImVec2(ImGui::GetContentRegionAvail().x, ui::Px(14.0f)));
            ImGui::Spacing();

            lm::SunAngles ang = lm::DirectionToSunAngles(dl.direction);
            f32 azimuth   = ang.azimuthDeg;
            f32 elevation = ang.elevationDeg;

            if (pg::Begin("LightSun"))
            {
                bool dirChanged = false;
                dirChanged |= pg::SliderFloat("方位 Azimuth", &azimuth, -180.0f, 180.0f, "%.0f°",
                                              &active, "太陽が見える方角。+Z が 0°、+X が 90°");
                dirChanged |= pg::SliderFloat("高度 Elevation", &elevation, -89.0f, 89.0f, "%.0f°",
                                              &active, "地平線が 0°、真上が 90°");
                if (dirChanged)
                {
                    lm::SunAngles na;
                    na.azimuthDeg   = azimuth;
                    na.elevationDeg = elevation;
                    dl.direction     = lm::SunAnglesToDirection(na);
                    dl._prevRotInit  = false;   // Transform 回転デルタ追従の基準を取り直させる
                    changed = true;
                }

                if (pg::SliderFloat("時刻 Time of Day", &s_hour, 0.0f, 24.0f, "%.1f 時", &active,
                                    "Lua の Lighting.setTimeOfDay(hour) と同じカーブで\n"
                                    "向き・色・強度・環境光をまとめて設定します"))
                {
                    const lm::TimeOfDaySample s = lm::SampleTimeOfDay(s_hour);
                    dl.direction    = s.direction;
                    dl.color        = s.color;
                    dl.intensity    = s.intensity;
                    dl.ambient      = s.ambient;
                    dl._prevRotInit = false;
                    changed = true;
                }

                pg::Group("明るさ");
                changed |= pg::Color3("色 Color", &dl.color.x, ImGuiColorEditFlags_NoInputs);
                active  |= ImGui::IsItemActive();
                changed |= pg::SliderFloat("強度 Intensity", &dl.intensity, 0.0f, 10.0f, "%.2f", &active);
                changed |= pg::SliderFloat("環境光 Ambient", &dl.ambient, 0.0f, 1.0f, "%.2f", &active,
                                           "影の中の明るさ。0 に近いほど陰影が強くなります");
                pg::End();
            }

            // 色温度プリセット（クリックで色だけ差し替え）
            ImGui::TextDisabled("色温度");
            struct Kelvin { const char* label; f32 k; };
            static const Kelvin kKelvins[] = {
                {"ろうそく 1900K", 1900.0f}, {"電球 2700K", 2700.0f}, {"白色 4000K", 4000.0f},
                {"昼光 5600K",     5600.0f}, {"曇天 7000K", 7000.0f}, {"日陰 9000K", 9000.0f},
            };
            for (int i = 0; i < 6; ++i)
            {
                if (i % 3 != 0) ImGui::SameLine(0.0f, ui::Px(4.0f));
                if (ImGui::SmallButton(kKelvins[i].label))
                {
                    dl.color = lm::KelvinToRGB(kKelvins[i].k);
                    changed  = true;
                }
            }

            EndTrack(reg, ctx, sun, s_sunTrack, changed, active, "DirectionalLight");
        }
    }

    // =====================================================================
    // 影（既存の「エンジン設定」窓と同じ値をここからも触る）
    // =====================================================================
    if (SectionHeader(nullptr, 0, "影 Shadows"))
    {
        const char* qualities[] = {"1024 (Low)", "2048 (Medium)", "4096 (High)", "8192 (Ultra)"};
        const u32 sizes[] = {1024u, 2048u, 4096u, 8192u};

        if (pg::Begin("LightShadow"))
        {
            bool shadowsOn = scene->GetShadowsEnabled();
            if (pg::Checkbox("このシーンで影を描く", &shadowsOn,
                             "OFF にすると影パスを丸ごと飛ばします（トップダウン等で FPS が上がる）"))
                scene->SetShadowsEnabled(shadowsOn);

            ImGui::BeginDisabled(!shadowsOn);
            if (pg::Combo("解像度", &shadowQualityIndex, qualities, 4))
            {
                if (shadowQualityIndex < 0) shadowQualityIndex = 0;
                if (shadowQualityIndex > 3) shadowQualityIndex = 3;
                shadowMapSize  = sizes[shadowQualityIndex];
                shadowMapDirty = true;
            }
            pg::Text("実サイズ", "%ux%u", shadowMapSize, shadowMapSize);

            pg::Group("CSM (4分割)");
            // 「エンジン設定」窓の「分割λ」と同じ値。λ は日本語フォントのグリフ範囲に
            // 入っていない(表示されない)ので、ここではラテン文字で書く。
            pg::SliderFloat("分割 lambda", &cascadeSplitLambda, 0.0f, 1.0f, "%.2f",
                            nullptr, "手前を細かくするか、奥まで均等にするかの配分");
            pg::SliderFloat("境界ブレンド", &cascadeBlendBand, 0.0f, 5.0f, "%.2f",
                            nullptr, "カスケードの継ぎ目をぼかす幅");
            pg::Checkbox("カスケード可視化", &showCascadeDebug,
                         "4分割の範囲を色分け表示（調整用）");
            ImGui::EndDisabled();
            pg::End();
        }
        ImGui::TextDisabled("影を落とせるのは スポット4灯 / ポイント2灯 まで（カメラに近い順）");

        // ---- コンタクトシャドウ（CSM の解像度では抜ける接地部の影をスクリーン空間で補う）----
        // CSM の ON/OFF とは独立（CSM を切っていても接地感だけ足せる）。
        if (pg::Begin("LightContactShadow"))
        {
            auto& cs = scene->GetContactShadowSettings();
            pg::Group("コンタクトシャドウ");
            pg::Checkbox("有効", &cs.enabled,
                         "深度バッファを太陽方向へレイマーチして、物と地面の接地部の細かい影を足します"
                         "（透視ビューのみ。2D 正射では無効）");
            ImGui::BeginDisabled(!cs.enabled);
            pg::SliderFloat("レイ長 (m)", &cs.rayLength, 0.02f, 2.0f, "%.2f", nullptr,
                            "伸ばすほど遠くの遮蔽を拾えますが、サンプル間隔が粗くなりノイズが増えます");
            pg::SliderInt("ステップ数", &cs.steps, 4, 32, nullptr,
                          "レイマーチのサンプル数。16 前後が品質/負荷の相場");
            pg::SliderFloat("厚み (m)", &cs.thickness, 0.02f, 2.0f, "%.2f", nullptr,
                            "遮蔽とみなす深度差の上限。深度バッファは表面しか持たないので、"
                            "これを超える差は「物体の裏側を通っただけ」として無視します");
            pg::SliderFloat("バイアス (m)", &cs.bias, 0.0f, 0.2f, "%.3f", nullptr,
                            "自己遮蔽（自分の面に自分の影が出るシミ）を抑える押し出し量");
            pg::SliderFloat("強度", &cs.intensity, 0.0f, 1.0f, "%.2f");
            pg::SliderFloat("フェード開始 (m)", &cs.maxDistance, 5.0f, 200.0f, "%.0f", nullptr,
                            "カメラからこの距離を超えたら効果を弱めます（遠景は精度が出ないため）");
            pg::SliderFloat("フェード幅 (m)", &cs.fadeDistance, 1.0f, 50.0f, "%.0f");
            ImGui::EndDisabled();
            pg::End();
        }

        // ---- PCSS（ソフトシャドウ）----
        // CSM の固定幅 PCF を「ブロッカー探索 → 可変ペナンブラ」へ置き換える。
        // OFF のときシェーダは従来の 3x3 PCF 経路をそのまま通る＝絵はビット一致。
        if (pg::Begin("LightPcss"))
        {
            auto& pc = scene->GetShadowPcssSettings();
            pg::Group("ソフトシャドウ (PCSS)");
            pg::Checkbox("有効", &pc.enabled,
                         "接地部は鋭く、遮蔽物から離れるほど柔らかい影にします"
                         "（太陽を面光源とみなす）。OFF で従来の 3x3 PCF に戻ります");
            ImGui::BeginDisabled(!pc.enabled);
            pg::SliderFloat("太陽の大きさ", &pc.lightTanAngle, 0.001f, 0.3f, "%.3f", nullptr,
                            "太陽の角半径の tan。実際の太陽は 0.0044（ほぼ硬い影）。"
                            "大きくするほど半影が急に太くなります");
            pg::SliderFloat("半影の上限 (texel)", &pc.maxPenumbraTexels, 1.0f, 64.0f, "%.0f", nullptr,
                            "ぼかし半径の上限。塗り面積＝コストの上限を決めます");
            pg::SliderFloat("探索半径 (texel)", &pc.blockerSearchTexels, 1.0f, 64.0f, "%.0f", nullptr,
                            "遮蔽物を探す範囲。大きすぎると重要な遮蔽物を飛ばして影に穴が開き、"
                            "小さすぎると遠くの遮蔽物を拾えず半影が伸びません");
            pg::Checkbox("時間ディザ (TAA 有効時のみ)", &pc.temporalDither,
                         "フレームごとにサンプルの回転位相を黄金比で回して、TAA の蓄積で"
                         "ノイズを消します。TAA が無効なときは自動で切れます（チラつくだけなので）");
            ImGui::EndDisabled();
            pg::End();
        }

        // ---- レイトレーシング（DXR）----
        // 太陽の影を BVH のレイで解く。CSM のアクネ / peter-panning / カスケード境界が消える。
        // ★出力先は既存のコンタクトシャドウ枠なので、ルートシグネチャは 1 DWORD も増えていない。
        if (pg::Begin("LightRt"))
        {
            auto& rt = scene->GetRtSettings();
            pg::Group("レイトレーシング (DXR)");

            // 非対応 GPU で「押せるのに何も起きない」のが最悪なので、必ず理由を出す。
            if (!ctx.dxrSupported)
            {
                const int t = ctx.dxrTier, sm = ctx.dxrShaderModel;
                ImGui::TextWrapped(
                    "この GPU では使えません（DXR Tier %s / ShaderModel %d.%d）。\n"
                    "inline raytracing には DXR Tier 1.1 かつ Shader Model 6.5 が必要です"
                    "（RTX 20 系 / RX 6000 系以降）。",
                    (t < 10) ? "非対応" : (std::to_string(t / 10) + "." + std::to_string(t % 10)).c_str(),
                    sm >> 4, sm & 0xF);
            }
            ImGui::BeginDisabled(!ctx.dxrSupported);

            pg::Checkbox("RT サン影", &rt.shadowEnabled,
                         "太陽の影をレイで解きます。シャドウマップ由来のアクネ・接地部の隙間"
                         "（peter-panning）・カスケード境界の段差が消えます。\n"
                         "★スキンドキャラと半透明は加速構造に入らないので従来どおり CSM が担当し、"
                         "両者は自動で合成されます（PCSS と併用して構いません）");
            ImGui::BeginDisabled(!rt.shadowEnabled);
            pg::SliderFloat("太陽の角直径 (度)", &rt.shadowSunAngle, 0.0f, 5.0f, "%.2f", nullptr,
                            "0 で完全なハードシャドウ（ノイズゼロ）。実際の太陽は 0.53 度。"
                            "大きくすると半影が出ますが 1px 1 レイなのでノイズが増えます");
            pg::SliderFloat("法線バイアス (m)", &rt.shadowNormalBias, 0.0f, 0.2f, "%.3f", nullptr,
                            "レイ始点を法線方向へ押し出す距離。シャドウマップの深度バイアスと違い"
                            "ワールド空間の実距離なので、影が浮く現象は起きません");
            pg::SliderFloat("最大距離 (m)", &rt.shadowMaxDistance, 0.0f, 2000.0f, "%.0f", nullptr,
                            "0 で無限。短くすると遠くの遮蔽物を追わないぶん速くなります");
            pg::SliderFloat("強さ", &rt.shadowIntensity, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();

            pg::Checkbox("RT-AO", &rt.aoEnabled,
                         "環境遮蔽をレイで解きます。SSAO と違い画面外のジオメトリも遮蔽に数えるので、"
                         "カメラを回しても AO が変動しません");
            ImGui::BeginDisabled(!rt.aoEnabled);
            pg::SliderFloat("AO 半径 (m)", &rt.aoRadius, 0.05f, 10.0f, "%.2f");
            pg::SliderInt("AO レイ本数", &rt.aoRayCount, 1, 8, nullptr,
                          "1px あたりの本数。増やすほど滑らかになりますが線形に重くなります");
            pg::SliderFloat("AO 強さ", &rt.aoIntensity, 0.0f, 1.0f, "%.2f");
            pg::SliderFloat("AO コントラスト", &rt.aoPower, 0.1f, 4.0f, "%.2f");
            pg::Checkbox("SSAO と合成 (min)", &rt.aoCombineWithSsao,
                         "大きな遮蔽は RT、1px 単位の細かい皺は SSAO、と役割を分けます"
                         "（SSAO も ON にしておくこと）");
            ImGui::EndDisabled();

            ImGui::EndDisabled();

            if (ctx.dxrSupported)
            {
                ImGui::TextDisabled("TLAS %u インスタンス / 加速構造 %.1f MB",
                                    ctx.dxrInstances,
                                    static_cast<double>(ctx.dxrAsBytes) / (1024.0 * 1024.0));
                if (ctx.dxrSkippedSkinned > 0)
                    ImGui::TextDisabled("スキンド %u 体は CSM が担当（仕様）", ctx.dxrSkippedSkinned);
            }
            pg::End();
        }
    }

    // =====================================================================
    // GI（間接光）: 旧 / 新の表示と切り替え。新 = DDGI の光輸送 + 環境光の置換 + SSGI + RT 影（GI_FOUNDATION_DESIGN）。
    // 既存シーンは旧のまま開く（勝手に移行しない）。切り替えは 1 操作 = Undo 1 回。
    // =====================================================================
    if (SectionHeader(nullptr, 0, "GI（間接光）", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const bool isNew = scene->GetGiSettings().mode == GiMode::New;
        const DdgiSettings& dd = scene->GetDdgiSettings();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("GI モード");
        ImGui::SameLine(0.0f, ui::Px(10.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, isNew ? theme::Good : theme::TextMid);
        ui::PushBold();
        ImGui::TextUnformatted(gi::ModeLabel(scene->GetGiSettings().mode));
        ui::PopBold();
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, ui::Px(10.0f));
        ImGui::TextDisabled("%s", isNew ? "光の回り込みをプローブで計算" : "従来の環境光（定数 ambient + IBL）");

        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
        if (isNew)
            ImGui::TextWrapped("室内の暗がりや色のにじみを DDGI（プローブ）が受け持ちます。"
                               "旧に戻すと DDGI を切り、環境光が 0 の太陽は 0.25 に戻します。");
        else
            ImGui::TextWrapped("既存シーンの見た目のままです。新しい GI に切り替えると、DDGI の自動配置・環境光 0・"
                               "SSGI・RT 影をまとめて有効にします。Ctrl+Z で元に戻せます。");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        if (isNew)
        {
            if (!dd.enabled)
                ImGui::TextColored(theme::Warn,
                                   "DDGI が OFF のため、環境光は従来の IBL のままです。");
            else
                ImGui::TextDisabled("DDGI 格子 %d × %d × %d（%d 個）/ 間隔 %.2f m",
                                    dd.probeCountX, dd.probeCountY, dd.probeCountZ,
                                    dd.probeCountX * dd.probeCountY * dd.probeCountZ, dd.spacing);
            ImGui::Spacing();
            if (ImGui::Button("DDGI の範囲をシーンに合わせ直す##GiRefit", ImVec2(-1.0f, ui::Px(26.0f))))
                RunGiAction(scene, ctx, GiAction::RefitGrid);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("動かない物のバウンディングボックスを覆うようにプローブの格子を置き直します。\n"
                                  "物を足したり動かしたりした後に押してください（1 m 間隔・4096 個以内）。");
            if (ImGui::Button("旧に戻す##GiToLegacy", ImVec2(-1.0f, ui::Px(26.0f))))
                RunGiAction(scene, ctx, GiAction::ToLegacy);
        }
        else
        {
            ImGui::BeginDisabled(!ctx.dxrSupported);
            if (ui::PrimaryButton("新しい GI に切り替える##GiToNew", ImVec2(-1.0f, ui::Px(30.0f))))
                RunGiAction(scene, ctx, GiAction::ToNew);
            ImGui::EndDisabled();
            if (!ctx.dxrSupported)
                ImGui::TextColored(theme::Warn,
                                   "この GPU はレイトレーシング（DXR 1.1）に対応していないため使えません。");
        }
    }

    // =====================================================================
    // スカイ / IBL
    // =====================================================================
    if (SectionHeader(nullptr, 0, "スカイ / IBL"))
    {
        auto& sky = scene->GetSkyboxSettings();
        // Q2: ライティング単位（従来 / 物理）。露出（EV100）は Post Process 窓の「露出モード」。既定は従来（絵は変わらない）。
        if (pg::Begin("LightUnits"))
        {
            static const char* const kUnits[] = {"従来", "物理 (lux / cd / nit)"};
            auto& ppUnits = scene->GetPostSettings();
            int unitsIdx = (ppUnits.lightingUnits == 1) ? 1 : 0;
            if (pg::Combo("ライティング単位", &unitsIdx, kUnits, 2,
                          "従来: 強度は任意単位（点/スポットは saturate(1-d/range)^2）。\n"
                          "物理: 太陽 = lux / 点・スポット = cd の逆二乗 / 空・IBL・自己発光 = nit（シーン RT の 1.0 = 1 nit）。"
                          "露出は Post Process 窓の「露出モード」で EV100 を選ぶ"))
                ppUnits.lightingUnits = unitsIdx;
            pg::End();
        }
        if (pg::Begin("LightSky"))
        {
            pg::InputTextStr("環境マップ", sky.envMapPath, nullptr,
                             "assets 相対の .dds (TEXTURECUBE)。\n"
                             "__procedural_sky__ = エンジン内蔵のグラデーション空（既定・アセット不要）。\n"
                             "空なら IBL 無効＝従来の ambient");
            pg::SliderFloat("IBL 強度", &sky.iblIntensity, 0.0f, 3.0f, "%.2f");
            pg::SliderFloat("スカイ強度", &sky.skyboxIntensity, 0.0f, 3.0f, "%.2f");
            pg::Checkbox("背景を描く", &sky.drawSkybox, "OFF なら IBL だけ効かせて背景は塗りません");
            pg::End();
        }
        if (ImGui::Button("環境マップを適用 / 再ベイク"))
            ctx.pendingSkyboxRebake = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("パスを変えた時だけ必要です（強度と背景 ON/OFF は即反映）");
    }

    // =====================================================================
    // 大気（物理ベース。Hillaire 2020）。既定 OFF ＝ 従来の空・従来の時刻曲線のまま。
    // 値はシーン設定（AtmosphereSettings）を直接触る（スカイ / IBL の節と同じ流儀。変更はシーンの指紋が検知する）。
    // =====================================================================
    if (SectionHeader(nullptr, 0, "大気 (物理ベース)"))
    {
        auto& at = scene->GetAtmosphereSettings();
        if (pg::Begin("LightAtmosphere"))
        {
            pg::Checkbox("物理大気を使う", &at.enabled,
                         "ON: 空・遠景の霞・太陽の色と強さを物理ベースの大気（レイリー/ミー/オゾン）から決めます。\n"
                         "OFF（既定）: 従来の空と従来の時刻カーブのまま（絵は変わりません）。\n"
                         "ON にしても「スカイ / IBL」の環境マップ設定は残り、OFF に戻すとそのまま使われます");
            if (at.enabled)
            {
                static const char* const kPresets[] = {"（選んで適用）", "地球（晴天）", "火星風", "霞", "薄明"};
                int presetPick = 0;
                if (pg::Combo("プリセット", &presetPick, kPresets, 5,
                              "大気の物理パラメータをまとめて切り替えます。\n"
                              "地球: 既定 / 火星風: 薄い大気+赤い塵（夕焼けが青い）/ 霞: 遠景が白く煙る / 薄明: 日没直後の時刻+少し霞\n"
                              "有効・時刻の進め方・太陽ライトの駆動などの使い方の設定は変わりません（薄明は時刻だけ 18.35 時にします）")
                    && presetPick > 0)
                    ApplyAtmospherePreset(at, static_cast<AtmospherePreset>(presetPick - 1));

                pg::Group("時刻");
                pg::SliderFloat("時刻", &at.timeOfDay, 0.0f, 24.0f, "%.2f 時", nullptr,
                                "現地太陽時（0〜24）。春分・秋分は 6 時が日の出・18 時が日の入り。\n"
                                "太陽の向き・色・強さ・空・環境光（IBL）・影が 1 つの時刻でつながって動きます");
                {
                    const int totalMin = static_cast<int>(std::lround(at.timeOfDay * 60.0f)) % (24 * 60);
                    pg::Text("現在時刻", "%02d:%02d", totalMin / 60, totalMin % 60);
                }
                pg::SliderFloat("時間経過 (時間/秒)", &at.timeSpeed, -2.0f, 2.0f, "%.3f", nullptr,
                                "Play 中に時刻を自動で進める速さ。0 = 止める。例: 0.1 なら 240 秒で 1 日。\n"
                                "エディタ編集中は進みません（Stop で元の時刻へ戻ります）");
                pg::SliderFloat("緯度", &at.latitudeDeg, -90.0f, 90.0f, "%.1f°", nullptr,
                                "北緯が正。太陽の南中高度と日の長さが変わります");
                pg::SliderInt("日付 (年内通日)", &at.dayOfYear, 1, 366, nullptr,
                              "1〜366。81 が春分（日の出 6 時・日の入り 18 時）、172 が夏至、355 が冬至");
                pg::SliderFloat("北の向き", &at.northYawDeg, -180.0f, 180.0f, "%.0f°", nullptr,
                                "ワールドの +Z が北から時計回りに何度ずれているか。太陽が昇る方角を回します");

                pg::Group("太陽");
                static const char* const kSunModes[] = {"時刻から決める", "太陽ライトの向きを直接指定"};
                pg::Combo("太陽の向き", &at.sunMode, kSunModes, 2,
                          "時刻から: 時刻・緯度・日付から太陽の位置を計算します。\n"
                          "直接指定: 太陽ライト（DirectionalLight）の向きをそのまま使います（月は使いません）。手で向きを決めたいときに");
                pg::Checkbox("太陽ライトを大気で駆動", &at.driveSun,
                             "ON: 太陽ライトの向き（時刻から決めるとき）・色・強さを、大気の透過率から毎フレーム決めます（夕方は赤く、夜は月光）。\n"
                             "OFF: ライトは手動のままで、空だけが大気になります");
                pg::Checkbox("空を環境光へ反映", &at.driveIBL,
                             "ON: 空を環境マップ（IBL / DDGI の空の項）へ反映します。時刻が動くと数フレームに分けて焼き直します。\n"
                             "OFF: 環境光は従来の環境マップのまま");
                pg::SliderFloat("太陽照度 (lux)", &at.sunIlluminance, 0.0f, 200000.0f, "%.0f", nullptr,
                                "大気の上端での太陽の照度。地表では約 8〜10 万 lux になります（既定 128000）");
                pg::Checkbox("星", &at.drawStars, "夜に星を描きます");
                pg::Checkbox("月", &at.drawMoon, "夜に月を描き、月光を光源にします（太陽が地平線の -12° を下回ると切り替わり）");

                pg::Group("地表と遠景の霞");
                pg::Color3("地表アルベド", at.groundAlbedo, ImGuiColorEditFlags_NoInputs);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("地面の反射率。地平線の下の色と、空の多重散乱の地面反射に効きます");
                pg::Checkbox("遠景の霞 (AP)", &at.aerialPerspective,
                             "エアリアルパースペクティブ。遠くの物が大気で青白く霞みます（不透明な物の描画後に合成）");
                pg::SliderFloat("霞の開始距離 (m)", &at.apStartDepth, 0.0f, 2000.0f, "%.0f", nullptr,
                                "これより手前には霞を掛けません（既定 100 m）");
                pg::SliderFloat("霞の最大距離 (km)", &at.apMaxDistanceKm, 1.0f, 200.0f, "%.0f", nullptr,
                                "霞の計算が届く最大距離。それより遠くは端の値を使います");
                pg::SliderFloat("霞の濃さ", &at.apStrength, 0.0f, 4.0f, "%.2f", nullptr,
                                "1 = 物理どおり。演出で濃くしたり薄くしたりできます");

                // ---- 読み取り専用: いまの太陽 ----
                {
                    using namespace atmosphere;
                    Vec3d sd;
                    if (at.sunMode == 1 && sun != entt::null)
                    {
                        const XMFLOAT3 d = reg.get<DirectionalLight>(sun).direction;
                        sd = Norm({-static_cast<double>(d.x), -static_cast<double>(d.y), -static_cast<double>(d.z)});
                    }
                    else
                        sd = SunDirectionFromTime(at);
                    const double el = std::asin(std::clamp(sd.y, -1.0, 1.0)) * 180.0 / kPi;
                    double az = std::atan2(sd.x, sd.z) * 180.0 / kPi - static_cast<double>(at.northYawDeg);
                    while (az < 0.0) az += 360.0;
                    while (az >= 360.0) az -= 360.0;
                    AtmosphereSettings sl = at;
                    if (sl.sunMode == 1) sl.drawMoon = false;
                    const SkyLight L = ComputeSkyLight(sl, sd, 0.002);   // 標高 2 m（0 ちょうどだと地面に接して透過率が 0 になる）
                    pg::Group("いまの太陽（地表・読み取り）");
                    pg::Text("高度 / 方位", "%.1f° / %.0f°%s", el, az, L.isMoon ? "  （月が光源）" : "");
                    pg::Text("地表の照度", "%.0f lux", RgbLuminance(L.illuminanceGround));
                    pg::Text("光の色 (RGB)", "%.2f  %.2f  %.2f",
                             L.illuminanceGround.r / std::max(std::max(L.illuminanceGround.r, L.illuminanceGround.g), std::max(L.illuminanceGround.b, 1e-9)),
                             L.illuminanceGround.g / std::max(std::max(L.illuminanceGround.r, L.illuminanceGround.g), std::max(L.illuminanceGround.b, 1e-9)),
                             L.illuminanceGround.b / std::max(std::max(L.illuminanceGround.r, L.illuminanceGround.g), std::max(L.illuminanceGround.b, 1e-9)));
                }
            }
            pg::End();
        }
        if (at.enabled)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("物理パラメータの詳細（散乱係数・惑星半径など）は MCP の set_scene_settings で変えられます");
            ImGui::PopTextWrapPos();
        }
    }

    // =====================================================================
    // ライティング・プリセット
    // =====================================================================
    if (SectionHeader(nullptr, 0, "プリセット", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("太陽 + ポストをまとめて設定します（Undo 1 回で戻せます）");

        const float availW = ImGui::GetContentRegionAvail().x;
        const float btnW   = (availW - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;

        for (int i = 0; i < kLightingPresetCount; ++i)
        {
            const LightingPreset& p = kLightingPresets[i];
            if (i % 3 != 0) ImGui::SameLine();
            if (ImGui::Button(p.label, ImVec2(btnW, 0.0f)))
            {
                DirectionalLight sunBefore{};
                const bool hasSun = (sun != entt::null);
                if (hasSun) sunBefore = reg.get<DirectionalLight>(sun);

                // 値と式は editor/LightingPresets.h に 1 本化（MCP と同じ結果になる）
                const DirectionalLight    sunAfter  = ApplyLightingPresetToSun(p, sunBefore);
                const PostProcessSettings postBefore = scene->GetPostSettings();
                const PostProcessSettings postAfter  = ApplyLightingPresetToPost(p, postBefore);

                if (hasSun) reg.get<DirectionalLight>(sun) = sunAfter;
                scene->GetPostSettings() = postAfter;

                ctx.undoSystem.PushCommand(std::make_unique<LightingPresetCommand>(
                    &reg, hasSun ? sun : entt::null, scene,
                    sunBefore, sunAfter, postBefore, postAfter));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.tip);
        }

        ImGui::Spacing();
        if (ImGui::Button("3点ライトを置く", ImVec2(-FLT_MIN, 0.0f)))
        {
            // 選択物（無ければ原点）を囲むキー / フィル / リムの 3 灯
            XMFLOAT3 pivot{0.0f, 0.0f, 0.0f};
            if (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity)
                && reg.all_of<Transform>(ctx.selectedEntity))
                pivot = reg.get<Transform>(ctx.selectedEntity).position;

            struct Spec { const char* name; XMFLOAT3 off; f32 kelvin; f32 intensity; f32 range; };
            static const Spec kSpecs[] = {
                {"Key Light",  { 2.4f, 3.0f, -2.4f}, 5200.0f, 8.0f, 18.0f},
                {"Fill Light", {-3.0f, 1.6f, -1.8f}, 6500.0f, 3.0f, 16.0f},
                {"Rim Light",  { 0.0f, 2.6f,  3.6f}, 7500.0f, 6.0f, 14.0f},
            };

            std::vector<SpawnStudioLightsCommand::Rec> recs;
            std::vector<entt::entity> created;
            for (const Spec& s : kSpecs)
            {
                SpawnStudioLightsCommand::Rec rec;
                rec.name = s.name;
                rec.tf.position = {pivot.x + s.off.x, pivot.y + s.off.y, pivot.z + s.off.z};
                rec.pl.color     = lm::KelvinToRGB(s.kelvin);
                rec.pl.intensity = s.intensity;
                rec.pl.range     = s.range;

                const entt::entity e = reg.create();
                reg.emplace<NameTag>(e, NameTag{rec.name});
                reg.emplace<Transform>(e, rec.tf);
                reg.emplace<PointLight>(e, rec.pl);

                recs.push_back(rec);
                created.push_back(e);
            }
            ctx.undoSystem.PushCommand(std::make_unique<SpawnStudioLightsCommand>(
                &reg, std::move(recs), std::move(created)));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("選択物（無ければ原点）の周りにキー/フィル/リムのポイントライトを 3 灯置きます。");
    }

    // =====================================================================
    // シーンビューの表示 / 操作
    // =====================================================================
    if (SectionHeader(nullptr, 0, "シーンビュー", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (pg::Begin("LightDebug"))
        {
            pg::Checkbox("全ライトの影響範囲を表示", &ctx.lightWireAll,
                         "選択していないライトの range / コーンも薄いワイヤで描きます");

            // クラスタードライティング（Forward+）のデバッグ表示。
            // 1 クラスタ 128 灯の切り捨ては CPU 側からは検出できないので、
            // 「どこが上限に張り付いているか」を見るにはこれしか手が無い。
            static const char* const kClusterDebugItems[] = {
                "オフ", "ライト複雑度（ヒートマップ）", "クラスタ境界", "デカール枚数（ヒートマップ）",
            };
            int clusterDebug = static_cast<int>(ctx.clusterDebugMode);
            if (pg::Combo("クラスタデバッグ表示", &clusterDebug, kClusterDebugItems, 4,
                          "クラスタードライティングの内部状態を可視化します。\n"
                          "・ライト複雑度: そのピクセルのクラスタが評価しているライト数。\n"
                          "  青(0灯) → 緑 → 赤 と増え、【白は上限 128 灯に張り付いて\n"
                          "  無言で切り捨てが起きている】場所です。\n"
                          "・クラスタ境界: 画面の分割（16x9 タイル x 24 スライス）を市松で確認。\n"
                          "・デカール枚数: そのクラスタに割り当てられたデカール数（上限 16 で白）。\n"
                          "透視ビューのみ（2D 正射ではクラスタード自体が無効）。\n"
                          "MCP からは dx12_render_debug の lightComplexity / clusterGrid / decalCount。"))
            {
                ctx.clusterDebugMode = static_cast<u32>(clusterDebug);
            }
            pg::End();
        }
        if (ctx.clusterDebugMode == 1)
            ImGui::TextDisabled("ヒートマップ: 青=0灯 → 緑 → 赤 / 白=1クラスタ128灯に張り付き（切り捨て中）");
        ImGui::TextDisabled("L を押しながらマウス移動: 太陽の向きを直接回す");
        ImGui::TextDisabled("ライトを選択: 丸ハンドルをドラッグでコーン角 / 距離 / 向き");
    }

    ImGui::End();
}

} // namespace dx12e
