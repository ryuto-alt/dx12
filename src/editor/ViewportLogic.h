#pragma once

// ===========================================================================
// ビューポート周りの純ロジック（フェーズ 1a）。標準ライブラリだけ・ヘッダオンリー
// ＝ImGui も GPU も要らない。tests/viewport_logic_test.cpp が単体で検証する。
//
//   ・スナップ量のプリセットと丸め
//   ・ビューポートのアスペクト比プリセットと、領域へのはめ込み
//   ・ビューキューブ（方向ギズモ）: 面 → カメラの向き / カメラの向き → 立方体の投影 / クリック判定
//   ・カメラ姿勢のなめらかな補間（ビューキューブ・ブックマーク呼び出し用）
//   ・カメラブックマーク（1〜9）の保存形式
//   ・選択アウトラインの CPU 版（マスク → 距離 → 縁の濃さ。GPU シェーダ SelectionOutline.hlsl と同じ式）
//
// ★座標系は Camera.cpp と同じ: LH・Y 上・yaw 0 で +Z を向く・forward = (sin y cos p, sin p, cos y cos p)。
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace dx12e::vp
{

inline constexpr float kPi = 3.14159265358979f;
// Camera::Rotate と同じ ±89 度の制限（真上/真下で右手が退化しない範囲）
inline constexpr float kMaxPitch = kPi * 0.5f - 0.01f;

// ---------------------------------------------------------------------------
// スナップ
// ---------------------------------------------------------------------------
inline constexpr float kSnapTranslate[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f };   // m
inline constexpr float kSnapRotate[]    = { 1.0f, 5.0f, 10.0f, 15.0f, 30.0f, 45.0f, 90.0f };   // 度
inline constexpr float kSnapScale[]     = { 0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f };            // 倍率
inline constexpr int kSnapTranslateCount = static_cast<int>(sizeof(kSnapTranslate) / sizeof(kSnapTranslate[0]));
inline constexpr int kSnapRotateCount    = static_cast<int>(sizeof(kSnapRotate) / sizeof(kSnapRotate[0]));
inline constexpr int kSnapScaleCount     = static_cast<int>(sizeof(kSnapScale) / sizeof(kSnapScale[0]));

// v に最も近いプリセットの番号（同じ距離なら小さい方）。空配列は -1。
inline int NearestPreset(const float* arr, int n, float v)
{
    int best = -1;
    float bestD = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float d = std::fabs(arr[i] - v);
        if (best < 0 || d < bestD - 1e-9f) { best = i; bestD = d; }
    }
    return best;
}

// step の倍数へ丸める。step <= 0 は丸めない（スナップ無し）。
inline float SnapValue(float v, float step)
{
    if (!(step > 0.0f) || !std::isfinite(v)) return v;
    return std::round(v / step) * step;
}

// スナップ量の表示文字列（"0.25 m" / "15°" / "×0.1"）。kind: 0=位置 1=回転 2=スケール
inline std::string FormatSnap(int kind, float v)
{
    char buf[48];
    if (kind == 1)      std::snprintf(buf, sizeof(buf), "%g\xC2\xB0", static_cast<double>(v));
    else if (kind == 2) std::snprintf(buf, sizeof(buf), "\xC3\x97%g", static_cast<double>(v));
    else                std::snprintf(buf, sizeof(buf), "%g m", static_cast<double>(v));
    return buf;
}

// ---------------------------------------------------------------------------
// アスペクト比
// ---------------------------------------------------------------------------
struct AspectPreset
{
    const char* label;
    float       ratio;   // 幅 / 高さ。0 = 領域いっぱい（フィット）
};
inline constexpr AspectPreset kAspects[] = {
    { "16:9",       16.0f / 9.0f },   // 既定（従来の固定と同じ）
    { "16:10",      16.0f / 10.0f },
    { "21:9",       21.0f / 9.0f },
    { "4:3",        4.0f / 3.0f },
    { "1:1",        1.0f },
    { "9:16 縦",    9.0f / 16.0f },
    { "フィット",    0.0f },
};
inline constexpr int kAspectCount = static_cast<int>(sizeof(kAspects) / sizeof(kAspects[0]));
inline constexpr int kAspectDefault = 0;

