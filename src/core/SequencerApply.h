#pragma once
// シーケンサー S1b: 適用層(純関数 Evaluate の結果をエンジンへ書く)。
//
//   Evaluate(seq, t) → EvalResult(値の列)  ──▶  ApplySequence(…)  ──▶  registry(Transform / 任意コンポーネントのフィールド)
//                                                                 └─▶  ApplyOutput(カットのカメラ・ポスト/DoF の上書き値・カメラシェイク)
//
// ★エンジン状態の書き換えは 2 種類に分ける(設計書 §3.3)。
//   ・エンティティのコンポーネント(Transform / ライト / カメラ / プロパティ)= 書き込む。エディタのスクラブでは
//     PreAnimatedState が元値を先に退避し、閉じる / Play / 保存の直前に戻す(非破壊)。Play 中の再生は戻さない(結果を残す。Stop でシーンごと復元)。
//   ・ポスト / DoF / カメラの揺れ / アクティブカメラの選択 = 書かない。ApplyOutput に溜めるだけで、描画側が「描画時のコピー」に上書きする
//     (シーンを汚さないので復元も要らない)。
//
// 決定論: 書き込みはバインディング配列順 → トラック順。同じ (対象, プロパティ) を複数トラックが書くと後ろが勝つ。
// 物理との競合規則・親子・スキンドの扱いは docs/DXSEQ_FORMAT.md §19。

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>

#include "ecs/Components.h"
#include "sequencer/SeqBinding.h"
#include "sequencer/SeqEval.h"
#include "sequencer/SeqModel.h"

