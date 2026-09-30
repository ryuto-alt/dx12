// シーケンサー S1b: バインディング解決 + 適用層 + PreAnimatedState(非破壊スクラブ)+ Play 統合(core/SequencerHost.*)。
//
// エンジン(GPU / Application)を起こさずに、Scene(registry だけ)と SequencerHost を直接叩いて検査する。
//   [1] バインディング解決(guid → 階層パス → 名前 / 重複名 / 未解決 / 削除・再作成後の再解決 / guid 確定)
//   [2] 適用の決定論(任意順で評価・適用した結果が一致)/ Transform(親子・クォータニオン)/ プロパティ(entt::meta)
//   [3] カメラ: カットの選択・FOV・DoF・フォーカス距離(描画時のコピーへの上書き。シーンは無変更)・シェイク
//   [4] PreAnimatedState: スクラブ → 保存(Save / SaveToString / SerializeEntity / SerializeSubtree)にスクラブの値が【絶対に】混ざらない /
//       閉じる・Play 直前・文書を閉じる・シーンが変わる で戻る / エンティティ削除中のスクラブ / 別 Scene の保存には触らない
//   [5] Play: 実時間の時計・イベント(前進で 1 回・スクラブでは発火しない・ループ・エディタのプレビューは発火しない)・
//       timeScale の復元・:done・物理との競合規則
//   [6] 文書: SeqOp の JSON(ops)・undo / redo・dryRun・保存 / 読み込み・シーンの自動再生設定
//
// 実行: ctest --output-on-failure -R SequencerBind

#include "core/SequencerApply.h"
#include "core/SequencerBinding.h"
#include "core/SequencerHost.h"
#include "ecs/Components.h"
#include "renderer/Mesh.h"   // Scene のデストラクタに Mesh の完全型が要る
#include "renderer/PostProcessSettings.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace dx12e;
using namespace dx12e::seqhost;

namespace
{
int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* expr, int line)
{
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::printf("FAIL line %d: %s\n", line, expr);
}
} // namespace
#define CHECK(c) Check((c), #c, __LINE__)
#define CHECK_NEAR(a, b, eps) Check(std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= (eps), #a " ~= " #b, __LINE__)

namespace
{
constexpr seq::Tick kSec = 6000;   // 1 秒 = 6000 tick

// ---- シーンの組み立て --------------------------------------------------------
entt::entity Make(Scene& s, const char* name, DirectX::XMFLOAT3 pos = { 0, 0, 0 }, entt::entity parent = entt::null)
{
    auto& reg = s.GetRegistry();
    const entt::entity e = reg.create();
    reg.emplace<NameTag>(e, NameTag{ name });
    Transform t;
    t.position = pos;
    t.parent = parent;
    reg.emplace<Transform>(e, t);
    return e;
}

bool SameTransform(const Transform& a, const Transform& b)
{
    return std::memcmp(&a.position, &b.position, sizeof(a.position)) == 0 && std::memcmp(&a.rotation, &b.rotation, sizeof(a.rotation)) == 0 &&
           std::memcmp(&a.scale, &b.scale, sizeof(a.scale)) == 0 && std::memcmp(&a.quaternion, &b.quaternion, sizeof(a.quaternion)) == 0 &&
           a.useQuaternion == b.useQuaternion;
}

// 標準のテストシーン: Box / Cam1(カメラ)/ Cam2(カメラ)/ Lamp(点光源)。全員に guid を振る(保存で振られるのと同じ)。
struct World
{
    Scene scene;
    entt::entity box{}, cam1{}, cam2{}, lamp{};
    World()
    {
        auto& reg = scene.GetRegistry();
        box = Make(scene, "Box", { 1, 2, 3 });
        cam1 = Make(scene, "Cam1", { 0, 5, -10 });
        cam2 = Make(scene, "Cam2", { 20, 5, -10 });
        lamp = Make(scene, "Lamp", { 0, 8, 0 });
        CameraComponent c;
        c.fovDegrees = 60.0f;
        reg.emplace<CameraComponent>(cam1, c);
        c.isActive = true;
        reg.emplace<CameraComponent>(cam2, c);
        PointLight pl;
        pl.intensity = 2.0f;
        pl.color = { 1, 0.5f, 0.25f };
        reg.emplace<PointLight>(lamp, pl);
        for (const entt::entity e : { box, cam1, cam2, lamp }) seqhost::EnsureEntityGuid(reg, e);
    }
    entt::registry& Reg() { return scene.GetRegistry(); }
};

// 標準のシーケンス(24000 tick = 4 秒。2 秒でカット)
const char* kSeqText = R"JSON({
  "format": "dxseq", "version": 1, "name": "T", "ticksPerSecond": 6000, "frameRate": 30,
  "cuts": [
    {"id": "c1", "start": 0, "end": 12000, "camera": "b_cam1"},
    {"id": "c2", "start": 12000, "end": 24000, "camera": "b_cam2"}
  ],
  "markers": [],
  "bindings": [
    {"id": "b_box", "name": "Box", "kind": "entity", "hint": {"name": "Box"}, "tracks": [
      {"id": "t_box", "type": "transform", "rotation": "euler", "channels": {
        "position.x": {"keys": [[0, 0, "l"], [12000, 10, "l"]]},
        "position.y": {"keys": [[0, 4, "s"]]},
        "rotation.y": {"keys": [[0, 0, "l"], [12000, 90, "l"]]},
        "scale.x": {"keys": [[0, 1, "l"], [12000, 3, "l"]]}}}]},
    {"id": "b_cam1", "name": "Cam1", "kind": "entity", "hint": {"name": "Cam1"}, "tracks": [
      {"id": "t_fov", "type": "camera", "focus": {"binding": "b_box"}, "channels": {
        "fov": {"keys": [[0, 60, "l"], [12000, 30, "l"]]},
        "dofAperture": {"keys": [[0, 1.5, "s"]]}}},
      {"id": "t_shk", "type": "shake", "clips": [{"id": "k1", "start": 0, "dur": 6000, "amp": 0.5, "freq": 10, "seed": 1}]}]},
    {"id": "b_cam2", "name": "Cam2", "kind": "entity", "hint": {"name": "Cam2"}, "tracks": []},
    {"id": "b_lamp", "name": "Lamp", "kind": "entity", "hint": {"name": "Lamp"}, "tracks": [
      {"id": "t_int", "type": "property", "path": "PointLight.intensity", "channels": {"value": {"keys": [[0, 1, "l"], [12000, 5, "l"]]}}},
      {"id": "t_col", "type": "property", "path": "PointLight.color", "valueType": "color", "channels": {
        "r": {"keys": [[0, 0, "s"]]}, "g": {"keys": [[0, 1, "s"]]}, "b": {"keys": [[0, 0.5, "s"]]}}},
      {"id": "t_sh", "type": "property", "path": "PointLight.castShadows", "valueType": "bool", "channels": {"value": {"keys": [[0, 0, "s"], [6000, 1, "s"]]}}}]},
    {"id": "b_scene", "name": "Scene", "kind": "scene", "tracks": [
      {"id": "t_post", "type": "post", "channels": {"bloom": {"keys": [[0, 0.2, "l"], [12000, 0.8, "l"]]}}},
      {"id": "t_ev", "type": "event", "events": [
        {"id": "e_boom", "t": 3000, "kind": "emit", "name": "boom", "data": {"value": 7}},
        {"id": "e_lua", "t": 6000, "kind": "lua", "name": "OnBoom", "args": [1, "x", true]},
        {"id": "e_log", "t": 9000, "kind": "log", "name": "halfway"}]}]}
  ]
})JSON";

