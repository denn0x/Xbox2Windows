# XCL capability matrix

Evidence map for 2026-11-24. XCL is pinned at [38b05b7](https://github.com/ivall/xcl/commit/38b05b7b6954cfa5bd21cec72f68823603ca2227) (observed 2026-11-23). The rules for reading it are in the [reference note](xcl-reference.md).

The XCL column describes public behaviour at the pin. It is not a local measurement of XCL. The Xbox2Windows column names checked-in CTest registrations; a row without a named test is a gap. A row becomes evidence only when that test is checked in. An XCL behaviour is a question, not a requirement to mirror its interface, and a gap is not a scheduled commitment.

| Area | XCL public behaviour | Xbox2Windows evidence (named checked-in test) | Gap |
| --- | --- | --- | --- |
| PE mapping and relocations | Maps x64 images and applies relocations. | `cpu_native_map`, `cpu_native_reloc_free`, `cpu_native_seh`, `cpu_native_tls`, `cpu_native_load_cookie` | No whole-image policy report; the mapping fixtures are source-built. |
| Import routing | Routes imports by name or ordinal, including delay-load, to HLE, Windows or stub providers. | `static_exe_intake`, `conditional_import_paths`, `bundled_export_forwarders`, `pe_package_import_closure`, `pe_package_ordinal_plan` | No checked-in per-image classification of every import against those policies. |
| Title memory and physical pages | Distributes title memory, physical pages and GPU-visible allocations. | `era_heap_allocation`, `era_heap_failure`, `era_heap_concurrency`, `era_heap_large_alignment`, `era_title_va_translate`, `era_title_gpu_alloc`, `gpu_alias_window_warp`, `era_physical_controls`, `xmem_shim_allocation`, `xmem_shim_mapped_pages`, `xmem_shim_query` | Large-memory and sparse-page behavior over a full title frame is not measured. |
| XGameRuntime | Provides GDK runtime services such as user, save, package and system calls. | None. | No XGameRuntime shim or source-built call fixture is checked in. |
| D3D12.X device, root signatures and descriptor heaps | Creates the console device, accepts console root-signature flags and builds descriptor heaps through the returned device. | `desktop_root_signature`, `d3d12_xs_shim_exports`, `d3d12_xs_shim_serialize`, `d3d12_xs_shim_deserialize`, `d3d12_xs_shim_warp_root`, `d3d12x_descriptor_heap_warp`, `d3d12x_descriptor_table_warp` | Only the exported root-signature surface, device-creation entry and desktop descriptor semantics on the returned device are covered; console vtable slots, console descriptor layout and console resource creation are not measured. Desktop resource and command-list methods on the returned device are covered by the D3D12.X command lists row. |
| D3D12.X command lists | Records and submits console compute and graphics lists, including PresentX. | `d3d12x_compute_list_warp`, `d3d12x_graphics_list_warp`, `d3d12x_graphics_list_hardware` | A compute buffer and a graphics triangle are recorded and read back on the device returned by XCL-08; PresentX is not covered, and no console command-list interface is claimed. |
| Shader translation | Translates console shader programs to desktop shaders across stages. | `pixel_aot_warp`, `pixel_aot_u32_from_f32_warp`, `compute_store_aot_warp`, `compute_store_aot_lds_qword_warp`, `mesh_entry_draw_warp`, `mesh_pixel_pipeline_warp`, `mesh_amplification_refusal` | Selected forms and stages are measured. The mesh translator reports unsupported amplification shaders by name. No whole-pack translation rate is checked in. |
| Shader census and tools | Reports shader records, stages and unsupported forms. | `shader_metadata_census`, `compressed_scene_metadata`, `shader_metadata_controls` | Focused metadata probes exist; no checked-in whole-pack census. |
| Pipelines | Creates graphics and compute pipelines, including mesh combinations. | `pso_root_signature_warp`, `pso_readiness_check`, `mesh_pixel_pipeline_warp`, `mesh_amplified_draw_warp`, `d3d12x_graphics_list_warp`, `d3d12x_graphics_list_hardware` | Selected desktop PSOs are measured, including a source-built AS+MS+PS pipeline. Console pipeline creation and multi-group mesh dispatches remain open. |
| Textures | Handles console tiling, mip layouts and BC formats. | `bc7_block_reference`, `bc7_tile_swizzle`, `bc7_draw_warp`, `bcpack_prefix_and_mip_tail`, `bcpack_rans_symbols` | BC7 and BCPack paths are measured; full GFX10 tiling and other BC families are not checked in as one runtime path. |
| Audio | Provides XAudio2 voices and console audio formats. | `audio_awc_metadata` | Metadata inventory only; no voice output or XMA/Opus decode path is measured. |
| Media and BVH | Runs Bink, FidelityFX and BVH media paths. | `bvh_root_roundtrip_warp`, `bvh_differential_warp`, `ffx_pass26_lds_warp`, `cpu_aot_bink_heap`, `cpu_bink_frame_to_heap` | Selected Bink, FidelityFX and BVH fixtures pass; no end-to-end media path is checked in. |
| End-to-end | Boots a retail title and reaches its first presented frame or menu on its development machine. | `cpu_exe_aot_convert`, `cpu_native_archive_blast`, `cpu_native_frame_warp` | Source-built programs run from PE entry; title CPU execution and full-game startup are not measured. |

Reading the rows:

- "None" means no checked-in test covers that area.
- A named test supports the behaviour in its own fixture; it does not prove a general implementation or a production interface.
- A `_warp` test uses the software adapter. A `_hardware` test may return a capability skip, which is not a pass.
- `pso_readiness_check` inspects manifests; it does not create a console pipeline.

Coverage boundaries:

- PE and import tests use source-built images, not a title image.
- Memory tests cover allocator and page bookkeeping, not a complete title frame.
- Shader and pipeline tests cover selected fixtures, not a whole shader pack.
- End-to-end means source-built programs from PE entry, not title execution.

Public reports behind the XCL column:

- [XCL README at the pin](https://github.com/ivall/xcl/blob/38b05b7b6954cfa5bd21cec72f68823603ca2227/README.md)
- [XCL HANDOFF at the pin](https://github.com/ivall/xcl/blob/38b05b7b6954cfa5bd21cec72f68823603ca2227/HANDOFF.md)
- [Public report #159](https://github.com/ivall/xcl/issues/159)
- [Public report #173](https://github.com/ivall/xcl/issues/173)
- [GPU results #42](https://github.com/ivall/xcl/issues/42)
- [Shader count reconciliation #36](https://github.com/ivall/xcl/issues/36)

What this table does not say:

- It does not rank XCL features or assign work.
- It does not assert that a service should be reproduced with the same interface.
- It does not turn an XCL observation into an Xbox2Windows measurement.
