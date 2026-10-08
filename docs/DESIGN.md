# vgpu 设计文档

> 本文是 README 的核心（最终版将替换为真实运行 log）。
> 词汇表见 [CONTEXT.md](../CONTEXT.md)；关键决策见 [docs/adr/](adr/)。
> 参考实现：主线 `drivers/gpu/drm/vgem`（Intel 虚拟 GPU，~1500 行）。

## 1. 项目定位

纯软件模拟 GPU 驱动的完整软件框架：设备注册（platform + DRM）→ 缓冲对象（GEM）→ 命令提交（Ring + Doorbell）→ 命令执行（Engine）→ 完成通知（中断上下半部 + dma_fence）→ 用户态等待与校验。不碰任何真实 GPU 硬件。

**边界（今晚明确不做，见 §14 Stretch）**：QEMU 自定义 PCI 设备、IOMMU 真映射、KASAN-off 性能基线、GPU hang 故障注入。

## 2. 运行环境

```
┌──────────────────────── Host: xuefeiz2-server ────────────────────────┐
│  vLLM (Qwen3.8-27B, :9070)  ← 硬约束：绝不可碰                          │
│  ~/linux (v6.17.0, KASAN) ──► bzImage + vgpu.ko（同一源码树编译）        │
│  QEMU 8.2.2 + KVM（4C / 2G，串口控制台）                                 │
│   ┌───────────────────────── Guest ────────────────────────────┐     │
│   │  6.17.0 KASAN 内核 + busybox initramfs（无磁盘）             │     │
│   │  /root/vgpu.ko + /root/vgpu_test(静态) + /init             │     │
│   │  /dev/dri/card0  ← vgpu DRM 设备                            │     │
│   └────────────────────────────────────────────────────────────┘     │
└──────────────────────────────────────────────────────────────────────┘
```

- Host 上只多一个 QEMU 进程（~2G 内存）：不装模块、不重启 → vLLM 零影响（ADR-0003）。
- **实时调试**：guest 内核以 `-s -S` 启动，host 上 `gdb-multiarch vmlinux -ex 'target remote :1234'`，可直接在 vgpu 代码里打断点单步（learn-kernel 现成的 DEBUG=1 机制）。

## 3. 架构总览

```
 userspace (vgpu_test, 静态链接)
   │ open /dev/dri/card0
   │ DRM_IOCTL_GEM_CREATE ──────────────► vgpu_gem_create（shmem BO = "显存"）
   │ DRM_IOCTL_GEM_MMAP ────────────────► mmap BO / Ring 到用户态
   │ 往 Ring 写 vgpu_cmd（mmap 直接写，64B/条）
   │ DRM_IOCTL_VGPU_CS {ring_off,ncmds} ── Doorbell ──► vgpu_ioctl_cs
   │        1) 校验 ring_off/ncmds          2) 分配 Submission(seqno++, fence)
   │        3) spinlock 下拷贝 Ring 条目到 scratch   4) queue_work(engine_wq)
   │ DRM_IOCTL_VGPU_WAIT {seqno} ────────► dma_fence_wait_timeout（内核态等待）
   │ 读回 BO 校验数据
   ▼
 kernel (vgpu.ko)
   vgpu_drv.c    platform probe/remove · DRM 注册 · GEM shmem · ioctls
                 · Ring · debugfs · stats
   vgpu_engine.c Engine workqueue（逐条 decode/执行）
                 · IRQ 上半部（读状态、清标志、交接）
                 · IRQ 下半部 workqueue（fence signal、唤醒等待者）

 内存布局
   Ring    = GEM shmem BO（4096B = 64 个 64B 槽位，用户 mmap 写入）
   scratch = dma_alloc_coherent（命令副本，Engine 独占）
   MMIO    = dma_alloc_coherent struct vgpu_regs {status, seqno, error}（虚拟 BAR0）
   BO      = GEM shmem（BLIT 源/目标"显存"）
```

## 4. 关键数据结构

