#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <random>
#include <string>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;

// 見た目の種別（Particle.hlsl の KIND_* と一致必須）。
enum class ParticleKind : int
{
    Glow = 0,   // 既定: 発光ソフト円（後方互換）
    Fire = 1,   // fbm 炎（温度ランプ）
    Smoke = 2,  // fbm 煙（α前乗算で遮蔽）
    Spark = 3,  // 明るいエネルギーコア（ストレッチで筋）
    Magic = 4,  // 極座標うず巻きリング
    Electric = 5, // ジッタ稲妻フィラメント（ストロボ）
    Ring = 6,   // 単一粒子で拡大する衝撃波リング
    Star = 7,   // アナモルフィック十字＋花弁フレア
};

enum class ParticleBlend : int { Additive = 0, AlphaPremul = 1 };

// ビームの見た目（Beam.hlsl の BEAM_* と一致）。
enum class BeamKind : int { Energy = 0, Electric = 1, Fire = 2 };

// CPUシミュレーション + GPUインスタンシングのプロシージャル質感パーティクル。
// Lua の fx:burst / fx:ring から放出され、scene の HDR RT へ描かれる。
// 質感はシェーダの数式（テクスチャ無し）。動きはカールノイズ（divergence-free）。
class ParticleSystem
{
public:
    // 放出パラメータ（Lua テーブルから組み立てる）。
    struct EmitParams
    {
        DirectX::XMFLOAT3 pos{0, 0, 0};
        int   count      = 16;
        DirectX::XMFLOAT3 dir{0, 1, 0}; // 基準方向（burst の偏り / ring は無視）
        float spread     = 1.0f;        // 0=dir方向に集中, 1=全球
        float speed      = 6.0f;
        float speedVar   = 0.5f;        // 0..1（速度のばらつき率）
        float size       = 0.4f;
        float sizeEnd    = 0.0f;
        float life       = 0.6f;
        float lifeVar    = 0.3f;
        DirectX::XMFLOAT3 color{1, 1, 1};
        DirectX::XMFLOAT3 colorMid{1, 1, 1};
        DirectX::XMFLOAT3 colorEnd{1, 1, 1};
        bool  hasColorMid = false;      // 3キー色カーブ（start→mid→end）
        bool  hasColorEnd = false;
        float intensity  = 3.0f;        // HDR増幅（>1 で白熱→ブルーム）
        float gravity    = 0.0f;        // y方向加速（負で落下）
        float drag       = 1.0f;        // 速度減衰/秒
        float up         = 0.0f;        // 初速の上向きバイアス
        bool  ring       = false;       // true=XZ平面に等間隔リング（衝撃波の配置）
        // --- 高品質化（コードのみ）---
        float stretch     = 0.0f;       // >0 で速度方向へ伸びる（火花/筋/弾道）
        float turbStrength = 0.0f;      // >0 でカールノイズ乱流（有機的な揺らぎ：煙/炎）
        float turbFreq     = 1.0f;      // 乱流の空間周波数
        int   kind         = 0;         // ParticleKind
        int   blend        = 0;         // ParticleBlend（既定は加算、煙はα）
        float flicker      = 0.0f;      // 発光明滅の強さ（0..1）
        float flickerFreq  = 18.0f;     // 明滅の速さ
        float sizeMid      = -1.0f;     // >=0 で3キーサイズカーブ（start→mid→end）
        float distort      = 0.0f;      // >0 で歪みパーティクル（シーンでなく歪みバッファへ描く。熱ゆらぎ/衝撃波）
        bool  light        = false;     // true で「明るい粒子上位N個のポイントライト化」候補
        float lightRange   = 3.0f;      // ポイントライト化時の到達距離
    };

    // 連続ビーム（2点間カメラ向きquad）。レーザー/エネルギー線/火柱/稲妻。
    struct BeamParams
    {
        DirectX::XMFLOAT3 p0{0, 0, 0};
        DirectX::XMFLOAT3 p1{0, 1, 0};
        float width      = 0.3f;
        DirectX::XMFLOAT3 color{1, 1, 1};
        float intensity  = 6.0f;
        float life       = 0.06f;   // 既定は毎フレーム再放出向け
        int   kind       = 0;       // BeamKind
    };

    // トレイル（軌跡リボン）のパラメータ。id 単位に位置履歴を保持して帯を描く。
    struct TrailParams
    {
        float width     = 0.25f;
        DirectX::XMFLOAT3 color{0.4f, 0.8f, 1.0f};
        DirectX::XMFLOAT3 colorEnd{0.1f, 0.2f, 1.0f};
        float intensity = 2.0f;
        float life      = 0.5f;    // 各点の寿命（秒）＝帯の長さ
        int   blend     = 0;       // 0=加算（エネルギー）, 1=前乗算アルファ（煙）
        float minDist   = 0.03f;   // 前回の点からこれ以上離れたら新しい点を打つ
    };

