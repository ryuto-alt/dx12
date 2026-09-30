// マテリアルグラフ窓（G2c）: ライブプレビュー / ノード内サムネイル / 非同期コンパイルの仕上げ（デバウンス・状態表示・エラーのノード上表示）。
//
//   ★エディタ専用。Application が MaterialGraphPanel::InitializeGpu を呼んだときだけ有効（呼ばれなければ従来の空き枠のまま）。
//   ★メインシーンの描画・状態には触らない（専用の RT / 定数 / 環境。GraphPreviewRenderer）。
//   ★グラフ（.dxmg）の内容には一切入らない: プレビュー設定はエディタ設定（prefs "matgraph.preview"）へ。
//
//   流れ:  グラフの編集
//            ├ 値だけの変更（HLSL 不変）: そのフレームのうちに CompileResult を送る → レコードだけ書き直す（再コンパイル 0）
//            └ 構造の変更: 250 ms のデバウンス → CompileResult を送る（PreviewLite の変種をワーカーが DXC + PSO）→ 旧版のまま描き続け、出来たら差し替え
//          描画（RenderGpu）: プレビュー RT（512 固定のうち pixels x pixels）→ 表示テクスチャ（ACES + ガンマ）/ サムネイルのアトラス（64px タイル）
#include "editor/panels/MaterialGraphPanelInternal.h"

#include <nlohmann/json.hpp>

#include "core/PathResolver.h"
#include "editor/EditorIcons.h"
#include "editor/EditorPrefs.h"
#include "editor/EditorTheme.h"
#include "editor/UiWidgets.h"
#include "gui/VirtualInputImGui.h"
#include "input/VirtualInput.h"
#include "renderer/GraphMaterialSystem.h"
#include "renderer/GraphPreviewRenderer.h"
#include "renderer/pt/PtImageIO.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace dx12e
{
namespace mat = matgraph;

namespace
{
using json = nlohmann::json;
using SteadyClock = std::chrono::steady_clock;

GraphMaterialSystem*                    g_graphs = nullptr;
std::unique_ptr<GraphPreviewRenderer>   g_pr;
std::vector<GraphPreviewRenderer::TileRequest> g_tileWork;   // このフレームに GPU で描くタイル（NodeThumbs::Update の結果）
uint64_t                                g_thumbFrame = 0;

ImU32 Col(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
ImU32 ColA(const ImVec4& c, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, a)); }

// NodeThumbs → GraphMaterialSystem（NodeLdr インスタンス）
struct ThumbBackend final : mg::NodeThumbBackend
{
    void SetCompiled(const std::string& key, std::shared_ptr<const mat::CompileResult> cr) override
    {
        if (!g_graphs) return;
        g_graphs->SetInstanceKind(key, GraphMaterialSystem::Kind::NodeLdr);
        g_graphs->SetInstanceCompiled(key, std::move(cr));
    }
    void Remove(const std::string& key) override
    {
        if (g_graphs) g_graphs->RemoveInstance(key);
    }
};
ThumbBackend g_thumbBe;

double Percentile(std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5))];
}
double PercentileF(std::vector<float> v, double p)
{
    std::vector<double> d(v.begin(), v.end());
    return Percentile(std::move(d), p);
}

// 直近のグラフを CPU でコンパイルして GPU へ送る（構造の編集はデバウンス後 / 値のみの変更は即時 / 開いた直後は即時）
void PushPreview(mgpanel::State& s)
{
    if (!g_graphs) return;
    const uint64_t sv = s.ed.Graph().StructureVersion();
    mat::CompileOptions opt;   // sourceName は空 = 同じ構造なら同じ HLSL（変種とキャッシュを共有する）
    auto cr = std::make_shared<const mat::CompileResult>(mat::CompileGraph(s.ed.Graph(), opt));
    s.pushedStruct = sv;
    if (!cr->ok) return;       // エラー: 旧版のまま（診断はエディタが出す。描画は止めない）
    g_graphs->SetInstanceKind(s.previewKey, GraphMaterialSystem::Kind::PreviewLite);
    g_graphs->SetInstanceCompiled(s.previewKey, cr);
    s.expectHash = cr->hash;
}

} // namespace