// ホスト + コールバックの記録
struct Rig
{
    World w;
    SequencerHost host;
    std::vector<std::string> emitted;
    std::vector<std::string> luaCalls;
    float timeScale = 1.0f;
    std::vector<float> timeScaleWrites;
    Rig()
    {
        HostCallbacks cb;
        cb.emit = [this](const EngineEvent& e) { emitted.push_back(e.name + (e.data.empty() ? "" : "=" + std::to_string(static_cast<int>(e.num("value", -1))))); };
        cb.callLua = [this](const std::string& fn, const std::vector<EngineEvent::Value>& args, std::string&) {
            luaCalls.push_back(fn + "/" + std::to_string(args.size()));
            return true;
        };
        cb.setTimeScale = [this](float s) { timeScale = s; timeScaleWrites.push_back(s); };
        cb.getTimeScale = [this]() { return timeScale; };
        host.SetCallbacks(std::move(cb));
        host.SetScene(&w.scene);
        host.InstallSaveHook();
    }
    DocPtr Doc(const char* name = "T", const char* text = kSeqText)
    {
        std::string err;
        DocPtr d = host.AddDocFromText(name, text, err);
        if (!d) std::printf("  (AddDocFromText: %s)\n", err.c_str());
        return d;
    }
    // Editor モードのフレーム(dt=0)
    void EditorFrame(std::uint64_t token = 1) { host.Update(0.0f, HostMode::Editor, false, token); }
    void PlayFrame(float dt, std::uint64_t token = 1) { host.Update(dt, HostMode::Playing, false, token); }
};

std::string SaveJson(Scene& s) { return SceneSerializer::SaveToString(s, ""); }

// ===========================================================================
void TestBinding()
{
    std::printf("[1] バインディング解決\n");
    Scene scene;
    auto& reg = scene.GetRegistry();
    const entt::entity a = Make(scene, "Target", { 1, 0, 0 });
    const entt::entity rig = Make(scene, "Rig");
    const entt::entity child = Make(scene, "Cam", { 0, 1, 0 }, rig);

    // guid 確定 API
    CHECK(reg.try_get<EntityGuid>(a) == nullptr);
    const std::uint64_t g1 = seqhost::EnsureEntityGuid(reg, a);
    CHECK(g1 != 0);
    CHECK(seqhost::EnsureEntityGuid(reg, a) == g1);   // 2 回目は同じ値
    CHECK(reg.get<EntityGuid>(a).value == g1);

    // 階層パスと hint
    CHECK(seqhost::EntityHierarchyPath(reg, child) == "Rig/Cam");
    const seq::BindingHint h = seqhost::MakeBindingHint(reg, child, true);
    CHECK(h.path == "Rig/Cam" && h.name == "Cam" && !h.guid.empty());
    CHECK(reg.try_get<EntityGuid>(child) != nullptr);   // MakeBindingHint が guid を確定させた

    seq::Sequence s;
    seq::Binding b;
    b.id = "b_x"; b.name = "X"; b.hint = h;
    s.bindings.push_back(b);

    // guid で解決
    {
        seqhost::EngineBindingResolver r(reg);
        const seq::BindingSet set = seq::ResolveBindings(s, r);
        CHECK(set.byBinding[0].via == seq::ResolveVia::Guid);
        CHECK(seqhost::ToEntity(set.byBinding[0].target) == child);
    }
    // guid が消えても(複製・プレハブ展開の再現)階層パスで解決 → FellBack
    reg.remove<EntityGuid>(child);
    {
        seqhost::EngineBindingResolver r(reg);
        const seq::BindingSet set = seq::ResolveBindings(s, r);
        CHECK(set.byBinding[0].via == seq::ResolveVia::Path);
        CHECK(seqhost::ToEntity(set.byBinding[0].target) == child);
        bool fell = false;
        for (const auto& is : set.issues) if (is.code == seq::BindingIssue::Code::FellBack) fell = true;
        CHECK(fell);
    }
    // パスも変わったら名前で
    s.bindings[0].hint.path = "Old/Cam";
    {
        seqhost::EngineBindingResolver r(reg);
        const seq::BindingSet set = seq::ResolveBindings(s, r);
        CHECK(set.byBinding[0].via == seq::ResolveVia::Name);
    }
    // 名前が重複 → 最後に作られたものが先頭(Scene::FindEntity と同じ)+ AmbiguousName の警告
    const entt::entity dup1 = Make(scene, "Dup");
    const entt::entity dup2 = Make(scene, "Dup");
    (void)dup1;
    s.bindings[0].hint = {}; s.bindings[0].hint.name = "Dup";
    {
        seqhost::EngineBindingResolver r(reg);
        const seq::BindingSet set = seq::ResolveBindings(s, r);
        CHECK(seqhost::ToEntity(set.byBinding[0].target) == dup2);
        CHECK(set.byBinding[0].candidates == 2);
        bool amb = false;
        for (const auto& is : set.issues) if (is.code == seq::BindingIssue::Code::AmbiguousName) amb = true;
        CHECK(amb);
        // 同じ規則を Scene::FindEntity も持っている(規則が食い違っていない)
        CHECK(scene.FindEntity("Dup").GetHandle() == dup2);
    }
    // 未解決 → 警告として保持(トラックは消えない)・mask 0
    s.bindings[0].hint = {}; s.bindings[0].hint.name = "Nobody";
    {
        BoundSequence bound;
        BindSequence(s, reg, BindOptions{}, bound);
        CHECK(!bound.set.byBinding[0].Resolved());
        CHECK(bound.mask[0] == 0);
        CHECK(bound.entity[0] == entt::null);
        CHECK(!bound.warnings.empty());
        CHECK(s.bindings.size() == 1);   // データは消えない
    }

    // 束縛の鮮度: 削除 → 古い、 再作成(同じ guid)→ 束縛し直しで解決
    seq::Sequence s2;
    seq::Binding b2;
    b2.id = "b_a"; b2.name = "Target"; b2.hint = seqhost::MakeBindingHint(reg, a, true);
    s2.bindings.push_back(b2);
    BoundSequence bound2;
    BindSequence(s2, reg, BindOptions{}, bound2);
    CHECK(bound2.entity[0] == a);
    CHECK(BoundIsFresh(bound2, reg));
    const std::uint64_t oldGuid = reg.get<EntityGuid>(a).value;
    reg.destroy(a);
    CHECK(!BoundIsFresh(bound2, reg));
    const entt::entity a2 = Make(scene, "Target", { 9, 9, 9 });
    reg.emplace<EntityGuid>(a2, EntityGuid{ oldGuid });   // Undo の keepGuid=true と同じ
    BindSequence(s2, reg, BindOptions{}, bound2);
    CHECK(bound2.entity[0] == a2);
    CHECK(bound2.set.byBinding[0].via == seq::ResolveVia::Guid);

    // ensureGuids: 束縛時に guid を確定させる
    const entt::entity naked = Make(scene, "Naked");
    seq::Sequence s3;
    seq::Binding b3;
    b3.id = "b_n"; b3.name = "Naked"; b3.hint.name = "Naked";
    s3.bindings.push_back(b3);
    BoundSequence bound3;
    BindOptions bo;
    bo.ensureGuids = true;
    BindSequence(s3, reg, bo, bound3);
    CHECK(reg.try_get<EntityGuid>(naked) != nullptr && reg.get<EntityGuid>(naked).value == bound3.guid[0]);
}

