#include "core/Application.h"

#include <Windows.h>
#include <string>

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
    try
    {
        bool gameMode = false;
        if (lpCmdLine)
        {
            std::string args(lpCmdLine);
            if (args.find("--game") != std::string::npos)
                gameMode = true;
        }

        dx12e::Application app;
        app.Initialize(hInstance, nCmdShow, gameMode);
        app.Run();
        app.Shutdown();
    }
    catch (const std::exception& e)
    {
        MessageBoxA(nullptr, e.what(), "Fatal Error", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