// ===============================================================================================
// 公開 API（Application が呼ぶ）
// ===============================================================================================
namespace MaterialGraphPanel
{

bool InitializeGpu(const GpuInit& init)
{
    ShutdownGpu();
    if (!init.device || !init.rootSignature || !init.resources || !init.srvHeap || !init.graphs) return false;
    auto pr = std::make_unique<GraphPreviewRenderer>();
    GraphPreviewRenderer::InitDesc d;
    d.device = init.device;
    d.rootSignature = init.rootSignature;
    d.resources = init.resources;
    d.srvHeap = init.srvHeap;
    d.graphs = init.graphs;
    d.shaderDirW = init.shaderDirW;
    if (!pr->Initialize(d)) return false;
    g_graphs = init.graphs;
    g_pr = std::move(pr);
    return true;
}

void ShutdownGpu()
{
    if (g_graphs)
    {
        // サムネイル / プレビューのインスタンスを捨てる（プールのレコードを返す）
        mgpanel::State& s = mgpanel::S();
        if (s.init) s.thumbs.Reset(&g_thumbBe);
        g_graphs->RemoveInstance(mgpanel::S().previewKey);
    }
    g_tileWork.clear();
    g_pr.reset();
    g_graphs = nullptr;
}

bool GpuReady() { return g_pr && g_pr->IsValid() && g_graphs && g_graphs->IsAvailable(); }

void RenderGpu(ID3D12GraphicsCommandList* cmd, unsigned frameIndex)
{
    if (!GpuReady() || !cmd) return;
    mgpanel::State& s = mgpanel::S();
    const auto t0 = SteadyClock::now();
    g_pr->AdvanceFrame();
    // 撮影（MCP）: 前の要求の結果が来たら書く
    {
        std::vector<uint8_t> data;
        uint32_t w = 0, h = 0, bpp = 0;
        bool linear = false;
        if (g_pr->PollCapture(data, w, h, bpp, linear))
        {
            namespace fs = std::filesystem;
            const std::string& pth = linear ? s.pendingShotPfm : s.pendingShotPath;
            if (!pth.empty())
            {
                std::error_code ec;
                fs::create_directories(fs::path(std::u8string(pth.begin(), pth.end())).parent_path(), ec);
                const fs::path fp{std::u8string(pth.begin(), pth.end())};
                if (linear)
                {
                    std::vector<float> rgb(static_cast<size_t>(w) * h * 3);
                    auto half = [](uint16_t hh) {
                        const uint32_t sg = (hh >> 15) & 1, e = (hh >> 10) & 0x1F, m = hh & 0x3FF;
                        uint32_t f;
                        if (e == 0) f = sg << 31;
                        else if (e == 31) f = (sg << 31) | 0x7F800000 | (m << 13);
                        else f = (sg << 31) | ((e + 127 - 15) << 23) | (m << 13);
                        float r;
                        std::memcpy(&r, &f, 4);
                        return r;
                    };
                    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
                        for (int k = 0; k < 3; ++k)
                        {
                            uint16_t hv;
                            std::memcpy(&hv, &data[i * 8 + static_cast<size_t>(k) * 2], 2);
                            rgb[i * 3 + static_cast<size_t>(k)] = half(hv);
                        }
                    pt::io::WritePfm(fp, w, h, rgb.data());
                    s.pendingShotPfm.clear();
                }
                else
                {
                    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
                    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
                        for (int k = 0; k < 3; ++k) rgb[i * 3 + static_cast<size_t>(k)] = data[i * 4 + static_cast<size_t>(k)];
                    pt::io::WritePngRgb8(fp, w, h, rgb.data());
                    s.pendingShotPath.clear();
                }
            }
        }
    }
    // 1. メインプレビュー（枠が描かれたフレームだけ）
    if (s.pvVisible)
    {
        g_pr->RenderMain(cmd, frameIndex, s.previewKey, s.pv, s.pvPixels, static_cast<float>(s.now));
        if (!s.pendingShotPath.empty() || !s.pendingShotPfm.empty())
        {
            // 表示は RequestCapture(false)・リニアは true。両方要るときは表示を先に（リニアは次の機会）
            if (!s.pendingShotPath.empty()) g_pr->RequestCapture(cmd, false);
            else g_pr->RequestCapture(cmd, true);
        }
        s.pvVisible = false;
    }
    // 2. ノード内サムネイル（描けた分だけ MarkDrawn。GPU がまだ使えない分は次のフレームにもう一度）
    if (!g_tileWork.empty())
    {
        std::vector<int> drawn;
        g_pr->RenderTiles(cmd, frameIndex, g_tileWork, &drawn);
        for (int t : drawn) s.thumbs.MarkDrawn(t);
    }
    const float ms = static_cast<float>(std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count());
    s.panelCpuMs.push_back(ms);
    if (s.panelCpuMs.size() > 1200) s.panelCpuMs.erase(s.panelCpuMs.begin(), s.panelCpuMs.begin() + 600);
    if (s.bench.active) s.bench.cpuMs.push_back(ms);
}

} // namespace MaterialGraphPanel

