#pragma once

namespace dx12e
{

// レイトレーシング（DXR 1.1 inline / RayQuery）のシーン単位の設定。
// SSAOSettings / ContactShadowSettings と同じ流儀のヘッダオンリー定義（シリアライズ対象）。
//
// ★既定は全部 OFF。DXR 非対応 GPU では Application が黙って無視する
//   （GraphicsDevice::SupportsInlineRaytracing() が唯一のゲート）。
//   OFF のとき絵は導入前と完全に一致する（新しいテクスチャを 1 枚も貼らないため）。
struct RtSettings
{
    // --- RT サン影（計画09 Step 2）------------------------------------------
    // 出力先は既存のコンタクトシャドウ枠(t11)。ルートシグネチャは 1 DWORD も増えない。
    // ON にするとコンタクトシャドウパスの代わりに RT 影パスが同じテクスチャを埋める。
    bool  shadowEnabled  = false;
    // 太陽の角直径(度)。0 でハードシャドウ。実際の太陽は 0.53 度。
    // ★PCSS と併用するときは 0 にして PCSS 側に半影を任せるのが正しい（RtScreenPass.h の表参照）。
    float shadowSunAngle = 0.53f;
    // レイ始点の法線方向オフセット(m)。自己遮蔽（アクネ）対策。
    // CSM の depthBias と違い**ワールド空間の実距離**なので peter-panning にならない。
    float shadowNormalBias = 0.02f;
    // レイの最大距離(m)。0 以下で無限。遠景の遮蔽物を追わない分だけ速くなる。
    float shadowMaxDistance = 0.0f;
    // 1=RT 影のみ / 0=無効。0..1 で CSM 側とブレンド（デバッグ用）。
    float shadowIntensity = 1.0f;

    // --- RT-AO（計画09 Step 3）----------------------------------------------
    // 出力先は既存の SSAO 枠(t8)。こちらもルートシグネチャ増分ゼロ。
    bool  aoEnabled   = false;
    float aoRadius    = 1.0f;    // 半球レイの長さ(m)
    int   aoRayCount  = 2;       // 1px あたりのレイ本数(1..8)
    float aoIntensity = 1.0f;
    float aoPower     = 1.5f;    // pow() のべき指数（SSAO と同じ意味）
    // SSAO と min() 合成するか。RT-AO はプローブ的で細かい皺の遮蔽が苦手なので、
    // 「大きな遮蔽=RT / 細部=SSAO」の合成が実用上いちばん良い。
    bool  aoCombineWithSsao = false;
    // 空間デノイザ（joint bilateral）。1px あたり数本のレイをそのまま出すとノイズが乗るので、
    // 深度・法線・接平面でエッジを守りながら平滑化する。構成は Unity HDRP の RTAO と同じ。
    // ★false で完全に従来経路（トレース結果をそのまま t8 へ）に戻る。
    // ★G-Buffer が書かれていないフレームは重みが作れないので自動的にスキップされる。
    bool  aoDenoise       = true;
    float aoDenoiseRadius = 8.0f;   // フィルタ半径(px)。0 で無効

    // --- 共通 ---------------------------------------------------------------
    // TLAS へ入れるインスタンスの上限。0 で既定（RaytracingScene::kMaxRtInstances）。
    int   maxInstances = 0;
    // デバッグ可視化用に、RT 影 / RT-AO が無効でも TLAS を建てるか
    // （dx12_render_debug の rtHit / rtNormal モードで使う）。
    bool  forceBuildTlas = false;
};

} // namespace dx12e
