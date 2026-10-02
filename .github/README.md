# vkd3d-proton for amdgpu-wddm

This fork of [HansKristian-Work/vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) carries the
vkd3d-proton engine of [amdgpu-wddm](https://github.com/D-Ogi/amdgpu-wddm), a Windows WDDM driver stack for the
AMD BC-250. There, vkd3d-proton is not a set of application-local DLLs: it is the engine behind a native
Direct3D 12 user-mode driver, so that applications use the standard D3D12 runtime and DXGI of Windows.

## Registered on unit A (2026-10-03)

| Binary | SHA-256 | Branch | Commit | Registered since |
|---|---|---|---|---|
| `amdgpu_wddm_vkd3d.dll`, the D3D12 engine | D79FEC49 | `amdgpu-wddm/registered-2026-10-02` (also on `amdgpu-wddm/draw-path`) | `c3710ac1` on `bed41016` | 2026-10-02 |

That engine, behind the project's D3D12 user-mode driver, runs The Witcher 3 in its DirectX 12 edition with ray
tracing on the lab machine, started from Steam, through the system D3D12 runtime: a device at feature level
12_1, tiled resources tier 3, hardware ray tracing, and Present to the desktop. The frame rates, the trial
numbers and the open defects are in the main repository, not here; the numbers are modest and the stack is a
prototype, but the path is native.

The `dxil-spirv` submodule points at [D-Ogi/dxil-spirv](https://github.com/D-Ogi/dxil-spirv), pinned on the
registered commit to `bc773cf2` of its `amdgpu-wddm/dxbc-udiv-type` branch.

## Branches

| Branch | What it is |
|---|---|
| `master` | upstream's `master`; no project commits land here |
| `amdgpu-wddm/registered-2026-10-02` | the exact commit the registered engine was built from; it does not move |
| `amdgpu-wddm/ddi-engine` | upstream vkd3d-proton plus `libs/ddi/` and two small libvkd3d changes; engine ABI 1.0 |
| `amdgpu-wddm/ddi-engine-1.3-rtcfg` | ABI 1.3 with the ray-tracing configuration, and the dxil-spirv fork as the submodule source |
| `amdgpu-wddm/quiet-stdio-2026-10-02` | nothing on stdio unless `AMDGPU_WDDM_LOG` or `VKD3D_DEBUG` asks; the base of the registered engine |
| `amdgpu-wddm/draw-path` | the per-draw path and the barrier counters, continuing past the registered commit |
| `amdgpu-wddm/engine-slowstart-2026-09-30` | the driver cache and the pipeline log written off the caller's thread |
| `amdgpu-wddm/ddi-engine-inline-wip` | draft: an inline queue mode, never frozen, never measured |

`-Denable_ddi_engine=true` (the project branches only) builds the engine `amdgpu_wddm_vkd3d.dll` and
`amdgpu_wddm_vkd3d_engine_test.exe`; commits before `a582668d` name them `bc250vkd3d.dll` and
`bc250vkd3d_engine_test.exe`.

Only the registered commit above has been deployed. Every other branch builds and passes its host tests; that
is not the same as having run on the hardware. Scope, ABI and gaps:
[d3d12-ddi-engine.md](https://github.com/D-Ogi/amdgpu-wddm/blob/main/docs/design/d3d12-ddi-engine.md); facts and
evidence: [docs/facts.md](https://github.com/D-Ogi/amdgpu-wddm/blob/main/docs/facts.md).

License: vkd3d-proton is under the GNU LGPL version 2.1 or later ([COPYING](https://github.com/D-Ogi/vkd3d-proton/blob/master/COPYING),
[LICENSE](https://github.com/D-Ogi/vkd3d-proton/blob/master/LICENSE)) and stays a separately loaded DLL. The files
under `libs/ddi/` carry `SPDX-License-Identifier: MIT`.
