// ===========================================================================
// Application: シーンのロード / Play⇔Editor 遷移 / 永続化
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;


void Application::LoadEditorIcons(ID3D12GraphicsCommandList* cmdList)
{
    auto load = [&](const char* name) -> u64
    {
        std::string p = PathResolver::AssetsDir() + "editor/icons/" + name + ".png";
        if (!std::filesystem::exists(p)) return 0;
        std::wstring wp = PathResolver::Utf8ToWide(p);
        Texture* t = m_resourceManager->GetOrLoadTexture(wp, cmdList, true);
        if (!t) return 0;
        return m_srvHeap->GetGpuHandle(t->GetSrvIndex()).ptr;
    };
    m_icons.logo        = load("logo");
    m_icons.newProject  = load("new_project");
    m_icons.openProject = load("open_project");
    m_icons.recent      = load("recent");
    m_icons.save        = load("save");
    m_icons.git         = load("git");
    m_icons.github      = load("github");
    m_icons.commit      = load("commit");
    m_icons.push        = load("push");

    // ツールバー
    m_icons.file        = load("file");
    m_icons.play        = load("play");
    m_icons.stop        = load("stop");
    m_icons.build       = load("build");
    m_icons.gizmoMove   = load("gizmo_move");
    m_icons.gizmoRotate = load("gizmo_rotate");
    m_icons.gizmoScale  = load("gizmo_scale");
    m_icons.spaceWorld  = load("space_world");
    m_icons.spaceLocal  = load("space_local");
    m_icons.window      = load("window");
    m_icons.uiMode      = load("ui_mode");

    // エンティティ / コンポーネント種別
    m_icons.entMesh     = load("ent_mesh");
    m_icons.entLight    = load("ent_light");
    m_icons.entCamera   = load("ent_camera");
    m_icons.entAudio    = load("ent_audio");
    m_icons.entScript   = load("ent_script");
    m_icons.entPhysics  = load("ent_physics");
    m_icons.entCollider = load("ent_collider");
    m_icons.entUi       = load("ent_ui");
    m_icons.entEmpty    = load("ent_empty");

    // プロジェクトテンプレート
    m_icons.tmplFps     = load("tmpl_fps");
    m_icons.tmplTps     = load("tmpl_tps");
    m_icons.tmpl2d      = load("tmpl_2d");
    m_icons.tmplEmpty   = load("tmpl_empty");

    // 各パネルが EditorContext 経由で参照できるようにポインタを配る
    if (m_editorCtx)
        m_editorCtx->icons = &m_icons;

    Logger::Info("Editor UI icons loaded");
}

// ---------------------------------------------------------------------------
// 手続きスカイ（Scene.h の kProceduralSkyPath）。.dds を 1 枚も用意しなくても
// 背景と IBL が最初から効くようにするための、その場生成のキューブマップ。
// ---------------------------------------------------------------------------

// 方向 → 空の色（リニア HDR）。天頂の青 → 地平の白 → 地面側の灰。
// ponytail: 太陽は焼き込まない。シーンの DirectionalLight と向きが食い違うのと、
//           mip を 1 枚しか作らないので輝点があると IBL プリフィルタでちらつくため。
static DirectX::PackedVector::XMHALF4 ProceduralSkySample(float dx, float dy, float dz)
{
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float y   = dy / len;

    // 表示基準(sRGB風)で決めた色をリニアへ。Skybox_PS はリニアのまま出力する。
    const float zenith[3]  = {0.29f, 0.48f, 0.80f};
    const float horizon[3] = {0.80f, 0.86f, 0.93f};
    const float ground[3]  = {0.33f, 0.32f, 0.31f};  // 明るめの灰＝地平より下が黒く沈まない

    float rgb[3];
    if (y >= 0.0f)
    {
        const float t = std::pow(y, 0.45f);   // 地平付近のグラデーションを厚めに取る
        for (int i = 0; i < 3; ++i) rgb[i] = horizon[i] + (zenith[i] - horizon[i]) * t;
    }
    else
    {
        const float t = std::min(-y / 0.35f, 1.0f);
        const float s = t * t * (3.0f - 2.0f * t);   // smoothstep
        for (int i = 0; i < 3; ++i) rgb[i] = horizon[i] + (ground[i] - horizon[i]) * s;
    }
    return DirectX::PackedVector::XMHALF4(std::pow(rgb[0], 2.2f), std::pow(rgb[1], 2.2f),
                                          std::pow(rgb[2], 2.2f), 1.0f);
}

// 64^2 × 6 面の RGBA16F キューブを CPU で作って GPU へ上げる。形式は IBL パイプラインと
// HDR の .dds に合わせてある（32F はフィルタリング対応がハード依存で安全でない）。
// ponytail: 単一 mip。IBL 側は 32(irradiance) / 128(prefilter) へ畳むので、
//           なめらかなグラデーションが相手ならこれで十分（〜200KB）。
static std::unique_ptr<Texture> MakeProceduralSkyCube(GraphicsDevice& device,
                                                      ID3D12GraphicsCommandList* cmd)
{
    using Half4 = DirectX::PackedVector::XMHALF4;
    constexpr u32 kSize  = 64;
    constexpr u32 kFaces = 6;

    std::vector<std::vector<Half4>> pixels(
        kFaces, std::vector<Half4>(static_cast<size_t>(kSize) * kSize));

    for (u32 f = 0; f < kFaces; ++f)
    {
        for (u32 py = 0; py < kSize; ++py)
        {
            const float v = 1.0f - 2.0f * (static_cast<float>(py) + 0.5f) / kSize;
            for (u32 px = 0; px < kSize; ++px)
            {
                const float u = 2.0f * (static_cast<float>(px) + 0.5f) / kSize - 1.0f;
                float d[3];
                switch (f)   // D3D のキューブ面順: +X -X +Y -Y +Z -Z
                {
                    case 0:  d[0] =  1; d[1] =  v; d[2] = -u; break;
                    case 1:  d[0] = -1; d[1] =  v; d[2] =  u; break;
                    case 2:  d[0] =  u; d[1] =  1; d[2] = -v; break;
                    case 3:  d[0] =  u; d[1] = -1; d[2] =  v; break;
                    case 4:  d[0] =  u; d[1] =  v; d[2] =  1; break;
                    default: d[0] = -u; d[1] =  v; d[2] = -1; break;
                }
                pixels[f][static_cast<size_t>(py) * kSize + px] =
                    ProceduralSkySample(d[0], d[1], d[2]);
            }
        }
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = kSize;
    desc.Height           = kSize;
    desc.DepthOrArraySize = static_cast<u16>(kFaces);
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc       = {1, 0};
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

    D3D12_SUBRESOURCE_DATA sub[kFaces]{};
    for (u32 f = 0; f < kFaces; ++f)
    {
        sub[f].pData      = pixels[f].data();
        sub[f].RowPitch   = static_cast<LONG_PTR>(kSize) * sizeof(Half4);
        sub[f].SlicePitch = sub[f].RowPitch * static_cast<LONG_PTR>(kSize);
    }

    auto tex = std::make_unique<Texture>();
    tex->Initialize(device, cmd, desc, sub, kFaces);
    return tex;
}

void Application::LoadSkyboxIfNeeded(ID3D12GraphicsCommandList* cmd)
{
    if (!m_scene || !m_iblBaker) return;

    const auto& sky = m_scene->GetSkyboxSettings();
    m_iblIntensity    = sky.iblIntensity;
    m_skyboxIntensity = sky.skyboxIntensity;
    m_drawSkybox      = sky.drawSkybox;

    // パス未指定: IBL 無し（ダミーを1度だけベイクしてテーブルは常に有効化）
    if (sky.envMapPath.empty())
    {
        // 「解放 + ダミー再ベイク」が要るのは、(a) まだ一度もベイクしていない
        // (=!m_iblReady)か、(b) 実 env が読み込まれている(=直前は別シーンで envMap 有り)
        // のいずれか。判定を m_loadedSkyboxPath 文字列ではなく実 env の有無で行うことで、
        // シーンロード時に m_loadedSkyboxPath をクリアしても env→empty 切替を取りこぼさない。
        const bool hasRealEnv = (m_envCubeTex != nullptr) || m_iblBaker->HasEnvironment();
        if (hasRealEnv || !m_iblReady)
        {
            // 既存 env を解放
            if (m_envCubeSrvIndex != DescriptorHeap::kInvalidIndex && m_srvHeap)
                m_srvHeap->Free(m_envCubeSrvIndex);
            m_envCubeSrvIndex = DescriptorHeap::kInvalidIndex;
            m_envCubeTex.reset();
            if (m_iblReady && m_srvHeap)
                m_srvHeap->FreeBlock(m_iblBaker->GetSrvBlockStart(), m_iblBaker->GetSrvBlockCount());
            // ダミーベイク（env=null）
            m_iblBaker->Bake(*m_graphicsDevice, cmd, *m_srvHeap, nullptr);
            m_iblReady = m_iblBaker->IsValid();
        }
        m_loadedSkyboxPath = "";
        return;
    }

    // 同一パスかつ既に有効ならスキップ
    if (sky.envMapPath == m_loadedSkyboxPath && m_iblReady && m_iblBaker->HasEnvironment())
        return;

    // 既存 IBL/env を解放してから再ロード
    if (m_iblReady && m_srvHeap)
        m_srvHeap->FreeBlock(m_iblBaker->GetSrvBlockStart(), m_iblBaker->GetSrvBlockCount());
    if (m_envCubeSrvIndex != DescriptorHeap::kInvalidIndex && m_srvHeap)
        m_srvHeap->Free(m_envCubeSrvIndex);
    m_envCubeSrvIndex = DescriptorHeap::kInvalidIndex;
    m_envCubeTex.reset();
    m_iblReady = false;

    // 手続きスカイ: ファイルを読まずにその場で生成する（新規プロジェクトの既定）
    if (sky.envMapPath == kProceduralSkyPath)
    {
        m_envCubeTex = MakeProceduralSkyCube(*m_graphicsDevice, cmd);
    }
    // VFS 経由でまず試みる（ゲームモード = pak から復号、ディスクモード = loose file）
    else
    {
        auto envBytes = vfs::ReadAsset(sky.envMapPath);
        if (!envBytes.empty())
        {
            m_envCubeTex = TextureLoader::LoadCubeFromMemory(*m_graphicsDevice, cmd,
                               envBytes.data(), envBytes.size(), /*srgb=*/false);
        }
        else
        {
            // disk fallback（ディスクモードかつファイルが存在する場合）
            std::string fullPath = PathResolver::AssetsDir() + sky.envMapPath;
            if (!std::filesystem::exists(fullPath))
            {
                Logger::Warn("スカイボックスの環境マップが見つかりません: {}", fullPath);
                // ダミーベイクしてフォールバック
                m_iblBaker->Bake(*m_graphicsDevice, cmd, *m_srvHeap, nullptr);
                m_iblReady = m_iblBaker->IsValid();
                m_loadedSkyboxPath = sky.envMapPath;
                return;
            }
            std::wstring wpath = PathResolver::Utf8ToWide(fullPath);
            m_envCubeTex = TextureLoader::LoadCubeFromFile(*m_graphicsDevice, cmd, wpath, /*srgb=*/false);
        }
    }
    if (!m_envCubeTex)
    {
        Logger::Warn("スカイボックス（キューブマップ）の読み込みに失敗: {}", sky.envMapPath);
        m_iblBaker->Bake(*m_graphicsDevice, cmd, *m_srvHeap, nullptr);
        m_iblReady = m_iblBaker->IsValid();
        m_loadedSkyboxPath = sky.envMapPath;
        return;
    }

    // env cube の TextureCube SRV を shader-visible ヒープに作成
    m_envCubeSrvIndex = m_srvHeap->AllocateIndex();
    m_envCubeTex->CreateCubeSRV(*m_graphicsDevice,
        m_srvHeap->GetCpuHandle(m_envCubeSrvIndex), m_envCubeTex->GetMipLevels());

    // 派生をベイク
    m_iblBaker->Bake(*m_graphicsDevice, cmd, *m_srvHeap, m_envCubeTex->GetResource());
    m_iblReady = m_iblBaker->IsValid();
    m_loadedSkyboxPath = sky.envMapPath;

    Logger::Info("Skybox loaded: {} (ibl={}, sky={})", sky.envMapPath, m_iblIntensity, m_skyboxIntensity);
}


// ---- ユーザー設定の永続化（settings.json）--------------------------------
// 数値のみのフラット JSON。Lua の savePersist/loadPersist と映像設定(video_*)が使う。

std::filesystem::path Application::PersistPath() const
{
    if (m_isGameMode)
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        return std::filesystem::path(exe).parent_path() / L"settings.json";
    }
    // ★AssetsDir() は末尾が '/'（ProjectShaderDir() が s_assets + "shaders/" で
    //   組み立てているのと同じ規約）。path("…/assets/").parent_path() は "…/assets" を
    //   返すので、意図した「assets の隣」ではなく **assets の中** に書いていた。
    //   assets 配下はゲームビルドで game.pak に丸ごと詰められるので、開発機の
    //   音量や解像度が配布物に混ざる。
    std::filesystem::path assets(PathResolver::AssetsDir());
    if (!assets.has_filename()) assets = assets.parent_path();   // 末尾の '/' を落とす
    const std::filesystem::path path = assets.parent_path() / "settings.json";

    // 旧位置(assets/settings.json)に既にあるなら、そちらを読み続ける。
    // 黙って場所が変わると「設定が全部戻った」に見えるため。
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        const std::filesystem::path legacy = assets / "settings.json";
        if (std::filesystem::exists(legacy, ec)) return legacy;
    }
    return path;
}