    // パーティクルライト（明るい粒子上位N個をポイントライト化する）情報。
    struct LightInfo
    {
        DirectX::XMFLOAT3 pos;
        float             range;
        DirectX::XMFLOAT3 color;   // intensity 乗算済み
    };

    void Initialize(GraphicsDevice& device, DXGI_FORMAT rtvFormat,
                    DXGI_FORMAT dsvFormat, DXGI_FORMAT distortFormat,
                    const std::wstring& shaderDir);

    // onDeath: 粒子の死亡位置で子バーストを放出（サブエミッタ・1段のみ）。
    void Emit(const EmitParams& p, const EmitParams* onDeath = nullptr);
    void EmitBeam(const BeamParams& b);
    // 毎フレーム呼んで軌跡点を記録する（id はエンティティID等の一意な値）。
    // 呼ばれなくなったトレイルは寿命経過後に自動削除。
    void TrailPoint(u64 id, const DirectX::XMFLOAT3& pos, const TrailParams& tp);
    void Update(f32 dt);
    void Clear();

    // light=true の生存粒子から明るい順に最大 maxCount 個を返す（forward のポイントライト空き枠用）。
    u32 CollectLights(u32 maxCount, LightInfo* out) const;

    // シーン深度 SRV（R32_FLOAT）と線形化パラメータを供給（soft particles 用）。
    // projA=_33, projB=_43, invRTW=1/RTwidth, invRTH=1/RTheight。invRTW<=0 で soft 無効。
    void SetSceneDepth(D3D12_GPU_DESCRIPTOR_HANDLE srv, float projA, float projB,
                       float invRTW, float invRTH)
    { m_depthSrv = srv; m_projA = projA; m_projB = projB; m_invRTW = invRTW; m_invRTH = invRTH; m_hasDepth = true; }
    void DisableSceneDepth() { m_hasDepth = false; }
    void SetTime(float t) { m_time = t; }

    // scene パス内（HDR RT バインド済み・深度は SRV としてバインド済み）で呼ぶ。
    void Render(ID3D12GraphicsCommandList* cmd, DirectX::XMMATRIX viewProj,
                DirectX::XMFLOAT3 camRight, DirectX::XMFLOAT3 camUp,
                DirectX::XMFLOAT3 camPos);

    // 歪みパーティクル（distort>0）を歪みバッファ(RG16F)へ描く。
    // Render() の後・同一フレーム内に、歪みRTをバインドしてから呼ぶこと。
    void RenderDistortion(ID3D12GraphicsCommandList* cmd, DirectX::XMMATRIX viewProj,
                          DirectX::XMFLOAT3 camRight, DirectX::XMFLOAT3 camUp);
    bool HasDistortion() const { return m_distortCount > 0; }

    // 画面インパクト（ヒット時にクロマ/放射ブラーを瞬間的に上げる用）。
    void  AddPulse(float amount) { if (amount > m_pulse) m_pulse = amount; }
    float GetPulse() const { return m_pulse; }

    int AliveCount() const { return m_aliveCount; }

private:
    struct Particle
    {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 vel;
        DirectX::XMFLOAT3 col0;
        DirectX::XMFLOAT3 colM;
        DirectX::XMFLOAT3 col1;
        float size0, size1;
        float sizeM = -1.0f;            // >=0 で3キーサイズ
        float age, life;
        float rot, rotVel;
        float gravity, drag, alpha;
        float stretch = 0.0f;
        float turbStrength = 0.0f, turbFreq = 1.0f;
        float seed = 0.0f;
        float flicker = 0.0f, flickerFreq = 18.0f;
        float distort = 0.0f;           // >0 で歪みバッファ行き
        float lightRange = 3.0f;
        int   childIdx = -1;            // 死亡時に放出する子定義（m_childDefs の添字）
        int   kind = 0;
        int   blend = 0;
        bool  light = false;
        bool  hasMid = false;
        bool  alive = false;
    };

    // シェーダのインスタンス入力レイアウトと一致（stride 64）。
    struct GpuParticle
    {
        DirectX::XMFLOAT3 center;   // 0  POSITION
        float             size;     // 12 TEXCOORD0
        DirectX::XMFLOAT4 color;    // 16 COLOR0
        float             rot;      // 32 TEXCOORD1
        float             stretch;  // 36 TEXCOORD2
        DirectX::XMFLOAT3 vel;      // 40 NORMAL0（速度ストレッチ用）
        float             age01;    // 52 TEXCOORD3
        u32               kind;     // 56 TEXCOORD4
        float             seed;     // 60 TEXCOORD5
    };

