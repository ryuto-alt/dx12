add_library(Renderer STATIC
    Mesh.cpp
    Camera.cpp
    Material.cpp
    GraphMaterialSystem.cpp   # マテリアルグラフ G2b: グラフ材質のランタイム（パラメータプール・非同期 DXC + PSO・ホットリロード）
    GraphPreviewRenderer.cpp  # マテリアルグラフ G2c: エディタのライブプレビュー + ノード内サムネイルの描画（エディタ専用）
    PostProcess.cpp
    ScreenShaderPass.cpp
    BloomPass.cpp
    AutoExposurePass.cpp
    LinearCapturePass.cpp     # Q2: 線形 HDR スクリーンショットの読み出し（要求のあるフレームだけ走る compute）
    GodRaysPass.cpp
    LensFlarePass.cpp
    DofPass.cpp
    MotionBlurPass.cpp
    SSAOPass.cpp
    ContactShadowPass.cpp
    TaaPass.cpp
    RenderDebugPass.cpp
    ScreenSpaceGiPass.cpp
    AccelerationStructure.cpp
    RaytracingScene.cpp
    RtScreenPass.cpp
    SkinningCompute.cpp
    DdgiVolume.cpp
    ClusteredLightCulling.cpp
    HiZPass.cpp
    OcclusionCullPass.cpp
    VolumetricFogPass.cpp
    DecalSystem.cpp
    SpriteRenderer.cpp
    SceneTransition.cpp
    ParticleSystem.cpp
    GpuParticleSystem.cpp
    IBLBaker.cpp
    SkyboxRenderer.cpp
    ViewPasses.cpp          # RenderView の段を IRenderPass（RenderPass.h の状態の契約）として切り出したもの
    PerceptionPass.cpp      # 知覚層（dx12_perceive）の ID パス + 読み戻し
    PerceptionStats.cpp     # 知覚層の集計（GPU 非依存の純関数。tests/perception_stats_test.cpp）
    vg/VirtualGeometrySystem.cpp   # 仮想ジオメトリ P2: .vgeo の読込（COPY キュー）+ GPU カリング（専用 RS・専用ヒープ。描かない）
    PathTracer.cpp                 # DXR パストレーサー(地上真値レンダラ。専用 RS・専用 TLAS。既定 OFF・遅延確保)
    pt/PtSceneBuilder.cpp          # パストレーサー専用の BLAS / TLAS / テーブルの構築
    atmosphere/AtmosphereRenderer.cpp   # 物理ベース大気 A1（Hillaire 2020 の 4 LUT + 空 + AP 合成。専用 RS）
)
target_include_directories(Renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(directxmath CONFIG REQUIRED)
# meshoptimizer はルート CMakeLists で find_package 済み（POST_BUILD DLL 同梱のため）。
# Resource は ShaderCompiler(.cso ローダ) のために必要。Resource は Renderer に依存しないのでサイクルにならない。
target_link_libraries(Renderer PUBLIC Graphics Microsoft::DirectXMath Resource meshoptimizer::meshoptimizer)

target_compile_options(Renderer PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Renderer PRIVATE ${DX12_COMPILE_DEFS})

# 仮想ジオメトリ(Nanite 風) P0: スタブ .vgeo 生成 CLI（tools/vgeo_stub。EXCLUDE_FROM_ALL = 既定ビルドに入らない）。
# 実装は src/renderer/vg/ のヘッダオンリー（VgeoFormat.h ほか）。ターゲットが要るのは CLI とテストだけで、Renderer には何も足さない。
if(EXISTS ${PROJECT_SOURCE_DIR}/tools/vgeo_stub/CMakeLists.txt)
    add_subdirectory(${PROJECT_SOURCE_DIR}/tools/vgeo_stub ${PROJECT_BINARY_DIR}/tools/vgeo_stub)
endif()
# 仮想ジオメトリ P1: オフライン cooker（tools/vgeo_cook。CLI は EXCLUDE_FROM_ALL、ライブラリ VgeoCookLib はテストのビルドで建つ）。Renderer には何も足さない。
if(EXISTS ${PROJECT_SOURCE_DIR}/tools/vgeo_cook/CMakeLists.txt)
    add_subdirectory(${PROJECT_SOURCE_DIR}/tools/vgeo_cook ${PROJECT_BINARY_DIR}/tools/vgeo_cook)
endif()

# マテリアルグラフ G1: グラフのデータモデル・型システム・HLSL コード生成・CPU 評価・.dxmg 入出力。
# 標準ライブラリ + nlohmann-json だけの純ロジック（GPU / D3D / ImGui 非依存）。tests/matgraph_*.cpp が直接リンクする。
# 設計は docs/MATERIAL_GRAPH_DESIGN.md、仕様は docs/MATGRAPH_FORMAT.md。統合（G2b）で Renderer / Editor がリンクする。
find_package(nlohmann_json CONFIG REQUIRED)
add_library(MatGraph STATIC
    matgraph/MatGraphTypes.cpp
    matgraph/NodeLibrary.cpp
    matgraph/BuiltinNodes.cpp
    matgraph/GraphModel.cpp
    matgraph/GraphAnalysis.cpp
    matgraph/Compiler.cpp
    matgraph/CpuEval.cpp
    matgraph/GraphIO.cpp
    matgraph/PbrTemplate.cpp      # G2b: 従来 PBR 材質と等価な標準テンプレートグラフ（グラフ化）
    matgraph/ShaderCache.cpp      # G2b: DXIL のディスクキャッシュ / include ハッシュ / キー
)
target_include_directories(MatGraph PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(MatGraph PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(MatGraph PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(MatGraph PRIVATE ${DX12_COMPILE_DEFS})

# 植生 F1（FoliageLayer + GPU カリング + ExecuteIndirect + 風）。設計は docs/GRAPHICS_PARITY_DESIGN.md §2.3、仕様は docs/FOLIAGE_F1.md。
# 上の add_library のリストへ挿し込まず target_sources で足しているのは、同じファイルを同時に触る作業とぶつからないようにするため。
target_sources(Renderer PRIVATE
    foliage/FoliageScatter.cpp     # ポアソンディスク散布 / ブラシ（純ロジック）
    foliage/FoliageIO.cpp          # .dxfoliage の入出力（CRC 付きバイナリ）
    foliage/FoliageLayerOps.cpp    # FoliageLayer の読込 / 保存 / モデル指定の分解 / カリング定数
    foliage/FoliageCuller.cpp      # GPU カリング compute（専用 RS。D3D12 デバイスだけで動く）
    foliage/FoliageSystem.cpp      # レイヤー管理 + ExecuteIndirect 描画（本体 / 深度 / 影 / 速度）
)
# GPU 駆動のインスタンス群（設計 docs/SCENE_FORMAT_DESIGN.md §4.2 = 4-3）。永続バッファ + チャンク → インスタンスの GPU カリング + LOD + 間接引数（専用 RS）。
target_sources(Renderer PRIVATE
    instancegpu/InstanceGpuCuller.cpp
)
# FoliageSystem::StatsJson が nlohmann::json を返す
target_link_libraries(Renderer PUBLIC nlohmann_json::nlohmann_json)

# 水面 W1（WaterBody + 専用パス。専用ルートシグネチャ・クリップマップ風リング・屈折 / 吸収 / 反射 / 泡）。設計は docs/GRAPHICS_PARITY_DESIGN.md §2.2。
target_sources(Renderer PRIVATE
    water/WaterRenderer.cpp        # 水域の収集 / カリング / 色・深度のコピー / 水中 / 水面メッシュ（専用 RS）
)
