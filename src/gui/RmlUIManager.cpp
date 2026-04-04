#include "gui/RmlUIManager.h"

#include <Windows.h>

#include <RmlUi/Core.h>

#include "core/Logger.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "gui/RmlSystem.h"
#include "gui/RmlRenderer.h"

namespace dx12e
{

// =============================================================================
// Ctor / Dtor
// =============================================================================

RmlUIManager::RmlUIManager()  = default;
RmlUIManager::~RmlUIManager() = default;

// =============================================================================
// Initialize
// =============================================================================

void RmlUIManager::Initialize(GraphicsDevice&      device,
                               ID3D12CommandQueue*  commandQueue,
                               DescriptorHeap&      srvHeap,
                               const GameClock&     clock,
                               DXGI_FORMAT          rtvFormat,
                               const std::wstring&  shaderDir,
                               u32                  viewportWidth,
                               u32                  viewportHeight)
{
    // 1. Create system interface
    m_system = std::make_unique<RmlSystem>(clock);
    Rml::SetSystemInterface(m_system.get());

    // 2. Create and initialize render interface
    m_renderer = std::make_unique<RmlRenderer>();
    m_renderer->Initialize(device, srvHeap, commandQueue, rtvFormat, shaderDir);
    Rml::SetRenderInterface(m_renderer.get());

    // 3. Initialize RmlUi core
    if (!Rml::Initialise())
    {
        Logger::Error("RmlUIManager: Rml::Initialise() failed");
        return;
    }

    // 4. Load fonts (meiryo for Japanese, segoeui as fallback)
    // Load fonts - try multiple options
    bool fontLoaded = false;
    const char* fontPaths[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/meiryo.ttc",
        "C:/Windows/Fonts/YuGothM.ttc",
    };
    for (const char* fontPath : fontPaths)
    {
        bool ok = Rml::LoadFontFace(fontPath, false);
        char buf[256];
        snprintf(buf, sizeof(buf), "[RmlUi] LoadFontFace('%s') = %s\n",
                 fontPath, ok ? "OK" : "FAILED");
        OutputDebugStringA(buf);
        if (ok) fontLoaded = true;
    }
    if (!fontLoaded)
    {
        OutputDebugStringA("[RmlUi] WARNING: No fonts loaded!\n");
    }

    // 5. Create context
    m_context = Rml::CreateContext(
        "game",
        Rml::Vector2i(static_cast<int>(viewportWidth),
                      static_cast<int>(viewportHeight)));

    if (!m_context)
    {
        Logger::Error("RmlUIManager: Rml::CreateContext() failed");
        Rml::Shutdown();
        return;
    }

    m_initialized = true;
    Logger::Info("RmlUIManager: initialized ({}x{})", viewportWidth, viewportHeight);
}

// =============================================================================
// Shutdown
// =============================================================================

void RmlUIManager::Shutdown()
{
    if (!m_initialized)
        return;

    if (m_context)
    {
        Rml::RemoveContext(m_context->GetName());
        m_context = nullptr;
    }

    Rml::Shutdown();

    m_renderer.reset();
    m_system.reset();

    m_initialized = false;
    Logger::Info("RmlUIManager: shutdown");
}

// =============================================================================
// Document management
// =============================================================================

bool RmlUIManager::LoadDocument(const std::string& path)
{
    if (!m_context)
        return false;

    Rml::ElementDocument* doc = m_context->LoadDocument(path);
    if (!doc)
    {
        Logger::Error("RmlUIManager: LoadDocument failed for '{}'", path);
        return false;
    }

    doc->Show();
    Logger::Info("RmlUIManager: loaded document '{}'", path);
    return true;
}

void RmlUIManager::CloseDocument(const std::string& id)
{
    if (!m_context)
        return;

    Rml::ElementDocument* doc = m_context->GetDocument(id);
    if (doc)
    {
        doc->Close();
    }
}

// =============================================================================
// DOM manipulation helpers
// =============================================================================

Rml::Element* RmlUIManager::FindElement(const std::string& elementId)
{
    if (!m_context)
        return nullptr;

    const int docCount = m_context->GetNumDocuments();
    for (int i = 0; i < docCount; ++i)
    {
        Rml::ElementDocument* doc = m_context->GetDocument(i);
        if (!doc)
            continue;

        Rml::Element* elem = doc->GetElementById(elementId);
        if (elem)
            return elem;
    }
    return nullptr;
}

void RmlUIManager::SetText(const std::string& elementId, const std::string& text)
{
    Rml::Element* elem = FindElement(elementId);
    if (elem)
    {
        elem->SetInnerRML(text);
    }
}

void RmlUIManager::SetProperty(const std::string& elementId,
                                const std::string& property,
                                const std::string& value)
{
    Rml::Element* elem = FindElement(elementId);
    if (elem)
    {
        elem->SetProperty(property, value);
    }
}

void RmlUIManager::ShowElement(const std::string& elementId)
{
    Rml::Element* elem = FindElement(elementId);
    if (elem)
    {
        elem->SetProperty("display", "block");
    }
}

void RmlUIManager::HideElement(const std::string& elementId)
{
    Rml::Element* elem = FindElement(elementId);
    if (elem)
    {
        elem->SetProperty("display", "none");
    }
}

// =============================================================================
// Input forwarding
// =============================================================================

bool RmlUIManager::ProcessMouseMove(int x, int y)
{
    if (!m_context)
        return false;

    return m_context->ProcessMouseMove(x, y, GetKeyModifiers());
}

bool RmlUIManager::ProcessMouseButton(int buttonIndex, bool down)
{
    if (!m_context)
        return false;

    if (down)
        return m_context->ProcessMouseButtonDown(buttonIndex, GetKeyModifiers());
    else
        return m_context->ProcessMouseButtonUp(buttonIndex, GetKeyModifiers());
}

bool RmlUIManager::ProcessMouseWheel(float delta)
{
    if (!m_context)
        return false;

    // Positive delta = scroll up (negative Y scroll in RmlUi convention)
    return m_context->ProcessMouseWheel(
        Rml::Vector2f(0.0f, -delta),
        GetKeyModifiers());
}

bool RmlUIManager::ProcessKeyDown(int vkCode)
{
    if (!m_context)
        return false;

    const int rmlKey = ConvertVirtualKey(vkCode);
    return m_context->ProcessKeyDown(
        static_cast<Rml::Input::KeyIdentifier>(rmlKey),
        GetKeyModifiers());
}

bool RmlUIManager::ProcessKeyUp(int vkCode)
{
    if (!m_context)
        return false;

    const int rmlKey = ConvertVirtualKey(vkCode);
    return m_context->ProcessKeyUp(
        static_cast<Rml::Input::KeyIdentifier>(rmlKey),
        GetKeyModifiers());
}

bool RmlUIManager::ProcessTextInput(wchar_t character)
{
    if (!m_context)
        return false;

    return m_context->ProcessTextInput(static_cast<Rml::Character>(character));
}

// =============================================================================
// Frame lifecycle
// =============================================================================

void RmlUIManager::Update()
{
    if (!m_context)
        return;

    m_context->Update();
}

void RmlUIManager::Render(ID3D12GraphicsCommandList* cmdList,
                           ID3D12DescriptorHeap*      /*srvHeap*/,
                           u32                        viewportWidth,
                           u32                        viewportHeight)
{
    if (!m_context)
        return;

    m_renderer->BeginFrame(cmdList,
                            static_cast<f32>(viewportWidth),
                            static_cast<f32>(viewportHeight));
    m_context->Render();
}

void RmlUIManager::OnResize(u32 width, u32 height)
{
    if (!m_context)
        return;

    m_context->SetDimensions(
        Rml::Vector2i(static_cast<int>(width),
                      static_cast<int>(height)));
}

// =============================================================================
// Private helpers
// =============================================================================

int RmlUIManager::GetKeyModifiers()
{
    int modifiers = 0;

    if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        modifiers |= Rml::Input::KM_CTRL;

    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        modifiers |= Rml::Input::KM_SHIFT;

    if (GetAsyncKeyState(VK_MENU) & 0x8000)
        modifiers |= Rml::Input::KM_ALT;

    return modifiers;
}

int RmlUIManager::ConvertVirtualKey(int vkCode)
{
    using K = Rml::Input::KeyIdentifier;

    switch (vkCode)
    {
    // --- control / whitespace ---
    case VK_BACK:   return K::KI_BACK;
    case VK_TAB:    return K::KI_TAB;
    case VK_RETURN: return K::KI_RETURN;
    case VK_ESCAPE: return K::KI_ESCAPE;
    case VK_SPACE:  return K::KI_SPACE;

    // --- navigation ---
    case VK_PRIOR:  return K::KI_PRIOR;    // Page Up
    case VK_NEXT:   return K::KI_NEXT;     // Page Down
    case VK_END:    return K::KI_END;
    case VK_HOME:   return K::KI_HOME;
    case VK_LEFT:   return K::KI_LEFT;
    case VK_UP:     return K::KI_UP;
    case VK_RIGHT:  return K::KI_RIGHT;
    case VK_DOWN:   return K::KI_DOWN;
    case VK_DELETE: return K::KI_DELETE;
    case VK_INSERT: return K::KI_INSERT;

    // --- modifier keys ---
    case VK_SHIFT:   return K::KI_LSHIFT;
    case VK_CONTROL: return K::KI_LCONTROL;
    case VK_MENU:    return K::KI_LMENU;

    // --- function keys F1-F12 ---
    case VK_F1:  return K::KI_F1;
    case VK_F2:  return K::KI_F2;
    case VK_F3:  return K::KI_F3;
    case VK_F4:  return K::KI_F4;
    case VK_F5:  return K::KI_F5;
    case VK_F6:  return K::KI_F6;
    case VK_F7:  return K::KI_F7;
    case VK_F8:  return K::KI_F8;
    case VK_F9:  return K::KI_F9;
    case VK_F10: return K::KI_F10;
    case VK_F11: return K::KI_F11;
    case VK_F12: return K::KI_F12;

    default: break;
    }

    // A-Z  (VK_A=0x41 … VK_Z=0x5A)
    if (vkCode >= 0x41 && vkCode <= 0x5A)
        return K::KI_A + (vkCode - 0x41);

    // 0-9  (VK_0=0x30 … VK_9=0x39)
    if (vkCode >= 0x30 && vkCode <= 0x39)
        return K::KI_0 + (vkCode - 0x30);

    return K::KI_UNKNOWN;
}

} // namespace dx12e
