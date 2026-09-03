#pragma once

#include "core/Types.h"
#include "resource/ShaderParams.h"

#include <entt/entt.hpp>
#include <string>
#include <vector>

// ===== 名前付きシェーダーパラメーターを「イベントで動かす」 =====
//
// カスタムシェーダーのパラメーター（resource/ShaderParams.h）は Inspector で手で
// 動かせるだけで、ゲーム中に変化させるには Lua を書くしかなかった。しかも画面
// シェーダー（CameraComponent::screenShaderPath）には Lua の口すら無く、
// 「部屋に入った瞬間まぶしくする」のような演出がデータだけでは作れなかった。
//
// ここは Trigger の SetShaderParam / AnimShaderParam アクションの実体。
// 対象エンティティが使っているシェーダーの宣言をリフレクション済みの名前で引き、
// その 1 個の float を即代入 or 時間をかけて動かす。
//
// ★「どの名前が使えるか」の出どころは ListShaderParams() 1 本に集約する。
//   Inspector のコンボもトリガーの名前解決も同じ答えを見るので、
//   UI で選べた名前は必ず動く（手打ちのタイポで黙って効かない、が起きない）。
namespace dx12e
{

// AnimShaderParam のイージング。TriggerAction::vec.z に整数で入る。
enum class ShaderTweenEase : u8
{
    Linear = 0,
    Out    = 1,   // 勢いよく始まって減速（フラッシュが引くときの定番）
    In     = 2,   // ゆっくり始まって加速
    InOut  = 3,
    Count  = 4,
};
const char* ShaderTweenEaseName(int e);

// e が使っているカスタムシェーダーが宣言している名前付きパラメーターを返す。
// MeshRenderer::shaderPath と CameraComponent::screenShaderPath の両方を見る
// （両方持っていれば両方返る。どちらの枠かは Param::space に入っている）。
std::vector<shaderparams::Param> ListShaderParams(const entt::registry& reg, entt::entity e);

// 名前で 1 個引く。見つからなければ false。
bool FindShaderParam(const entt::registry& reg, entt::entity e,
                     const std::string& name, shaderparams::Param& out);

// パラメーターへ書く。ベクトル型は先頭成分（.x）を対象にする。
// 対象のコンポーネントが無い / 添字が枠外なら false。
bool WriteShaderParam(entt::registry& reg, entt::entity e,
                      const shaderparams::Param& p, f32 value);

// 進行中のトゥイーンの置き場。Play 中だけ動く（ScriptEngine::UpdateTriggers が回す）。
class ShaderParamTweens
{
public:
    // from → to へ duration 秒。duration が 0 以下なら即 to を書いて終わる。
    // 同じ (対象, 枠, 添字) に進行中のものがあれば打ち切って差し替える
    // ＝出入りを往復しても二重に動いて値が飛ばない。
    //
    // ★keepIfIdentical は Stay（居る間 毎フレーム発火）用。true なら「まったく同じ
    //   from/to/秒数/ease が既に走っている」ときだけ積み直さずに走らせ続ける。
    //   これが無いと Stay アクションは毎フレーム打ち切って積み直す＝進行がリセットされ続け、
    //   値が開始値に張り付いたまま永久に目標へ着かない（動いていないようにしか見えない）。
    //   Enter/Exit は false のまま＝入り直したらちゃんと頭から掛け直る。
    bool Start(entt::registry& reg, entt::entity target, const std::string& name,
               f32 from, f32 to, f32 duration, ShaderTweenEase ease,
               bool keepIfIdentical = false);

    // 即代入（進行中のトゥイーンがあれば打ち切る）。
    bool SetNow(entt::registry& reg, entt::entity target, const std::string& name, f32 value);

    void   Update(entt::registry& reg, f32 dt);
    void   Clear() { m_items.clear(); }
    size_t ActiveCount() const { return m_items.size(); }

private:
    struct Item
    {
        entt::entity        target = entt::null;
        shaderparams::Space space  = shaderparams::Space::MeshObject;
        u32                 index  = 0;   // 自由枠先頭からの float 添字
        f32                 from = 0.0f, to = 0.0f, duration = 0.0f, elapsed = 0.0f;
        ShaderTweenEase     ease = ShaderTweenEase::Linear;
    };

    // 同じスロットを指す進行中のものを消す。
    void CancelSlot(entt::entity target, shaderparams::Space space, u32 index);

    std::vector<Item> m_items;
};

} // namespace dx12e
