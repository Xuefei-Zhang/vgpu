# vgpu → GPGPU 计算卡：演进方案

> 纯设计文档（plan），不改代码。目的：把当前"图形命令提交语义"的虚拟 GPU，推进到**真实 GPGPU 计算卡（CUDA 卡）驱动的机制形态**。
> 词汇表见 [CONTEXT.md](../CONTEXT.md)；既有学习路线见 [LEARNING-PATH.md](LEARNING-PATH.md)；ftrace 手段见 [FTRACE-GUIDE.md](FTRACE-GUIDE.md)。
> 约束不变：全部演进仍发生在 QEMU/KVM KASAN guest 内，Host 零接触（ADR-0003）。

---

## 1. 这份方案要什么、不要什么

**要的（机制对齐）**：

- 提交路径：用户态组包 + 写门铃（MMIO），内核只在异常路径介入——这是所有真实 GPU 驱动高吞吐提交的根基
- 内存模型：per-context GPU 虚拟地址空间（VMID）、页表、显存上限与驱逐（TTM 语义）、统一虚拟内存（UVM）的按需迁移
- 执行模型：kernel descriptor（grid/block/warp）、SM 分发、occupancy 与 coalescing 的可观测化
- 调度：多 context 时间片、优先级、**在 kernel 执行中抢占**
- 同步：内存语义的 timeline semaphore、event slot、CUDA Graph 式 DAG 提交
- 故障：MMU fault buffer + fault containment（只恢复出事的 context）
- 功耗/时钟：runtime PM + DVFS 模拟 + per-context 计费（nvidia-smi 的数据从哪来）

**不要的（明确不做，防兔子洞）**：

- cycle-accurate SIMT 执行器——我们是**机制准确、时序近似**的模拟器：指令"执行"用整数 ns 延迟模型表达，不建流水线
- bit-accurate 的 PTX/SASS ISA——自定义 ≤10 条指令的最小集，只够 saxpy/vector 类 kernel
- cache 一致性协议（directory/MESI）——单一 L2，单一 owner
- MIG / SR-IOV 切分——记为远期边界，不进本方案

---

## 2. 参照系：真实 GPGPU 驱动长什么样

对照三栏：NVIDIA（CUDA + open kernel module）、AMD（KFD）、以及我们现在的 vgpu。**这张表就是后面每个阶段的立项理由。**

| 机制 | NVIDIA (CUDA/GSP 时代) | AMD (KFD) | vgpu 现状 | 差距 |
|---|---|---|---|---|
| 设备节点 | `/dev/nvidiactl` + `/dev/nvidiaN` + `/dev/nvidia-uvm` | `/dev/kfd` + `/dev/dri/renderD*` | 单个 `/dev/dri/card0` | 控制面/计算面/内存面未分离 |
| 上下文 | channel + VA space + VMID | process/queue + VMID | 无显式 ctx（fd 即隐式） | 无 VMID 隔离 |
| 提交 | 用户态写 **host pushbuffer**，doorbell MMIO 通知 | MQD（host 内存） + doorbell 写 KIQ | ioctl：内核每笔都在路径上 | **提交路径完全不同** |
| 命令格式 | PM4 packet 宏（SET_SH_REG / DISPATCH_DIRECT / WAIT_REG…） | PM4（同类） | 自研 64B `vgpu_cmd`（NOP/BLIT/SET） | 语义太薄，无寄存器写/等待类包 |
| 引擎 | GR（图形+计算）/COPY/NVDEC，GSP 微码调度 | gfx ring / compute ring / sdma | 一个 ordered workqueue | 无引擎抽象 |
| 内存 | VRAM + sysmem，UVM 统一 VA，按需迁移 | VRAM/GTT，SVM | shmem BO，无 VA 空间 | 无 VA、无迁移、无驱逐 |
| 同步 | semaphore memory（地址+值）、external semaphore | timeline syncobj | 每 submission 一个 dma_fence | 无内存语义 timeline、无 event |
| 图提交 | CUDA Graph（一次提交 DAG） | — | 无 | 无 |
| 抢占 | channel preemption（保存/恢复 GFX 状态） | preemption（mid-kernel via mark/set） | 无 | 无 |
| 故障 | UVM fault buffer（地址/原因/VMID）+ containment | IH ring + GPUVM fault | `sub->error` + dmesg | 无 fault buffer、无恢复策略 |
| 功耗 | runtime PM、DVFS、clock gating | rpm + DC | 无 | 无 |

