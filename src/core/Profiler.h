#pragma once
// ---------------------------------------------------------------------------
// Tracy Profiler の薄いラッパー。
//
// DX12_TRACY はルート CMakeLists の Dx12Profiler(INTERFACE) が立てる。
// OFF のときは tracy のヘッダーもライブラリも一切参照しない＝配布ビルドに
// Tracy のコードは 1 バイトも入らない。
//
// 既存の CpuScopeTimer(core/CpuScope.h) は消さない。役割が違うため:
//   CpuScopeTimer … N フレーム平均の合計 ms を 8 スロットに畳んだ値。AI が
//                   dx12_perf_stats で読む「数値」。
//   Tracy        … 1 フレームの時系列を階層で見る「タイムライン」。人間が読む。
// なので同じ行に 2 行並べる運用にする（片方へ寄せる意味がない）。
//
// 使い方（計測ビルド）:
//   cmake --preset windows-tracy && cmake --build build/tracy
//   build/tracy/DX12Engine.exe を起動し、Tracy.exe から Connect
//   （vcpkg 側で on-demand フィーチャを有効にしているので、繋ぐまでのオーバーヘッドは
//     ほぼゼロ。ゾーンを張ったまま普段使いしてよい）
//
// 注意:
//  - <tracy/Tracy.hpp> は角括弧 include なので /external:anglebrackets + /external:W0 に
//    より外部ヘッダ警告が /WX と衝突しない。
//  - pch.h には入れない。DX12_TRACY の ON/OFF で PCH 全体が再構築されるため。
//  - TracyPlot 系のマクロはここに用意しない。OFF 時に値を捨てると、その値を計算する
//    ローカル変数が C4189(未参照)→ /WX でビルドエラーになる。必要になったら
//    #if defined(DX12_TRACY) のブロックで直接書く。
// ---------------------------------------------------------------------------

#if defined(DX12_TRACY)
    #include <tracy/Tracy.hpp>
    #define DX12_PROFILE_FRAME()             FrameMark
    #define DX12_PROFILE_ZONE()              ZoneScoped
    #define DX12_PROFILE_ZONE_N(name)        ZoneScopedN(name)
    #define DX12_PROFILE_THREAD(name)        tracy::SetThreadName(name)
#else
    #define DX12_PROFILE_FRAME()             ((void)0)
    #define DX12_PROFILE_ZONE()              ((void)0)
    #define DX12_PROFILE_ZONE_N(name)        ((void)0)
    #define DX12_PROFILE_THREAD(name)        ((void)0)
#endif
