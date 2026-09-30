// ビューポート周りの純ロジック（editor/ViewportLogic.h）の単体テスト（フェーズ 1a）。
//   ・スナップ量: プリセットの近傍探索 / 丸め / 表示文字列
//   ・アスペクト: はめ込み（中央寄せ・最大・比の厳密さ）/ フィット
//   ・ビューキューブ: 面 → カメラの向き（その面の側から見る）/ 投影の向き・可視面 3 枚 / クリック判定
//   ・姿勢補間: yaw の最短経路 / 端点 / イージング / 上下の ±89 度制限
//   ・ブックマーク: 保存 → 読み込みの往復 / 壊れた行の読み飛ばし
//   ・選択アウトライン（CPU 版）: 内側は 0・境界の隣は 1・遠方は 0・対称・シザー矩形
//   ・矩形選択の判定
// 標準ライブラリだけ（ImGui も GPU も要らない）。
#include "editor/ViewportLogic.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace dx12e::vp;

namespace
{
int g_checks = 0, g_failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);        \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)
bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void TestSnap()
{
    CHECK(NearestPreset(kSnapTranslate, kSnapTranslateCount, 1.0f) == 3, "1.0 は 4 番目");
    CHECK(NearestPreset(kSnapTranslate, kSnapTranslateCount, 0.9f) == 3, "0.9 → 1.0");
    CHECK(NearestPreset(kSnapTranslate, kSnapTranslateCount, 0.0f) == 0, "0 → 最小");
    CHECK(NearestPreset(kSnapTranslate, kSnapTranslateCount, 99.0f) == kSnapTranslateCount - 1, "巨大 → 最大");
    CHECK(NearestPreset(kSnapRotate, kSnapRotateCount, 15.0f) == 3, "15° は既定");
    CHECK(NearestPreset(kSnapScale, kSnapScaleCount, 0.1f) == 2, "0.1 は既定");
    CHECK(NearestPreset(kSnapRotate, 0, 5.0f) == -1, "空配列は -1");
    // 既定値（EditorContext の snapTranslate=1 / snapRotateDeg=15 / snapScale=0.1）が全部プリセットに載っている
    CHECK(Near(kSnapTranslate[NearestPreset(kSnapTranslate, kSnapTranslateCount, 1.0f)], 1.0f), "既定の位置スナップがプリセット上");
    CHECK(Near(kSnapRotate[NearestPreset(kSnapRotate, kSnapRotateCount, 15.0f)], 15.0f), "既定の回転スナップがプリセット上");
    CHECK(Near(kSnapScale[NearestPreset(kSnapScale, kSnapScaleCount, 0.1f)], 0.1f), "既定のスケールスナップがプリセット上");

    CHECK(Near(SnapValue(1.26f, 0.25f), 1.25f), "1.26 → 1.25");
    CHECK(Near(SnapValue(1.38f, 0.25f), 1.5f), "1.38 → 1.5");
    CHECK(Near(SnapValue(-0.4f, 0.5f), -0.5f), "負の丸め");
    CHECK(Near(SnapValue(7.3f, 0.0f), 7.3f), "step 0 は丸めない");
    CHECK(Near(SnapValue(7.3f, -1.0f), 7.3f), "step 負は丸めない");
    CHECK(Near(SnapValue(23.0f, 15.0f), 30.0f), "23° → 30°");
    CHECK(Near(SnapValue(22.0f, 15.0f), 15.0f), "22° → 15°");

    CHECK(FormatSnap(0, 0.25f) == "0.25 m", "位置の表示: %s", FormatSnap(0, 0.25f).c_str());
    CHECK(FormatSnap(1, 15.0f) == "15\xC2\xB0", "回転の表示");
    CHECK(FormatSnap(2, 0.1f) == "\xC3\x97" "0.1", "スケールの表示: %s", FormatSnap(2, 0.1f).c_str());
}

