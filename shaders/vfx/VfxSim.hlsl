// VfxSim.hlsl - UnoFX のシミュレーション（W1-BD4）。
//   CSEmit  : 放出（1 ステップ・1 インスタンスにつき 1 dispatch。粒子 id = firstId + k、slot = base + id % cap）
//   CSSim   : 既存の粒子を 1 ステップ進める（生存数を wave 単位にまとめた 1 回のアトミックで統計へ）
//   CSClear : インスタンスの範囲を全部「死んでいる」にする（seek の巻き戻し・確保の直後）
//   CSStatClear : 統計の 1 スロットを 0 にする
// 生成にアトミックも dead リストも使わない（同じ入力なら同じバイト列になる）。
// ルートシグネチャ: b0 = SimCB（36 DWORD）, t0 = gOps（root SRV）, u0 = gPool（root UAV）, u1 = gStats（root UAV）, s0 = 線形ラップ
#include "VfxCommon.hlsli"

cbuffer SimCB : register(b0)
{
    float4 wRow0;     // 放出体のローカル→ワールド（現在）。行 i = (m_i0, m_i1, m_i2, 平行移動 i)
    float4 wRow1;
    float4 wRow2;
    float4 prevPos;   // xyz = 1 つ前のステップの放出体の位置
    uint4  uA;        // x = base, y = cap, z = firstId, w = count（CSEmit）
    uint4  uB;        // x = initStart, y = initCount, z = updStart, w = updCount
    float4 fA;        // x = ステップ開始時の sim 時刻, y = ステップの長さ h
    uint4  uC;        // x = seed, y = statIdx, z = simCount（CSSim のスレッド数）, w = flags（bit0: 統計に数える）
    uint4  uD;        // x = カール場の SRV 番号
};

RWStructuredBuffer<GPart> gPool  : register(u0);
RWStructuredBuffer<uint>  gStats : register(u1);

float3 LocalDir(float3 d) { return float3(dot(wRow0.xyz, d), dot(wRow1.xyz, d), dot(wRow2.xyz, d)); }

// 単位球の一様な方向
float3 RandDir(uint seed, uint id, uint salt)
{
    float z = Hash01(seed, id, salt) * 2.0 - 1.0;
    float a = Hash01(seed, id, salt + 1) * 2.0 * kPi;
    float r = sqrt(max(0.0, 1.0 - z * z));
    return float3(r * cos(a), z, r * sin(a));
}

// dir を軸に、半角 halfAngleDeg の円錐の中で一様な方向
float3 SpreadDir(float3 dir, float halfAngleDeg, uint seed, uint id, uint salt)
{
    if (halfAngleDeg <= 0.0) return dir;
    float cosMax = cos(radians(min(halfAngleDeg, 180.0)));
    float cosT = lerp(1.0, cosMax, Hash01(seed, id, salt));
    float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi = Hash01(seed, id, salt + 1) * 2.0 * kPi;
    float3 up = abs(dir.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(up, dir));
    float3 b = cross(dir, t);
    return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + dir * cosT);
}