```c
/* 一条 "GPU 命令"，64B，用户态写入 Ring */
struct vgpu_cmd {
    __u32 opcode;      /* VGPU_OP_NOP | VGPU_OP_BLIT | VGPU_OP_SET */
    __u32 pad;
    __u32 src_bo;      /* GEM handle（BLIT） */
    __u32 dst_bo;
    __u32 src_off;
    __u32 dst_off;
    __u32 length;      /* 页对齐 */
    __u32 set_off;     /* (SET) MMIO 内偏移 */
    __u64 set_data;    /* (SET) 写入值 */
    __u32 pad2[2];     /* 凑 64B */
};

struct vgpu {          /* 设备私有 */
    struct platform_device *pdev;
    struct drm_device    *dev;
    struct workqueue_struct *engine_wq;   /* “GPU 核心”：执行命令 */
    struct work_struct    engine_work;
    struct workqueue_struct *irq_wq;      /* 下半部：处理完成（per-sub work） */
    struct vgpu_regs *regs;  dma_addr_t regs_dma;   /* 一致性映射 */
    struct drm_gem_shmem_object *ring_bo; u8 *ring_cpu;  /* Ring = GEM BO */
    u8 *scratch; dma_addr_t scratch_dma;               /* 一致性映射 */
    spinlock_t lock;              /* 保护 ring_used/seqno/链表 */
    struct list_head pending;     /* 已提交未执行 */
    struct list_head in_flight;   /* 执行中/等中断 */
    u32 next_seqno, ring_used;
    u32 stats_submitted, stats_completed, stats_errors;
    struct dentry *dbg;           /* debugfs */
};

struct vgpu_submission {
    struct list_head node;
    struct vgpu *v;               /* finish work 反向指回设备 */
    u32 seqno;
    struct dma_fence fence;       /* vgem 风格：fence + completion */
    u32 ncmds, cmds_off;          /* scratch 内偏移 */
    u32 error;
    struct completion done;       /* rmmod drain 用 */
    struct work_struct finish_work;  /* 下半部：每个 sub 一个 work */
};
```

## 5. UAPI ABI

```c
/* driver/include/uapi/vgpu.h */
struct vgpu_cs    { __u64 ring_off; __u32 ncmds; __u32 seqno; };
struct vgpu_wait  { __u32 seqno;  __u32 timeout_ms; __s32 status; __u32 pad; };
struct vgpu_query { __u32 submitted, completed, errors, ring_used;
                    __u32 r_status, r_seqno, r_error; __u32 pad; };
#define DRM_IOCTL_VGPU_CS    _IOWR(DRM_COMMAND_BASE + 0x60, struct vgpu_cs)
#define DRM_IOCTL_VGPU_WAIT  _IOWR(DRM_COMMAND_BASE + 0x61, struct vgpu_wait)
#define DRM_IOCTL_VGPU_QUERY _IOWR(DRM_COMMAND_BASE + 0x62, struct vgpu_query)
```

- opcode：`VGPU_OP_NOP=0`（udelay 使时序在 ftrace 可见）、`VGPU_OP_BLIT=1`（两 BO 间 DMA 拷贝）、`VGPU_OP_SET=2`（写 MMIO 寄存器）。
- WAIT 语义：`status=0` 完成；`-ETIMEDOUT` 超时；`-EIO` fence 带错误；`-ENOENT` seqno 不存在。**等待发生在内核**（`dma_fence_wait_timeout`），用户态永不轮询。

## 6. 命令执行模型

1. 用户态把 N 条 `vgpu_cmd` 写到 mmap 的 Ring 的 `ring_off` 处；
2. `VGPU_CS` = Doorbell：校验 → 分配 Submission（`seqno++`）→ spinlock 下拷 Ring 条目到 scratch → `queue_work(engine_wq)` → 返回 seqno；
3. Engine（单 workqueue，天然串行，模拟 GPU 引擎单执行流）逐条执行：NOP / BLIT / SET；
4. 完成后回写 `regs.seqno/error`、`regs.status |= DONE`，再**直接调用 `vgpu_irq_handler()` 模拟硬件中断**（ADR-0001）。

