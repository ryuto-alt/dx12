#include "editor/panels/ConsolePanel.h"
#include "editor/EditorTheme.h"
#include "scripting/ScriptEngine.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace dx12e
{
namespace
{
constexpr const char* kWindowName = "コンソール";

// 重大度の 3 区分（spdlog level → 表示区分）。0=情報(trace/debug/info) 1=警告 2=エラー(err/critical)
int SeverityOf(const LogEntry& e)
{
    if (e.level >= 4) return 2;
    if (e.level == 3) return 1;
    return 0;
}

// ASCII のみ大文字小文字を無視した部分一致（UTF-8 日本語はバイト一致）
bool ContainsCI(const std::string& hay, const char* needle)
{
    if (!needle || !*needle) return true;
    const size_t nl = std::strlen(needle);
    if (hay.size() < nl) return false;
    auto low = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; };
    for (size_t i = 0; i + nl <= hay.size(); ++i)
    {
        size_t j = 0;
        while (j < nl && low(hay[i + j]) == low(needle[j])) ++j;
        if (j == nl) return true;
    }
    return false;
}

// メッセージの1行目だけ返す（複数行は詳細ペインで見る）
const char* FirstLineEnd(const std::string& s)
{
    const size_t p = s.find('\n');
    return s.c_str() + (p == std::string::npos ? s.size() : p);
}
} // namespace

void ConsolePanel::PullNewEntries()
{
    std::vector<LogEntry> fresh;
    m_cursor = Logger::ReadBuffered(m_cursor, fresh);
    if (fresh.empty()) { m_newThisFrame = false; return; }

    m_newThisFrame = true;
    for (auto& e : fresh)
    {
        if (m_focusOnError && SeverityOf(e) == 2)
            m_wantFocus = true;
        m_entries.push_back(std::move(e));
    }
    // ローカルも無制限には持たない（リング容量の2倍で頭を捨てる）
    if (m_entries.size() > 8000)
        m_entries.erase(m_entries.begin(), m_entries.begin() + (m_entries.size() - 8000));
    m_viewDirty = true;
}

void ConsolePanel::RebuildView()
{
    m_view.clear();
    m_viewCount.clear();
    m_cntInfo = m_cntWarn = m_cntError = 0;

    std::unordered_map<std::string, int> collapseMap;   // text → m_view 添字
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
    {
        const LogEntry& e = m_entries[i];
        const int sev = SeverityOf(e);
        if (sev == 0) ++m_cntInfo;
        else if (sev == 1) ++m_cntWarn;
        else ++m_cntError;

        if (sev == 0 && !m_showInfo)  continue;
        if (sev == 1 && !m_showWarn)  continue;
        if (sev == 2 && !m_showError) continue;
        if (!ContainsCI(e.text, m_filter)) continue;

        if (m_collapse)
        {
            auto it = collapseMap.find(e.text);
            if (it != collapseMap.end())
            {
                ++m_viewCount[it->second];
                m_view[it->second] = i;   // 代表は最新の発生（時刻が新しい方が有用）
                continue;
            }
            collapseMap.emplace(e.text, static_cast<int>(m_view.size()));
        }
        m_view.push_back(i);
        m_viewCount.push_back(1);
    }
    m_viewDirty = false;
}

void ConsolePanel::ClearLocal()
{
    m_entries.clear();
    m_view.clear();
    m_viewCount.clear();
    m_selectedId = 0;
    m_viewDirty  = true;
}

