#include "core/Application.h"
#include "core/PathResolver.h"

#include <Windows.h>
#include <string>
#include <fstream>
#include <filesystem>

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
    try
    {
        bool gameMode  = false;
        bool buildMode = false;
        if (lpCmdLine)
        {
            std::string args(lpCmdLine);
            if (args.find("--game") != std::string::npos)
                gameMode = true;
            if (args.find("--build") != std::string::npos)
                buildMode = true;
            if (args.find("--editor") != std::string::npos)
                gameMode = false;  // 明示的にエディタ起動
        }

        // 配布レイアウト判定: exe の隣に game.json があれば（引数無しの直起動でも）ゲームモード。
        // これでビルドした Game.exe をダブルクリックするとゲームが立ち上がる。
        if (!buildMode)
        {
            wchar_t buf[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            std::filesystem::path exeDir = std::filesystem::path(buf).parent_path();
            std::error_code ec;
            if (std::filesystem::exists(exeDir / "game.json", ec))
                gameMode = true;
        }

        // exe の場所を基準に assets/shaders/scripts のパスを確定（配布 exe を別フォルダで動かすため）
        dx12e::PathResolver::Initialize(gameMode);

        dx12e::Application app;
        app.Initialize(hInstance, nCmdShow, gameMode);

        if (buildMode)
        {
            // ヘッドレスでゲームをビルドして終了
            app.BuildGameStandalone();
            app.Shutdown();
            return EXIT_SUCCESS;
        }

        app.Run();
        app.Shutdown();
    }
    catch (const std::exception& e)
    {
        // GUI アプリはコンソールが無いので、致命的エラーをファイルにも残す
        {
            std::ofstream f("dx12_crash.log", std::ios::trunc);
            if (f) f << "Fatal Error: " << e.what() << "\n";
        }
        MessageBoxA(nullptr, e.what(), "Fatal Error", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