void TestAspect()
{
    // 横が余る（1200x600 の領域に 16:9）→ 高さ基準・左右に帯
    Rect r = FitAspect(100, 50, 1200, 600, 16.0f / 9.0f);
    CHECK(Near(r.h, 600.0f) && Near(r.w, 600.0f * 16.0f / 9.0f, 0.01f), "横余り: %f x %f", static_cast<double>(r.w), static_cast<double>(r.h));
    CHECK(Near(r.x, 100.0f + (1200.0f - r.w) * 0.5f, 0.01f) && Near(r.y, 50.0f), "中央寄せ");
    // 縦が余る（800x600 に 16:9）→ 幅基準・上下に帯
    r = FitAspect(0, 0, 800, 600, 16.0f / 9.0f);
    CHECK(Near(r.w, 800.0f) && Near(r.h, 450.0f, 0.01f), "縦余り");
    CHECK(Near(r.y, 75.0f, 0.01f), "縦の中央寄せ");
    // フィット（0）は領域そのもの
    r = FitAspect(10, 20, 640, 480, 0.0f);
    CHECK(Near(r.x, 10) && Near(r.y, 20) && Near(r.w, 640) && Near(r.h, 480), "フィット");
    // 比の厳密さ（全プリセット・いろいろな領域）
    for (int i = 0; i < kAspectCount; ++i)
    {
        if (kAspects[i].ratio <= 0.0f) continue;
        for (float w : { 320.0f, 777.0f, 1192.0f, 1920.0f })
            for (float h : { 200.0f, 591.0f, 625.0f, 1080.0f })
            {
                const Rect f = FitAspect(0, 0, w, h, kAspects[i].ratio);
                CHECK(Near(f.w / f.h, kAspects[i].ratio, 1e-3f), "%s %fx%f の比", kAspects[i].label, static_cast<double>(w), static_cast<double>(h));
                CHECK(f.w <= w + 1e-3f && f.h <= h + 1e-3f && f.x >= -1e-3f && f.y >= -1e-3f, "%s 領域内", kAspects[i].label);
                CHECK(Near(f.w, w, 0.01f) || Near(f.h, h, 0.01f), "%s 最大（どちらかは領域いっぱい）", kAspects[i].label);
            }
    }
    // 退化した領域でも 0 除算・負サイズを作らない
    r = FitAspect(0, 0, 0, 0, 16.0f / 9.0f);
    CHECK(r.w >= 1.0f && r.h >= 1.0f && std::isfinite(r.w) && std::isfinite(r.h), "0 領域");
    // 既定は従来と同じ 16:9
    CHECK(Near(kAspects[kAspectDefault].ratio, 16.0f / 9.0f), "既定 = 16:9");
    // 番号と表示名の一意性
    for (int i = 0; i < kAspectCount; ++i)
        for (int j = i + 1; j < kAspectCount; ++j)
            CHECK(std::string(kAspects[i].label) != kAspects[j].label, "ラベル重複");
}

void TestViewCubeAngles()
{
    // その面の側から中心を見る: カメラの forward が面の法線の逆向きになる
    for (int i = 0; i < kCubeFaceCount; ++i)
    {
        const CubeFace f = static_cast<CubeFace>(i);
        const ViewAngles a = LookAnglesFromFace(f, 0.7f);
        float fwd[3], n[3];
        ForwardOf(a.yaw, a.pitch, fwd);
        CubeFaceNormal(f, n);
        const float dot = fwd[0] * n[0] + fwd[1] * n[1] + fwd[2] * n[2];
        CHECK(dot < -0.99f, "面 %d: forward が法線の逆向き (dot=%f)", i, static_cast<double>(dot));
    }
    // 前（-Z 側から見る）は既定のカメラ（yaw 0, pitch 0）そのもの
    const ViewAngles front = LookAnglesFromFace(CubeFace::NegZ, 2.0f);
    CHECK(Near(front.yaw, 0.0f) && Near(front.pitch, 0.0f), "前 = yaw 0");
    // 上下は現在の yaw を保つ（クリックで視界が回らない）+ pitch は制限内
    const ViewAngles top = LookAnglesFromFace(CubeFace::PosY, 1.25f);
    CHECK(Near(top.yaw, 1.25f) && Near(top.pitch, -kMaxPitch), "上 = 真上から");
    const ViewAngles bot = LookAnglesFromFace(CubeFace::NegY, -0.5f);
    CHECK(Near(bot.yaw, -0.5f) && Near(bot.pitch, kMaxPitch), "下 = 真下から");
    // ラベルは全部あり互いに違う
    for (int i = 0; i < kCubeFaceCount; ++i)
    {
        CHECK(std::strlen(CubeFaceLabel(static_cast<CubeFace>(i))) > 0, "ラベル");
        for (int j = i + 1; j < kCubeFaceCount; ++j)
            CHECK(std::string(CubeFaceLabel(static_cast<CubeFace>(i))) != CubeFaceLabel(static_cast<CubeFace>(j)), "ラベル重複");
    }
}

