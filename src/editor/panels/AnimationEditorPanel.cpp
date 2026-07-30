#include "editor/panels/AnimationEditorPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <imgui.h>

#include "ecs/Components.h"
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "ui/UiAnimRuntime.h"

namespace fs = std::filesystem;

namespace dx12e
{
namespace
{

constexpr f32 kTrackListW = 250.0f;   // 左のトラック一覧の幅
constexpr f32 kRulerH     = 24.0f;    // 時間目盛りの高さ
constexpr f32 kRowH       = 22.0f;    // トラック1行の高さ
constexpr f32 kKeyR       = 5.0f;     // キーのひし形の半径（px）
constexpr f32 kKeyGrabPx  = 7.0f;     // キーを掴める距離（px）

const char* EntityLabel(const entt::registry& reg, entt::entity e)
{
    if (e == entt::null || !reg.valid(e)) return "(なし)";
    const auto* n = reg.try_get<NameTag>(e);
    return (n && !n->name.empty()) ? n->name.c_str() : "(無名)";
}

// トラックの表示名。target が空なら「(ルート)」と出して自分自身であることを明示する。
std::string TrackLabel(const UiAnimTrack& tr)
{
    const std::string who = tr.target.empty() ? "(ルート)" : tr.target;
    return who + " / " + UiAnimPropName(tr.prop);
}

// 値をスナップ間隔へ丸める（step<=0 ならそのまま）。
f32 SnapTo(f32 v, f32 step)
{
    if (step <= 0.0f) return v;
    return std::round(v / step) * step;
}

// ひし形（キーの図形）。選択中は塗りを明るく、外周を白にする。
void DrawKeyDiamond(ImDrawList* dl, ImVec2 c, bool selected, bool stepProp)
{
    const ImU32 fill = selected ? IM_COL32(255, 220, 120, 255) : IM_COL32(210, 170, 70, 255);
    const ImU32 edge = selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(40, 40, 40, 255);
    if (stepProp)
    {
        // step プロパティは「補間しない」ことが図形で分かるよう四角にする
        const ImVec2 a(c.x - kKeyR, c.y - kKeyR), b(c.x + kKeyR, c.y + kKeyR);
        dl->AddRectFilled(a, b, fill, 1.0f);
        dl->AddRect(a, b, edge, 1.0f);
        return;
    }
    const ImVec2 pts[4] = {{c.x, c.y - kKeyR}, {c.x + kKeyR, c.y}, {c.x, c.y + kKeyR}, {c.x - kKeyR, c.y}};
    dl->AddConvexPolyFilled(pts, 4, fill);
    dl->AddPolyline(pts, 4, edge, ImDrawFlags_Closed, 1.0f);
}

} // namespace

// ---------------------------------------------------------------------------
// アセット IO
// ---------------------------------------------------------------------------
void AnimationEditorPanel::RefreshAssetList(const std::string& assetsDir)
{
    m_assetNames.clear();
    const std::string dir = assetsDir + "uianim/";
    std::error_code ec;
    if (fs::exists(dir, ec))
    {
        for (auto& entry : fs::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".uianim") continue;
            m_assetNames.push_back(entry.path().stem().string());
        }
        std::sort(m_assetNames.begin(), m_assetNames.end());
    }
    m_assetListLoaded = true;
}

void AnimationEditorPanel::NewAsset()
{
    m_clip = UiAnimClip{};
    m_clip.name = "NewUiAnim";
    m_clip.duration = 1.0f;
    m_currentPath.clear();
    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", m_clip.name.c_str());
    m_time = 0.0f;
    m_playing = m_recording = false;
    m_selTrack = m_selKey = -1;
    m_undoStack.clear();
    m_redoStack.clear();
}

bool AnimationEditorPanel::LoadAsset(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
    UiAnimClip clip;
    if (!ParseUiAnimClip(bytes, clip)) return false;

    if (clip.name.empty()) clip.name = fs::path(path).stem().string();
    m_clip = std::move(clip);
    m_currentPath = path;
    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", m_clip.name.c_str());
    m_time = 0.0f;
    m_playing = m_recording = false;
    m_selTrack = m_selKey = -1;
    m_undoStack.clear();
    m_redoStack.clear();
    return true;
}

