// ===========================================================================
// Application: MCP ディスパッチ表の土台 + スクショ / 読み戻し / 診断
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// method の足し方は本ファイル内 McpDefine の並びに倣う（作法は ApplicationInternal.h の DX12E_MCP_HANDLER 付近）。
// ===========================================================================
#include "core/ApplicationInternal.h"

#include <unordered_set>

namespace dx12e
{
using namespace appdetail;


// names は "a" または "a|b"（get/set を 1 本のハンドラで捌く場合。本文が method を見て分ける）。
void Application::McpDefine(const char* names, const char* paramSpec, McpHandler fn)
{
    const std::string all(names);
    size_t pos = 0;
    for (;;)
    {
        const size_t bar = all.find('|', pos);
        const std::string one = all.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
        if (!one.empty())
        {
            if (!m_mcpMethods.emplace(one, McpMethodEntry{paramSpec, fn}).second)
                Logger::Error("MCP: duplicate method name '{}' (dispatch table)", one);
        }
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
}

// 表は初回の MCP コマンドで 1 度だけ組む（起動時コストをエディタ操作に持ち込まない）。
void Application::EnsureMcpMethodTable()
{
    if (!m_mcpMethods.empty()) return;
    m_mcpMethods.reserve(192);
    RegisterMcpEntityMethods();
    RegisterMcpEditorMethods();
    RegisterMcpRenderMethods();
    RegisterMcpToolingMethods();
    RegisterMcpAssetMethods();
    RegisterMcpTerrainMethods();
    RegisterMcpLightingMethods();
}


std::string Application::HandleMcpCommand(uint64_t client, const std::string& line)
{
    using json = nlohmann::json;

    json req;
    try { req = json::parse(line); }
    catch (const std::exception& e)
    {
        return json{{"id", nullptr}, {"ok", false}, {"error_code", McpErr::InvalidParam},
                    {"error", std::string("parse error: ") + e.what()}}.dump();
    }

    json resp;
    resp["id"] = req.value("id", json(nullptr));
    const std::string method = req.value("method", std::string());
    const json params = req.value("params", json::object());

    // 遅延応答(create/spawn/delete/open_scene/play/stop)の相関情報。
    // 該当ハンドラで deferred=true にし、保留キューへ mcp を積んで空文字列を返す。
    McpDeferred deferred{ client, req.value("id", 0LL), params.value("idempotency_key", std::string()) };
    bool isDeferred = false;

    try
    {
        if (!m_scene || !m_scriptEngine)
            throw std::runtime_error("engine not ready");

        // 生成/削除/シーン系を弾く判定。Playing 中はもちろん、同一 Poll バッチで先に play が
        // 積まれた(モード遷移保留)場合も弾く＝そのフレームで spawn ドレインが skip されて
        // 遅延応答が宙吊り(クライアント timeout)になるのを防ぐ。
        const bool busyPlaying = (m_engineMode == EngineMode::Playing) ||
                                 (m_modeChangeRequested && m_pendingMode == EngineMode::Playing);

        // ★ディスパッチ（旧: 118 本の else-if 連鎖 = C1061 の温床。N37 / N43）。
        //   表引きなので method を何本足しても入れ子は深くならない。
        EnsureMcpMethodTable();
        const auto it = m_mcpMethods.find(method);
        if (it != m_mcpMethods.end())
        {
            it->second.fn(params, resp, method, deferred, isDeferred, busyPlaying);
        }
        else
        {
            resp["ok"] = false;
            resp["error"] = "unknown method: " + method;
            resp["error_code"] = McpErr::InvalidParam;
        }
        if (isDeferred) resp["ok"] = true;   // パネル表示用: dispatch 成功(本応答は遅延)
    }
    catch (const McpError& e)
    {
        resp["ok"] = false;
        resp["error"] = e.what();
        resp["error_code"] = e.code;
        // 「次の一手」と有効値。付いているときだけ載せる(旧来のエラー形は変えない)。
        if (!e.hint.empty())        resp["error_hint"]   = e.hint;
        if (!e.validValues.empty()) resp["error_values"] = e.validValues;
        isDeferred = false;
    }
    catch (const std::exception& e)
    {
        resp["ok"] = false;
        resp["error"] = e.what();
        resp["error_code"] = McpErr::InvalidParam;   // 大半は引数検証エラー
        isDeferred = false;
    }
    // パネル(MCP / AI Bridge)用にコマンド結果を記録（メインスレッドからのみ）。
    if (m_mcpBridge)
        m_mcpBridge->RecordCommand(method, resp.value("ok", false), resp.value("error", std::string()));

    // 未保存フラグ: MCP でシーンを変えた分も拾う。
    // MCP ハンドラは 100 個以上あり、Undo を積むのは group_entities だけなので、
    // ここ 1 箇所で名前から判定する。★読み取り専用リストの方を持つ（新しい書き込み系
    // メソッドが増えたときに黙って漏れる側にしない）。誤検出しても「保存しますか」と
    // 余計に聞くだけで済むが、取りこぼすと黙って作業が消える。
    if (m_editorCtx && resp.value("ok", false) && !method.empty())
    {
        static const std::unordered_set<std::string> kReadOnly = {
            "ping", "get_mode", "get_log", "get_entity", "get_hierarchy", "list_entities",
            "list_scenes", "list_assets", "list_lights", "query_entities", "find_entity",
            "get_bounds", "get_editor_camera", "get_scene_settings", "get_post_process",
            "get_ssao", "get_ssr", "get_ssgi", "get_taa", "get_contact_shadow",
            "get_shadow_pcss", "get_volumetric_fog", "get_dxr", "get_physics_state",
            "get_anim_state", "get_lua_component_state", "get_script_errors",
            "get_play_session", "read_lua_component", "read_shader", "describe_components",
            "describe_lua_api", "describe_anim_graph", "describe_mcp_params",
            "asset_info", "perf_stats", "diagnose", "validate_scene", "raycast",
            "raycast_precise", "overlap_box", "overlap_sphere", "pick",
            "project_world_to_screen", "screenshot", "screenshot_final",
            "screenshot_game_view", "read_texture", "preview_model", "ui_tree",
            "ui_screenshot", "terrain_sample", "terrain_splat_info", "net_status",
            // play/stop はシーンを汚さない（Stop がスナップショットへ戻す）。
            // undo/redo は状態を変えるので入れない（安全側に倒す）。
            // アセット操作: シーンを汚すのは move_asset が参照を実際に書き換えたときだけで、
            // その場合はハンドラ内で MarkEdited を呼んでいる。ここで一律に汚すと
            // 「参照していないアセットを整理しただけ」で未保存扱いになる。
            "move_asset", "delete_asset", "import_asset",
            "benchmark", "step_frames", "play", "stop", "save_scene", "select_entity",
            "focus_camera", "look_at", "set_editor_camera", "key_down",
            "key_up", "key_press", "render_debug", "eval_lua",
        };
        // eval_lua と render_debug は「シーンを変えうる」が、変えないことの方が多い。
        // 変えた場合は設定フィンガープリント（Run ループの定期比較）か、
        // ハンドラ内で積まれる Undo の側で拾われる。
        if (kReadOnly.find(method) == kReadOnly.end())
            m_editorCtx->undoSystem.MarkEdited();
    }
    // 遅延応答は今は送らない(フレーム境界で SendToClient が送る)。Poll が空文字列をスキップ。
    if (isDeferred) return std::string();
    // 不正 UTF-8(例: CP932 のモデル名由来の NameTag)で dump() が例外を投げないよう置換。
    return resp.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// BGRA8(tightly packed, w*4 stride)を PNG ファイルへ。WIC(OS 標準)で書くので外部依存なし。

bool Application::ReadbackSceneBgra(std::vector<u8>& outBgra, u32& outW, u32& outH, std::string& err)
{
    using Microsoft::WRL::ComPtr;

    if (!m_sceneRT || !m_sceneRT->GetResource()) { err = "scene RT not ready"; return false; }
    if (!m_commandQueue || !m_frameResources)    { err = "gpu not ready";     return false; }

    auto* dev    = m_graphicsDevice->GetDevice();
    auto* srcTex = m_sceneRT->GetResource();
    const D3D12_RESOURCE_DESC texDesc = srcTex->GetDesc();
    const UINT w = static_cast<UINT>(texDesc.Width);
    const UINT h = texDesc.Height;
    if (w == 0 || h == 0) { err = "scene size is 0"; return false; }

    // readback バッファのレイアウト(行ピッチは 256B アライン)を取得
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT   rowCount   = 0;
    UINT64 rowSize    = 0;
    UINT64 totalBytes = 0;
    dev->GetCopyableFootprints(&texDesc, 0, 1, 0, &fp, &rowCount, &rowSize, &totalBytes);

    ComPtr<ID3D12Resource> readback;
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = totalBytes;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        { err = "readback alloc failed"; return false; }
    }

    // 前フレームの GPU 完了を待ってから sceneRT(単一リソース＝内容確定)をコピー
    m_commandQueue->WaitIdle();
    auto* cmd = m_frameResources->BeginFrame(*m_commandQueue);

    const D3D12_RESOURCE_STATES prev = m_sceneRT->GetState();
    auto barrier = [&](D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER br{};
        br.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource   = srcTex;
        br.Transition.StateBefore = a;
        br.Transition.StateAfter  = b;
        br.Transition.Subresource = 0;
        cmd->ResourceBarrier(1, &br);
    };
    const bool needBarrier = (prev != D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (needBarrier) barrier(prev, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = srcTex;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource       = readback.Get();
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    if (needBarrier) barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, prev);  // 元の状態へ戻す(エンジンの追跡と一致)

    if (FAILED(cmd->Close())) { m_frameResources->EndFrame(*m_commandQueue); err = "cmd close failed"; return false; }
    m_commandQueue->ExecuteCommandList(cmd);
    m_commandQueue->WaitIdle();
    m_frameResources->EndFrame(*m_commandQueue);

    // R16G16B16A16_FLOAT(リニアHDR) → 表示変換 → BGRA8。
    //
    // ここは PostProcess.hlsl の ToneMapGamma と *同じ分岐* を辿らせること。
    // 以前は ACES 決め打ちだったので、シーンのトーンマップを AgX / なし にしていると
    // スクリーンショットだけ別物の絵になり、色やコントラストの判断を誤らせていた
    // (2D ゲームは「なし(ガンマのみ)」を使うので特に事故りやすい)。
    // 露出も同様に効かせる。それ以外のグレーディングは掛けない(掛けたければ PostProcess を CPU で
    // 丸ごと再実装することになる)ので、色を厳密に見るときはビューポートを信じること。
    void* mapped = nullptr;
    D3D12_RANGE rr{ 0, static_cast<SIZE_T>(totalBytes) };
    if (FAILED(readback->Map(0, &rr, &mapped))) { err = "readback map failed"; return false; }

    static const PostProcessSettings kDefaultPost{};
    const PostProcessSettings& pp = m_scene ? m_scene->GetPostSettings() : kDefaultPost;
    const int   tonemapper = (pp.tonemapper >= 0 && pp.tonemapper <= 2) ? pp.tonemapper : 0;
    const float exposure   = pp.exposureOn ? pp.exposure : 1.0f;

    auto sat = [](float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };

    // 以下 3 つは PostProcess.hlsl の ACESFilm / AgXContrast / TonemapAgX / ToneMapGamma の写し。
    // 片方を直したらもう片方も直すこと（ずれるとスクショだけ違う絵になる）。
    auto aces1 = [&](float x)
    {
        const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
        return sat((x * (a * x + b)) / (x * (c * x + d) + e));
    };
    auto agxContrast = [](float x)
    {
        const float x2 = x * x, x4 = x2 * x2;
        return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4
             - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
    };
    auto toneMapGamma = [&](float rgb[3])
    {
        if (tonemapper == 2)   // トーンマップなし（ガンマのみ）
        {
            for (int i = 0; i < 3; ++i) rgb[i] = std::pow((std::max)(rgb[i], 0.0f), 1.0f / 2.2f);
            return;
        }
        if (tonemapper == 0)   // ACES
        {
            for (int i = 0; i < 3; ++i) rgb[i] = std::pow(aces1(rgb[i]), 1.0f / 2.2f);
            return;
        }
        // AgX（行列 → log2 → コントラスト曲線 → 逆行列。出力は既にガンマ空間）
        static const float kAgx[9] = {
            0.842479062253094f, 0.0784335999999992f, 0.0792237451477643f,
            0.0423282422610123f, 0.878468636469772f, 0.0791661274605434f,
            0.0423756549057051f, 0.0784336f,         0.879142973793104f };
        static const float kAgxInv[9] = {
             1.19687900512017f,  -0.0980208811401368f, -0.0990297440797205f,
            -0.0528968517574562f, 1.15190312990417f,   -0.0989611768448433f,
            -0.0529716355144438f,-0.0980434501171241f,  1.15107367264116f };
        const float minEv = -12.47393f, maxEv = 4.026069f;

        auto mul3 = [](const float m[9], float v[3])
        {
            const float a = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
            const float b = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
            const float c = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
            v[0] = a; v[1] = b; v[2] = c;
        };
        for (int i = 0; i < 3; ++i) rgb[i] = (std::max)(rgb[i], 0.0f);
        mul3(kAgx, rgb);
        for (int i = 0; i < 3; ++i)
        {
            const float l = std::log2((std::max)(rgb[i], 1e-10f));
            rgb[i] = ((l < minEv ? minEv : (l > maxEv ? maxEv : l)) - minEv) / (maxEv - minEv);
            rgb[i] = agxContrast(rgb[i]);
        }
        mul3(kAgxInv, rgb);
        for (int i = 0; i < 3; ++i) rgb[i] = sat(rgb[i]);
    };

    auto toByte = [&](float c)
    {
        const int v = static_cast<int>(sat(c) * 255.0f + 0.5f);
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };

    outBgra.assign(static_cast<size_t>(w) * h * 4, 0);
    const auto* base = static_cast<const uint8_t*>(mapped);
    for (UINT y = 0; y < h; ++y)
    {
        const auto* in  = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(fp.Footprint.RowPitch) * y);
        uint8_t*    out = &outBgra[static_cast<size_t>(w) * 4 * y];
        for (UINT x = 0; x < w; ++x)
        {
            // ★render debug 中はトーンマップも露出も掛けない（RenderDebug.hlsl が
            //   「表示したい色」をそのまま書いているので、加工すると偽色の意味が壊れる）。
            const float ex = m_renderDebugRawReadback ? 1.0f : exposure;
            float rgb[3] = {
                DirectX::PackedVector::XMConvertHalfToFloat(in[x * 4 + 0]) * ex,
                DirectX::PackedVector::XMConvertHalfToFloat(in[x * 4 + 1]) * ex,
                DirectX::PackedVector::XMConvertHalfToFloat(in[x * 4 + 2]) * ex };
            if (!m_renderDebugRawReadback) toneMapGamma(rgb);
            out[x * 4 + 0] = toByte(rgb[2]);   // BGRA 順
            out[x * 4 + 1] = toByte(rgb[1]);
            out[x * 4 + 2] = toByte(rgb[0]);
            out[x * 4 + 3] = 255;
        }
    }
    D3D12_RANGE wr{ 0, 0 };
    readback->Unmap(0, &wr);

    outW = w;
    outH = h;
    return true;
}

// MCP のスクショ系が共有する出力先の解決。
//   省略 → CWD の defName（＝従来の挙動。dx12_engine.log と同じ場所へ上書き）
//   相対 → CWD 基準 / 絶対 → そのまま。拡張子が .png でなければ足す。
// ".." は弾く（ブリッジは localhost 専用だが、AI が誤って上位ディレクトリへ書くのを防ぐ）。
static std::filesystem::path McpScreenshotPath(const std::string& rel, const char* defName)
{
    namespace fs = std::filesystem;
    if (rel.empty()) return fs::absolute(defName);
    fs::path p(rel);
    for (const auto& part : p)
        if (part == "..")
            throw McpError(McpErr::InvalidParam, "path must not contain '..'",
                           "CWD からの相対パスか絶対パスで指定する");
    if (p.extension() != ".png") p += ".png";
    p = fs::absolute(p);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    return p;
}

std::string Application::CaptureSceneScreenshot(std::string& err, const std::string& outPathRel)
{
    namespace fs = std::filesystem;

    std::vector<u8> bgra;
    u32 w = 0, h = 0;
    if (!ReadbackSceneBgra(bgra, w, h, err)) return {};

    const fs::path outPath = McpScreenshotPath(outPathRel, "mcp_screenshot.png");
    if (!WriteBgraPng(outPath.wstring(), bgra.data(), w, h, err)) return {};
    return outPath.string();
}

// ---------------------------------------------------------------------------
// screenshot_final — バックバッファ（ポスト適用後の最終画）の読み戻し（§6 B5 の根治）
// ---------------------------------------------------------------------------
// ★なぜ別経路が要るのか
//   ReadbackSceneBgra が読む m_sceneRT は **ポストプロセスの入力** なので、
//   グレーディング / ブルーム / ゴッドレイ / ビネット / LUT / FXAA / デバンド、
//   そして TAA の解決結果（履歴 RT 側に出る）が 1 つも写らない。
//   MCP の測定と目視が食い違う根本原因がこれ。バックバッファを読めば全部解決する。
//
// ★撮る位置
//   Render() の中、**ImGui フレームを始める前**。この時点のバックバッファには
//   「ポスト適用後のシーン + エディタアイコン + ゲーム内 UI 画像」が入っており、
//   ImGui のパネル / ギズモ / オーバーレイはまだ 1 ピクセルも乗っていない。
//   ＝エディタで撮ってもゲームと同じ絵になる。パネル込みが欲しいときは ui_screenshot。
void Application::CaptureFinalBackBufferRegion(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backBuffer,
                                               u32 vpX, u32 vpY, u32 vpW, u32 vpH)
{
    if (!m_mcpFinalShot.pending || !cmd || !backBuffer) return;
    m_mcpFinalShot.pending  = false;
    m_mcpFinalShot.captured = false;

    // ★どの早期 return でも必ずここを通す。通さないと遅延応答が宙吊りになり、
    //   決定論モードの「時間を固定したまま」状態がエディタに残り続ける（＝時間が止まって見える）。
    auto bail = [&](const char* why)
    {
        FailMcp(m_mcpBridge.get(), m_mcpFinalShot.reply, McpErr::Internal, why);
        m_mcpFinalShot = {};
        m_deterministicCapture = false;
    };

    auto* dev = m_graphicsDevice ? m_graphicsDevice->GetDevice() : nullptr;
    if (!dev) { bail("graphics device not ready"); return; }

    const D3D12_RESOURCE_DESC bbDesc = backBuffer->GetDesc();
    const u32 fullW = static_cast<u32>(bbDesc.Width);
    const u32 fullH = bbDesc.Height;
    // ビューポートがバックバッファをはみ出していたらクランプ（リサイズ直後の 1 フレームで起きる）。
    if (vpW == 0 || vpH == 0 || fullW == 0 || fullH == 0 || vpX >= fullW || vpY >= fullH)
    { bail("viewport rect is empty (window minimized or resizing?)"); return; }
    vpW = (std::min)(vpW, fullW - vpX);
    vpH = (std::min)(vpH, fullH - vpY);

    // コピー先のレイアウトは「切り出す矩形と同じサイズのテクスチャ」で計算する。
    D3D12_RESOURCE_DESC regionDesc = bbDesc;
    regionDesc.Width  = vpW;
    regionDesc.Height = vpH;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT   rowCount = 0;
    UINT64 rowSize = 0, totalBytes = 0;
    dev->GetCopyableFootprints(&regionDesc, 0, 1, 0, &fp, &rowCount, &rowSize, &totalBytes);

    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = totalBytes;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.Format           = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    { bail("readback alloc failed"); return; }

    // バックバッファはこの時点で RENDER_TARGET（呼び出し側の契約）。COPY_SOURCE へ往復させる。
    auto barrier = [&](D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER br{};
        br.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource   = backBuffer;
        br.Transition.StateBefore = a;
        br.Transition.StateAfter  = b;
        br.Transition.Subresource = 0;
        cmd->ResourceBarrier(1, &br);
    };
    barrier(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = backBuffer;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource       = readback.Get();
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    const D3D12_BOX box{ vpX, vpY, 0, vpX + vpW, vpY + vpH, 1 };
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    m_mcpFinalShot.readback = readback;
    m_mcpFinalShot.w        = vpW;
    m_mcpFinalShot.h        = vpH;
    m_mcpFinalShot.rowPitch = fp.Footprint.RowPitch;
    m_mcpFinalShot.bytes    = totalBytes;
    m_mcpFinalShot.format   = static_cast<u32>(bbDesc.Format);
    m_mcpFinalShot.captured = true;
}

void Application::FinishFinalScreenshot()
{
    if (!m_mcpFinalShot.captured) return;
    m_mcpFinalShot.captured = false;
    const bool wasDeterministic = m_mcpFinalShot.deterministic;
    m_deterministicCapture = false;   // 撮り終わったら必ず通常の時間へ戻す

    McpDeferred reply = m_mcpFinalShot.reply;
    m_mcpFinalShot.reply = {};
    auto readback = m_mcpFinalShot.readback;
    m_mcpFinalShot.readback.Reset();
    if (!readback || reply.client == 0) return;

    // コピーは Present と同じコマンドリストに積んである。読む前に GPU の完了を待つ。
    m_commandQueue->WaitIdle();

    const u32 w = m_mcpFinalShot.w, h = m_mcpFinalShot.h, pitch = m_mcpFinalShot.rowPitch;
    // バックバッファは既に表示色（トーンマップ + ガンマ済み）。並びだけ BGRA へ揃える。
    const bool bgraSrc = (m_mcpFinalShot.format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                          m_mcpFinalShot.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

    void* mapped = nullptr;
    // ★read range は必ず GetCopyableFootprints の totalBytes。pitch*h を渡すと最終行の
    //   パディング分だけリソースを超えて E_INVALIDARG になる（実際に踏んだ）。
    D3D12_RANGE rr{ 0, static_cast<SIZE_T>(m_mcpFinalShot.bytes) };
    if (FAILED(readback->Map(0, &rr, &mapped)))
    {
        FailMcp(m_mcpBridge.get(), reply, McpErr::Internal, "readback map failed");
        return;
    }
    std::vector<u8> bgra(static_cast<size_t>(w) * h * 4);
    const auto* base = static_cast<const uint8_t*>(mapped);
    for (u32 y = 0; y < h; ++y)
    {
        const uint8_t* in  = base + static_cast<size_t>(pitch) * y;
        uint8_t*       out = &bgra[static_cast<size_t>(w) * 4 * y];
        for (u32 x = 0; x < w; ++x)
        {
            const uint8_t c0 = in[x * 4 + 0], c1 = in[x * 4 + 1], c2 = in[x * 4 + 2];
            out[x * 4 + 0] = bgraSrc ? c0 : c2;   // B
            out[x * 4 + 1] = c1;                  // G
            out[x * 4 + 2] = bgraSrc ? c2 : c0;   // R
            out[x * 4 + 3] = 255;
        }
    }
    D3D12_RANGE wr{ 0, 0 };
    readback->Unmap(0, &wr);

    std::string err;
    std::filesystem::path outPath;
    try { outPath = McpScreenshotPath(m_mcpFinalShot.path, "mcp_screenshot_final.png"); }
    catch (const std::exception& e)
    {
        FailMcp(m_mcpBridge.get(), reply, McpErr::InvalidParam, e.what());
        return;
    }
    if (!WriteBgraPng(outPath.wstring(), bgra.data(), w, h, err))
    {
        FailMcp(m_mcpBridge.get(), reply, McpErr::Internal, err.empty() ? "png write failed" : err);
        return;
    }
    const PostProcessSettings& pp = m_scene->GetPostSettings();
    CompleteMcp(m_mcpBridge.get(), reply,
        nlohmann::json{{"path", outPath.string()},
                       {"width", w}, {"height", h},
                       {"source", "backbuffer"},
                       {"postApplied", pp.enabled},
                       {"deterministic", wasDeterministic},
                       {"taa", m_scene->GetTaaSettings().enabled},
                       {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"},
                       {"note", "ポスト適用後のバックバッファ。ImGui を描く前に撮るので"
                                "エディタのパネル/ギズモは写らない。dx12_screenshot（ポスト前の "
                                "m_sceneRT）とは別物なので、見た目の判断はこちらを使う"}});
}

Application::DiagRenderInfo Application::GetDiagRenderInfo() const
{
    DiagRenderInfo info;
    info.backBufferFormat = m_swapChain ? static_cast<u32>(m_swapChain->GetFormat()) : 0u;
    info.sceneColorFormat = m_sceneRT   ? static_cast<u32>(m_sceneRT->GetFormat())   : 0u;
    info.depthFormat      = static_cast<u32>(DXGI_FORMAT_D32_FLOAT);
    if (m_scene)
    {
        const PostProcessSettings& pp = m_scene->GetPostSettings();
        info.tonemapper  = pp.tonemapper;
        info.postEnabled = pp.enabled;
        info.exposureOn  = pp.exposureOn;
        info.exposure    = pp.exposure;
    }
    return info;
}

Application::DiagRenderHealth Application::GetDiagRenderHealth() const
{
    DiagRenderHealth h;
    h.renderDebugMode = m_renderDebugMode;
    h.renderDebugName = m_renderDebugModeName;
    if (m_srvHeap)
    {
        h.srvHeapCapacity = m_srvHeap->GetCapacity();
        h.srvHeapFree     = m_srvHeap->GetFreeCount();
    }
    h.renderW = m_renderW;
    h.renderH = m_renderH;
    u32 vx = 0, vy = 0;
    GetDisplayViewport(vx, vy, h.viewportW, h.viewportH);
    h.atLauncher       = m_showLauncher;
    h.cameraOverridden = m_mcpCameraOverride;
    if (m_camera)
    {
        const DirectX::XMFLOAT3 p = m_camera->GetPosition();
        h.cameraFinite = std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
        if (h.cameraFinite) h.cameraDistance = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    }
    return h;
}

Application::DiagDxrInfo Application::GetDiagDxrInfo() const
{
    DiagDxrInfo d;
    d.supported = m_dxrEnabled;
    if (m_graphicsDevice)
    {
        d.tier        = static_cast<int>(m_graphicsDevice->GetRaytracingTier());
        d.shaderModel = static_cast<int>(m_graphicsDevice->GetHighestShaderModel());
        d.inlineRt    = m_graphicsDevice->SupportsInlineRaytracing();
    }
    if (m_scene)
    {
        const RtSettings& r = m_scene->GetRtSettings();
        d.shadowEnabled = r.shadowEnabled;
        d.aoEnabled     = r.aoEnabled;
    }
    d.shadowActive = m_rtShadowActiveThisFrame;
    if (m_rtScene)
    {
        const auto& s = m_rtScene->GetStats();
        d.tlasReady          = m_rtScene->IsReady();
        d.instances          = s.instances;
        d.blasCount          = s.blasCount;
        d.skippedSkinned     = s.skippedSkinned;
        d.skippedTransparent = s.skippedTransparent;
        d.droppedOverLimit   = s.droppedOverLimit;
        d.skinnedInstances   = s.skinnedInstances;
        d.skinnedRebuilds    = s.skinnedRebuilds;
        d.skinnedStale       = s.skinnedStale;
        d.skinnedTriangles   = s.skinnedTriangles;
        d.blasBytes          = s.blasBytes + s.skinnedBlasBytes;
        d.blasTriangles      = s.blasTriangles;
        d.tlasBytes          = s.tlasBytes;
        d.scratchBytes       = s.scratchBytes;
        d.instanceDescBytes  = s.instanceDescBytes;
    }
    return d;
}

Application::DiagFrameStats Application::TakeDiagnosticFrameStats()
{
    const DiagFrameStats out = m_diagFrameStats;
    m_diagFrameStats = {};
    return out;
}



} // namespace dx12e