// ===========================================================================
void TestApply()
{
    std::printf("[2] 適用(Transform / プロパティ / 決定論)\n");
    Rig r;
    auto& reg = r.w.Reg();
    DocPtr d = r.Doc();
    CHECK(d != nullptr);
    std::string err;
    CHECK(r.host.EditorScrub(d, 6000, err));   // 1 秒

    const Transform& t = reg.get<Transform>(r.w.box);
    CHECK_NEAR(t.position.x, 10.0 * 6000.0 / 12000.0, 1e-4);   // 線形 0→10 の半分
    CHECK_NEAR(t.position.y, 4.0, 1e-6);
    CHECK_NEAR(t.position.z, 3.0, 1e-6);                        // 書かなかったチャンネルは元のまま
    CHECK_NEAR(t.rotation.y, 45.0, 1e-4);
    CHECK_NEAR(t.scale.x, 2.0, 1e-4);
    CHECK_NEAR(t.scale.y, 1.0, 1e-6);
    // プロパティ(entt::meta)
    const PointLight& pl = reg.get<PointLight>(r.w.lamp);
    CHECK_NEAR(pl.intensity, 3.0, 1e-4);
    CHECK_NEAR(pl.color.x, 0.0, 1e-6);
    CHECK_NEAR(pl.color.y, 1.0, 1e-6);
    CHECK_NEAR(pl.color.z, 0.5, 1e-6);
    CHECK(pl.castShadows == true);                               // 1 秒(6000)で 0→1 の Step 切替
    CHECK_NEAR(reg.get<CameraComponent>(r.w.cam1).fovDegrees, 45.0, 1e-4);   // camera トラックの fov

    // 決定論: 任意の順序で評価・適用しても、同じ時刻なら同じ結果(ビット一致)
    std::mt19937 rng(12345);
    std::vector<seq::Tick> ts;
    for (int i = 0; i < 40; ++i) ts.push_back(static_cast<seq::Tick>(rng() % 30000));
    std::vector<Transform> first;
    for (seq::Tick tk : ts) { CHECK(r.host.EditorScrub(d, tk, err)); first.push_back(reg.get<Transform>(r.w.box)); }
    std::vector<std::size_t> order(ts.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng);
    bool allSame = true;
    for (std::size_t i : order)
    {
        r.host.EditorScrub(d, ts[i], err);
        if (!SameTransform(reg.get<Transform>(r.w.box), first[i])) allSame = false;
    }
    CHECK(allSame);
    r.host.EditorEnd();
}

// ===========================================================================
void TestTransformDetails()
{
    std::printf("[2b] Transform: 親子・クォータニオン・未対応プロパティの警告\n");
    Rig r;
    auto& reg = r.w.Reg();
    const entt::entity parent = Make(r.w.scene, "Parent", { 100, 0, 0 });
    const entt::entity kid = Make(r.w.scene, "Kid", { 1, 0, 0 }, parent);
    const char* text = R"JSON({"format":"dxseq","version":1,"name":"P","ticksPerSecond":6000,"frameRate":30,
      "bindings":[{"id":"b_kid","name":"Kid","hint":{"name":"Kid"},"tracks":[
        {"id":"t_tr","type":"transform","rotation":"quat","channels":{
          "rotation.x":{"keys":[[0,0,"s"]]},"rotation.y":{"keys":[[0,0.7071067811865476,"s"]]},
          "rotation.z":{"keys":[[0,0,"s"]]},"rotation.w":{"keys":[[0,0.7071067811865476,"s"]]},
          "position.x":{"keys":[[0,5,"s"]]}}},
        {"id":"t_bad","type":"property","path":"MeshRenderer.roughness","channels":{"value":{"keys":[[0,1,"s"]]}}},
        {"id":"t_bad2","type":"property","path":"PointLight.nothing","channels":{"value":{"keys":[[0,1,"s"]]}}}]}]})JSON";
    DocPtr d = r.Doc("P", text);
    CHECK(d != nullptr);
    std::string err;
    CHECK(r.host.EditorScrub(d, 0, err));
    const Transform& t = reg.get<Transform>(kid);
    CHECK_NEAR(t.position.x, 5.0, 1e-6);                 // ローカル座標に書く(親の 100 は足されない)
    CHECK_NEAR(t.rotation.y, 90.0, 1e-3);                // クォータニオン → Euler(度)
    CHECK(!t.useQuaternion);
    CHECK_NEAR(reg.get<Transform>(parent).position.x, 100.0, 1e-6);   // 親は無変更
    // 未対応・不正なプロパティは警告になり、書かれない(落ちない)
    const EditorStatus es = r.host.GetEditorStatus();
    int warns = 0;
    for (const std::string& w : es.warnings) if (w.find("MeshRenderer") != std::string::npos || w.find("nothing") != std::string::npos) ++warns;
    CHECK(warns >= 2);
    r.host.EditorEnd();
    CHECK_NEAR(reg.get<Transform>(kid).position.x, 1.0, 1e-6);
}

