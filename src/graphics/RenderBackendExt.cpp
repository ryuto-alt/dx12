#include "graphics/RenderBackendExt.h"
#include "core/Application.h"   // RenderBackendExtDeleter の宣言

namespace dx12e
{
void RenderBackendExtDeleter::operator()(RenderBackendExt* p) const { delete p; }
} // namespace dx12e
