#pragma once

#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <vector>
#include "core/Types.h"
#include "ecs/Components.h"   // ScriptProp / ScriptPropType
#include <entt/entt.hpp>
#include "engine/core/EventBus.h"   // C++ 汎用イベントバス（events バインドの実体）

// sol2 forward declaration
struct lua_State;
namespace sol { class state; }

namespace dx12e
{

class Scene;
class InputSystem;
class Camera;
class AudioSystem;
class PhysicsSystem;
class ParticleSystem;
class GpuParticleSystem;
class NetworkSystem;
class UISystem;
class ActionMap;

// スクリプトコンポーネントのプロパティ宣言（.lua の properties から解析）。
// 型 / 既定値 / 範囲 / 表示名を持ち、Inspector の自動 UI 生成と Play 時の注入に使う。
struct ScriptPropDef
{
    std::string    name;
    ScriptPropType type = ScriptPropType::Float;
    ScriptProp     def;                 // 既定値
    bool           hasRange = false;
    float          minVal   = 0.0f;
    float          maxVal   = 0.0f;
    std::string    label;               // 表示名（省略時は name）
};

class ScriptEngine
{
public:
    ScriptEngine();
    ~ScriptEngine();

    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    void Initialize(Scene* scene, InputSystem* input, Camera* camera,
                    AudioSystem* audio, PhysicsSystem* physics,
                    const std::string& assetsDir);

    // プロジェクト切替時に assets ベースを再設定（スクリプトの相対パス解決用）。
    // 同じ相対パスが別ファイルを指す可能性があるためスキーマキャッシュも破棄する。
    void SetAssetsDir(const std::string& assetsDir)
    {
        m_assetsDir = assetsDir;
        m_propSchemaCache.clear();
    }

    // パーティクルシステムを Lua fx API へ公開（Application が一度だけ注入）。
    // ポインタはメンバに保持され、Initialize 再実行（シーン切替）後の fx バインドも参照する。
    void SetParticleSystem(ParticleSystem* p) { m_particleSystem = p; }
    void SetGpuParticleSystem(GpuParticleSystem* p) { m_gpuParticleSystem = p; }

    // マルチプレイシステムを Lua net API へ公開（Application が一度だけ注入。null 許容）。
    void SetNetworkSystem(NetworkSystem* n) { m_network = n; }
    // ゲーム UI が入力を食ったかを Lua から読めるようにするためだけに保持する。null 許容。
    void SetUiSystem(UISystem* u) { m_uiSystem = u; }

    // EventBus 注入（WireScriptCallbacks から Application が呼ぶ）。
    // events:on/emit/clear バインドはこのポインタを実行時に参照する（null 許容）。
    void SetEventBus(EventBus* bus) { m_eventBus = bus; }

    void LoadScript(const std::string& filePath);
    void LoadScriptFromString(const std::string& code, const std::string& chunkName);

    // 画面サイズを Lua グローバル SCREEN_W / SCREEN_H に公開（UI レイアウト用）
    void SetScreenSize(int w, int h);

    // ゲームライフサイクル
    void CallOnStart();
    void CallOnUpdate(f32 dt);

    // エンティティアタッチ版 API
    void AttachScriptToEntity(entt::entity e, const std::string& scriptPath);
    void DetachScriptFromEntity(entt::entity e);

    // スクリプトが公開するプロパティ宣言を取得（.lua の `properties = {...}` を解析）。
    // 結果は scriptPath ごとにキャッシュ。Inspector が自動 UI 生成に使う。
    // 宣言が無い / 解析失敗時は空配列を返す（プロパティ無しコンポーネントとして扱う）。
    const std::vector<ScriptPropDef>& GetPropertySchema(const std::string& scriptPath);
    // ファイルを編集した後などにスキーマ再解析を促す（次回 GetPropertySchema で解析しなおす）。
    void InvalidatePropertySchema(const std::string& scriptPath);
    void OnPlayStart();                   // 全 LuaScript 初期化 + OnStart
    // saveNum/loadNum のストア。シーンをまたぐ受け渡しが目的なので Shutdown では消さず、
    // 「Play を押し直したら初期状態」にするために Play 開始時だけ Application が消す。
    void ClearBlackboard();
    void OnPlayStop();                    // 全 env 破棄、started リセット
    void UpdateAttachedScripts(f32 dt);   // 毎フレーム OnUpdate
    void UpdateTriggers(f32 dt);          // 毎フレーム Trigger（イベント）評価（Play 中）
    void ReloadScript(entt::entity e);    // Inspector Reload ボタン用

    // 監視中の .lua が書き換わっていたら、それを使う全エンティティを作り直す。
    // Application が 0.5 秒ごとに呼ぶ。Play を止めずにスクリプトを差し替えるための入口。
    // 戻り値: 作り直したエンティティ数（0 なら変更なし）。
    int ReloadChangedScripts();

