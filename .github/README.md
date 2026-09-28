# vkd3d-proton for amdgpu-wddm

This fork of [HansKristian-Work/vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) carries the
vkd3d-proton engine of [amdgpu-wddm](https://github.com/D-Ogi/amdgpu-wddm), a Windows WDDM driver stack for the
AMD BC-250. The plan there is a native Direct3D 12 user-mode driver with vkd3d-proton as its engine, so that
applications use the standard D3D12 runtime of Windows; that driver is being built and no D3D12 device has been
demonstrated through the system Windows runtime yet.

| Branch | What it is |
|---|---|
| `master` | upstream's `master`; no project commits land here |
| `amdgpu-wddm/ddi-engine` | upstream vkd3d-proton plus `libs/ddi/` and two small libvkd3d changes |
| `amdgpu-wddm/ddi-engine-inline-wip` | draft: an inline queue mode for engine ABI 1.1, not frozen |

Both project branches fork from upstream commit `472989aabdcf3e343e16d13c402feae3815008aa` (2026-09-25).

- `amdgpu-wddm/ddi-engine` (head `a582668d`) has five commits: `VKD3D_CONFIG` defaults set by an embedding
  module, the engine DLL, the static C runtime for it, swapchain extensions only with `VK_KHR_swapchain`, and
  the rename of the DLL.
- `-Denable_ddi_engine=true` (this branch only) builds the engine `amdgpu_wddm_vkd3d.dll` and
  `amdgpu_wddm_vkd3d_engine_test.exe`; commits before `a582668d` name them `bc250vkd3d.dll` and
  `bc250vkd3d_engine_test.exe`.
- Engine ABI 1.0 is frozen: capability queries and engine bring-up off the runtime.
- `amdgpu-wddm/ddi-engine-inline-wip` (head `439a96cd`) adds 14 commits on top: the inline queue mode planned
  for ABI 1.1 (see "Gaps" in the design note below). Its header says revision r2-draft, ABI 1.1, not frozen.
  Nothing on it has been measured on the BC-250.

Tested on the BC-250 (facts in the main repository's
[docs/facts.md](https://github.com/D-Ogi/amdgpu-wddm/blob/main/docs/facts.md)): the frozen ABI 1.0 engine from
`5712e8c5`, under its pre-rename file name and with the hosted RADV driver loaded directly, passes independent
devices, a GPU copy with readback and DXIL compute, and reports FL11_1 (M757); the same engine test passes with
a ray-tracing candidate RADV (M759). A local, unpublished ABI 1.2 build (source `7bfcd7f0`, renamed DLL) answered adapter capability queries on hosted RADV without creating a device: FL11_1, tiled resources tier 0, binding tier 3, RT tier 1.1 (M769); the published ABI 1.0 and 1.1 branches did not perform that run. FL12_1, rendering through the D3D12 runtime and ray tracing in the engine
are not established. Scope, ABI and gaps:
[d3d12-ddi-engine.md](https://github.com/D-Ogi/amdgpu-wddm/blob/main/docs/design/d3d12-ddi-engine.md).

License: vkd3d-proton is under the GNU LGPL version 2.1 or later ([COPYING](https://github.com/D-Ogi/vkd3d-proton/blob/master/COPYING),
[LICENSE](https://github.com/D-Ogi/vkd3d-proton/blob/master/LICENSE)) and stays a separately loaded DLL. The files
under `libs/ddi/` carry `SPDX-License-Identifier: MIT`.
