#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;

// ===== スクリーンシェーダー（カメラに割り当てる「画面全体の .hlsl」）=====
//
// ポストプロセスの uber パスが終わった【最後】に走る 1 パス。完成した絵をテクスチャとして
// 受け取り、ユーザーが書いた HLSL で好きに書き換えてバックバッファへ出す。
//
//   MeshRenderer::shaderPath  … 1 個のモデルの描き方を差し替える
//   Sprite2D::shaderPath      … 1 枚のスプライトの描き方を差し替える
//   CameraComponent::screenShaderPath … ★画面そのものを差し替える（これ）
//
// ルートシグネチャはここが持つ（全スクリーンシェーダーで共有）。PSO だけシェーダーごとに作る。
//   t0 = 画面カラー（ポスト適用後の LDR / ガンマ空間）
//   t1 = シーン深度（R32_FLOAT。0=near, 1=far の非線形深度）
//   b0 = ScreenShaderCB（32bit ルート定数 16 個。CB リソースを作らないので Apply が安い）
//   s0 = linear clamp / s1 = point clamp
//
// エントリポイントはメッシュ用カスタムシェーダーと同じ VSMain / PSMain（vs_6_0 / ps_6_0）。
// バイトコードの取得は Application::FetchCustomShaderBytecode に任せる（＝エディタでは
// 実行時コンパイル + ホットリロード、配布ゲームでは game.pak の .cso を復号）。
class ScreenShaderPass
{
public:
    // HLSL の cbuffer ScreenShaderCB と一致させる（20 DWORD = 5 float4）。
    struct Constants
    {
        float resolution[4];   // xy=表示矩形のpx, zw=1/px
        float timeParams[4];   // x=経過秒, y=デルタ秒, z=アスペクト(W/H), w=フレーム番号
        float params[4];       // CameraComponent::screenShaderParams（意味はシェーダー依存）
        float cameraParams[4]; // x=near, y=far, z=垂直FOV(度), w=正射なら1
        // ★入力テクスチャは【ウィンドウ全面】の RT で、絵はその中の表示矩形にだけ入っている。
        //   uv(0..1) から実際のテクセルへ写すためのオフセット/スケール。
        //   雛形の SampleScreen() / SampleDepth() がこれを内部で掛けるので、
        //   シェーダーを書く人は uv 0..1 だけ意識すればよい。
        float uvOffsetScale[4]; // xy=オフセット, zw=スケール
    };
    static_assert(sizeof(Constants) == 20 * sizeof(float), "ScreenShaderCB must be 20 DWORDs");

    void Initialize(GraphicsDevice& device, DXGI_FORMAT outFormat);

    // shaderRel（assets/shaders 相対のキー。正規化済みを渡すこと）に対する PSO を返す。
    // 未生成なら vsBytes/psBytes から作る。作れなければ nullptr（呼び出し側は素通しへ）。
    ID3D12PipelineState* GetOrCreatePso(GraphicsDevice& device,
                                        const std::string& key,
                                        const std::vector<u8>& vsBytes,
                                        const std::vector<u8>& psBytes,
                                        std::string* outError = nullptr);

    // ホットリロード時に呼ぶ（該当キーだけ / key が空なら全部捨てる）。
    void InvalidatePso(const std::string& key);

    // すでに作った PSO があるか（無ければ呼び出し側がバイトコードを取りに行く）。
    ID3D12PipelineState* FindPso(const std::string& key) const;

    // 1 パス発行。colorSrv/depthSrv は SRV ヒープ上のハンドル、rtv は出力先。
    // ビューポート/シザーは呼び出し側が張っておくこと（表示矩形へ描くため）。
    void Apply(ID3D12GraphicsCommandList* cmd,
               ID3D12PipelineState* pso,
               D3D12_GPU_DESCRIPTOR_HANDLE colorSrv,
               D3D12_GPU_DESCRIPTOR_HANDLE depthSrv,
               const Constants& cb);

    bool IsReady() const { return m_rootSig != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    // 直列化済みのルートシグネチャ。PSO 生成に失敗したとき、シェーダーのリフレクションと
    // 突き合わせて「どの register が余計なのか」を名指しするために取っておく
    // （デバッグレイヤーが無い Release ビルドでも同じ説明を出せる）。
    Microsoft::WRL::ComPtr<ID3DBlob> m_rsBlob;
    DXGI_FORMAT m_outFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    struct Entry
    {
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        bool tried = false;   // 失敗しても毎フレーム作り直さない（ログが溢れる）
    };
    std::unordered_map<std::string, Entry> m_psos;
};

} // namespace dx12e
