/* Offline check of the root descriptor remap of the descriptor-heap interface.
 *
 * The descriptor-heap path maps a root SRV or UAV SSBO to a push address, which has
 * no range. For a native 16-bit structured buffer whose stride is not a multiple of
 * the robustness size alignment, dxil-spirv bounds each access with OpArrayLength.
 * The remap must give such a root descriptor a buffer device address instead.
 *
 * Arguments: DXIL file of test_sm69_long_vector_load_store (root signature t0-t5,
 * u0-u5 as root descriptors). Link against libvkd3d-shader, libvkd3d_common and the
 * dxil-spirv static libraries of an engine build. Exit code 0 means no OpArrayLength
 * and at least one PhysicalStorageBuffer pointer in the SPIR-V. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkd3d_shader.h"

#define SPV_OP_ARRAY_LENGTH 68
#define SPV_OP_TYPE_POINTER 32
#define SPV_STORAGE_CLASS_PHYSICAL_STORAGE_BUFFER 5349

int main(int argc, char **argv)
{
    static const enum vkd3d_shader_target_extension scalar_block_layout =
            VKD3D_SHADER_TARGET_EXTENSION_SCALAR_BLOCK_LAYOUT;
    struct vkd3d_shader_resource_binding bindings[12];
    struct vkd3d_shader_compile_arguments args;
    struct vkd3d_shader_interface_info info;
    unsigned int array_length = 0, physical = 0;
    struct vkd3d_shader_code dxil, spirv;
    const uint32_t *words;
    size_t i, count;
    long size;
    void *data;
    FILE *f;
    int ret;

    if (argc < 2 || argc > 3 || !(f = fopen(argv[1], "rb")))
        return 2;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    data = malloc(size);
    if (fread(data, 1, size, f) != (size_t)size)
        return 3;
    fclose(f);

    /* The root signature of the descriptor-heap path (state.c): every root SRV and UAV
     * is a raw VA binding with a raw SSBO in set 0, binding = root descriptor index. */
    memset(bindings, 0, sizeof(bindings));
    for (i = 0; i < 12; i++)
    {
        bindings[i].type = i < 6 ? VKD3D_SHADER_DESCRIPTOR_TYPE_SRV : VKD3D_SHADER_DESCRIPTOR_TYPE_UAV;
        bindings[i].register_index = i % 6;
        bindings[i].register_count = 1;
        bindings[i].shader_visibility = VKD3D_SHADER_VISIBILITY_ALL;
        bindings[i].flags = VKD3D_SHADER_BINDING_FLAG_BUFFER | VKD3D_SHADER_BINDING_FLAG_RAW_VA |
                VKD3D_SHADER_BINDING_FLAG_RAW_SSBO;
        bindings[i].binding.set = 0;
        bindings[i].binding.binding = i;
    }

    memset(&info, 0, sizeof(info));
    info.flags = argc == 3 && !strcmp(argv[2], "noheap") ? 0 : VKD3D_SHADER_INTERFACE_HEAP_LOWERING;
    /* RADV: minStorageBufferOffsetAlignment 4 gives 1 on the heap path,
     * robustStorageBufferAccessSizeAlignment is 4. */
    info.min_ssbo_alignment = 1;
    info.ssbo_robustness_size_alignment = 4;
    info.bindings = bindings;
    info.binding_count = 12;
    info.stage = VK_SHADER_STAGE_COMPUTE_BIT;

    memset(&args, 0, sizeof(args));
    args.target = VKD3D_SHADER_TARGET_SPIRV_VULKAN_1_0;
    args.target_extension_count = 1;
    args.target_extensions = &scalar_block_layout;
    args.min_subgroup_size = 32;
    args.max_subgroup_size = 64;
    /* Slot 0 would make u0 the NVAPI extension UAV. */
    args.nv_shader_extn_uav_slot = UINT32_MAX;

    dxil.code = data;
    dxil.size = size;
    memset(&spirv, 0, sizeof(spirv));
    if ((ret = vkd3d_shader_compile_dxbc(&dxil, &spirv, NULL, 0, &info, &args)) < 0)
    {
        fprintf(stderr, "compile failed %d\n", ret);
        return 4;
    }

    words = spirv.code;
    count = spirv.size / sizeof(uint32_t);
    for (i = 5; i < count && (words[i] >> 16); i += words[i] >> 16)
    {
        if ((words[i] & 0xffff) == SPV_OP_ARRAY_LENGTH)
            array_length++;
        else if ((words[i] & 0xffff) == SPV_OP_TYPE_POINTER &&
                words[i + 2] == SPV_STORAGE_CLASS_PHYSICAL_STORAGE_BUFFER)
            physical++;
    }

    printf("spirv bytes=%zu OpArrayLength=%u PhysicalStorageBuffer pointer types=%u\n",
            spirv.size, array_length, physical);
    vkd3d_shader_free_shader_code(&spirv);
    free(data);
    return array_length == 0 && physical ? 0 : 1;
}