// ===========================================================================
void TestCameraCutsAndPost()
{
    std::printf("[3] カメラ: カット・FOV・DoF・フォーカス・シェイク\n");
    Rig r;
    auto& reg = r.w.Reg();
    DocPtr d = r.Doc();
    std::string err;

    CHECK(r.host.CutCameraEntity() == entt::null);       // セッション前は何も選ばない
    // cam2 が isActive のシーンで、カットは 0..2 秒が cam1 / 2..4 秒が cam2
    CHECK(r.host.EditorScrub(d, 3000, err));
    CHECK(r.host.CutCameraEntity() == r.w.cam1);
    CHECK(r.host.EditorScrub(d, 15000, err));
    CHECK(r.host.CutCameraEntity() == r.w.cam2);
    CHECK(r.host.EditorScrub(d, 11999, err));
    CHECK(r.host.CutCameraEntity() == r.w.cam1);
    CHECK(r.host.EditorScrub(d, 12000, err));            // 境界は半開区間: 次のカット
    CHECK(r.host.CutCameraEntity() == r.w.cam2);
    CHECK(!reg.get<CameraComponent>(r.w.cam1).isActive);   // isActive は書き換えない
    CHECK(reg.get<CameraComponent>(r.w.cam2).isActive);

    // ポスト: 描画時のコピーへの上書き。シーンの設定は無変更
    r.host.EditorScrub(d, 6000, err);
    const PostProcessSettings before = r.w.scene.GetPostSettings();
    PostProcessSettings pp = r.w.scene.GetPostSettings();
    CHECK(!pp.bloomOn);
    CHECK(r.host.ApplyPostOverrides(pp, /*cameraView=*/true));
    CHECK(pp.bloomOn);                                    // 値を書いたら XxxOn が自動で ON
    CHECK_NEAR(pp.bloom, 0.5, 1e-5);                      // 0.2 → 0.8 の中点
    CHECK(std::memcmp(&before.bloom, &r.w.scene.GetPostSettings().bloom, sizeof(float)) == 0);
    CHECK(!r.w.scene.GetPostSettings().bloomOn);          // シーンの設定は書き換わっていない
    // DoF: カットで選ばれた cam1 の dofAperture / focus(Box まで)。カットが cam1 の間だけ効く
    CHECK(pp.dofOn);
    CHECK_NEAR(pp.dofAperture, 1.5, 1e-6);
    {
        // cam1 (0,5,-10) → Box(位置は scrub 後の値)。Z 軸前方 +Z のビュー距離 = 対象の z − カメラの z
        const auto& bt = reg.get<Transform>(r.w.box);
        const double expectZ = bt.position.z - (-10.0);
        CHECK_NEAR(pp.dofFocusDist, expectZ, 0.05);
    }
    PostProcessSettings freeView = r.w.scene.GetPostSettings();
    r.host.ApplyPostOverrides(freeView, /*cameraView=*/false);   // エディタの自由カメラのビュー: DoF は掛けない
    CHECK(freeView.bloomOn && !freeView.dofOn);
    // cam2 のカットに移ると cam1 の DoF は効かない
    r.host.EditorScrub(d, 15000, err);
    PostProcessSettings pp2 = r.w.scene.GetPostSettings();
    r.host.ApplyPostOverrides(pp2, true);
    CHECK(!pp2.dofOn);
    r.host.EditorEnd();
    PostProcessSettings pp3 = r.w.scene.GetPostSettings();
    CHECK(!r.host.ApplyPostOverrides(pp3, true));         // 終わったら上書きは無い
    CHECK(r.host.CutCameraEntity() == entt::null);

    // シェイク: cam1 に 0..1 秒、決定論(同じ t は同じ値)
    r.host.EditorScrub(d, 1500, err);
    float x1 = 0, y1 = 0, z1 = 0, x2 = 0, y2 = 0, z2 = 0;
    CHECK(r.host.ShakeFor(r.w.cam1, x1, y1, z1));
    CHECK(x1 != 0.0f || y1 != 0.0f || z1 != 0.0f);
    CHECK(!r.host.ShakeFor(r.w.cam2, x2, y2, z2));
    r.host.EditorScrub(d, 20000, err);
    r.host.EditorScrub(d, 1500, err);
    r.host.ShakeFor(r.w.cam1, x2, y2, z2);
    CHECK(x1 == x2 && y1 == y2 && z1 == z2);
    r.host.EditorScrub(d, 12000, err);                    // クリップの外
    CHECK(!r.host.ShakeFor(r.w.cam1, x2, y2, z2));
    // Transform は書き換えない(シェイクは描画側の加算)
    CHECK_NEAR(reg.get<Transform>(r.w.cam1).position.x, 0.0, 1e-9);
    r.host.EditorEnd();
}

