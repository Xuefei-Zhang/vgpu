# vgpu —— 一个纯软件模拟的 GPU 驱动

在 QEMU/KVM guest 里运行的虚拟 GPU 设备驱动，用来**不动任何真实 GPU 硬件**地学完整条 Linux GPU 驱动软件链：
设备注册（platform + DRM）→ 缓冲对象（GEM）→ 命令提交（Ring + Doorbell）→ 命令执行（Engine）→ 中断上下半部 + dma_fence → 用户态等待与校验。

词汇表见 [CONTEXT.md](CONTEXT.md)，设计文档见 [docs/DESIGN.md](docs/DESIGN.md)，关键决策见 [docs/adr/](docs/adr/)。
参考实现：主线 `drivers/gpu/drm/vgem`（Intel 虚拟 GPU）。

## 运行环境

```
┌──────────────────────── Host: xuefeiz2-server ────────────────────────┐
│  vLLM (生产)  ← 硬约束：绝不可碰                                        │
│  ~/linux (6.17.0, KASAN) ──► bzImage + vgpu.ko（同一源码树编译）          │
│  QEMU 8.2.2 + KVM（4C / 2G，串口控制台）                                 │
│  ┌───────────────────────── Guest ───────────────────────────┐       │
│  │  6.17.0 KASAN 内核 + busybox initramfs（无磁盘）             │       │
│  │  /root/vgpu.ko + /root/vgpu_test(静态) + /init             │       │
│  │  /dev/dri/card0  ← vgpu DRM 设备                            │       │
│  └────────────────────────────────────────────────────────────┘       │
└──────────────────────────────────────────────────────────────────────┘
```

Host 只多一个 QEMU 进程（~2G 内存）：不装模块、不重启（ADR-0003），vLLM 零影响。
实时调试：guest 以默认参数启动即可打断点；`DEBUG=1 scripts/30-run.sh` 会 `-s -S` 暂停，host 上
`gdb-multiarch vmlinux -ex 'target remote :1234'` 直接进 vgpu 代码单步。

## 一分钟跑起来

```bash
scripts/10-build.sh        # 1) 编译 vgpu.ko（与 guest 内核同源）+ 静态用户态测试
scripts/20-initramfs.sh    # 2) 打包 busybox initramfs（vgpu.ko + vgpu_test + /init）
scripts/30-run.sh          # 3) 启动 KVM guest；/init 自动 insmod，autorun 时自动跑全部测试
```

guest 内手动跑：`/root/vgpu_test`（单进程全链路）、`/root/vgpu_test -m 4`（4 进程并发）、
`/root/vgpu_test -e`（负路径）。代码树里 `touch autorun` 后重新打包即可开机自动测试。

测试覆盖六个阶段，全部通过（摘自我机器上最近一次真实运行，非示意）：

```
=== single process full chain ===
vgpu vgpu.0: [vgpu:probe] ready: /dev/dri/card0 (renderD128), ring 4096 bytes
vgpu vgpu.0: [vgpu:gem] bo#1 size=65536 mmap_offset=0x100001000
vgpu vgpu.0: [vgpu:gem] bo#2 size=65536 mmap_offset=0x100011000
vgpu vgpu.0: [vgpu:ring] fd handle=3 mmap_offset=0x100000000
vgpu vgpu.0: [vgpu:cs] seq#1 ring_off=0x0 ncmds=3
vgpu vgpu.0: [vgpu:engine] seq#1 done (3 cmds)
vgpu vgpu.0: [vgpu:irq] top-half seq#1 -> bottom half
vgpu vgpu.0: [vgpu:bh] seq#1 signaling fence
vgpu vgpu.0: [vgpu:wait] seq#1 status=0
[pid 85] PASS: seqno=1 blit ok mmio ok (submitted=1 completed=1 errors=0)
=== 4-process concurrent submission ===
...
multi-process: 4/4 passed
=== negative paths (bad handle / engine fault / ring full) ===
[pid 92] PASS: wait(unknown seqno) -> -ENOENT
[pid 92] PASS: cs(bad blit handle) rejected (-No such file or directory)
[pid 92] PASS: engine fault (bad opcode) -> fence -EINVAL
[pid 92] PASS: engine fault (misaligned SET) -> fence -EINVAL
[pid 92] PASS: cs(ring full) rejected (-Device or resource busy)
negative paths: all passed
=== debugfs stats ===
submitted  9
completed  7
errors     2
ring_used  0 / 4096 bytes
--- regs ---
status = 0x00000000
seqno  = 10
error  = 0
user   = 0xdeadbeef
=== rmmod drain ===
vgpu vgpu.0: [vgpu:remove] draining and shutting down
vgpu vgpu.0: [vgpu:remove] done (submitted=9 completed=7 errors=2)
rmmod: OK (drain path clean)
=== all tests passed ===
```

