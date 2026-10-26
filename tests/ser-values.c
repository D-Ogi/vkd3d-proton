/* Headless subset of the native D3D12 cross-test client. */
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#define INITGUID
#define VKD3D_TEST_DECLARE_MAIN
#include "d3d12_crosstest.h"

__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";

void test_shader_execution_reordering_values(void);
START_TEST(ser_values)
{
    pfn_D3D12CreateDevice = get_d3d12_pfn(D3D12CreateDevice);
    pfn_D3D12EnableExperimentalFeatures = get_d3d12_pfn(D3D12EnableExperimentalFeatures);
    pfn_D3D12GetDebugInterface = get_d3d12_pfn(D3D12GetDebugInterface);
    pfn_D3D12GetInterface = get_d3d12_pfn(D3D12GetInterface);
    pfn_D3D12CreateVersionedRootSignatureDeserializer = get_d3d12_pfn(D3D12CreateVersionedRootSignatureDeserializer);
    pfn_D3D12SerializeVersionedRootSignature = get_d3d12_pfn(D3D12SerializeVersionedRootSignature);
    parse_args(argc, argv);
    enable_d3d12_debug_layer(argc, argv);
    enable_feature_level_override(argc, argv);
    vkd3d_set_running_in_test_suite();
    init_adapter_info();
    run_test(test_shader_execution_reordering_values);
}
