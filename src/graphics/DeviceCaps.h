#pragma once

namespace dx12e
{

// デバイスの機能（caps）。W05-03 では GraphicsDevice::m_caps（unique_ptr）の型を完全にするためだけの空の定義。
// 中身は W05-04 が入れる（このヘッダは GraphicsDevice.h からは include しない。120 TU に広げない）。
struct DeviceCaps
{
};

} // namespace dx12e