// ===============================================================================================
// 窓の内側（mgpanel）
// ===============================================================================================
namespace mgpanel
{

void PreviewInit(State& s)
{
    // 設定の読み込み（エディタ設定）。グラフには入れない
    if (!s.pvLoaded)
    {
        s.pv.FromString(prefs::GetString("matgraph.preview", ""));
        s.pvSaved = s.pv.ToString();
        s.pvLoaded = true;
    }
    s.debounce.SetConfig(mat::Debouncer::Config{0.25, prefs::GetBool("matgraph.debounceLeading", false), 1.0});
    s.thumbsOn = prefs::GetBool("matgraph.nodeThumbs", true);
    if (!MaterialGraphPanel::GpuReady()) return;
    // ノード内サムネイル: 対象の型のノードにプレビュー領域を予約し、描画を差し込む
    mg::NodeThumbs::ReserveInModel(s.ed.Model());
    s.ed.Doc().Layout().Invalidate();
    s.view.SetNodePreviewDrawer([&s](ng::NodeId id, ImDrawList* dl, ImVec2 mn, ImVec2 mx) -> bool
    {
        s.thumbVisible.push_back(id);
        if (!MaterialGraphPanel::GpuReady() || !s.thumbsOn) return false;
        const mg::NodeThumbs::State st = s.thumbs.StateOf(id);
        if (st == mg::NodeThumbs::State::Ready)
        {
            const int tile = s.thumbs.TileOf(id);
            if (tile < 0) return false;
            float u0, v0, u1, v1;
            GraphPreviewRenderer::TileUv(tile, u0, v0, u1, v1);
            const float R = std::max(2.0f, ui::PxF(3.0f));
            dl->AddImageRounded(static_cast<ImTextureID>(g_pr->AtlasHandle()), mn, mx, ImVec2(u0, v0), ImVec2(u1, v1), IM_COL32_WHITE, R);
            return true;
        }
        if (st == mg::NodeThumbs::State::Error)
        {
            // 評価できない（上流のエラー等）: 斜線 + 「!」。ホバーで理由
            const float R = std::max(2.0f, ui::PxF(3.0f));
            dl->AddRectFilled(mn, mx, ColA(theme::Bad, 0.10f), R);
            dl->PushClipRect(mn, mx, true);
            const float step = std::max(6.0f, ui::PxF(8.0f));
            for (float x = mn.x - (mx.y - mn.y); x < mx.x; x += step)
                dl->AddLine(ImVec2(x, mx.y), ImVec2(x + (mx.y - mn.y), mn.y), ColA(theme::Bad, 0.22f), std::max(1.0f, ui::PxF(1.0f)));
            dl->PopClipRect();
            const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
            const ImVec2 ts = ImGui::CalcTextSize("!");
            dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), Col(theme::Bad), "!");
            if (ImGui::IsMouseHoveringRect(mn, mx)) ImGui::SetTooltip("サムネイルを作れません: %s", s.thumbs.ErrorOf(id).c_str());
            return true;
        }
        return false;   // 準備中: G0 の既定のプレースホルダー
    });
}

void PreviewAfterLoad(State& s)
{
    s.thumbs.Reset(&g_thumbBe);
    g_tileWork.clear();
    s.thumbVisible.clear();
    s.seenStruct = ~0ull;
    s.seenValueSeq = ~0ull;
    s.pushedStruct = ~0ull;
    s.debounce.Cancel();
    s.editAt = -1.0;
    s.expectHash = 0;
    s.latencyMs.clear();
    s.latencyNoDebounceMs.clear();
}

