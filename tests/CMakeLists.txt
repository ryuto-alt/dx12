# 単体テスト。
#
# このディレクトリは2通りで使える:
#  1) ルート CMakeLists から add_subdirectory(tests) 経由 (エンジンと一緒にビルド)
#  2) スタンドアロン: cmake -S tests -B build-tests
#     -> vcpkg も VS2026 も不要。依存ゼロなので CI(GitHub Actions)が緑になる。
cmake_minimum_required(VERSION 3.20)

# スタンドアロン構成時のみ project() を宣言 (サブディレクトリ取り込み時は親に従う)
if(NOT DEFINED PROJECT_NAME OR PROJECT_NAME STREQUAL "")
    project(DX12EngineTests LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

add_executable(IndexAllocatorTests test_index_allocator.cpp)

# core/IndexAllocator.h, core/Types.h を解決するため src/ をインクルードパスへ
target_include_directories(IndexAllocatorTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

if(MSVC)
    target_compile_options(IndexAllocatorTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(IndexAllocatorTests PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME IndexAllocatorTests COMMAND IndexAllocatorTests)

# NetWriter/NetReader 往復テスト（GPU非依存・ヘッダオンリー、vcpkg/entt不要なのでスタンドアロンでも動く）。
add_executable(NetBufferTests net_buffer_test.cpp)
target_include_directories(NetBufferTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(NetBufferTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(NetBufferTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME NetBufferTests COMMAND NetBufferTests)

# クラスタードライティング（Forward+）の Z 指数分割とカリング判定のテスト。
# renderer/ClusterMath.h はヘッダオンリー・GPU/DirectXMath 非依存なのでスタンドアロンでも動く。
# HLSL 側（shaders/forward/ClusterCommon.hlsli）は同じ式を手で写しているので、
# 片方を直したら必ず両方直すこと。
add_executable(ClusterMathTests cluster_math_test.cpp)
target_include_directories(ClusterMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(ClusterMathTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(ClusterMathTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME ClusterMathTests COMMAND ClusterMathTests)

# UIText リッチテキスト(インラインタグ)パーサのテスト(ヘッダオンリー・依存ゼロなのでスタンドアロンでも動く)。
add_executable(UiRichTextTests ui_rich_text_test.cpp)
target_include_directories(UiRichTextTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(UiRichTextTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(UiRichTextTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME UiRichTextTests COMMAND UiRichTextTests)

# Sprite2D フリップブック/UVスクロールの UV 計算(renderer/SpriteAnim.h)テスト。
# ヘッダオンリー・依存ゼロなのでスタンドアロンでも動く。
add_executable(SpriteAnimTests sprite_anim_test.cpp)
target_include_directories(SpriteAnimTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(SpriteAnimTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(SpriteAnimTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME SpriteAnimTests COMMAND SpriteAnimTests)

# ブレンドツリーの重み計算(animation/BlendTree.h)テスト。
# 1D 区間線形補間 + 2D Gradient Band（Johansen 論文 §6.3/§6.3.1）。
# ヘッダオンリー・依存ゼロなのでスタンドアロン構成でも動く。
add_executable(BlendTreeTests blend_tree_test.cpp)
target_include_directories(BlendTreeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(BlendTreeTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(BlendTreeTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME BlendTreeTests COMMAND BlendTreeTests)

# UIアニメクリップ(animation/UiAnimAsset.h)の評価テスト。
# 評価とイージングは純関数(nlohmann も entt も要らない)なのでスタンドアロンでも動く。
add_executable(UiAnimTests ui_anim_test.cpp)
target_include_directories(UiAnimTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(UiAnimTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(UiAnimTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME UiAnimTests COMMAND UiAnimTests)

# スプライトシート(animation/SpriteAnimAsset.h)の任意セル順/可変フレーム長の畳み込みテスト。
# こちらも純関数・依存ゼロ。
add_executable(SpriteAnimAssetTests sprite_anim_asset_test.cpp)
target_include_directories(SpriteAnimAssetTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(MSVC)
    target_compile_options(SpriteAnimAssetTests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(SpriteAnimAssetTests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME SpriteAnimAssetTests COMMAND SpriteAnimAssetTests)

# OGG Vorbis デコード(third_party/stb_vorbis.c、AudioClip .ogg 対応の土台)テスト。
# 埋め込み OGG(ogg_test_data.h)をメモリからデコードして PCM 長を確認する。依存ゼロ。
# stb_vorbis はサードパーティ実装をそのまま取り込むため、警告のエラー化はしない。
add_executable(OggDecodeTests ogg_decode_test.cpp)
target_include_directories(OggDecodeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
if(MSVC)
    target_compile_options(OggDecodeTests PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(OggDecodeTests PRIVATE -Wall -Wextra)
endif()
add_test(NAME OggDecodeTests COMMAND OggDecodeTests)

# シリアライズ往復テスト（安全網）。Scene ライブラリ（+ 推移依存）をリンクするため、
# エンジン本体と一緒にビルドする構成でのみ有効。
# tests を単体（cmake -S tests, vcpkg/VS不要）で構成したときは Scene が無いのでスキップし、
# 依存ゼロの CI を壊さない。
if(TARGET Scene)
    add_executable(SerializeRoundtripTests serialize_roundtrip_test.cpp)

    # Scene が PUBLIC で src/ インクルードと全依存（entt/nlohmann/DirectXMath/...）を伝播する。
    target_link_libraries(SerializeRoundtripTests PRIVATE Scene)
    # 封印ランタイムの中立公開ヘッダ（include/engine/...）。
    target_include_directories(SerializeRoundtripTests PRIVATE ${CMAKE_SOURCE_DIR}/include)

    if(MSVC)
        # エンジン本体と同一のフラグ（/external:W0 で外部ヘッダ警告を /WX から除外）。
        target_compile_options(SerializeRoundtripTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SerializeRoundtripTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()

    add_test(NAME SerializeRoundtripTests COMMAND SerializeRoundtripTests)
endif()

# .dxmat(マテリアルアセット)JSON往復テスト。Resource(GPU非依存の純ロジック部分のみ使用)をリンクするため、
# エンジン本体と一緒にビルドする構成でのみ有効(スタンドアロン構成では Resource が無いのでスキップ)。
if(TARGET Resource)
    add_executable(DxmatRoundtripTests dxmat_roundtrip_test.cpp)
    target_link_libraries(DxmatRoundtripTests PRIVATE Resource)
    if(MSVC)
        target_compile_options(DxmatRoundtripTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(DxmatRoundtripTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME DxmatRoundtripTests COMMAND DxmatRoundtripTests)
endif()

# BC7/BC5 テクスチャ圧縮の形式選択 + DirectXTex の圧縮/伸長往復テスト。
# Resource が Microsoft::DirectXTex を PUBLIC でリンクしているので、GPU 無しで CPU コーデックを叩ける。
if(TARGET Resource)
    add_executable(TextureCompressTests texture_compress_test.cpp)
    target_link_libraries(TextureCompressTests PRIVATE Resource)
    if(MSVC)
        target_compile_options(TextureCompressTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(TextureCompressTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME TextureCompressTests COMMAND TextureCompressTests)
endif()

# entt::meta コア部品フィールド反映テスト（Phase 2 土台）。ECS（+ entt/DirectXMath）をリンク。
if(TARGET ECS)
    add_executable(ComponentMetaTests component_meta_test.cpp)
    target_link_libraries(ComponentMetaTests PRIVATE ECS)
    if(MSVC)
        target_compile_options(ComponentMetaTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ComponentMetaTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ComponentMetaTests COMMAND ComponentMetaTests)
endif()

# Camera 投影行列（透視/正射）テスト（Phase 4: ortho 能力）。
# Camera.cpp は純 DirectXMath（GPU非依存）なので直接ビルドして検証する。
# 完全ビルド時のみ（directxmath が要るため Renderer ターゲットの有無で判定）。
if(TARGET Renderer)
    find_package(directxmath CONFIG REQUIRED)
    add_executable(CameraProjectionTests
        camera_projection_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/renderer/Camera.cpp)
    target_include_directories(CameraProjectionTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(CameraProjectionTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(CameraProjectionTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(CameraProjectionTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME CameraProjectionTests COMMAND CameraProjectionTests)

    # 速度バッファ（モーションベクター）と TAA ジッタの数式テスト。
    # 「静止時に速度が 0」「ジッタは NDC の平行移動」を守る。
    # renderer/TaaJitter.h はヘッダオンリー・GPU 非依存なので .cpp のビルドは不要。
    add_executable(VelocityMathTests velocity_math_test.cpp)
    target_include_directories(VelocityMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(VelocityMathTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(VelocityMathTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(VelocityMathTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME VelocityMathTests COMMAND VelocityMathTests)

    # AnimationClip::NormalizeToSeconds（ticks→秒正規化。glTF=1000ticks/s 由来の
    # ポーズ凍結/スロー再生の再発防止）テスト。AnimationClip.cpp は純ロジックなので直接ビルド。
    add_executable(AnimClipNormalizeTests
        anim_clip_normalize_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimationClip.cpp)
    target_include_directories(AnimClipNormalizeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(AnimClipNormalizeTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(AnimClipNormalizeTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(AnimClipNormalizeTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AnimClipNormalizeTests COMMAND AnimClipNormalizeTests)

    # Animator の再生速度倍率(SetSpeed。speed=2.0 で進行2倍、0 で一時停止)テスト。
    # Animator/Skeleton/AnimationClip は純ロジック(GPU非依存)なので直接ビルド。
    add_executable(AnimatorSpeedTests
        animator_speed_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/Animator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimPose.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimationClip.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/Skeleton.cpp)
    target_include_directories(AnimatorSpeedTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(AnimatorSpeedTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(AnimatorSpeedTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(AnimatorSpeedTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AnimatorSpeedTests COMMAND AnimatorSpeedTests)

    # ポーズ空間ブレンド（animation/AnimPose.h）テスト。クロスフェードのバグ B1
    # 「最終スキニング行列を要素ごとに lerp すると骨が縮む」の回帰テストが本体。
    # AnimPose/Animator/Skeleton/AnimationClip は純ロジック（GPU非依存）なので直接ビルド。
    add_executable(AnimPoseTests
        anim_pose_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimPose.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/Animator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimationClip.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/Skeleton.cpp)
    target_include_directories(AnimPoseTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(AnimPoseTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(AnimPoseTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(AnimPoseTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AnimPoseTests COMMAND AnimPoseTests)

    # アニメイベント収集（animation/AnimEvent.h）+ ステートマシン（AnimGraphAsset.h）テスト。
    # どちらも純関数（GPU/entt/Jolt 非依存）。AnimationClip.cpp だけ実体が要る。
    add_executable(AnimGraphTests
        anim_graph_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimationClip.cpp)
    target_include_directories(AnimGraphTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(AnimGraphTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(AnimGraphTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(AnimGraphTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME AnimGraphTests COMMAND AnimGraphTests)

    # ボーンマスク（animation/BoneMask.h）+ レイヤー合成テスト。
    # 「マスク外のボーンが下位レイヤーとビット単位で同一」を守る。
    add_executable(BoneMaskTests
        bone_mask_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimPose.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/AnimationClip.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animation/Skeleton.cpp)
    target_include_directories(BoneMaskTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(BoneMaskTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(BoneMaskTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(BoneMaskTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME BoneMaskTests COMMAND BoneMaskTests)

    # 2 ボーン解析 IK（animation/TwoBoneIK.h）テスト。フット IK の心臓部。
    # 「解いたあと FK し直すと足首が目標に乗る」「ボーン長が保存される」を数値で守る。
    # ヘッダオンリー・純 DirectXMath。
    add_executable(TwoBoneIKTests two_bone_ik_test.cpp)
    target_include_directories(TwoBoneIKTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(TwoBoneIKTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(TwoBoneIKTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(TwoBoneIKTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME TwoBoneIKTests COMMAND TwoBoneIKTests)

    # Frustum（視錐台抽出＋球カリング）テスト。ヘッダオンリー・純 DirectXMath。
    add_executable(FrustumTests frustum_test.cpp)
    target_include_directories(FrustumTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(FrustumTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(FrustumTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(FrustumTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME FrustumTests COMMAND FrustumTests)

    # 精密ピッキングの土台（editor/RayGeometry.h: レイ-三角形/AABB/球 + ヒット順ソート）テスト。
    # ヘッダオンリー・純 DirectXMath（entt も GPU も要らない）。
    add_executable(RayPickTests ray_pick_test.cpp)
    target_include_directories(RayPickTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(RayPickTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(RayPickTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(RayPickTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME RayPickTests COMMAND RayPickTests)

    # SnapshotBuffer(ネットワーク補間バッファ)テスト。ヘッダオンリー・純DirectXMath。
    add_executable(InterpolationTests interpolation_test.cpp)
    target_include_directories(InterpolationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(InterpolationTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(InterpolationTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(InterpolationTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME InterpolationTests COMMAND InterpolationTests)

    # RpcValue/RpcArgs(net:rpc引数)シリアライズ往復テスト。ヘッダオンリー・純DirectXMath。
    add_executable(RpcArgsTests rpc_args_test.cpp)
    target_include_directories(RpcArgsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(RpcArgsTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(RpcArgsTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(RpcArgsTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME RpcArgsTests COMMAND RpcArgsTests)

    # IsRelevant(興味管理の距離判定)テスト。ヘッダオンリー・純DirectXMath。
    add_executable(InterestFilterTests interest_filter_test.cpp)
    target_include_directories(InterestFilterTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(InterestFilterTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(InterestFilterTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(InterestFilterTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME InterestFilterTests COMMAND InterestFilterTests)
endif()

# 地形（ハイトフィールドのサンプリング / スカルプトブラシ / .hf 往復 / レイキャスト）テスト。
# HeightField.cpp / TerrainBrush.cpp は純ロジック（GPU も entt も vfs も不要）なので
# Terrain ライブラリをリンクせず直接ビルドする（CameraProjectionTests と同じ流儀）。
if(TARGET Renderer)
    find_package(directxmath CONFIG REQUIRED)
    add_executable(TerrainTests
        terrain_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/terrain/HeightField.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/terrain/TerrainBrush.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/terrain/TerrainSplatMap.cpp)
    target_include_directories(TerrainTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(TerrainTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(TerrainTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(TerrainTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME TerrainTests COMMAND TerrainTests)
endif()

# 頂点スカルプト（素体生成 / 溶接・隣接 / 各ブラシ / 法線の部分再計算 / 対称 / .smsh 往復）テスト。
# SculptMesh.cpp も純ロジック（重み関数とノイズだけ TerrainBrush と共有）なので、
# TerrainTests と同じくライブラリをリンクせず直接ビルドする。
if(TARGET Renderer)
    find_package(directxmath CONFIG REQUIRED)
    add_executable(SculptTests
        sculpt_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/terrain/SculptMesh.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/terrain/TerrainBrush.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/terrain/HeightField.cpp)
    target_include_directories(SculptTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(SculptTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(SculptTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SculptTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME SculptTests COMMAND SculptTests)
endif()

# SpatialIndex（一様グリッド）テスト（Phase 5: RTS 空間クエリ）。ヘッダオンリー、entt のみ依存。
if(TARGET ECS)
    find_package(EnTT CONFIG REQUIRED)
    add_executable(SpatialIndexTests spatial_index_test.cpp)
    target_include_directories(SpatialIndexTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(SpatialIndexTests PRIVATE EnTT::EnTT)
    if(MSVC)
        target_compile_options(SpatialIndexTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(SpatialIndexTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME SpatialIndexTests COMMAND SpatialIndexTests)
endif()

# EventBus（汎用イベントバス）テスト（Phase 3 中核）。ヘッダオンリー、entt のみ依存。
if(TARGET ECS)
    find_package(EnTT CONFIG REQUIRED)
    add_executable(EventBusTests event_bus_test.cpp)
    target_include_directories(EventBusTests PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(EventBusTests PRIVATE EnTT::EnTT)
    if(MSVC)
        target_compile_options(EventBusTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(EventBusTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME EventBusTests COMMAND EventBusTests)
endif()

# ActionMap（データ駆動入力）テスト（Phase 4 能力カタログ）。ヘッダオンリー、DirectXMath のみ依存。
if(TARGET Renderer)
    find_package(directxmath CONFIG REQUIRED)
    add_executable(ActionMapTests action_map_test.cpp)
    target_include_directories(ActionMapTests PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(ActionMapTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(ActionMapTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ActionMapTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ActionMapTests COMMAND ActionMapTests)
endif()

# VFS（Crypto / Compression / Pak）往復テスト（GPU 非依存）。Core（+ bcrypt/cabinet）をリンク。
# Core は STATIC なので PRIVATE 依存（bcrypt.lib/Cabinet.lib）も実行ファイルへ伝播する。
if(TARGET Core)
    add_executable(VfsTests vfs_test.cpp)
    target_link_libraries(VfsTests PRIVATE Core)
    target_include_directories(VfsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(VfsTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(VfsTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME VfsTests COMMAND VfsTests)
endif()

# CrashHandler 統合テスト（子プロセスをわざとアクセス違反で落とし、
# dx12_crash.log / dx12_crash.dmp の生成とレポート内容を確認）。
if(TARGET Core)
    add_executable(CrashHandlerTests crash_handler_test.cpp)
    target_link_libraries(CrashHandlerTests PRIVATE Core)
    target_include_directories(CrashHandlerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    if(MSVC)
        target_compile_options(CrashHandlerTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(CrashHandlerTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME CrashHandlerTests COMMAND CrashHandlerTests)
endif()

# GitIntegration::ToWebUrl（remote URL → ブラウザ用 https URL）の文字列変換テスト。
# プレフィックス文字数のオフバイワン再発防止（実際に "ryuto-alt" → "yuto-alt" になった事例）。
if(TARGET ProjectLib)
    add_executable(GitIntegrationTests git_integration_test.cpp)
    target_link_libraries(GitIntegrationTests PRIVATE ProjectLib)
    if(MSVC)
        target_compile_options(GitIntegrationTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(GitIntegrationTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME GitIntegrationTests COMMAND GitIntegrationTests)
endif()

# ENet ループバック疎通テスト（同一プロセス内でサーバー/クライアントを立てて往復確認）。
# Network（+ vcpkg unofficial-enet, Core）が必要なので統合ビルドでのみ有効。
if(TARGET Network)
    add_executable(EnetLoopbackTests enet_loopback_test.cpp)
    target_link_libraries(EnetLoopbackTests PRIVATE Network)
    if(MSVC)
        target_compile_options(EnetLoopbackTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(EnetLoopbackTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME EnetLoopbackTests COMMAND EnetLoopbackTests)
endif()

# editor/LightMath.h（太陽の方位/高度 ⇔ direction、時刻カーブ、色温度、コーン角、
# ワールド→スクリーン投影、レイ幾何）のテスト。シーンビューのライト直接操作の心臓部。
# ヘッダオンリー・純 DirectXMath（ImGui も entt も要らない）。
if(TARGET Renderer)
    find_package(directxmath CONFIG REQUIRED)
    add_executable(LightMathTests light_math_test.cpp)
    target_include_directories(LightMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(LightMathTests PRIVATE Microsoft::DirectXMath)
    if(MSVC)
        target_compile_options(LightMathTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(LightMathTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME LightMathTests COMMAND LightMathTests)
endif()

# モデルのインポートサイズ回帰テスト（FBX の cm→m 正規化）。ModelLoader::Probe は GPU を
# 触らないが Resource ライブラリ(assimp)が要るので統合ビルドでのみ有効。
if(TARGET Resource AND TARGET Renderer)
    add_executable(ModelScaleTests model_scale_test.cpp)
    target_link_libraries(ModelScaleTests PRIVATE Resource Renderer)
    target_compile_definitions(ModelScaleTests PRIVATE
        DX12E_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    if(MSVC)
        target_compile_options(ModelScaleTests PRIVATE ${DX12_COMPILE_OPTIONS})
        target_compile_definitions(ModelScaleTests PRIVATE ${DX12_COMPILE_DEFS})
    endif()
    add_test(NAME ModelScaleTests COMMAND ModelScaleTests)
endif()

# MCP ディスパッチ表（mcp/ApplicationMcp*.cpp の McpDefine）の「申告キー表 ⊇ 本文が読むキー」を
# 機械的に見張るテスト。dx12_describe_mcp_params が嘘をつかないことの保証（#20-7 / #30）。
# エンジンをリンクせずソースをテキストとして読むだけ。依存ゼロ。
# ★登録はテーマ別 TU に散っているので、ここは '|' 区切りの一覧を渡す（増えたら足すこと）。
set(DX12E_MCP_SOURCES "")
foreach(_mcp Entity Editor Render Tooling Asset Terrain Lighting)
    set(DX12E_MCP_SOURCES
        "${DX12E_MCP_SOURCES}${CMAKE_CURRENT_SOURCE_DIR}/../src/core/mcp/ApplicationMcp${_mcp}.cpp|")
endforeach()
add_executable(McpParamSpecTests mcp_param_spec_test.cpp)
target_compile_definitions(McpParamSpecTests PRIVATE
    DX12E_APPLICATION_CPP="${DX12E_MCP_SOURCES}${CMAKE_CURRENT_SOURCE_DIR}/../src/core/mcp/ApplicationMcp.cpp")
if(MSVC)
    target_compile_options(McpParamSpecTests PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(McpParamSpecTests PRIVATE ${DX12_COMPILE_DEFS})
endif()
add_test(NAME McpParamSpecTests COMMAND McpParamSpecTests)
