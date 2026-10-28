# Native half structured-buffer bounds

The Vulkan storage-buffer robustness range can round up to a device granularity.
A D3D12 structured view instead has an exact element count. With a ten-byte
stride and five elements, a 52-byte Vulkan range must not admit the half at byte
50. The same defect occurs at byte 70 for a fourteen-byte stride.

`half-bounds-translate.cpp` translates DXIL with the bindless SSBO interface.
Its arguments are input DXIL, output SPIR-V, minimum SSBO alignment and optional
robustness range alignment. Build it against the pinned dxil-spirv C shared
library. Alignment 1 disables offset metadata, as on the descriptor-buffer and
descriptor-heap paths. Set the fourth argument to 4 to enable the new bound.

Use the existing `sm69_long_vector_load_store` DXIL unchanged. Preserve the
translation from dxil-spirv dcb954d5038bc9319a9bb684083c725aacb034b7 as the
negative control. Validate each translation with `spirv-val --target-env
vulkan1.3 --scalar-block-layout`, then disassemble it with `spirv-dis`.

Run `half-bounds-address-model.py OLD.spvasm FIXED.spvasm --extended --output
result.json`. The evaluator follows the generated address instructions and
fails on an unknown instruction. It models the documented four-byte Vulkan
range rounding. It does not execute shader arithmetic or a GPU.

The matrix covers loads and stores at strides 10 and 14, valid final elements,
first invalid elements, UINT32_MAX indices, near-4GB views, and descriptor bases
0, 4144 and 10240. It checks a descriptor-relative address before adding the
resource base. The old translation has 14 failing cases out of 172; the fixed
translation has none.

Compile `half-bounds-tail.cs_6_9.hlsl` with DXC targeting cs_6_9 and native
16-bit types. Translate with range alignment 1 and 4, validate, disassemble,
then run the model with `--tail --extended`. Both controls must pass all 172
cases. This checks the last half at byte offsets 8 and 12 inside a structure;
the new guard must not reject a valid tail.

The runtime acceptance test remains the unchanged legacy-descriptor client:
in-bounds and robust modes on the reference device and the target driver. CPU
address-model success does not establish GPU conformance. Keep any reference
device failure as a failure, without a vendor exception.

## Exact-size metadata case

This correction recovers the element count only when the structured stride is
at least the Vulkan range granularity. A scalar-half stride of 2 cannot use
that proof: both five and six elements can produce the same twelve-byte Vulkan
range. The exact-size option reads the original byte range from the packed
descriptor metadata. It uses the same range for GetDimensions and the element
bound. SRV creation now writes that range, as UAV creation already does. Null
SRVs write zero. Descriptor copies retain the metadata with the payload.

Compile `half-bounds-exact.cs_6_9.hlsl` with native 16-bit types. The offline
translator accepts a fifth option argument, the byte offset of the uint32
range in each 64-byte descriptor (52 for this fixture). A sixth argument
selects the UBO-containing-BDA heap view; omit it for the SSBO heap view.
Validate both SPIR-V files. Run the address model with `--scalar --extended`.
The metadata-free control has 10 failures in 124 cases. Both metadata paths
have zero. These cases include an empty SRV, odd element counts, valid last
elements, first invalid elements, UINT32_MAX and large ranges. This is an
address-model result, not a GPU result.

The exact runtime fixture checks strides 2, 6, 10, 14 and 32, raw aliases and
integer GetDimensions markers. Its reference run currently fails an SRV
dimension marker and several OOB reads. Keep these results as failures while
the independent dimension-only control is investigated. Do not change the
expected count to match that device's result.

## Root descriptor case

The bound above uses OpArrayLength. A root SRV or UAV has no range in D3D12.
On the descriptor-heap path, a root SSBO is a push address, and Mesa cannot
lower OpArrayLength on a push address. RADV then fails in
`radv_nir_lower_descriptors` with a null set layout. The remap therefore gives
a root structured buffer a buffer device address when its alignment is less
than the robustness range alignment.

`half-bounds-root-remap.c` compiles the `sm69_long_vector_load_store` DXIL
through `vkd3d_shader_compile_dxbc` with the root signature of
`test_sm69_long_vector_load_store_root_desc`: t0-t5 and u0-u5 as root
descriptors, minimum SSBO alignment 1 and robustness range alignment 4. Link it
against the static vkd3d-shader, vkd3d-common and dxil-spirv libraries of an
engine build. The optional second argument `noheap` selects the interface
without heap lowering. Exit code 0 means no OpArrayLength and at least one
PhysicalStorageBuffer pointer type. Without the remap change, the heap
translation has four OpArrayLength instructions and no physical pointer.
Both interfaces pass with the change.

The physical pointer path must keep the vector index. A root structured
buffer of `vector<half, 5>` or `vector<half, 7>` previously used an index in
half units with a vector pointer type. Element i went to element 5i or 7i.
