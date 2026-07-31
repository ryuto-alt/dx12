#pragma once

#include <string>
#include <vector>
#include <entt/entt.hpp>

namespace dx12e
{

class Scene;

class SceneSerializer
{
public:
    static bool Save(const Scene& scene, const std::string& filePath,
                     const std::string& assetsDir);
    static bool Load(Scene& scene, const std::string& filePath,
                     const std::string& assetsDir);
    // 既存エンティティの Transform / Material を JSON から上書き（エンティティは作らない）
    static bool ApplyOverrides(Scene& scene, const std::string& filePath,
                               const std::string& assetsDir);

    // in-memory snapshot 用（Play→Stop 時のシーン復元に使用）
    static std::string SaveToString(const Scene& scene, const std::string& assetsDir);
    static bool LoadFromString(Scene& scene, const std::string& jsonStr,
                               const std::string& assetsDir);

    // 単一エンティティの JSON 化（クリップボード/複製用。parent は含まない）
    static std::string SerializeEntity(const Scene& scene, entt::entity e,
                                       const std::string& assetsDir);
    // JSON からエンティティを既存シーンに追加生成（Clear しない）
    // 失敗時は entt::null を返す
    // keepGuid=true は「同じエンティティを作り直す」経路（Undo の復元 / Redo）専用。
    // ★false のままだと復元されたエンティティは EntityGuid を持たず、次の保存で新しい guid が
    //   振られる。そのエンティティを guid で指していた参照（Trigger の相手、Lua の entity
    //   プロパティ、NetworkIdentity）は全部ぶら下がりになり、名前一致へ静かに格下げされる
    //   （同名が居ると誤爆する）。複製/貼り付けは guid を振り直すのが正しいので既定は false。
    static entt::entity InstantiateEntity(Scene& scene, const std::string& jsonStr,
                                          const std::string& assetsDir,
                                          bool keepGuid = false);
    // ── アセット参照（assets 相対パス）の付け替え / 数え上げ ──
    //
    // アセットを移動・削除しても参照は追従しない、というのが長らくの状態だった。
    // 切れても実行時は「音が鳴らない」「UI が真っ白」と出るだけでエラーが無いので、
    // 気付かないまま壊れたシーンを保存できてしまう。
    //
    // ★対象は「いま開いているシーン」だけ。他のシーンファイルや .prefab の中は触らない
    //   （それらを直すにはアセット GUID が要る）。呼び出し側はその前提で結果を伝えること。
    //
    // oldRel がディレクトリの場合も想定して、前方一致 + 区切り境界で判定する
    // （"tex" が "textures/a.png" に誤爆しないように）。
    // 戻り値は書き換えた参照の数。
    static int RewriteAssetPathRefs(Scene& scene, const std::string& oldRel,
                                    const std::string& newRel);
    // rel を参照しているフィールドの数を数える（削除前の警告用）。
    // outWho に「エンティティ名: 種別」を最大 maxNames 件だけ積む。
    static int CountAssetPathRefs(const Scene& scene, const std::string& rel,
                                  std::vector<std::string>* outWho = nullptr,
                                  int maxNames = 8);

    // ── ディスク上のファイル内のアセット参照を一括で付け替える ──
    //
    // 上の RewriteAssetPathRefs は「いま開いているシーンのメモリ上の値」しか直さない。
    // 実際にはアセット参照は他のシーン・.prefab・.dxmat・.animfsm・.spranim・
    // .terrainlayers・.uianim にも入っていて、そちらは移動すると切れたままになる。
    //
    // どれも JSON なので、パースして**全文字列値**を走査し、oldRel と一致する
    // （またはディレクトリとして配下にある）ものだけを置き換える。
    // キー名を 30 箇所以上列挙する方式にしないのは、フィールドが増えるたびに
    // ここを更新し忘れて静かに漏れるため（実際 sceneWrite.ts の表がそれで腐っていた）。
    //
    // skipAbsPath は「メモリ側で処理済みなので触らないファイル」（＝開いているシーン）。
    struct AssetRefFileRewrite
    {
        int filesChanged = 0;
        int refsChanged  = 0;
        std::vector<std::string> files;   // assets 相対。最大 kMaxReportedFiles 件
        std::vector<std::string> failed;  // 読めなかった/書けなかったファイル
    };
    static constexpr int kMaxReportedFiles = 32;
    static AssetRefFileRewrite RewriteAssetPathRefsInFiles(const std::string& assetsDir,
                                                           const std::string& oldRel,
                                                           const std::string& newRel,
                                                           const std::string& skipAbsPath = {});

    // エンティティを全コンポーネント込みで複製（名前は重複しないよう連番付与）
    static entt::entity DuplicateEntity(Scene& scene, entt::entity src,
                                        const std::string& assetsDir);

    // リストから「先祖も含まれているエンティティ」を除いた最上位だけを返す。
    // サブツリー複製と併用しないと、親子両方を選択してコピーしたとき子が二重化する。
    static std::vector<entt::entity> TopmostRoots(const Scene& scene,
                                                  const std::vector<entt::entity>& entities);