[numthreads(64, 1, 1)]
void CSEmit(uint3 dtid : SV_DispatchThreadID)
{
    const uint k = dtid.x;
    const uint count = uA.w;
    bool alive = false;
    bool overwritten = false;   // リングの周回で、まだ生きている古い粒子を上書きした（統計の二重計上を引く）
    if (k < count)
    {
        const uint seed = uC.x;
        const uint id = uA.z + k;
        const uint slot = uA.x + (id % uA.y);
        const float h = fA.y;
        const float u = (float(k) + 0.5) / float(count);

        float3 lpos = float3(0, 0, 0);
        float3 sdir = float3(0, 1, 0);
        float3 vdirL = float3(0, 1, 0);
        float  speed = 0.0, spreadDeg = 0.0, inherit = 0.0;
        uint   velMode = 0;
        float  life = 1.0;
        float2 size0 = float2(0.1, 0.1), size1 = float2(0.1, 0.1);
        float4 col0 = float4(1, 1, 1, 1), col1 = float4(1, 1, 1, 0);
        float  rot = 0.0, rotRate = 0.0;

        for (uint i = 0; i < uB.y; ++i)
        {
            GOp o = gOps[uB.x + i];
            const uint s = i * 16u;   // 命令ごとに別の乱数列
            if (o.op == kOpShape)
            {
                const uint kind = uint(o.a0 + 0.5);
                const bool surface = o.a1 > 0.5;
                if (kind == 0) { sdir = RandDir(seed, id, s + 100); }
                else if (kind == 1)
                {
                    float3 d = RandDir(seed, id, s + 100);
                    float r = surface ? o.b.x : lerp(o.c.x, o.b.x, pow(Hash01(seed, id, s + 102), 1.0 / 3.0));
                    lpos = d * r; sdir = d;
                }
                else if (kind == 2)
                {
                    float3 r3 = float3(Hash01(seed, id, s + 100), Hash01(seed, id, s + 101), Hash01(seed, id, s + 102)) * 2.0 - 1.0;
                    lpos = r3 * o.b.xyz;
                }
                else if (kind == 3)
                {
                    float a = Hash01(seed, id, s + 100) * 2.0 * kPi;
                    float rr = surface ? 1.0 : sqrt(Hash01(seed, id, s + 102));
                    float2 c = float2(cos(a), sin(a));
                    float ang = radians(o.a2) * rr;
                    sdir = normalize(float3(c.x * sin(ang), cos(ang), c.y * sin(ang)));
                    lpos = float3(c.x, 0, c.y) * (o.b.x * rr) + sdir * (o.b.w * Hash01(seed, id, s + 103));
                }
                else
                {
                    float a = Hash01(seed, id, s + 100) * 2.0 * kPi;
                    float r = surface ? o.b.x : lerp(o.c.x, o.b.x, sqrt(Hash01(seed, id, s + 102)));
                    float2 c = float2(cos(a), sin(a));
                    lpos = float3(c.x, 0, c.y) * r; sdir = float3(c.x, 0, c.y);
                }
            }
            else if (o.op == kOpVelocity)
            {
                velMode = uint(o.a0 + 0.5);
                spreadDeg = o.a1; inherit = o.a2;
                float3 d = o.b.xyz;
                vdirL = dot(d, d) > 1e-12 ? normalize(d) : float3(0, 1, 0);
                speed = lerp(o.c.x, o.c.y, Hash01(seed, id, s + 110));
            }
            else if (o.op == kOpLifetime) { life = lerp(o.c.x, o.c.y, Hash01(seed, id, s + 120)); }
            else if (o.op == kOpSize)
            {
                size0 = float2(lerp(o.b.x, o.b.y, Hash01(seed, id, s + 130)), 0);
                float e = lerp(o.b.z, o.b.w, Hash01(seed, id, s + 131));
                size1 = float2(e, 0);
            }
            else if (o.op == kOpColor)
            {
                float v = 1.0 + o.a0 * (Hash01(seed, id, s + 140) * 2.0 - 1.0);
                col0 = float4(o.b.rgb * v, o.b.a);
                col1 = float4(o.c.rgb * v, o.c.a);
            }
            else if (o.op == kOpRotation)
            {
                rot = radians(lerp(o.b.x, o.b.y, Hash01(seed, id, s + 150)));
                rotRate = radians(lerp(o.b.z, o.b.w, Hash01(seed, id, s + 151)));
            }
        }

        // 方向（velMode 0 = direction, 1 = radial, 2 = shape）と広がり
        float3 dirL = vdirL;
        if (velMode == 1) dirL = dot(lpos, lpos) > 1e-10 ? normalize(lpos) : sdir;
        else if (velMode == 2) dirL = sdir;
        dirL = SpreadDir(dirL, spreadDeg, seed, id, 200);

        // サブフレーム補間: 放出体の位置は prev→cur を u で補間
        const float3 curT = float3(wRow0.w, wRow1.w, wRow2.w);
        const float3 t = lerp(prevPos.xyz, curT, u);
        float3 pos = float3(dot(wRow0.xyz, lpos), dot(wRow1.xyz, lpos), dot(wRow2.xyz, lpos)) + t;
        float3 dirW = LocalDir(dirL);
        dirW = dot(dirW, dirW) > 1e-12 ? normalize(dirW) : float3(0, 1, 0);
        float3 emitterVel = (curT - prevPos.xyz) / max(h, 1e-6);
        float3 vel = dirW * speed + emitterVel * inherit;

        // 放出された時刻からステップの終わりまでを、同じ update スタックで先に進める
        float age = 0.0;
        const float dtp = h * (1.0 - u);
        const float simT = fA.x + u * h;
        StepParticle(pos, vel, age, life, dtp, simT, uB.z, uB.w, uD.x);

        GPart p;
        p.pos = pos; p.age = age;
        p.vel = vel; p.life = (age < life) ? life : 0.0;
        p.id = id;
        p.c0a = PackHalf2(col0.r, col0.g); p.c0b = PackHalf2(col0.b, col0.a);
        p.c1a = PackHalf2(col1.r, col1.g); p.c1b = PackHalf2(col1.b, col1.a);
        p.size = PackHalf2(size0.x, size1.x);
        p.rot = PackHalf2(rot + rotRate * dtp, rotRate);
        p.flags = 0;
        overwritten = gPool[slot].life > 0.0;   // 直前の CSSim が数えた生存。id が slot ごとに 1 つなので競合しない
        gPool[slot] = p;
        alive = p.life > 0.0;
    }
    if ((uC.w & 1u) != 0u)
    {
        const uint n = WaveActiveCountBits(alive);
        const uint over = WaveActiveCountBits(overwritten);
        if (WaveIsFirstLane() && n > 0) InterlockedAdd(gStats[uC.y], n);
        if (WaveIsFirstLane() && over > 0) InterlockedAdd(gStats[uC.y], 0u - over);   // u32 の巻き戻しで減算
    }
}

[numthreads(64, 1, 1)]
void CSSim(uint3 dtid : SV_DispatchThreadID)
{
    const uint i = dtid.x;
    bool alive = false;
    if (i < uC.z)
    {
        const uint slot = uA.x + i;
        GPart p = gPool[slot];
        if (p.life > 0.0)
        {
            const float h = fA.y;
            float life = p.life;
            StepParticle(p.pos, p.vel, p.age, life, h, fA.x, uB.z, uB.w, uD.x);
            float2 r = UnpackHalf2(p.rot);
            p.rot = PackHalf2(r.x + r.y * h, r.y);
            if (p.age >= life) life = 0.0;
            p.life = life;
            gPool[slot] = p;
            alive = life > 0.0;
        }
    }
    if ((uC.w & 1u) != 0u)
    {
        const uint n = WaveActiveCountBits(alive);
        if (WaveIsFirstLane() && n > 0) InterlockedAdd(gStats[uC.y], n);
    }
}

[numthreads(64, 1, 1)]
void CSClear(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= uA.y) return;
    GPart z = (GPart)0;
    gPool[uA.x + dtid.x] = z;
}

[numthreads(1, 1, 1)]
void CSStatClear()
{
    gStats[uC.y] = 0;
}
