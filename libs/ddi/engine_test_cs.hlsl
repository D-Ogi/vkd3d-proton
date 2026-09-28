// SPDX-License-Identifier: MIT
// Compute shader of amdgpu_wddm_vkd3d_engine_test: fills a raw UAV reached through a shader-visible
// descriptor table.
// The root signature is embedded (RTS0 part of the DXIL container), so the test needs no serializer from the
// system d3d12.dll. engine_test_cs.h is generated from this file (dxc from the Windows SDK 10.0.26100,
// 1.8.2502.11):
//   dxc -T cs_6_0 -E main -Vn g_engine_test_cs -Fh engine_test_cs.h engine_test_cs.hlsl

#define TEST_RS "DescriptorTable(UAV(u0)), RootConstants(num32BitConstants = 1, b0)"

RWByteAddressBuffer output : register(u0);

cbuffer Params : register(b0)
{
    uint seed;
};

[RootSignature(TEST_RS)]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    output.Store(id.x * 4, id.x * 2654435761u + seed);
}
