/* SPDX-License-Identifier: MIT
 * Offline shader-wrapper regression. No device, Vulkan loader or runtime shader compiler is used.
 */
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_SHADER
#include "vkd3d_shader.h"
#include "dxil_spirv_c.h"
#include <inttypes.h>
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

/* The FSR 4.1.2 conv passes get a compute barrier after the dispatch in every application, with and without
 * application quirks (vkd3d_shader_fsr4_post_barrier_hashes, vkd3d_shader_main.c). The key is the hash of the DXIL
 * part, pinned here as derived from the DLL's containers; the conv passes come from a frame capture of the DLL's
 * dispatches and barriers. */
static void check_builtin_quirks(void)
{
    static const vkd3d_shader_hash_t conv_part_hashes[] = {
        0x0a2cf36e9a267dbcull, 0xf6a627fe621f2ecaull, 0xb80be06cbf660f5full, 0xac8621a7509d92f5ull,
        0x539bdf3943318ae1ull, 0x38e117464cb90a26ull, 0x856803dfb52462b8ull, 0xc98e37733b65770dull,
        0xf6b6ff1f520da32cull, 0xe08e83c7aa16ff62ull, 0x3e8dab2e67c83f6bull, 0xa650d84f380d5435ull,
        0x46f48307cce37f93ull,
        0xb41306fbe32af591ull, 0xf583f1bcc34310d1ull, 0xc0532fda75f90acfull, 0xd26bdd22c83fa6e1ull,
        0xd5582283674bec12ull, 0x2a1c8509970fcadfull, 0xeeceb55864a63a69ull, 0x7b25e192737f244bull,
        0x3c5c88452c25177cull, 0x9ffa7a11946b6f2dull, 0x22e436abd5bbb8e4ull, 0xbaa8999b14c1d737ull,
        0xc1c54a540edcfbd0ull,
    };
    /* pass3_post of the 4K set: a _post pass needs no barrier of its own. */
    const vkd3d_shader_hash_t post_part_hash = 0x1e0b6840e3042fc7ull;
    /* The container hash of the 4K prepass as the DLL passes it: the old key, which no longer selects anything. */
    const vkd3d_shader_hash_t prepass_container_hash = 0x71eebd9780b7703cull;
    vkd3d_shader_quirks_t quirks;
    unsigned int i;

    for (i = 0; i < sizeof(conv_part_hashes) / sizeof(conv_part_hashes[0]); ++i)
    {
        quirks = vkd3d_shader_builtin_quirks_for_dxil_part(conv_part_hashes[i]);
        CHECK(quirks == VKD3D_SHADER_QUIRK_FORCE_COMPUTE_BARRIER, "conv part hash %u: quirks %#"PRIx64".\n",
                i, (uint64_t)quirks);
    }
    CHECK(!vkd3d_shader_builtin_quirks_for_dxil_part(post_part_hash), "post part hash: unexpected quirk.\n");
    CHECK(!vkd3d_shader_builtin_quirks_for_dxil_part(prepass_container_hash), "container hash: unexpected quirk.\n");
    CHECK(!(vkd3d_shader_compile_arguments_select_quirks(NULL, prepass_container_hash, NULL) &
            VKD3D_SHADER_QUIRK_FORCE_COMPUTE_BARRIER), "container hash selects a quirk.\n");
    CHECK(vkd3d_shader_get_revision(NULL) != 1, "revision does not include the built-in quirks.\n");
    printf("built-in quirks: %u conv part hashes checked\n", i);
}

/* The D3D12 shell gives the engine a container that it writes around the DDI's DXIL part. The key must not depend
 * on the rest of the container: wrap the DXIL part of a fixture in a container of that part alone, and compare. */