    // サブツリー（src + 全子孫）を全コンポーネント込みで複製。root の親は元と同じ。
    // outAll に生成した全エンティティ（root 先頭）を返す（Undo 用）。
    static entt::entity DuplicateSubtree(Scene& scene, entt::entity src,
                                         const std::string& assetsDir,
                                         std::vector<entt::entity>* outAll = nullptr);

    // エンティティのモデル(modelPath)だけを差し替える。全コンポーネント・親子関係・
    // 名前を維持したまま JSON 経由で再生成するため entt::entity ID は変わる。
    // 失敗時（モデルロード失敗等）は entt::null を返し、元エンティティは無傷。
    // 注意: モデルロードを伴うため cmdList が有効なフレーム境界で呼ぶこと。
    static entt::entity SwapEntityModel(Scene& scene, entt::entity e,
                                        const std::string& newModelPath,
                                        const std::string& assetsDir);

    // --- Prefab / サブツリー（再利用テンプレート）---
    // root + 全子孫を 1 つの自己完結 JSON（parent はローカル index 参照）に直列化
    static std::string SerializeSubtree(const Scene& scene, entt::entity root,
                                        const std::string& assetsDir);
    // サブツリー JSON を既存シーンへ展開（Clear しない）。戻り値 = root。
    // outAll に生成した全エンティティ（root 先頭）を返す。
    // keepGuids=false（既定）… 展開は「別のエンティティ」を作る。複製 / 貼り付け /
    //   プレハブの新規配置がこれ。guid は捨てて次の保存で新しい値を振らせる。
    // keepGuids=true … **同じエンティティ**を作り直す。Undo の復元と、プレハブ適用の
    //   伝播（消してから作り直す）がこれ。guid を引き継がないと、参照が guid を向いた
    //   瞬間に Undo やプレハブ適用のたびに黙って参照が切れる。
    static entt::entity InstantiateSubtree(Scene& scene, const std::string& jsonStr,
                                           const std::string& assetsDir,
                                           std::vector<entt::entity>* outAll = nullptr,
                                           bool keepGuids = false);
    // root + 子孫を .prefab ファイルへ保存
    static bool SavePrefab(const Scene& scene, entt::entity root,
                           const std::string& filePath, const std::string& assetsDir);
    // .prefab ファイルを既存シーンへ生成。戻り値 = root（失敗時 entt::null）。
    // 生成した root には PrefabLink（元 .prefab への紐付け）が自動で付く＝以後 Apply/Revert 可能。
    static entt::entity InstantiatePrefab(Scene& scene, const std::string& filePath,
                                          const std::string& assetsDir,
                                          std::vector<entt::entity>* outAll = nullptr);

    // --- プレハブのリンク維持（Unity の Apply / Revert / オーバーライド表示 相当）---

    // インスタンスと元 .prefab の差分1件。
    struct PrefabOverride
    {
        int         entityIndex = 0;   // サブツリー BFS 順のインデックス（0 = ルート）
        std::string entityName;
        std::string component;         // JSON キー（例 "uiRect"）。構造差なら "(構成)"
        std::string field;             // フィールド名。コンポーネント丸ごとの増減なら "(追加)"/"(削除)"
    };

    // root（PrefabLink 必須）と元 .prefab を比べて変更点を列挙する。
    // 名前は展開時に連番が付くので比較対象から外す（毎回全インスタンスが差分扱いになるため）。
    // PrefabLink 自身も比較しない。プレハブが読めない場合は false。
    static bool ComputePrefabOverrides(const Scene& scene, entt::entity root,
                                       const std::string& assetsDir,
                                       std::vector<PrefabOverride>& out);

    // インスタンスの現在の姿を元 .prefab へ書き戻し（Apply）、同じ .prefab から作った
    // 他のインスタンスへも配る。配るときは 3-way マージなので、各インスタンスの手直し
    // （置いた位置、差し替えたマテリアル、手で足した子）は残る。
    // 書き込む前の .prefab が base として要るので、この 2 つは 1 回の呼び出しにまとめてある。
    // PrefabLink は .prefab へ書き出さない。outPropagated に配った個数を返す。
    // 作り直しなので他インスタンスの entity ID は変わる（name / guid は維持）。
    static bool ApplyPrefabInstance(Scene& scene, entt::entity root,
                                    const std::string& assetsDir,
                                    int* outPropagated = nullptr);

    // インスタンスを元 .prefab の状態へ戻す（Revert）。作り直しなので entity ID は変わる。
    // 外部親（サブツリーの外側にいる親）だけは維持する（戻した拍子に階層から飛び出さないため）。
    // 戻り値 = 新しい root（失敗時 entt::null。この場合は元のインスタンスが無傷で残る）。
    static entt::entity RevertPrefabInstance(Scene& scene, entt::entity root,
                                             const std::string& assetsDir);

    // sourcePath が一致する他のインスタンスを全部 Revert する。
    // ★上書き（位置調整など）は消える。.prefab を外部で直接編集したときの手動リセット用。
    //   通常の「直して配る」は ApplyPrefabInstance を使うこと（上書きが残る）。
    // except は起点のインスタンス自身（自分は作り直さない）。戻り値 = 更新した個数。
    static int RefreshPrefabInstances(Scene& scene, const std::string& sourcePath,
                                      const std::string& assetsDir, entt::entity except);
};

} // namespace dx12e