namespace dx12e
{
struct PostProcessSettings;

namespace seqhost
{

// ---------------------------------------------------------------------------
// プロパティアダプタ(entt::meta。ComponentMeta.cpp に登録された約 40 コンポーネントのフィールド名でアクセスする)
// ---------------------------------------------------------------------------
struct FieldRef
{
    enum class Kind : std::uint8_t { Invalid, Float, Int, Bool, Vec2, Vec3, Vec4, Enum };
    Kind kind = Kind::Invalid;
    entt::meta_data data{};
    std::string comp;        // meta 上の型名("PointLight" 等。"Light" 総称は解決後の実名)
    std::string field;
    bool Valid() const { return kind != Kind::Invalid; }
    int Components() const { return kind == Kind::Vec2 ? 2 : kind == Kind::Vec3 ? 3 : kind == Kind::Vec4 ? 4 : 1; }
};

// meta 型名 → エンティティからその部品への参照ハンドル(無ければ空)。登録の無い名前は nullptr。
using CompHandleFn = entt::meta_any (*)(entt::registry&, entt::entity);
CompHandleFn FindCompHandle(std::string_view compName);
// 対応しているコンポーネント名の一覧(MCP のエラーメッセージ・辞書用)
std::vector<std::string> SupportedComponentNames();

// "Component.field"(または総称 "Light.intensity")を解決する。失敗したら err に日本語の理由。
// entity は総称("Light")の解決と存在確認に使う(部品を持っていなければ err)。
bool ResolveFieldRef(entt::registry& reg, entt::entity entity, std::string_view comp, std::string_view field,
                     FieldRef& out, std::string& err);

// ---------------------------------------------------------------------------
// ポストプロセス(scene バインディング / カメラの DoF)= 描画時のコピーへの上書き値
// ---------------------------------------------------------------------------
struct PostFieldInfo
{
    const char* name = "";
    char kind = 'F';         // B / F / I / V / S(DX12E_POST_FIELDS の種別)
    int onIndex = -1;        // この値を書いたら自動で ON にする bool フィールドの添字(無ければ -1)
    void (*set)(PostProcessSettings&, int comp, double v) = nullptr;   // S は nullptr(文字列は補間しない)
};
const std::vector<PostFieldInfo>& PostFieldTable();
int FindPostField(std::string_view name);            // -1 = 無い

struct PostWrite
{
    int field = -1;          // PostFieldTable の添字
    int comp = 0;            // V(XMFLOAT3)の成分 0..2
    double value = 0.0;
    bool camera = false;     // カメラ視点でしか意味を持たない(DoF)。エディタの自由カメラのビューには適用しない
};
// 値のフィールド(F/I/V)を先に書き(=対応する XxxOn を自動 ON)、そのあと明示の bool を書く(明示の OFF が勝つ)。
// includeCamera=false ならカメラ視点専用の書き込み(DoF)を飛ばす(エディタの自由カメラのビュー)。
void ApplyPostWrites(PostProcessSettings& pp, const std::vector<PostWrite>& writes, bool includeCamera = true);

// ---------------------------------------------------------------------------
// バインディング結果 + トラックのコンパイル(チャンネル名 → 書き込み先)
// ---------------------------------------------------------------------------
struct BoundChannel
{
    enum class Target : std::uint8_t { None, Transform, Field, Post, Dof, TimeScale };
    Target target = Target::None;
    int a = 0;               // Transform: 0 位置 / 1 回転 / 2 スケール。Field: 成分(-1 = スカラー / 0..3)。Post・Dof: PostFieldTable の添字
    int b = 0;               // Transform: 成分 0..2。Post・Dof: V の成分
    FieldRef field;          // Target::Field のとき
};

struct BoundTrack
{
    seq::TrackType type = seq::TrackType::Transform;
    std::vector<BoundChannel> chans;    // track.channels と同じ並び
    std::string warning;                // このトラックが(一部でも)書けない理由。空 = 問題なし
};

struct BoundSequence
{
    std::vector<entt::entity> entity;           // バインディングごとの対象(scene / 未解決 = null)
    std::vector<std::uint64_t> guid;            // 解決時点の EntityGuid(0 = 無し)。生存確認に使う
    seq::BindingSet set;                        // 解決の経路・警告(S0 の BindingSet)
    std::vector<std::uint8_t> mask;             // EvalFilter::bindingEnabled 用
    std::vector<std::vector<BoundTrack>> tracks;
    std::vector<std::string> warnings;          // 日本語(解決の警告 + プロパティの解決失敗)
    std::uint32_t aliveCount = 0;               // 解決時点のエンティティ数(増減で再解決の要否を見る)
    bool Bound() const { return !entity.empty() || set.byBinding.empty(); }
};

struct BindOptions
{
    bool ensureGuids = false;                            // エディタ: 参照するエンティティへ guid を確定させる
    const seq::BindingOverrides* overrides = nullptr;    // SequencePlayer 側の上書き(bindingId → guid)
};
// 解決 + トラックのコンパイル。out は作り直される。
void BindSequence(const seq::Sequence& seq, entt::registry& reg, const BindOptions& opt, BoundSequence& out);
// 束縛済みのエンティティがまだ生きていて guid も同じか / エンティティの数が変わっていないか(= 束縛が古くなっていないか)。
// checkAliveCount=false は Play 中の再生用(エンティティが毎フレーム増減するゲームで、数の変化だけでは束縛し直さない)。
bool BoundIsFresh(const BoundSequence& b, const entt::registry& reg, bool checkAliveCount = true);
// 生きているエンティティの数(BoundSequence::aliveCount と比べる)
std::uint32_t AliveEntityCount(const entt::registry& reg);

// ---------------------------------------------------------------------------
// PreAnimatedState(エディタのスクラブが触った値の元値)
// ---------------------------------------------------------------------------
class PreAnimatedState
{
public:
    bool Empty() const { return m_entries.empty(); }
    std::size_t Count() const { return m_entries.size(); }

    // 書き込む直前に呼ぶ。同じ (guid, 対象) は最初の 1 回だけ退避する。
    void SaveTransform(const entt::registry& reg, entt::entity e, std::uint64_t guid);
    void SaveField(std::uint64_t guid, const FieldRef& f, entt::meta_any& handle);