全程 KASAN 内核下**零告警**（无 WARNING、无 KASAN 报告、rmmod 无资源泄漏）。

### ftrace：一次提交的完整函数图

`function_graph` + `vgpu_*` 过滤（guest 内 `/sys/kernel/debug/tracing`），一次单进程运行的真实抓取：

```
# tracer: function_graph
 3) ! 816.046 us  |  vgpu_bo_create_ioctl [vgpu]();      <- CPU3 进程上下文
 3) ! 755.332 us  |  vgpu_bo_create_ioctl [vgpu]();
 3) ! 696.781 us  |  vgpu_ring_get_ioctl [vgpu]();
 3) ! 637.190 us  |  vgpu_cs_ioctl [vgpu]();              <- Doorbell
 2)               |  vgpu_engine_work [vgpu]() {          <- CPU2 workqueue（Engine）
 3) # 1380.183 us |  vgpu_wait_ioctl [vgpu]();            <- 内核态 fence 等待被唤醒
 2) ! 322.252 us  |    vgpu_irq_handler [vgpu]();         <- 上半部：读状态、清标志、摘链
 2) ! 623.890 us  |  }
 2)               |  vgpu_irq_bh_work [vgpu]() {          <- 下半部：irq_wq
 2) ! 9.189 us    |    vgpu_finish_submission.part.0 [vgpu]();  <- signal fence、唤醒等待者
 2) ! 274.280 us  |  }
 3)   1.014 us    |  vgpu_query_ioctl [vgpu]();
```

一条提交跨了两个 CPU、两种上下文：进程态 ioctl（CPU3）→ doorbell → Engine workqueue（CPU2）→
上半部摘 submission → 下半部 signal fence → 唤醒 CPU3 上的等待者。

## 架构总览

```
 userspace (vgpu_test, 静态链接)
   │ open /dev/dri/renderD128
   │ DRM_IOCTL_VGPU_BO_CREATE ──► shmem GEM BO = "显存"
   │ DRM_IOCTL_VGPU_RING_GET ──► mmap 命令环（4096B = 64 x 64B 槽位）
   │ 往 Ring 写 vgpu_cmd（opcode + operands，64B/条）
   │ DRM_IOCTL_VGPU_CS {ring_off,ncmds} ──► Doorbell ──► vgpu_ioctl_cs
   │     1) 校验 2) spinlock 下分配 seqno + 拷 Ring 条目到 scratch
   │     3) 解析 BLIT 的 BO（持引用、pin 页）4) 建 fence 入 idr 5) queue_work(engine_wq)
   │ DRM_IOCTL_VGPU_WAIT {seqno} ──► dma_fence_wait_timeout（内核态等待，用户态永不轮询）
   │ 读回 BO 校验数据；DRM_IOCTL_VGPU_QUERY ──► 统计 + MMIO 值
 ▼
 kernel (vgpu.ko)
   vgpu_drv.c    platform probe/remove · DRM 注册 · GEM shmem · ioctls · Ring · debugfs
   vgpu_engine.c Engine workqueue（逐条 decode/执行：NOP/BLIT/SET）
                 中断上半部（spinlock，读状态、清标志、摘 submission）
                 中断下半部（irq_wq，per-sub work：signal fence、释放、唤醒）

  内存：Ring = shmem GEM BO；scratch/MMIO regs = dma_alloc_coherent；BLIT = dma_map_single 按页循环
```