void Application::LoadPersistStore()
{
    if (m_persistLoaded) return;
    m_persistLoaded = true;
    m_persistStore.clear();
    std::ifstream f(PersistPath());
    if (!f) return;
    try
    {
        nlohmann::json j = nlohmann::json::parse(f);
        for (auto& [k, v] : j.items())
            if (v.is_number()) m_persistStore[k] = v.get<double>();
    }
    catch (const std::exception& e)
    {
        Logger::Warn("settings.json の読み込みに失敗: {}", e.what());
    }
}

void Application::SavePersistStore()
{
    nlohmann::json j = nlohmann::json::object();
    for (auto& [k, v] : m_persistStore) j[k] = v;
    const auto path = PersistPath();
    std::ofstream f(path);
    // ★失敗を黙って捨てない。配布ゲームの PersistPath は exe の隣なので、
    //   Program Files 配下へインストールされると設定・進行度の保存が**全部無言で消える**
    //   （Lua の savePersist は void で、成功したように見える）。
    //   兄弟の SaveActionBindings は同じ失敗を警告しているので、片方だけ抜けていた。
    if (!f)
    {
        Logger::Warn("設定の保存に失敗しました（書き込み権限？）: {}", path.string());
        return;
    }
    f << j.dump(2);
}

double Application::PersistGet(const std::string& key, double def)
{
    LoadPersistStore();
    auto it = m_persistStore.find(key);
    return it == m_persistStore.end() ? def : it->second;
}

void Application::PersistSet(const std::string& key, double v)
{
    LoadPersistStore();
    m_persistStore[key] = v;
    SavePersistStore();
}

void Application::WireScriptCallbacks()
{
    if (!m_scriptEngine) return;

    // アクションマップ（キーリバインド）。Play/Stop で ScriptEngine は作り直されるが
    // 割り当ては設定なので Application 側で持ち続ける＝ここで借り直すだけ。
    m_scriptEngine->SetActionMap(&m_actionMap);
    m_scriptEngine->SetActionSaveCallback([this]() { SaveActionBindings(); });

    m_scriptEngine->SetLoadSceneCallback(
        [this](const std::string& rel) { m_editorCtx->pendingGameLoadPath = rel; });

    m_scriptEngine->SetNextSceneCallback(
        [this]() {
            if (m_sceneFlow)
            {
                std::string n = m_sceneFlow->Next(m_currentSceneRel);
                if (!n.empty()) m_editorCtx->pendingGameLoadPath = n;
            }
        });

    m_scriptEngine->SetQuitCallback(
        [this]() { if (m_window) PostMessageW(m_window->GetHwnd(), WM_CLOSE, 0, 0); });

    m_scriptEngine->SetTransitionCallback(
        [this](const std::string& rel, int type, float dur) {
            if (!m_sceneTransition) return;
            m_transitionTargetScene = rel;
            m_sceneTransition->Start(static_cast<TransitionType>(type), dur);
        });

    // preloadScene: cmdList が要るのでフレーム境界まで遅延（pendingGameLoadPath と同じ流儀）
    m_scriptEngine->SetPreloadSceneCallback(
        [this](const std::string& rel) { m_pendingScenePreloads.push_back(rel); });

    m_scriptEngine->SetUiFocusCallback(
        [this](std::uint32_t id) {
            if (m_uiSystem)
                m_uiSystem->SetFocus(static_cast<entt::entity>(id));
        });

    // ゲーム内 UI（即時モード）コールバック
    m_scriptEngine->SetUiCallbacks(
        [this](float x, float y, const std::string& text, float size, float r, float g, float b, float a) {
            UICommand c; c.type = UICommand::Type::Text;
            c.x = x; c.y = y; c.size = size; c.text = text;
            c.r = r; c.g = g; c.b = b; c.a = a;
            m_uiCommands.push_back(std::move(c));
        },
        [this](float x, float y, float w, float h, const std::string& label) -> bool {
            UICommand c; c.type = UICommand::Type::Button;
            c.x = x; c.y = y; c.w = w; c.h = h; c.text = label;
            m_uiCommands.push_back(std::move(c));
            // 前フレームに押されたか
            return m_pressedButtons.count(label) > 0;
        },
        [this](float x, float y, float w, float h, const std::string& path) {
            UICommand c; c.type = UICommand::Type::Image;
            c.x = x; c.y = y; c.w = w; c.h = h; c.text = path;
            m_uiCommands.push_back(std::move(c));
        });

    m_scriptEngine->SetUiRectCallback(
        [this](float x, float y, float w, float h, float r, float g, float b, float a, float rounding) {
            UICommand c; c.type = UICommand::Type::Rect;
            c.x = x; c.y = y; c.w = w; c.h = h; c.size = rounding;
            c.r = r; c.g = g; c.b = b; c.a = a;
            m_uiCommands.push_back(std::move(c));
        });

    // 映像設定（Lua display.*）。set 系は即適用し settings.json にも保存する。
    // 保存キー: video_vsync / video_fps / video_mode(0=win,1=borderless,2=full) / video_w / video_h
    {
        ScriptEngine::DisplayCallbacks dc;
        // m_persistedVsync も揃える（エディタ側の「変化を見て保存する」判定が
        // 毎フレーム同じ値を書き直さないように）。
        dc.setVsync = [this](bool b) {
            m_useVsync = m_persistedVsync = b;
            PersistSet("video_vsync", b ? 1.0 : 0.0);
        };
        dc.getVsync = [this] { return m_useVsync; };
        dc.setFpsLimit = [this](int v) {
            m_fpsLimit = static_cast<f32>(std::max(0, v));
            PersistSet("video_fps", static_cast<double>(m_fpsLimit));
        };
        dc.getFpsLimit = [this] { return static_cast<int>(m_fpsLimit); };
        dc.setWindowMode = [this](const std::string& m) {
            WindowMode mode = m == "borderless" ? WindowMode::Borderless
                            : m == "fullscreen" ? WindowMode::Fullscreen
                                                : WindowMode::Windowed;
            if (m_window)
                m_window->SetMode(mode, static_cast<u32>(PersistGet("video_w", 0)),
                                        static_cast<u32>(PersistGet("video_h", 0)));
            PersistSet("video_mode", static_cast<double>(mode));
        };
        dc.getWindowMode = [this]() -> std::string {
            if (!m_window) return "windowed";
            switch (m_window->GetMode())
            {
            case WindowMode::Borderless: return "borderless";
            case WindowMode::Fullscreen: return "fullscreen";
            default:                     return "windowed";
            }
        };
        dc.setResolution = [this](int w, int h) {
            PersistSet("video_w", w);
            PersistSet("video_h", h);
            if (!m_window) return;
            if (m_window->GetMode() == WindowMode::Fullscreen)
                m_window->SetMode(WindowMode::Fullscreen, static_cast<u32>(w), static_cast<u32>(h));
            else
                m_window->SetClientSize(static_cast<u32>(w), static_cast<u32>(h));
        };
        dc.getResolution = [this](int& w, int& h) {
            w = m_window ? static_cast<int>(m_window->GetWidth()) : 0;
            h = m_window ? static_cast<int>(m_window->GetHeight()) : 0;
        };
        dc.getResolutions = [] {
            std::vector<std::pair<int, int>> out;
            for (auto& [w, h] : Window::EnumResolutions())
                out.emplace_back(static_cast<int>(w), static_cast<int>(h));
            return out;
        };
        m_scriptEngine->SetDisplayCallbacks(std::move(dc));

        m_scriptEngine->SetPersistCallbacks(
            [this](const std::string& key, double v) { PersistSet(key, v); },
            [this](const std::string& key, double def) { return PersistGet(key, def); });
    }

    // C++ EventBus を ScriptEngine へ注入（events:on/emit/clear が薄いバインドになる）。
    m_scriptEngine->SetEventBus(&m_eventBus);

    // マルチプレイシステムを Lua net API へ注入（net:host/join 等が薄いバインドになる）。
    m_scriptEngine->SetNetworkSystem(m_networkSystem.get());
    m_scriptEngine->SetUiSystem(m_uiSystem.get());   // input:isUiCapturing* 用
}

