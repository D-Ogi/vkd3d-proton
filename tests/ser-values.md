# HitObject value readback

`ser-values.exe` is a headless Windows cross-test client. It loads Microsoft's D3D12 runtime
and Agility SDK 619 from `D3D12/D3D12Core.dll`. It has no dependency on the local translation
engine. The same executable can test a native driver and the installed system D3D12 route.
The `test_shader_execution_reordering_values` entry is also in the full cross-test suite.

Each of the 21 cases dispatches one ray and reads back exact integers. `SER_CASE` selects
one case; an unset or empty value runs all cases. Unsupported SM 6.9, missing pipelines,
skipped cases and absent readbacks are not passes. Run under an external deadline of no
more than 150 seconds. The client creates no window. It prints loaded modules while the
device is live; the caller must record their versions and hashes and check the intended route.

| Cases | Contract |
| --- | --- |
| copy | A copied value can change SBT index without changing the original: 111, 123, 0, 1. |
| phi00, phi01, phi10, phi11 | Two independent PHIs select miss records 0 and 1. |
| unbound, nop | Invoke on an unbound query result or NOP leaves payload 100. |
| unbound-root | An unbound hit reads root constant 0 and still reports IsHit=1. |
| bound-root | Binding that query hit selects record 0: root 0xcafe, payload 111, IsHit=1. |
| direct | A normal miss invocation writes payload 111 (positive control). |
| payload-alias | TraceRay, CallShader and Invoke share a payload allocation: 129. |
| loop0, loop1, loop2, loop3, loop16, loop31 | A live object survives construction of its successor across iterations. |
| array0, array1 | Dynamically indexed local storage retains both objects. |
| helper | A helper's copied value does not alias its caller's value. |
| hint | Hint-only reordering leaves payload 100. |

The bound and unbound query cases share one triangle scene and one hit record containing
0xcafe. The miss shaders add 11 or 23; the hit shader adds 11; the callable shader adds 7.
The payload-alias input is validated DXIL assembly because current DXC usually splits the
payload allocations when compiling ordinary HLSL. It is the same fixture as the pinned
dxil-spirv regression, not a source-level substitute that misses the temporary copy path.

Regenerate `shaders/rt/headers/ser_values.h` with `shaders/rt/generate_ser_values.py`, using
DXC 1.9.2609.5, its `dxv.exe`, and the assembler built from the pinned dxil-spirv
`tests/ser-values/assemble-dxil.cpp`. Pass `--dxc`, `--assembler` and a new `--output`
directory. It records commands and source/tool/output hashes. The other regression inputs
come from that submodule. Compile validation does not establish GPU correctness.

The isolated diagnostic engine build uses the LAB `ddi-engine` recipe, Clang 23.1.4 and
two jobs without LTO. It is for correctness comparisons, not release or performance claims.