struct Rect
{
    float x = 0, y = 0, w = 0, h = 0;
};

// 領域 [x,y,w,h] に収まる最大の aspect の矩形を中央寄せで返す。aspect <= 0 は領域そのもの。
// 領域が 1px 未満でも 1 へ丸める（0 除算・負のサイズを作らない）。
inline Rect FitAspect(float x, float y, float w, float h, float aspect)
{
    if (w < 1.0f) w = 1.0f;
    if (h < 1.0f) h = 1.0f;
    Rect r{ x, y, w, h };
    if (!(aspect > 0.0f) || !std::isfinite(aspect)) return r;
    if (w / h > aspect) r.w = h * aspect;   // 横が余る → 左右に帯
    else                r.h = w / aspect;   // 縦が余る → 上下に帯
    r.w = std::max(r.w, 1.0f);              // 退化した領域でも 1px 未満にしない
    r.h = std::max(r.h, 1.0f);
    r.x = x + (w - r.w) * 0.5f;
    r.y = y + (h - r.h) * 0.5f;
    return r;
}

// ---------------------------------------------------------------------------
// カメラ姿勢・補間
// ---------------------------------------------------------------------------
struct CameraPose
{
    float pos[3] = { 0, 0, 0 };
    float yaw = 0, pitch = 0;
};

inline void ForwardOf(float yaw, float pitch, float out[3])
{
    const float cp = std::cos(pitch);
    out[0] = std::sin(yaw) * cp;
    out[1] = std::sin(pitch);
    out[2] = std::cos(yaw) * cp;
}

// [-pi, pi] へ畳んだ角度差（from → to の最短経路）
inline float ShortestAngleDelta(float from, float to)
{
    float d = std::fmod(to - from, 2.0f * kPi);
    if (d > kPi) d -= 2.0f * kPi;
    if (d < -kPi) d += 2.0f * kPi;
    return d;
}

inline float ClampPitch(float p) { return std::clamp(p, -kMaxPitch, kMaxPitch); }