// ===========================================================================
void TestPreAnimated()
{
    std::printf("[4] PreAnimatedState: 保存への混入ゼロ・復元タイミング\n");
    std::string err;
    {
        Rig r;
        auto& reg = r.w.Reg();
        DocPtr d = r.Doc();
        const std::string base = SaveJson(r.w.scene);            // 保存(guid は全員に付いている)
        CHECK(SaveJson(r.w.scene) == base);                       // 前提: 同じシーンは同じ JSON

        CHECK(r.host.EditorScrub(d, 9000, err));
        CHECK(r.host.PreAnimatedCount() > 0);
        CHECK_NEAR(reg.get<Transform>(r.w.box).position.x, 7.5, 1e-4);   // 確かにスクラブ値が入っている
        // (d) 保存の直前に戻る: SaveToString
        const std::string during = SaveJson(r.w.scene);
        CHECK(during == base);                                    // ★スクラブ中の保存はスクラブ前の保存とバイト一致
        CHECK(during.find("7.5") == std::string::npos || base.find("7.5") != std::string::npos);
        // 保存の後は(フレーム末で)再適用される = ちらつかない
        r.host.PostUpdate();
        CHECK_NEAR(reg.get<Transform>(r.w.box).position.x, 7.5, 1e-4);
        // Save(ファイル)も同じ
        {
            const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "dx12_seqbind_scene.json";
            CHECK(SceneSerializer::Save(r.w.scene, tmp.string(), ""));
            std::ifstream f(tmp, std::ios::binary);
            std::stringstream ss; ss << f.rdbuf();
            const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
            bool ok = j.is_object() && j.contains("entities");
            double boxX = -1;
            if (ok)
                for (const auto& e : j["entities"])
                    if (e.value("name", "") == "Box") boxX = e["transform"]["position"][0].get<double>();
            CHECK(ok && std::fabs(boxX - 1.0) < 1e-9);            // ファイルの Box.x は元の 1(スクラブの 7.5 ではない)
            std::error_code ec; std::filesystem::remove(tmp, ec);
        }
        r.host.PostUpdate();
        // SerializeEntity / SerializeSubtree(Undo・複製・プレハブ)も元の値
        {
            const nlohmann::json ej = nlohmann::json::parse(SceneSerializer::SerializeEntity(r.w.scene, r.w.box, ""), nullptr, false);
            CHECK(ej.is_object() && std::fabs(ej["transform"]["position"][0].get<double>() - 1.0) < 1e-9);
        }
        r.host.PostUpdate();
        // ライトの色 / 強度(プロパティ)も戻る
        {
            SaveJson(r.w.scene);
            CHECK_NEAR(reg.get<PointLight>(r.w.lamp).intensity, 2.0, 1e-6);    // 保存直後(再適用前)は元の値
            r.host.PostUpdate();
            CHECK_NEAR(reg.get<PointLight>(r.w.lamp).intensity, 1.0 + 4.0 * 9000.0 / 12000.0, 1e-4);
        }
        // (a) スクラブ終了 → 元へ戻る(ビット一致)
        r.host.EditorEnd();
        CHECK(r.host.PreAnimatedCount() == 0);
        CHECK(!r.host.EditorActive());
        CHECK(SaveJson(r.w.scene) == base);
        Transform orig; orig.position = { 1, 2, 3 };
        CHECK(SameTransform(reg.get<Transform>(r.w.box), orig));
        CHECK_NEAR(reg.get<PointLight>(r.w.lamp).intensity, 2.0, 1e-9);
        CHECK(reg.get<PointLight>(r.w.lamp).castShadows == PointLight{}.castShadows);
        CHECK_NEAR(reg.get<CameraComponent>(r.w.cam1).fovDegrees, 60.0, 1e-9);
    }
    {   // (b) 文書を閉じる
        Rig r;
        DocPtr d = r.Doc();
        const std::string base = SaveJson(r.w.scene);
        r.host.EditorScrub(d, 6000, err);
        CHECK(r.host.CloseDoc("T", err));
        CHECK(!r.host.EditorActive() && r.host.PreAnimatedCount() == 0);
        CHECK(SaveJson(r.w.scene) == base);
        CHECK_NEAR(r.w.Reg().get<Transform>(r.w.box).position.x, 1.0, 1e-9);
    }
    {   // (c) Play 開始: EditorEnd(EnterPlayMode が呼ぶ)/ フレーム更新のモード遷移(保険)
        Rig r;
        DocPtr d = r.Doc();
        r.host.EditorScrub(d, 6000, err);
        r.host.EditorEnd();
        CHECK_NEAR(r.w.Reg().get<Transform>(r.w.box).position.x, 1.0, 1e-9);
        r.host.EditorScrub(d, 6000, err);
        r.PlayFrame(0.0f);                                       // モードが Playing に変わった最初のフレーム
        CHECK(!r.host.EditorActive() && r.host.PreAnimatedCount() == 0);
        CHECK_NEAR(r.w.Reg().get<Transform>(r.w.box).position.x, 1.0, 1e-9);
    }
    {   // Play のスナップショット(SaveToString)にも混ざらない(EnterPlayMode の SaveToString と同じ経路)
        Rig r;
        DocPtr d = r.Doc();
        const std::string base = SaveJson(r.w.scene);
        r.host.EditorScrub(d, 12000, err);
        const std::string snap = SaveJson(r.w.scene);
        CHECK(snap == base);
    }
    {   // エンティティ削除中のスクラブ
        Rig r;
        auto& reg = r.w.Reg();
        DocPtr d = r.Doc();
        r.host.EditorScrub(d, 6000, err);
        const std::uint64_t boxGuid = reg.get<EntityGuid>(r.w.box).value;
        reg.destroy(r.w.box);                                    // スクラブ中に対象を消す
        r.EditorFrame();                                         // 落ちない・再束縛される
        CHECK(r.host.EditorScrub(d, 9000, err));
        const std::string during = SaveJson(r.w.scene);          // Box の居ないシーンを保存しても落ちない
        CHECK(during.find("\"Box\"") == std::string::npos);
        // Undo で作り直される(同じ guid・スクラブ値が焼かれた状態)→ 保存すると元値に戻り、次のフレームで再び制御される
        const entt::entity again = Make(r.w.scene, "Box", { 7.5f, 4.0f, 3.0f });
        reg.emplace<EntityGuid>(again, EntityGuid{ boxGuid });
        r.EditorFrame();
        CHECK_NEAR(reg.get<Transform>(again).position.x, 7.5, 1e-3);   // 再束縛されて制御下に戻る
        r.host.EditorEnd();
        CHECK_NEAR(reg.get<Transform>(again).position.x, 1.0, 1e-9);   // 元値(退避していた 1)へ戻る
        CHECK_NEAR(reg.get<PointLight>(r.w.lamp).intensity, 2.0, 1e-9);
    }
    {   // 別の Scene の保存には触らない
        Rig r;
        DocPtr d = r.Doc();
        r.host.EditorScrub(d, 9000, err);
        Scene other;
        Make(other, "Other");
        (void)SaveJson(other);
        CHECK(r.host.PreAnimatedCount() > 0);
        CHECK_NEAR(r.w.Reg().get<Transform>(r.w.box).position.x, 7.5, 1e-4);
        r.host.EditorEnd();
    }
    {   // シーンが作り直された(open_scene)→ 退避は捨てるだけ。新シーンの値は触らない
        Rig r;
        DocPtr d = r.Doc();
        r.host.EditorScrub(d, 9000, err);
        r.EditorFrame(1);
        r.EditorFrame(2);                                        // トークンが変わった
        CHECK(!r.host.EditorActive() && r.host.PreAnimatedCount() == 0);
    }
    {   // ユーザーが制御中の Transform を触った → 検出される。それでも保存は元値
        Rig r;
        auto& reg = r.w.Reg();
        DocPtr d = r.Doc();
        const std::string base = SaveJson(r.w.scene);
        r.host.EditorScrub(d, 6000, err);
        reg.get<Transform>(r.w.box).position.x += 3.0f;          // 人がギズモで動かした
        r.host.EditorScrub(d, 6000, err);
        CHECK(r.host.GetEditorStatus().userDrift >= 1);
        CHECK(SaveJson(r.w.scene) == base);                      // 保存への混入ゼロを優先
        r.host.EditorEnd();
    }
    {   // ホストが消えたら保存フックも外れる(ダングリングしない)
        Scene s;
        Make(s, "A");
        {
            SequencerHost h;
            h.SetScene(&s);
            h.InstallSaveHook();
        }
        CHECK(!SaveJson(s).empty());
    }
}