void Application::ApplyCameraTransformToGlobal(entt::entity camEntity)
{
    using namespace DirectX;
    auto& reg = m_scene->GetRegistry();
    if (!reg.valid(camEntity) || !reg.all_of<Transform>(camEntity)) return;
    const auto& tf = reg.get<Transform>(camEntity);

    // ローカル euler からカメラ規約の forward を構築（pitch 反転は従来同様。
    // エディタのギズモ/フラスタム(Euler)と Camera(forward.y=sin(pitch)) は符号が逆）。
    // 親なしのときはこの forward がそのまま使われ、従来挙動と完全に一致する。
    const f32 yawL   = XMConvertToRadians(tf.rotation.y);
    const f32 pitchL = XMConvertToRadians(-tf.rotation.x);
    const f32 cosP   = std::cos(pitchL);
    XMVECTOR fwd = XMVectorSet(std::sin(yawL) * cosP, std::sin(pitchL), std::cos(yawL) * cosP, 0.0f);

    // 親階層のワールド回転を forward に乗せる＝親オブジェクトの回転に追従する。
    if (tf.parent != entt::null && reg.valid(tf.parent))
        fwd = XMVector3TransformNormal(fwd, ComputeWorldMatrix(reg, tf.parent));
    fwd = XMVector3Normalize(fwd);

    XMFLOAT3 f; XMStoreFloat3(&f, fwd);
    const f32 yaw   = std::atan2(f.x, f.z);
    const f32 pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));

    // 親階層込みのワールド位置を抽出（親オブジェクトの移動に追従する）。
    XMFLOAT3 worldPos; XMStoreFloat3(&worldPos, ComputeWorldMatrix(reg, camEntity).r[3]);

    m_camera->SetPosition(worldPos);
    m_camera->SetYaw(yaw);
    m_camera->SetPitch(pitch);
    // roll は forward に現れないので euler からそのまま渡す（SetYaw/SetPitch が 0 に
    // 戻すので、この順で最後に呼ぶこと）。ヘッドボブはこれで効く。
    m_camera->SetRoll(XMConvertToRadians(tf.rotation.z));
}

void Application::SyncActiveCameraToGlobal()
{
    auto& reg = m_scene->GetRegistry();
    auto camView = reg.view<const CameraComponent, const Transform>();
    for (auto [e, cam, tf] : camView.each())
    {
        if (!cam.isActive) continue;
        // 位置・向きは親階層込みのワールド変換で同期（親にアタッチしたカメラの追従）。
        ApplyCameraTransformToGlobal(e);
        const f32 camAspect =
            static_cast<f32>(m_window->GetWidth()) / static_cast<f32>(m_window->GetHeight());
        if (cam.projection == CameraProjection::Orthographic)
            m_camera->SetOrthographic(2.0f * cam.orthoSize, camAspect, cam.nearClip, cam.farClip);
        else
            m_camera->SetPerspective(
                DirectX::XMConvertToRadians(cam.fovDegrees), camAspect, cam.nearClip, cam.farClip);
        break;
    }
}

void Application::DoRuntimeSceneLoad(const std::string& rel, ID3D12GraphicsCommandList* cmdList)
{
    std::string full = PathResolver::AssetsDir() + rel;
    // ゲームモードはシーンが pak 内＝ディスクに無いので vfs::Exists で確認（エディタはディスク）。
    if (!dx12e::vfs::Exists(rel))
    {
        Logger::Warn("loadScene: シーンが見つかりません: {}", rel);
        return;
    }

    // 物理リセット
    m_physicsSystem->UnregisterAllBodies(m_scene->GetRegistry());
    m_physicsSystem->UnregisterAllCharacters(m_scene->GetRegistry());
    m_physicsSystem->Shutdown();
    m_physicsSystem->Initialize();

    // シーン再構築
    m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(), m_srvHeap.get(), cmdList);
    if (!SceneSerializer::Load(*m_scene, full, PathResolver::AssetsDir()))
    {
        Logger::Warn("loadScene: 読み込みに失敗しました: {}", full);
        return;
    }
    m_editorCtx->currentScenePath = full;
    m_currentSceneRel = rel;

    // ★Lua state を捨てる前に、ネットワークが握っている RPC ハンドラを外す。
    //   ハンドラは Lua の関数を値で持っているので、lua_close 後まで残すと
    //   破棄済みの lua_State を参照する（次の受信 RPC か、同名の再登録による
    //   古いハンドラの破棄で踏む）。接続自体は切らない＝シーンをまたいで繋がったまま。
    if (m_networkSystem) m_networkSystem->ClearRpcHandlers();

    // ScriptEngine 作り直し（コールバック再注入）
    m_scriptEngine->Shutdown();
    m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(), m_camera.get(),
                               m_audioSystem.get(), m_physicsSystem.get(), PathResolver::AssetsDir());
    WireScriptCallbacks();
    LoadGameScript(/*callOnStart=*/true);   // ランタイムのシーン切替もゲームプレイの一部

    // アクティブカメラがなければ最初のものを有効化
    {
        auto& reg = m_scene->GetRegistry();
        auto camView = reg.view<CameraComponent>();
        bool hasActive = false;
        for (auto [e, c] : camView.each()) if (c.isActive) { hasActive = true; break; }
        if (!hasActive && !camView.empty())
            reg.get<CameraComponent>(*camView.begin()).isActive = true;
    }

    m_eventBus.Clear();   // 前シーンの購読を消去（ランタイムシーン切替）
    // 前シーンのボタンに対する保留クリックも破棄（新シーンのハンドラへ誤配信しない）
    if (m_uiSystem)
        m_uiSystem->ResetRuntimeState(m_scene->GetRegistry());
    m_scriptEngine->OnPlayStart();
    // ランタイムのシーン切替でも新シーンのクリップを頭出しする（Play 開始と同じ規律）
    if (m_uiAnimRuntime) m_uiAnimRuntime->OnPlayStart(m_scene->GetRegistry());
    if (m_particleSystem) m_particleSystem->Clear();  // シーン切替時に前シーンの粒子を消す
    if (m_gpuParticles) m_gpuParticles->Clear();
    // 前ステージの振動を持ち込まない（Stop 側と同じ理由）
    if (m_inputSystem)
        for (int pad = 0; pad < 4; ++pad) m_inputSystem->SetPadVibration(pad, 0.0f, 0.0f);
    // ★前シーンの効果音も消す。ここだけ抜けていた（Play→Stop 経路にはある）。
    //   AudioSource(loop=true) の環境音は BuffersQueued が永久に 0 にならないので
    //   空きスロット探索で回収されず、ステージ↔タイトルを数往復すると 16 スロットを
    //   ゾンビが埋め尽くし、**以降すべての SE が同じスロットを奪い合って即座に切れる**。
    //   BGM は「シーンをまたいで鳴らし続ける」用途があるので触らない。
    //   リスナー位置の上書きも戻す（次シーンで setListener を呼ばないと前ステージの
    //   座標にリスナーが固定されたままになる）。
    if (m_audioSystem)
    {
        m_audioSystem->StopAllSFX();
        m_audioSystem->ClearListenerOverride();
    }
    SyncActiveCameraToGlobal();

    // 新シーンの RigidBody を物理登録
    {
        auto& reg = m_scene->GetRegistry();
        for (auto [e, rb] : reg.view<RigidBody>().each())
        {
            if (rb.bodyId != kInvalidBodyId) m_physicsSystem->UnregisterBody(reg, e);
            m_physicsSystem->RegisterBody(reg, e);
        }
    }

    // 新シーンの CharacterController を CharacterVirtual として生成
    {
        auto& reg = m_scene->GetRegistry();
        for (auto [e, cc] : reg.view<CharacterController>().each())
        {
            if (cc._registered) m_physicsSystem->UnregisterCharacter(reg, e);
            m_physicsSystem->RegisterCharacter(reg, e);
        }
    }
    m_physicsSystem->ResetAccumulator();

    // ランタイムでシーンが切り替わったので、新シーンの SkyboxSettings で IBL/skybox を
    // 次フレーム冒頭に再ベイク。m_loadedSkyboxPath はクリアして旧パス一致のスキップを防ぐ。
    m_loadedSkyboxPath.clear();
    m_skyboxDirty = true;

    Logger::Info("Runtime scene loaded: {}", rel);
}

