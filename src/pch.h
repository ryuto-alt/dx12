#pragma once
// 全内部ターゲット共通のプリコンパイル済みヘッダー。
// CMake の target_precompile_headers(ルート CMakeLists の DX12_PCH ループ)が /FI で全 TU に
// 自動注入するので、ソース側で include を書く必要はない(既存の include はそのままで害なし)。
//
// ここに足してよいのは「多数の TU が使う & 変更頻度が極めて低い」ヘッダーだけ。
// エンジン自身のヘッダー(src/**)を入れると、その変更のたび全 PCH が再構築されるので入れない。

// --- C++ 標準ライブラリ ---
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// --- Windows / DirectX ---
// WIN32_LEAN_AND_MEAN / NOMINMAX / UNICODE は DX12_COMPILE_DEFS でターゲット定義済み。
#include <Windows.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#if __has_include(<directx/d3d12.h>)
    #include <directx/d3d12.h>
#endif

// --- サードパーティ(ヘッダーオンリー or 全ターゲット到達可能なもの) ---
#if __has_include(<spdlog/spdlog.h>)
    #include <spdlog/spdlog.h>
#endif
#if __has_include(<entt/entt.hpp>)
    #include <entt/entt.hpp>
#endif
#if __has_include(<nlohmann/json.hpp>)
    #include <nlohmann/json.hpp>
#endif
#if __has_include(<imgui.h>)
    // ImGuizmo.cpp / エディタパネルが ImVec2 演算子を使うため、PCH 側で先に定義してから
    // include する(TU 側の #define は imgui.h 取り込み後になるので効かない)。
    #ifndef IMGUI_DEFINE_MATH_OPERATORS
        #define IMGUI_DEFINE_MATH_OPERATORS
    #endif
    #include <imgui.h>
#endif