void TestViewCubeProjection()
{
    // 既定のカメラ（yaw 0・pitch 0・+Z を見る）: 見える面は -Z（前）だけ。+X は右側に見える面ではなく真横
    CubeQuad q[kCubeFaceCount];
    ProjectCube(0.0f, 0.0f, 10.0f, q);
    CHECK(q[static_cast<int>(CubeFace::NegZ)].visible, "前は見える");
    CHECK(!q[static_cast<int>(CubeFace::PosZ)].visible, "後ろは見えない");
    // 斜め上から（yaw 少し + pitch 下向き）: 3 面が見える
    ProjectCube(0.6f, -0.5f, 10.0f, q);
    int visible = 0;
    for (const CubeQuad& f : q) visible += f.visible ? 1 : 0;
    CHECK(visible == 3, "斜めからは 3 面が見える（%d）", visible);
    CHECK(q[static_cast<int>(CubeFace::PosY)].visible, "見下ろしなら上面が見える");
    // 真正面（yaw 0, pitch 0）で右の面 +X は画面右側にある（中心 x > 0）→ カメラを少し左へ回すと右面が見える
    ProjectCube(-0.5f, 0.0f, 10.0f, q);   // +X 側へ回り込む
    CHECK(q[static_cast<int>(CubeFace::PosX)].visible, "yaw<0 で +X 面が見える");
    // 投影は原点対称: 面の頂点の重心は面ごとに違い、反対の面（同時に見えない）どうしは反対側
    ProjectCube(0.0f, 0.0f, 10.0f, q);
    const CubeQuad& front = q[static_cast<int>(CubeFace::NegZ)];
    float cx = 0, cy = 0;
    for (int k = 0; k < 4; ++k) { cx += front.x[k]; cy += front.y[k]; }
    CHECK(Near(cx / 4, 0.0f, 1e-3f) && Near(cy / 4, 0.0f, 1e-3f), "正面の面は中心");
    // 面の大きさ = 2*scale の正方形
    CHECK(Near(std::fabs(front.x[1] - front.x[0]), 20.0f, 1e-3f), "面の辺 = 2*scale");
    // 画面の上下: +Y 面は画面の上（y が小さい）にある（斜め下から見上げず、上から見下ろす）
    ProjectCube(0.0f, -0.6f, 10.0f, q);
    const CubeQuad& top = q[static_cast<int>(CubeFace::PosY)];
    float ty = 0;
    for (int k = 0; k < 4; ++k) ty += top.y[k];
    CHECK(top.visible && ty / 4 < 0.0f, "上面は画面の上側に描かれる (y=%f)", static_cast<double>(ty / 4));
}