static void check_dxil_part_hash(void)
{
    const uint8_t *fixture = (const uint8_t *)control_point_phase_hs_code_dxil;
    const size_t fixture_size = sizeof(control_point_phase_hs_code_dxil);
    struct vkd3d_shader_code original = {0}, rebuilt = {0}, malformed = {0};
    vkd3d_shader_hash_t original_part = 0, rebuilt_part = 0, ignored;
    uint32_t chunk_count, offset, tag, size, i;
    static uint8_t container[16384];
    const uint32_t zero[8] = {0};
    bool found = false;

    original.code = fixture;
    original.size = fixture_size;
    CHECK(vkd3d_shader_hash_dxil_part(&original, &original_part), "fixture: no DXIL part.\n");

    memcpy(&chunk_count, fixture + 28, 4);
    for (i = 0; i < chunk_count && !found; ++i)
    {
        memcpy(&offset, fixture + 32 + 4 * i, 4);
        memcpy(&tag, fixture + offset, 4);
        memcpy(&size, fixture + offset + 4, 4);
        if (!memcmp(&tag, "DXIL", 4) && 36 + 8 + size <= sizeof(container))
        {
            const uint32_t version = 1, total = 36 + 8 + size, count = 1, part_offset = 36;

            memcpy(container, "DXBC", 4);
            memset(container + 4, 0x5a, 16); /* another checksum */
            memcpy(container + 20, &version, 4);
            memcpy(container + 24, &total, 4);
            memcpy(container + 28, &count, 4);
            memcpy(container + 32, &part_offset, 4);
            memcpy(container + 36, fixture + offset, 8 + size);
            rebuilt.code = container;
            rebuilt.size = total;
            found = true;
        }
    }
    CHECK(found, "fixture: DXIL part not located.\n");
    CHECK(vkd3d_shader_hash_dxil_part(&rebuilt, &rebuilt_part), "rebuilt: no DXIL part.\n");
    CHECK(original_part == rebuilt_part, "DXIL part hash %016"PRIx64" != %016"PRIx64".\n",
            original_part, rebuilt_part);
    CHECK(vkd3d_shader_hash(&original) != vkd3d_shader_hash(&rebuilt), "container hashes are equal.\n");
    CHECK(!vkd3d_shader_select_builtin_quirks(&original, vkd3d_shader_hash(&original)), "fixture has a quirk.\n");
    malformed.code = zero;
    malformed.size = sizeof(zero);
    CHECK(!vkd3d_shader_hash_dxil_part(&malformed, &ignored), "malformed container has a DXIL part.\n");
    CHECK(!vkd3d_shader_hash_dxil_part(&(struct vkd3d_shader_code){nop_hs_code_dxbc, sizeof(nop_hs_code_dxbc)},
            &ignored), "DXBC container has a DXIL part.\n");
    printf("DXIL part hash: %016"PRIx64" in both containers\n", original_part);
}

/* Development-PC check, outside the regression: for each container file, the container hash, the DXIL part hash
 * and the built-in quirks that the compile path applies (vkd3d_shader_compile_dxil). */
static int check_files(int argc, char **argv)
{
    static uint8_t data[1 << 20];
    int i, errors = 0;

    for (i = 1; i < argc; ++i)
    {
        struct vkd3d_shader_code code = {0};
        vkd3d_shader_hash_t part = 0;
        size_t size;
        FILE *f;

        if (!(f = fopen(argv[i], "rb")))
        {
            printf("QUIRK_FILE_ERROR %s: cannot open\n", argv[i]);
            ++errors;
            continue;
        }
        size = fread(data, 1, sizeof(data), f);
        fclose(f);
        code.code = data;
        code.size = size;
        if (!vkd3d_shader_hash_dxil_part(&code, &part))
        {
            printf("QUIRK_FILE_ERROR %s: no DXIL part\n", argv[i]);
            ++errors;
            continue;
        }
        printf("QUIRK_FILE %s hash=%016"PRIx64" dxil_part=%016"PRIx64" quirks=%#"PRIx64"\n", argv[i],
                vkd3d_shader_hash(&code), part, (uint64_t)vkd3d_shader_select_builtin_quirks(&code, 0));
    }
    return errors ? 1 : 0;
}

int main(int argc, char **argv)
{
    const uint32_t malformed[4] = {0};
    unsigned int debug;

    if (argc > 1)
        return check_files(argc, argv);

    check_builtin_quirks();
    check_dxil_part_hash();

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