bool AnimationEditorPanel::SaveAsset(const std::string& path, UiAnimRuntime* runtime)
{
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    SortUiAnimClip(m_clip);
    f << SerializeUiAnimClip(m_clip);
    f.close();

    // 保存したクリップを再生中のエンティティへ即反映（ホットリロード）。
    // ランタイムのキャッシュキーは assets 相対なので、絶対パスから "uianim/名前.uianim" を作る。
    if (runtime) runtime->Invalidate("uianim/" + fs::path(path).filename().string());
    return true;
}

// ---------------------------------------------------------------------------
// Undo / Redo（クリップ丸ごとのスナップショット）
// ---------------------------------------------------------------------------
void AnimationEditorPanel::PushUndo()
{
    m_undoStack.push_back(m_clip);
    if (m_undoStack.size() > kUndoLimit) m_undoStack.erase(m_undoStack.begin());
    m_redoStack.clear();   // 新しい編集をしたら redo は捨てる（一般的な分岐なし方式）
}

void AnimationEditorPanel::Undo()
{
    if (m_undoStack.empty()) return;
    m_redoStack.push_back(m_clip);
    m_clip = m_undoStack.back();
    m_undoStack.pop_back();
    m_selTrack = m_selKey = -1;
}

void AnimationEditorPanel::Redo()
{
    if (m_redoStack.empty()) return;
    m_undoStack.push_back(m_clip);
    m_clip = m_redoStack.back();
    m_redoStack.pop_back();
    m_selTrack = m_selKey = -1;
}

// ---------------------------------------------------------------------------
// 編集操作
// ---------------------------------------------------------------------------
void AnimationEditorPanel::SetKeyAt(entt::registry& reg, size_t trackIdx, f32 time, f32 value)
{
    if (trackIdx >= m_clip.tracks.size()) return;
    auto& tr = m_clip.tracks[trackIdx];

    // 同時刻(1ms 以内)のキーがあれば値だけ差し替える。キーが増殖しないようにする
    for (auto& k : tr.keys)
    {
        if (std::fabs(k.time - time) < 0.001f) { k.value = value; return; }
    }
    // 直前のキーのイージングを引き継ぐ（打つたびに既定へ戻ると作業しづらい）
    int easing = 2;
    for (const auto& k : tr.keys)
        if (k.time < time) easing = k.easing;

    tr.keys.push_back({time, value, easing});
    std::stable_sort(tr.keys.begin(), tr.keys.end(),
                     [](const UiAnimKey& a, const UiAnimKey& b) { return a.time < b.time; });
    (void)reg;
}

void AnimationEditorPanel::ApplyAtTime(entt::registry& reg, f32 time)
{
    if (m_root == entt::null || !reg.valid(m_root)) return;
    for (const auto& tr : m_clip.tracks)
    {
        const entt::entity target = UiAnimRuntime::ResolveTarget(reg, m_root, tr.target);
        if (target == entt::null) continue;
        UiAnimRuntime::WriteProp(reg, target, tr.prop, EvaluateUiAnimTrack(tr, time));
    }
}

void AnimationEditorPanel::CaptureRecordedEdits(entt::registry& reg)
{
    if (m_root == entt::null || !reg.valid(m_root)) return;

    // 「今の実際の値」と「クリップの評価値」を比べ、ずれていたらキーを打つ。
    // こうしておくと Inspector でも UI エディタでもドラッグでも、どこで値を変えても拾える
    // （エディタ側にフックを仕込まなくてよい）。
    for (size_t i = 0; i < m_clip.tracks.size(); ++i)
    {
        const auto& tr = m_clip.tracks[i];
        const entt::entity target = UiAnimRuntime::ResolveTarget(reg, m_root, tr.target);
        if (target == entt::null) continue;

        const f32 actual = UiAnimRuntime::ReadProp(reg, target, tr.prop);
        const f32 evaluated = EvaluateUiAnimTrack(tr, m_time);
        // 相対許容: 位置(px, 数百)と正規化値(0..1)を同じ絶対閾値では判定できない
        const f32 tol = 1e-4f * (1.0f + std::fabs(actual) + std::fabs(evaluated));
        if (std::fabs(actual - evaluated) > tol)
        {
            PushUndo();
            SetKeyAt(reg, i, m_time, actual);
        }
    }
}