**一句话结论**：当前 vgpu 的提交/执行模型停留在"ioctl + 单一环 + 单引擎"，这是 2005 年时代 UMS 的形态；真实 GPGPU 的全部设计都围绕**"用户态直提、门铃通知、GPU 侧自行取指调度"**。

---

## 3. 目标形态（终点架构）

```
userspace: vgpu_rt（"CUDA runtime lite"）
  ctx    = open("/dev/vgpuN")                      # 每 context 一个 fd
  va     = vm_alloc(ctx, size)                     # GPU 虚拟地址（per-VMID 空间）
  mem    = vm_bind(ctx, va, bo_handle)             # BO 绑进页表；或 managed=UVM
  q      = queue_create(ctx, ENGINE_COMPUTE, PRIO) # host 侧队列（MQD-lite）
  kernel = module_load("saxpy.vgpu")                # 指令 blob → kernel_id
  # 在 mmap 的命令缓冲里组包（用户态直接写，不经 syscall）：
  #   [WAIT_REG addr val] [LAUNCH grid=4 block=32 smem=0 kid=? params_va=?]
  #   [SIGNAL_TIMELINE sem_va val] [WRITE_EVENT evt_slot val] [COPY_DMA …]
  doorbell_write(q, put_ptr)                        # MMIO 写门铃 = "kick"
  timeline_wait(sem_va, val)                        # 用户态等 semaphore 内存
  dump_report(ctx)                                  # occupancy / coalescing / 时间

kernel side（仍在同一 .ko 里演进）：
  vgpu_core     device / ctx 生命周期 / rpm / 时钟 / fault buffer / debugfs(nvidia-smi 数据源)
  vgpu_vm       per-ctx VA 空间、页表（软件 walk）、VRAM 管理器（budget + 驱逐）
  vgpu_queue    队列描述符（base/size/put/get/doorbell）、引擎亲和
  vgpu_frontend 取指 worker：读门铃 → 从 put..get 取包 → 解码 → 分发引擎
  vgpu_compute  SM×warp 模拟：dispatch block、延迟模型、coalescing/occupancy 统计
  vgpu_copy     DMA 引擎（现有 BLIT 的继承者）
  vgpu_sched    context timeslice / 优先级 / 抢占（寄存器保存恢复）
```

MMIO 布局（Phase B 引入，之后所有阶段都在其上长）：

```
+0x000  STATUS      (R)  引擎状态位图
+0x004  CONTROL     (W)  全局 reset/enable
+0x008  CLOCK_HZ    (RW) DVFS  governor（Phase I）
+0x100  DOORBELL_0  (W)  queue#0 的 put pointer（用户态写）
+0x108  DOORBELL_1  (W)  queue#1
...
```

---

## 4. 阶段计划（A → J）

每阶段给：目标 / 构建什么 / UAPI 草案 / 最小验收 / 真实参照 / 工作量估计。
**所有阶段共用现有基础设施**：KASAN guest、ftrace/gdb/debugfs 三板斧、pr_info 结构化日志。

### Phase A：多队列 + 多引擎（stream 语义）