inline float EaseOutCubic(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

// yaw は最短経路で補間する（350° → 10° で逆回りしない）。t は 0..1（呼び出し側でイージング済みの値）。
inline CameraPose LerpPose(const CameraPose& a, const CameraPose& b, float t)
{
    CameraPose r;
    for (int i = 0; i < 3; ++i) r.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
    r.yaw = a.yaw + ShortestAngleDelta(a.yaw, b.yaw) * t;
    r.pitch = ClampPitch(a.pitch + (b.pitch - a.pitch) * t);
    return r;
}

// pivot の周りを dist 離れて yaw/pitch の向きで見る姿勢（pos = pivot - forward * dist）。
inline CameraPose PoseLookingAt(const float pivot[3], float dist, float yaw, float pitch)
{
    CameraPose p;
    float f[3];
    ForwardOf(yaw, ClampPitch(pitch), f);
    for (int i = 0; i < 3; ++i) p.pos[i] = pivot[i] - f[i] * dist;
    p.yaw = yaw;
    p.pitch = ClampPitch(pitch);
    return p;
}

// カメラ姿勢のなめらかな遷移（ビューキューブのクリック / ブックマーク呼び出し）。
struct PoseTween
{
    CameraPose from, to;
    float t = 1.0f;        // 経過（0..1）
    float duration = 0.32f; // 秒
    bool  running = false;

    void Start(const CameraPose& a, const CameraPose& b, float seconds = 0.32f)
    {
        from = a; to = b; t = 0.0f; duration = seconds > 1e-4f ? seconds : 1e-4f; running = true;
    }
    void Cancel() { running = false; }
    // dt 進めて現在の姿勢を返す。終わったら running=false（最後の呼び出しは to そのもの）。
    CameraPose Step(float dt)
    {
        if (!running) return to;
        t += dt / duration;
        if (t >= 1.0f) { t = 1.0f; running = false; return to; }
        return LerpPose(from, to, EaseOutCubic(t));
    }
};

// ---------------------------------------------------------------------------
// ビューモード → RenderDebugPass のモード番号（renderer/RenderDebugPass.h の RenderDebugMode と同じ数値。
// ApplicationRender.cpp が static_assert で突き合わせている）。0 = 通常描画（ワイヤは別経路）。
// ---------------------------------------------------------------------------
inline constexpr int kViewModeLit = 0, kViewModeDepth = 1, kViewModeNormal = 2, kViewModeRoughness = 3,
                     kViewModeMetallic = 4, kViewModeAo = 5, kViewModeWire = 6,
                     kViewModeLightComplexity = 7, kViewModeClusterGrid = 8;   // EditorContext::clusterDebugMode（1 / 2）へ振り分ける
inline constexpr int kViewModeCount = 9;
// ビューモード → EditorContext::clusterDebugMode（0=なし / 1=ライト複雑度 / 2=クラスタ境界）
constexpr unsigned ViewModeToClusterDebug(int viewMode)
{
    return viewMode == kViewModeLightComplexity ? 1u : viewMode == kViewModeClusterGrid ? 2u : 0u;
}
constexpr unsigned ViewModeToDebugPass(int viewMode)
{
    switch (viewMode)
    {
    case kViewModeDepth:     return 4;   // RenderDebugMode::Depth
    case kViewModeNormal:    return 1;   // Normal
    case kViewModeRoughness: return 2;   // Roughness
    case kViewModeMetallic:  return 3;   // Metallic
    case kViewModeAo:        return 5;   // Ao
    default:                 return 0;
    }
}

// ---------------------------------------------------------------------------
// ビューキューブ
// ---------------------------------------------------------------------------
enum class CubeFace : int { PosX = 0, NegX, PosY, NegY, PosZ, NegZ };
inline constexpr int kCubeFaceCount = 6;

// 面の日本語ラベル。面の外側（その軸の正/負の側）からシーンを見る、というビュー名で揃える。
//   -Z 側から見る = エディタカメラの既定の向き（yaw 0 で +Z を向く）＝「前」
inline const char* CubeFaceLabel(CubeFace f)
{
    switch (f)
    {
    case CubeFace::PosX: return "\xE5\x8F\xB3";   // 右
    case CubeFace::NegX: return "\xE5\xB7\xA6";   // 左
    case CubeFace::PosY: return "\xE4\xB8\x8A";   // 上
    case CubeFace::NegY: return "\xE4\xB8\x8B";   // 下
    case CubeFace::PosZ: return "\xE5\xBE\x8C";   // 後
    case CubeFace::NegZ: return "\xE5\x89\x8D";   // 前
    }
    return "";
}

// 面の法線（ワールド）
inline void CubeFaceNormal(CubeFace f, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    switch (f)
    {
    case CubeFace::PosX: out[0] = 1; break;
    case CubeFace::NegX: out[0] = -1; break;
    case CubeFace::PosY: out[1] = 1; break;
    case CubeFace::NegY: out[1] = -1; break;
    case CubeFace::PosZ: out[2] = 1; break;
    case CubeFace::NegZ: out[2] = -1; break;
    }
}

struct ViewAngles
{
    float yaw = 0, pitch = 0;
};

// その面の側から中心を見るカメラの向き。上/下は真上/真下（yaw は今のものを保つ＝視界が回らない）。
inline ViewAngles LookAnglesFromFace(CubeFace f, float currentYaw)
{
    switch (f)
    {
    case CubeFace::PosX: return { -kPi * 0.5f, 0.0f };   // +X 側から -X を見る
    case CubeFace::NegX: return {  kPi * 0.5f, 0.0f };
    case CubeFace::PosZ: return {  kPi,        0.0f };
    case CubeFace::NegZ: return {  0.0f,       0.0f };
    case CubeFace::PosY: return { currentYaw, -kMaxPitch };   // 上から見下ろす
    case CubeFace::NegY: return { currentYaw,  kMaxPitch };   // 下から見上げる
    }
    return { currentYaw, 0.0f };
}

// カメラの右・上ベクトル（Camera::UpdateVectors と同じ。right = cross(worldUp, forward)）
inline void CameraBasis(float yaw, float pitch, float right[3], float up[3], float fwd[3])
{
    ForwardOf(yaw, pitch, fwd);
    // cross((0,1,0), fwd) = (fwd.z, 0, -fwd.x)
    float rx = fwd[2], ry = 0.0f, rz = -fwd[0];
    const float len = std::sqrt(rx * rx + rz * rz);
    if (len < 1e-6f) { rx = std::cos(yaw); rz = -std::sin(yaw); }   // 真上/真下の退化は yaw から合成
    else { rx /= len; rz /= len; }
    right[0] = rx; right[1] = ry; right[2] = rz;
    // up = cross(fwd, right)
    up[0] = fwd[1] * right[2] - fwd[2] * right[1];
    up[1] = fwd[2] * right[0] - fwd[0] * right[2];
    up[2] = fwd[0] * right[1] - fwd[1] * right[0];
}

// 画面へ投影した 1 面。座標は立方体の中心が (0,0)、半辺 = scale のスクリーン座標（+X 右 / +Y 下）。
struct CubeQuad
{
    CubeFace face = CubeFace::PosX;
    float x[4] = {}, y[4] = {};
    float depth = 0.0f;      // 小さいほど手前（面の中心のカメラ前方距離）
    bool  visible = false;   // カメラ側を向いている面だけ true
};

// 正射でカメラの向き (yaw, pitch) から立方体の 6 面を投影する。
inline void ProjectCube(float yaw, float pitch, float scale, CubeQuad out[kCubeFaceCount])
{
    float r[3], u[3], f[3];
    CameraBasis(yaw, ClampPitch(pitch), r, u, f);
    for (int i = 0; i < kCubeFaceCount; ++i)
    {
        const CubeFace face = static_cast<CubeFace>(i);
        float n[3];
        CubeFaceNormal(face, n);
        // 面内の 2 本の接線（法線と直交する軸 2 本）
        float t1[3] = { 0, 0, 0 }, t2[3] = { 0, 0, 0 };
        const int a = (i / 2 == 0) ? 1 : 0;
        const int b = (i / 2 == 2) ? 1 : 2;
        t1[a] = 1.0f;
        t2[b] = 1.0f;
        static const int sx[4] = { -1, 1, 1, -1 };
        static const int sy[4] = { -1, -1, 1, 1 };
        CubeQuad q;
        q.face = face;
        for (int k = 0; k < 4; ++k)
        {
            float p[3];
            for (int c = 0; c < 3; ++c) p[c] = n[c] + t1[c] * sx[k] + t2[c] * sy[k];
            const float px = p[0] * r[0] + p[1] * r[1] + p[2] * r[2];
            const float py = p[0] * u[0] + p[1] * u[1] + p[2] * u[2];
            q.x[k] = px * scale;
            q.y[k] = -py * scale;   // スクリーンは Y 下向き
        }
        const float facing = -(n[0] * f[0] + n[1] * f[1] + n[2] * f[2]);   // 法線がカメラを向いているほど正
        q.visible = facing > 1e-4f;
        q.depth = n[0] * f[0] + n[1] * f[1] + n[2] * f[2];   // 面の中心のカメラ前方距離（手前ほど小さい）
        out[i] = q;
    }
}

// 点 (px,py) が凸四角形（頂点は順に回る）の内側か。
inline bool PointInQuad(const float qx[4], const float qy[4], float px, float py)
{
    float sign = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        const int j = (i + 1) & 3;
        const float cross = (qx[j] - qx[i]) * (py - qy[i]) - (qy[j] - qy[i]) * (px - qx[i]);
        if (std::fabs(cross) < 1e-6f) continue;
        if (sign == 0.0f) sign = cross > 0.0f ? 1.0f : -1.0f;
        else if ((cross > 0.0f ? 1.0f : -1.0f) != sign) return false;
    }
    return true;
}