// ===== シーンの参照アセット（先読み／段階ロード共通）=====
namespace
{
bool EndsWithCI(const std::string& s, const char* suf)
{
    const size_t n = std::strlen(suf);
    return s.size() >= n && _stricmp(s.c_str() + s.size() - n, suf) == 0;
}
bool IsTextureRef(const std::string& s)
{
    return EndsWithCI(s, ".png") || EndsWithCI(s, ".jpg") || EndsWithCI(s, ".jpeg")
        || EndsWithCI(s, ".dds") || EndsWithCI(s, ".tga");
}
bool IsModelRef(const std::string& s)
{
    return EndsWithCI(s, ".obj") || EndsWithCI(s, ".fbx")
        || EndsWithCI(s, ".gltf") || EndsWithCI(s, ".glb");
}

// シーン JSON を走査して、参照されているテクスチャ/モデルの assets 相対パスを集める（重複除去）。
//
// ★テクスチャは「どのキーから来たか」で色空間と圧縮形式が変わる。ResourceManager の
//   キャッシュキーにも (srgb, usage) が入っているので、種別を取り違えると
//   **先読みが一度も当たらないうえ、非圧縮のフル解像度が丸ごと余計に載る**
//   （実際そうなっていた: 先読みは usage 既定 Unknown = 圧縮しない、
//     描画は BaseColor/Normal/NonColor で要求するのでキーが一致しない）。
//   JSON のキー名から種別を決める（法線は "normal"、ORM は "metalRoughness"、他は色）。
std::vector<SceneAssetRef> CollectSceneAssetRefs(const std::string& rel)
{
    std::vector<SceneAssetRef> out;
    auto bytes = dx12e::vfs::ReadAsset(rel);
    if (bytes.empty()) return out;
    nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (j.is_discarded()) return out;

    // 同じパスでも種別が違えば別エントリ（キャッシュキーが別なので両方温める必要がある）
    std::unordered_set<std::string> seen;
    std::function<void(const nlohmann::json&, const std::string&)> walk =
        [&](const nlohmann::json& node, const std::string& key) {
        if (node.is_string())
        {
            const std::string s = node.get<std::string>();
            SceneAssetRef r;
            r.path = s;
            if (IsTextureRef(s))
            {
                const bool isNormal = (key.find("normal") != std::string::npos)
                                   || (key.find("Normal") != std::string::npos);
                const bool isMR     = (key.find("metalRoughness") != std::string::npos)
                                   || (key.find("MetalRoughness") != std::string::npos);
                // materialTextureOverrides の "albedo" は BaseColor で要求される
                const bool isAlbedo = (key == "albedo") || (key.find("Albedo") != std::string::npos);
                r.isTexture = true;
                r.srgb  = !(isNormal || isMR);
                // ★既定は Unknown（GetOrLoadTexture の既定と同じ）。
                //   スプライト / パーティクル / UI 画像 / LUT の実ロードは
                //   `GetOrLoadTexture(path, cmd)` と既定引数で呼んでいるので、
                //   ここで BaseColor にすると**今度はそちらと不一致になる**。
                //   明示的な usage を渡すのはマテリアル上書きの 3 キーだけ
                //   （ApplicationPipeline.cpp が BaseColor/Normal/NonColor で要求する）。
                r.usage = isNormal ? TextureUsage::Normal
                        : isMR     ? TextureUsage::NonColor
                        : isAlbedo ? TextureUsage::BaseColor
                                   : TextureUsage::Unknown;
            }
            else if (IsModelRef(s))
            {
                r.isTexture = false;
            }
            else return;

            const std::string dedup = s + "|" + std::to_string(static_cast<int>(r.usage))
                                    + (r.srgb ? "1" : "0");
            if (seen.insert(dedup).second) out.push_back(std::move(r));
        }
        else if (node.is_object())
        {
            for (auto it = node.begin(); it != node.end(); ++it) walk(it.value(), it.key());
        }
        else if (node.is_array())
        {
            for (const auto& c : node) walk(c, key);   // 配列は親のキーを引き継ぐ
        }
    };
    walk(j, std::string());
    return out;
}
} // namespace

// 参照 1 件をキャッシュへ載せる。キャッシュキーは実ロード経路と同一の絶対パス形式
// (テクスチャ=wide絶対+srgb既定true / モデル=assetsDir+相対)に合わせる。
void Application::WarmSceneAssetRef(const SceneAssetRef& r, ID3D12GraphicsCommandList* cmdList)
{
    if (!m_resourceManager || !cmdList) return;
    if (r.isTexture)
        m_resourceManager->GetOrLoadTexture(
            PathResolver::Utf8ToWide(PathResolver::AssetsDir() + r.path), cmdList, r.srgb, r.usage);
    else
        m_resourceManager->GetOrLoadModel(PathResolver::AssetsDir() + r.path, cmdList);
}

// シーン JSON を走査し、参照されているテクスチャ/モデルをキャッシュへ先読みする。
// シーンは構築しない(エンティティ/物理/スクリプトに影響なし)。
void Application::DoScenePreload(const std::string& rel, ID3D12GraphicsCommandList* cmdList)
{
    const auto refs = CollectSceneAssetRefs(rel);
    if (refs.empty())
    {
        Logger::Warn("preloadScene: シーンが見つからないか参照アセットがありません: {}", rel);
        return;
    }
    for (const auto& s : refs) WarmSceneAssetRef(s, cmdList);
    Logger::Info("preloadScene: {} (アセット{}件)", rel, refs.size());
}

// ===== 段階的シーンロード =====
// 重いシーンを1フレームで読むとメッセージポンプが止まり「エンジンが固まった」ように見える。
// 参照アセットの読み込みをフレームに分散し、その間はローディング UI を出し続ける。
void Application::BeginSceneLoadJob(const std::string& fullPath, const std::string& rel, bool runtime)
{
    if (m_sceneLoadJob) return;   // 同時に1本だけ

    // 「重い」の判定は参照アセット数だけでは足りない。エンティティが数万あるシーンは
    // 参照アセットが数件でも SceneSerializer::Load 自体に秒単位かかる（＝固まって見える）。
    // JSON のサイズも見て、大きいシーンは必ずローディング UI を出す経路に乗せる。
    std::error_code sizeEc;
    const auto bytes = std::filesystem::file_size(fullPath, sizeEc);
    const bool bigFile = !sizeEc && bytes >= kSceneLoadBigFileBytes;

    // 巨大な JSON では参照アセットの走査自体が重い（12MB で ~1 秒）。ここで走らせると
    // ローディング UI が出る前に固まるので、大きいシーンは走査せずに UI を出して
    // そのまま実体化へ回す（アセットは SceneSerializer::Load が構築中に読む）。
    auto refs = bigFile ? std::vector<SceneAssetRef>{} : CollectSceneAssetRefs(rel);

    // 軽いシーンは分割しない（オーバーレイが1フレームだけ光って見えるのを避ける）
    if (!bigFile && refs.size() < kSceneLoadAsyncThreshold) return;

    m_sceneLoadJob = std::make_unique<SceneLoadJob>();
    m_sceneLoadJob->fullPath = fullPath;
    m_sceneLoadJob->rel      = rel;
    m_sceneLoadJob->runtime  = runtime;
    m_sceneLoadJob->assets   = std::move(refs);
    Logger::Info("シーンを分割ロード: {} (アセット{}件)", rel, m_sceneLoadJob->assets.size());
}