- **目标**：一个 context 持有多条 stream；每条 stream 绑定一种引擎；同 stream 内保序，跨 stream 并发。
- **构建**：queue 对象（engine/prio/size/cond）；三种引擎 workqueue（compute/copy/gfx）；每条队列独立 fence 与 in-order 游标；**跨队列只靠同步原语**（此处先用占位 event，Phase G 换成真 timeline）。
- **UAPI**：`QUEUE_CREATE{engine,prio,nslots} → qid + 门铃 mmap offset`；`QUEUE_DESTROY{qid}`。
- **验收**：三个 stream 各提交 4 个 kernel，ftrace 看到三个引擎在三个 CPU 交错执行；同 stream 的 seqno 严格递增完成。
- **真实参照**：`amdgpu` 的 gfx/copy ring；NVIDIA channel。
- **工作量**：1–2 天。**前置**：LEARNING-PATH Lab 5 的双引擎就是这个 Phase 的前半。

### Phase B：主机侧命令缓冲 + Doorbell + 前端取指（核心大关）

- **目标**：把提交路径从"ioctl 一笔一交"换成"**用户态组包、门铃通知、内核只在异常路径**"。
- **构建**：
  1. MMIO 页（上节布局）：`DOORBELL_q` 写 put pointer；
  2. host 队列描述符（MQD-lite，coherent 内存）：base/size/put/get/engine/prio；
  3. **前端 worker**：被门铃唤醒（Lab 6 之后是 IRQ，之前用 wq），从 `get..put` 取包解码，分发引擎，推进 get；
  4. packet 集（PM4 风格，每条 32–64B）：`NOP / WRITE_REG{addr,val} / LAUNCH{…} / COPY_DMA{…} / WAIT_REG{addr,val,mask} / SIGNAL_TIMELINE{addr,val} / WRITE_EVENT{slot,val} / EOP`；
  5. 用户态 lib（薄封装：组包 + sfence + 门铃写）。
- **UAPI**：ioctl 只剩 `QUEUE_CREATE / VM_* / MODULE_LOAD / QUERY`；**`VGpu_CS` 退役**（文档保留一段"为什么退役"）。
- **验收**：用户态一次门铃提交 16 个包，`dmesg` 零输出（无 pr_info 噪声，因为无异常）；ftrace 看到 `doorbell_write → frontend_fetch → dispatch` 链条；用户态测 submit→doorbell 延迟（应 <1µs，对比 ioctl 路径的 ~2µs）。
- **面试故事**：**为什么 GPU 提交走 doorbell 而不是 syscall**——kernel launch 的 CPU 侧开销预算只有几 µs，一次 ioctl 的陷入+返回就吃掉大半；这也是 GPFIFO/MQD 存在的全部理由。
- **真实参照**：`amdgpu` 的 `gfx_v9_0_ring_emit_ib`/doorbell 写；nouveau MQD；NVIDIA open-kernel-module 的 `nv50…-channel`。
- **工作量**：3–5 天。**深水提示**：先做"门铃通过 ioctl trampoline"版本（不依赖 Lab 6），MMIO 直写留到 Phase F/真中断就绪。

### Phase C：GPU 虚拟内存：VMID + 页表 + VRAM 管理器

- **目标**：每个 context 拥有独立 GPU VA 空间；BO 绑定到 VA；"显存"有容量上限，满了会**驱逐**。
- **构建**：`drm_gpuvm` 风格的 per-ctx vm；VA range 分配器（`drm_mm` 即可）；两级软件页表（PDE/PTE 都是内核页，walk 由 driver/engine 做——**模拟真实硬件里 GCMC/SMMU 的 walk**）；VRAM budget（如 32MB），LRU 驱逐到 host shmem（迁移 + 页表解绑）；`dma_resv` 挂 fence（Lab 2 的果实）。
- **UAPI**：`VM_ALLOC{size,align,managed} → va`；`VM_BIND{va,bo,flags}`；`VM_UNBIND{va}`。
- **验收**：**两个 context 用同一个 VA 地址指向各自不同 BO，互不可见**（VMID 隔离的可演示证据）；显存写满后自动驱逐，debugfs 显示 evicted 计数与性能计数器变化。
- **面试故事**：GPU VA/VMID 为什么必须 per-process；GPU 的 TTM/eviction 与 CPU swap 的同构；页表谁 walk。
- **真实参照**：`amdkfd` VMID 管理；`nouveau_vm`；TTM 的 `ttm_bo_eviction_valuable`。
- **工作量**：3–4 天。

