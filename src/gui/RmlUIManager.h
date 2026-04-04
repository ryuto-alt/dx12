#pragma once

#include <memory>
#include <string>

#include "core/Types.h"

// Forward declarations - RmlUi types
namespace Rml
{
class Context;
class Element;
class ElementDocument;
} // namespace Rml

// DXGI_FORMAT needs the full typedef; pull in only the format header.
#include <dxgiformat.h>

// D3D12 forward declarations (COM interfaces)
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12DescriptorHeap;

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;
class GameClock;
class RmlSystem;
class RmlRenderer;

/// Orchestration class that ties RmlUi into the engine.
/// Manages initialization, font loading, document lifecycle, DOM manipulation,
/// input forwarding and frame rendering.
class RmlUIManager
{
public:
    RmlUIManager();
    ~RmlUIManager();

    RmlUIManager(const RmlUIManager&)            = delete;
    RmlUIManager& operator=(const RmlUIManager&) = delete;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    void Initialize(GraphicsDevice&      device,
                    ID3D12CommandQueue*  commandQueue,
                    DescriptorHeap&      srvHeap,
                    const GameClock&     clock,
                    DXGI_FORMAT          rtvFormat,
                    const std::wstring&  shaderDir,
                    u32                  viewportWidth,
                    u32                  viewportHeight);

    void Shutdown();

    // -------------------------------------------------------------------------
    // Document management
    // -------------------------------------------------------------------------

    /// Load and show an RML document.  Returns true on success.
    bool LoadDocument(const std::string& path);

    /// Close a document by its 'id' attribute value.
    void CloseDocument(const std::string& id);

    // -------------------------------------------------------------------------
    // DOM manipulation (called from Lua or game code)
    // -------------------------------------------------------------------------

    void SetText(const std::string& elementId, const std::string& text);
    void SetProperty(const std::string& elementId,
                     const std::string& property,
                     const std::string& value);
    void ShowElement(const std::string& elementId);
    void HideElement(const std::string& elementId);

    // -------------------------------------------------------------------------
    // Input forwarding (called from Window::WndProc)
    // Returns true if RmlUi consumed the event.
    // -------------------------------------------------------------------------

    bool ProcessMouseMove(int x, int y);
    bool ProcessMouseButton(int buttonIndex, bool down);
    bool ProcessMouseWheel(float delta);
    bool ProcessKeyDown(int vkCode);
    bool ProcessKeyUp(int vkCode);
    bool ProcessTextInput(wchar_t character);

    // -------------------------------------------------------------------------
    // Frame lifecycle
    // -------------------------------------------------------------------------

    void Update();

    void Render(ID3D12GraphicsCommandList* cmdList,
                ID3D12DescriptorHeap*      srvHeap,
                u32                        viewportWidth,
                u32                        viewportHeight);

    void OnResize(u32 width, u32 height);

private:
    // ----- helpers -----
    static int ConvertVirtualKey(int vkCode);
    static int GetKeyModifiers();

    // Helper: search all documents for an element by id
    // Returns nullptr if not found.
    Rml::Element* FindElement(const std::string& elementId);

    // ----- owned resources -----
    std::unique_ptr<RmlSystem>   m_system;
    std::unique_ptr<RmlRenderer> m_renderer;

    Rml::Context* m_context     = nullptr;
    bool          m_initialized = false;
};

} // namespace dx12e