void AnimationEditorPanel::SnapshotTargets(entt::registry& reg)
{
    m_snapshot.clear();
    m_snapshotValid = false;
    if (m_root == entt::null || !reg.valid(m_root)) return;

    for (const auto& tr : m_clip.tracks)
    {
        const entt::entity target = UiAnimRuntime::ResolveTarget(reg, m_root, tr.target);
        if (target == entt::null) continue;
        // 同じ (entity, prop) が複数トラックにあることは通常ないが、あっても最初のものを正とする
        const bool dup = std::any_of(m_snapshot.begin(), m_snapshot.end(),
                                     [&](const PropSnapshot& s)
                                     { return s.e == target && s.prop == tr.prop; });
        if (dup) continue;
        m_snapshot.push_back({target, tr.prop, UiAnimRuntime::ReadProp(reg, target, tr.prop)});
    }
    m_snapshotValid = true;
}

void AnimationEditorPanel::RestoreTargets(entt::registry& reg)
{
    if (!m_snapshotValid) return;
    for (const auto& s : m_snapshot)
        UiAnimRuntime::WriteProp(reg, s.e, s.prop, s.value);
    m_snapshot.clear();
    m_snapshotValid = false;
}

// ---------------------------------------------------------------------------
// ウィンドウ本体
// ---------------------------------------------------------------------------
void AnimationEditorPanel::RenderWindow(entt::registry& reg, EditorContext& ctx,
                                        const std::string& assetsDir, UiAnimRuntime* runtime)
{
    // AssetBrowser から .uianim をダブルクリックされたら開く（窓が閉じていても開き直す）
    if (!ctx.pendingOpenUiAnimPath.empty())
    {
        const std::string path = ctx.pendingOpenUiAnimPath;
        ctx.pendingOpenUiAnimPath.clear();
        ctx.showAnimEditor = true;
        LoadAsset(path);
    }
    if (!ctx.showAnimEditor)
    {
        // 窓を閉じたらスクラブで壊した値を戻す（開きっぱなしのつもりで閉じても事故らない）
        if (m_snapshotValid) RestoreTargets(reg);
        return;
    }
    if (!m_assetListLoaded) RefreshAssetList(assetsDir);
    if (m_clip.tracks.empty() && m_clip.name.empty()) NewAsset();

    ImGui::SetNextWindowSize(ImVec2(1100.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("UIアニメーション###AnimationEditorPanelFloating", &ctx.showAnimEditor,
                      ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                               | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem
                               | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
        ctx.floatingToolWindowHoveredThisFrame = true;

    DrawToolbar(reg, ctx, assetsDir, runtime);
    ImGui::Separator();

    // ---- 本体: 左トラック一覧 + 右タイムライン ----
    const f32 bottomH = 118.0f;   // 下部のキー編集欄ぶんを残す
    ImGui::BeginChild("##AnimBody", ImVec2(0.0f, -bottomH), false);
    DrawTrackList(reg, ctx);
    ImGui::SameLine(0.0f, 0.0f);
    DrawTimeline(reg, ctx);
    ImGui::EndChild();

    ImGui::Separator();
    DrawKeyInspector();

    // ---- 再生 / 録画の進行 ----
    const f32 dt = ImGui::GetIO().DeltaTime;
    if (m_playing)
    {
        m_time += dt * m_playSpeed;
        WrapUiAnimTime(m_time, m_clip.duration, m_clip.loop);
        if (!m_clip.loop && m_time >= m_clip.duration) m_playing = false;
        ApplyAtTime(reg, m_time);
    }
    else if (m_recording)
    {
        // 録画中は「時間を止めたまま値をいじる」= ポーズ録画。再生しながらの録画は
        // 値の変化がアニメ自身の適用と区別できないので許さない（m_playing と排他）。
        CaptureRecordedEdits(reg);
    }

    if (m_statusFlash > 0.0f)
    {
        m_statusFlash -= dt;
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1.0f), "%s", m_statusMsg.c_str());
    }

    // ---- パネル内ショートカット ----
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? Redo() : Undo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) { m_playing = !m_playing; m_recording = false; }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && m_selTrack >= 0 && m_selKey >= 0)
        {
            auto& keys = m_clip.tracks[static_cast<size_t>(m_selTrack)].keys;
            if (m_selKey < static_cast<int>(keys.size()))
            {
                PushUndo();
                keys.erase(keys.begin() + m_selKey);
                m_selKey = -1;
            }
        }
    }

    ImGui::End();
}