### Phase D：计算语义：kernel descriptor / warp / SM 模拟 / coalescing 报告

- **目标**：从"只会 BLIT"变成"能 launch kernel"，并让 GPGPU 的三个核心概念**可观测**。
- **构建**：
  1. 最小 ISA（≤10 条）：`LD/ST/FMA/MOV/EXIT/…`，kernel 以"指令 blob + 寄存器数 + smem 大小"注册；
  2. **dispatch**：grid 的 block 分发给 N 个 SM（round-robin + occupancy 上限）；
  3. **warp 执行**：每个 SM 持 W=32 lane，一条指令 vector 化"执行"（memcpy/算术），**延迟模型**：L1 hit 4ns / L2 hit 20ns / VRAM 40ns（可调）；
  4. **统计上报**：occupancy%、warp divergence%、coalesced 访存比例、每 warp  stall 直方图；
  5. shared memory + block-level barrier（`BAR.sync` 指令）。
- **UAPI**：`MODULE_LOAD{blob,nregs,smem} → kid`；`LAUNCH packet{grid,block,smem,args_va,kid}`。
- **验收**：saxpy `grid=4 block=32` 结果正确读回；debugfs 输出 occupancy/coalescing 报告；**同 kernel 故意写不 coalesced 的地址模式，看统计数字变化**（一个实验讲透 memory coalescing）。
- **面试故事**：occupancy 由什么决定；warp divergence 的代价；coalescing 为何是访存性能第一杠杆。
- **真实参照**：`amdgpu` 的 `amdgpu_ring` dispatch；任何 SM 教科书模型即可——这里学的是"驱动如何把 launch 变任务"，不是 SIMT 本身。
- **工作量**：4–6 天。**护栏**：不做 cycle-accurate，延迟模型一律整数 ns。

### Phase E：统一虚拟内存（UVM-lite）+ HMM 语义

- **目标**：单一 VA 跨 host/device；GPU 访问未驻留页 → page fault → **driver 迁移页 → 重试**。
- **构建**：`managed` 段标记；SM 访存走页表，缺页产生 fault packet；frontend 暂停该 warp → 调 `migrate_vma`/`hmm_range_fault` 把页从 host RAM 迁入 VRAM（或反向释放）→ 更新 PTE → 重放该指令；统计迁移字节数/页数。
- **验收**：managed buffer 首次访问延迟是 µs→ms 量级（迁移开销可见），二次访问命中；ftrace 完整看到 `fault → migrate → pte_update → replay`；debugfs 累计迁移页数。
- **面试故事**：UVM 到底是什么（对程序员是"统一地址",对驱动是缺页迁移）；为什么 GPU 的页错误处理和 CPU 的缺页同构。
- **真实参照**：`Documentation/mm/hmm.rst`、`hmm_test`；NVIDIA `nvidia-uvm`。
- **工作量**：4–5 天。**前置**：Phase C。
- **已交付（本分支）**：`VGPU_PKT_LAUNCH_VA`（VA 命名 launch）+ `VGPU_VM_BIND_MANAGED`（标志负载化）
  + 引擎侧逐访存走提交者页表、缺 PTE 走 phase C 的按需重映射并重放（`vgpu_vm_loan`）；
  `engine vm faults` 计数器 + per-launch `va_faults`；`userspace/examples/uvm_saxpy.c`（驱逐后透明迁回、
  结果全对、计数上升，连跑两轮）。**未做**：真 HMM/migrate_vma（我们的"host memory"就是 shmem 本身，
  迁移是重装 PTE，物理搬运量恒为 0，所以 µs→ms 的延迟差在这台模拟器里没有对应物）；fault 时挂起 warp
  交给独立 fault handler 再 replay（我们是同步服务：访存路径内 migration + 立即重试，语义等价、
  少一层状态机）；`fault → migrate → pte_update → replay` 的 ftrace 四段式（迁移函数与 VM_COPY 共用，
  trace 里看到的是同一条 `vgpu_vm_loan` → `vgpu_vm_map_range` 路径）。

