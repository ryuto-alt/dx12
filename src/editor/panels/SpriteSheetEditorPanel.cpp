#include "editor/panels/SpriteSheetEditorPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <imgui.h>

#include "core/PathResolver.h"
#include "ecs/Components.h"
#include "editor/EditorContext.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/Texture.h"
#include "resource/ResourceManager.h"
#include "ui/UiAnimRuntime.h"

namespace fs = std::filesystem;

namespace dx12e
{
namespace
{

// テクスチャを ImTextureID へ解決する（UISystem と同じ経路。パスキーでキャッシュされる）。
ImTextureID ResolveTexture(const std::string& assetsDir, const std::string& relPath,
                           ResourceManager* resources, DescriptorHeap* srvHeap,
                           ID3D12GraphicsCommandList* cmdList, f32& outW, f32& outH)
{
    outW = outH = 0.0f;
    if (relPath.empty() || !resources || !srvHeap || !cmdList) return 0;
    Texture* tex = resources->GetOrLoadTexture(PathResolver::Utf8ToWide(assetsDir + relPath),
                                               cmdList, true);
    if (!tex) return 0;
    outW = static_cast<f32>(tex->GetWidth());
    outH = static_cast<f32>(tex->GetHeight());
    return static_cast<ImTextureID>(srvHeap->GetGpuHandle(tex->GetSrvIndex()).ptr);
}

const char* const kModeNames[] = {"ループ", "単発", "往復"};

} // namespace

// ---------------------------------------------------------------------------
// アセット IO
// ---------------------------------------------------------------------------
void SpriteSheetEditorPanel::RefreshAssetList(const std::string& assetsDir)
{
    m_assetNames.clear();
    const std::string dir = assetsDir + "spriteanim/";
    std::error_code ec;
    if (fs::exists(dir, ec))
    {
        for (auto& entry : fs::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".spranim") continue;
            m_assetNames.push_back(entry.path().stem().string());
        }
        std::sort(m_assetNames.begin(), m_assetNames.end());
    }
    m_assetListLoaded = true;
}

void SpriteSheetEditorPanel::NewAsset()
{
    m_sheet = SpriteAnimSheet{};
    m_currentPath.clear();
    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "NewSheet");
    m_selSeq = -1;
    m_previewTime = 0.0f;
}

bool SpriteSheetEditorPanel::LoadAsset(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
    SpriteAnimSheet sheet;
    if (!ParseSpriteAnimSheet(bytes, sheet)) return false;

    m_sheet = std::move(sheet);
    m_currentPath = path;
    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", fs::path(path).stem().string().c_str());
    m_selSeq = m_sheet.seqs.empty() ? -1 : 0;
    m_previewTime = 0.0f;
    return true;
}

bool SpriteSheetEditorPanel::SaveAsset(const std::string& path, UiAnimRuntime* runtime)
{
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f << SerializeSpriteAnimSheet(m_sheet);
    f.close();

    if (runtime) runtime->Invalidate("spriteanim/" + fs::path(path).filename().string());
    return true;
}