void AnimationEditorPanel::DrawToolbar(entt::registry& reg, EditorContext& ctx,
                                       const std::string& assetsDir, UiAnimRuntime* runtime)
{
    if (ImGui::Button("＋ 新規")) NewAsset();
    ImGui::SameLine();

    if (ImGui::Button("開く ▾")) ImGui::OpenPopup("##AnimOpenPopup");
    if (ImGui::BeginPopup("##AnimOpenPopup"))
    {
        if (ImGui::MenuItem("一覧を更新")) RefreshAssetList(assetsDir);
        ImGui::Separator();
        if (m_assetNames.empty()) ImGui::TextDisabled("(assets/uianim/ が空)");
        for (const auto& nm : m_assetNames)
        {
            if (ImGui::MenuItem(nm.c_str()))
            {
                if (m_snapshotValid) RestoreTargets(reg);
                LoadAsset(assetsDir + "uianim/" + nm + ".uianim");
                SnapshotTargets(reg);
            }
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    const bool canSave = m_nameBuf[0] != '\0';
    if (!canSave) ImGui::BeginDisabled();
    if (ImGui::Button("保存"))
    {
        m_clip.name = m_nameBuf;
        const std::string path = m_currentPath.empty()
            ? (assetsDir + "uianim/" + m_clip.name + ".uianim") : m_currentPath;
        if (SaveAsset(path, runtime))
        {
            m_currentPath = path;
            RefreshAssetList(assetsDir);
            m_statusMsg = "保存しました: " + m_clip.name;
            m_statusFlash = 2.0f;
        }
        else
        {
            m_statusMsg = "保存に失敗: " + path;
            m_statusFlash = 3.0f;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("名前を付けて保存"))
    {
        m_clip.name = m_nameBuf;
        const std::string path = assetsDir + "uianim/" + m_clip.name + ".uianim";
        if (SaveAsset(path, runtime))
        {
            m_currentPath = path;
            RefreshAssetList(assetsDir);
            m_statusMsg = "保存しました: " + m_clip.name;
            m_statusFlash = 2.0f;
        }
    }
    if (!canSave) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputText("##AnimName", m_nameBuf, sizeof(m_nameBuf));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    if (ImGui::DragFloat("尺(秒)", &m_clip.duration, 0.05f, 0.05f, 600.0f, "%.2f"))
        m_time = std::clamp(m_time, 0.0f, m_clip.duration);
    ImGui::SameLine();
    ImGui::Checkbox("ループ", &m_clip.loop);

    // ---- 2段目: 対象ルート / 再生 / 録画 ----
    ImGui::Text("対象ルート:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", EntityLabel(reg, m_root));
    ImGui::SameLine();
    const bool hasSel = ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity);
    if (!hasSel) ImGui::BeginDisabled();
    if (ImGui::Button("選択中を対象に"))
    {
        if (m_snapshotValid) RestoreTargets(reg);
        m_root = ctx.selectedEntity;
        SnapshotTargets(reg);
        ApplyAtTime(reg, m_time);
    }
    if (!hasSel) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("元に戻す"))
    {
        // スクラブで書き換えた値をパネルを開いた時点へ戻す（保存済みシーンを壊さない保険）
        RestoreTargets(reg);
        SnapshotTargets(reg);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("スクラブで書き換えた対象の値を、対象を選んだ時点の値へ戻す");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button(m_playing ? "⏸ 停止" : "▶ 再生"))
    {
        m_playing = !m_playing;
        if (m_playing) m_recording = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("⏮ 先頭"))
    {
        m_time = 0.0f;
        m_playing = false;
        ApplyAtTime(reg, m_time);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::DragFloat("速度", &m_playSpeed, 0.05f, 0.05f, 8.0f, "%.2fx");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    // 録画: 押している間、対象の値変更が自動でキーになる
    if (m_recording) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.18f, 0.18f, 1.0f));
    if (ImGui::Button(m_recording ? "● 録画中" : "○ 録画"))
    {
        m_recording = !m_recording;
        if (m_recording) m_playing = false;
    }
    if (m_recording) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON の間、Inspector や UI エディタで対象の値を変えると\n"
                          "再生ヘッドの位置に自動でキーが打たれる（時間は止まったまま）");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("スナップ", &m_snapStep, 0.005f, 0.0f, 1.0f, "%.3fs");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("キーと再生ヘッドを丸める間隔。0 でスナップ無し");
}