    struct Beam
    {
        DirectX::XMFLOAT3 p0, p1;
        DirectX::XMFLOAT3 col;   // intensity 乗算済み
        float width;
        float age, life;
        float seed;
        int   kind;
        bool  alive = false;
    };

    // Beam.hlsl のインスタンスレイアウトと一致（stride 56）。
    struct GpuBeam
    {
        DirectX::XMFLOAT3 p0;     // 0  POSITION
        DirectX::XMFLOAT3 p1;     // 12 NORMAL
        DirectX::XMFLOAT4 color;  // 24 COLOR0
        float             halfW;  // 40 TEXCOORD0
        float             age01;  // 44 TEXCOORD1
        u32               kind;   // 48 TEXCOORD2
        float             seed;   // 52 TEXCOORD3
    };

    // トレイル内部表現（点の帯）。ノードは古い順に並ぶ。
    struct TrailNode
    {
        DirectX::XMFLOAT3 pos;
        float age = 0.0f;
    };
    struct Trail
    {
        u64 id = 0;
        TrailParams p;
        std::vector<TrailNode> nodes;
        float sinceTouched = 0.0f;   // TrailPoint が来なくなってからの経過秒
    };

    // Trail.hlsl の頂点レイアウトと一致（stride 36）。
    struct TrailVtx
    {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT4 col;
        DirectX::XMFLOAT2 uv;    // x=尾0..先頭1, y=横 -1..1
    };

    float Rand(float a, float b);
    void  ResetPool();   // 空きリスト/生存リストを初期状態へ（Initialize/Clear 共用）

    static constexpr u32 kMaxParticles   = 8000;
    static constexpr u32 kMaxBeams       = 512;
    static constexpr u32 kMaxChildDefs   = 2048;  // サブエミッタ定義リング（超過で古い定義を上書き）
    static constexpr u32 kMaxTrails      = 128;
    static constexpr u32 kMaxTrailNodes  = 96;    // 1トレイルあたりの最大点数
    static constexpr u32 kMaxTrailVerts  = 24576;
    // in-flight 多重度（SwapChain::kFrameCount と一致）。動的バッファをこの数の区画に
    // 分けてフレームごとに書き分ける（前フレームがGPUで読行中の区画を上書きしない）
    static constexpr u32 kFrames       = 3;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoAdd;    // 加算
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoAlpha;  // 前乗算アルファ（煙）
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoBeam;   // 加算ビーム
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDistort;   // 歪みバッファ行き（RG16F加算）
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoTrailAdd;  // トレイル（加算）
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoTrailAlpha;// トレイル（前乗算α）
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_instanceBuffer; // UPLOAD
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_beamBuffer;     // UPLOAD
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_trailBuffer;    // UPLOAD
    D3D12_VERTEX_BUFFER_VIEW                     m_vbView{};
    D3D12_VERTEX_BUFFER_VIEW                     m_beamVbView{};
    D3D12_VERTEX_BUFFER_VIEW                     m_trailVbView{};

    std::vector<Particle>   m_particles;
    std::vector<GpuParticle> m_gpu;     // 毎フレーム再構築（加算→α→歪み の順に詰める）
    std::vector<Beam>        m_beamPool;
    std::vector<GpuBeam>     m_gpuBeams;
    std::vector<u32>         m_freeList; // 空きスロットのスタック（O(1) 確保）
    std::vector<u32>         m_live;     // 生存パーティクルの密なインデックス列
    std::vector<EmitParams>  m_childDefs;     // サブエミッタ定義（リング上書き）
    std::vector<std::pair<DirectX::XMFLOAT3, int>> m_deathSpawns; // Update 中に溜める死亡時バースト
    std::vector<Trail>       m_trails;
    std::vector<TrailVtx>    m_trailVerts;    // 毎フレーム再構築（加算→α の順）
    u32   m_trailAddVerts = 0;          // m_trailVerts 内の加算頂点数（先頭から）
    u32   m_childCursor  = 0;
    u32   m_additiveCount = 0;          // m_gpu 内の加算粒子数（先頭から）
    u32   m_alphaCount   = 0;           // 続く α 粒子数
    u32   m_distortCount = 0;           // 末尾の歪み粒子数
    u32   m_frameIdx   = 0;             // 動的バッファの書き込み区画（Render毎に巡回）
    u32   m_beamCursor = 0;
    int   m_aliveCount = 0;
    float m_pulse      = 0.0f;
    float m_time       = 0.0f;
    std::mt19937 m_rng{1337u};
    bool  m_initialized = false;

    // soft particles 用
    D3D12_GPU_DESCRIPTOR_HANDLE m_depthSrv{};
    float m_projA = 0.0f, m_projB = 0.0f, m_invRTW = 0.0f, m_invRTH = 0.0f;
    bool  m_hasDepth = false;
};

} // namespace dx12e