void Application::UpdateSceneLoadJob(ID3D12GraphicsCommandList* cmdList)
{
    if (!m_sceneLoadJob || !cmdList) return;
    SceneLoadJob& job = *m_sceneLoadJob;
    job.spin += m_gameClock.GetDeltaTime();

    // 最初の1フレームは何もしない＝ローディング UI が必ず1回 Present される。
    // これをしないと、参照アセットが少ない（けどエンティティが多い）シーンで
    // 同じフレーム内に実体化まで走り、画面には何も出ないまま固まって見える。
    if (job.frames++ == 0)
    {
        if (m_editorCtx) m_editorCtx->sceneLoadProgress = 0.0f;
        return;
    }

    // 時間予算ぶんだけ進める。1件も進まないと永久に終わらないので必ず1件は処理してから測る。
    const auto t0 = std::chrono::steady_clock::now();
    while (job.next < job.assets.size())
    {
        // 先に current を更新してから読む。1 件に数秒かかる（初回の BC 圧縮など）
        // ケースで「どのファイルで止まっているか」が画面に出るようにするため。
        job.current = job.assets[job.next].path;   // 進捗表示はパスだけ
        WarmSceneAssetRef(job.assets[job.next++], cmdList);
        const f64 ms = std::chrono::duration<f64, std::milli>(
                           std::chrono::steady_clock::now() - t0).count();
        if (ms >= kSceneLoadBudgetMs) break;
    }
    if (job.next >= job.assets.size())
        job.current.clear();

    // ステータスバー用の進捗（ローディングオーバーレイと同じ割合）
    if (m_editorCtx && !job.assets.empty())
        m_editorCtx->sceneLoadProgress =
            static_cast<f32>(job.next) / static_cast<f32>(job.assets.size());

    if (job.next < job.assets.size()) return;   // 続きは次フレーム（この間もUIは動く）

    // 先読み完了 → 実体化。参照アセットは全てキャッシュ済みなのでここは短時間で終わる。
    const std::string full = job.fullPath;
    const std::string rel  = job.rel;
    const bool runtime     = job.runtime;
    m_sceneLoadJob.reset();
    if (m_editorCtx) m_editorCtx->sceneLoadProgress = -1.0f;
    FinishSceneLoad(full, rel, runtime, cmdList);
}

// 先読みが済んだ（または軽くて分割不要だった）シーンを実際に構築する。
// 同期ロードと段階ロードの共通後段。runtime=true は Play 中の切替（物理/スクリプト再構築込み）。
void Application::FinishSceneLoad(const std::string& fullPath, const std::string& rel, bool runtime,
                                  ID3D12GraphicsCommandList* cmdList)
{
    // TAA: 別のシーンの絵を履歴として持ち越さない（持ち越すと切替直後に前のシーンが半透明で残る）。
    InvalidateTemporalHistory();

    if (runtime)
    {
        DoRuntimeSceneLoad(rel, cmdList);
        return;
    }

    // ---- エディタでシーンを開く ----
    m_editorCtx->ClearSelection();
    m_editorCtx->undoSystem.Clear();
    // 前のシーンの粒子・トレイルを持ち込まない（ランタイム側の DoRuntimeSceneLoad と同じ規律）
    if (m_particleSystem) m_particleSystem->Clear();
    if (m_gpuParticles)   m_gpuParticles->Clear();

    m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(), m_srvHeap.get(), cmdList);
    const bool loaded = SceneSerializer::Load(*m_scene, fullPath, PathResolver::AssetsDir());
    if (loaded)
    {
        // 開いた直後はディスクの内容と一致している＝未保存ではない
        MarkSceneClean();
        m_editorCtx->currentScenePath = fullPath;
        m_currentSceneRel = rel;
        ProjectManager::SaveLastOpenedScene(fullPath);
        m_editorCtx->hotReloadFlash = 1.5f;
        m_editorLayer->RefreshAssetBrowser();
        // 開いたシーン(既存ゲーム / プロジェクト / Grid 無しテンプレ)に Grid が無ければ補う。
        // Scene::Initialize 済み(有効 cmdList)なのでここでメッシュ生成して安全。
        EnsureEditorGrid();
        // ロードしたシーンの SkyboxSettings(envMapPath/iblIntensity 等) を反映するため
        // 次フレーム冒頭で再ベイクを要求。別 envMapPath のシーンを開いても自動追従する。
        // 差分判定の取りこぼし防止に m_loadedSkyboxPath をクリア。
        m_loadedSkyboxPath.clear();
        m_skyboxDirty = true;
        ++m_sceneGeneration;   // 古い entity id を無効化(MCP の STALE_SCENE 検出用)
        m_mcpIdempotency.clear();   // 別シーンの entity を idempotentReplay で誤返却しないようクリア
        Logger::Info("Scene loaded: {}", fullPath);

        if (!m_autosaveRestoreTarget.empty())
        {
            // オートセーブから復旧した。パスを本来のシーンへ戻す。
            // 戻さないと次の Ctrl+S が .autosave/scene.json を上書きしてしまう。
            m_editorCtx->currentScenePath = m_autosaveRestoreTarget;
            m_currentSceneRel             = ToAssetRel(m_autosaveRestoreTarget);
            m_autosaveRestoreTarget.clear();
            // 復旧した内容はディスクの本体と違う＝未保存。ここを落とすと
            // 「復旧したのに保存し忘れて閉じる」が無警告で通ってしまう。
            m_editorCtx->undoSystem.MarkEdited();
            Logger::Info("自動保存から復旧しました。保存先: {}", m_editorCtx->currentScenePath);
        }
        else
        {
            CheckAutosaveRecovery(fullPath);
        }
    }

    // MCP open_scene の遅延応答。
    if (m_mcpLoadReply.client != 0)
    {
        if (loaded)
        {
            auto& reg = m_scene->GetRegistry();
            int entityCount = 0;
            for (auto e : reg.view<NameTag>()) { (void)e; ++entityCount; }
            CompleteMcp(m_mcpBridge.get(), m_mcpLoadReply,
                nlohmann::json{{"sceneName", std::filesystem::path(fullPath).stem().string()},
                               {"path", rel},
                               {"entityCount", entityCount},
                               {"sceneGeneration", m_sceneGeneration}});
        }
        else
        {
            FailMcp(m_mcpBridge.get(), m_mcpLoadReply, McpErr::Internal,
                    "scene load failed: " + fullPath);
        }
        m_mcpLoadReply = {};
    }
}

