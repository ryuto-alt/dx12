#include "renderer/RenderStageHooks.h"
#include "core/Application.h"   // FeatureRegistryDeleter の宣言

namespace dx12e
{
void FeatureRegistryDeleter::operator()(FeatureRegistry* p) const { delete p; }
} // namespace dx12e