// ---------------------------------------------------------------------------
// ウィンドウ本体
// ---------------------------------------------------------------------------
void SpriteSheetEditorPanel::RenderWindow(entt::registry& reg, EditorContext& ctx,
                                          const std::string& assetsDir, ResourceManager* resources,
                                          DescriptorHeap* srvHeap,
                                          ID3D12GraphicsCommandList* cmdList, UiAnimRuntime* runtime)
{
    if (!ctx.pendingOpenSpriteSheetPath.empty())
    {
        const std::string path = ctx.pendingOpenSpriteSheetPath;
        ctx.pendingOpenSpriteSheetPath.clear();
        ctx.showSpriteSheetEditor = true;
        LoadAsset(path);
    }
    if (!ctx.showSpriteSheetEditor) return;
    if (!m_assetListLoaded) RefreshAssetList(assetsDir);

    ImGui::SetNextWindowSize(ImVec2(1080.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("スプライトシート###SpriteSheetEditorPanelFloating",
                      &ctx.showSpriteSheetEditor, ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                               | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem
                               | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
        ctx.floatingToolWindowHoveredThisFrame = true;

    DrawToolbar(ctx, assetsDir, runtime);
    ImGui::Separator();

    ImGui::BeginChild("##SprLeft", ImVec2(190.0f, 0.0f), true);
    DrawSeqList();
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##SprGrid", ImVec2(-380.0f, 0.0f), true);
    DrawSheetGrid(assetsDir, resources, srvHeap, cmdList);
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##SprSeq", ImVec2(0.0f, 0.0f), true);
    DrawSeqEditor(reg, ctx, assetsDir, resources, srvHeap, cmdList);
    ImGui::EndChild();

    if (m_previewPlaying) m_previewTime += ImGui::GetIO().DeltaTime;
    if (m_statusFlash > 0.0f) m_statusFlash -= ImGui::GetIO().DeltaTime;

    ImGui::End();
}

void SpriteSheetEditorPanel::DrawToolbar(EditorContext& /*ctx*/, const std::string& assetsDir,
                                         UiAnimRuntime* runtime)
{
    if (ImGui::Button("＋ 新規")) NewAsset();
    ImGui::SameLine();
    if (ImGui::Button("開く ▾")) ImGui::OpenPopup("##SprOpen");
    if (ImGui::BeginPopup("##SprOpen"))
    {
        if (ImGui::MenuItem("一覧を更新")) RefreshAssetList(assetsDir);
        ImGui::Separator();
        if (m_assetNames.empty()) ImGui::TextDisabled("(assets/spriteanim/ が空)");
        for (const auto& nm : m_assetNames)
            if (ImGui::MenuItem(nm.c_str()))
                LoadAsset(assetsDir + "spriteanim/" + nm + ".spranim");
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    const bool canSave = m_nameBuf[0] != '\0';
    if (!canSave) ImGui::BeginDisabled();
    if (ImGui::Button("保存"))
    {
        const std::string path = m_currentPath.empty()
            ? (assetsDir + "spriteanim/" + m_nameBuf + ".spranim") : m_currentPath;
        if (SaveAsset(path, runtime))
        {
            m_currentPath = path;
            RefreshAssetList(assetsDir);
            m_statusMsg = std::string("保存しました: ") + m_nameBuf;
            m_statusFlash = 2.0f;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("名前を付けて保存"))
    {
        const std::string path = assetsDir + "spriteanim/" + m_nameBuf + ".spranim";
        if (SaveAsset(path, runtime))
        {
            m_currentPath = path;
            RefreshAssetList(assetsDir);
            m_statusMsg = std::string("保存しました: ") + m_nameBuf;
            m_statusFlash = 2.0f;
        }
    }
    if (!canSave) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputText("##SprName", m_nameBuf, sizeof(m_nameBuf));

    // テクスチャは assets 相対で持つ。アセットブラウザからの D&D も受ける。
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    char texBuf[260];
    std::snprintf(texBuf, sizeof(texBuf), "%s", m_sheet.texturePath.c_str());
    if (ImGui::InputText("テクスチャ", texBuf, sizeof(texBuf))) m_sheet.texturePath = texBuf;
    if (ImGui::BeginDragDropTarget())
    {
        // AssetBrowser の payload は絶対パス。assets ルートからの相対へ直して保存する
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH"))
        {
            const std::string abs(static_cast<const char*>(p->Data), p->DataSize ? p->DataSize - 1 : 0);
            std::error_code ec;
            const fs::path rel = fs::relative(fs::path(abs), fs::path(assetsDir), ec);
            m_sheet.texturePath = ec ? abs : rel.generic_string();
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("アセットブラウザから画像をここへドラッグしてもええ");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    if (ImGui::DragInt("列", &m_sheet.cols, 0.2f, 1, 64)) m_sheet.cols = (std::max)(1, m_sheet.cols);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    if (ImGui::DragInt("行", &m_sheet.rows, 0.2f, 1, 64)) m_sheet.rows = (std::max)(1, m_sheet.rows);

    if (m_statusFlash > 0.0f)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1.0f), "%s", m_statusMsg.c_str());
    }
}

void SpriteSheetEditorPanel::DrawSeqList()
{
    ImGui::TextDisabled("シーケンス");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##NewSeqName", m_seqNameBuf, sizeof(m_seqNameBuf));
    if (ImGui::Button("＋ 追加", ImVec2(-1.0f, 0.0f)) && m_seqNameBuf[0] != '\0')
    {
        SpriteAnimSeq s;
        s.name = m_seqNameBuf;
        m_sheet.seqs.push_back(std::move(s));
        m_selSeq = static_cast<int>(m_sheet.seqs.size()) - 1;
        m_previewTime = 0.0f;
    }
    ImGui::Separator();

    for (size_t i = 0; i < m_sheet.seqs.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        const auto& s = m_sheet.seqs[i];
        char label[96];
        std::snprintf(label, sizeof(label), "%s (%d)",
                      s.name.empty() ? "(無名)" : s.name.c_str(), static_cast<int>(s.frames.size()));
        if (ImGui::Selectable(label, m_selSeq == static_cast<int>(i)))
        {
            m_selSeq = static_cast<int>(i);
            m_previewTime = 0.0f;
        }
        if (ImGui::BeginPopupContextItem("##SeqCtx"))
        {
            if (ImGui::MenuItem("複製"))
            {
                SpriteAnimSeq copy = s;
                copy.name += "_copy";
                m_sheet.seqs.push_back(std::move(copy));
            }
            if (ImGui::MenuItem("削除"))
            {
                m_sheet.seqs.erase(m_sheet.seqs.begin() + static_cast<ptrdiff_t>(i));
                m_selSeq = -1;
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (m_sheet.seqs.empty()) ImGui::TextDisabled("(なし)");
}

void SpriteSheetEditorPanel::DrawSheetGrid(const std::string& assetsDir, ResourceManager* resources,
                                           DescriptorHeap* srvHeap,
                                           ID3D12GraphicsCommandList* cmdList)
{
    f32 texW = 0.0f, texH = 0.0f;
    const ImTextureID texId = ResolveTexture(assetsDir, m_sheet.texturePath, resources, srvHeap,
                                             cmdList, texW, texH);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("拡大", &m_gridZoom, 0.25f, 4.0f, "%.2fx");
    ImGui::SameLine();
    ImGui::Checkbox("番号", &m_showCellIndex);
    ImGui::SameLine();
    if (ImGui::Button("行×列を推定") && texW > 0.0f && texH > 0.0f)
    {
        // 正方形セルを仮定して縦横比から推定する。等分割シートなら大抵これで当たる。
        // 外れた時に手で直せるよう、あくまで初期値の当てずっぽうという位置づけ。
        const f32 ratio = texW / texH;
        int bestC = m_sheet.cols, bestR = m_sheet.rows;
        f32 bestErr = 1e9f;
        for (int r = 1; r <= 16; ++r)
        {
            const int c = static_cast<int>(std::lround(ratio * static_cast<f32>(r)));
            if (c < 1 || c > 64) continue;
            const f32 err = std::fabs(ratio - static_cast<f32>(c) / static_cast<f32>(r));
            if (err < bestErr) { bestErr = err; bestC = c; bestR = r; }
        }
        m_sheet.cols = bestC;
        m_sheet.rows = bestR;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("セルが正方形だと仮定して縦横比から行列数を当てる。外したら手で直してや");
    ImGui::Separator();

    if (texId == 0)
    {
        ImGui::TextDisabled("テクスチャが未設定 or 読み込めへん。\n上の欄に assets 相対パスを入れるか、\nアセットブラウザからドラッグしてや。");
        return;
    }

    const int cols = (std::max)(1, m_sheet.cols);
    const int rows = (std::max)(1, m_sheet.rows);

    // 表示サイズ: 窓幅に収めた上で拡大率を掛ける
    const f32 availW = (std::max)(80.0f, ImGui::GetContentRegionAvail().x - 8.0f);
    const f32 baseW  = (std::min)(availW, texW);
    const f32 dispW  = baseW * m_gridZoom;
    const f32 dispH  = dispW * (texH / (std::max)(1.0f, texW));

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Image(texId, ImVec2(dispW, dispH));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 cw = dispW / static_cast<f32>(cols);
    const f32 ch = dispH / static_cast<f32>(rows);

    // 選択中シーケンスに入っているコマを強調する
    const SpriteAnimSeq* seq = (m_selSeq >= 0 && m_selSeq < static_cast<int>(m_sheet.seqs.size()))
        ? &m_sheet.seqs[static_cast<size_t>(m_selSeq)] : nullptr;

    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < cols; ++c)
        {
            const int cell = r * cols + c;
            const ImVec2 a(p0.x + cw * static_cast<f32>(c), p0.y + ch * static_cast<f32>(r));
            const ImVec2 b(a.x + cw, a.y + ch);

            int useCount = 0;
            if (seq)
                for (i32 f : seq->frames) if (f == cell) ++useCount;

            if (useCount > 0)
            {
                dl->AddRectFilled(a, b, IM_COL32(80, 170, 255, 60));
                dl->AddRect(a, b, IM_COL32(110, 190, 255, 255), 0.0f, 0, 2.0f);
            }
            else
            {
                dl->AddRect(a, b, IM_COL32(255, 255, 255, 55));
            }
            if (m_showCellIndex && cw > 18.0f && ch > 14.0f)
            {
                char buf[24];
                if (useCount > 1) std::snprintf(buf, sizeof(buf), "%d x%d", cell, useCount);
                else              std::snprintf(buf, sizeof(buf), "%d", cell);
                dl->AddText(ImVec2(a.x + 2.0f, a.y + 1.0f), IM_COL32(255, 255, 255, 200), buf);
            }
        }
    }

    // クリックでコマを操作: 左=末尾に追加 / Ctrl+左=そのコマを全部除去
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && seq)
    {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const int c = static_cast<int>((m.x - p0.x) / cw);
        const int r = static_cast<int>((m.y - p0.y) / ch);
        if (c >= 0 && c < cols && r >= 0 && r < rows)
        {
            const int cell = r * cols + c;
            auto& frames = m_sheet.seqs[static_cast<size_t>(m_selSeq)].frames;
            auto& holds  = m_sheet.seqs[static_cast<size_t>(m_selSeq)].holds;
            if (ImGui::GetIO().KeyCtrl)
            {
                // holds は frames と長さを合わせておかないと評価側に丸ごと無視される
                for (size_t i = frames.size(); i-- > 0;)
                {
                    if (frames[i] != cell) continue;
                    frames.erase(frames.begin() + static_cast<ptrdiff_t>(i));
                    if (i < holds.size()) holds.erase(holds.begin() + static_cast<ptrdiff_t>(i));
                }
            }
            else
            {
                frames.push_back(cell);
                if (!holds.empty()) holds.push_back(1.0f);
            }
            m_previewTime = 0.0f;
        }
    }
    if (!seq)
        ImGui::TextDisabled("左でシーケンスを選ぶとコマを拾える（左クリック=追加 / Ctrl+左=除去）");
}

void SpriteSheetEditorPanel::DrawSeqEditor(entt::registry& reg, EditorContext& ctx,
                                           const std::string& assetsDir, ResourceManager* resources,
                                           DescriptorHeap* srvHeap,
                                           ID3D12GraphicsCommandList* cmdList)
{
    if (m_selSeq < 0 || m_selSeq >= static_cast<int>(m_sheet.seqs.size()))
    {
        ImGui::TextDisabled("シーケンスを選んでや。");
        return;
    }
    auto& seq = m_sheet.seqs[static_cast<size_t>(m_selSeq)];

    char nameBuf[64];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", seq.name.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##SeqName", nameBuf, sizeof(nameBuf))) seq.name = nameBuf;

    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("fps", &seq.fps, 0.5f, 1.0f, 120.0f, "%.1f");
    ImGui::SetNextItemWidth(120.0f);
    ImGui::Combo("再生", &seq.mode, kModeNames, IM_ARRAYSIZE(kModeNames));

    char evBuf[96];
    std::snprintf(evBuf, sizeof(evBuf), "%s", seq.finishEvent.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##FinEv", "完了イベント名（単発時、空=なし）", evBuf, sizeof(evBuf)))
        seq.finishEvent = evBuf;

    ImGui::Separator();
    ImGui::Text("フレーム列 (%d)", static_cast<int>(seq.frames.size()));
    ImGui::SameLine();
    if (ImGui::SmallButton("全消し")) { seq.frames.clear(); seq.holds.clear(); }
    ImGui::SameLine();
    const bool hasHolds = seq.holds.size() == seq.frames.size() && !seq.frames.empty();
    if (ImGui::SmallButton(hasHolds ? "尺を均一に" : "コマ別の尺"))
    {
        if (hasHolds) seq.holds.clear();
        else          seq.holds.assign(seq.frames.size(), 1.0f);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("コマごとに表示時間の倍率を持たせる（溜めのある攻撃モーション用）");

    ImGui::BeginChild("##FrameList", ImVec2(0.0f, 170.0f), true);
    for (size_t i = 0; i < seq.frames.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        ImGui::Text("%2d:", static_cast<int>(i));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        int cell = seq.frames[i];
        if (ImGui::DragInt("##cell", &cell, 0.2f, 0, m_sheet.cols * m_sheet.rows - 1))
            seq.frames[i] = cell;
        if (seq.holds.size() == seq.frames.size())
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.0f);
            ImGui::DragFloat("尺", &seq.holds[i], 0.05f, 0.0f, 20.0f, "%.2f");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("↑") && i > 0)
        {
            std::swap(seq.frames[i], seq.frames[i - 1]);
            if (seq.holds.size() == seq.frames.size()) std::swap(seq.holds[i], seq.holds[i - 1]);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("↓") && i + 1 < seq.frames.size())
        {
            std::swap(seq.frames[i], seq.frames[i + 1]);
            if (seq.holds.size() == seq.frames.size()) std::swap(seq.holds[i], seq.holds[i + 1]);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("×"))
        {
            seq.frames.erase(seq.frames.begin() + static_cast<ptrdiff_t>(i));
            if (i < seq.holds.size()) seq.holds.erase(seq.holds.begin() + static_cast<ptrdiff_t>(i));
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (seq.frames.empty()) ImGui::TextDisabled("(中央のシートをクリックしてコマを拾う)");
    ImGui::EndChild();

    // ---- プレビュー ----
    ImGui::Separator();
    ImGui::Checkbox("再生", &m_previewPlaying);
    ImGui::SameLine();
    if (ImGui::Button("頭出し")) m_previewTime = 0.0f;
    ImGui::SameLine();
    ImGui::Text("尺 %.2fs", SpriteSeqDuration(seq));

    f32 texW = 0.0f, texH = 0.0f;
    const ImTextureID texId = ResolveTexture(assetsDir, m_sheet.texturePath, resources, srvHeap,
                                             cmdList, texW, texH);
    if (texId != 0 && !seq.frames.empty())
    {
        const SpriteUvRect uv = SpriteSeqUvAt(m_sheet, seq, m_previewTime);
        const f32 side = (std::min)(150.0f, (std::max)(60.0f, ImGui::GetContentRegionAvail().x - 8.0f));
        ImGui::Image(texId, ImVec2(side, side), ImVec2(uv.u0, uv.v0), ImVec2(uv.u1, uv.v1));
        const i32 idx = SpriteSeqIndexAt(seq, m_previewTime);
        if (idx >= 0)
            ImGui::Text("コマ %d / %d (セル %d)", idx + 1, static_cast<int>(seq.frames.size()),
                        seq.frames[static_cast<size_t>(idx)]);
    }

    // ---- 選択エンティティへ適用 ----
    ImGui::Separator();
    const bool hasSel = ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity);
    if (!hasSel || m_currentPath.empty()) ImGui::BeginDisabled();
    if (ImGui::Button("選択中のエンティティに割り当て", ImVec2(-1.0f, 0.0f)))
    {
        auto& sa = reg.get_or_emplace<SpriteAnimator>(ctx.selectedEntity);
        sa.sheetPath  = "spriteanim/" + fs::path(m_currentPath).filename().string();
        sa.currentSeq = seq.name;
        sa._time = 0.0f;
        sa._finished = false;
        m_statusMsg = "割り当てた: " + sa.currentSeq;
        m_statusFlash = 2.0f;
    }
    if (!hasSel || m_currentPath.empty()) ImGui::EndDisabled();
    if (m_currentPath.empty())
        ImGui::TextDisabled("※ 先に保存してからやないと割り当てられへん");
}

} // namespace dx12e