void AnimationEditorPanel::DrawTrackList(entt::registry& reg, EditorContext& ctx)
{
    ImGui::BeginChild("##AnimTracks", ImVec2(kTrackListW, 0.0f), true);

    if (ImGui::Button("＋ トラック追加", ImVec2(-1.0f, 0.0f)))
        ImGui::OpenPopup("##AddTrackPopup");
    DrawAddTrackPopup(reg, ctx);

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, kRulerH - ImGui::GetStyle().ItemSpacing.y));   // 目盛りぶん行を揃える

    for (size_t i = 0; i < m_clip.tracks.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        const auto& tr = m_clip.tracks[i];

        // 対象が解決できないトラックは赤く出す（要素をリネーム/削除したのに気付けるように）
        const entt::entity target = (m_root != entt::null && reg.valid(m_root))
            ? UiAnimRuntime::ResolveTarget(reg, m_root, tr.target) : entt::null;
        const bool broken = (target == entt::null);
        if (broken) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));

        const std::string label = TrackLabel(tr);
        if (ImGui::Selectable(label.c_str(), m_selTrack == static_cast<int>(i),
                              0, ImVec2(0.0f, kRowH)))
        {
            m_selTrack = static_cast<int>(i);
            m_selKey = -1;
        }
        if (broken) ImGui::PopStyleColor();
        if (broken && ImGui::IsItemHovered())
            ImGui::SetTooltip("対象が見つからない: \"%s\"\n要素名を変えた/消した可能性がある",
                              tr.target.c_str());

        if (ImGui::BeginPopupContextItem("##TrackCtx"))
        {
            if (ImGui::MenuItem("現在値をキーにする"))
            {
                if (target != entt::null)
                {
                    PushUndo();
                    SetKeyAt(reg, i, SnapTo(m_time, m_snapStep),
                             UiAnimRuntime::ReadProp(reg, target, tr.prop));
                }
            }
            if (ImGui::MenuItem("トラックを削除"))
            {
                PushUndo();
                m_clip.tracks.erase(m_clip.tracks.begin() + static_cast<ptrdiff_t>(i));
                m_selTrack = m_selKey = -1;
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    if (m_clip.tracks.empty())
        ImGui::TextDisabled("トラックが無い。\n対象を選んで\n「＋ トラック追加」から\nプロパティを選ぶ。");

    ImGui::EndChild();
}

void AnimationEditorPanel::DrawAddTrackPopup(entt::registry& reg, EditorContext& ctx)
{
    if (!ImGui::BeginPopup("##AddTrackPopup")) return;

    const entt::entity sel = ctx.selectedEntity;
    std::string path;
    const bool ok = (m_root != entt::null && reg.valid(m_root) && sel != entt::null
                     && reg.valid(sel) && UiAnimRuntime::MakeTargetPath(reg, m_root, sel, path));

    if (!ok)
    {
        ImGui::TextDisabled("対象ルート配下の要素を\nヒエラルキー/UIエディタで選んでから開いてや。");
        ImGui::EndPopup();
        return;
    }

    ImGui::Text("対象: %s", path.empty() ? "(ルート)" : path.c_str());
    ImGui::Separator();
    for (int p = 0; p < kUiPropCount; ++p)
    {
        // 既に同じ (target, prop) のトラックがあるなら二重に足させない
        const bool exists = std::any_of(m_clip.tracks.begin(), m_clip.tracks.end(),
                                        [&](const UiAnimTrack& t)
                                        { return t.target == path && t.prop == p; });
        if (exists) ImGui::BeginDisabled();
        if (ImGui::MenuItem(UiAnimPropName(p)))
        {
            PushUndo();
            UiAnimTrack tr;
            tr.target = path;
            tr.prop   = p;
            // 追加と同時に現在値のキーを 1 本打つ（空トラックのままだと何も起きず戸惑うため）
            tr.keys.push_back({SnapTo(m_time, m_snapStep), UiAnimRuntime::ReadProp(reg, sel, p), 2});
            m_clip.tracks.push_back(std::move(tr));
            m_selTrack = static_cast<int>(m_clip.tracks.size()) - 1;
            m_selKey = 0;
            // ★先に復元してからスナップし直す。他の呼び出し元（対象切替 / Open）は必ずそうしている。
            //   スクラブは実コンポーネントへ直接書く（ApplyAtTime → WriteProp）ので、
            //   復元せずにここでスナップすると **t=0.6 の途中ポーズが「元の値」になる**。
            //   窓を閉じる / 「元に戻す」でそれが復元され、Ctrl+S でシーンに焼き付く
            //   （パネルがずれたまま / 半透明のまま / visible=false のまま保存される）。
            if (m_snapshotValid) RestoreTargets(reg);
            SnapshotTargets(reg);
        }
        if (exists) ImGui::EndDisabled();
    }
    ImGui::EndPopup();
}

void AnimationEditorPanel::DrawTimeline(entt::registry& reg, EditorContext& /*ctx*/)
{
    ImGui::BeginChild("##AnimTimeline", ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const f32 avail = (std::max)(60.0f, ImGui::GetContentRegionAvail().x);
    const f32 rowsH = kRulerH + kRowH * static_cast<f32>(m_clip.tracks.size()) + 8.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const f32 dur = (std::max)(0.05f, m_clip.duration);
    const f32 pxPerSec = (avail / dur) * m_zoom;
    auto timeToX = [&](f32 t) { return origin.x + (t - m_scroll) * pxPerSec; };
    auto xToTime = [&](f32 x) { return (x - origin.x) / pxPerSec + m_scroll; };

    // 操作を受け取る透明ボタン（先に置いて、描画をこの上に重ねる）
    ImGui::InvisibleButton("##TimelineCanvas", ImVec2(avail, (std::max)(rowsH, 60.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool canvasHovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // --- 背景 ---
    dl->AddRectFilled(origin, ImVec2(origin.x + avail, origin.y + rowsH), IM_COL32(28, 28, 32, 255));
    // クリップ範囲外を暗くして「尺」を視覚化する
    const f32 x0 = timeToX(0.0f), x1 = timeToX(dur);
    dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + rowsH), IM_COL32(38, 38, 44, 255));

    // --- 目盛り ---
    // 1目盛りが 60px を下回らない範囲で 0.05/0.1/0.25/0.5/1/2/5/10 秒から選ぶ
    static const f32 kSteps[] = {0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f};
    f32 step = kSteps[0];
    for (f32 s : kSteps) { step = s; if (s * pxPerSec >= 60.0f) break; }
    for (f32 t = 0.0f; t <= dur + step * 0.5f; t += step)
    {
        const f32 x = timeToX(t);
        if (x < origin.x - 40.0f || x > origin.x + avail + 40.0f) continue;
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + rowsH), IM_COL32(70, 70, 80, 160));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2fs", t);
        dl->AddText(ImVec2(x + 3.0f, origin.y + 3.0f), IM_COL32(160, 160, 175, 255), buf);
    }
    dl->AddLine(ImVec2(origin.x, origin.y + kRulerH), ImVec2(origin.x + avail, origin.y + kRulerH),
                IM_COL32(90, 90, 100, 255));

    // --- トラック行とキー ---
    int hitTrack = -1, hitKey = -1;
    for (size_t i = 0; i < m_clip.tracks.size(); ++i)
    {
        const f32 rowY = origin.y + kRulerH + kRowH * static_cast<f32>(i);
        const f32 cy = rowY + kRowH * 0.5f;
        if (m_selTrack == static_cast<int>(i))
            dl->AddRectFilled(ImVec2(origin.x, rowY), ImVec2(origin.x + avail, rowY + kRowH),
                              IM_COL32(60, 70, 95, 120));
        dl->AddLine(ImVec2(origin.x, rowY + kRowH), ImVec2(origin.x + avail, rowY + kRowH),
                    IM_COL32(55, 55, 62, 255));

        const auto& tr = m_clip.tracks[i];
        const bool stepProp = UiAnimPropIsStep(tr.prop);

        // キー間を線で結ぶ（どこからどこまで動くかが一目で分かる）
        for (size_t k = 0; k + 1 < tr.keys.size(); ++k)
        {
            dl->AddLine(ImVec2(timeToX(tr.keys[k].time), cy),
                        ImVec2(timeToX(tr.keys[k + 1].time), cy), IM_COL32(150, 130, 70, 180), 2.0f);
        }
        for (size_t k = 0; k < tr.keys.size(); ++k)
        {
            const ImVec2 c(timeToX(tr.keys[k].time), cy);
            if (c.x < origin.x - 20.0f || c.x > origin.x + avail + 20.0f) continue;
            const bool sel = (m_selTrack == static_cast<int>(i) && m_selKey == static_cast<int>(k));
            DrawKeyDiamond(dl, c, sel, stepProp);
            if (canvasHovered && std::fabs(mouse.x - c.x) <= kKeyGrabPx
                && std::fabs(mouse.y - c.y) <= kKeyGrabPx)
            {
                hitTrack = static_cast<int>(i);
                hitKey   = static_cast<int>(k);
            }
        }
    }

    // --- イベントマーカー ---
    for (const auto& ev : m_clip.events)
    {
        const f32 x = timeToX(ev.time);
        dl->AddTriangleFilled(ImVec2(x, origin.y + kRulerH - 8.0f), ImVec2(x - 5.0f, origin.y + kRulerH),
                              ImVec2(x + 5.0f, origin.y + kRulerH), IM_COL32(120, 220, 140, 255));
    }

    // --- 再生ヘッド ---
    {
        const f32 x = timeToX(m_time);
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + rowsH), IM_COL32(255, 90, 90, 230), 2.0f);
        dl->AddTriangleFilled(ImVec2(x - 6.0f, origin.y), ImVec2(x + 6.0f, origin.y),
                              ImVec2(x, origin.y + 9.0f), IM_COL32(255, 90, 90, 255));
    }

    // --- 入力 ---
    if (canvasHovered)
    {
        // ホイールでズーム（カーソル位置の時刻を固定したまま拡大 = 見ている場所が逃げない）
        const f32 wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const f32 tAtCursor = xToTime(mouse.x);
            m_zoom = std::clamp(m_zoom * ((wheel > 0.0f) ? 1.15f : 1.0f / 1.15f), 0.25f, 40.0f);
            const f32 newPxPerSec = (avail / dur) * m_zoom;
            m_scroll = tAtCursor - (mouse.x - origin.x) / newPxPerSec;
        }
        // 中ドラッグで横スクロール
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            m_scroll -= ImGui::GetIO().MouseDelta.x / pxPerSec;

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (hitKey >= 0)
            {
                m_selTrack = hitTrack;
                m_selKey   = hitKey;
                m_dragTrack = hitTrack;
                m_dragKey   = hitKey;
            }
            else if (mouse.y < origin.y + kRulerH)
            {
                m_draggingPlayhead = true;   // 目盛り帯のクリックは再生ヘッド移動
                m_playing = false;
            }
            else
            {
                // トラック行の空きをクリック = 再生ヘッドをそこへ（キー打ちは Ctrl+クリック）
                const int row = static_cast<int>((mouse.y - origin.y - kRulerH) / kRowH);
                if (ImGui::GetIO().KeyCtrl && row >= 0 && row < static_cast<int>(m_clip.tracks.size()))
                {
                    const f32 t = std::clamp(SnapTo(xToTime(mouse.x), m_snapStep), 0.0f, dur);
                    const auto& tr = m_clip.tracks[static_cast<size_t>(row)];
                    const entt::entity target = UiAnimRuntime::ResolveTarget(reg, m_root, tr.target);
                    PushUndo();
                    SetKeyAt(reg, static_cast<size_t>(row), t,
                             (target != entt::null)
                                 ? UiAnimRuntime::ReadProp(reg, target, tr.prop)
                                 : EvaluateUiAnimTrack(tr, t));
                    m_selTrack = row;
                }
                else
                {
                    m_draggingPlayhead = true;
                    m_playing = false;
                }
                if (row >= 0 && row < static_cast<int>(m_clip.tracks.size())) m_selTrack = row;
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hitKey >= 0)
        {
            m_selTrack = hitTrack;
            m_selKey   = hitKey;
            ImGui::OpenPopup("##KeyCtx");
        }
    }

    // キーのドラッグ（時刻の移動）
    if (m_dragKey >= 0)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            auto& keys = m_clip.tracks[static_cast<size_t>(m_dragTrack)].keys;
            if (m_dragKey < static_cast<int>(keys.size()))
            {
                const f32 t = std::clamp(SnapTo(xToTime(mouse.x), m_snapStep), 0.0f, dur);
                keys[static_cast<size_t>(m_dragKey)].time = t;
                // 並び替えで index がずれるので、掴んでいるキーを追跡し直す
                const UiAnimKey moved = keys[static_cast<size_t>(m_dragKey)];
                std::stable_sort(keys.begin(), keys.end(),
                                 [](const UiAnimKey& a, const UiAnimKey& b) { return a.time < b.time; });
                for (size_t k = 0; k < keys.size(); ++k)
                {
                    if (keys[k].time == moved.time && keys[k].value == moved.value)
                    {
                        m_dragKey = static_cast<int>(k);
                        m_selKey  = m_dragKey;
                        break;
                    }
                }
                ApplyAtTime(reg, m_time);   // ドラッグ中も絵が追従する
            }
        }
        else
        {
            m_dragTrack = m_dragKey = -1;
        }
    }
    else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && m_dragKey < 0 && m_draggingPlayhead)
    {
        // 掴んだ後は canvasHovered を抜けても追従させる（窓の外へ振り切っても切れない）
        m_time = std::clamp(SnapTo(xToTime(mouse.x), m_snapStep), 0.0f, dur);
        ApplyAtTime(reg, m_time);
    }
    if (m_draggingPlayhead)
    {
        m_time = std::clamp(SnapTo(xToTime(mouse.x), m_snapStep), 0.0f, dur);
        ApplyAtTime(reg, m_time);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_draggingPlayhead = false;
    }

    // キーの右クリックメニュー
    if (ImGui::BeginPopup("##KeyCtx"))
    {
        if (m_selTrack >= 0 && m_selKey >= 0
            && m_selKey < static_cast<int>(m_clip.tracks[static_cast<size_t>(m_selTrack)].keys.size()))
        {
            auto& key = m_clip.tracks[static_cast<size_t>(m_selTrack)].keys[static_cast<size_t>(m_selKey)];
            ImGui::TextDisabled("この区間のイージング");
            for (int e = 0; e < kUiEaseCount; ++e)
            {
                if (ImGui::MenuItem(UiEaseNames()[e], nullptr, key.easing == e))
                {
                    PushUndo();
                    key.easing = e;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("キーを削除"))
            {
                PushUndo();
                auto& keys = m_clip.tracks[static_cast<size_t>(m_selTrack)].keys;
                keys.erase(keys.begin() + m_selKey);
                m_selKey = -1;
            }
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

void AnimationEditorPanel::DrawKeyInspector()
{
    if (m_selTrack < 0 || m_selTrack >= static_cast<int>(m_clip.tracks.size()))
    {
        ImGui::TextDisabled("キーを選ぶと、ここで時刻・値・イージングを数値で編集できる。");
        ImGui::TextDisabled("タイムライン: 左ドラッグ=再生ヘッド / キーを掴んで移動 / "
                            "Ctrl+クリック=その位置にキー / 右クリック=イージング・削除 / "
                            "ホイール=ズーム / 中ドラッグ=横スクロール");
        return;
    }

    auto& track = m_clip.tracks[static_cast<size_t>(m_selTrack)];
    ImGui::Text("トラック: %s", TrackLabel(track).c_str());

    if (m_selKey < 0 || m_selKey >= static_cast<int>(track.keys.size()))
    {
        ImGui::TextDisabled("このトラックのキーを選んでや（キーの数: %d）",
                            static_cast<int>(track.keys.size()));
        return;
    }

    auto& key = track.keys[static_cast<size_t>(m_selKey)];
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::DragFloat("時刻", &key.time, 0.01f, 0.0f, m_clip.duration, "%.3fs"))
    {
        std::stable_sort(track.keys.begin(), track.keys.end(),
                         [](const UiAnimKey& a, const UiAnimKey& b) { return a.time < b.time; });
        m_selKey = -1;   // 並び替えで index が変わるので選択を落とす（誤編集を防ぐ）
        return;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::DragFloat("値", &key.value, 0.5f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    if (UiAnimPropIsStep(track.prop))
    {
        ImGui::TextDisabled("(step プロパティ = 補間しない)");
    }
    else
    {
        int easing = key.easing;
        if (ImGui::Combo("イージング", &easing, UiEaseNames(), kUiEaseCount))
        {
            PushUndo();
            key.easing = easing;
        }
    }

    // 区間カーブのプレビュー（次のキーがある時だけ）
    if (!UiAnimPropIsStep(track.prop) && m_selKey + 1 < static_cast<int>(track.keys.size()))
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 sz(160.0f, 42.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(24, 24, 28, 255));
        ImVec2 prev(p.x, p.y + sz.y);
        for (int s = 1; s <= 32; ++s)
        {
            const f32 u = static_cast<f32>(s) / 32.0f;
            const ImVec2 cur(p.x + sz.x * u, p.y + sz.y * (1.0f - UiEase(key.easing, u)));
            dl->AddLine(prev, cur, IM_COL32(120, 200, 255, 255), 1.5f);
            prev = cur;
        }
        ImGui::Dummy(sz);
    }
}

} // namespace dx12e