### Phase F：调度与抢占（context timeslice + 寄存器保存/恢复）

- **目标**：多个 context 时间片轮转；高优先级能**抢下正在跑的 kernel**。
- **构建**：per-context "寄存器文件"（模拟状态：PC + 假 GPR + warp mask）；engine 每隔 N µs 检查 timeslice → context switch（save/restore）；抢占包（前端立即让出）；优先级类（低/普通/高）。
- **验收**：两个 context 各 launch 长 kernel，debugfs 看到交替执行；注入高优先级提交，低优先级在 X µs 内让出且结果正确；ftrace 看到 `save_ctx → run_other → restore_ctx`。
- **面试故事**：**GPU 上怎么抢占一个正在执行的 kernel**（没有中断可用，靠 mark/yield 包 + 上下文保存；为什么 GPU 抢占可比 CPU 中断难得多）——面试高频。
- **真实参照**：`amdgpu` 的 `amdgpu_ctx` 优先级与 preemption；nouveau 的 channel preemption。
- **工作量**：3–4 天。**前置**：Phase D。

### Phase G：同步模型升级：timeline semaphore / event / CUDA Graph

- **目标**：从"每提交一个 dma_fence"进到**内存语义**的同步，和图提交。
- **构建**：per-ctx semaphore 内存区（mmap，地址+值）；`SIGNAL_TIMELINE/WAIT_TIMELINE` 包直接操作它；event slot 数组；**GRAPH_SUBMIT**：用户态先建 DAG（节点 = launch/copy，边 = semaphore 等待），一次 doorbell 提交整图；frontend 拓扑分发。
- **验收**：用户态 poll semaphore 内存等到完成（不碰 ioctl）；一次图提交减少 N−1 次 doorbell，**把 CPU 侧提交开销从 O(N) 降到 O(1) 并测出来**。
- **面试故事**：显式同步为什么取代隐式（CPU-GPU 拥抱）；CUDA Graph 的节省本质是什么；VK timeline semaphore 和它的同构。
- **真实参照**：`drm_sched` 的 job 依赖；AMD/Intel 的 `dma_fence_chain`；userspace 的 `sync_file`。
- **工作量**：3–4 天。**前置**：Phase B。

### Phase H：故障模型：fault buffer + fault containment

- **目标**：非法 VA 访问 → 结构化 fault 记录 → **只恢复出事的 context，不 reset 全卡**。
- **构建**：fault packet 从 SM 上报；host 内存里的 fault buffer ring（记录 addr/reason/vmid/ctx）；containment 策略：杀掉该 context 后续提交 + 恢复页表 + 允许其他 context 继续；可选：注入 ECC-lite 单比特错误演示 RAS。
- **验收**：launch 一个故意越界的 kernel → fault buffer 有完整记录、该 ctx 被隔离、其他 ctx 的提交不受影响；对比"整卡 reset"的旧行为。
- **面试故事**：GPU fault containment 的意义（数据中心里一张卡一个 context 出错不能拖垮整机）；GPU RAS 与 row remapping。
- **真实参照**：NVIDIA UVM fault buffer；`amdgpu` 的 `gmc` fault 处理。
- **工作量**：3–4 天。**前置**：Phase C + D。

### Phase I：功耗与时钟：runtime PM + DVFS 模拟 + per-context 计费

- **目标**：GPU 会"睡觉"，也会"变频"；每个 context 的用量可查（nvidia-smi 的数据源）。
- **构建**：`pm_runtime` 挂 probe/remove（idle → power gate）；DVFS governor（debugfs 写目标 MHz，frontend 按负载调）；per-context 记账：执行时间、显存峰值、提交数；clock/reset 进 debugfs。
- **验收**：idle 3 秒后 `rpm_suspend`（dmesg+ftrace 为证），下一次提交测出 resume 延迟；debugfs 改时钟 → 统计报告的延迟随之变化（**一个 knob 演示 DVFS 对 kernel 时间的影响**）。
- **面试故事**：GPU runtime PM 的难点（设备可能正被 doorbell 唤醒）；DVFS 为什么在 GPU 上比 CPU 保守。
- **真实参照**：`amdgpu` 的 `amdgpu_pm` / `amd_gpu_reset`；Intel 的 `intel_runtime_pm`。
- **工作量**：2–3 天。**前置**：Phase D（否则没有"负载"可调）。

