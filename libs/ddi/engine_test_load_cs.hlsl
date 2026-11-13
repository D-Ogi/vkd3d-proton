// SPDX-License-Identifier: MIT
// Compute shader of amdgpu_wddm_vkd3d_engine_test, V15 linear layouts: reads one subresource (mip, layer) of an
// R8G8B8A8_UINT Texture2DArray through an SRV and writes each texel, packed, to a raw UAV at word base + y * width
// + x. The root signature is embedded (RTS0 part of the DXIL container). engine_test_load_cs.h is generated from
// this file (dxc from the Windows SDK 10.0.26100, 1.8.2502.11):
//   dxc -T cs_6_0 -E main -Vn g_engine_test_load_cs -Fh engine_test_load_cs.h engine_test_load_cs.hlsl

#define TEST_RS "DescriptorTable(SRV(t0), UAV(u0)), RootConstants(num32BitConstants = 5, b0)"

Texture2DArray<uint4> image : register(t0);
RWByteAddressBuffer output : register(u0);

cbuffer Params : register(b0)
{
    uint mip;
    uint layer;
    uint width;
    uint height;
    uint base;
};

[RootSignature(TEST_RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    uint4 t = image.Load(int4(id.x, id.y, layer, mip));
    output.Store((base + id.y * width + id.x) * 4, t.x | (t.y << 8) | (t.z << 16) | (t.w << 24));
}
