# trace 分析：一个 launch 的完整生命周期

原始 trace：[raw/dequant-kv-trace.log](raw/dequant-kv-trace.log)（function_graph，5686 行，
过滤到 9 个 vgpu 函数 + 调度切换点）。复现命令见 [README.md](README.md)。

## 1. 全链路一览（trace 的结构）

一次 `/root/dequant_kv` = 一个 doorbell、一个 launch、512 线程。整个 trace 的形状：

```
CPU 0（用户态进程 dequant-289）        CPU 1（workqueue kworker）
─────────────────────────────────      ─────────────────────────────
vgpu_doorbell_ioctl  378.6us  ────────► vgpu_engine_work_compute
  （校验/解析/建 fence/queue_work）        ├─ vgpu_compute_run
vgpu_wait_ioctl     5319.4us  ◄───────  │   ├─ vgpu_run_warp × 16
  （# = 等待中被抢占）                     │   │   └─ vgpu_mem_word × 5632
                                         │   ├─ vgpu_irq_handler（上半部）
                                         │   └─ vgpu_irq_bh_work（下半部）
                                         │       └─ signal fence
                                       （唤醒 CPU 0 上阻塞的等待者）
```

## 2. 逐段标注（trace 原文节选 + 解读）

### 2.1 提交路径（CPU 0）

```trace
 0) ! 378.627 us  |  vgpu_doorbell_ioctl [vgpu]();
```

`!` = 超过 100µs 阈值。这一次 ioctl 里发生的事：校验 packet（引擎类别、边界）→
解析参数 BO 引用（`drm_gem_object_lookup` + pin）→ 分配 fence 并入 idr →
`queue_work(engine_wq[compute])` → 返回 seqno。**没有发生任何“执行”**——执行在Workqueue 里，
这就是 doorbell 型提交的意义：系统调用只负责“通知”。

### 2.2 CPU 阻塞在 fence 上（CPU 0）

```trace
 0) # 5319.430 us |  vgpu_wait_ioctl [vgpu]();
```

`#` = 该函数执行期间发生过抢占/上下文切换。5.3ms 不是“等待耗时不准”，
而是这一段的 trace 是在 autorun 全程抓的，等待期间 guest 还在跑别的阶段。
关键点是：**用户态从不轮询**——它阻塞在 `dma_fence_wait_timeout` 里，由下半部唤醒。

### 2.3 引擎执行（CPU 1）

```trace
 1)               |  vgpu_engine_work_compute [vgpu]() {
 1)               |    vgpu_compute_run [vgpu]() {
 1)               |      vgpu_run_warp [vgpu]() {
 1)   0.896 us    |        vgpu_mem_word [vgpu]();
 1)   0.163 us    |        vgpu_mem_word [vgpu]();
 1)   0.146 us    |        vgpu_mem_word [vgpu]();
  ...（共 5632 次）
```

三个值得注意的事实：

1. **跨 CPU**：ioctl 在 CPU 0，引擎在 CPU 1。提交者在哪个 CPU 上与执行者在哪个 CPU 上
   完全解耦——这是所有异步 GPU 驱动的常态，trace 里一眼可见。
2. **嵌套层次 = 驱动分层**：`engine_work`（引擎）→ `compute_run`（一次 launch）→
   `run_warp`（一个 warp）→ `mem_word`（一次访存，含 VA 走表/页解析）。
3. **5632 = 16 warp × 32 lane × 11 次访存**，与 kernel 的指令结构（9 LD + 2 ST）**逐字吻合**。
   这是“读 trace 验证对 kernel 的理解”的标准练习：数字对不上，说明你对执行模型的理解有错。

### 2.4 完成：模拟中断的上下半部（CPU 1）

```trace
 1) ! 110.593 us  |      }                    ← 最后一个 warp 结束
 1) # 1924.488 us |    }                      ← compute_run 结束
 1) ! 818.281 us  |    vgpu_irq_handler [vgpu]();      ← 上半部
 1) # 3630.384 us |  }                        ← engine_work 结束
 1) ! 738.945 us  |  vgpu_irq_bh_work [vgpu]();         ← 下半部
```

- **上半部**（`vgpu_irq_handler`）：读状态寄存器、清 DONE、按 seqno 摘下 submission、
  交接给下半部。在本实现里它跑在 workqueue 上下文（ADR-0001：engine 完成时直接调用），
  但代码保持 hardirq 约束：spinlock、不睡眠。
- **下半部**（`vgpu_irq_bh_work`）：signal fence、更新统计、回收 ring/scratch——
  这里就是唤醒 2.2 里那个阻塞等待者的地方。

## 3. 配套的系统级证据（同一次运行的 dmesg）

```
[vgpu:module] kid=1 nregs=16 smem=0 instrs=20     ← “编译期”：kernel 注册
[vgpu:gem]    bo#1 size=16384                     ← cudaMalloc
[vgpu:queue]  q#1 engine=compute prio=1           ← 建流（compute 引擎）
[vgpu:rpm]    resumed for seq#35 in 2 us          ← 设备睡了，这次提交唤醒它（2µs）
[vgpu:engine] seq#34 (engine 2) done (1 cmds)     ← 1 个 packet = 1 次“命令获取”
[vgpu:irq]    top-half seq#34 -> bottom half      ← 上下半部交接
[vgpu:bh]     seq#34 signaling fence              ← fence 被 signal
[vgpu:wait]   seq#34 status=0                     ← 等待者拿到“完成”
```

时间戳（同一运行）：doorbell 5.8527 → engine done 5.8555 → bh 5.8572 → wait 返回 5.8581。
**从门铃到用户态看到完成 ≈ 5.4ms**（其中绝大部分是引擎按块执行的 `udelay` 模拟耗时，
不是真实硬件延迟）。

## 4. 统计视图的交叉验证

`cat /sys/kernel/debug/vgpu/compute`（这次 launch 的份额，累计值里可辨）：

| 指标 | 本 launch 贡献 | 怎么算出来的 |
|---|---|---|
| kernels | +1 | 一次 launch |
| blocks / threads | +8 / +512 | grid=8 × block=64 |
| warps | +16 | 512 线程 / 32 lane |
| instrs | + 512×20 = 10240 | 每线程 20 条指令 |
| mem_ops | +5632 | 16 warp × 32 lane × 11 |
| broadcast warps | +96（6/warp） | header 六次广播读 |
| coalesced warps | +80（5/warp） | q/out/ver 三处的连续访问 |
| strided warps | +0 | 没有任何跨步访问 |

**trace 与统计互证**：trace 里 `vgpu_mem_word` 出现 5632 次，
compute 视图里 mem_ops 增量 5632——两个独立观测点给出同一个数字，这是本项目
“可观测性必须自洽”的标准 practice。

## 5. 这份 trace 的教学用法

1. **先读 trace 再读码**：只看函数图，能不能说出“用户态在这里阻塞、谁唤醒的它”？
2. **改 kernel 再看 trace**：把指令里第 8 条的偏移从 0 改成一个跨步（比如 +64），
   重跑后 `strided warps` 会从 0 变成非 0——内存合并从抽象概念变成可观测数字。
3. **改 grid 再看 trace**：grid=64 时 warp 数翻 8 倍，`waves` 计数随之变化——
   occupancy 的第一手体验。