### Phase J：可观测性：vgpu-smi-lite

- **目标**：host 侧一个用户态工具，输出 nvidia-smi 风格：每进程/每 context 的显存、利用率、队列深度、时间片分配、fault 统计。
- **构建**：debugfs/procfs 汇总前面所有计数器；host 端小工具（静态链接进 initramfs，或 host 直接跑 netcat 也行）；接入 guest 的 `run_test.sh` 做回归。
- **验收**：跑多 context 混合负载，工具输出与 kernelshark/ftrace 数字一致（**交叉验证观测数据本身**）。
- **面试故事**：可观测性设计——计数器放哪、原子性、跨 CPU 汇总。
- **工作量**：2 天，随时可做（越早做，后面每阶段的验收越省力）。

---

## 5. 依赖与推荐路线

```
        ┌──────────────── Lab 6（真 IRQ，随时）────────────────┐
        ▼                                                     │
  A(多引擎) ──► B(门铃+前端) ──► D(计算语义) ──► F(抢占) ──► H(故障)
        │            │                                      ▲
        │            └──► G(同步/图) ────────────────────────┤
        └──► C(VM/页表/显存) ──► E(UVM 迁移) ────────────────┘
  I(功耗/DVFS) 依赖 D；  J(观测) 任意时刻开始并贯穿全程
```

**推荐顺序**：`A → B → C`（三者的前置都已具备，可并行）→ `D` → `E/F/G` 三选一按兴趣 → `H → I → J`。
每阶段独立 commit + 一个可 demo 的验收，**机制必须眼见为实**（ftrace/debugfs 证据），不接受"看起来对"。

---

## 6. 深水区与护栏（写死在方案里）

| 诱惑 | 护栏 |
|---|---|
| 把 SM 做成 cycle-accurate | 只建"指令类型 × 内存层级"的整数 ns 表，不建流水线、不做 warp issue 模拟 |
| 设计更丰富的 ISA | ≤10 条指令；新增指令必须服务于一个演示（否则不加） |
| 做 cache 一致性协议 | 单一 L2 + 单一 owner，任何"一致性"需求用 fence/barrier 语义表达 |
| 显存管理器做成完整 TTM | 只做 LRU 驱逐 + budget，不做 buddy 碎片整理 |
| MIG / SR-IOV | 不进本方案；作为"远期边界"写进 docs |
| 每阶段都追求性能 | 这是教学驱动：性能数字唯一用途是**证明机制差异**（doorbell vs ioctl、graph vs 逐笔） |

---

## 7. 面试故事沉淀清单（每阶段至少贡献一个）

1. （现有）中断上下半部划分 2. （现有）一致性 vs 流式 DMA 3. （现有）命令队列并发安全
4. doorbell vs syscall：提交路径的吞吐预算（B）
5. GPU VA/VMID 隔离 + 页表谁 walk（C）
6. 显存驱逐/swap：TTM 与 CPU swap 同构（C/E）
7. UVM：统一地址背后是 driver 的缺页迁移（E）
8. GPU 上如何抢占 kernel：无中断、mark/yield、上下文保存（F）
9. 显式同步为何取代隐式；CUDA Graph 省的是 CPU 提交开销（G）
10. fault containment：为什么不能一 fault 就 reset（H）
11. runtime PM/DVFS：GPU 的睡眠与变频为何保守（I）

加上 [DESIGN.md §13](DESIGN.md) 原有三故事，演进完成后是一套 11 篇的"GPU 驱动机制故事集"——每篇都有这个 repo 里的可运行证据。

