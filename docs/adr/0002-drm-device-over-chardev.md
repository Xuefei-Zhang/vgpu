# Real DRM device instead of a bare character device

The original learning plan targeted a bare `/dev/vgpu` character device. We implement vgpu as a real DRM card (`/dev/dri/card0`): `drm_driver` + `drm_dev_register`, shmem GEM buffer objects, private `VGPU_CS` / `VGPU_WAIT` / `VGPU_QUERY` ioctls, and a `dma_fence` for completion. Module init registers a local platform_driver + platform_device (the vgem trick), so the probe/remove lifecycle from the original plan's week 3 is kept without a device tree (x86 guest has no DT; the platform resource path is the x86 analogue and is noted as such in the README).

**Why**: DRM is the framework every modern GPU driver (amdgpu, i915, and the reference driver vgem) is built on. A chardev would teach the wrong API surface for a GPU-driver interview, and vgem can then be read line-by-line as the reference implementation.

**Consequences**: there is no `/dev/vgpu`; the userspace test must speak the DRM ioctl ABI (GEM_CREATE / GEM_MMAP plus the private ioctls); device-tree matching from the original plan is dropped (intentional scope cut, documented in DESIGN.md §与原计划的差异).
