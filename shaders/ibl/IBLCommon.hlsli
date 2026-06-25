// IBLCommon.hlsli - IBL プリコンピュート共通関数（Hammersley / GGX importance sampling / 方向ベクトル）

#ifndef IBL_COMMON_HLSLI
#define IBL_COMMON_HLSLI

static const float PI_IBL = 3.14159265359;

// cube の (face, uv) からワールド方向ベクトルを得る。uv は [0,1]、face は 0..5（+X,-X,+Y,-Y,+Z,-Z）。
float3 DirectionFromFaceUV(uint face, float2 uv)
{
    // [0,1] → [-1,1]
    float2 st = uv * 2.0 - 1.0;
    float3 dir;
    switch (face)
    {
        case 0: dir = float3( 1.0,  -st.y, -st.x); break; // +X
        case 1: dir = float3(-1.0,  -st.y,  st.x); break; // -X
        case 2: dir = float3( st.x,  1.0,   st.y); break; // +Y
        case 3: dir = float3( st.x, -1.0,  -st.y); break; // -Y
        case 4: dir = float3( st.x, -st.y,  1.0);  break; // +Z
        default:dir = float3(-st.x, -st.y, -1.0);  break; // -Z
    }
    return normalize(dir);
}

// Van der Corput / Hammersley
float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

float2 Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), RadicalInverseVdC(i));
}

// GGX importance sampling: roughness に応じたハーフベクトルを N 周りで生成
float3 ImportanceSampleGGX(float2 xi, float3 N, float roughness)
{
    float a = roughness * roughness;

    float phi      = 2.0 * PI_IBL * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    float3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    // tangent space → world
    float3 up        = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent   = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

#endif // IBL_COMMON_HLSLI
