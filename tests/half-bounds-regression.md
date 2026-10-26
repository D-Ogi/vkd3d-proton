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

## Remaining exact-size case

This correction recovers the element count only when the structured stride is
at least the Vulkan range granularity. A scalar-half stride of 2 cannot use
that proof: both five and six elements can produce the same twelve-byte Vulkan
range. That case still needs exact descriptor-size metadata. It is not fixed
or exempted by this change.
