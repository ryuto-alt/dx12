// GpuParticleSim.hlsl - GPUパーティクルの compute シミュレーション一式。
// dead/alive リスト方式（Wicked Engine 系の骨格）:
//   CSInit     … dead リストを 0..N-1 で満たしカウンタを初期化（初回のみ）
//   CSPrepare  … 前フレームの alive(次) カウントを alive(現) へ移し、次カウントを 0 に
//   CSEmit     … dead から pop して初期化、alive(現) へ push（放出リクエスト毎に Dispatch）
//   CSKickoff  … alive(現) カウントから simulate の間接 Dispatch 引数と draw 引数(instance=0)を書く
//   CSSimulate … alive(現) を走査。死亡→dead へ返却 / 生存→シム後 alive(次) へ push + 描画数を加算
// 加算合成専用（ソート不要）。ネスト/衝突は将来拡張。

// 粒子 96B（GpuParticleDraw.hlsl と一致必須）
struct GPart
{
    float3 pos;   float life;
    float3 vel;   float age;
    float3 col0;  float size0;
    float3 col1;  float size1;
    float gravity; float drag; float turb; float seed;
    uint  kind;   float stretch; float2 _pad;
};

cbuffer GPCB : register(b0)
{
    float4 emitPos;   // xyz=位置, w=放出数
    float4 emitDir;   // xyz=方向, w=spread(0=dir集中,1=全球)
    float4 emitCol0;  // rgb=開始色(強度込み), w=speed
    float4 emitCol1;  // rgb=終了色(強度込み), w=speedVar
    float4 emitP0;    // size0, size1, life, lifeVar
    float4 emitP1;    // gravity, drag, up, turb
    float4 emitP2;    // kind, stretch, seed, 未使用
    float4 simP;      // dt, time, maxParticles, floorY(床バウンド高さ。-1e9で無効)
    float4 simP3;     // 予備
};

RWStructuredBuffer<GPart> gParticles : register(u0);
RWStructuredBuffer<uint>  gAliveCur  : register(u1);
RWStructuredBuffer<uint>  gAliveNext : register(u2);
RWStructuredBuffer<uint>  gDead      : register(u3);
RWByteAddressBuffer       gCounters  : register(u4);  // 0=dead数, 4=alive現数, 8=alive次数
RWByteAddressBuffer       gDispatchArgs : register(u5);  // 0..11 = simulate の Dispatch(x,y,z)
RWByteAddressBuffer       gDrawArgs  : register(u6);     // 0..15 = Draw(6, instance, 0, 0)

// ---- ハッシュ / ランダム ----
uint pcg(uint v)
{
    v = v * 747796405u + 2891336453u;
    uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}
float rand01(inout uint state)
{
    state = pcg(state);
    return (float)(state & 0x00FFFFFFu) / 16777216.0;
}

[numthreads(256, 1, 1)]
void CSInit(uint3 id : SV_DispatchThreadID)
{
    uint maxP = (uint)simP.z;
    if (id.x < maxP)
        gDead[id.x] = maxP - 1u - id.x;   // 末尾から pop → 0,1,2,... の順で配る
    if (id.x == 0u)
    {
        gCounters.Store(0, maxP);  // dead
        gCounters.Store(4, 0u);    // alive現
        gCounters.Store(8, 0u);    // alive次
        gDrawArgs.Store4(0, uint4(6u, 0u, 0u, 0u));
        gDispatchArgs.Store3(0, uint3(0u, 1u, 1u));
    }
}

[numthreads(1, 1, 1)]
void CSPrepare(uint3 id : SV_DispatchThreadID)
{
    uint next = gCounters.Load(8);
    gCounters.Store(4, next);   // 前フレームの生存が今フレームの入力
    gCounters.Store(8, 0u);
}