void Application::RenderSceneLoadingOverlay()
{
    if (!m_sceneLoadJob) return;
    const SceneLoadJob& job = *m_sceneLoadJob;

    // ゲームは全画面、エディタは3Dビューポート矩形だけ覆う（パネルは操作できるまま）。
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pos = vp->Pos, size = vp->Size;
    if (!m_isGameMode && m_editorLayer && m_editorLayer->GetViewportSize().x > 1.0f)
    {
        pos  = m_editorLayer->GetViewportPos();
        size = m_editorLayer->GetViewportSize();
    }

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(vp->ID);
    // 不透明: トランジション（ImGui より後に描かれる）をロード中は止めているので、
    // 旧シーンを隠す役目はこのオーバーレイが全部持つ。
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.05f, 0.07f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##SceneLoadingOverlay", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);

    // スピナー（プロジェクト読込のオーバーレイと同じ意匠）
    const float r = 30.0f;
    const int   segs = 28;
    const float t = job.spin * 3.2f;
    for (int i = 0; i < segs; ++i)
    {
        const float a0 = t + static_cast<float>(i) / segs * 6.2831853f;
        const float a1 = t + static_cast<float>(i + 1) / segs * 6.2831853f;
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(
            ImVec4(0.39f, 0.58f, 0.93f, static_cast<float>(i) / segs));
        dl->AddLine(ImVec2(c.x + cosf(a0) * r, c.y + sinf(a0) * r),
                    ImVec2(c.x + cosf(a1) * r, c.y + sinf(a1) * r), col, 5.0f);
    }

    auto centerText = [&](const char* s, float dy, ImU32 col) {
        const ImVec2 ts = ImGui::CalcTextSize(s);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + dy), col, s);
    };
    const bool warming = job.next < job.assets.size();
    centerText(warming ? "アセットを読み込み中..." : "シーンを構築中...",
               r + 22.0f, IM_COL32(235, 235, 235, 255));
    centerText(job.rel.c_str(), r + 44.0f, IM_COL32(140, 143, 152, 255));

    // 「今なにをしているか」。件数だけだと 1 件に数秒かかる初回 BC 圧縮で
    // 止まって見えるので、処理中のファイル名まで出す。
    if (warming && !job.assets.empty())
    {
        char line[320];
        const std::string& cur = job.current;
        const size_t slash = cur.find_last_of('/');
        const char* base = (slash == std::string::npos) ? cur.c_str() : cur.c_str() + slash + 1;
        snprintf(line, sizeof(line), "%zu / %zu   %s", job.next, job.assets.size(), base);
        centerText(line, r + 66.0f, IM_COL32(120, 124, 134, 255));
    }

    // 進捗バー（アセット先読みの消化率）。件数が分かるのは先読みフェーズだけなので、
    // 構築フェーズでは端から端へ往復するバーにする＝満タンのまま止まって見せない。
    const float barW = (std::min)(size.x * 0.5f, 320.0f), barH = 6.0f;
    const float y    = c.y + r + 94.0f;
    const float x0   = c.x - barW * 0.5f;
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + barW, y + barH),
                      IM_COL32(40, 42, 50, 255), barH * 0.5f);
    if (!job.assets.empty())
    {
        const float frac = static_cast<float>(job.next) / static_cast<float>(job.assets.size());
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + barW * frac, y + barH),
                          IM_COL32(76, 141, 255, 255), barH * 0.5f);
    }
    else
    {
        const float segW = barW * 0.3f;
        const float sx   = x0 + (barW - segW) * (0.5f - 0.5f * cosf(job.spin * 3.0f));
        dl->AddRectFilled(ImVec2(sx, y), ImVec2(sx + segW, y + barH),
                          IM_COL32(76, 141, 255, 255), barH * 0.5f);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void Application::EnsureEditorGrid()
{
    // 封印ランタイム(ゲーム)では編集用グリッドは不要。
    if (m_isGameMode)
        return;

    auto& reg = m_scene->GetRegistry();

    // 既に GridPlane を持つエンティティがあれば何もしない（二重生成を防ぐ）。
    auto gridView = reg.view<GridPlane>();
    if (gridView.begin() != gridView.end())
        return;

    // グリッド未配置のシーン（旧データ / Grid 無しテンプレ）に編集用グリッドを追加。
    // SpawnPlane はメッシュ生成に Scene の cmdList を使うので、呼び出し側で有効化済みであること。
    m_scene->SpawnPlane("Grid", {0, 0, 0}, kEditorGridSize, true);
    Logger::Info("EnsureEditorGrid: グリッド未配置のシーンへ編集用グリッドを追加");
}

void Application::LaunchNetTestClient()
{
    // ホスト(自分)の待ち受けポートへ 127.0.0.1 で自動接続する検証用の別ウィンドウを起動する。
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    // ★ホストが待ち受けている実ポートへ繋ぐ（net_setup で port を変えていたら defaultPort ではない）
    const u16 port = (m_editorCtx && m_editorCtx->netTestJoinPort != 0)
                   ? m_editorCtx->netTestJoinPort
                   : (m_networkSystem ? m_networkSystem->Config().defaultPort : u16(7777));
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --editor --net-client 127.0.0.1:"
                     + std::to_wstring(port);
    if (!m_projectInfo.rootDir.empty())
    {
        // rootDir は UTF-8。日本語パスでも化けないように wide へ変換してから渡す。
        int n = MultiByteToWideChar(CP_UTF8, 0, m_projectInfo.rootDir.c_str(), -1, nullptr, 0);
        std::wstring wroot(n > 0 ? static_cast<size_t>(n) : 0, L'\0');
        if (n > 0)
        {
            MultiByteToWideChar(CP_UTF8, 0, m_projectInfo.rootDir.c_str(), -1, wroot.data(), n);
            wroot.pop_back();
        }
        cmd += L" --project \"" + wroot + L"\"";
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Logger::Info("テストクライアントを起動しました (127.0.0.1:{})", port);
    }
    else
    {
        Logger::Warn("テストクライアントの起動に失敗しました (GetLastError={})", GetLastError());
    }
}

void Application::EnterPlayMode()
{
    InvalidateTemporalHistory();   // Editor の絵を Play の履歴に持ち越さない
    m_mcpCameraOverride = false;   // MCP の撮影用カメラ固定はモード遷移で必ず解除する

    // カメラ設置チェック
    {
        auto& reg = m_scene->GetRegistry();
        auto camCheck = reg.view<const CameraComponent>();
        if (camCheck.empty())
        {
            // ★配布したゲームではここで詰む。ゲームモードは起動時に一度だけ
            //   m_modeChangeRequested を立てて消費するので**再試行しない**うえ、
            //   errorMessage を出す ToolbarPanel はエディタでしか描かれない。
            //   結果「Play へ入れず Editor モードのまま、フリーカメラの絵だけが出続ける」
            //   ＝ゲームが無言で動かない。理由を必ず見せてから終了する。
            Logger::Error("Play を中止しました: アクティブな CameraComponent がありません");
            if (m_isGameMode)
            {
                MessageBoxW(nullptr,
                            L"開始シーンに Camera がありません。\n"
                            L"エディタでシーンに Camera を追加してからビルドし直してください。",
                            L"起動できません", MB_OK | MB_ICONERROR);
                m_isRunning = false;   // 動かないウィンドウを残さない
                return;
            }
            m_editorCtx->errorMessage = "シーンに Camera が配置されていません。\nHierarchy 右クリック → Camera で追加してください。";
            m_editorCtx->errorFlash = 1.0f;
            return;
        }
    }

    // GPU を待機してコマンドリスト状態を安全にする
    m_commandQueue->WaitIdle();

    // カメラ状態保存
    m_cameraSnapshot.position = m_camera->GetPosition();
    m_cameraSnapshot.yaw = m_camera->GetYaw();
    m_cameraSnapshot.pitch = m_camera->GetPitch();
    m_cameraSnapshot.moveSpeed        = m_camera->GetMoveSpeed();
    m_cameraSnapshot.mouseSensitivity = m_camera->GetMouseSensitivity();

    // Lua が触る前のエディタ状態を Stop 時の完全復元用に保存
    m_playSceneJson = SceneSerializer::SaveToString(*m_scene, PathResolver::AssetsDir());

    // ★「isActive なカメラが無ければ先頭を有効化」はこのスナップショットより**後**に置く。
    //   前に置くと自動有効化が JSON に焼き付き、全カメラを非アクティブにしてあるシーンで
    //   Play を押しただけで Stop 後もそのカメラが有効なまま残る（未保存フラグも立たない）。
    {
        auto& reg = m_scene->GetRegistry();
        auto camCheck = reg.view<const CameraComponent>();
        bool hasActive = false;
        for (auto [e, cam] : camCheck.each())
            if (cam.isActive) { hasActive = true; break; }
        if (!hasActive && camCheck.begin() != camCheck.end())
        {
            auto first = *camCheck.begin();
            reg.get<CameraComponent>(first).isActive = true;
            Logger::Info("Auto-activated first CameraComponent for play mode");
        }
    }

    // ★saveNum のストアは Play をまたいで残っていた（シーンまたぎの受け渡しが目的で
    //   Shutdown では消さない設計）。Play を押し直したら初期状態から始まるべきなので
    //   ここで消す（ランタイムのシーン切替から呼ばれる OnPlayStart に入れてはいけない）。
    if (m_scriptEngine) m_scriptEngine->ClearBlackboard();

    // シーンパスも退避（Play 中の loadScene/goToScene が書き換えるため。Stop で復元）
    m_playScenePathSnapshot = m_editorCtx->currentScenePath;
    m_playSceneRelSnapshot  = m_currentSceneRel;

    // ゲーム用カメラ: アクティブな CameraComponent をグローバル Camera に同期
    SyncActiveCameraToGlobal();

    // OnPlayStart 直後に Lua が変えた値を打ち消すため、
    // Transform / RigidBody / Light / Material PBR を覚えておく
    m_editorSnapshots.clear();
    {
        auto& reg = m_scene->GetRegistry();
        auto view = reg.view<NameTag, Transform>();
        for (auto [entity, name, transform] : view.each())
        {
            EntitySnapshot snap;
            snap.position      = transform.position;
            snap.rotation      = transform.rotation;
            snap.scale         = transform.scale;
            snap.quaternion    = transform.quaternion;
            snap.useQuaternion = transform.useQuaternion;

            snap.hasRigidBody = reg.all_of<RigidBody>(entity);
            if (snap.hasRigidBody)
                snap.rigidBodyData = reg.get<RigidBody>(entity);

            snap.hasPointLight = reg.all_of<PointLight>(entity);
            if (snap.hasPointLight)
                snap.pointLightData = reg.get<PointLight>(entity);

            snap.hasDirectionalLight = reg.all_of<DirectionalLight>(entity);
            if (snap.hasDirectionalLight)
                snap.directionalLightData = reg.get<DirectionalLight>(entity);

            snap.hasSpotLight = reg.all_of<SpotLight>(entity);
            if (snap.hasSpotLight)
                snap.spotLightData = reg.get<SpotLight>(entity);

            snap.hasMeshRenderer = reg.all_of<MeshRenderer>(entity);
            if (snap.hasMeshRenderer)
            {
                const auto& mr = reg.get<MeshRenderer>(entity);
                snap.materialMetallicOverride  = mr.overrideMetallic;
                snap.materialRoughnessOverride = mr.overrideRoughness;
            }

            m_editorSnapshots[name.name] = snap;
        }
    }

    m_inputSystem->SetMouseCapture(false);

    // スクリプトエンジン初期化（エンティティにアタッチされた LuaScript 用）
    // ※ 以前ここには「グローバルな game.lua の OnStart は呼ばない」とあったが、
    //   docs/API_REFERENCE.md は OnStart / OnUpdate の両方をライフサイクルとして書いており、
    //   実際 CallOnStart の呼び出し元は 1 つも無かった＝ドキュメント側が正しく実装が欠けていた。
    //   Play 開始はゲームプレイの開始なので、ここで呼ぶ（下の LoadGameScript(true)）。
    m_scriptEngine->Shutdown();
    m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                               m_camera.get(), m_audioSystem.get(),
                               m_physicsSystem.get(), PathResolver::AssetsDir());
    WireScriptCallbacks();

    LoadGameScript(/*callOnStart=*/true);
    m_eventBus.Clear();   // Play 開始時に前 Play の購読を完全消去
    // playOnStart のクリップ/シートを頭出し。events:on の登録より前だと発火を取りこぼすので、
    // Lua の OnPlayStart より**後**に置く（最初の実評価は次フレームの Update）。
    m_scriptEngine->OnPlayStart();
    // プレイセッション記録を開始（Play を押した時点で自動。開始用ツールは要らない）。
    // OnPlayStart の後＝ここまでに出た Lua のロードエラーもログ経由で記録に載る。
    m_playSession.Start(m_gameClock.GetTotalTime());
    if (m_uiAnimRuntime) m_uiAnimRuntime->OnPlayStart(m_scene->GetRegistry());
    if (m_particleSystem) m_particleSystem->Clear();  // Play 開始時に粒子をリセット
    if (m_gpuParticles) m_gpuParticles->Clear();

    // マルチプレイ ローカルテストループ(フェーズ⑨): ツールバーのPlayドロップダウンで
    // 選んだロールに従い、Luaを書かずに net:host()/net:join() 相当を自動実行する。
    // m_eventBus.Clear() より後なので、net.hostStarted/net.connected イベントは
    // OnStart() 内の events:on 登録に間に合う(Post→Flushはフレーム末)。
    if (m_networkSystem && m_editorCtx->netTestRole != NetTestRole::Offline)
    {
        std::string netErr;
        if (m_editorCtx->netTestRole == NetTestRole::Host)
        {
            // ★netTestJoinPort はクライアント分岐だけが見ていて、ホストは defaultPort 固定だった。
            //   `dx12_net_setup(role="host", port=9000)` は {"port":9000} を返して成功するのに
            //   実際は 7777 で待ち受ける＝**別マシンから繋がらないのにログも出ない**。
            const u16 hostPort = m_editorCtx->netTestJoinPort != 0
                               ? m_editorCtx->netTestJoinPort
                               : m_networkSystem->Config().defaultPort;
            if (!m_networkSystem->Host(hostPort, m_networkSystem->Config().maxPlayers, netErr))
                Logger::Warn("テストロール: ホスト開始に失敗しました: {}", netErr);
        }
        else if (m_editorCtx->netTestRole == NetTestRole::Client)
        {
            const u16 port = m_editorCtx->netTestJoinPort != 0
                           ? m_editorCtx->netTestJoinPort           // --net-client ip:port の明示指定
                           : m_networkSystem->Config().defaultPort;
            if (!m_networkSystem->Join(m_editorCtx->netTestJoinAddress, port, netErr))
                Logger::Warn("テストロール: 接続開始に失敗しました: {}", netErr);
        }
    }

    // エディタのスナップショットで上書き（Luaが勝手に変えた状態をエディタの状態に戻す）
    {
        auto& reg = m_scene->GetRegistry();
        auto view = reg.view<NameTag, Transform>();
        for (auto [entity, name, transform] : view.each())
        {
            auto it = m_editorSnapshots.find(name.name);
            if (it == m_editorSnapshots.end()) continue;
            const auto& snap = it->second;

            // Transform 復元
            transform.position      = snap.position;
            transform.rotation      = snap.rotation;
            transform.scale         = snap.scale;
            transform.quaternion    = snap.quaternion;
            transform.useQuaternion = snap.useQuaternion;

            // Physics: エディタで外してたら Lua が付けたものを削除
            if (!snap.hasRigidBody)
            {
                reg.remove<RigidBody>(entity);
                reg.remove<ConvexHullCollider>(entity);
                reg.remove<BoxCollider>(entity);
                reg.remove<SphereCollider>(entity);
                reg.remove<CapsuleCollider>(entity);
            }
            else
            {
                // エディタのパラメータで上書き（Luaのデフォルト値ではなくエディタの設定値を使う）
                auto rb = snap.rigidBodyData;
                rb.bodyId = kInvalidBodyId;
                reg.emplace_or_replace<RigidBody>(entity, rb);
            }

            // Lighting: エディタで設定したライトを Play 初期化後も維持する。
            // LuaScript の OnStart が同じエンティティへ既定ライト値を入れても、配置値を優先する。
            if (snap.hasPointLight)
                reg.emplace_or_replace<PointLight>(entity, snap.pointLightData);
            else
                reg.remove<PointLight>(entity);

            if (snap.hasDirectionalLight)
            {
                auto dl = snap.directionalLightData;
                dl._prevRot = transform.rotation;
                dl._prevRotInit = true;
                reg.emplace_or_replace<DirectionalLight>(entity, dl);
            }
            else
            {
                reg.remove<DirectionalLight>(entity);
            }

            if (snap.hasSpotLight)
            {
                auto sl = snap.spotLightData;
                sl._prevRot = transform.rotation;
                sl._prevRotInit = true;
                reg.emplace_or_replace<SpotLight>(entity, sl);
            }
            else
            {
                reg.remove<SpotLight>(entity);
            }

            // Material PBR 復元（エディタで持っていた override 状態をそのまま戻す）。
            // Material を持たないプリミティブ床へ有効 override を捏造すると metallic=1 になり、
            // 床だけ拡散光が消えて暗くなるので、-1(=Material/既定値を使う) も保持する。
            if (snap.hasMeshRenderer && reg.all_of<MeshRenderer>(entity))
            {
                auto& mr = reg.get<MeshRenderer>(entity);
                mr.overrideMetallic  = snap.materialMetallicOverride;
                mr.overrideRoughness = snap.materialRoughnessOverride;
            }
        }
    }

    // 物理のタイムステップ蓄積をリセット
    m_physicsSystem->ResetAccumulator();

    // 全 RigidBody を物理エンジンに登録（エディタで復元した状態で）
    {
        auto& reg = m_scene->GetRegistry();
        auto view = reg.view<RigidBody>();
        for (auto [entity, rb] : view.each())
        {
            if (rb.bodyId != kInvalidBodyId)
                m_physicsSystem->UnregisterBody(reg, entity);
            m_physicsSystem->RegisterBody(reg, entity);
        }
    }

    // 全 CharacterController を CharacterVirtual として生成
    {
        auto& reg = m_scene->GetRegistry();
        for (auto [entity, cc] : reg.view<CharacterController>().each())
        {
            if (cc._registered) m_physicsSystem->UnregisterCharacter(reg, entity);
            m_physicsSystem->RegisterCharacter(reg, entity);
        }
    }

    // ホットリロード用タイムスタンプ更新（エディタ）
    {
        std::string scriptPath = PathResolver::GameLuaPath();
        if (std::filesystem::exists(scriptPath))
            m_scriptLastWriteTime = std::filesystem::last_write_time(scriptPath);
    }

    m_engineMode = EngineMode::Playing;
    Logger::Info("Entered PLAY mode");
}