    // ファイルが書き換わっていなくても作り直す（強制リロード）。
    // 実行時エラーで死んだスクリプトを Play を止めずに復帰させる入口。
    // onlyPath が空でなければ、その scriptPath を使うものだけが対象。
    // 戻り値: 作り直したエンティティ数。
    int ReloadAllScripts(const std::string& onlyPath = {});

    // いま loadError が立っている LuaScript を全部集める（MCP の一括エラー取得用）。
    // ログを漁らずに「どのスクリプトが死んでいるか」を 1 回で答えるためのもの。
    struct ScriptError
    {
        u32         entity;
        std::string name;         // NameTag（無ければ空）
        std::string scriptPath;
        std::string message;      // errorMessage（sol2 の traceback 込み）
    };
    std::vector<ScriptError> CollectScriptErrors();

    // Lua をコンパイルのみして構文を検証(実行しない・副作用なし)。OK なら true。
    // 失敗時 err にエラー文を入れる。MCP の create で書き込み前チェックに使う。
    bool CheckLuaSyntax(const std::string& code, std::string& err);

    // 任意 Lua コードをその場実行する(MCP eval / デバッグ用)。globals フォールバック環境で
    // 実行するため scene/physics/camera/audio 等の既存グローバルバインディングがそのまま使える
    // (例: `local e = scene:findEntity("Player"); e.transform.position.y = 5`)。
    // 戻り値: 実行成功なら true。code が値を return していれば resultStr に tostring() 文字列。
    // 失敗時は false を返し err にエラー文を入れる(resultStr は空のまま)。
    bool EvalLua(const std::string& code, std::string& resultStr, std::string& err);

    // Lua 入力行の末尾トークンに対する補完候補を返す(コンソールの予測変換用)。
    // グローバル/テーブルは実際の lua state から動的列挙、usertype(scene: 等)は
    // メタテーブルのメソッド名を列挙する。候補はソート済み・重複なし。
    std::vector<std::string> GetCompletions(const std::string& line);

    void Shutdown();

    const std::string& GetLastError() const { return m_lastError; }
    void ClearError() { m_lastError.clear(); }

    // ゲーム制御コールバック（Application が注入）。Lua の loadScene/nextScene/quit から呼ばれる。
    using LoadSceneCb = std::function<void(const std::string&)>;
    using VoidCb      = std::function<void()>;
    // ゲーム内 UI 描画コマンド（WP7）: type/x/y/w/h/size/text/r/g/b/a。戻り値はボタン押下判定に使う id。
    using UiTextCb   = std::function<void(float x, float y, const std::string& text, float size, float r, float g, float b, float a)>;
    using UiButtonCb = std::function<bool(float x, float y, float w, float h, const std::string& label)>;
    using UiImageCb  = std::function<void(float x, float y, float w, float h, const std::string& path)>;
    using UiRectCb   = std::function<void(float x, float y, float w, float h, float r, float g, float b, float a, float rounding)>;

    using TransitionCb = std::function<void(const std::string& rel, int type, float dur)>;
    // ゲーム内 UI のフォーカスナビへ初期フォーカスを与える（Lua: setUiFocus(entity)）。id=entt id。
    using UiFocusCb = std::function<void(std::uint32_t id)>;

    void SetLoadSceneCallback(LoadSceneCb cb) { m_loadSceneCb = std::move(cb); }
    void SetPreloadSceneCallback(LoadSceneCb cb) { m_preloadSceneCb = std::move(cb); }
    void SetNextSceneCallback(VoidCb cb)      { m_nextSceneCb = std::move(cb); }
    void SetQuitCallback(VoidCb cb)           { m_quitCb = std::move(cb); }
    void SetTransitionCallback(TransitionCb cb) { m_transitionCb = std::move(cb); }
    void SetUiCallbacks(UiTextCb t, UiButtonCb b, UiImageCb i)
    { m_uiTextCb = std::move(t); m_uiButtonCb = std::move(b); m_uiImageCb = std::move(i); }
    void SetUiRectCallback(UiRectCb r) { m_uiRectCb = std::move(r); }
    void SetUiFocusCallback(UiFocusCb cb) { m_uiFocusCb = std::move(cb); }

    // ── アクションマップ（キーリバインドの土台）──
    // Application が所有する。Play/Stop で lua state は作り直されるが、
    // キー割り当ては「設定」なのでアプリ寿命で持つ必要がある（ここで借りるだけ）。
    // null 許容（未設定なら Lua の actions.* は何もしない）。
    void SetActionMap(ActionMap* am) { m_actionMap = am; }
    // actions.save() から呼ばれる。バインドの永続化は Application 側の仕事。
    void SetActionSaveCallback(VoidCb cb) { m_actionSaveCb = std::move(cb); }