// クリック位置 (px,py)（立方体中心基準）が当たった面。当たり無しは -1。見えている面の中で最も手前を返す。
inline int HitCubeFace(const CubeQuad q[kCubeFaceCount], float px, float py)
{
    int best = -1;
    float bestDepth = 0.0f;
    for (int i = 0; i < kCubeFaceCount; ++i)
    {
        if (!q[i].visible) continue;
        if (!PointInQuad(q[i].x, q[i].y, px, py)) continue;
        if (best < 0 || q[i].depth < bestDepth) { best = i; bestDepth = q[i].depth; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// カメラブックマーク（1〜9）
// ---------------------------------------------------------------------------
inline constexpr int kBookmarkCount = 9;

struct Bookmark
{
    bool used = false;
    CameraPose pose;
    float fovDeg = 45.0f;
};

struct BookmarkSet
{
    Bookmark slot[kBookmarkCount];

    // 1 行 1 件: "<番号 1..9> x y z yaw pitch fov"。番号順・未使用は出さない。
    std::string Serialize() const
    {
        std::string s;
        char buf[160];
        for (int i = 0; i < kBookmarkCount; ++i)
        {
            const Bookmark& b = slot[i];
            if (!b.used) continue;
            std::snprintf(buf, sizeof(buf), "%d %.6g %.6g %.6g %.6g %.6g %.6g\n", i + 1,
                          static_cast<double>(b.pose.pos[0]), static_cast<double>(b.pose.pos[1]),
                          static_cast<double>(b.pose.pos[2]), static_cast<double>(b.pose.yaw),
                          static_cast<double>(b.pose.pitch), static_cast<double>(b.fovDeg));
            s += buf;
        }
        return s;
    }

    // 壊れた行は読み飛ばす（全体を捨てない）。1 件でも読めたら true。
    bool Parse(const std::string& text)
    {
        for (Bookmark& b : slot) b = Bookmark{};
        bool any = false;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            int idx = 0;
            float v[6] = {};
            std::istringstream is(line);
            is >> idx >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5];
            if (is.fail()) continue;   // 数が足りない / 数でない（"nan" も失敗する）行は読み飛ばす
            if (idx < 1 || idx > kBookmarkCount) continue;
            bool finite = true;
            for (float f : v) if (!std::isfinite(f)) finite = false;
            if (!finite) continue;
            Bookmark& b = slot[idx - 1];
            b.used = true;
            b.pose.pos[0] = v[0]; b.pose.pos[1] = v[1]; b.pose.pos[2] = v[2];
            b.pose.yaw = v[3];
            b.pose.pitch = ClampPitch(v[4]);
            b.fovDeg = std::clamp(v[5], 5.0f, 170.0f);
            any = true;
        }
        return any;
    }
};

// ---------------------------------------------------------------------------
// 選択アウトライン（CPU 版。GPU の SelectionOutline.hlsl と同じ式）
// ---------------------------------------------------------------------------
namespace outline
{
// 縁の太さ（100% 表示の px）。芯 = くっきりした線 / 外側 = 淡く広がる光。
inline constexpr float kCorePx = 1.6f;
inline constexpr float kGlowPx = 5.0f;
inline constexpr float kGlowStrength = 0.30f;

// マスク外の 1 点の縁の濃さ。d = 最も近いマスク画素までの距離(px)。マスクの内側（d <= 0）は 0（内側は塗らない）。
// 芯: d が [1, 1 + core] の間で 1（境界はなだらか）/ 光: 芯の外側で 2 乗に減衰。
inline float EdgeAlpha(float d, float corePx, float glowPx, float glowStrength)
{
    if (!(d > 0.0f)) return 0.0f;
    const float core = std::clamp(corePx + 0.5f - (d - 0.5f), 0.0f, 1.0f);   // d=1 → corePx 以上なら 1
    const float g = std::clamp(1.0f - (d - 0.5f) / std::max(glowPx, 1e-3f), 0.0f, 1.0f);
    const float glow = glowStrength * g * g;
    return std::max(core, glow);
}

// 距離 0..radius を探索して最短距離を返す（マスク画素 = mask[y*w+x] != 0）。見つからなければ radius+1。
inline float NearestMaskDistance(const std::vector<unsigned char>& mask, int w, int h, int x, int y, int radius)
{
    float best = static_cast<float>(radius) + 1.0f;
    for (int dy = -radius; dy <= radius; ++dy)
    {
        const int yy = y + dy;
        if (yy < 0 || yy >= h) continue;
        for (int dx = -radius; dx <= radius; ++dx)
        {
            const int xx = x + dx;
            if (xx < 0 || xx >= w) continue;
            if (mask[static_cast<size_t>(yy) * w + xx] == 0) continue;
            const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            if (d < best) best = d;
        }
    }
    return best;
}

// マスク全体の縁の濃さ（0..1）。マスクの内側は 0。GPU 版の検算用（単体テスト）。
inline std::vector<float> RenderRing(const std::vector<unsigned char>& mask, int w, int h,
                                     float corePx, float glowPx, float glowStrength)
{
    std::vector<float> out(static_cast<size_t>(w) * h, 0.0f);
    const int radius = static_cast<int>(std::ceil(glowPx + corePx)) + 1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            if (mask[static_cast<size_t>(y) * w + x] != 0) continue;
            const float d = NearestMaskDistance(mask, w, h, x, y, radius);
            out[static_cast<size_t>(y) * w + x] = EdgeAlpha(d, corePx, glowPx, glowStrength);
        }
    return out;
}

// 画面矩形（スクリーン座標）にアウトラインの探索半径ぶん余白を足し、ビューポート内へクランプする。
// アウトラインの合成パスのシザーに使う（対象の周りだけ重い探索をする）。空なら w/h = 0。
inline Rect ScissorFor(float minX, float minY, float maxX, float maxY, float marginPx,
                       float vpX, float vpY, float vpW, float vpH)
{
    Rect r;
    const float x0 = std::max(vpX, minX - marginPx), y0 = std::max(vpY, minY - marginPx);
    const float x1 = std::min(vpX + vpW, maxX + marginPx), y1 = std::min(vpY + vpH, maxY + marginPx);
    if (x1 <= x0 || y1 <= y0) return r;
    r.x = x0; r.y = y0; r.w = x1 - x0; r.h = y1 - y0;
    return r;
}
} // namespace outline

// ---------------------------------------------------------------------------
// 矩形選択（ビューポート上のドラッグ）
// ---------------------------------------------------------------------------
// ドラッグ開始からこの距離(px)を超えるまでは「ただのクリック」（クリック選択と矩形選択の切り分け）。
inline constexpr float kMarqueeThresholdPx = 4.0f;

struct MarqueeRect
{
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // 正規化済み（x0<=x1, y0<=y1）
};

inline MarqueeRect MakeMarquee(float ax, float ay, float bx, float by)
{
    return { std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by) };
}

// 矩形 m と、点 (px,py) を中心とする矩形（スクリーン上の AABB）が重なるか。
inline bool MarqueeOverlaps(const MarqueeRect& m, float minX, float minY, float maxX, float maxY)
{
    return !(maxX < m.x0 || minX > m.x1 || maxY < m.y0 || minY > m.y1);
}

} // namespace dx12e::vp