void PreviewUpdate(State& s)
{
    // フレーム時間（直近 600 フレーム）
    {
        const float dt = ImGui::GetIO().DeltaTime * 1000.0f;
        s.frameMs.push_back(dt);
        if (s.frameMs.size() > 1200) s.frameMs.erase(s.frameMs.begin(), s.frameMs.begin() + 600);
        if (s.bench.active) s.bench.frameMs.push_back(dt);
    }
    if (!MaterialGraphPanel::GpuReady()) return;

    // ---- ベンチ（MCP）: 一定間隔でグラフを編集し、編集 → 見た目更新を測る ----
    if (s.bench.active && s.now >= s.bench.nextAt)
    {
        if (s.bench.done >= s.bench.edits)
        {
            // 最後の編集の見た目更新を待つ（editAt が消えるまで）。最大 5 秒
            if (s.editAt < 0.0 || s.now > s.bench.nextAt + 5.0) s.bench.active = false;
        }
        else
        {
            const float salt = 3.0f + 0.001f * static_cast<float>(s.bench.done);
            if (s.bench.value)
            {
                s.ed.Graph().SetNodeProp("tint", "default", mat::Json::array({0.2 + 0.003 * s.bench.done, 0.5, 0.6, 1.0}));
            }
            else
            {
                mg::SaltGraph(s.ed.Graph(), salt);
            }
            ++s.bench.done;
            s.bench.nextAt = s.now + s.bench.intervalSec;
        }
    }

    // ---- 自動回転 ----
    if (s.pv.autoRotate) s.pv.yaw += s.pv.autoRotateSpeed * ImGui::GetIO().DeltaTime;

    // ---- 変更の検出 → デバウンス / 即時送信 ----
    const uint64_t sv = s.ed.Graph().StructureVersion();
    if (sv != s.seenStruct)
    {
        const bool first = s.seenStruct == ~0ull;
        s.seenStruct = sv;
        if (first)
        {
            s.seenValueSeq = s.ed.ValueOnlyEditSeq();
            PushPreview(s);   // 開いた直後 / 新規: 待たずに送る
        }
        else
        {
            s.debounce.Edit(s.now);
            s.editAt = s.now;
        }
    }
    if (s.debounce.Poll(s.now))
    {
        s.fireAt = s.now;
        PushPreview(s);
    }
    else if (s.ed.ValueOnlyEditSeq() != s.seenValueSeq)
    {
        s.seenValueSeq = s.ed.ValueOnlyEditSeq();
        // 値だけの変更（HLSL 不変）は待たずに 1 フレーム以内に反映する。構造の編集がデバウンス待ちのときは、その送信が最新の値も運ぶ
        if (sv == s.pushedStruct && !s.debounce.Pending()) PushPreview(s);
    }

    // ---- 状態（ワーカー）・エラーのノード上表示 ----
    GraphMaterialSystem::InstanceStatus st;
    if (g_graphs->GetInstanceStatus(s.previewKey, st))
    {
        s.gpuCompiling = st.compiling;
        s.gpuFailed = st.failed;
        s.gpuOptimizing = st.optimizing;
        s.gpuLevel = st.level;
        s.gpuPhase = st.pendingPhase;
        s.gpuOptPhase = st.optPhase;
        s.gpuCacheHit = st.cacheHit;
        s.gpuDxcMs = st.dxcMs;
        s.gpuPsoMs = st.psoMs;
        s.gpuUsableMs = st.firstUsableMs;
        s.gpuFinalMs = st.finalMs;
        s.gpuCodegenMs = st.codegenMs;
        s.gpuActiveHash = st.activeHash;
        s.gpuError = st.errorLog;
        // 見た目更新の実測: 編集 → 送った版が描画に使われるようになるまで
        if (s.editAt >= 0.0 && st.hasActive && st.activeHash == s.expectHash && !s.debounce.Pending() && !st.compiling)
        {
            s.latencyMs.push_back((s.now - s.editAt) * 1000.0);
            if (s.fireAt >= 0.0) s.latencyNoDebounceMs.push_back((s.now - s.fireAt) * 1000.0);
            if (s.bench.active) s.bench.lat.push_back((s.now - s.editAt) * 1000.0);
            s.editAt = -1.0;
            if (s.latencyMs.size() > 200) s.latencyMs.erase(s.latencyMs.begin());
            if (s.latencyNoDebounceMs.size() > 200) s.latencyNoDebounceMs.erase(s.latencyNoDebounceMs.begin());
        }
        // DXC のエラー（nodeId + 行へ逆引き済み）→ エディタの診断（ノードの赤枠 + ツールチップ + 診断リストからジャンプ）
        std::vector<mat::Diagnostic> ext;
        if (st.failed)
        {
            for (const mat::Diagnostic& d : st.diagnostics)
                if (d.code == mat::code::kDxc) ext.push_back(d);
            if (ext.empty() && !st.errorLog.empty())
            {
                mat::Diagnostic d;
                d.severity = mat::Severity::Error;
                d.code = "E_RUNTIME";
                d.message = st.errorLog.substr(0, st.errorLog.find('\n'));
                d.hint = "シェーダーのコンパイル / PSO の作成に失敗しました（描画は直前の版のまま）";
                ext.push_back(std::move(d));
            }
        }
        s.ed.SetExternalDiagnostics(std::move(ext));
    }

    // ---- ノード内サムネイル: 前のフレームに描いたノード（= 可視）だけ対象 ----
    if (s.thumbsOn)
    {
        std::sort(s.thumbVisible.begin(), s.thumbVisible.end());
        s.thumbVisible.erase(std::unique(s.thumbVisible.begin(), s.thumbVisible.end()), s.thumbVisible.end());
        const auto work = s.thumbs.Update(s.ed, s.thumbVisible, g_thumbBe, ++g_thumbFrame);
        g_tileWork.clear();
        for (const auto& w : work) g_tileWork.push_back({w.tile, w.key});
    }
    else
        g_tileWork.clear();
    s.thumbs.SetEnabled(s.thumbsOn);
    s.thumbVisible.clear();
}

// ---- 「生成中…」チップ ----
bool GpuChip(State& s, std::string& text, ImU32& dot, bool& ok, std::string& tip)
{
    if (!MaterialGraphPanel::GpuReady()) return false;
    char buf[192];
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(s.now) * 7.0f);
    auto phaseName = [](int ph) { return ph <= 0 ? "待機" : ph == 1 ? "DXC" : ph == 2 ? "PSO" : "反映"; };
    std::string info;
    std::snprintf(buf, sizeof(buf), "CPU 生成 %.2f ms", s.gpuCodegenMs);
    info = buf;
    if (s.gpuUsableMs > 0.0)
    {
        std::snprintf(buf, sizeof(buf), "\nシェーダー: 描ける版まで %.0f ms（DXC %.0f / PSO %.0f%s）", s.gpuUsableMs, s.gpuDxcMs, s.gpuPsoMs, s.gpuCacheHit ? " / キャッシュ" : "");
        info += buf;
    }
    if (!s.latencyMs.empty())
    {
        std::snprintf(buf, sizeof(buf), "\n編集 → 見た目更新（デバウンス 250 ms 込み）: 直近 %.0f ms / 中央値 %.0f ms（%zu 回）", s.latencyMs.back(), Percentile(s.latencyMs, 0.5), s.latencyMs.size());
        info += buf;
    }
    tip = info;
    if (s.gpuFailed)
    {
        text = "シェーダーエラー（旧版で表示中）";
        dot = Col(theme::Bad);
        ok = false;
        return true;
    }
    if (s.debounce.Pending())
    {
        std::snprintf(buf, sizeof(buf), "変更を検出…  あと %.2f 秒", s.debounce.RemainingSec(s.now));
        text = buf;
        dot = ColA(theme::Accent, 0.4f + 0.4f * pulse);
        ok = true;
        return true;
    }
    if (s.gpuCompiling)
    {
        std::snprintf(buf, sizeof(buf), "生成中…  %s", phaseName(s.gpuPhase));
        text = buf;
        dot = ColA(theme::Accent, 0.5f + 0.5f * pulse);
        ok = true;
        return true;
    }
    if (s.gpuOptimizing)
    {
        std::snprintf(buf, sizeof(buf), "高速版で表示中・最適化中…  %s", phaseName(s.gpuOptPhase));
        text = buf;
        dot = ColA(theme::Warn, 0.5f + 0.5f * pulse);
        ok = true;
        return true;
    }
    return false;   // 通常の「生成 OK」表示のまま（CPU の生成結果）
}

