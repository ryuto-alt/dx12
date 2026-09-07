#include "editor/panels/TransitionPreviewPanel.h"
#include "editor/EditorContext.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/CommandList.h"
#include "renderer/TransitionPresets.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <cmath>

namespace dx12e
{
namespace
{
// サムネイルのループ 1 周の秒数。実際の遷移秒とは関係なく、一覧の見た目を揃えるための値。
constexpr float kThumbCycle = 2.6f;
// サムネイルの被覆率が往復する範囲。
// ★1.0 まで閉じさせると「ただの黒い四角」になる。28 枚並ぶ一覧では、位相をずらしても
//   常に半数近くが真っ黒 = 一覧としてまったく読めなかった。閉じ切らせず 0.12〜0.82 を
//   往復させると、どのタイルもいつ見ても形が出ている。閉じ切った所と切り替わりの瞬間は
//   左の大プレビューで見る（そちらは 0→1→0 をきちんと通す）。
constexpr float kThumbMin = 0.12f;
constexpr float kThumbMax = 0.82f;

// タイル 1 枚の見た目。画像 + 下にラベル 1 行。
constexpr float kTileDrawW = 158.0f;
constexpr float kTileDrawH = 112.0f;
constexpr float kTileImgH  = 84.0f;
} // namespace

void TransitionPreviewPanel::Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap,
                                        const std::wstring& shaderDir)
{
    m_device    = &device;
    m_srvHeap   = srvHeap;
    m_shaderDir = shaderDir;

    m_atlasRows = (static_cast<u32>(kTransitionPresetCount) + kAtlasCols - 1) / kAtlasCols;

    // RTV は 2 枚（大プレビュー + サムネイルアトラス）。SRV は共有ヒープから 1 個ずつ借りる。
    m_rtvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, false);
    constexpr float clearColor[4] = {0.02f, 0.02f, 0.03f, 1.0f};
    m_previewRT.Initialize(device, &m_rtvHeap, srvHeap, kPreviewW, kPreviewH,
                           DXGI_FORMAT_R8G8B8A8_UNORM, clearColor);
    m_atlasRT.Initialize(device, &m_rtvHeap, srvHeap, kAtlasCols * kTileW, m_atlasRows * kTileH,
                         DXGI_FORMAT_R8G8B8A8_UNORM, clearColor);

    // --- Root Signature: b0(progress/type/aspect/total = 4 DWORD, PIXEL) ---
    // ★実機の SceneTransition と同じレイアウト。同じ定数を同じ順で渡すので、
    //   片方だけ増やすと「プレビューだけ壊れる」になる。増やすときは両方揃えること。
    {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister = 0;
        param.Constants.Num32BitValues = 4;
        param.ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 1;
        desc.pParameters   = &param;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &serialized, &error));
        ThrowIfFailed(device.GetDevice()->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSig)));
    }

    RecreatePipelines(device);
    m_ready = true;
    Logger::Info("TransitionPreviewPanel initialized ({} presets)", kTransitionPresetCount);
}

