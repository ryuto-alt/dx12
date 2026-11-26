#include "renderer/vfx/VfxRuntime.h"

namespace dx12e::vfx
{
Runtime& Runtime::Get()
{
    static Runtime r;
    return r;
}
} // namespace dx12e::vfx
