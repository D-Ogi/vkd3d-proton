/* SPDX-License-Identifier: MIT
 * Offline shader-wrapper regression. No device, Vulkan loader or runtime shader compiler is used.
 */
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_SHADER
#include "vkd3d_shader.h"
#include "dxil_spirv_c.h"
#include <stdio.h>
#include <string.h>

#include "../../tests/shaders/tessellation/headers/nop_hs.h"
#include "../../tests/shaders/tessellation/headers/control_point_phase_hs.h"
#include "../../tests/shaders/tessellation/headers/nop_hs_null_dxil.h"

static unsigned int failures;

#define CHECK(condition, ...) do { if (!(condition)) { \
    fprintf(stderr, "FAIL: " __VA_ARGS__); ++failures; } } while (0)

static void check_fixture(const void *data, size_t size, unsigned int expected_entries)
{
    dxil_spv_parsed_blob blob = NULL;
    unsigned int entries = ~0u;
    dxil_spv_result result;

    dxil_spv_begin_thread_allocator_context();
    result = dxil_spv_parse_dxil_blob(data, size, &blob);
    CHECK(result == DXIL_SPV_SUCCESS && blob, "fixture parse returned %d.\n", result);
    if (blob)
    {
        result = dxil_spv_parsed_blob_get_num_entry_points(blob, &entries);
        CHECK(result == DXIL_SPV_SUCCESS && entries == expected_entries,
                "fixture entries %u, expected %u (result %d).\n", entries, expected_entries, result);
        CHECK(dxil_spv_parsed_blob_get_shader_stage(blob) == DXIL_SPV_STAGE_HULL, "fixture is not a hull shader.\n");
        dxil_spv_parsed_blob_free(blob);
    }
    dxil_spv_end_thread_allocator_context();
}

static void check_compile(const char *name, const void *data, size_t size,
        VkShaderStageFlagBits stage, int expected_result, bool with_debug)
{
    struct vkd3d_shader_interface_info interface_info = {0};
    struct vkd3d_shader_compile_arguments arguments = {0};
    struct vkd3d_shader_code input = {0}, output = {0};
    struct vkd3d_shader_code_debug debug = {0};
    int result;

    interface_info.stage = stage;
    arguments.target = VKD3D_SHADER_TARGET_SPIRV_VULKAN_1_0;
    input.code = data;
    input.size = size;
    result = vkd3d_shader_compile_dxbc(&input, &output, with_debug ? &debug : NULL, 0, &interface_info, &arguments);
    CHECK(result == expected_result, "%s: result %d, expected %d.\n", name, result, expected_result);
    if (expected_result == VKD3D_OK)
    {
        CHECK(output.code && output.size >= 5 * sizeof(uint32_t) &&
                ((const uint32_t *)output.code)[0] == 0x07230203, "%s: missing SPIR-V output.\n", name);
        if (with_debug)
            CHECK(debug.debug_entry_point_name && debug.debug_entry_point_name[0], "%s: empty debug entry name.\n", name);
    }
    else
    {
        CHECK(!output.code && !output.size, "%s: failed compilation returned code.\n", name);
        CHECK(!debug.debug_entry_point_name, "%s: failed compilation returned debug data.\n", name);
    }
    printf("%s debug=%u result=%d bytes=%zu entry=%s\n", name, with_debug, result, output.size,
            debug.debug_entry_point_name ? debug.debug_entry_point_name : "<none>");
    vkd3d_shader_free_shader_code_debug(&debug);
    vkd3d_shader_free_shader_code(&output);
}

int main(void)
{
    const uint32_t malformed[4] = {0};
    unsigned int debug;

    check_fixture(nop_hs_null_code_dxil, sizeof(nop_hs_null_code_dxil), 0);
    check_fixture(control_point_phase_hs_code_dxil, sizeof(control_point_phase_hs_code_dxil), 1);
    for (debug = 0; debug < 2; ++debug)
    {
        check_compile("DXBC HS", nop_hs_code_dxbc, sizeof(nop_hs_code_dxbc),
                VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VKD3D_OK, debug);
        check_compile("named DXIL HS", control_point_phase_hs_code_dxil, sizeof(control_point_phase_hs_code_dxil),
                VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VKD3D_OK, debug);
        check_compile("null DXIL HS", nop_hs_null_code_dxil, sizeof(nop_hs_null_code_dxil),
                VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VKD3D_OK, debug);
        check_compile("malformed", malformed, sizeof(malformed),
                VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VKD3D_ERROR_INVALID_SHADER, debug);
        check_compile("named HS as VS", control_point_phase_hs_code_dxil, sizeof(control_point_phase_hs_code_dxil),
                VK_SHADER_STAGE_VERTEX_BIT, VKD3D_ERROR_INVALID_ARGUMENT, debug);
        check_compile("null HS as VS", nop_hs_null_code_dxil, sizeof(nop_hs_null_code_dxil),
                VK_SHADER_STAGE_VERTEX_BIT, VKD3D_ERROR_INVALID_SHADER, debug);
    }
    printf("Offline shader regression: %s (%u failures).\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
