#include "editor/panels/MaterialEditorPanel.h"
#include "editor/EditorContext.h"
#include "editor/panels/AssetBrowserPanel.h"
#include "resource/MaterialAssetManager.h"
#include "core/Logger.h"
#include "core/PathResolver.h"

#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstdio>

#include <commdlg.h>

namespace fs = std::filesystem;

namespace dx12e
{

namespace
{
// AssetBrowserPanel/InspectorPanel と同じ絶対→assets相対の正規化(バックスラッシュ→スラッシュ、
// assetsDir プレフィックス除去)。ここでも独立実装が一番シンプルなので踏襲する。
std::string ToRelative(const std::string& absOrRelPath, const std::string& assetsDir)
{
    std::string abs  = fs::path(absOrRelPath).lexically_normal().string();
    std::string base = fs::path(assetsDir).lexically_normal().string();
    std::replace(abs.begin(), abs.end(), '\\', '/');
    std::replace(base.begin(), base.end(), '\\', '/');
    if (abs.rfind(base, 0) == 0)
    {
        std::string rel = abs.substr(base.size());
        while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
        return rel;
    }
    return abs;  // 既に相対 or assetsDir 外
}

std::string Slugify(const std::string& name)
{
    std::string s;
    s.reserve(name.size());
    for (char c : name)
    {
        if (std::isalnum(static_cast<unsigned char>(c))) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!s.empty() && s.back() != '_') s += '_';
    }
    while (!s.empty() && s.back() == '_') s.pop_back();
    return s.empty() ? "material" : s;
}
}

void MaterialEditorPanel::Initialize(MaterialAssetManager* materialAssetManager, AssetBrowserPanel* assetBrowser,
                                     GraphicsDevice& device, DescriptorHeap* srvHeap, ResourceManager* resourceManager)
{
    m_materialAssetManager = materialAssetManager;
    m_assetBrowser = assetBrowser;
    m_preview.Initialize(device, srvHeap, resourceManager);
}

void MaterialEditorPanel::RenderPreview3D(EditorContext& ctx, CommandList& cmd)
{
    if (!ctx.showMaterialEditor || !m_preview.IsValid()) return;

    MaterialPreviewRenderer::DrawInput input;
    input.albedoPath         = m_current.albedoPath;
    input.normalPath         = m_current.normalPath;
    input.metalRoughnessPath = m_current.metalRoughnessPath;
    input.metallic           = m_current.metallic;
    input.roughness          = m_current.roughness;
    m_preview.Render(cmd, input, m_previewShape, m_camYaw, m_camPitch, m_camDist);
}

void MaterialEditorPanel::NewAsset()
{
    m_current = MaterialAssetData{};
    m_current.metallic = 1.0f;
    m_current.roughness = 1.0f;
    m_current.uvTilingU = 1.0f;
    m_current.uvTilingV = 1.0f;
    m_current.name = "NewMaterial";
    m_currentPath.clear();
    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", m_current.name.c_str());
}

bool MaterialEditorPanel::LoadAsset(const std::string& relPath, const std::string& assetsDir)
{
    std::ifstream ifs(assetsDir + relPath, std::ios::binary);
    if (!ifs) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    MaterialAssetData data;
    if (!ParseMaterialAsset(bytes, data)) return false;
    if (data.name.empty()) data.name = fs::path(relPath).stem().string();

    m_current = data;
    m_currentPath = relPath;
    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", m_current.name.c_str());
    return true;
}

bool MaterialEditorPanel::SaveAsset(const std::string& assetsDir)
{
    m_current.name = m_nameBuf;
    if (m_currentPath.empty())
        m_currentPath = "materials/" + Slugify(m_current.name) + ".dxmat";

    std::error_code ec;
    fs::path fullPath(assetsDir + m_currentPath);
    fs::create_directories(fullPath.parent_path(), ec);

    std::ofstream ofs(fullPath, std::ios::binary | std::ios::trunc);
    if (!ofs)
    {
        Logger::Warn("マテリアルアセットの保存に失敗しました: {}", fullPath.string());
        return false;
    }
    std::string json = SerializeMaterialAsset(m_current);
    ofs.write(json.data(), static_cast<std::streamsize>(json.size()));
    ofs.close();

    if (m_materialAssetManager)
        m_materialAssetManager->Invalidate(m_currentPath);
    m_preview.InvalidateThumbnail(assetsDir + m_currentPath);   // アセットブラウザの球体サムネも更新
    return true;
}