---

## 8. 与现有实验/文档的衔接

- [LEARNING-PATH.md](LEARNING-PATH.md) 的 Lab 1–5（per-context ring、dma_resv、sync_file、hang/reset、双引擎）分别是本方案 **A/C/G/F 的前置**，顺序天然衔接。
- [FTRACE-GUIDE.md](FTRACE-GUIDE.md) §10 实验 7（irq 事件里为什么没有 vgpu）= Lab 6 = **Phase B 的 MMIO doorbell 前提**。
- 现有 64B `vgpu_cmd` 就是 PM4 packet 的学前版——Phase B 是它的正式化。
- 每阶段验收沿用同一套：`run_test.sh` 加阶段用例 + ftrace 片段贴文档 + commit message 写清"机制是什么、证据是什么"。

---

## 9. 进度（随开发更新）

| 阶段 | 主题 | 状态 | commit |
|---|---|---|---|
| A | 多引擎多队列（stream 语义） | ✅ 已完成并 guest 验证 | `1657cdc` |
| B | 门铃 + 引擎侧取指（packet 提交） | ✅ 已完成并 guest 验证 | `8ebd16f` |
| D | 计算语义：kernel launch / warp 执行 / 合并率 | ✅ 已完成并 guest 验证 | `f6b9b0c` |
| C | GPU 虚拟内存：VMID / VA / 页表 / VRAM 预算驱逐 | ✅ 已完成并 guest 验证 | 合并于 main |
| F | 抢占与时间片 | ✅ 已完成并 guest 验证 | `74e5142` |
| G | timeline semaphore / event / 图提交 | ✅ 已完成并 guest 验证 | `b19abbf` |
| J | vgpu-smi-lite 观测工具 | ✅ 已完成并 guest 验证 | `a71f6ba` |
| I | RPM / DVFS / per-context 计费 | ✅ 已完成并 guest 验证 | `28d6a3f` |
| E | 统一虚拟内存：managed VA + 引擎侧按需迁移 | ✅ 已完成并 guest 验证 | 本分支 `phase/e-uvm` |
| H | fault containment / fault buffer | ⬜ 未开始（C 已落地，无阻塞） | — |
**已交付内容**：per-fd 多队列（每队列独立 ring + fence timeline + doorbell）、三个引擎（gfx/copy/compute）各自
ordered workqueue + 各自 scratch、packet 格式 + VGPU_DOORBELL + 引擎侧 fetch/decode、7 指令教学 ISA +
warp 同步执行器 + LAUNCH packet + MODULE_LOAD 模块注册 + broadcast/coalesced/strided 三类访存统计、
debugfs `queues`/`compute` 视图、`-q`/`-p`/saxpy 测试与 autorun 集成。
**Phase C 新增**：per-fd GPU 地址空间（每 fd 一个 VMID + 自己的两级软件页表，driver 做这个 walk——模拟
真实硬件里 GCMC/SMMU 的 walk）、`VM_ALLOC`/`VM_BIND`/`VM_UNBIND` ioctl、`VGPU_PKT_VM_COPY`（VA→VA DMA，
提交时通过提交者的页表解析，引擎只看到页）、设备级 VRAM budget + LRU 驱逐 + 按需重映射（transparent
re-map）、debugfs `vm` 视图（每 context 的 VA 区间状态 + budget/evicted/fault 计数器，可写 budget）。
**实例**：`userspace/examples/saxpy.c`——CUDA SAXPY 的 vgpu 对应实现，4096 元素全对；
入门/测试/trace/实例教学见 `docs/GETTING-STARTED.md`。

