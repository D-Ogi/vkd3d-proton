/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VKD3D_PIPELINE_LINK_H
#define VKD3D_PIPELINE_LINK_H

#include <vulkan/vulkan.h>
#include <stdbool.h>

/* Four disjoint GPL subsets, whether supplied separately or already combined.
 * The caller retains ownership of every library until Vulkan finishes linking. */
struct vkd3d_pipeline_libraries
{
    VkPipeline handles[4];
    uint32_t count;
    VkGraphicsPipelineLibraryFlagsEXT subsets;
};

static inline bool vkd3d_pipeline_library_subsets_valid(VkGraphicsPipelineLibraryFlagsEXT subsets)
{
    const VkGraphicsPipelineLibraryFlagsEXT valid =
            VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT |
            VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT |
            VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT |
            VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;

    return subsets && !(subsets & ~valid);
}

static inline bool vkd3d_pipeline_libraries_add(struct vkd3d_pipeline_libraries *set,
        VkPipeline library, VkGraphicsPipelineLibraryFlagsEXT subsets)
{
    if (!library || !vkd3d_pipeline_library_subsets_valid(subsets) || (set->subsets & subsets) || set->count >= 4)
        return false;
    set->handles[set->count++] = library;
    set->subsets |= subsets;
    return true;
}

enum vkd3d_pipeline_link_policy
{
    VKD3D_PIPELINE_LINK_MINIMAL,
    VKD3D_PIPELINE_LINK_OPTIMIZE,
};

/* output has room for input_count entries. Preserve specialization and module
 * identifier chains; compact copies rather than changing the owning array. */
static inline uint32_t vkd3d_pipeline_library_stages(const VkPipelineShaderStageCreateInfo *input,
        uint32_t input_count, VkGraphicsPipelineLibraryFlagsEXT subsets,
        VkPipelineShaderStageCreateInfo *output)
{
    const VkShaderStageFlags preraster = VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
            VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    uint32_t i, count = 0;

    for (i = 0; i < input_count; ++i)
    {
        if (!subsets ||
                ((subsets & VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT) &&
                (input[i].stage & preraster)) ||
                ((subsets & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) &&
                input[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT))
            output[count++] = input[i];
    }
    return count;
}

static inline VkPipelineCreateFlags2 vkd3d_pipeline_link_flags(VkPipelineCreateFlags2 flags,
        enum vkd3d_pipeline_link_policy policy)
{
    flags &= ~VK_PIPELINE_CREATE_2_LINK_TIME_OPTIMIZATION_BIT_EXT;
    if (policy == VKD3D_PIPELINE_LINK_OPTIMIZE)
        flags |= VK_PIPELINE_CREATE_2_LINK_TIME_OPTIMIZATION_BIT_EXT;
    return flags;
}

/* Vulkan graphics state subsets (pipelines-graphics-subsets). The rendering
 * pNext chain and full layout stay intact: viewMask is shared by both shader
 * subsets, and unused rendering formats are ignored for those subsets. */
static inline void vkd3d_pipeline_library_fixed_state(VkGraphicsPipelineCreateInfo *info,
        VkGraphicsPipelineLibraryFlagsEXT subsets, bool keep_tir_multisample)
{
    if (!subsets)
        return;
    if (!(subsets & VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT))
    {
        info->pVertexInputState = NULL;
        info->pInputAssemblyState = NULL;
    }
    if (!(subsets & VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT))
    {
        info->pTessellationState = NULL;
        info->pViewportState = NULL;
        info->pRasterizationState = NULL;
    }
    if (!(subsets & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT))
        info->pDepthStencilState = NULL;
    if (!(subsets & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT))
    {
        info->pColorBlendState = NULL;
        if (!(subsets & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) ||
                (!keep_tir_multisample && !info->renderPass && (!info->pMultisampleState || !info->pMultisampleState->sampleShadingEnable)))
            info->pMultisampleState = NULL;
    }
}
#endif