void TestViewCubeHit()
{
    CubeQuad q[kCubeFaceCount];
    ProjectCube(0.6f, -0.5f, 30.0f, q);
    // 見えている各面の重心をクリックすると、その面に当たる
    int hitCount = 0;
    for (int i = 0; i < kCubeFaceCount; ++i)
    {
        if (!q[i].visible) continue;
        float cx = 0, cy = 0;
        for (int k = 0; k < 4; ++k) { cx += q[i].x[k]; cy += q[i].y[k]; }
        cx /= 4; cy /= 4;
        CHECK(HitCubeFace(q, cx, cy) == i, "面 %d の重心が当たる", i);
        ++hitCount;
    }
    CHECK(hitCount == 3, "見えている面の検査数");
    // 立方体の外は当たらない
    CHECK(HitCubeFace(q, 100.0f, 100.0f) == -1, "外");
    CHECK(HitCubeFace(q, -100.0f, 0.0f) == -1, "外(左)");
    // 見えない面（裏側）は当たらない: 前から見て、後ろ面の位置（同じ画面位置）でも前面が返る
    ProjectCube(0.0f, 0.0f, 30.0f, q);
    CHECK(HitCubeFace(q, 0.0f, 0.0f) == static_cast<int>(CubeFace::NegZ), "正面の中心 → 前");
    // 点と四角形
    const float qx[4] = { 0, 10, 10, 0 }, qy[4] = { 0, 0, 10, 10 };
    CHECK(PointInQuad(qx, qy, 5, 5) && !PointInQuad(qx, qy, 11, 5) && !PointInQuad(qx, qy, 5, -1), "四角形内外");
    const float rx[4] = { 0, 0, 10, 10 }, ry[4] = { 0, 10, 10, 0 };   // 逆回りでも同じ
    CHECK(PointInQuad(rx, ry, 5, 5) && !PointInQuad(rx, ry, 5, 11), "逆回りの四角形");
}

void TestPoseTween()
{
    CHECK(Near(ShortestAngleDelta(0.0f, 1.0f), 1.0f), "差分");
    CHECK(Near(ShortestAngleDelta(3.0f, -3.0f), 2.0f * kPi - 6.0f, 1e-4f), "3 → -3 は短い方（+0.283）");
    CHECK(Near(ShortestAngleDelta(-3.0f, 3.0f), 6.0f - 2.0f * kPi, 1e-4f), "-3 → 3 は逆回り");
    CHECK(Near(ShortestAngleDelta(0.0f, 2.0f * kPi), 0.0f, 1e-4f), "1 周は 0");
    CHECK(Near(EaseOutCubic(0.0f), 0.0f) && Near(EaseOutCubic(1.0f), 1.0f), "端点");
    CHECK(EaseOutCubic(0.5f) > 0.5f, "ease-out は前半が速い");
    CHECK(Near(EaseOutCubic(-1.0f), 0.0f) && Near(EaseOutCubic(2.0f), 1.0f), "範囲外は丸め");

    CameraPose a, b;
    a.pos[0] = 0; a.pos[1] = 0; a.pos[2] = 0; a.yaw = 3.0f; a.pitch = 0.2f;
    b.pos[0] = 10; b.pos[1] = 4; b.pos[2] = -2; b.yaw = -3.0f; b.pitch = -0.4f;
    CameraPose m = LerpPose(a, b, 0.5f);
    CHECK(Near(m.pos[0], 5) && Near(m.pos[1], 2) && Near(m.pos[2], -1), "位置の中点");
    CHECK(Near(m.yaw, 3.0f + (2.0f * kPi - 6.0f) * 0.5f, 1e-4f), "yaw は短い経路（+3.14 側を通る）: %f", static_cast<double>(m.yaw));
    CHECK(Near(m.pitch, -0.1f), "pitch の中点");
    CHECK(Near(LerpPose(a, b, 0.0f).yaw, a.yaw) && Near(LerpPose(a, b, 1.0f).pos[0], 10.0f), "端点");

    // pitch は ±89 度を超えない
    a.pitch = 5.0f;
    CHECK(LerpPose(a, b, 0.0f).pitch <= kMaxPitch + 1e-6f, "pitch 制限");

    // 補間は単調（位置）で、終了時ぴったり目標
    PoseTween tw;
    a.pitch = 0.0f;
    tw.Start(a, b, 0.5f);
    CHECK(tw.running, "開始");
    float prev = -1.0f;
    bool mono = true;
    int steps = 0;
    while (tw.running && steps < 1000)
    {
        const CameraPose p = tw.Step(1.0f / 60.0f);
        if (p.pos[0] < prev - 1e-5f) mono = false;
        prev = p.pos[0];
        ++steps;
    }
    CHECK(mono, "位置が単調に進む");
    CHECK(!tw.running && steps >= 28 && steps <= 32, "0.5 秒 / 60fps ≒ 30 ステップ（%d）", steps);
    CHECK(Near(prev, 10.0f), "終了時は目標ぴったり");
    // 動いていない時は目標を返す・Cancel で止まる
    CHECK(Near(tw.Step(1.0f).pos[0], 10.0f), "停止後は目標");
    tw.Start(a, b, 1.0f);
    tw.Cancel();
    CHECK(!tw.running, "Cancel");

    // ピボット周りの姿勢: 中心から距離 dist だけ後ろに引く
    const float pivot[3] = { 1, 2, 3 };
    const CameraPose p = PoseLookingAt(pivot, 10.0f, 0.0f, 0.0f);   // +Z を見る → カメラは pivot の -Z 側
    CHECK(Near(p.pos[0], 1) && Near(p.pos[1], 2) && Near(p.pos[2], -7), "yaw 0 のカメラは pivot の手前");
    const CameraPose p2 = PoseLookingAt(pivot, 10.0f, -kPi * 0.5f, 0.0f);   // -X を見る → カメラは +X 側
    CHECK(Near(p2.pos[0], 11.0f, 1e-3f) && Near(p2.pos[2], 3.0f, 1e-3f), "右から見るカメラは +X 側");
}