**并发约定（面试点）**：Ring 是共享 scratch，多进程并发写必须用不相交的槽位区（测试程序按 pid 分配槽，对应真实 GPU 的 per-context ring）；内核侧拷贝在 spinlock 下进行。
**Ring 满**：`ring_used + ncmds*64 > ring_size` → 返回 `-EBUSY`，`stats_errors++`；submission 完成后 `ring_used` 递减。

## 7. 中断路径（上下半部）

```
engine work（模拟硬件完成）
  └─ regs.status |= DONE; regs.seqno/error = …
  └─ vgpu_irq_handler(0, vgpu)            ← 模拟“硬件中断”（直接调用）
       [上半部：spinlock，禁止睡眠，动作必须短]
       读 regs.status → 清 DONE → 按 seqno 找到并摘出 Submission（list_del_init）
       → INIT_WORK(sub->finish_work) → queue_work(irq_wq)
       return IRQ_HANDLED
  └─ [下半部：irq_wq，进程上下文，可睡眠；每个 sub 一个 work，无单槽覆盖]
       signal 该 sub 的 fence（唤醒 VGPU_WAIT 等待者）
       stats_completed++ · ring_used -= · 释放 submission
```

- 为什么分两半：真实 hardirq 上下文不能睡眠、必须短；上半部只做读状态/清标志/交接，可能慢的动作（fence signal、链表操作、唤醒进程）放下半部。**面试故事 #1**。
- 本实现的"上半部"跑在 workqueue 上下文（非真实 hardirq）——模拟的代价，代码仍保持 spinlock/禁睡眠约束（ADR-0001）。

## 8. DMA 设计

| 内存 | API | 为什么 |
|---|---|---|
| scratch（命令副本） | `dma_alloc_coherent` | CPU 与 Engine 长期共享，需缓存一致 |
| MMIO region（regs） | `dma_alloc_coherent` | 模拟 BAR 寄存器：CPU 读回 + "设备" 写入，必须一致 |
| BLIT 拷贝 | `dma_map_single` / `dma_unmap_single`（按页循环） | 流式："设备"临时借用，期间 CPU 不碰 |

- x86_64 无 IOMMU 时 DMA API 实质是 identity 映射（no-op），API 与路径完整走通；开 IOMMU / ARM 上才真正做 SMMU/页表映射——"一致性 vs 流式"的面试答案就在这里（**面试故事 #2**）。

## 9. 锁设计

| 资源 | 锁 | 上下文 |
|---|---|---|
| Ring 拷贝 / ring_used / seqno 分配 | `spin_lock` | CS ioctl（进程）↔ engine（模拟硬中断路径） |
| pending / in_flight 链表、stats | `spin_lock`（下半部短持） | 进程 + workqueue |
| DRM 生命周期 | `drm_dev` 内部机制 + module 级串行 | 进程 |
| 用户态完成等待 | `dma_fence` / `completion` | 进程 |

模拟硬中断路径（engine → irq handler）一律 spinlock、不睡眠——与真实约束同形（**面试故事 #3**）。

## 10. 错误路径

| 场景 | 行为 |
|---|---|
| BLIT 引用不存在的 BO handle / 越界 offset | engine 记 `sub->error=-EINVAL`，fence 带错误 signal，`stats_errors++`，dmesg 打完整 cmd |
| Ring 满 | CS 返回 `-EBUSY` |
| WAIT 未知 seqno | `-ENOENT` |
| rmmod 时有未完成 submission | Reset：flush 两个 workqueue，剩余 fence 以 `-EIO` signal，DRM unregister，逆序释放全部资源 |
| 内存错误 | KASAN 当场红色报告（guest 内核已开） |

## 11. 可观测性