**Phase E 新增**：`VGPU_PKT_LAUNCH_VA`——**VA 命名的 kernel launch**（`base_va` + `size`，不带 GEM handle），
kernel 的 LD/ST 于是都是 GPU VA，逐个通过**提交者的页表**解析；`VGPU_VM_BIND_MANAGED` 终于有负载
（只有 managed 区间允许被 VA 命名 launch 寻址）；`vgpu_vm_loan()`/`vgpu_vm_unloan()`——引擎侧借一页
做一次访存，缺 PTE 就走 phase C 的按需重映射（**复用，非重写**）并把访问重放；计数器：
`demand-faults`/`remapped`（phase C 的，引擎 fault 同样计入）+ compute 视图新增 `engine vm faults`
（把"内核自己缺页"与"VA→VA DMA 缺页"分开）+ 每次 launch 的 `va_faults` 逐 kernel 统计。
**实例**：`userspace/examples/uvm_saxpy.c`——与 saxpy **同一份 ISA 程序**，只把参数 buffer 从 handle
命名换成 VA 命名：CPU 经 mmap 写 in[]、设备经页表读写的都是同一批 shmem 页，budget 压到 0 驱逐后
launch，首次缺页即迁移回驻，结果全对且 fault 计数上升（连跑两轮证明是机制不是侥幸）；同一 buffer
不带 `MANAGED` 标志再绑一次，VA 命名 launch 被 `-ENOENT` 拒绝（标志是承重墙）。

**过程中抓到并修复的 4 个真 bug**（都有 guest 证据，写入各自 commit message）：
1. 自旋锁内调可睡眠 API（`drm_gem_shmem_pin` 的页分配 + `set_pages_array`）
2. `container_of` 数组元素陷阱：减了数组基址偏移导致 engine 1/2 的设备指针错位
3. 设备级 scratch 水位按**完成序**回收——多引擎并发才暴露的分配/回收乱序
4. **三种尺寸混淆**：ring 槽 64B / command 结构 48B / packet 40B，混用导致 scratch 水位下溢 48B/批

**Phase C 过程中抓到并修复的 3 个真 bug**：
5. `list_add_tail(new, &prev->node)` 用错：该宏第二个参数是**链表头**，于是新区间被插到 `prev` **之前**，
   保留区间不再按地址有序 → 分配器把同一个 VA 发第二次，两个 buffer 共享一组 PTE（隔离直接失效）
6. seq_file show 回调的 `data` 参数是 `SEQ_START_TOKEN`（`(void*)1`）而不是私有指针——从 `data` 取设备指针
   等于解引用 1，KASAN 当场 panic；驱动其余 show 函数都用 `s->private`
7. VM_BIND 引用计数：`drm_gem_object_lookup` 的引用 + `drm_gem_object_get` 共拿了两个引用，unbind 只还一个，
   每个 bind 泄漏一个对象引用

**下一步建议**：C/E 已落地（E 用 VA 命名 launch + 引擎侧按需迁移补完了 UVM 的"迁移"半边）；
剩下的 H（fault containment）只依赖 C+D，是最后一块未交付的机制。
## 10. 里程碑总览

| 阶段 | 主题 | 工作量 | 面试故事 |
|---|---|---|---|
| A | 多队列多引擎 | 1–2 天 | 引擎抽象 |
| B | 门铃 + 前端取指 | 3–5 天 | doorbell vs syscall |
| C | VMID/页表/显存管理 | 3–4 天 | VA 隔离、TTM |
| D | 计算语义 warp/SM | 4–6 天 | occupancy/coalescing |
| E | UVM 按需迁移 | 4–5 天 | 统一虚拟内存 |
| F | 抢占与时间片 | 3–4 天 | GPU 抢占 |
| G | timeline sem/图 | 3–4 天 | 显式同步、CUDA Graph |
| H | fault containment | 3–4 天 | GPU RAS |
| I | RPM/DVFS/计费 | 2–3 天 | GPU 功耗管理 |
| J | vgpu-smi-lite | 2 天 | 可观测性 |

合计约 30–40 个工作日。**任何阶段做完都可独立成为简历上的一条**——每个阶段都有清晰的机制、接口、可运行证据和面试叙事。
目前 A+B 已交付（占约 1/4），工作树 `~/self/wt/{a,b,c}` 与分支 `phase/{a-queues,b-doorbell,c-vm}` 已就绪，C 可直接接力。