    // Transform の「最後に書いた値」と今の値を比べる(ユーザーが制御中の値を触ったかの検出)。
    // 触られていたら true。触っても元値の復元は変えない(保存への混入ゼロを優先)。
    bool TransformDrifted(std::uint64_t guid, const Transform& current) const;
    void NoteTransformWritten(std::uint64_t guid, const Transform& written);

    // 退避の逆順に元へ戻す。guid で引き直すので Undo で作り直されたエンティティにも効く。戻した数を返す。
    // 戻す先のエンティティが(いま)無いときの扱い: keepMissing=false(セッション終了)は捨てる /
    // keepMissing=true(保存の直前)は元値を持ち続ける = 後から同じ guid で作り直された(Undo)エンティティに、セッション終了で元値を戻せる。
    int RestoreAll(entt::registry& reg, bool keepMissing = false);
    void Clear() { m_entries.clear(); m_index.clear(); }

private:
    struct Entry
    {
        std::uint64_t guid = 0;
        bool isTransform = true;
        Transform tf{};             // 元値(isTransform)
        Transform lastWritten{};
        bool hasLastWritten = false;
        FieldRef field{};
        entt::meta_any value{};     // 元値(!isTransform)
    };
    std::vector<Entry> m_entries;
    std::unordered_map<std::string, std::size_t> m_index;   // キー("guid|comp.field" / "guid|tf")→ 添字
};

// ---------------------------------------------------------------------------
// 適用
// ---------------------------------------------------------------------------
struct ShakeWrite
{
    entt::entity entity = entt::null;   // 揺らすカメラ(バインディングの対象)
    float x = 0, y = 0, z = 0;          // ワールド座標での加算オフセット
};

struct ApplyOutput
{
    entt::entity cutCamera = entt::null;    // カットが選んだカメラ(無ければ null)
    int cutIndex = -1;
    std::vector<PostWrite> post;            // ポスト / DoF(描画時のコピーへ上書き)
    std::vector<ShakeWrite> shakes;
    bool hasTimeScale = false;
    float timeScale = 1.0f;
    // 統計・警告
    int transformsWritten = 0;
    int fieldsWritten = 0;
    int skippedPhysics = 0;                 // 動的な剛体 / キャラコントローラ(物理が位置を決める)を書かなかった数
    int skippedMissing = 0;                 // 対象が無い / 部品が無いで書けなかった数
    int userDrift = 0;                      // ユーザーが制御中の Transform を触った検出数(エディタ)
    std::vector<std::string> warnings;
    void Clear()
    {
        cutCamera = entt::null; cutIndex = -1; post.clear(); shakes.clear(); hasTimeScale = false; timeScale = 1.0f;
        transformsWritten = fieldsWritten = skippedPhysics = skippedMissing = userDrift = 0; warnings.clear();
    }
};

struct ApplyOptions
{
    PreAnimatedState* pre = nullptr;   // エディタのスクラブ: 書く前に元値を退避する。nullptr(Play の再生)なら退避しない
    bool physicsActive = false;        // Play 中(物理が動いている)= 動的な剛体の Transform は書かない
};

// res(seq を t で評価した結果)を registry へ書く。bound は BindSequence の結果(seq と同じもの)。
void ApplySequence(const seq::Sequence& seq, const seq::EvalResult& res, const BoundSequence& bound,
                   entt::registry& reg, const ApplyOptions& opt, ApplyOutput& out);

// カメラシェイクの式(旧 sequence_author の Lua と同じ: a = amp·decay²、位相 = 経過秒·freq、3 軸は sin の別周波数)。純関数。
// dur/local はティック、decayPow は減衰の冪(2 = 旧式)。
void ShakeOffset(double timeSec, double amp, double freq, double seed, double decayPow, double progress01, double weight,
                 float& x, float& y, float& z);

} // namespace seqhost
} // namespace dx12e