1. **结构化 pr_info**（每步带 seqno + ktime 时间戳，tag 前缀）：
   `[vgpu:probe]` · `[vgpu:gem] bo#3 64K` · `[vgpu:cs] seq#7 off=0x100 n=2` · `[vgpu:engine] seq#7 BLIT 4K ok` · `[vgpu:irq] top-half seq#7` · `[vgpu:bh] fence#7 signaled`
2. **ftrace**（guest 内 `/sys/kernel/debug/tracing`，内核已开 FUNCTION_TRACER）：
   `echo function_graph > current_tracer; echo 'vgpu_*' > set_ftrace_filter; echo 1 > tracing_on`，`cat trace` 看全链路函数图。
3. **debugfs** `/sys/kernel/debug/vgpu/`：`stats`（submitted/completed/errors/ring_used）、`ring`（hexdump）、`pending`（在飞 seqno + fence 状态）、`regs`（虚拟 BAR 当前值）。

## 12. 构建与运行

```bash
make -C ~/linux M=$PWD/driver modules    # 1) 模块（与 guest 内核同源）
make -C userspace                        # 2) 静态用户态测试
scripts/20-initramfs.sh                  # 3) busybox + /root/{vgpu.ko,vgpu_test} + /init
scripts/30-run.sh                        # 4) 启动 KVM guest（DEBUG=1 可接 gdb :1234）
# guest 内（/init 自动 insmod，也可手动）：
/root/vgpu_test              # 单进程全链路
/root/vgpu_test -m 4         # 4 进程并发
```

## 13. 面试三故事

1. **中断为什么分上下半部**：hardirq 上下文不能睡眠、必须短；上半部只读状态/清标志/交接，慢动作（fence signal、唤醒进程）放下半部（workqueue）。vgpu：上半部 spinlock 下摘 submission，下半部 signal fence。
2. **一致性 vs 流式 DMA**：coherent = CPU 与设备长期共享、缓存保持一致（命令环、doorbell、寄存器）；streaming = 设备临时借用、期间 CPU 不碰、需 map/unmap（帧缓冲搬运）。x86 无 IOMMU 时两者都是 no-op，但在 ARM/IOMMU 上这个区别决定正确性。
3. **命令队列如何并发安全**：spinlock 保护 ring 拷贝 + seqno + 在飞链表（模拟硬中断路径不睡眠）；fence + completion 做跨上下文完成通知；Ring 本身是用户态共享 scratch，约定不相交槽位（生产做法是 per-context ring）；rmmod 走 drain 路径保证不丢 fence。

## 14. Stretch 路线图（按价值排序）

1. **QEMU C 语言 PCI 设备**：真 BAR（memory_region）+ `pci_irq_raise` 真中断线 + `dma_memory_rw` 真 DMA，替换 ADR-0001 的直接调用模拟——驱动结构不变，只换触发源。
2. **GPU hang + 故障注入**：vgem 的 FAKE_HANG 风格，注入 hang → 走 reset 流程（面试高频：GPU hang 恢复）。
3. **KASAN-off 性能基线**：另构一个内核，测 submission→fence 延迟（ktime）与队列吞吐。
4. **IOMMU**：guest 开 intel-iommu，让非法 DMA 真正 fault 一次，读 DMA-fault 日志。

## 15. 与原 28 天计划的差异

| 原计划 | 本实现 | 原因 |
|---|---|---|
| chardev `/dev/vgpu`（第 1 周） | DRM `/dev/dri/card0` | ADR-0002 |
| 循环队列（通用语义） | Ring + Doorbell（GPU 提交语义） | 更贴近真实命令提交模型 |
| 设备树匹配（第 3 周） | platform_device + platform_driver（无 DT） | x86 无 DT；机制同构，README 会说明 |
| tasklet 下半部 | workqueue | 现代内核已不推荐 tasklet |
| QEMU 虚拟 PCIe 设备（技术栈里列了） | 今晚不做 | 独立项目，Stretch #1 |
| IOMMU / 性能基线 | Stretch | 见 §14 |
