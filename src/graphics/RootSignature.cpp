#include "graphics/RootSignature.h"
#include "graphics/GraphicsDevice.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{

void RootSignature::Initialize(GraphicsDevice& device)
{
    // Root parameters: 14
    //
    // ★ルート定数の予算（上限 64 DWORD。超えると D3D12SerializeVersionedRootSignature が
    //   失敗して全描画が死ぬ）:
    //     slot0  32bit定数          40 DWORD
    //     slot1  CBV                 2 DWORD
    //     slot5  32bit定数           8 DWORD
    //     slot2,3,4,6,7,8,9,10,11,12,13 テーブル × 11 = 11 DWORD
    //     -------------------------------------- 合計 61 / 64
    //   残り 3 DWORD。ディスクリプタテーブルはレンジを何本持っても 1 DWORD なので、
    //   これ以降 SRV を足す機能は新規スロットを取らず、既存テーブルへレンジを足して
    //   相乗りすること（slot11 が既にその例＝クラスタ 3 本 + デカール予約 4 本）。
    D3D12_ROOT_PARAMETER1 rootParams[14]{};

    // [0] Per-Object: 32bit constants (40 DWORDs = MVP(16) + Model(16) + CustomEffect(1) + pad(3) + CustomParams(4))
    // CustomEffect は MeshRenderer::effectValue（カスタムシェーダー向けの汎用進捗値、Sprite2D::effectValue
    // と同じ役割）、CustomParams は MeshRenderer::shaderParams（汎用 float4）。pad(3) は HLSL cbuffer の
    // パッキング規則(float4 は 16 バイト境界から開始)に合わせる詰め物 = HLSL 側は effectValue の後ろへ
    // float4 shaderParams; を足すだけでオフセットが一致する。既存シェーダーはこの末尾を読まないので後方互換。
    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.Num32BitValues  = 40;
    rootParams[0].Constants.ShaderRegister  = 0;
    rootParams[0].Constants.RegisterSpace   = 0;
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [1] Per-Frame: CBV (b1)
    rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].Descriptor.RegisterSpace  = 0;
    rootParams[1].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [2] PBR Textures: DescriptorTable (t0=albedo, t1=normal, t2=metalRoughness)
    D3D12_DESCRIPTOR_RANGE1 pbrRange{};
    pbrRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    pbrRange.NumDescriptors                    = 3;  // 3連続: t0, t1, t2
    pbrRange.BaseShaderRegister                = 0;
    pbrRange.RegisterSpace                     = 0;
    pbrRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    pbrRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges   = &pbrRange;
    rootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // [3] Bones SRV DescriptorTable (t3)
    D3D12_DESCRIPTOR_RANGE1 boneRange{};
    boneRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    boneRange.NumDescriptors                    = 1;
    boneRange.BaseShaderRegister                = 3;  // t3 (was t1)
    boneRange.RegisterSpace                     = 0;
    boneRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    boneRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges   = &boneRange;
    rootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_VERTEX;

    // [4] Shadow Map SRV DescriptorTable (t4)
    D3D12_DESCRIPTOR_RANGE1 shadowRange{};
    shadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors                    = 1;
    shadowRange.BaseShaderRegister                = 4;  // t4 (was t2)
    shadowRange.RegisterSpace                     = 0;
    shadowRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[4].DescriptorTable.pDescriptorRanges   = &shadowRange;
    rootParams[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // [5] PBR Material: 8 constants (metallic, roughness, flags, pad, uvScaleOffset(float4))
    // uvScaleOffset は MeshRenderer の UV スクロール/連番アニメ用: PS が texCoord * xy + zw で
    // サンプリング UV を作る。無効時は (1,1,0,0) を入れる = 従来と同じ結果。float4 は 16 バイト
    // 境界から始まる HLSL の詰め方に合わせて pad の後ろへ置いてあるのでオフセットが一致する。
    rootParams[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[5].Constants.Num32BitValues  = 8;
    rootParams[5].Constants.ShaderRegister  = 2;  // b2
    rootParams[5].Constants.RegisterSpace   = 0;
    rootParams[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [6] IBL SRV DescriptorTable (t5=irradiance, t6=prefiltered, t7=brdfLUT) 連続3枚
    D3D12_DESCRIPTOR_RANGE1 iblRange{};
    iblRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    iblRange.NumDescriptors                    = 3;  // t5,t6,t7 連続
    iblRange.BaseShaderRegister                = 5;
    iblRange.RegisterSpace                     = 0;
    iblRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    iblRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[6].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[6].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[6].DescriptorTable.pDescriptorRanges   = &iblRange;
    rootParams[6].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // [7] SSAO AO SRV DescriptorTable (t8) — スクリーン空間 AO（ambient へ乗算）
    D3D12_DESCRIPTOR_RANGE1 aoRange{};
    aoRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    aoRange.NumDescriptors                    = 1;
    aoRange.BaseShaderRegister                = 8;   // t8
    aoRange.RegisterSpace                     = 0;
    aoRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    aoRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[7].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[7].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[7].DescriptorTable.pDescriptorRanges   = &aoRange;
    rootParams[7].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // [8] スポット/ポイント影 SRV DescriptorTable (t9=スポットTexture2DArray, t10=ポイントTextureCubeArray) 連続2枚
    D3D12_DESCRIPTOR_RANGE1 punctualShadowRange{};
    punctualShadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    punctualShadowRange.NumDescriptors                    = 2;  // t9, t10 連続
    punctualShadowRange.BaseShaderRegister                = 9;
    punctualShadowRange.RegisterSpace                     = 0;
    punctualShadowRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    punctualShadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[8].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[8].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[8].DescriptorTable.pDescriptorRanges   = &punctualShadowRange;
    rootParams[8].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // [9] コンタクトシャドウ SRV DescriptorTable (t11) — スクリーン空間の近接遮蔽（太陽の寄与へ乗算）。
    // SSAO(t8)と同じく PS が無条件で Load するため、無効時も白ダミー(1x1)を必ずバインドすること。
    D3D12_DESCRIPTOR_RANGE1 contactShadowRange{};
    contactShadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    contactShadowRange.NumDescriptors                    = 1;
    contactShadowRange.BaseShaderRegister                = 11;  // t11
    contactShadowRange.RegisterSpace                     = 0;
    contactShadowRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    contactShadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[9].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[9].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[9].DescriptorTable.pDescriptorRanges   = &contactShadowRange;
    rootParams[9].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // [10] 前フレームのボーン行列 SRV DescriptorTable (t12) — 速度バッファ生成パス専用。
    // SkinningBuffer は frameCount 枚を多重化しているので「前フレームの frameIndex のスロット」を
    // そのままここへ張れば前フレームのボーン行列になる（二重バッファ不要）。
    // 速度パス以外の PSO はこのレジスタを読まないのでバインドしなくてよい。
    D3D12_DESCRIPTOR_RANGE1 prevBoneRange{};
    prevBoneRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    prevBoneRange.NumDescriptors                    = 1;
    prevBoneRange.BaseShaderRegister                = 12;  // t12
    prevBoneRange.RegisterSpace                     = 0;
    prevBoneRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    prevBoneRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[10].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[10].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[10].DescriptorTable.pDescriptorRanges   = &prevBoneRange;
    rootParams[10].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_VERTEX;

    // [11] クラスタードライティング SRV DescriptorTable。ヒープ上は 7 本連続だが、
    // シェーダレジスタは t13,t14,t15（クラスタ）と t18..t21（デカール予約）に割れている。
    //   t13 = StructuredBuffer<ClusterLight>（フレーム毎に切り替わる）
    //   t14 = クラスタ別ライトインデックスリスト（固定ストライド 128/クラスタ）
    //   t15 = クラスタ毎のライト数
    //   t18..t21 = デカール（計画06）が同じテーブルへ相乗りするための予約枠
    // t16/t17 は SSR/SSGI（計画04）が別スロットで取るのでレンジを 2 本に割ってある。
    // ★ディスクリプタテーブルはレンジを何本持っても 1 DWORD。
    // ★OFFSET_APPEND なので 2 本目はヒープ上でオフセット 3 から始まる（レジスタの穴は
    //   ヒープを消費しない）。DATA_VOLATILE でも「ディスクリプタ自体」は静的扱いなので、
    //   予約枠 4 本も有効なディスクリプタで埋めてから SetGraphicsRootDescriptorTable すること
    //   （ClusteredLightCulling::Initialize がカウントバッファの SRV で埋めている）。
    D3D12_DESCRIPTOR_RANGE1 clusterRanges[3]{};
    clusterRanges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    clusterRanges[0].NumDescriptors                    = 3;   // t13,t14,t15
    clusterRanges[0].BaseShaderRegister                = 13;
    clusterRanges[0].RegisterSpace                     = 0;
    clusterRanges[0].Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    clusterRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    clusterRanges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    clusterRanges[1].NumDescriptors                    = 4;   // t18,t19,t20,t21（デカール予約）
    clusterRanges[1].BaseShaderRegister                = 18;
    clusterRanges[1].RegisterSpace                     = 0;
    clusterRanges[1].Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    clusterRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // t22 = DDGI の irradiance アトラス（段階1） / t23 = 距離モーメント（段階2）。
    // ★ここも新規スロットを取らず slot11 へ相乗りする＝ルート定数の予算は 61/64 のまま。
    //   OFF のときも 1x1 黒ダミーで必ず埋めること（未初期化はデバッグレイヤが落とす）。
    clusterRanges[2].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    clusterRanges[2].NumDescriptors                    = 2;   // t22, t23
    clusterRanges[2].BaseShaderRegister                = 22;
    clusterRanges[2].RegisterSpace                     = 0;
    clusterRanges[2].Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    clusterRanges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[11].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[11].DescriptorTable.NumDescriptorRanges = _countof(clusterRanges);
    rootParams[11].DescriptorTable.pDescriptorRanges   = clusterRanges;
    rootParams[11].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // [12] SSR 結果 SRV DescriptorTable (t16) / [13] SSGI 結果 SRV DescriptorTable (t17)
    // どちらも SSAO(t8) / コンタクトシャドウ(t11) と同じ「フル解像度のスクリーン空間補助
    // テクスチャを PS が Load(SV_Position.xy) で直読みする」流儀。
    // ★無効時も 1x1 黒ダミー(RGBA16F)を必ずバインドすること。1x1 なので Load(画面座標) が
    //   範囲外で 0 を返し、SSR の confidence=0 / SSGI の寄与=0 に自動的に落ちる。
    D3D12_DESCRIPTOR_RANGE1 ssrRange{};
    ssrRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ssrRange.NumDescriptors                    = 1;
    ssrRange.BaseShaderRegister                = 16;  // t16
    ssrRange.RegisterSpace                     = 0;
    ssrRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    ssrRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[12].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[12].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[12].DescriptorTable.pDescriptorRanges   = &ssrRange;
    rootParams[12].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE1 ssgiRange = ssrRange;
    ssgiRange.BaseShaderRegister                = 17;  // t17

    rootParams[13].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[13].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[13].DescriptorTable.pDescriptorRanges   = &ssgiRange;
    rootParams[13].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static Samplers (s0=albedo wrap, s1=shadow PCF, s2=IBL linear-clamp mip有,
    //                  s3=BRDF linear-clamp mipなし, s4=AO point-clamp スクリーンサンプル,
    //                  s5=DDGI irradiance linear-clamp mipなし)
    D3D12_STATIC_SAMPLER_DESC staticSamplers[6]{};

    // s0 - Anisotropic Wrap (albedo, normal, metalRoughness)
    // ★等方トリリニアだと LOD が「フットプリントの長軸」で決まるため、床や壁を
    //   グレージング角で見たときに視線方向の伸びに合わせて全体がボケる。
    //   法線マップは mip が上がると (0.5,0.5)=平坦法線へ収束するので、
    //   「ある距離から先だけ模様が消えて灰色に見え、カメラを動かすと境界も動く」
    //   という不具合になっていた（環境光は法線非依存なので点光源のときだけ出る）。
    // ★MaxAnisotropy は構造体の {} 初期化で 0 のままなので明示的に設定すること。
    //   Filter だけ ANISOTROPIC にして MaxAnisotropy=0 だと不正な組み合わせになる。
    staticSamplers[0].Filter           = D3D12_FILTER_ANISOTROPIC;
    staticSamplers[0].MaxAnisotropy    = 8;
    staticSamplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    staticSamplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    staticSamplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
    staticSamplers[0].ShaderRegister   = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s1 - Comparison sampler for shadow PCF
    staticSamplers[1].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    staticSamplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    staticSamplers[1].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    staticSamplers[1].MaxLOD           = D3D12_FLOAT32_MAX;
    staticSamplers[1].ShaderRegister   = 1;
    staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s2 - IBL: LINEAR CLAMP (mip有, MaxLOD=FLOAT32_MAX) — irradiance/prefiltered 用
    staticSamplers[2].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamplers[2].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[2].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[2].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[2].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    staticSamplers[2].BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    staticSamplers[2].MaxLOD           = D3D12_FLOAT32_MAX;
    staticSamplers[2].ShaderRegister   = 2;
    staticSamplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s3 - BRDF LUT: LINEAR CLAMP (mipなし, MaxLOD=0)
    staticSamplers[3]                  = staticSamplers[2];
    staticSamplers[3].MaxLOD           = 0.0f;
    staticSamplers[3].ShaderRegister   = 3;

    // s4 - SSAO AO: POINT CLAMP（スクリーン空間 AO の素直なサンプル）
    staticSamplers[4].Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    staticSamplers[4].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[4].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[4].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[4].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    staticSamplers[4].BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    staticSamplers[4].MaxLOD           = D3D12_FLOAT32_MAX;
    staticSamplers[4].ShaderRegister   = 4;  // s4
    staticSamplers[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s5 - DDGI irradiance アトラス: LINEAR CLAMP (mipなし)
    // ★s2(IBL) を使い回せない: Forward.hlsl が s2 を g_iblSampler として宣言済みで、
    //   同じ register に 2 つ目の SamplerState を宣言すると DXC が弾く。
    //   静的サンプラはルート定数の DWORD を 1 つも消費しないので足しても無料。
    staticSamplers[5]                  = staticSamplers[2];
    staticSamplers[5].MaxLOD           = 0.0f;
    staticSamplers[5].ShaderRegister   = 5;  // s5

    // Root Signature (Version 1.1)
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc{};
    versionedDesc.Version                     = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versionedDesc.Desc_1_1.NumParameters      = _countof(rootParams);
    versionedDesc.Desc_1_1.pParameters        = rootParams;
    versionedDesc.Desc_1_1.NumStaticSamplers  = _countof(staticSamplers);
    versionedDesc.Desc_1_1.pStaticSamplers    = staticSamplers;
    versionedDesc.Desc_1_1.Flags              = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    // ★直列化結果はメンバへ残す。カスタムシェーダーの PSO 生成が失敗したとき、
    //   ここから「実際に使えるレジスタ」を読み出してユーザーへ提示する。
    HRESULT hr = D3D12SerializeVersionedRootSignature(&versionedDesc, &m_serialized, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            Logger::Error("ルートシグネチャのシリアライズに失敗しました: {}",
                static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        ThrowIfFailed(hr);
    }

    ThrowIfFailed(device.GetDevice()->CreateRootSignature(
        0,
        m_serialized->GetBufferPointer(),
        m_serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)));

    Logger::Info("RootSignature created (PBR: 14 slots, 61/64 DWORD)");
}

} // namespace dx12e