void TestBookmarks()
{
    BookmarkSet s;
    CHECK(s.Serialize().empty(), "空の保存は空");
    s.slot[0].used = true;
    s.slot[0].pose.pos[0] = 1.5f; s.slot[0].pose.pos[1] = -2.25f; s.slot[0].pose.pos[2] = 30.0f;
    s.slot[0].pose.yaw = 0.75f; s.slot[0].pose.pitch = -0.3f; s.slot[0].fovDeg = 60.0f;
    s.slot[8].used = true;
    s.slot[8].pose.pos[0] = -100.0f; s.slot[8].pose.yaw = 3.0f; s.slot[8].fovDeg = 30.0f;
    const std::string text = s.Serialize();
    BookmarkSet t;
    CHECK(t.Parse(text), "往復: 読める");
    for (int i = 0; i < kBookmarkCount; ++i)
    {
        CHECK(t.slot[i].used == s.slot[i].used, "スロット %d の使用有無", i + 1);
        if (!s.slot[i].used) continue;
        for (int c = 0; c < 3; ++c) CHECK(Near(t.slot[i].pose.pos[c], s.slot[i].pose.pos[c], 1e-4f), "位置");
        CHECK(Near(t.slot[i].pose.yaw, s.slot[i].pose.yaw) && Near(t.slot[i].pose.pitch, s.slot[i].pose.pitch), "向き");
        CHECK(Near(t.slot[i].fovDeg, s.slot[i].fovDeg), "FOV");
    }
    CHECK(!t.slot[1].used && !t.slot[4].used, "未使用は未使用のまま");

    // 壊れた行は読み飛ばす（他の行は生きる）
    BookmarkSet u;
    CHECK(u.Parse("garbage\n2 1 2 3 0.5 0.1 70\n11 0 0 0 0 0 45\n3 x y z\n0 0 0 0 0 0 45\n"), "壊れた行が混ざっても読める");
    CHECK(u.slot[1].used && Near(u.slot[1].fovDeg, 70.0f), "2 番は生きる");
    int used = 0;
    for (const Bookmark& b : u.slot) used += b.used ? 1 : 0;
    CHECK(used == 1, "範囲外の番号・不完全な行は捨てる（%d 件）", used);
    // 全部壊れていれば false・全消し
    CHECK(!u.Parse("nothing here\n"), "全部壊れていれば false");
    used = 0;
    for (const Bookmark& b : u.slot) used += b.used ? 1 : 0;
    CHECK(used == 0, "読み直しは前の内容を消す");
    // NaN / 範囲外の値
    BookmarkSet v;
    CHECK(!v.Parse("1 nan 0 0 0 0 45\n"), "NaN の行は捨てる");
    CHECK(v.Parse("1 0 0 0 0 5 999\n") && v.slot[0].pose.pitch <= kMaxPitch + 1e-6f && v.slot[0].fovDeg <= 170.0f, "pitch / FOV は範囲へ丸める");
}

