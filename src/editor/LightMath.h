#pragma once

// ===== シーンビューのライティング編集で使う「純粋な数学」 =====
// ImGui / entt / GPU に一切依存しないヘッダオンリー。ここに閉じ込めてあるので
// ヘッドレスの単体テスト（tests/light_math_test.cpp）でそのまま検証できる。
//
// 使う側:
//   editor/LightHandles.cpp        … 太陽ドラッグ / コーン・range ハンドルの当たり判定と変換
//   editor/panels/LightingPanel.cpp … 方位/高度スライダ・時刻プリセット・色温度
//
// 【向きの規約】DirectionalLight::direction / SpotLight::direction は
//   「光が進む向き」（太陽が真上なら (0,-1,0)）。太陽が見える方向はその逆ベクトル。
//   方位/高度は常に「太陽が見える方向」で表す（人間の直感に合う側）。

#include <DirectXMath.h>
#include <cmath>

#include "core/Types.h"

namespace dx12e::lightmath
{

// ---------------------------------------------------------------------------
// 小物
// ---------------------------------------------------------------------------

inline f32 Clampf(f32 v, f32 lo, f32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline f32 Lerpf(f32 a, f32 b, f32 t)
{
    return a + (b - a) * t;
}

// -180 < deg <= 180 へ畳む（非有限値は 0 に潰す＝ドラッグ中に NaN が伝播しない）
inline f32 WrapDeg180(f32 deg)
{
    if (!std::isfinite(deg)) return 0.0f;
    f32 d = std::fmod(deg + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

// ---------------------------------------------------------------------------
// 太陽の方位 / 高度  ⇔  direction ベクトル
// ---------------------------------------------------------------------------

// azimuthDeg   : +Z を 0°、+X を 90° とする方位角（-180..180）
// elevationDeg : 地平線 0°、天頂 +90°（実用上 -89..89 にクランプして使う）
struct SunAngles
{
    f32 azimuthDeg   = 0.0f;
    f32 elevationDeg = 45.0f;
};

inline SunAngles DirectionToSunAngles(const DirectX::XMFLOAT3& direction)
{
    // 太陽が見える方向 = -direction
    f32 x = -direction.x, y = -direction.y, z = -direction.z;
    const f32 len = std::sqrt(x * x + y * y + z * z);
    if (!(len > 1e-6f)) return SunAngles{};   // 0 ベクトル / NaN は既定値へ
    x /= len; y /= len; z /= len;

    SunAngles a;
    a.azimuthDeg   = DirectX::XMConvertToDegrees(std::atan2(x, z));
    a.elevationDeg = DirectX::XMConvertToDegrees(std::asin(Clampf(y, -1.0f, 1.0f)));
    return a;
}

inline DirectX::XMFLOAT3 SunAnglesToDirection(const SunAngles& a)
{
    const f32 az = DirectX::XMConvertToRadians(a.azimuthDeg);
    const f32 el = DirectX::XMConvertToRadians(a.elevationDeg);
    const f32 ce = std::cos(el);
    // 太陽が見える方向
    const f32 sx = ce * std::sin(az);
    const f32 sy = std::sin(el);
    const f32 sz = ce * std::cos(az);
    return { -sx, -sy, -sz };   // 光が進む向きへ戻す
}

// マウス移動量(px) → 方位/高度。右へ動かすと方位が進み、下へ動かすと太陽が沈む。
// 高度は ±89° でクランプ（±90 ちょうどだと方位が定義できなくなり、離した瞬間に方位が飛ぶ）。
inline SunAngles ApplySunDrag(const SunAngles& cur, f32 dxPixels, f32 dyPixels, f32 degPerPixel)
{
    SunAngles a;
    a.azimuthDeg   = WrapDeg180(cur.azimuthDeg + dxPixels * degPerPixel);
    a.elevationDeg = Clampf(cur.elevationDeg - dyPixels * degPerPixel, -89.0f, 89.0f);
    return a;
}

// ---------------------------------------------------------------------------
// 時刻（0..24）→ 太陽。Lua の Lighting.sample(hour) と同じ式・同じ定数。
// エディタのスライダと Lua の Lighting.setTimeOfDay が食い違わないよう、
// カーブはここ 1 箇所に写して両方から同じ結果を得る。
// ---------------------------------------------------------------------------

struct TimeOfDaySample
{
    DirectX::XMFLOAT3 direction{0.0f, -1.0f, 0.0f};   // 光が進む向き
    DirectX::XMFLOAT3 color{1.0f, 1.0f, 1.0f};
    f32               intensity = 1.0f;
    f32               ambient   = 0.25f;
};

inline TimeOfDaySample SampleTimeOfDay(f32 hour)
{
    // ScriptEngine.cpp の Lighting.dayColor 等と同じ既定値
    const DirectX::XMFLOAT3 kDayColor  {1.00f, 0.97f, 0.92f};
    const DirectX::XMFLOAT3 kDuskColor {1.00f, 0.46f, 0.18f};
    const DirectX::XMFLOAT3 kNightColor{0.40f, 0.52f, 0.85f};
    const f32 kDayIntensity   = 3.0f;
    const f32 kNightIntensity = 0.35f;
    const f32 kDayAmbient     = 0.30f;
    const f32 kNightAmbient   = 0.05f;
    const f32 kDuskAmbient    = 0.12f;

    f32 h = std::fmod(std::isfinite(hour) ? hour : 12.0f, 24.0f);
    if (h < 0.0f) h += 24.0f;

    const f32 a = (h - 6.0f) / 12.0f * DirectX::XM_PI;   // 6時=東の地平線 / 12時=天頂 / 18時=西
    f32 sx = -std::cos(a);
    f32 sy =  std::sin(a);
    f32 sz =  0.35f;

    const bool night = (sy <= 0.0f);
    if (night) { sx = -sx; sy = -sy; sz = -sz; }         // 夜は月（太陽の反対側）を光源にする

    const f32 len = std::sqrt(sx * sx + sy * sy + sz * sz);
    f32 t = Clampf((sy / len) / 0.35f, 0.0f, 1.0f);      // 地平線=0 → 20度ほど昇れば 1
    t = t * t * (3.0f - 2.0f * t);                       // smoothstep

    const DirectX::XMFLOAT3& lo = night ? kNightColor : kDuskColor;
    const DirectX::XMFLOAT3& hi = night ? kNightColor : kDayColor;
    const f32 imax = night ? kNightIntensity : kDayIntensity;
    const f32 amb1 = night ? kNightAmbient   : kDayAmbient;

    TimeOfDaySample s;
    s.direction = { -sx / len, -sy / len, -sz / len };
    s.color     = { Lerpf(lo.x, hi.x, t), Lerpf(lo.y, hi.y, t), Lerpf(lo.z, hi.z, t) };
    s.intensity = imax * t;
    s.ambient   = Lerpf(kDuskAmbient, amb1, t);
    return s;
}

// ---------------------------------------------------------------------------
// 色温度（ケルビン）→ RGB。Tanner Helland の近似式（1000..40000K）。
// 「電球色にしたい」を数値ではなくボタンで選べるようにするための変換。
// ---------------------------------------------------------------------------
inline DirectX::XMFLOAT3 KelvinToRGB(f32 kelvin)
{
    const f32 t = Clampf(kelvin, 1000.0f, 40000.0f) / 100.0f;

    f32 r, g, b;
    if (t <= 66.0f)
    {
        r = 1.0f;
        g = Clampf((99.4708025861f * std::log(t) - 161.1195681661f) / 255.0f, 0.0f, 1.0f);
        b = (t <= 19.0f)
            ? 0.0f
            : Clampf((138.5177312231f * std::log(t - 10.0f) - 305.0447927307f) / 255.0f, 0.0f, 1.0f);
    }
    else
    {
        r = Clampf((329.698727446f * std::pow(t - 60.0f, -0.1332047592f)) / 255.0f, 0.0f, 1.0f);
        g = Clampf((288.1221695283f * std::pow(t - 60.0f, -0.0755148492f)) / 255.0f, 0.0f, 1.0f);
        b = 1.0f;
    }
    return { r, g, b };
}

// ---------------------------------------------------------------------------
// 投影 / 幾何（ハンドルの位置決めと当たり判定）
// ---------------------------------------------------------------------------

// ワールド → スクリーン（ImGui 座標）。viewProj は
//   XMStoreFloat4x4(&m, camera.GetViewProjMatrix())
// をそのまま渡す（行ベクトル規約。HLSL 用の転置はしない）。
// カメラ後方 / near より手前なら false（戻り値が false のときは outScreen を使わないこと）。
inline bool WorldToScreen(const DirectX::XMFLOAT4X4& viewProj,
                          const DirectX::XMFLOAT3& world,
                          f32 vpX, f32 vpY, f32 vpW, f32 vpH,
                          DirectX::XMFLOAT2& outScreen)
{
    using namespace DirectX;
    const XMMATRIX vp = XMLoadFloat4x4(&viewProj);
    const XMVECTOR clip = XMVector4Transform(XMVectorSet(world.x, world.y, world.z, 1.0f), vp);
    const f32 w = XMVectorGetW(clip);
    if (!(w > 1e-6f)) return false;

    const f32 ndcX = XMVectorGetX(clip) / w;
    const f32 ndcY = XMVectorGetY(clip) / w;
    const f32 ndcZ = XMVectorGetZ(clip) / w;
    if (ndcZ < 0.0f) return false;   // near より手前（正射でも同じ判定でよい）

    outScreen.x = vpX + (ndcX * 0.5f + 0.5f) * vpW;
    outScreen.y = vpY + (0.5f - ndcY * 0.5f) * vpH;
    return true;
}

inline f32 ScreenDistance(const DirectX::XMFLOAT2& a, const DirectX::XMFLOAT2& b)
{
    const f32 dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// dir（正規化済み前提）に直交する単位ベクトル 2 本。dir が上下を向いていても破綻しない。
inline void BuildBasis(const DirectX::XMFLOAT3& dir,
                       DirectX::XMFLOAT3& outU, DirectX::XMFLOAT3& outV)
{
    using namespace DirectX;
    XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&dir));
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (std::fabs(XMVectorGetX(XMVector3Dot(d, up))) > 0.99f)
        up = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    const XMVECTOR u = XMVector3Normalize(XMVector3Cross(d, up));
    const XMVECTOR v = XMVector3Cross(d, u);
    XMStoreFloat3(&outU, u);
    XMStoreFloat3(&outV, v);
}

// 頂点 apex・軸 dir のコーンで、距離 distance における底円上の点（phase = 円周上の位相 rad）。
inline DirectX::XMFLOAT3 ConeRimPoint(const DirectX::XMFLOAT3& apex,
                                      const DirectX::XMFLOAT3& dir,
                                      f32 distance, f32 halfAngleDeg, f32 phaseRad)
{
    using namespace DirectX;
    XMFLOAT3 u{}, v{};
    BuildBasis(dir, u, v);
    const XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&dir));
    const f32 radius = distance * std::tan(XMConvertToRadians(Clampf(halfAngleDeg, 0.0f, 89.5f)));

    const XMVECTOR center = XMLoadFloat3(&apex) + d * distance;
    const XMVECTOR p = center
        + XMLoadFloat3(&u) * (radius * std::cos(phaseRad))
        + XMLoadFloat3(&v) * (radius * std::sin(phaseRad));
    XMFLOAT3 out{};
    XMStoreFloat3(&out, p);
    return out;
}

// 距離 distance の位置でのコーン底円の半径
inline f32 ConeRadiusAt(f32 distance, f32 halfAngleDeg)
{
    return distance * std::tan(DirectX::XMConvertToRadians(Clampf(halfAngleDeg, 0.0f, 89.5f)));
}

// 底円の半径 → 半頂角（度）。ConeRadiusAt の逆変換。
inline f32 ConeHalfAngleFromRadius(f32 radius, f32 distance)
{
    if (!(distance > 1e-4f)) return 1.0f;
    return DirectX::XMConvertToDegrees(std::atan2(radius < 0.0f ? 0.0f : radius, distance));
}

// レイと平面の交点パラメータ。平行、または後ろ側なら false。
inline bool RayPlane(const DirectX::XMFLOAT3& rayOrigin, const DirectX::XMFLOAT3& rayDir,
                     const DirectX::XMFLOAT3& planePoint, const DirectX::XMFLOAT3& planeNormal,
                     f32& outT)
{
    const f32 denom = rayDir.x * planeNormal.x + rayDir.y * planeNormal.y + rayDir.z * planeNormal.z;
    if (std::fabs(denom) < 1e-6f) return false;
    const f32 num = (planePoint.x - rayOrigin.x) * planeNormal.x
                  + (planePoint.y - rayOrigin.y) * planeNormal.y
                  + (planePoint.z - rayOrigin.z) * planeNormal.z;
    // 上の早期 return で denom != 0 は保証済みだが、平行なレイを定数で渡すと MSVC が
    // 定数畳み込み後に C4723 を出して /WX で落ちるので、値の変わらない下限クランプを噛ませる。
    const f32 safeDenom = (denom >= 0.0f) ? (denom < 1e-6f ? 1e-6f : denom)
                                          : (denom > -1e-6f ? -1e-6f : denom);
    outT = num / safeDenom;
    return outT > 0.0f;
}

// 直線 L(t) = lineOrigin + lineDir * t 上で、レイ R(s) = rayOrigin + rayDir * s に
// 最も近い点のパラメータ t を返す。ほぼ平行なら false（range ハンドルの発散防止）。
inline bool ClosestParamOnLineToRay(const DirectX::XMFLOAT3& lineOrigin,
                                    const DirectX::XMFLOAT3& lineDir,
                                    const DirectX::XMFLOAT3& rayOrigin,
                                    const DirectX::XMFLOAT3& rayDir,
                                    f32& outT)
{
    using namespace DirectX;
    const XMVECTOR d1 = XMVector3Normalize(XMLoadFloat3(&lineDir));
    const XMVECTOR d2 = XMVector3Normalize(XMLoadFloat3(&rayDir));
    const XMVECTOR w0 = XMLoadFloat3(&lineOrigin) - XMLoadFloat3(&rayOrigin);

    const f32 a = XMVectorGetX(XMVector3Dot(d1, d1));
    const f32 b = XMVectorGetX(XMVector3Dot(d1, d2));
    const f32 c = XMVectorGetX(XMVector3Dot(d2, d2));
    const f32 d = XMVectorGetX(XMVector3Dot(d1, w0));
    const f32 e = XMVectorGetX(XMVector3Dot(d2, w0));

    const f32 denom = a * c - b * b;
    if (std::fabs(denom) < 1e-5f) return false;   // ほぼ平行
    outT = (b * e - c * d) / denom;
    return true;
}

// 中心 center・半径 radius の球へレイを当て、当たった点の方向（単位ベクトル）を返す。
// 外れた場合は「レイ上の最近接点を球面へ射影」＝掴んだハンドルが画面外へ逃げても必ず追従する。
inline DirectX::XMFLOAT3 DirectionFromRayOnSphere(const DirectX::XMFLOAT3& center, f32 radius,
                                                  const DirectX::XMFLOAT3& rayOrigin,
                                                  const DirectX::XMFLOAT3& rayDir)
{
    using namespace DirectX;
    const XMVECTOR c  = XMLoadFloat3(&center);
    const XMVECTOR ro = XMLoadFloat3(&rayOrigin);
    const XMVECTOR rd = XMVector3Normalize(XMLoadFloat3(&rayDir));
    const XMVECTOR oc = ro - c;

    const f32 r = (radius > 1e-4f) ? radius : 1.0f;
    const f32 b = XMVectorGetX(XMVector3Dot(oc, rd));
    const f32 k = XMVectorGetX(XMVector3Dot(oc, oc)) - r * r;
    const f32 disc = b * b - k;

    f32 t;
    if (disc >= 0.0f)
    {
        const f32 s = std::sqrt(disc);
        t = -b - s;
        if (t < 0.0f) t = -b + s;
    }
    else
    {
        t = -b;   // 最近接点へフォールバック
    }

    XMVECTOR p = ro + rd * t;
    XMVECTOR v = p - c;
    if (XMVectorGetX(XMVector3LengthSq(v)) < 1e-8f)
        return { 0.0f, -1.0f, 0.0f };

    XMFLOAT3 out{};
    XMStoreFloat3(&out, XMVector3Normalize(v));
    return out;
}

} // namespace dx12e::lightmath