[numthreads(256, 1, 1)]
void CSEmit(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)emitPos.w) return;

    // dead から 1 スロット確保（枯渇したら戻して終了）
    int prevDead;
    gCounters.InterlockedAdd(0, -1, prevDead);
    if (prevDead <= 0)
    {
        int dummy;
        gCounters.InterlockedAdd(0, 1, dummy);
        return;
    }
    uint slot = gDead[prevDead - 1];

    uint h = pcg(id.x * 7919u + asuint(emitP2.z));

    // 単位球サンプル → dir へ寄せる（CPU 版 Emit と同じ流儀）
    float z = rand01(h) * 2.0 - 1.0;
    float a = rand01(h) * 6.2831853;
    float r = sqrt(max(0.0, 1.0 - z * z));
    float3 sph = float3(r * cos(a), z, r * sin(a));
    float3 dirN = emitDir.xyz;
    float dl = length(dirN);
    dirN = (dl > 1e-4) ? dirN / dl : float3(0, 1, 0);
    float spread = emitDir.w;
    float3 vdir = sph * spread + dirN * (1.0 - spread);

    float spd = emitCol0.w * (1.0 - rand01(h) * emitCol1.w);

    GPart p;
    p.pos     = emitPos.xyz;
    p.life    = max(0.05, emitP0.z * (1.0 - rand01(h) * emitP0.w));
    p.vel     = vdir * spd + float3(0.0, emitP1.z * emitCol0.w * 0.5, 0.0);
    p.age     = 0.0;
    p.col0    = emitCol0.rgb;
    p.size0   = emitP0.x * (0.7 + 0.3 * rand01(h));
    p.col1    = emitCol1.rgb;
    p.size1   = emitP0.y;
    p.gravity = emitP1.x;
    p.drag    = emitP1.y;
    p.turb    = emitP1.w;
    p.seed    = rand01(h) * 1000.0;
    p.kind    = (uint)emitP2.x;
    p.stretch = emitP2.y;
    p._pad    = float2(0, 0);
    gParticles[slot] = p;

    uint ai;
    gCounters.InterlockedAdd(4, 1, ai);
    gAliveCur[ai] = slot;
}

[numthreads(1, 1, 1)]
void CSKickoff(uint3 id : SV_DispatchThreadID)
{
    uint cur = gCounters.Load(4);
    gDispatchArgs.Store3(0, uint3((cur + 255u) / 256u, 1u, 1u));
    gDrawArgs.Store4(0, uint4(6u, 0u, 0u, 0u));
}

// 安価な擬似カール乱流（sin 場の組み合わせ。divergence は厳密でないが見た目は有機的）
float3 cheapCurl(float3 p, float t)
{
    float3 q = p * 1.7 + float3(0.0, t * 0.35, 0.0);
    return float3(
        sin(q.y * 2.1 + q.z * 1.3) - sin(q.z * 1.7),
        sin(q.z * 2.3 + q.x * 1.1) - sin(q.x * 1.9),
        sin(q.x * 2.7 + q.y * 1.7) - sin(q.y * 1.3));
}

[numthreads(256, 1, 1)]
void CSSimulate(uint3 id : SV_DispatchThreadID)
{
    uint count = gCounters.Load(4);
    if (id.x >= count) return;

    uint slot = gAliveCur[id.x];
    GPart p = gParticles[slot];

    float dt = simP.x;
    p.age += dt;
    if (p.age >= p.life)
    {
        uint d;
        gCounters.InterlockedAdd(0, 1, d);
        gDead[d] = slot;
        return;
    }

    if (p.turb > 0.0)
        p.vel += cheapCurl(p.pos, simP.y) * (p.turb * 2.0) * dt;

    p.vel.y += p.gravity * dt;
    float damp = max(0.0, 1.0 - p.drag * dt);
    p.vel *= damp;
    p.pos += p.vel * dt;

    // 床バウンド（CPU 版と同じ簡易挙動）
    if (p.pos.y < simP.w && p.vel.y < 0.0)
    {
        p.pos.y = simP.w;
        p.vel.y = -p.vel.y * 0.35;
        p.vel.xz *= 0.7;
    }

    gParticles[slot] = p;

    uint n;
    gCounters.InterlockedAdd(8, 1, n);
    gAliveNext[n] = slot;
    gDrawArgs.InterlockedAdd(4, 1, n);   // InstanceCount++
}
