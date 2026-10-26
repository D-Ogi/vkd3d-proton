// Offline reproduction of the legacy bindless SSBO translation options.
#include "dxil_spirv_c.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static dxil_spv_bool srv(void *, const dxil_spv_d3d_binding *d, dxil_spv_srv_vulkan_binding *v)
{
    v->buffer_binding = {};
    v->buffer_binding.set = 0;
    v->buffer_binding.binding = 0;
    v->buffer_binding.bindless.use_heap = 1;
    v->buffer_binding.bindless.heap_root_offset = d->register_index;
    v->buffer_binding.descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_SSBO;
    v->offset_binding = {};
    v->offset_binding.set = 1;
    return 1;
}
static dxil_spv_bool uav(void *, const dxil_spv_uav_d3d_binding *d, dxil_spv_uav_vulkan_binding *v)
{
    dxil_spv_srv_vulkan_binding b{};
    srv(nullptr, &d->d3d_binding, &b);
    v->buffer_binding = b.buffer_binding;
    v->buffer_binding.bindless.heap_root_offset += 6;
    v->offset_binding = b.offset_binding;
    return 1;
}
int main(int argc, char **argv)
{
    if (argc < 4 || argc > 7) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 3;
    fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
    std::vector<char> bytes(size); if (fread(bytes.data(), 1, size, f) != size) return 4; fclose(f);
    dxil_spv_parsed_blob blob{}; dxil_spv_converter converter{};
    if (dxil_spv_parse_dxil_blob(bytes.data(), bytes.size(), &blob)) return 5;
    if (dxil_spv_create_converter(blob, &converter)) return 6;
    dxil_spv_converter_set_srv_remapper(converter, srv, nullptr);
    dxil_spv_converter_set_uav_remapper(converter, uav, nullptr);
    dxil_spv_converter_set_root_constant_word_count(converter, 1);
    dxil_spv_option_ssbo_alignment alignment{{DXIL_SPV_OPTION_SSBO_ALIGNMENT}, unsigned(atoi(argv[3]))};
    dxil_spv_option_scalar_block_layout scalar{{DXIL_SPV_OPTION_SCALAR_BLOCK_LAYOUT}, 1, 1};
    if (dxil_spv_converter_add_option(converter, &alignment.base) ||
        dxil_spv_converter_add_option(converter, &scalar.base)) return 7;
    if (argc >= 5) {
        dxil_spv_option_ssbo_robustness_size_alignment granularity{
            {DXIL_SPV_OPTION_SSBO_ROBUSTNESS_SIZE_ALIGNMENT}, unsigned(atoi(argv[4]))};
        if (dxil_spv_converter_add_option(converter, &granularity.base)) return 12;
    }
    if (argc >= 6) {
        dxil_spv_option_ssbo_exact_size_metadata metadata{
            {DXIL_SPV_OPTION_SSBO_EXACT_SIZE_METADATA}, unsigned(atoi(argv[5]))};
        dxil_spv_option_physical_address_descriptor_indexing layout{
            {DXIL_SPV_OPTION_PHYSICAL_ADDRESS_DESCRIPTOR_INDEXING}, 8, 0};
        if (dxil_spv_converter_add_option(converter, &metadata.base) ||
            dxil_spv_converter_add_option(converter, &layout.base)) return 13;
        dxil_spv_converter_set_meta_descriptor(converter, DXIL_SPV_META_DESCRIPTOR_RAW_DESCRIPTOR_HEAP_VIEW,
                argc == 7 ? DXIL_SPV_META_DESCRIPTOR_KIND_UBO_CONTAINING_BDA :
                            DXIL_SPV_META_DESCRIPTOR_KIND_READONLY_SSBO, 2, 0);
    }
    if (dxil_spv_converter_run(converter)) return 8;
    dxil_spv_compiled_spirv spirv{};
    if (dxil_spv_converter_get_compiled_spirv(converter, &spirv)) return 9;
    f = fopen(argv[2], "wb"); if (!f) return 10;
    bool good = fwrite(spirv.data, 1, spirv.size, f) == spirv.size;
    fclose(f); printf("SPIRV alignment=%u bytes=%zu\n", alignment.alignment, spirv.size);
    dxil_spv_converter_free(converter); dxil_spv_parsed_blob_free(blob);
    return good ? 0 : 11;
}