void ConsolePanel::Render(ScriptEngine* scriptEngine, bool isPlaying)
{
    PullNewEntries();

    // Play 開始でクリア（Unity の Clear on Play）
    if (isPlaying && !m_prevPlaying && m_clearOnPlay)
        ClearLocal();
    m_prevPlaying = isPlaying;

    // 新着エラーでタブを前面へ（Begin より前ならどこで呼んでもよい）
    if (m_wantFocus)
    {
        ImGui::SetWindowFocus(kWindowName);
        m_wantFocus = false;
    }

    if (!ImGui::Begin(kWindowName))
    {
        ImGui::End();
        return;
    }

    if (m_viewDirty) RebuildView();

    // ============ ツールバー ============
    if (ImGui::Button("クリア")) ClearLocal();
    ImGui::SameLine();
    ImGui::Checkbox("Play時クリア", &m_clearOnPlay);
    ImGui::SameLine();
    if (ImGui::Checkbox("折りたたみ", &m_collapse)) m_viewDirty = true;
    ImGui::SameLine();
    ImGui::Checkbox("自動スクロール", &m_autoScroll);
    ImGui::SameLine();
    ImGui::Checkbox("エラーで前面", &m_focusOnError);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::InputTextWithHint("##consolefilter", "検索...", m_filter, sizeof(m_filter)))
        m_viewDirty = true;
    if (m_filter[0] != '\0')
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("×##clearfilter")) { m_filter[0] = '\0'; m_viewDirty = true; }
    }

    // 重大度トグル（右寄せ・件数バッジ。OFF はグレーで沈める）
    {
        auto sevToggle = [&](const char* label, int count, bool* on, const ImVec4& col)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s %d", label, count > 999 ? 999 : count);
            ImGui::PushStyleColor(ImGuiCol_Text, *on ? col : theme::TextFaint);
            ImGui::PushStyleColor(ImGuiCol_Button, *on ? theme::GroupBg : theme::PanelBg);
            if (ImGui::Button(buf)) { *on = !*on; m_viewDirty = true; }
            ImGui::PopStyleColor(2);
        };
        // 右端から3個分を逆算して配置（入り切らない時は成り行きで続ける）
        const float w1 = 78.0f;
        ImGui::SameLine();
        const float rightStart = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
                               - (w1 * 3.0f + 8.0f);
        if (rightStart > ImGui::GetCursorPosX())
            ImGui::SetCursorPosX(rightStart);
        sevToggle("情報",   m_cntInfo,  &m_showInfo,  theme::TextMid);
        ImGui::SameLine();
        sevToggle("警告",   m_cntWarn,  &m_showWarn,  theme::Warn);
        ImGui::SameLine();
        sevToggle("エラー", m_cntError, &m_showError, theme::Bad);
    }

    ImGui::Separator();

    // ============ 高さ配分（リスト / 詳細ペイン / Lua入力行） ============
    const float inputH  = ImGui::GetFrameHeightWithSpacing();
    const bool  hasSel  = (m_selectedId != 0);
    const float detailH = hasSel ? 110.0f : 0.0f;
    const float listH   = ImGui::GetContentRegionAvail().y - detailH - inputH - 4.0f;

    // ============ メッセージリスト ============
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::AppBg);
    ImGui::BeginChild("##consolelist", ImVec2(0, listH), ImGuiChildFlags_None);
    {
        const float rowH   = ImGui::GetTextLineHeight() + 6.0f;
        ImDrawList* dl     = ImGui::GetWindowDrawList();
        const float timeW  = ImGui::CalcTextSize("00:00:00").x;
        const float badgeW = ImGui::CalcTextSize("エラー").x;

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_view.size()), rowH);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const LogEntry& e = m_entries[m_view[row]];
                const int sev = SeverityOf(e);
                const bool selected = (e.id == m_selectedId);

                ImGui::PushID(row);
                const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                // ゼブラ縞（偶数行をわずかに持ち上げる）
                if ((row & 1) != 0)
                    dl->AddRectFilled(rowMin,
                        ImVec2(rowMin.x + ImGui::GetContentRegionAvail().x, rowMin.y + rowH),
                        ImGui::ColorConvertFloat4ToU32(theme::Hex(0xffffff, 0.025f)));

                if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None, ImVec2(0, rowH)))
                    m_selectedId = selected ? 0 : e.id;   // 再クリックで閉じる

                // 行の中身は DrawList で重ね描き（時刻=薄 / バッジ=重大度色 / 本文）
                const float ty = rowMin.y + 3.0f;
                float x = rowMin.x + 6.0f;
                dl->AddText(ImVec2(x, ty),
                    ImGui::ColorConvertFloat4ToU32(theme::TextFaint), e.time.c_str());
                x += timeW + 12.0f;

                static const char* kBadge[3] = { "情報", "警告", "エラー" };
                static const ImVec4* kBadgeCol[3] = { &theme::TextDim, &theme::Warn, &theme::Bad };
                ImVec4 badgeCol = *kBadgeCol[sev];
                if (sev == 0 && e.level <= 1) badgeCol = theme::TextFaint;   // debug/trace はさらに薄く
                dl->AddText(ImVec2(x, ty),
                    ImGui::ColorConvertFloat4ToU32(badgeCol), kBadge[sev]);
                x += badgeW + 12.0f;

                const ImVec4 textCol = (sev == 2) ? theme::Bad
                                     : (sev == 1) ? theme::Warn
                                     : (e.level <= 1 ? theme::TextDim : theme::TextMid);
                dl->AddText(ImVec2(x, ty),
                    ImGui::ColorConvertFloat4ToU32(textCol),
                    e.text.c_str(), FirstLineEnd(e.text));

                // 折りたたみ件数バッジ（右端）
                if (m_collapse && m_viewCount[row] > 1)
                {
                    char cnt[16];
                    std::snprintf(cnt, sizeof(cnt), "x%d", m_viewCount[row]);
                    const float cw = ImGui::CalcTextSize(cnt).x;
                    const float rx = rowMin.x + ImGui::GetContentRegionAvail().x - cw - 10.0f;
                    dl->AddText(ImVec2(rx, ty),
                        ImGui::ColorConvertFloat4ToU32(theme::AccentLight), cnt);
                }
                ImGui::PopID();
            }
        }
        clipper.End();

        // 自動スクロール: 末尾付近に居るときだけ追従（上へスクロール中は邪魔しない）
        if (m_autoScroll && m_newThisFrame &&
            ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - rowH * 2.0f)
            ImGui::SetScrollHereY(1.0f);

        if (m_view.empty())
            ImGui::TextColored(theme::TextFaint,
                m_entries.empty() ? "  ログはまだありません"
                                  : "  フィルタに一致するログがありません");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ============ 詳細ペイン（選択時のみ） ============
    if (hasSel)
    {
        const LogEntry* sel = nullptr;
        for (const auto& e : m_entries)
            if (e.id == m_selectedId) { sel = &e; break; }

        if (sel)
        {
            ImGui::BeginChild("##consoledetail", ImVec2(0, detailH), ImGuiChildFlags_Borders);
            const int sev = SeverityOf(*sel);
            static const char* kBadge[3] = { "情報", "警告", "エラー" };
            static const ImVec4* kCol[3] = { &theme::TextDim, &theme::Warn, &theme::Bad };
            ImGui::TextColored(*kCol[sev], "%s", kBadge[sev]);
            ImGui::SameLine();
            ImGui::TextColored(theme::TextFaint, "%s", sel->time.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("コピー"))
                ImGui::SetClipboardText(sel->text.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("閉じる"))
                m_selectedId = 0;

            // 全文（読み取り専用の複数行テキスト = 部分選択コピーもできる）
            ImGui::InputTextMultiline("##detailtext",
                const_cast<char*>(sel->text.c_str()), sel->text.size() + 1,
                ImVec2(-1.0f, -1.0f), ImGuiInputTextFlags_ReadOnly);
            ImGui::EndChild();
        }
        else
        {
            m_selectedId = 0;   // クリア等で消えた
        }
    }

    // ============ Lua 即時実行行（Unreal のコンソール入力相当） ============
    {
        ImGui::TextColored(theme::Accent, "＞");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        const bool entered = ImGui::InputTextWithHint("##luainput",
            "Lua を実行... (例: log(scene:findEntity(\"Player\"):getPosition().y))",
            m_luaInput, sizeof(m_luaInput), ImGuiInputTextFlags_EnterReturnsTrue);
        if (entered && m_luaInput[0] != '\0')
        {
            const std::string code = m_luaInput;
            Logger::Info("> {}", code);
            if (scriptEngine)
            {
                std::string result, err;
                if (scriptEngine->EvalLua(code, result, err))
                {
                    if (!result.empty()) Logger::Info("= {}", result);
                }
                else
                {
                    Logger::Error("[Lua] {}", err.empty() ? "実行に失敗しました" : err);
                }
            }
            else
            {
                Logger::Warn("Lua 実行環境が初期化されていません");
            }
            m_luaInput[0] = '\0';
            ImGui::SetKeyboardFocusHere(-1);   // 連続入力できるようフォーカス維持
        }
    }

    ImGui::End();
}

} // namespace dx12e