void TransitionPreviewPanel::RecreatePipelines(GraphicsDevice& device)
{
    if (!m_rootSig) return;

    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"TransitionPreview_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"TransitionPreview_PS.cso");

    // 出力は不透明（架空シーンごと描くので合成は要らない）。深度も無い。
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature        = m_rootSig.Get();
    pso.VS                    = { vs.GetData(), vs.GetSize() };
    pso.PS                    = { ps.GetData(), ps.GetSize() };
    pso.InputLayout           = { nullptr, 0 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable   = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask       = UINT_MAX;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0]    = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat        = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc       = { 1, 0 };

    ThrowIfFailed(device.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

f32 TransitionPreviewPanel::Progress() const
{
    const f32 half = m_dur * 0.5f;
    if (half <= 0.0f) return 0.0f;
    if (m_t < half) return std::clamp(m_t / half, 0.0f, 1.0f);
    return std::clamp(1.0f - (m_t - half) / half, 0.0f, 1.0f);
}

f32 TransitionPreviewPanel::TotalNorm() const
{
    return (m_dur > 0.0f) ? std::clamp(m_t / m_dur, 0.0f, 1.0f) : 0.0f;
}

void TransitionPreviewPanel::Focus(int type, f32 dur)
{
    m_type    = type;
    m_dur     = (dur > 0.05f) ? dur : 0.05f;
    m_t       = 0.0f;
    m_playing = true;
}

void TransitionPreviewPanel::DrawTransitionPass(CommandList& cmd, RenderTarget& rt,
                                                u32 x, u32 y, u32 w, u32 h,
                                                f32 progress, f32 total, int type)
{
    // ★実機（SceneTransition::Render）と同じ 4 定数・同じ並び。
    struct PreviewCB { float progress; int type; float aspect; float total; } cb{};
    cb.progress = progress;
    cb.type     = type;
    cb.aspect   = (h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
    cb.total    = total;

    ID3D12GraphicsCommandList* native = cmd.GetNative();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rt.GetRtv();
    native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    // ビューポートを絞ると、頂点なしフルスクリーン三角形の uv 0..1 がそのまま
    // その矩形へ写る＝アトラスの 1 タイルだけを塗れる。
    cmd.SetViewportAndScissor(x, y, w, h);

    native->SetPipelineState(m_pso.Get());
    native->SetGraphicsRootSignature(m_rootSig.Get());
    native->SetGraphicsRoot32BitConstants(0, 4, &cb, 0);
    native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native->IASetVertexBuffers(0, 0, nullptr);
    native->IASetIndexBuffer(nullptr);
    native->DrawInstanced(3, 1, 0, 0);
}

void TransitionPreviewPanel::RenderOffscreen(CommandList& cmd, f32 dt)
{
    if (!m_ready || !m_pso) return;

    // ---- 時計を進める ----
    m_atlasTime = std::fmod(m_atlasTime + dt, kThumbCycle * 64.0f);   // 桁あふれ防止
    if (m_playing && m_dur > 0.0f)
    {
        m_t += dt * m_speed;
        if (m_t >= m_dur)
        {
            if (m_loop) m_t = std::fmod(m_t, m_dur);
            else        { m_t = m_dur; m_playing = false; }
        }
    }

    constexpr float clearColor[4] = {0.02f, 0.02f, 0.03f, 1.0f};

    // ---- 大プレビュー ----
    m_previewRT.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd.ClearRenderTarget(m_previewRT.GetRtv(), clearColor);
    DrawTransitionPass(cmd, m_previewRT, 0, 0, kPreviewW, kPreviewH,
                       Progress(), TotalNorm(), m_type);
    m_previewRT.Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // ---- サムネイルアトラス（プリセット全件を 1 枚へ焼く）----
    // タイルごとに位相をずらす＝一覧が同時に同じ形にならず、波打って見える。
    m_atlasRT.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd.ClearRenderTarget(m_atlasRT.GetRtv(), clearColor);
    for (int i = 0; i < kTransitionPresetCount; ++i)
    {
        const f32 phase = std::fmod(m_atlasTime / kThumbCycle
                                    + static_cast<f32>(i) * 0.137f, 1.0f);
        const f32 tri  = 1.0f - std::fabs(phase * 2.0f - 1.0f);          // 0→1→0
        const f32 prog = kThumbMin + tri * (kThumbMax - kThumbMin);
        // total は「閉じフェーズの途中」に固定する（= 常にシーンA・常に閉じる向き）。
        // 向きや時間演出を total から取る型（slide / seek / glitch）も一覧では
        // 同じ向きで見えた方が比べやすい。
        const f32 total = prog * 0.5f;

        const u32 cx = (static_cast<u32>(i) % kAtlasCols) * kTileW;
        const u32 cy = (static_cast<u32>(i) / kAtlasCols) * kTileH;
        DrawTransitionPass(cmd, m_atlasRT, cx, cy, kTileW, kTileH, prog, total,
                           static_cast<int>(kTransitionPresets[i].type));
    }
    m_atlasRT.Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_atlasValid = true;
}

void TransitionPreviewPanel::DrawPresetThumb(ImDrawList* dl, const ImVec2& a, const ImVec2& b,
                                             int presetIndex) const
{
    if (!dl) return;
    if (!m_atlasValid || !m_srvHeap || presetIndex < 0 || presetIndex >= kTransitionPresetCount)
    {
        // まだ焼けていない（窓を開いた最初の 1 フレーム）。枠だけ出しておく。
        dl->AddRectFilled(a, b, IM_COL32(18, 19, 24, 255), 3.0f);
        return;
    }

    const int   col = presetIndex % static_cast<int>(kAtlasCols);
    const int   row = presetIndex / static_cast<int>(kAtlasCols);
    const float tw  = static_cast<float>(kTileW);
    const float th  = static_cast<float>(kTileH);
    const float aw  = static_cast<float>(m_atlasRT.GetWidth());
    const float ah  = static_cast<float>(m_atlasRT.GetHeight());
    const ImVec2 uv0(static_cast<float>(col) * tw / aw,
                     static_cast<float>(row) * th / ah);
    const ImVec2 uv1(static_cast<float>(col + 1) * tw / aw,
                     static_cast<float>(row + 1) * th / ah);

    const D3D12_GPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetGpuHandle(m_atlasRT.GetSrvIndex());
    dl->AddImage(static_cast<ImTextureID>(h.ptr), a, b, uv0, uv1);
}

bool TransitionPreviewPanel::RenderWindow(EditorContext& ctx, int& defaultType, f32& defaultDur)
{
    if (!ctx.showTransitionPreview) return false;

    bool changed = false;

    ImGui::SetNextWindowSize(ImVec2(1180.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("トランジション", &ctx.showTransitionPreview))
    {
        ImGui::End();
        return false;
    }

    // 外（Scene Flow 窓 / settings.json の読み直し）で既定が変わったら追従する。
    if (m_type != defaultType)
    {
        m_type    = defaultType;
        m_t       = 0.0f;
        m_playing = true;
    }
    m_dur = (defaultDur > 0.05f) ? defaultDur : 0.05f;

    const int selPreset = FindTransitionPresetIndexByType(static_cast<TransitionType>(m_type));

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("シーンを切り替えるときの演出。ここで選んだ 1 件がプロジェクトの既定になり、"
                        "ビルドしたゲームにも同梱されます。");
    ImGui::PopTextWrapPos();

    // 左 = プレビューと再生コントロール（常に見えている） / 右 = プリセット一覧（縦スクロール）。
    // 縦 1 列にすると 28 枚のタイルがプレビューの下へ押し出され、選ぶたびにスクロールが要る。
    const float kLeftW = 560.0f;
    ImGui::BeginChild("##transLeft", ImVec2(kLeftW, 0.0f), ImGuiChildFlags_None);

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("プレビューは編集中のシーンではなく、専用の架空ゲーム画面"
                        "（昼のフィールド → 夜のダンジョン）で再生します。"
                        "遷移の中間点で本当に絵が差し替わるので、隠れている間に"
                        "シーンが入れ替わる様子まで確認できます。");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    // ================= 大プレビュー =================
    if (m_ready)
    {
        const float availW = ImGui::GetContentRegionAvail().x;
        const float imgW   = std::clamp(availW, 240.0f, 720.0f);
        const float imgH   = imgW * static_cast<float>(kPreviewH) / static_cast<float>(kPreviewW);

        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + imgW, p0.y + imgH);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const D3D12_GPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetGpuHandle(m_previewRT.GetSrvIndex());
        dl->AddImage(static_cast<ImTextureID>(h.ptr), p0, p1);
        dl->AddRect(p0, p1, IM_COL32(70, 72, 82, 255), 3.0f);

        // どちらのシーンを見ているか（HLSL では文字を出せないのでここで重ねる）。
        // 架空 HUD（左上のゲージ / 右上のミニマップ / 下中央のホットバー）を隠さないよう左下へ置く。
        const bool  sceneB = (TotalNorm() >= 0.5f);
        const char* tag    = sceneB ? "シーンB（切り替え後）" : "シーンA（切り替え前）";
        const ImU32 tagCol = sceneB ? IM_COL32(150, 120, 235, 255) : IM_COL32(90, 175, 235, 255);
        const ImVec2 ts = ImGui::CalcTextSize(tag);
        const ImVec2 tp(p0.x + 8.0f, p1.y - 10.0f - ts.y);
        dl->AddRectFilled(tp, ImVec2(tp.x + 12.0f + ts.x, tp.y + 6.0f + ts.y),
                          IM_COL32(8, 9, 14, 205), 3.0f);
        dl->AddText(ImVec2(tp.x + 6.0f, tp.y + 3.0f), tagCol, tag);

        ImGui::Dummy(ImVec2(imgW, imgH));
    }

    ImGui::Spacing();

    // ================= 再生コントロール =================
    if (ImGui::Button(m_playing ? "一時停止" : "▶ 再生", ImVec2(110.0f, 0.0f)))
        m_playing = !m_playing;
    ImGui::SameLine();
    if (ImGui::Button("最初から", ImVec2(110.0f, 0.0f)))
    {
        m_t       = 0.0f;
        m_playing = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox("ループ", &m_loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("再生速度", &m_speed, 0.15f, 2.0f, "x%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("形をじっくり見たいときは 0.25 くらいまで落とす。\n"
                          "実機の速さは下の「長さ（秒）」で決まる（ここは見るためだけの倍率）。");

    // スクラブ（手で遷移の途中を掴む）
    {
        float scrub = TotalNorm();
        ImGui::SetNextItemWidth(-160.0f);
        if (ImGui::SliderFloat("##transScrub", &scrub, 0.0f, 1.0f, "コマ送り %.2f"))
        {
            m_t       = scrub * m_dur;
            m_playing = false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%.2f / %.2f 秒", static_cast<double>(m_t), static_cast<double>(m_dur));
    }

    // 長さ（＝実機の遷移秒。settings.json へ入る）
    ImGui::SetNextItemWidth(260.0f);
    ImGui::SliderFloat("長さ（秒）", &defaultDur, 0.2f, 3.0f, "%.2f");
    // スライダーは掴んでいる間ずっと値が変わる。ディスクへ書くのは離した時だけ。
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("「閉じる → （シーンをロード） → 開く」の合計秒。\n"
                          "1.0 秒あたりが下限の目安。0.6 秒台だと演出の形を読み取る前に終わる。");
    ImGui::SameLine();
    if (ImGui::SmallButton("プリセットの既定秒に戻す") && selPreset >= 0)
    {
        defaultDur = kTransitionPresets[selPreset].duration;
        changed    = true;
    }

    if (selPreset >= 0)
    {
        const TransitionPreset& pr = kTransitionPresets[selPreset];
        ImGui::TextColored(ImVec4(0.47f, 0.75f, 1.0f, 1.0f),
                           "選択中: %s（%.2f 秒） / Lua: transitionToScene(rel, \"%s\")",
                           pr.label, static_cast<double>(defaultDur), pr.id);
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                           "選択中: (不明な型 %d)。下から選び直してください", m_type);
    }

    ImGui::EndChild();   // ##transLeft

    // ================= プリセット一覧（右カラム・縦スクロール）=================
    ImGui::SameLine();
    ImGui::BeginChild("##transPresets", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("クリックで 1 つ選ぶ（トランジションは重ねられません）。"
                        "サムネイルは実物と同じシェーダーで動かしています。");
    ImGui::PopTextWrapPos();

    ImDrawList*  dl      = ImGui::GetWindowDrawList();
    const float  sp      = ImGui::GetStyle().ItemSpacing.x;
    TransitionGroup curGroup = TransitionGroup::Count;
    float  lineW = 0.0f;
    bool   first = true;

    for (int i = 0; i < kTransitionPresetCount; ++i)
    {
        const TransitionPreset& pr = kTransitionPresets[i];

        if (pr.group != curGroup)
        {
            curGroup = pr.group;
            ImGui::SeparatorText(TransitionGroupLabel(curGroup));
            lineW = 0.0f;
            first = true;
        }

        const float availW = ImGui::GetContentRegionAvail().x;
        if (!first && lineW + sp + kTileDrawW <= availW)
        {
            ImGui::SameLine();
            lineW += sp + kTileDrawW;
        }
        else
        {
            lineW = kTileDrawW;
        }
        first = false;

        ImGui::PushID(i);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##transtile", ImVec2(kTileDrawW, kTileDrawH));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const bool sel     = (i == selPreset);

        const ImVec2 s0(p0.x + 3.0f, p0.y + 3.0f);
        const ImVec2 s1(p0.x + kTileDrawW - 3.0f, p0.y + 3.0f + kTileImgH);
        DrawPresetThumb(dl, s0, s1, i);

        const ImVec2 ls = ImGui::CalcTextSize(pr.label);
        dl->AddText(ImVec2(p0.x + (kTileDrawW - ls.x) * 0.5f, s1.y + 6.0f),
                    sel ? IM_COL32(120, 190, 255, 255) : IM_COL32(205, 205, 212, 255),
                    pr.label);

        dl->AddRect(p0, ImVec2(p0.x + kTileDrawW, p0.y + kTileDrawH),
                    sel       ? IM_COL32(60, 140, 245, 255)
                    : hovered ? IM_COL32(150, 152, 162, 220)
                              : IM_COL32(64, 65, 74, 180),
                    4.0f, 0, sel ? 2.5f : 1.0f);

        if (hovered)
            ImGui::SetTooltip("%s\n\nID: \"%s\"  /  既定 %.1f 秒\n"
                              "クリックで選ぶと上のプレビューで頭から再生します",
                              pr.tip, pr.id, static_cast<double>(pr.duration));
        if (clicked)
        {
            defaultType = static_cast<int>(pr.type);
            defaultDur  = pr.duration;
            changed     = true;
            Focus(defaultType, defaultDur);
        }
        ImGui::PopID();
    }

    ImGui::EndChild();   // ##transPresets

    ImGui::End();
    return changed;
}

} // namespace dx12e
