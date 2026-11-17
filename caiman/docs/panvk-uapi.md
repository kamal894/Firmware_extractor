# What PanVK asks of a kernel (for a `hakios-panthor` shim over HakiosGPU)

Sources: Mesa main (2026-11) `include/drm-uapi/panthor_drm.h`, `src/panfrost/lib/kmod/panthor_kmod.c`,
`src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c`; the Pixel's kbase r54p3 GPL source for the firmware-side
formats (`~/caiman-work/kbase-src/tree`). Written by hand while Codex was at its usage limit; to be
re-checked by Codex.

## The requests

Through `pan_kmod` (panthor_kmod.c): `DEV_QUERY` (GPU info, CSIF info, timestamps, group priorities),
`VM_CREATE`, `VM_DESTROY`, `VM_BIND`, `VM_GET_STATE`, `BO_CREATE`, `BO_MMAP_OFFSET`, `BO_QUERY_INFO`,
`BO_SET_LABEL`, `BO_SYNC`, `SET_USER_MMIO_OFFSET` (the LATEST_FLUSH_ID page).

Directly from PanVK's CSF code: `GROUP_CREATE`, `GROUP_DESTROY`, `GROUP_GET_STATE`, `GROUP_SUBMIT`,
`TILER_HEAP_CREATE`, `TILER_HEAP_DESTROY`, and DRM sync objects (`SYNCOBJ_CREATE/DESTROY/RESET/SIGNAL/
WAIT`, timeline waits, transfers, export/import as sync files and as fds).

## How PanVK uses a group (panvk_vX_gpu_queue.c, around :650)

One group per Vulkan queue, with **three queues (command streams)**: vertex-tiler, fragment, compute, each
created with priority 1 and a **64 KiB ring** (`drm_panthor_queue_create.ringbuf_size`); compute and fragment
core masks = the shader cores, tiler mask 1, max cores = their counts. A submission (`GROUP_SUBMIT`) is a list
of `drm_panthor_queue_submit`: queue index, the user command stream's GPU address (64-byte aligned) and size,
`latest_flush` (the FLUSH_ID when it was built: the kernel flushes and invalidates the caches before calling
it unless nothing changed since), and sync operations (wait / signal on sync objects, timeline values).
The kernel's own ring for each queue does: cache flush, CALL the user stream, then signal (a sync object
update in memory and, in Linux, an interrupt). Cross-queue ordering inside a submission is PanVK's own, with
SYNC_WAIT/SYNC_ADD on memory sync objects in its streams.

The kernel keeps the **last 4 CS registers** for itself (`unpreserved_cs_reg_count`; with CS features
0x7105f there are 96 work registers, r92-r95 the kernel's). Our `mali_submit` uses r68-r70 today: it must move
to r92-r95 before PanVK's streams run on it.

## Tiler heaps (TILER_HEAP_CREATE)

Arguments: chunk size, initial chunk count, max chunks, target in flight. Returns the heap context's GPU VA and
the first chunk's VA (PanVK puts the latter in its Tiler Heap descriptor: base, bottom = base + 64, top =
base + size; panvk_vX_gpu_queue.c :772). In kbase's format (csf/mali_kbase_csf_tiler_heap*.{c,h}): the
context is 32 bytes whose first u64 points at the first chunk; every chunk starts with a 64-byte header whose
first u64 points at the next; a pointer is encoded `(address & ~0xfff) | (size >> 12)`. PanVK gives the
context to HEAP_SET at queue start. When the chunks run out the firmware raises a tiler out-of-memory event
for the kernel to add chunks; a large initial count (PanVK asks for 5 x 2 MiB) avoids that at first.

## The kernel side, in order (what we have, what is missing)

| Need | Have (docs/gpu-plan.md step 8) | Missing |
|---|---|---|
| GPU/CSIF info | the values are known (GPU_ID, features, CS features) | a query request |
| VM, BOs, bind | one AS1, page mapping, kernel pages | per-BO objects, mmap to user space (HakiosGPU has this for the Adreno) |
| Groups | CSG 0 with CS 0, started by the loader | CS 1 and 2 (fragment, compute) with 64 KiB rings each; the ring CS registers r92-r95 |
| Submit | CALL + memory fence on CS 0 | per-queue cache flush before CALL, per-queue fences, sync object signal |
| Sync objects | fences in memory | the shim keeps sync objects over the kernel's fences (as hakios-drm does) |
| Tiler heaps | a hand-made one for our triangle | contexts in kbase's format, chunk lists |
| User MMIO page | - | LATEST_FLUSH_ID readable from user space (or a constant 0: always flush) |

First goal through PanVK: vkCreateDevice and one compute dispatch; then a render pass with a triangle.