// ---- 右カラムのプレビュー枠 ----
float DrawPreviewPane(State& s, float pad)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const float availW = ImGui::GetContentRegionAvail().x - pad * 2.0f;
    const float side = std::max(ui::Px(120.0f), std::min(availW, ui::Px(330.0f)));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 mn(p0.x + pad + (availW - side) * 0.5f, p0.y + pad), mx(mn.x + side, mn.y + side);
    const float R = ui::Px(7.0f);
    const float b1 = std::max(1.0f, ui::PxF(1.0f));
    s.pvMin = mn;
    s.pvMax = mx;

    // ---- 画像 ----
    dl->AddRectFilled(mn, mx, Col(theme::Bg0), R);
    s.pvVisible = true;
    s.pvPixels = std::min(512u, std::max(256u, static_cast<unsigned>(std::lround(side * 1.5f / 32.0f)) * 32u));
    dl->AddImageRounded(static_cast<ImTextureID>(g_pr->MainDisplayHandle()), mn, mx, ImVec2(0.0f, 0.0f), ImVec2(g_pr->MainUv1(), g_pr->MainUv1()), IM_COL32_WHITE, R);
    dl->AddRect(mn, mx, Col(theme::Border), R, 0, b1);

    // ---- 操作: 左ドラッグ = 回転 / 右ドラッグ・Shift+左 = ライトの向き / ホイール = 距離 / ダブルクリック = 戻す ----
    ImGui::SetCursorScreenPos(mn);
    ImGui::InvisibleButton("##mgpreview", ImVec2(side, side), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hov = ImGui::IsItemHovered();
    const bool act = ImGui::IsItemActive();
    Anchor("area", "mg:preview:view");
    if (act)
    {
        const bool light = io.KeyShift || ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (light)
        {
            s.pv.lightYaw += io.MouseDelta.x * 0.012f;
            s.pv.lightPitch -= io.MouseDelta.y * 0.012f;
        }
        else
        {
            s.pv.yaw += io.MouseDelta.x * 0.011f;
            s.pv.pitch += io.MouseDelta.y * 0.011f;
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    else if (hov)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (io.MouseWheel != 0.0f) s.pv.dist *= std::pow(0.92f, io.MouseWheel);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            const mat::PreviewSettings d;
            s.pv.yaw = d.yaw; s.pv.pitch = d.pitch; s.pv.dist = d.dist; s.pv.lightYaw = d.lightYaw; s.pv.lightPitch = d.lightPitch;
        }
        ImGui::SetTooltip("左ドラッグ: 回転   右ドラッグ / Shift+左: ライトの向き\nホイール: 距離   ダブルクリック: 元に戻す");
    }
    s.pv.Clamp();

    // ---- 左下の状態ピル（生成中 / エラー）----
    {
        std::string text, tip;
        ImU32 dot = 0;
        bool ok = true;
        if (GpuChip(s, text, dot, ok, tip))
        {
            const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
            const float ph = ui::Px(20.0f), padX = ui::Px(9.0f), dr = ui::PxF(3.2f);
            const float w = padX + dr * 2.0f + ui::Px(6.0f) + ts.x + padX;
            const ImVec2 a(mn.x + ui::Px(8.0f), mx.y - ui::Px(8.0f) - ph), bb(a.x + std::min(w, side - ui::Px(16.0f)), a.y + ph);
            dl->AddRectFilled(a, bb, IM_COL32(12, 14, 20, 200), ph * 0.5f);
            dl->AddCircleFilled(ImVec2(a.x + padX + dr, a.y + ph * 0.5f), dr, dot, 12);
            dl->PushClipRect(a, bb, true);
            dl->AddText(ImVec2(a.x + padX + dr * 2.0f + ui::Px(6.0f), a.y + (ph - ts.y) * 0.5f), Col(ok ? theme::Text : theme::Bad), text.c_str());
            dl->PopClipRect();
        }
    }

    // ---- 形状 / 環境 / 自動回転 / リセット ----
    ImGui::SetCursorScreenPos(ImVec2(p0.x, mx.y + ui::Px(8.0f)));
    ImGui::Indent(pad);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ui::Px(5.0f, 4.0f));
    struct Opt { const char* label; const char* id; const char* anchor; };
    const Opt shapes[] = {{"球", "##mgSh0", "sphere"}, {"平面", "##mgSh1", "plane"}, {"円柱", "##mgSh2", "cylinder"}, {"立方体", "##mgSh3", "cube"}};
    for (int i = 0; i < 4; ++i)
    {
        if (i) ImGui::SameLine();
        if (ui::Chip(shapes[i].id, shapes[i].label, static_cast<int>(s.pv.shape) == i)) s.pv.shape = static_cast<mat::PreviewShape>(i);
        Anchor("chip", std::string("mg:pv:shape:") + shapes[i].anchor);
    }
    const Opt envs[] = {{"スタジオ", "##mgEn0", "studio"}, {"屋外", "##mgEn1", "outdoor"}, {"暗所", "##mgEn2", "dark"}};
    for (int i = 0; i < 3; ++i)
    {
        if (i) ImGui::SameLine();
        if (ui::Chip(envs[i].id, envs[i].label, static_cast<int>(s.pv.env) == i)) s.pv.env = static_cast<mat::PreviewEnv>(i);
        Anchor("chip", std::string("mg:pv:env:") + envs[i].anchor);
    }
    ImGui::SameLine();
    if (ui::Chip("##mgAuto", "自動回転", s.pv.autoRotate)) s.pv.autoRotate = !s.pv.autoRotate;
    Anchor("chip", "mg:pv:auto");
    ImGui::SameLine();
    if (ui::Chip("##mgThumbs", "ノード内プレビュー", s.thumbsOn))
    {
        s.thumbsOn = !s.thumbsOn;
        prefs::SetBool("matgraph.nodeThumbs", s.thumbsOn);
    }
    Anchor("chip", "mg:pv:thumbs");
    ImGui::PopStyleVar();
    ImGui::Unindent(pad);

    // ---- 設定の保存（グラフではなくエディタ設定。変化があったときだけ）----
    const std::string cur = s.pv.ToString();
    if (cur != s.pvSaved)
    {
        prefs::SetString("matgraph.preview", cur);
        s.pvSaved = cur;
    }
    return (mx.y - p0.y) + ui::Px(8.0f) + ImGui::GetFrameHeight() * 2.0f + ui::Px(14.0f);
}

} // namespace mgpanel