void TestOutline()
{
    // 10x10 の正方形マスクを 40x40 の中央に置く
    const int W = 40, H = 40;
    std::vector<unsigned char> mask(W * H, 0);
    for (int y = 15; y < 25; ++y)
        for (int x = 15; x < 25; ++x) mask[y * W + x] = 255;
    const float core = outline::kCorePx, glow = outline::kGlowPx, gk = outline::kGlowStrength;
    const std::vector<float> ring = outline::RenderRing(mask, W, H, core, glow, gk);

    CHECK(Near(ring[20 * W + 20], 0.0f), "マスクの内側は 0（内側は塗らない）");
    CHECK(Near(ring[20 * W + 25], 1.0f), "境界のすぐ外は 1（右）");
    CHECK(Near(ring[20 * W + 14], 1.0f), "境界のすぐ外は 1（左）");
    CHECK(Near(ring[14 * W + 20], 1.0f) && Near(ring[25 * W + 20], 1.0f), "上下も 1");
    CHECK(Near(ring[0], 0.0f) && Near(ring[39 * W + 39], 0.0f), "遠方は 0");
    // 対称
    for (int i = 0; i < 10; ++i)
    {
        CHECK(Near(ring[(15 + i) * W + 25], ring[(15 + i) * W + 14]), "左右対称");
        CHECK(Near(ring[25 * W + 15 + i], ring[14 * W + 15 + i]), "上下対称");
    }
    // 外へ向かって単調に減る（右方向）
    float prev = 2.0f;
    bool mono = true;
    for (int x = 25; x < 40; ++x)
    {
        const float a = ring[20 * W + x];
        if (a > prev + 1e-6f) mono = false;
        prev = a;
    }
    CHECK(mono, "外へ向かって単調に減る");
    CHECK(ring[20 * W + 25 + 3] > 0.0f && ring[20 * W + 25 + 3] < 1.0f, "3px 外は光（0 と 1 の間）");
    CHECK(Near(ring[20 * W + 25 + 9], 0.0f), "十分外は 0");
    // 光の強さは芯（1）を超えない・負にならない
    for (float a : ring) CHECK(a >= 0.0f && a <= 1.0f, "0..1");

    // 式そのもの（GPU シェーダと同じ）
    CHECK(Near(outline::EdgeAlpha(0.0f, core, glow, gk), 0.0f), "d=0 は内側");
    CHECK(Near(outline::EdgeAlpha(1.0f, core, glow, gk), 1.0f), "d=1 は芯");
    CHECK(outline::EdgeAlpha(2.0f, core, glow, gk) < 1.0f && outline::EdgeAlpha(2.0f, core, glow, gk) > outline::EdgeAlpha(4.0f, core, glow, gk), "減衰");
    CHECK(Near(outline::EdgeAlpha(100.0f, core, glow, gk), 0.0f), "遠方");
    // 倍率が上がると芯が太く・光が広くなる（DPI 追従）
    CHECK(outline::EdgeAlpha(3.0f, core * 2.0f, glow * 2.0f, gk) > outline::EdgeAlpha(3.0f, core, glow, gk), "200%% は同じ距離でも濃い");

    // シザー矩形: 余白を足してビューポートへクランプ
    {
        const Rect r = outline::ScissorFor(100, 100, 200, 150, 8, 0, 0, 1000, 600);
        CHECK(Near(r.x, 92) && Near(r.y, 92) && Near(r.w, 116) && Near(r.h, 66), "余白つき");
        const Rect c = outline::ScissorFor(-50, -50, 20, 20, 8, 0, 0, 1000, 600);
        CHECK(Near(c.x, 0) && Near(c.y, 0) && Near(c.w, 28) && Near(c.h, 28), "左上をクランプ");
        const Rect e = outline::ScissorFor(2000, 2000, 2100, 2100, 8, 0, 0, 1000, 600);
        CHECK(e.w == 0.0f && e.h == 0.0f, "画面外は空");
        const Rect big = outline::ScissorFor(-1e6f, -1e6f, 1e6f, 1e6f, 8, 10, 20, 300, 200);
        CHECK(Near(big.x, 10) && Near(big.y, 20) && Near(big.w, 300) && Near(big.h, 200), "巨大なら全域");
    }
}