opcode：`NOP`（udelay 让时序在 ftrace 可见）、`BLIT`（两 BO 间拷贝）、`SET`（写虚拟 BAR0 寄存器）。
engine 完成一个 submission 后**直接调用 `vgpu_irq_handler()` 模拟硬件中断**（ADR-0001，vgem 同款做法），
代码里仍保持真实 hardirq 的约束（spinlock、不睡眠）。

## UAPI（driver/uapi/vgpu_drm.h）

| ioctl | 作用 |
|---|---|
| `VGPU_BO_CREATE` | 建 BO，返回 GEM handle + mmap_offset |
| `VGPU_RING_GET` | 取命令环的 per-fd handle + mmap_offset（probe 期一次性分配 offset，随设备生命周期） |
| `VGPU_CS` | Doorbell：提交 Ring 里 `ncmds` 条命令，返回 seqno |
| `VGPU_WAIT` | 按 seqno 等 fence：`0` 完成、负值 = engine 错误、`-ETIMEDOUT`、`-ENOENT` |
| `VGPU_QUERY` | 统计 + 虚拟 BAR0 当前值 |

错误路径：坏 BO handle / 越界 → submit 时 `-ENOENT`；engine 解码失败 → fence 带 `-EINVAL` 并打 dmesg；
Ring 满 → `-EBUSY`；rmmod 时有未完成 submission → drain 两个 workqueue，剩余 fence 以 `-EIO` signal。

## 目录结构

```
driver/      vgpu_drv.c（DRM 设备 + ioctls）· vgpu_engine.c（Engine + 中断路径 + fence）
userspace/   vgpu_test.c（-m 并发 / -e 负路径；静态链接）
scripts/     10-build.sh · 20-initramfs.sh（生成 initramfs/root）· 30-run.sh
docs/        DESIGN.md（完整设计）· adr/（0001 模拟中断 · 0002 DRM 而非 chardev · 0003 KASAN guest）
initramfs/   构建产物（gitignore；由 20-initramfs.sh 生成）
CONTEXT.md   术语表（Ring / Doorbell / Submission / Engine / BO / Reset 的严格定义）
```

## 三个面试故事

1. **中断为什么分上下半部**：hardirq 上下文不能睡眠、必须短；上半部只读状态/清标志/摘 submission（spinlock），
   慢动作（fence signal、唤醒进程）放下半部 workqueue。上面的 ftrace 图里上半部 `322us` 跑在 Engine 同一上下文、
   下半部 signal fence 唤醒等待者，就是这个故事。
2. **一致性 vs 流式 DMA**：scratch（命令副本）和 MMIO regs（虚拟 BAR0）用 `dma_alloc_coherent`——CPU 与"设备"
   长期共享必须一致；BLIT 用 `dma_map_single`/`dma_unmap_single`——设备临时借用，期间 CPU 不碰。x86 无 IOMMU 时
   两者都是 identity no-op，但 API 路径完整，在 ARM/IOMMU 上这个区别决定正确性。
3. **命令提交如何并发安全**：Ring 是用户态共享 scratch，约定不相交槽位（测试按 pid 分槽，对应真实 GPU 的
   per-context ring）；内核侧 ring 拷贝 + seqno 分配 + 在飞链表全程 spinlock；跨上下文通知用 dma_fence +
   idr（WAIT 在内核态等）；fence 带错误时正确上报负 errno；rmmod 走 drain 保证不丢 fence。

调试踩过的两个坑（都有 commit 记录）：GEM handle 创建后不归还创建引用，BO 对象连带 vma offset 节点泄漏到
rmmod，`drm_mm_takedown` 报 "Memory manager not clean"；以及 `max_t(fence->error, 0)` 把 fence 的错误钳成 0，
让失败的提交看起来成功——这两个都是这条链上真实会犯的错。

## 当前状态与路线图

已实现并验证：注册 / GEM / Ring+Doorbell / Engine / 上下半部模拟中断 / dma_fence / 错误路径 / rmmod drain /
debugfs 可观测 / ftrace 全链路。明确不做（见 DESIGN §14 Stretch 按价值排序）：QEMU 真 PCI 设备（真 BAR +
`pci_irq_raise` + `dma_memory_rw`，只换 ADR-0001 的触发源）、GPU hang 故障注入、KASAN-off 性能基线、IOMMU 实跑。
