#include "resource/ShaderTemplates.h"

#include "core/PathResolver.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace dx12e::shadertemplates
{
namespace
{
// 実体は shaders-src/templates/*.hlsl（配布時は exe 隣、開発時はリポジトリの shaders/）。
// 雛形を足すときは .hlsl を置いてここに 1 行足す。
const std::vector<Info> kBuiltin = {
    {"water", "水（池・水路・プール）",
     "ゲルストナー波 + フレネル + 太陽のギラつき + 峰の泡。分割した平面に貼り、アルファブレンドを ON にする",
     "Water.hlsl"},
    {"ocean", "海（うねりと白波）",
     "長い波長のうねり 4 本 + 白波 + 遠景を大気色へ溶かす水平線。広い水面向け",
     "Ocean.hlsl"},
    {"particle_ember", "パーティクル: 燃えさし",
     "粒の中に温度勾配を持つ火の粉。芯が白熱し外へ向かって冷える。"
     "ParticleLayer::shaderPath に割り当てる（メッシュ用とは別契約）",
     "ParticleEmber.hlsl"},
};

std::filesystem::path TemplateDir()
{
    return std::filesystem::path(PathResolver::ShaderSourceDirW()) / "templates";
}
} // namespace

const std::vector<Info>& List()
{
    return kBuiltin;
}

bool Load(const std::string& name, std::string& outCode)
{
    for (const Info& t : kBuiltin)
    {
        if (t.name != name) continue;
        const std::filesystem::path full = TemplateDir() / t.file;
        std::ifstream ifs(full, std::ios::binary);
        if (!ifs) return false;
        std::ostringstream oss;
        oss << ifs.rdbuf();
        outCode = oss.str();
        return !outCode.empty();
    }
    return false;
}

nlohmann::json DescribeContract(const std::string& kind)
{
    using nlohmann::json;

    if (kind == "sprite")
    {
        return json{
            {"kind", "sprite"},
            {"assignTo", "Sprite2D::shaderPath（worldSpace=true のスプライトのみ）"},
            {"entryPoints", {"VSMain", "PSMain"}},
            {"note", "頂点レイアウトとルートシグネチャがメッシュ用と異なる。"
                     "メッシュ用の雛形をそのまま貼っても動かない（docs/AUTHORING.md 6.1）"},
        };
    }
    if (kind == "particle")
    {
        return json{
            {"kind", "particle"},
            {"assignTo", "ParticleLayer::shaderPath（set_component の shaderPath / layer 指定）"},
            {"entryPoints", {"VSMain", "PSMain"}},
            {"include",
             {{"header", "UnoParticle.hlsli"},
              {"usage", "#include \"UnoParticle.hlsli\""},
              {"why", "定数・入出力・ビルボード展開・ソフトパーティクルが全部入る。"
                      "メッシュ用の UnoCustom.hlsli とは別物なので取り違えないこと"}}},
            {"constants",
             {{"b0",
               {{"viewProj", "float4x4"},
                {"camRight", "float4  カメラ右ベクトル"},
                {"camUp", "float4  カメラ上ベクトル"},
                {"params", "float4  x=全体強度 y=グロー柔らかさ z=時間(秒) w=ソフトフェード距離"},
                {"params2", "float4  x=projA y=projB z=1/RT幅 w=1/RT高（z<=0 でソフト無効）"}}}}},
            {"textures",
             {{"t0", "シーン深度（R32_FLOAT）。UnoSoftParticle() が使う"},
              {"t2", "粒子テクスチャ（texIdx != kNoTexture のときだけ）"},
              {"s0", "SamplerState（LINEAR CLAMP）"}}},
            {"perParticleInput",
             {"center/size/color/rot/stretch/vel/age01/kind/seed/texIdx",
              "age01 は 0=生まれた瞬間 1=消える。色は放出器の color/colorEnd 補間済み"}},
            {"gotchas",
             {"出力は【前乗算アルファ】。色に α を掛けてから返すこと"
              "（掛け忘れると加算でふちが四角く光る）",
              "UnoSoftParticle() を最終アルファに掛けること。掛けないと床や壁に紙のように刺さり、"
              "壁の向こうの粒子まで見える（手動オクルージョンも兼ねている）",
              "使える register は b0 / t0 / t2 / s0 だけ。ほかを宣言すると"
              "コンパイルは通っても PSO 生成で落ちる（t1 は GPU パーティクルが使用中）",
              "GPU パーティクル(gpu=true)は compute 経路なので非対応。既定の見た目で描かれる",
              "ブレンドは放出器の blend(0=加算 / 1=前乗算α)がそのまま PSO に反映される"}},
            {"availableTemplates", {"particle_ember"}},
        };
    }
    if (kind == "screen")
    {
        return json{
            {"kind", "screen"},
            {"assignTo", "CameraComponent::screenShaderPath"},
            {"entryPoints", {"VSMain", "PSMain"}},
            {"textures", {{"t0", "画面カラー（LDR・ガンマ空間）"}, {"t1", "深度"}}},
            {"constants", {{"b0", "ScreenShaderCB"}}},
            {"note", "ポストプロセスの最後に 1 パス走る。完成した絵そのものを書き換える"},
        };
    }

    // 既定 = メッシュ用
    return json{
        {"kind", "mesh"},
        {"assignTo", "MeshRenderer::shaderPath（静的メッシュのみ。スキンドは既定シェーダーへフォールバック）"},
        {"entryPoints", {"VSMain", "PSMain"}},
        {"include",
         {{"header", "UnoCustom.hlsli"},
          {"usage", "#include \"UnoCustom.hlsli\""},
          {"why", "b0/b1 の定数・頂点レイアウト・波/ノイズ/フレネルの関数が全部入る。"
                  "cbuffer はオフセットで対応が決まるため、手で書き写すと 1 つのズレで"
                  "エラー無しに値が化ける。include すればその事故が起きない"}}},
        {"constants",
         {{"b0",
           {{"mvp", "float4x4  ローカル→クリップ（頂点を動かさないならこれだけでよい）"},
            {"model", "float4x4  ローカル→ワールド"},
            {"effectValue", "float   汎用の進捗/強度。Lua scene:setMeshEffect / Trigger SetShaderParam"},
            {"shaderParamsB", "float3  自由枠"},
            {"shaderParams", "float4  自由枠"}}},
          {"b1",
           {{"view", "float4x4  ワールド→ビュー"},
            {"proj", "float4x4  ビュー→クリップ"},
            {"lightDir", "float3   太陽の向き"},
            {"time", "float    秒（起動からの経過）"},
            {"lightColor", "float3"},
            {"ambientStrength", "float"},
            {"cameraPos", "float3   ワールド空間のカメラ位置（offset 448）"}}}}},
        {"textures",
         {{"t0", "albedo（マテリアル/D&D で割り当てたもの）"},
          {"t1", "normalMap"},
          {"t2", "metalRoughness"},
          {"s0", "SamplerState"}}},
        {"vertexInput",
         {"POSITION float3", "NORMAL float3", "COLOR float4", "TEXCOORD0 float2",
          "TANGENT float4", "BLENDINDICES uint4", "BLENDWEIGHT float4"}},
        {"pixelInput",
         {"SV_POSITION float4", "NORMAL float3 (worldNormal)", "TEXCOORD1 float3 (worldPos)",
          "TEXCOORD0 float2 (texCoord)"}},
        {"alphaBlend",
         "既定は不透明固定（PS が alpha を書いても無視される）。半透明にしたい場合は "
         "set_mesh_shader の alphaBlend=true / Inspector のチェックを ON にすること"},
        {"gotchas",
         {"頂点をワールド座標で動かしたら mvp ではなく UnoWorldToClip() を使う（mvp は model 込みなので二重になる）",
          "波や変位は頂点数に依存する。4 頂点の板ポリでは何も動かない",
          "ルートシグネチャに無い register を宣言するとコンパイルは通っても PSO 生成で落ちる"
          "（その診断は create_shader の issue に出る）",
          "スキンド（SkeletalAnimation 付き）には効かない。既定シェーダーで描かれる"}},
        {"availableTemplates", [] {
             nlohmann::json a = nlohmann::json::array();
             for (const Info& t : kBuiltin) a.push_back(t.name);
             return a;
         }()},
        {"notAvailableYet",
         {"シーン深度（浅瀬の泡・ソフトな交差）",
          "不透明を描き終えた画面のコピー（屈折）"}},
    };
}

} // namespace dx12e::shadertemplates