// ===========================================================================
void TestPlay()
{
    std::printf("[5] Play: 時計・イベント・timeScale・物理\n");
    std::string err;
    {   // 前進で 1 回 / t=0 を落とさない / 実時間の時計
        Rig r;
        const char* text = R"JSON({"format":"dxseq","version":1,"name":"E","ticksPerSecond":6000,"frameRate":30,
          "range":[0,12000],
          "bindings":[{"id":"b_s","name":"Scene","kind":"scene","tracks":[{"id":"t_ev","type":"event","events":[
            {"id":"e0","t":0,"kind":"emit","name":"start"},
            {"id":"e1","t":6000,"kind":"emit","name":"mid","data":{"value":5}},
            {"id":"e2","t":12000,"kind":"emit","name":"end"}]}]}]})JSON";
        DocPtr d = r.Doc("E", text);
        SequencePlayOptions o;
        r.PlayFrame(0.0f);                                       // Playing へ遷移
        r.timeScale = 0.25f;                                     // スローモでも
        const int id = r.host.PlayRuntime(d, o, err);
        CHECK(id > 0);
        r.PlayFrame(0.5f);
        CHECK(r.emitted.size() == 1 && r.emitted[0] == "start");            // t=0 のイベントは最初の 1 歩で発火
        r.PlayFrame(0.5f);                                       // 実時間: 0.5 + 0.5 = 1.0 秒経過(スケール 0.25 でも 1 秒)
        CHECK(std::fabs(r.host.LuaTime("E") - 1.0) < 1e-6);
        CHECK(r.emitted.size() == 2 && r.emitted[1] == "mid=5");               // 1 秒(6000)ちょうどで 1 回(右閉)
        r.PlayFrame(0.5f);
        CHECK(r.emitted.size() == 2);
        r.PlayFrame(0.0f);
        r.PlayFrame(0.0f);
        CHECK(r.emitted.size() == 2);                            // dt=0 では再発火しない
        r.PlayFrame(1.0f);
        CHECK(r.emitted.size() == 4 && r.emitted[2] == "end" && r.emitted[3] == "E:done=1");   // 終端 + :done(旧 sequence_author と同名)
        CHECK(!r.host.LuaIsPlaying("E"));                        // 終わったら消える
    }
    {   // スクラブ(Seek)では発火しない / 一時停止 / ゲーム時間クロック
        Rig r;
        const char* text = R"JSON({"format":"dxseq","version":1,"name":"S","ticksPerSecond":6000,"frameRate":30,
          "range":[0,60000],
          "bindings":[{"id":"b_s","name":"Scene","kind":"scene","tracks":[{"id":"t_ev","type":"event","events":[
            {"id":"e1","t":12000,"kind":"emit","name":"a"},{"id":"e2","t":30000,"kind":"emit","name":"b"}]}]}]})JSON";
        DocPtr d = r.Doc("S", text);
        r.PlayFrame(0.0f);
        SequencePlayOptions o;
        o.clockGame = true;
        r.timeScale = 0.5f;
        r.host.PlayRuntime(d, o, err);
        r.PlayFrame(2.0f);                                       // ゲーム時間: 2 × 0.5 = 1 秒
        CHECK(std::fabs(r.host.LuaTime("S") - 1.0) < 1e-6);
        CHECK(r.emitted.empty());
        CHECK(r.host.SeekRuntime("S", 6.0));                     // 12000 を飛び越えて 6 秒へ
        r.PlayFrame(0.0f);
        CHECK(r.emitted.empty());                                // ★スクラブでは発火しない
        CHECK(r.host.PauseRuntime("S", true));
        r.PlayFrame(10.0f);
        CHECK(std::fabs(r.host.LuaTime("S") - 6.0) < 1e-6);      // 一時停止中は進まない
        r.host.PauseRuntime("S", false);
        r.timeScale = 1.0f;
        r.PlayFrame(3.0f);                                       // 6 → 9 秒(2 秒・5 秒のイベントは既に通過済み → 鳴らない)
        CHECK(r.emitted.empty());
        CHECK(r.host.StopRuntime("S"));
        CHECK(!r.host.LuaIsPlaying("S"));
    }
    {   // ループ: 周回ごとに 1 回ずつ(終端は鳴らさない)/ lua イベント
        Rig r;
        const char* text = R"JSON({"format":"dxseq","version":1,"name":"L","ticksPerSecond":6000,"frameRate":30,
          "range":[0,12000],
          "bindings":[{"id":"b_s","name":"Scene","kind":"scene","tracks":[{"id":"t_ev","type":"event","events":[
            {"id":"e1","t":3000,"kind":"lua","name":"Hit","args":[1,"a"]}]}]}]})JSON";
        DocPtr d = r.Doc("L", text);
        r.PlayFrame(0.0f);
        SequencePlayOptions o;
        o.loop = 1;
        r.host.PlayRuntime(d, o, err);
        for (int i = 0; i < 12; ++i) r.PlayFrame(0.5f);          // 6 秒 = 3 周
        CHECK(r.luaCalls.size() == 3);
        CHECK(!r.luaCalls.empty() && r.luaCalls[0] == "Hit/2");
        CHECK(r.host.LuaIsPlaying("L"));
    }
    {   // エディタのプレビュー再生ではイベントを発火しない
        Rig r;
        DocPtr d = r.Doc();
        CHECK(r.host.EditorPlay(d, false, 1.0, err));
        for (int i = 0; i < 20; ++i) r.host.Update(0.25f, HostMode::Editor, false, 1);
        CHECK(r.emitted.empty() && r.luaCalls.empty());
        CHECK(!r.host.GetEditorStatus().playing);                // 4 秒で終わった
        r.host.EditorEnd();
    }
    {   // timeScale トラック: 書く / 終了で元へ戻す / 強制的に実時間
        Rig r;
        const char* text = R"JSON({"format":"dxseq","version":1,"name":"Z","ticksPerSecond":6000,"frameRate":30,
          "range":[0,12000],
          "bindings":[{"id":"b_s","name":"Scene","kind":"scene","tracks":[{"id":"t_ts","type":"timeScale","channels":{
            "value":{"keys":[[0,0.25,"s"],[6000,1,"s"]]}}}]}]})JSON";
        DocPtr d = r.Doc("Z", text);
        r.PlayFrame(0.0f);
        r.timeScale = 1.0f;
        SequencePlayOptions o;
        o.clockGame = true;                                       // 指定しても timeScale トラックがあれば実時間へ強制
        r.host.PlayRuntime(d, o, err);
        r.PlayFrame(0.5f);
        CHECK_NEAR(r.timeScale, 0.25, 1e-6);
        CHECK(std::fabs(r.host.LuaTime("Z") - 0.5) < 1e-6);       // 実時間で進んでいる(自分が書いたスケールに影響されない)
        r.PlayFrame(1.0f);
        CHECK_NEAR(r.timeScale, 1.0, 1e-6);
        r.PlayFrame(1.0f);                                        // 終了
        CHECK_NEAR(r.timeScale, 1.0, 1e-6);                       // 元(1.0)へ戻った
        CHECK(!r.host.AnyPlaying());
    }
    {   // 物理との競合規則
        Rig r;
        auto& reg = r.w.Reg();
        DocPtr d = r.Doc();
        RigidBody rb;
        rb.motionType = MotionType::Dynamic;
        rb.bodyId = 5;                                            // 物理に登録済み(Play 中)
        reg.emplace<RigidBody>(r.w.box, rb);
        // エディタ(物理は動かない)では動的な剛体でも書ける(戻す)
        CHECK(r.host.EditorScrub(d, 6000, err));
        CHECK_NEAR(reg.get<Transform>(r.w.box).position.x, 5.0, 1e-4);
        r.host.EditorEnd();
        CHECK_NEAR(reg.get<Transform>(r.w.box).position.x, 1.0, 1e-9);
        // Play では動的な剛体の Transform を書かない(物理が上書きする)
        r.PlayFrame(0.0f);
        SequencePlayOptions o;
        r.host.PlayRuntime(d, o, err);
        r.PlayFrame(1.0f);
        CHECK_NEAR(reg.get<Transform>(r.w.box).position.x, 1.0, 1e-9);
        CHECK(r.host.LastOutput().skippedPhysics >= 1);
        // キネマティックなら書く(物理が MoveKinematic で追従する)
        reg.get<RigidBody>(r.w.box).motionType = MotionType::Kinematic;
        r.PlayFrame(0.0f);
        CHECK_NEAR(reg.get<Transform>(r.w.box).position.x, 10.0 * 1.0 / 2.0, 0.6);
        // Play の再生は元値を退避しない(演出の結果を残す)
        CHECK(r.host.PreAnimatedCount() == 0);
    }
    {   // Play でのカット: 再生中は最後のカットのカメラが選ばれ、終わったら戻る
        Rig r;
        DocPtr d = r.Doc();
        r.PlayFrame(0.0f);
        SequencePlayOptions o;
        r.host.PlayRuntime(d, o, err);
        r.PlayFrame(0.5f);
        CHECK(r.host.CutCameraEntity() == r.w.cam1);
        r.PlayFrame(2.0f);
        CHECK(r.host.CutCameraEntity() == r.w.cam2);
        r.PlayFrame(2.0f);                                        // 終了(restoreOnEnd)
        CHECK(r.host.CutCameraEntity() == entt::null);
    }
}