// ===============================================================================================
// MCP / 自動テスト用の窓の操作と状態
// ===============================================================================================
namespace MaterialGraphPanel
{

namespace
{
std::string Dump(const json& j) { return j.dump(); }

json StatusJson(mgpanel::State& s)
{
    json j;
    j["gpuReady"] = GpuReady();
    j["path"] = s.ed.Path();
    j["dirty"] = s.ed.IsDirty();
    j["nodes"] = static_cast<int>(s.ed.Graph().Nodes().size());
    j["compileCount"] = s.ed.CompileCount();
    j["errors"] = s.ed.ErrorCount();
    j["warnings"] = s.ed.WarningCount();
    j["externalErrors"] = s.ed.ExternalErrorCount();
    j["structureVersion"] = s.ed.Graph().StructureVersion();
    j["valueOnlyEdits"] = s.ed.ValueOnlyEditSeq();
    j["preview"] = s.pv.ToString();
    j["previewRect"] = {s.pvMin.x, s.pvMin.y, s.pvMax.x, s.pvMax.y};
    json gpu;
    gpu["compiling"] = s.gpuCompiling;
    gpu["failed"] = s.gpuFailed;
    gpu["optimizing"] = s.gpuOptimizing;
    gpu["level"] = s.gpuLevel;
    gpu["phase"] = s.gpuPhase;
    gpu["debouncePending"] = s.debounce.Pending();
    gpu["debounceFires"] = s.debounce.Fires();
    gpu["cacheHit"] = s.gpuCacheHit;
    gpu["codegenMs"] = s.gpuCodegenMs;
    gpu["dxcMs"] = s.gpuDxcMs;
    gpu["psoMs"] = s.gpuPsoMs;
    gpu["firstUsableMs"] = s.gpuUsableMs;
    gpu["finalMs"] = s.gpuFinalMs;
    char hb[24];
    std::snprintf(hb, sizeof(hb), "%016llx", static_cast<unsigned long long>(s.gpuActiveHash));
    gpu["activeHash"] = hb;
    std::snprintf(hb, sizeof(hb), "%016llx", static_cast<unsigned long long>(s.expectHash));
    gpu["expectHash"] = hb;
    gpu["error"] = s.gpuError.substr(0, 600);
    gpu["latencyMs"] = json(s.latencyMs);
    gpu["latencyNoDebounceMs"] = json(s.latencyNoDebounceMs);
    gpu["latencyP50"] = Percentile(s.latencyMs, 0.5);
    gpu["latencyP95"] = Percentile(s.latencyMs, 0.95);
    j["gpu"] = gpu;
    if (g_graphs)
    {
        const GraphMaterialSystem::Counters c = g_graphs->GetCounters();
        json cj;
        cj["instances"] = c.instances; cj["variants"] = c.variants; cj["dxcCompiles"] = c.dxcCompiles; cj["fastCompiles"] = c.fastCompiles;
        cj["optCompiles"] = c.optCompiles; cj["cacheHits"] = c.cacheHits; cj["psoCreates"] = c.psoCreates; cj["valueUpdates"] = c.valueUpdates;
        cj["jobsQueued"] = c.jobsQueued; cj["jobsDone"] = c.jobsDone; cj["jobsCancelled"] = c.jobsCancelled; cj["staleIgnored"] = c.staleIgnored;
        cj["workers"] = c.workers; cj["poolUsed"] = c.poolUsed;
        j["counters"] = cj;
    }
    const auto ts = s.thumbs.GetStats();
    json tj;
    tj["enabled"] = s.thumbsOn;
    tj["resident"] = ts.resident; tj["wanted"] = ts.wanted; tj["compiledThisFrame"] = ts.compiledThisFrame; tj["compilesTotal"] = ts.compilesTotal;
    tj["sentTotal"] = ts.sentTotal; tj["skippedSame"] = ts.skippedSame; tj["evicted"] = ts.evicted; tj["overBudget"] = ts.overBudget; tj["drawPending"] = ts.drawPending;
    j["thumbs"] = tj;
    j["frameMs"] = {{"p50", PercentileF(s.frameMs, 0.5)}, {"p95", PercentileF(s.frameMs, 0.95)}, {"p99", PercentileF(s.frameMs, 0.99)}, {"max", PercentileF(s.frameMs, 1.0)}, {"n", s.frameMs.size()}};
    j["panelCpuMs"] = {{"p50", PercentileF(s.panelCpuMs, 0.5)}, {"p95", PercentileF(s.panelCpuMs, 0.95)}, {"p99", PercentileF(s.panelCpuMs, 0.99)}, {"max", PercentileF(s.panelCpuMs, 1.0)}, {"n", s.panelCpuMs.size()}};
    json bj;
    bj["active"] = s.bench.active;
    bj["done"] = s.bench.done;
    bj["edits"] = s.bench.edits;
    if (!s.bench.frameMs.empty())
        bj["frameMs"] = {{"p50", PercentileF(s.bench.frameMs, 0.5)}, {"p95", PercentileF(s.bench.frameMs, 0.95)}, {"p99", PercentileF(s.bench.frameMs, 0.99)}, {"max", PercentileF(s.bench.frameMs, 1.0)}, {"n", s.bench.frameMs.size()},
                         {"over16_7", static_cast<int>(std::count_if(s.bench.frameMs.begin(), s.bench.frameMs.end(), [](float f) { return f > 16.7f; }))},
                         {"over33", static_cast<int>(std::count_if(s.bench.frameMs.begin(), s.bench.frameMs.end(), [](float f) { return f > 33.4f; }))}};
    if (!s.bench.cpuMs.empty())
        bj["panelCpuMs"] = {{"p50", PercentileF(s.bench.cpuMs, 0.5)}, {"p95", PercentileF(s.bench.cpuMs, 0.95)}, {"p99", PercentileF(s.bench.cpuMs, 0.99)}, {"max", PercentileF(s.bench.cpuMs, 1.0)}};
    bj["latencyMs"] = json(s.bench.lat);
    bj["latencyP50"] = Percentile(s.bench.lat, 0.5);
    bj["latencyP95"] = Percentile(s.bench.lat, 0.95);
    j["bench"] = bj;
    return j;
}
} // namespace

std::string DebugCall(EditorContext& ctx, const std::string& jsonIn)
{
    mgpanel::State& s = mgpanel::S();
    json in = json::parse(jsonIn, nullptr, false);
    if (in.is_discarded() || !in.is_object()) return Dump({{"ok", false}, {"error", "JSON を解釈できません"}});
    const std::string op = in.value("op", std::string("status"));
    ctx.showMaterialGraph = true;
    if (!s.init) mgpanel::InitState(s);

    if (op == "status") { json j = StatusJson(s); j["ok"] = true; return Dump(j); }
    if (op == "open")
    {
        std::string err;
        const std::string path = in.value("path", std::string());
        if (path.empty()) return Dump({{"ok", false}, {"error", "path が必要です"}});
        std::string full = path;
        if (path.size() < 2 || (path[1] != ':' && path[0] != '/' && path[0] != '\\')) full = PathResolver::AssetsDir() + path;
        if (!OpenFileNow(ctx, full, &err)) return Dump({{"ok", false}, {"error", err}});
        return Dump({{"ok", true}, {"path", s.ed.Path()}});
    }
    if (op == "new")
    {
        const std::string t = in.value("template", std::string("sample"));
        mg::NewTemplate nt = t == "empty" ? mg::NewTemplate::Empty : t == "pbr" ? mg::NewTemplate::StandardPbr : t == "big" ? mg::NewTemplate::Big200 : mg::NewTemplate::Sample;
        s.ed.NewGraph(nt);
        mgpanel::AfterLoadExternal(s);
        return Dump({{"ok", true}, {"nodes", static_cast<int>(s.ed.Graph().Nodes().size())}});
    }
    if (op == "preview")
    {
        if (in.contains("shape")) { mat::PreviewShape sh; if (mat::ParsePreviewShape(in["shape"].get<std::string>(), sh)) s.pv.shape = sh; }
        if (in.contains("env")) { mat::PreviewEnv e; if (mat::ParsePreviewEnv(in["env"].get<std::string>(), e)) s.pv.env = e; }
        if (in.contains("yaw")) s.pv.yaw = in["yaw"].get<float>();
        if (in.contains("pitch")) s.pv.pitch = in["pitch"].get<float>();
        if (in.contains("dist")) s.pv.dist = in["dist"].get<float>();
        if (in.contains("lightYaw")) s.pv.lightYaw = in["lightYaw"].get<float>();
        if (in.contains("lightPitch")) s.pv.lightPitch = in["lightPitch"].get<float>();
        if (in.contains("autoRotate")) s.pv.autoRotate = in["autoRotate"].get<bool>();
        if (in.contains("nodeThumbs")) { s.thumbsOn = in["nodeThumbs"].get<bool>(); prefs::SetBool("matgraph.nodeThumbs", s.thumbsOn); }
        s.pv.Clamp();
        return Dump({{"ok", true}, {"preview", s.pv.ToString()}});
    }
    if (op == "screenshot")
    {
        const std::string path = in.value("path", std::string());
        if (path.empty()) return Dump({{"ok", false}, {"error", "path が必要です"}});
        if (in.value("linear", false)) s.pendingShotPfm = path; else s.pendingShotPath = path;
        return Dump({{"ok", true}, {"note", "次の数フレームで書き出します（プレビュー枠が見えている必要があります）"}});
    }
    if (op == "bench")
    {
        s.bench = mgpanel::State::Bench{};
        s.bench.active = true;
        s.bench.edits = in.value("edits", 12);
        s.bench.value = in.value("kind", std::string("structure")) == "value";
        s.bench.intervalSec = in.value("intervalMs", 400) / 1000.0;
        s.bench.nextAt = s.now + 0.3;
        s.bench.startedAt = s.now;
        return Dump({{"ok", true}});
    }
    if (op == "edit")
    {
        // ops: set {id, prop, value} / literal {id, pin, value} / connect {from:"id.pin", to:"id.pin"} / disconnect {to:"id.pin"} / add {type, id, pos} / undo / redo
        json results = json::array();
        bool allOk = true;
        auto split = [](const std::string& x, std::string& node, std::string& pin) {
            const size_t k = x.find('.');
            if (k == std::string::npos) return false;
            node = x.substr(0, k); pin = x.substr(k + 1);
            return true;
        };
        for (const json& o : in.value("ops", json::array()))
        {
            const std::string k = o.value("op", std::string());
            bool ok = false;
            std::string why;
            mat::MaterialGraph& g = s.ed.Graph();
            if (k == "set")
            {
                const ng::NodeId nid = s.ed.Model().FindInt(o.value("id", std::string()));
                ok = nid && s.ed.SetNodeProp(nid, o.value("prop", std::string()), o.value("value", json()));
                if (!ok) why = "set に失敗（id / prop / value を確かめる）";
            }
            else if (k == "literal")
            {
                const std::string id = o.value("id", std::string()), pin = o.value("pin", std::string());
                const mat::Value v = mat::Value::Float(o.value("value", 0.0f));
                ok = g.SetLiteral(id, pin, v);
            }
            else if (k == "connect" || k == "disconnect")
            {
                std::string fn, fp, tn, tp;
                if (k == "connect") ok = split(o.value("from", std::string()), fn, fp) && split(o.value("to", std::string()), tn, tp) && g.Connect(fn, fp, tn, tp);
                else ok = split(o.value("to", std::string()), tn, tp) && g.Disconnect(tn, tp);
                if (!ok) why = "接続を変更できません";
            }
            else if (k == "add")
            {
                const std::string id = o.value("id", std::string());
                const auto pos = o.value("pos", std::vector<float>{0.0f, 0.0f});
                ok = !g.AddNode(o.value("type", std::string()), pos.size() > 0 ? pos[0] : 0.0f, pos.size() > 1 ? pos[1] : 0.0f, id).empty();
            }
            else if (k == "undo") { if (s.ed.Doc().History().CanUndo()) { s.ed.Doc().History().Undo(s.ed.Doc()); ok = true; } }
            else if (k == "redo") { if (s.ed.Doc().History().CanRedo()) { s.ed.Doc().History().Redo(s.ed.Doc()); ok = true; } }
            else why = "未知の op";
            allOk = allOk && ok;
            results.push_back({{"op", k}, {"ok", ok}, {"why", why}});
        }
        return Dump({{"ok", allOk}, {"results", results}, {"structureVersion", s.ed.Graph().StructureVersion()}});
    }
    return Dump({{"ok", false}, {"error", "未知の op: " + op}});
}

} // namespace MaterialGraphPanel
} // namespace dx12e
