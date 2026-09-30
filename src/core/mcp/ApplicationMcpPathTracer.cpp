// ===========================================================================
// MCP: DXR パストレーサー(地上真値レンダラ。パリティ基盤 Q1a)
//   render_reference         … リファレンスレンダーを開始する(即座に返る。進捗は render_reference_status)
//   render_reference_status  … 状態 / 進捗 / 出力ファイル / 統計
//   render_reference_cancel  … 中止する(save:true でそこまでの結果を保存)
// ---------------------------------------------------------------------------
// ★重い処理なので「開始 → ポーリング」の非同期 API にした。応答の progress は M6 のジョブ API の Progress
//   ({phase, pct, message, etaSec})と同じ形＝ジョブ種別(エンジンメソッドを叩くタイプ)へそのまま載せられる。
// ★エンジンを固めない: 1 フレームあたりに使う GPU 時間は frameBudgetMs(既定 12 ms)まで。
// 設計・仕様・光の単位の対応は docs/PATH_TRACER.md。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/PathTracerHost.h"
#include "core/mcp/McpManifestBuild.h"
#include "renderer/pt/PtImageIO.h"

namespace dx12e
{
using namespace appdetail;
using namespace mcpdata;   // P(...)（core/mcp/McpManifestBuild.h）

void Application::RegisterMcpPathTracerMethods()
{
    using json = nlohmann::json;

    // ---- render_reference ----------------------------------------------------
    {
        McpMeta m;
        m.summary    = "DXR パストレーサー(地上真値)でリファレンスを描いて線形 HDR(PFM / EXR)+ LDR プレビュー PNG + メタ JSON を書く。"
                       "即座に返り、進捗は render_reference_status。フォワード + DDGI/SSGI/SSR との比較の基準画像(UE の並べ比較の代わり)。"
                       "エンジンを固めないよう 1 フレームに使う GPU 時間は frameBudgetMs まで(既定 12ms)";
        m.keywords   = "path tracer pathtracer reference ground truth パストレーサー リファレンス 地上真値 基準画像 gt ray tracing 比較 hdr pfm exr render_reference "
                       "unreal parity ue 並べ比較 光輸送 グローバルイルミネーション";
        m.category   = "render";
        m.group      = "render_reference";
        m.target     = "path_tracer";
        m.effect     = McpEffect::Runtime;
        m.mode       = "any";
        m.timeoutMs  = 15000;
        m.idempotent = false;
        m.aliases    = {"dx12_render_reference"};
        m.params     = {
            P("spp", "int", false, nullptr, "1", "1048576", "256", "画素あたりのサンプル数(目標)。多いほど滑らか(誤差は 1/√N)"),
            P("bounces", "int", false, nullptr, "1", "64", "8", "散乱頂点の上限。1 = 直接光のみ / N = 最大 N-1 回の間接バウンス(ロシアンルーレット付き)"),
            P("size", "array", false, nullptr, nullptr, nullptr, "[1920,1080]", "出力解像度 [幅, 高さ]。ビューポートの矩形に依存しない固定サイズ"),
            P("camera", "object", false, nullptr, nullptr, nullptr, nullptr,
              "カメラ {position:[x,y,z], target:[x,y,z], fovDeg?, lensRadius?, focusDist?}。省略 = 今の描画カメラ(エディタ / ゲーム)。fovDeg は垂直 FOV"),
            P("output", "string", false, nullptr, nullptr, nullptr, nullptr,
              "出力の基準パス(拡張子なし。.pfm / .exr / .png / .json を付けて書く)。省略 = <project>/.dx12/pt/render_<日時>"),
            P("seed", "int", false, nullptr, "0", "2147483647", "1", "乱数シード。同じシード + 同じ設定 = 同じ結果(決定論)"),
            P("maxRadiance", "number", false, nullptr, "0", "1000000", "0", "1 サンプルの放射輝度の上限(ファイアフライ抑制)。0 = クランプ無し(GT の既定)"),
            P("frameBudgetMs", "number", false, nullptr, "0.5", "200", "12", "1 フレームに PT へ使う GPU 時間の上限(ms)。小さいほどエディタが軽く遅い。無人の計測は 40 程度"),
            P("maxSeconds", "number", false, nullptr, "0", "86400", "0", "実行時間の上限(秒。0 = 無制限)。超えたらそこまでの結果を保存して終わる(truncated:true)"),
            P("formats", "array", false, nullptr, nullptr, nullptr, "[\"pfm\",\"png\"]", "書き出す形式(\"pfm\" / \"exr\" / \"png\")。メタ JSON は常に書く"),
            P("exposure", "number", false, nullptr, "0.0001", "10000", "1", "プレビュー PNG の露出(倍率。簡易 ACES + sRGB)。線形 HDR には掛けない"),
            P("lightFalloff", "enum", false, "engine|physical", nullptr, nullptr, "\"engine\"",
              "点 / スポット光の減衰。engine = フォワードの saturate(1-d/range)^2(既定・比較の前提)/ physical = 逆二乗(1/d^2)"),
            P("sunAngularRadiusDeg", "number", false, nullptr, "0", "45", nullptr, "太陽の角半径(度)。省略 = PCSS 設定に従う(OFF ならデルタ光)。0 = デルタ光"),
            P("russianRoulette", "bool", false, nullptr, nullptr, nullptr, "true", "ロシアンルーレット(不偏。false で全経路を bounces まで追う)"),
            P("forceLambert", "bool", false, nullptr, nullptr, nullptr, "false", "全材質を純ランバートにする(検証用)"),
            P("normalMaps", "bool", false, nullptr, nullptr, nullptr, "true", "法線マップを使う"),
            P("quantizeLikeForward", "bool", false, nullptr, nullptr, nullptr, "true",
              "材質値(色ティント / 不透明度 / アルファ閾値 / 発光)をフォワードの b2 と同じ 8bit 量子化にする(比較を揃える)。false = 元の float 値"),
            P("background", "bool", false, nullptr, nullptr, nullptr, "true", "カメラから見える空を描く(skybox の設定に従う)。false = 黒"),
            P("tileSize", "int", false, nullptr, "16", "2048", "256", "1 ディスパッチのタイル辺(画素)。TDR 対策の分割単位"),
            P("samplesPerDispatch", "int", false, nullptr, "1", "256", "1", "1 ディスパッチで回すサンプル数"),
            P("note", "string", false, nullptr, nullptr, nullptr, nullptr, "メタ JSON にそのまま入れる自由メモ"),
        };
        m.next       = {{"render_reference_status", "進捗 / 出力ファイル / 統計を読む(waitSec でこの秒数まで待てる TS ツールもある)"},
                        {"render_reference_cancel", "実行中のレンダーを止める"}};
        m.examples   = {{"{\"spp\":1024,\"bounces\":8,\"size\":[1920,1080],\"output\":\"C:/tmp/ref/ps1\"}", "今のカメラで 1080p / 1024spp"},
                        {"{\"spp\":64,\"size\":[640,360],\"camera\":{\"position\":[0,2,-6],\"target\":[0,1,0],\"fovDeg\":45}}", "固定カメラで低解像度の下見"}};
        McpDefine("render_reference", McpMeta(m), DX12E_MCP_HANDLER
            {
                PtRequest r;
                r.spp     = static_cast<uint32_t>(McpIntParam(params, "spp", 256, 1, 1 << 20));
                r.bounces = static_cast<uint32_t>(McpIntParam(params, "bounces", 8, 1, 64));
                r.seed    = static_cast<uint32_t>(McpIntParam(params, "seed", 1, 0, 2147483647));
                r.maxRadiance   = McpFloatParam(params, "maxRadiance", 0.0f, 0.0f, 1.0e6f);
                r.frameBudgetMs = McpFloatParam(params, "frameBudgetMs", 12.0f, 0.5f, 200.0f);
                r.maxSeconds    = static_cast<double>(McpFloatParam(params, "maxSeconds", 0.0f, 0.0f, 86400.0f));
                r.exposure      = McpFloatParam(params, "exposure", 1.0f, 0.0001f, 10000.0f);
                r.tileSize      = static_cast<uint32_t>(McpIntParam(params, "tileSize", 256, 16, 2048));
                r.samplesPerDispatch = static_cast<uint32_t>(McpIntParam(params, "samplesPerDispatch", 1, 1, 256));
                if (params.contains("size") && !params["size"].is_null())
                {
                    const json& s = params["size"];
                    if (!s.is_array() || s.size() != 2 || !s[0].is_number() || !s[1].is_number())
                        throw McpError(McpErr::InvalidParam, "size は [幅, 高さ] の整数 2 要素", "例 [1920,1080]");
                    r.width = static_cast<uint32_t>(std::max(1, s[0].get<int>()));
                    r.height = static_cast<uint32_t>(std::max(1, s[1].get<int>()));
                }
                r.outputBase = params.value("output", std::string());
                r.note = params.value("note", std::string());
                r.russianRoulette = params.value("russianRoulette", true);
                r.forceLambert = params.value("forceLambert", false);
                r.noNormalMaps = !params.value("normalMaps", true);
                r.quantizeLikeForward = params.value("quantizeLikeForward", true);
                r.background = params.value("background", true);
                const int fall = McpEnumParam(params, "lightFalloff", {"engine", "physical"}, 0);
                r.physicalFalloff = (fall == 1);
                r.sunAngularRadiusDeg = McpFloatParam(params, "sunAngularRadiusDeg", -1.0f, 0.0f, 45.0f);
                if (params.contains("formats") && !params["formats"].is_null())
                {
                    if (!params["formats"].is_array())
                        throw McpError(McpErr::InvalidParam, "formats は文字列の配列", "例 [\"pfm\",\"exr\",\"png\"]");
                    r.writePfm = r.writeExr = r.writePng = false;
                    for (const auto& f : params["formats"])
                    {
                        const std::string s = f.is_string() ? f.get<std::string>() : std::string();
                        if (s == "pfm") r.writePfm = true;
                        else if (s == "exr") r.writeExr = true;
                        else if (s == "png") r.writePng = true;
                        else throw McpError(McpErr::InvalidParam, "unknown format: " + s, "pfm / exr / png のどれか", {"pfm", "exr", "png"});
                    }
                }
                if (params.contains("camera") && params["camera"].is_object())
                {
                    const json& c = params["camera"];
                    DirectX::XMFLOAT3 pos{}, tgt{};
                    if (!McpTryVec3(c, "position", pos) || !McpTryVec3(c, "target", tgt))
                        throw McpError(McpErr::InvalidParam, "camera には position と target([x,y,z])が要る",
                                       "例 {\"position\":[0,2,-6],\"target\":[0,1,0],\"fovDeg\":45}。省略すると今の描画カメラを使う");
                    r.useSceneCamera = false;
                    r.camPos[0] = pos.x; r.camPos[1] = pos.y; r.camPos[2] = pos.z;
                    r.camTarget[0] = tgt.x; r.camTarget[1] = tgt.y; r.camTarget[2] = tgt.z;
                    r.fovDeg = McpFloatParam(c, "fovDeg", 45.0f, 1.0f, 170.0f);
                    r.lensRadius = McpFloatParam(c, "lensRadius", 0.0f, 0.0f, 10.0f);
                    r.focusDist = McpFloatParam(c, "focusDist", 10.0f, 0.01f, 1.0e5f);
                }

                PtHost& host = EnsurePathTracerHost();
                std::string err;
                if (!host.Request(*this, r, &err))
                {
                    McpError e(host.Busy() ? McpErr::Busy : McpErr::Unsupported, err,
                               host.Busy() ? "render_reference_status で進捗を見るか render_reference_cancel で止める" : "GPU / カメラ / 引数を確認する");
                    if (host.Busy()) e.fix.push_back(MakeMcpFix("render_reference_status", json::object(), "実行中のジョブの進捗を読む"));
                    throw e;
                }
                json res = json::object();
                res["accepted"] = true;
                res["jobId"] = host.jobId;
                res["state"] = "requested";
                res["size"] = json::array({r.width, r.height});
                res["spp"] = r.spp;
                res["bounces"] = r.bounces;
                res["seed"] = r.seed;
                res["frameBudgetMs"] = r.frameBudgetMs;
                res["output"] = host.outputBase;
                res["hint"] = "即座に返った。render_reference_status で進捗と出力ファイルを読む。エディタは前面に出さず、GPU 時間の上限は frameBudgetMs";
                resp["ok"] = true;
                resp["result"] = std::move(res);
            });
    }

    // ---- render_reference_status ---------------------------------------------
    {
        McpMeta m;
        m.summary    = "リファレンスレンダー(render_reference)の状態。{state: idle|requested|preparing|running|finalizing|done|failed|cancelled, "
                       "progress{phase,pct,message,etaSec}, samples{done,target}, gpu{msPerSpp}, output{base,files[],truncated,sppDone}, scene{...}, error}";
        m.keywords   = "path tracer reference status progress 進捗 リファレンス パストレーサー render_reference_status";
        m.category   = "render";
        m.group      = "render_reference";
        m.target     = "path_tracer";
        m.effect     = McpEffect::Read;
        m.timeoutMs  = 8000;
        m.idempotent = true;
        m.aliases    = {"dx12_render_reference_status"};
        m.params     = {P("preview", "bool", false, nullptr, nullptr, nullptr, "false",
                          "true = 今までの累積を読み戻して <output>.preview.png(簡易トーンマップ)へ書く。GPU の完了待ちで数十 ms 止まる。実行中のみ")};
        m.next       = {{"render_reference_cancel", "止める"}, {"render_reference", "次のレンダーを開始する"}};
        m.examples   = {{"{}", "状態と進捗"}, {"{\"preview\":true}", "途中経過のプレビュー PNG も書く"}};
        McpDefine("render_reference_status", McpMeta(m), DX12E_MCP_HANDLER
            {
                PtHost& host = EnsurePathTracerHost();
                json st = host.Status(*this);
                if (params.value("preview", false))
                {
                    uint32_t ps = 0; std::string pe;
                    if (host.WritePreview(&ps, &pe)) { st["preview"] = host.previewPath; st["previewSamples"] = ps; }
                    else st["previewError"] = pe;
                }
                resp["ok"] = true;
                resp["result"] = std::move(st);
            });
    }

    // ---- render_reference_cancel ---------------------------------------------
    {
        McpMeta m;
        m.summary    = "実行中のリファレンスレンダーを止める。save:true でそこまでの累積(spp 不足)を保存する(既定は捨てる)";
        m.keywords   = "path tracer reference cancel stop 中止 キャンセル パストレーサー render_reference_cancel";
        m.category   = "render";
        m.group      = "render_reference";
        m.target     = "path_tracer";
        m.effect     = McpEffect::Runtime;
        m.timeoutMs  = 8000;
        m.idempotent = true;
        m.aliases    = {"dx12_render_reference_cancel"};
        m.params     = {P("save", "bool", false, nullptr, nullptr, nullptr, "false", "そこまでの結果を保存する(spp は目標未満)")};
        m.next       = {{"render_reference_status", "止まったことを確かめる"}};
        m.examples   = {{"{}", "捨てて中止"}, {"{\"save\":true}", "そこまでの結果を保存して中止"}};
        McpDefine("render_reference_cancel", McpMeta(m), DX12E_MCP_HANDLER
            {
                PtHost& host = EnsurePathTracerHost();
                const bool wasBusy = host.Busy();
                host.Cancel(params.value("save", false));
                json res = json::object();
                res["cancelled"] = wasBusy;
                res["state"] = host.Status(*this)["state"];
                res["hint"] = wasBusy ? "次のフレームで止まる。render_reference_status で確認" : "実行中のジョブは無い";
                resp["ok"] = true;
                resp["result"] = std::move(res);
            });
    }
}

} // namespace dx12e