void Application::EnterEditorMode()
{
    DX_ASSERT(!m_playSceneJson.empty(),
              "EnterEditorMode requires a prior EnterPlayMode snapshot");

    InvalidateTemporalHistory();   // Play の絵を Editor の履歴に持ち越さない
    m_mcpCameraOverride = false;   // MCP の撮影用カメラ固定はモード遷移で必ず解除する

    m_commandQueue->WaitIdle();

    // Play 中に鳴っていた SE（空間含む）と BGM を停止（Stop で鳴り続けるのを防ぐ）
    if (m_audioSystem) { m_audioSystem->StopAllSFX(); m_audioSystem->StopBGM(); }

    // ★パッドの振動も止める。`padVibrate(1,1)`（seconds 省略＝手動で止めるまで鳴り続ける形）の
    //   最中に Stop すると、エディタに戻ってもコントローラーが震えたままで、止める UI が無い。
    //   音・粒子・ネットワークは全部ここで畳んでいるのに、振動だけ抜けていた。
    if (m_inputSystem)
        for (int pad = 0; pad < 4; ++pad) m_inputSystem->SetPadVibration(pad, 0.0f, 0.0f);

    // ★Play 中の粒子・トレイルも消す。Clear は Play 開始側にしか無かった。
    //   Application::Update はエディタ中も粒子シムを回すので、爆発の途中で Stop すると
    //   寿命ぶん（fx:burst{life=8} なら 8 秒）エディタのビューポートで飛び続ける。
    //   トレイルはもっと悪い: ParticleSystem::m_trails のキーが u64(entt::entity) で、
    //   Stop はレジストリを作り直す＝id が再利用されるため、**新しいエンティティが
    //   前のエンティティの軌跡を相続して**、離れた 2 点間にリボンが張られる。
    if (m_particleSystem) m_particleSystem->Clear();
    if (m_gpuParticles)   m_gpuParticles->Clear();

    // Play 中に張ったネットワーク接続を Stop で確実に切る（次の Play に持ち越さない）。
    if (m_networkSystem) m_networkSystem->Disconnect();

    // OnPlayStop は ScriptEngine::Shutdown より前に呼ぶ（Shutdown で Lua state が消える）
    if (m_engineMode == EngineMode::Playing)
        m_scriptEngine->OnPlayStop();
    // 記録を閉じる（中身は次の Play まで残す＝遊び終えてから MCP で取りに来られる）。
    m_playSession.Stop(m_gameClock.GetTotalTime());

    // EventBus には Play 中に登録された Lua ハンドラのラムダが残っている。
    // 物理を Shutdown→Initialize で作り直す前に購読を消し、物理側の EventBus 参照も外す。
    // こうしておくと Initialize と ScriptEngine::Shutdown の間に万一 Flush が走っても
    // 半壊状態の古いハンドラが発火しない（呼び順が将来変わっても安全）。
    m_eventBus.Clear();
    m_physicsSystem->SetEventBus(nullptr);

    // retained UI の保留クリック/ボタン状態を破棄（次の Play へ持ち越さない）
    if (m_uiSystem)
        m_uiSystem->ResetRuntimeState(m_scene->GetRegistry());
    if (m_uiAnimRuntime)
        m_uiAnimRuntime->OnPlayStop(m_scene->GetRegistry());

    m_physicsSystem->UnregisterAllBodies(m_scene->GetRegistry());
    m_physicsSystem->UnregisterAllCharacters(m_scene->GetRegistry());
    m_physicsSystem->Shutdown();
    m_physicsSystem->Initialize();
    // 新しい物理システムに同じ EventBus を再注入（接触イベントの配信先を復帰）。
    m_physicsSystem->SetEventBus(&m_eventBus);

    m_inputSystem->SetMouseCapture(false);

    m_camera->SetPosition(m_cameraSnapshot.position);
    m_camera->SetYaw(m_cameraSnapshot.yaw);
    m_camera->SetPitch(m_cameraSnapshot.pitch);
    // ゲームが camera:setMoveSpeed / setMouseSensitivity を触っていた場合に備えて戻す
    // （エディタのフライカメラとツールバーの「カメラ速度」が同じ値を読むため）。
    m_camera->SetMoveSpeed(m_cameraSnapshot.moveSpeed);
    m_camera->SetMouseSensitivity(m_cameraSnapshot.mouseSensitivity);

    // ★Play 中に始まった段階ロード（Lua の loadScene）が生き残ると、Stop で復元した
    //   直後のシーンを数フレーム後に丸ごと差し替える＝編集内容が無言で消える。
    //   UpdateSceneLoadJob はモードを見ずに毎フレーム進むので、ここで捨てる。
    if (m_sceneLoadJob)
    {
        Logger::Warn("Stop: 進行中のランタイムシーン読み込みを破棄しました");
        m_sceneLoadJob.reset();
        m_editorCtx->sceneLoadProgress = -1.0f;
    }

    // ★環境マップ / IBL はシーン JSON の復元では戻らない（ベイク結果は
    //   m_loadedSkyboxPath でキャッシュされ、m_skyboxDirty 経由でしか焼き直さない）。
    //   ランタイムのシーン切替をしたあと Stop すると、空と環境光だけが切替先のまま残り、
    //   「明るさは合っているのに背景と色味だけ違う」という気づきにくい形になる。
    m_loadedSkyboxPath.clear();
    m_skyboxDirty = true;

    m_editorCtx->ClearSelection();

    // Play 中のランタイムシーン切替で汚れたシーンパスを Play 開始時点に戻す。
    // これをしないとパス無し save_scene / Ctrl+S / エディタ終了時の自動保存が
    // 別シーン(タイトル等)を上書きする。下のディスク再読込フォールバックも復元後のパスで行う。
    // ★以前は `if (!m_playScenePathSnapshot.empty())` で囲っていた。Play 開始時が
    //   未保存の新規シーン（パス空）だと復元自体を飛ばすので、Lua の loadScene で
    //   移った先のパスが残り、以後の Ctrl+S / 自動保存が**そのシーンを上書きする**。
    //   このコメントが防ごうとした事故が、まさにこの経路だけ通っていた。無条件に戻す。
    m_editorCtx->currentScenePath = m_playScenePathSnapshot;
    m_currentSceneRel             = m_playSceneRelSnapshot;
    m_playScenePathSnapshot.clear();
    m_playSceneRelSnapshot.clear();

    // JSON スナップショットからシーン全体を完全復元
    {
        auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);

        // Scene::Spawn は内部で m_cmdList を使うので最新の cmdList で更新する
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), cmdList);

        // スナップショットからシーンを完全復元する。失敗(空/破損 JSON、復元中の例外)すると
        // ApplySceneJson が scene.Clear() 後に中断してシーンが空になる(= Stop 後に list_entities が
        // count:0 になる原因)。失敗を握りつぶさず、ディスク上の現在シーンから読み直してフォールバックする
        // (ユーザが手動で open_scene し直して復旧していた動作を自動化)。
        bool restored = false;
        try {
            restored = SceneSerializer::LoadFromString(*m_scene, m_playSceneJson, PathResolver::AssetsDir());
        } catch (const std::exception& ex) {
            Logger::Error("EnterEditorMode: スナップショット復元で例外が発生: {}", ex.what());
        }
        if (!restored && !m_editorCtx->currentScenePath.empty()) {
            Logger::Error("EnterEditorMode: スナップショット復元に失敗。ディスクから再読込します: {}",
                          ToAssetRel(m_editorCtx->currentScenePath));
            try {
                restored = SceneSerializer::Load(*m_scene, m_editorCtx->currentScenePath,
                                                 PathResolver::AssetsDir());
            } catch (const std::exception& ex) {
                Logger::Error("EnterEditorMode: ディスクからの再読込にも失敗: {}", ex.what());
            }
        }
        if (!restored)
            Logger::Error("EnterEditorMode: Stop 後のシーンが空です（有効なスナップショットもディスクのシーンもありません）");

        m_scriptEngine->Shutdown();
        m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                                   m_camera.get(), m_audioSystem.get(),
                                   m_physicsSystem.get(), PathResolver::AssetsDir());
        WireScriptCallbacks();

        ThrowIfFailed(cmdList->Close());
        m_commandQueue->ExecuteCommandList(cmdList);
        m_commandQueue->WaitIdle();
        m_resourceManager->FinishUploads();
        m_frameResources->EndFrame(*m_commandQueue);
    }

    // 古い JSON で誤復元しないよう、消費後はクリアする
    m_playSceneJson.clear();
    m_playSceneJson.shrink_to_fit();

    // シーン再構築でエンティティ ID が変わり Undo スタックの参照が無効になるためクリア
    m_editorCtx->undoSystem.Clear();
    m_editorCtx->ClearSelection();

    // Stop でもシーンを丸ごと作り直すため entity id が全部変わる。open_scene/new_scene と同様に
    // 世代を進めて古い id を無効化する(これが無いと Stop 後に古い id が別 entity に化けて
    // "invalid entity id" や誤動作になる)。dx12_stop の応答に新しい sceneGeneration が乗る。
    ++m_sceneGeneration;

    // Play 中に CameraComponent の FOV を採用してた可能性があるためエディタ用に戻す
    m_camera->SetPerspective(DirectX::XM_PIDIV4,
        static_cast<f32>(m_window->GetWidth()) / static_cast<f32>(m_window->GetHeight()),
        0.1f, 1000.0f);

    m_inputSystem->SetMouseCapture(false);
    m_engineMode = EngineMode::Editor;
    Logger::Info("Entered EDITOR mode");
}