// ===========================================================================
void TestDocuments()
{
    std::printf("[6] 文書: ops JSON / undo / dryRun / 保存 / 自動再生\n");
    Rig r;
    auto& reg = r.w.Reg();
    std::string err;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dx12_seqbind_assets";
    std::error_code ec;
    fs::remove_all(dir, ec);
    r.host.SetAssetsDirOverride(dir.string());

    CHECK(SequencerHost::RelPathFor("Intro") == "sequences/Intro.dxseq");
    CHECK(SequencerHost::RelPathFor("sequences/a/Intro.dxseq") == "sequences/a/Intro.dxseq");
    CHECK(SequencerHost::KeyFor("sequences/Intro.dxseq") == "Intro");
    CHECK(SequencerHost::KeyFor("Intro") == "Intro");

    DocPtr d = r.host.NewDoc("Intro", 30, err);
    CHECK(d != nullptr && d->dirty && !d->onDisk);
    const std::string empty = seq::SerializeSequence(d->seq);

    // addBinding は entity 名から hint(guid / path / name)を作る。ID は省略できる。時刻は秒でも書ける
    const char* ops = R"JSON({"label":"build","ops":[
      {"op":"addBinding","binding":{"id":"b_box","entity":"Box","tracks":[
        {"type":"transform","channels":{"position.x":{"keys":[{"sec":0,"v":0,"ip":"e:outQuad"},{"sec":2,"v":10,"ip":"l"}]}}}]}},
      {"op":"add_cut","cut":{"camera":"Box","startSec":0,"endSec":2}}]})JSON";
    // cut の camera はバインディングの id(名前でも書ける: 1 つに決まれば id へ直る)
    seqhost::EditResult er = r.host.ApplyOps(d, ops, /*dryRun=*/true);
    CHECK(er.ok && er.applied == 2);
    CHECK(d->seq.bindings.empty() && seq::SerializeSequence(d->seq) == empty);            // dryRun は何も変えない
    er = r.host.ApplyOps(d, ops, false);
    CHECK(er.ok);
    CHECK(d->seq.bindings.size() == 1 && d->seq.cuts.size() == 1);
    CHECK(d->seq.bindings[0].hint.name == "Box" && !d->seq.bindings[0].hint.guid.empty());
    CHECK(d->seq.bindings[0].tracks.size() == 1 && !d->seq.bindings[0].tracks[0].id.empty());       // ID が発行された
    CHECK(d->seq.cuts[0].camera == "b_box");
    CHECK(d->seq.cuts[0].end == 2 * kSec);
    CHECK(d->dirty && d->history.CanUndo());
    // 評価: 2 秒の outQuad の終端 = 10、1 秒 = 10 * (1 - (1-0.5)^2) = 7.5
    seqhost::EvalReport rep;
    CHECK(r.host.EvalAt(d, kSec, rep, err));
    CHECK(rep.res.channels.size() == 1 && std::fabs(rep.res.channels[0].value - 7.5) < 1e-9);
    // sequence_eval 相当は非破壊: 何も書かず、退避も作らず、guid も新しく確定しない
    CHECK_NEAR(reg.get<Transform>(r.w.box).position.x, 1.0, 1e-9);
    CHECK(r.host.PreAnimatedCount() == 0 && !r.host.EditorActive());

    // 不正な op は全体を巻き戻し、文書は無変更(位置つきのエラー)
    const std::string beforeBad = seq::SerializeSequence(d->seq);
    const std::uint64_t revBad = d->revision;
    er = r.host.ApplyOps(d, R"JSON([{"op":"addKey","trackId":"t_none","channel":"position.x","key":[0,1,"a"]}])JSON", false);
    CHECK(!er.ok && er.error.find("ops") != std::string::npos || !er.ok);
    er = r.host.ApplyOps(d, R"JSON([{"op":"noSuchOp"}])JSON", false);
    CHECK(!er.ok && er.error.find("noSuchOp") != std::string::npos);
    CHECK(seq::SerializeSequence(d->seq) == beforeBad && d->revision == revBad);

    // Undo / Redo
    CHECK(r.host.Undo(d).ok);
    CHECK(d->seq.bindings.empty() && d->seq.cuts.empty());
    CHECK(seq::SerializeSequence(d->seq) == empty);
    CHECK(r.host.Redo(d).ok);
    CHECK(d->seq.bindings.size() == 1);

    // 保存(アトミック・正準形)→ 別のホストで読み直して一致
    CHECK(r.host.SaveDoc(d, err));
    CHECK(!d->dirty && d->onDisk);
    CHECK(fs::exists(dir / "sequences" / "Intro.dxseq"));
    {
        SequencerHost h2;
        h2.SetAssetsDirOverride(dir.string());
        DocPtr d2 = h2.LoadDoc("Intro", err);
        CHECK(d2 != nullptr && seq::SerializeSequence(d2->seq) == seq::SerializeSequence(d->seq));
        const std::vector<std::string> list = h2.ListAssets();
        CHECK(list.size() == 1 && list[0] == "sequences/Intro.dxseq");
        CHECK(h2.LoadDoc("Missing", err) == nullptr && !err.empty());
    }
    // 保存前後で編集すれば dirty、元に戻せば dirty でなくなる
    CHECK(r.host.Undo(d).ok && d->dirty);
    CHECK(r.host.Redo(d).ok && !d->dirty);

    // シーンの自動再生設定: Play 開始で再生。空なら JSON に何も足さない
    const std::string baseNoPlayers = SaveJson(r.w.scene);
    CHECK(baseNoPlayers.find("sequencePlayers") == std::string::npos);
    SequenceAutoPlay sp;
    sp.sequence = "Intro";
    sp.loop = true;
    sp.rate = 2.0f;
    r.w.scene.GetSequenceAutoPlay().push_back(sp);
    const std::string withPlayers = SaveJson(r.w.scene);
    CHECK(withPlayers.find("sequencePlayers") != std::string::npos);
    {
        Scene loaded;
        CHECK(SceneSerializer::LoadFromString(loaded, withPlayers, ""));
        CHECK(loaded.GetSequenceAutoPlay().size() == 1 && loaded.GetSequenceAutoPlay()[0].sequence == "Intro" &&
              loaded.GetSequenceAutoPlay()[0].loop && loaded.GetSequenceAutoPlay()[0].rate == 2.0f);
    }
    r.PlayFrame(0.0f);                                            // Play 開始 → 自動再生
    CHECK(r.host.LuaIsPlaying("Intro"));
    const auto players = r.host.Players();
    CHECK(players.size() == 1 && players[0].loop && players[0].rate == 2.0);
    r.host.StopRuntime("*");

    // 文書を閉じたら再生も止まる
    r.host.PlayRuntime(d, SequencePlayOptions{}, err);
    CHECK(r.host.CloseDoc("Intro", err));
    CHECK(!r.host.AnyPlaying());
    fs::remove_all(dir, ec);
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // クラッシュしても途中の出力が残るように
    TestBinding();
    TestApply();
    TestTransformDetails();
    TestCameraCutsAndPost();
    TestPreAnimated();
    TestPlay();
    TestDocuments();
    if (g_failures != 0)
    {
        std::printf("\nsequencer_bind: %d チェック中 %d 件 NG\n", g_checks, g_failures);
        return 1;
    }
    std::printf("\nsequencer_bind: %d チェックすべて通過\n", g_checks);
    return 0;
}