    // 映像設定（Application が注入）。Lua の display.* から呼ばれる（'.' 呼び、time と同様）。
    struct DisplayCallbacks
    {
        std::function<void(bool)>                        setVsync;
        std::function<bool()>                            getVsync;
        std::function<void(int)>                         setFpsLimit;      // 0=無制限
        std::function<int()>                             getFpsLimit;
        std::function<void(const std::string&)>          setWindowMode;    // "windowed"|"borderless"|"fullscreen"
        std::function<std::string()>                     getWindowMode;
        std::function<void(int, int)>                    setResolution;
        std::function<void(int&, int&)>                  getResolution;
        std::function<std::vector<std::pair<int, int>>()> getResolutions;
    };
    void SetDisplayCallbacks(DisplayCallbacks cb) { m_displayCb = std::move(cb); }

    // ディスク永続の数値ストア（settings.json）。Lua: savePersist(key,v) / loadPersist(key,def)。
    using PersistSaveCb = std::function<void(const std::string&, double)>;
    using PersistLoadCb = std::function<double(const std::string&, double)>;
    void SetPersistCallbacks(PersistSaveCb s, PersistLoadCb l)
    { m_persistSaveCb = std::move(s); m_persistLoadCb = std::move(l); }

private:
    void RegisterBindings();
    // 高レベルヘルパー(actor/keyDown/cameraFollow 等)をグローバルへ定義する
    // Lua prelude を実行する。全アタッチスクリプトから参照可能になる。
    void LoadPrelude();
    void RegisterPhysicsBindings();
    // events グローバルを C++ EventBus への薄いバインドとして登録する。
    void RegisterEventsBinding();
    // net グローバルを NetworkSystem への薄いバインドとして登録する。
    void RegisterNetworkBindings();
    // nav グローバル（ナビメッシュの経路探索）を登録する。
    void RegisterNavBindings();
    // .lua の properties テーブルを解析して out へ詰める（失敗時は out 空のまま）。
    void ParsePropertySchema(const std::string& scriptPath, std::vector<ScriptPropDef>& out);

    std::unique_ptr<sol::state> m_lua;
    Scene*         m_scene   = nullptr;
    InputSystem*   m_input   = nullptr;
    Camera*        m_camera  = nullptr;
    AudioSystem*   m_audio   = nullptr;
    PhysicsSystem* m_physics = nullptr;
    ParticleSystem* m_particleSystem = nullptr;
    GpuParticleSystem* m_gpuParticleSystem = nullptr;   // 大量粒子用（fx の gpu=true で使用）
    NetworkSystem* m_network = nullptr;   // マルチプレイ（net:host/join等）。null 許容
    UISystem*      m_uiSystem = nullptr;  // input:isUiCapturing* 用。null 許容
    EventBus*    m_eventBus = nullptr;   // Application が所有、null 許容（エディタ中は非使用）
    ActionMap*   m_actionMap = nullptr;  // Application が所有、null 許容
    std::string  m_assetsDir;
    std::string  m_lastError;

    // time API の状態（Lua グローバル time.*）。Play 開始でリセット。
    // m_timeScale はスクリプトに渡る dt 自体を倍率する(0=ポーズ、0.5=スローモ)。
    // 物理/パーティクルは対象外(スクリプト駆動のゲームロジックだけがスケールされる)。
    double m_timeElapsed    = 0.0;   // スケール適用済み経過秒
    double m_timeUnscaled   = 0.0;   // 実時間経過秒
    float  m_timeScale      = 1.0f;
    float  m_timeDt         = 0.0f;  // 今フレームのスケール済み dt
    float  m_timeUnscaledDt = 0.0f;
    u64    m_timeFrame      = 0;

    // シーン切替（Shutdown→Initialize で lua state は作り直される）をまたいで
    // 残す数値ストレージ。Lua: saveNum(key,val) / loadNum(key,default)。
    // 例: ゲームシーンでスコアを保存 → リザルトシーンで読み出す。
    std::unordered_map<std::string, double> m_blackboard;

    // scriptPath → プロパティ宣言スキーマのキャッシュ。
    std::unordered_map<std::string, std::vector<ScriptPropDef>> m_propSchemaCache;

    // scriptPath → 最後に見た mtime（file_time_type の生カウント）。ホットリロードの差分検出用。
    // ヘッダに <filesystem> を持ち込まないため int64_t で持つ。
    std::unordered_map<std::string, int64_t> m_scriptMtimes;

    LoadSceneCb  m_loadSceneCb;
    LoadSceneCb  m_preloadSceneCb;
    VoidCb       m_nextSceneCb;
    VoidCb       m_quitCb;
    VoidCb       m_actionSaveCb;
    TransitionCb m_transitionCb;
    UiTextCb    m_uiTextCb;
    UiButtonCb  m_uiButtonCb;
    UiImageCb   m_uiImageCb;
    UiRectCb    m_uiRectCb;
    UiFocusCb   m_uiFocusCb;
    DisplayCallbacks m_displayCb;
    PersistSaveCb    m_persistSaveCb;
    PersistLoadCb    m_persistLoadCb;
};

} // namespace dx12e