void Application::LoadGameScript(bool callOnStart)
{
    // ゲームモードでは game.lua はディスクに無く game.pak 内に在る（exists() は false）。
    // LoadScript は VFS 経由で pak から読むので、InGameMode 時は存在チェックを迂回する。
    // ※ ScriptEngine を作り直すたび（EnterPlayMode / シーン切替 / プロジェクト読込）に
    //   グローバル OnUpdate も作り直されるので、その都度ここを呼ぶ必要がある。
    // ★ゲームモードでも存在確認する。以前は InGameMode なら無条件に LoadScript を
    //   呼んでいたので、グローバル game.lua を持たないプロジェクト（持つかどうかは任意）では
    //   **配布物のログに毎回「Luaエラー（スクリプト読み込み）」が出ていた**。
    //   無いだけならエラーではないし、本物のエラーが埋もれる。
    //   GameLuaPath() は vfs 対応済みなので、同じ判定で両モードを見られる。
    std::string scriptPath = PathResolver::GameLuaPath();
    if (dx12e::vfs::ExistsAbs(scriptPath) || std::filesystem::exists(scriptPath))
    {
        m_scriptEngine->LoadScript(scriptPath);
        // ★OnStart はここでしか呼ばれない。以前は ScriptEngine::CallOnStart の呼び出し元が
        //   **リポジトリ全体で 0 件**で、OnUpdate だけが配線されていた。
        //   docs/API_REFERENCE.md は両方あると書いているので、
        //   「ファイルは明らかに読めていて OnUpdate は毎フレーム動くのに OnStart だけ死んでいる」
        //   という一番たちの悪い形になっていた（初期化・BGM 開始・キー割り当てが全部無言で消える）。
        if (callOnStart) m_scriptEngine->CallOnStart();
    }
    else
        Logger::Warn("ゲームスクリプトが見つかりません: {}（グローバル game.lua は任意）", scriptPath);
}

// エンジン診断が検査前に現在のシーンを退避するための保存先。assets 配下に置くのは、
// シーンJSON がアセットを assets 相対で参照するため（外に出すと復元時に解決できない）。
std::string Application::SaveSceneSnapshot()
{
    if (!m_scene || !m_editorCtx)
        return {};

    const std::string assetsDir = PathResolver::AssetsDir();
    const std::string path      = assetsDir + "scenes/.diagnostics_snapshot.json";
    std::error_code ec;
    std::filesystem::create_directories(assetsDir + "scenes", ec);
    if (!SceneSerializer::Save(*m_scene, path, assetsDir))
    {
        Logger::Warn("診断: シーンの退避に失敗しました: {}", path);
        return {};
    }
    return path;
}

void Application::RequestSceneRestore(const std::string& path)
{
    // ★同じ「波括弧の無い if」がここにもあった。skipConfirm は無条件に立っていたので、
    //   path が空（退避に失敗した等）でも次のシーン読み込みが未保存確認を飛ばす＝
    //   編集内容を聞かずに捨てうる。
    if (m_editorCtx && !path.empty())
    {
        m_editorCtx->pendingLoadPath        = path;   // フレーム境界で SceneSerializer::Load される
        m_editorCtx->pendingLoadSkipConfirm = true;   // 診断のシーン復元。人間の操作ではないので聞かない
    }
}



} // namespace dx12e
