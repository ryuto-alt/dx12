// VfxCommon.hlsli - UnoFX（W1-BD4）の共通部。src/renderer/vfx/VfxTypes.h と一致させること。
//   粒子 64B（GPart）、命令 48B（GOp）、乱数は VfxSpawnMath.h の Pcg / HashU / Hash01 と同じ式。
#ifndef UNO_VFX_COMMON_HLSLI
#define UNO_VFX_COMMON_HLSLI

struct GPart
{
    float3 pos; float age;
    float3 vel; float life;     // life == 0 は死んでいる
    uint   id;
    uint   c0a; uint c0b;       // 開始色 half4
    uint   c1a; uint c1b;       // 終了色 half4
    uint   size;                // half2（開始, 終了）
    uint   rot;                 // half2（角度, 角速度）
    uint   flags;
};

struct GOp
{
    uint   op;
    float  a0; float a1; float a2;
    float4 b;
    float4 c;
};

static const uint kOpShape = 1, kOpVelocity = 2, kOpLifetime = 3, kOpSize = 4, kOpColor = 5, kOpRotation = 6;
static const uint kOpGravity = 16, kOpDrag = 17, kOpCurl = 18, kOpVortex = 19, kOpAttract = 20, kOpFloor = 21;

#ifndef UNO_VFX_NO_OPS
StructuredBuffer<GOp> gOps : register(t0);
SamplerState gLinearWrap : register(s0);
#endif

static const float kPi = 3.14159265358979f;
static const float kCurlWorldSize = 8.0f;

uint Pcg(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint HashU(uint seed, uint id, uint salt) { return Pcg(seed ^ Pcg(id + salt * 0x9E3779B9u)); }
float Hash01(uint seed, uint id, uint salt) { return float(HashU(seed, id, salt) >> 8) * (1.0 / 16777216.0); }

uint  PackHalf2(float x, float y) { return f32tof16(x) | (f32tof16(y) << 16); }
float2 UnpackHalf2(uint v) { return float2(f16tof32(v & 0xFFFFu), f16tof32(v >> 16)); }
float4 UnpackColor(uint a, uint b) { return float4(UnpackHalf2(a), UnpackHalf2(b)); }

#ifndef UNO_VFX_NO_OPS
// 粒子の更新（update スタック）。pos / vel / age を進める。life を 0 にして殺すことがある（floor の kill）。
void StepParticle(inout float3 pos, inout float3 vel, inout float age, inout float life,
                  float dt, float simTime, uint opStart, uint opCount, uint curlSrv)
{
    float3 acc = float3(0, 0, 0);
    float dragK = 0.0;
    float  floorH = 0, floorBounce = 0, floorFric = 0, floorKill = 0;
    bool   hasFloor = false;
    for (uint i = 0; i < opCount; ++i)
    {
        GOp o = gOps[opStart + i];
        if (o.op == kOpGravity)       acc += float3(o.a0, o.a1, o.a2);
        else if (o.op == kOpDrag)     dragK += o.a0;
        else if (o.op == kOpCurl)
        {
            Texture3D<float4> curl = ResourceDescriptorHeap[curlSrv];
            float3 uvw = pos * (o.a1 / kCurlWorldSize) + simTime * o.a2 * float3(0.13, 0.07, 0.11);
            acc += curl.SampleLevel(gLinearWrap, uvw, 0).xyz * o.a0;
        }
        else if (o.op == kOpVortex)
        {
            float3 axis = float3(o.a0, o.a1, o.a2);
            float3 r = pos - o.b.xyz;
            float3 radial = r - axis * dot(r, axis);
            float dist = length(radial);
            float3 tang = cross(axis, radial) / max(dist, 1e-3);
            acc += tang * (o.b.w * saturate(1.0 - dist / o.c.x));
        }
        else if (o.op == kOpAttract)
        {
            float3 d = o.b.xyz - pos;
            float d2 = dot(d, d) + o.c.y * o.c.y;
            float inv = rsqrt(d2);
            acc += d * inv * (o.b.w * saturate(1.0 - sqrt(d2) / o.c.x));
        }
        else if (o.op == kOpFloor)
        {
            hasFloor = true; floorH = o.a0; floorBounce = o.a1; floorFric = o.a2; floorKill = o.b.x;
        }
    }
    vel += acc * dt;
    vel *= exp(-dragK * dt);
    pos += vel * dt;
    if (hasFloor && pos.y < floorH)
    {
        if (floorKill > 0.5) life = 0.0;
        pos.y = floorH;
        vel.y = abs(vel.y) * floorBounce;
        vel.xz *= (1.0 - floorFric);
    }
    age += dt;
}
#endif

#endif