void TestViewModes()
{
    // RenderDebugMode の数値（renderer/RenderDebugPass.h。ApplicationRender.cpp が static_assert で突き合わせる）
    CHECK(ViewModeToDebugPass(kViewModeLit) == 0, "ライティングあり = 0（何も被せない）");
    CHECK(ViewModeToDebugPass(kViewModeDepth) == 4 && ViewModeToDebugPass(kViewModeNormal) == 1, "デプス / 法線");
    CHECK(ViewModeToDebugPass(kViewModeRoughness) == 2 && ViewModeToDebugPass(kViewModeMetallic) == 3 && ViewModeToDebugPass(kViewModeAo) == 5, "ラフネス / メタリック / AO");
    CHECK(ViewModeToDebugPass(kViewModeWire) == 0, "ワイヤは別経路（RenderDebugPass を使わない）");
    CHECK(ViewModeToDebugPass(kViewModeLightComplexity) == 0 && ViewModeToDebugPass(kViewModeClusterGrid) == 0, "クラスタ診断も別経路");
    CHECK(ViewModeToDebugPass(-1) == 0 && ViewModeToDebugPass(99) == 0, "範囲外は 0");
    CHECK(ViewModeToClusterDebug(kViewModeLightComplexity) == 1 && ViewModeToClusterDebug(kViewModeClusterGrid) == 2, "クラスタ診断へ振り分け");
    CHECK(ViewModeToClusterDebug(kViewModeLit) == 0 && ViewModeToClusterDebug(kViewModeDepth) == 0 && ViewModeToClusterDebug(kViewModeWire) == 0, "他のモードでは 0");
    // 全モードが一意に振り分けられる（デバッグパスかクラスタ診断か、どちらでもなければライティング / ワイヤだけ）
    for (int m = 0; m < kViewModeCount; ++m)
    {
        const bool viaPass = ViewModeToDebugPass(m) != 0, viaCluster = ViewModeToClusterDebug(m) != 0;
        CHECK(!(viaPass && viaCluster), "モード %d が両方へ振り分けられている", m);
        if (!viaPass && !viaCluster) CHECK(m == kViewModeLit || m == kViewModeWire, "モード %d はどこにも振り分けられない", m);
    }
}

void TestMarquee()
{
    const MarqueeRect m = MakeMarquee(50, 80, 10, 20);
    CHECK(m.x0 == 10 && m.y0 == 20 && m.x1 == 50 && m.y1 == 80, "正規化");
    CHECK(MarqueeOverlaps(m, 40, 70, 60, 90), "角が重なる");
    CHECK(MarqueeOverlaps(m, 20, 30, 30, 40), "内包");
    CHECK(MarqueeOverlaps(m, 0, 0, 100, 100), "包む");
    CHECK(!MarqueeOverlaps(m, 51, 0, 60, 100), "右に外れる");
    CHECK(!MarqueeOverlaps(m, 0, 81, 100, 90), "下に外れる");
    CHECK(kMarqueeThresholdPx >= 3.0f, "クリックとの切り分け閾値");
}
} // namespace

int main()
{
    TestSnap();
    TestAspect();
    TestViewCubeAngles();
    TestViewCubeProjection();
    TestViewCubeHit();
    TestPoseTween();
    TestBookmarks();
    TestOutline();
    TestViewModes();
    TestMarquee();
    std::printf("ViewportLogicTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