void MaterialEditorPanel::DrawTextureSlot(const std::string& assetsDir, const char* label, std::string& texRelPath)
{
    ImGui::PushID(label);
    constexpr float kThumbSize = 72.0f;

    bool hasTex = !texRelPath.empty();
    u64 gpuHandle = (hasTex && m_assetBrowser) ? m_assetBrowser->GetOrQueueThumbnail(assetsDir + texRelPath) : 0;

    ImGui::Text("%s", label);
    bool clicked;
    if (gpuHandle != 0)
        clicked = ImGui::ImageButton("##thumb", static_cast<ImTextureID>(gpuHandle), ImVec2(kThumbSize, kThumbSize));
    else
        clicked = ImGui::Button(hasTex ? "..." : "(none)", ImVec2(kThumbSize * 2.5f, kThumbSize));
    if (clicked)
        ImGui::OpenPopup("MatEditTexPicker");

    bool changed = false;
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
        {
            const char* droppedPath = static_cast<const char*>(payload->Data);
            std::string ext = fs::path(droppedPath).extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Texture)
            {
                texRelPath = ToRelative(droppedPath, assetsDir);
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (hasTex)
    {
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 180.0f);
        ImGui::TextWrapped("%s", texRelPath.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("x")) { texRelPath.clear(); changed = true; }
        ImGui::EndGroup();
    }

    if (ImGui::BeginPopup("MatEditTexPicker"))
    {
        ImGui::TextDisabled("Select Texture");
        ImGui::Separator();
        if (ImGui::Selectable("(None)", !hasTex)) { texRelPath.clear(); changed = true; ImGui::CloseCurrentPopup(); }

        std::error_code ec;
        fs::path root(assetsDir);
        if (fs::exists(root, ec))
        {
            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator end;
            for (; !ec && it != end; it.increment(ec))
            {
                std::error_code fec;
                if (!it->is_regular_file(fec) || fec) continue;
                std::string rowExt = it->path().extension().string();
                for (char& c : rowExt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (AssetBrowserPanel::ClassifyExtension(rowExt) != AssetBrowserPanel::AssetType::Texture) continue;

                fs::path relPath = fs::relative(it->path(), root, fec);
                if (fec) continue;
                std::string relStr = relPath.generic_string();
                if (ImGui::Selectable(relStr.c_str(), texRelPath == relStr))
                {
                    texRelPath = relStr;
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndPopup();
    }

    ImGui::PopID();

    // テクスチャは離散的な割当操作なので、変更が確定した時点で即保存+Invalidate(SRV再構築)する。
    if (changed && !m_currentPath.empty())
        SaveAsset(assetsDir);
}

void MaterialEditorPanel::ImportFromImage(const std::string& assetsDir)
{
    // ワイド版ダイアログを使う。ANSI 版(GetOpenFileNameA)だと日本語ファイル名/フォルダ名が
    // CP932 で返り、UTF-8 前提のエンジン内パス(vfs::Exists / PathResolver::Utf8ToWide)と食い違って
    // 「コピーは成功するのにテクスチャ解決に失敗して白いまま」になる。
    // OFN_NOCHANGEDIR: ダイアログがプロセスのカレントディレクトリを選択先へ変えてしまう
    // Win32 の既定挙動を抑止(相対パス依存の処理を壊さないため)。
    wchar_t pathBuf[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = nullptr;
    ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg)\0*.png;*.jpg;*.jpeg\0All Files\0*.*\0";
    ofn.lpstrFile   = pathBuf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    std::wstring initDir = PathResolver::Utf8ToWide(assetsDir);
    ofn.lpstrInitialDir = initDir.c_str();
    if (!GetOpenFileNameW(&ofn))
        return;

    fs::path picked(pathBuf);
    std::wstring extW = picked.extension().wstring();
    for (wchar_t& c : extW) c = static_cast<wchar_t>(std::towlower(c));
    if (extW != L".png" && extW != L".jpg" && extW != L".jpeg")
        return;
    std::string extAscii;  // 上でASCII拡張子のみ通しているので1バイト化して安全
    for (wchar_t c : extW) extAscii += static_cast<char>(c);

    // コピー先ファイル名は ASCII へ正規化する。日本語等の非ASCII名のままだと、UTF-8/ACP の
    // 変換が混在する下流(vfs::Exists のナロー ifstream 等)で解決に失敗するため。
    std::string asciiStem;
    for (wchar_t wc : picked.stem().wstring())
    {
        if ((wc >= L'a' && wc <= L'z') || (wc >= L'A' && wc <= L'Z') || (wc >= L'0' && wc <= L'9'))
            asciiStem += static_cast<char>(std::tolower(static_cast<int>(wc)));
        else if (!asciiStem.empty() && asciiStem.back() != '_')
            asciiStem += '_';
    }
    while (!asciiStem.empty() && asciiStem.back() == '_') asciiStem.pop_back();
    if (asciiStem.empty()) asciiStem = "imported";

    // assets 配下かの判定もワイドパスで行う(大文字小文字/区切り文字を正規化して前方一致)。
    auto normalizeW = [](std::wstring s) {
        for (wchar_t& c : s) { if (c == L'\\') c = L'/'; c = static_cast<wchar_t>(std::towlower(c)); }
        return s;
    };
    const std::wstring absNorm  = normalizeW(picked.lexically_normal().wstring());
    const std::wstring baseNorm = normalizeW(fs::path(PathResolver::Utf8ToWide(assetsDir)).lexically_normal().wstring());

    std::string rel;
    if (!baseNorm.empty() && absNorm.rfind(baseNorm, 0) == 0)
    {
        // 既にプロジェクトassets配下 → コピーせずそのまま参照。相対部分が非ASCIIを含む場合は
        // 下流のパス解決が保証できないため、プロジェクト外と同じコピー経路へフォールバックする。
        std::wstring relW = absNorm.substr(baseNorm.size());
        while (!relW.empty() && relW.front() == L'/') relW.erase(relW.begin());
        bool asciiOnly = std::all_of(relW.begin(), relW.end(), [](wchar_t c) { return c < 128; });
        if (asciiOnly)
            for (wchar_t c : relW) rel += static_cast<char>(c);
    }

    if (rel.empty())
    {
        // プロジェクト外(「自分のPNG/JPG」) → assets/textures/imported/ へコピー(重複時は連番)。
        // ASSET_MANIFEST.md への記録は行わない(ユーザー自身の素材でライセンス記録の対象外)。
        std::error_code ec;
        fs::path destDir = fs::path(PathResolver::Utf8ToWide(assetsDir)) / L"textures" / L"imported";
        fs::create_directories(destDir, ec);
        std::string destName = asciiStem + extAscii;
        fs::path destPath = destDir / destName;
        int suffix = 1;
        while (fs::exists(destPath))
        {
            destName = asciiStem + "_" + std::to_string(suffix++) + extAscii;
            destPath = destDir / destName;
        }
        ec.clear();
        fs::copy_file(picked, destPath, ec);
        if (ec)
        {
            Logger::Warn("画像のコピーに失敗しました: {}", ec.message());
            m_statusMsg = "\xe7\x94\xbb\xe5\x83\x8f\xe3\x81\xae\xe3\x82\xb3\xe3\x83\x94\xe3\x83\xbc\xe3\x81\xab\xe5\xa4\xb1\xe6\x95\x97\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f";  // 画像のコピーに失敗しました
            m_statusFlash = 3.0f;
            return;
        }
        rel = "textures/imported/" + destName;
    }

    // 「よくわからない普通の画像」への無難な既定値: 非金属・粗さ高め。
    m_current = MaterialAssetData{};
    m_current.name        = asciiStem;
    m_current.albedoPath  = rel;
    m_current.metallic    = 0.0f;
    m_current.roughness   = 0.8f;
    m_current.uvTilingU   = 1.0f;
    m_current.uvTilingV   = 1.0f;
    m_currentPath.clear();
    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", m_current.name.c_str());

    m_statusMsg = "\xe7\x94\xbb\xe5\x83\x8f\xe3\x81\x8b\xe3\x82\x89\xe4\xbd\x9c\xe6\x88\x90\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f: " + m_current.name;  // 画像から作成しました:
    m_statusFlash = 2.0f;
}

void MaterialEditorPanel::RenderWindow(EditorContext& ctx, const std::string& assetsDir)
{
    if (!ctx.pendingOpenMaterialPath.empty())
    {
        std::string rel = ToRelative(ctx.pendingOpenMaterialPath, assetsDir);
        ctx.pendingOpenMaterialPath.clear();
        LoadAsset(rel, assetsDir);
        ctx.showMaterialEditor = true;
    }

    if (!ctx.showMaterialEditor) return;

    ImGui::SetNextWindowSize(ImVec2(1300.0f, 980.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("\xe3\x83\x9e\xe3\x83\x86\xe3\x83\xaa\xe3\x82\xa2\xe3\x83\xab\xe3\x82\xa8\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xbf###MaterialEditorFloating",  // マテリアルエディタ
                      &ctx.showMaterialEditor, ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }
    // このフローティング窓は3Dビューポートの上に重なって浮かぶことがある。ホバー中はシーンカメラの
    // ドラッグ/ホイール操作(HandleCameraNavigation)が同時に反応しないよう EditorContext 経由で伝える
    // (ThisFrame に書き、読む側は前フレームの確定値を見る=EditorContext のコメント参照)。
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                               | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem
                               | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
        ctx.floatingToolWindowHoveredThisFrame = true;

    if (ImGui::Button("\xe6\x96\xb0\xe8\xa6\x8f New")) NewAsset();  // 新規
    ImGui::SameLine();
    if (ImGui::Button("\xe7\x94\xbb\xe5\x83\x8f\xe3\x81\x8b\xe3\x82\x89\xe4\xbd\x9c\xe6\x88\x90... Import Image"))  // 画像から作成...
        ImportFromImage(assetsDir);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_currentPath.empty() ? "(unsaved)" : m_currentPath.c_str());

    ImGui::Separator();

    // ---- 左: 3Dプレビュー(球体/平面、オービットドラッグ) ----
    ImGui::BeginGroup();
    {
        const char* shapeLabel = (m_previewShape == MaterialPreviewShape::Sphere)
            ? "\xe7\x90\x83\xe4\xbd\x93 Sphere" : "\xe5\xb9\xb3\xe9\x9d\xa2 Plane";  // 球体/平面
        if (ImGui::Button(shapeLabel, ImVec2(140.0f, 0.0f)))
            m_previewShape = (m_previewShape == MaterialPreviewShape::Sphere)
                ? MaterialPreviewShape::Plane : MaterialPreviewShape::Sphere;

        if (m_preview.IsValid())
        {
            // ImGui::Image は「掴めない」アイテムのため、その上で左ドラッグを始めるとImGui標準の
            // ウィンドウ移動として扱われ、回転操作と一緒に窓ごと動いてしまう。InvisibleButton を
            // 敷いてドラッグをアイテムとして捕捉し(=アクティブアイテム化)、画像は DrawList で重ね描きする。
            const float previewSize = static_cast<float>(MaterialPreviewRenderer::kPreviewSize);
            const ImVec2 imgPos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##previewOrbit", ImVec2(previewSize, previewSize));
            ImGui::GetWindowDrawList()->AddImage(
                static_cast<ImTextureID>(m_preview.GetPreviewGpuHandle()),
                imgPos, ImVec2(imgPos.x + previewSize, imgPos.y + previewSize));

            ImGuiIO& io = ImGui::GetIO();
            // カーソル固定オービット(無限回転、Unrealのビューポート操作と同じ)。
            // ImGui のイベントキュー(io.MousePos/WantSetMousePos)経由はワープとイベント適用の
            // タイミングが噛み合わず回転量が出ないため、Win32 の物理カーソル位置を直接読み書きする:
            // GetCursorPos(即値) でアンカーからの移動量を取り、SetCursorPos で即座にアンカーへ戻す。
            // ワープ→読み取りが同期的なので相殺やフレーム遅延が原理的に起きない。
            if (ImGui::IsItemActivated())
            {
                POINT p;
                ::GetCursorPos(&p);   // スクリーン座標(物理)。ImGui座標系とは独立に扱う
                m_orbitAnchorX = static_cast<f32>(p.x);
                m_orbitAnchorY = static_cast<f32>(p.y);
            }
            // IsItemActive: プレビュー上で押下開始したドラッグのみ回転(途中で画像外へ出ても継続)。
            if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                POINT p;
                ::GetCursorPos(&p);
                m_camYaw   -= (static_cast<f32>(p.x) - m_orbitAnchorX) * 0.01f;
                m_camPitch -= (static_cast<f32>(p.y) - m_orbitAnchorY) * 0.01f;
                m_camPitch = std::clamp(m_camPitch, -1.5f, 1.5f);

                ::SetCursorPos(static_cast<int>(m_orbitAnchorX), static_cast<int>(m_orbitAnchorY));
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);   // ドラッグ中は非表示。離すとアンカー位置に再表示
            }
            if (ImGui::IsItemHovered())
                m_camDist = std::clamp(m_camDist - io.MouseWheel * 0.3f, 1.5f, 12.0f);

            ImGui::TextDisabled("\xe3\x83\x89\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xb0\xe3\x81\xa7\xe5\x9b\x9e\xe8\xbb\xa2"
                                 "\xe3\x83\xbb\xe3\x83\x9b\xe3\x82\xa4\xe3\x83\xab\xe3\x81\xa7\xe3\x82\xba\xe3\x83\xbc\xe3\x83\xa0");
                                 // ドラッグで回転・ホイールでズーム
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                "\xe3\x83\x97\xe3\x83\xac\xe3\x83\x93\xe3\x83\xa5\xe3\x83\xbc\xe5\x88\x9d\xe6\x9c\x9f\xe5\x8c\x96\xe5\xa4\xb1\xe6\x95\x97");
                // プレビュー初期化失敗
        }
    }
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginGroup();

    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputText("\xe5\x90\x8d\xe5\x89\x8d Name", m_nameBuf, sizeof(m_nameBuf));  // 名前
    ImGui::SameLine();
    if (ImGui::Button("\xe4\xbf\x9d\xe5\xad\x98 Save"))
    {
        if (SaveAsset(assetsDir))
        {
            m_statusMsg = "\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f: " + m_current.name;  // 保存しました:
            m_statusFlash = 2.0f;
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Textures");
    DrawTextureSlot(assetsDir, "Albedo", m_current.albedoPath);
    DrawTextureSlot(assetsDir, "Normal", m_current.normalPath);
    DrawTextureSlot(assetsDir, "MetalRoughness (ARM: G=Rough/B=Metal)", m_current.metalRoughnessPath);

    ImGui::Spacing();
    ImGui::TextDisabled("Scalars");

    // ドラッグ中は UpdateScalarsOnly で SRV を触らず即時プレビュー反映、指を離した瞬間にディスクへ保存する。
    auto liveSlider = [&](const char* label, f32* v, f32 lo, f32 hi)
    {
        bool dragging = ImGui::SliderFloat(label, v, lo, hi, "%.3f");
        if (dragging && m_materialAssetManager && !m_currentPath.empty())
        {
            m_materialAssetManager->UpdateScalarsOnly(m_currentPath, m_current.metallic, m_current.roughness,
                                                       m_current.uvTilingU, m_current.uvTilingV);
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && !m_currentPath.empty())
            SaveAsset(assetsDir);
    };
    liveSlider("\xe9\x87\x91\xe5\xb1\x9e\xe6\x84\x9f Metallic", &m_current.metallic, 0.0f, 1.0f);
    liveSlider("\xe7\xb2\x97\xe3\x81\x95 Roughness", &m_current.roughness, 0.0f, 1.0f);

    ImGui::Spacing();
    ImGui::TextDisabled("UV Tiling");
    liveSlider("U", &m_current.uvTilingU, 0.01f, 32.0f);
    liveSlider("V", &m_current.uvTilingV, 0.01f, 32.0f);

    if (m_currentPath.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
            "\xe6\x9c\xaa\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\xae\xe6\x96\xb0\xe8\xa6\x8f\xe3\x83\x9e\xe3\x83\x86\xe3\x83\xaa\xe3\x82\xa2\xe3\x83\xab"
            "\xe3\x81\xa7\xe3\x81\x99\xe3\x80\x82\xe5\x90\x8d\xe5\x89\x8d\xe3\x82\x92\xe4\xbb\x98\xe3\x81\x91\xe3\x81\xa6\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84");
            // 未保存の新規マテリアルです。名前を付けて保存してください

    if (m_statusFlash > 0.0f)
    {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.6f, 1.0f), "%s", m_statusMsg.c_str());
        m_statusFlash -= ImGui::GetIO().DeltaTime;
    }

    ImGui::EndGroup();

    ImGui::End();
}

} // namespace dx12e
