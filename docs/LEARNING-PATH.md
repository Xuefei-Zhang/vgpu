# vgpu 学习路线：从「能跑」到「像真驱动」

> 主线功能已全部实现并验证（见 [README](../README.md)），本文管剩下的部分。
> 分两类：**补缺**（真实驱动有、我们因为教学简化而缺的机制）和 **拓展**（DESIGN §14 的 Stretch）。
> 每关都设计成「一小时上下、可独立验收」：先看现状的破绽 → 再动手 → 用 ftrace/gdb/debugfs 看进去 → 验收。

## 1. 当前缺口清单（对照真实 GPU 驱动）

| # | 缺口 | vgpu 现在怎么做的 | 真实驱动怎么做 | 补上它补上什么认知 |
|---|---|---|---|---|
| 1 | **Ring 是设备级共享 scratch** | 一个 ring BO 发给所有 fd，多进程靠"不相交槽位"君子协定（CONTEXT.md 明说这是简化） | per-context ring：每个 `drm_file` 自己的环；一个 context 还常有多环（amdgpu: GFX/COMPUTE/SDMA） | 上下文是隔离与调度的基本单位 |
| 2 | **没有 exec 期校验/pin** | CS ioctl（submit 时）就把 BLIT 的 BO 解析好、页 pin 好，一直到 finish 才 unpin | 执行器在跑之前才 lock reservation、pin/unpin 是短命的；还有"未提交校验"两阶段模型 | 提交与执行的边界 |
| 3 | **没有 reservation（dma_resv）** | BO 完全不知道有谁在用它；两个提交同时 BLIT 同一对 BO 不互斥 | BO 的 `dma_resv` 挂 in-flight fence，执行前等依赖 | 显存访问排序/隐性同步 |
| 4 | **fence 只能在内核态等** | 只有 `VGPU_WAIT` 阻塞 ioctl | sync_file 导出 fence fd，用户态 poll/epoll；Vulkan/GL timeline semaphore | fence ABI 全貌 |
| 5 | **单引擎单队列** | 一个 ordered workqueue 串行跑 | 多硬件引擎 + `drm_gpu_scheduler`（entity/优先级/运行队列） | 调度器为什么存在 |
| 6 | **Reset 只由 rmmod 触发** | `vgpu_drain()` 只在 remove 里调；watchdog 超时只 signal 等待者，不做恢复 | hang 检测 → 引擎停止 → 排空 → 错误 fence → 恢复 → 重放 | GPU hang 恢复（面试高频） |
| 7 | **中断是直接函数调用** | ADR-0001：engine 跑完直接 `vgpu_irq_handler()` | 写 doorbell 寄存器 → 设备 raise IRQ → hardirq 上半部 | 硬件中断路径 |
| 8 | **`VGPU_WAIT` 超时路径无测试** | 只测了 happy path | `-ETIMEDOUT` 是一等公民语义（DESIGN §5 列了没测） | 边界与超时语义 |
| 9 | **DMA/IOMMU 是 no-op** | x86 无 IOMMU，map/unmap 是 identity | IOMMU 真翻译；非法访问真 fault | 一致性 vs 流式的实际差别 |

驱动结构上还差一个：**`.open`/`.postclose` 回调**（缺口 1 的载体，顺便学 drm_file 生命周期）。

## 2. 关卡总览（按依赖排序，建议顺序）

| 关 | 主题 | 类型 | 预计 | 一句话验收 |
|---|---|---|---|---|
| 0 | `VGPU_WAIT` 超时：把没测的 `-ETIMEDOUT` 补上 | 补缺 | 30 min | `-e` 多两条 PASS，超时不消费 fence |
| 1 | per-context Ring + `drm_file` 生命周期 | 补缺 | 1–1.5 h | 两个进程各用自己的 ring，槽位约定删除 |
| 2 | `dma_resv`：让 BO 知道谁在用它 | 补缺 | 1.5 h | 同 BO 并发 BLIT 真正串行，数据不再错乱 |
| 3 | sync_file：把 fence 递给用户态 | 补缺 | 1 h | 用户态 `poll()` 等到完成 |
| 4 | hang 注入 + 完整 Reset 恢复 | 补缺 | 1.5 h | 注入 hang 后设备可恢复、后续提交正常 |
| 5 | 双引擎 + 最简调度 | 拓展 | 2 h | ftrace 里看到两个引擎在俩 CPU 上真并行 |
| 6 | platform → PCI 设备 → 真 IRQ 线 | 拓展（ADR-0001 兑现） | 半天 | Doorbell 写 MMIO → QEMU raise IRQ → hardirq |
| 7 | IOMMU 开起来，看 DMA 真映射 + 真 fault | 拓展 | 1.5 h | dmesg 有 DMAR；注入坏地址有 DMA fault |
| 8 | KASAN-off 性能基线 | 拓展 | 2 h | 拿到 submit→fence 的 p50/p99，两内核对比 |

推荐前三关：**0 热身 → 1 认知升级最大 → 4 面试直接相关**；2 最难也最值钱，建议在 1 之后。

---

## 3. 逐关详情

### Lab 0 — 把 `VGPU_WAIT` 的超时补成测试（30 min）

**概念**：`dma_fence_wait_timeout()` 的超时语义；超时不等于 fence 被消费。

**① 先看现状**：`userspace/vgpu_test.c` 的 `run_negative()` 里全是"必然失败/必然成功"的路径；`timeout_ms=1` 从没出现过。

**② 动手**：在 `run_negative()` 末尾加一段（建议照抄现有 64-NOP 填环的技巧）：

1. 64 条 NOP 填满 ring 提交（engine 要 64×10µs 才跑完，窗口稳定）；
2. **同一个 seqno** 先 `timeout_ms=1` → 断言 `status == -ETIMEDOUT`；
3. 再 `timeout_ms=5000` → 断言 `status == 0`。

**③ Trace**：
```sh
# guest 内
echo function_graph > /sys/kernel/debug/tracing/current_tracer
echo 'vgpu_wait_ioctl dma_fence_wait_timeout __switch_to' > /sys/kernel/debug/tracing/set_ftrace_filter
echo 1 > /sys/kernel/debug/tracing/tracing_on
/root/vgpu_test -e ; echo 0 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/trace | grep -A2 -B2 vgpu_wait
```
能看到 `dma_fence_wait_timeout` 里 schedule 出去、被 bh 唤醒再回来；同时 `/sys/kernel/debug/vgpu/pending` 在超时瞬间应显示 `in-flight seq#N`（**说明超时的 fence 还在系统里，没被吃掉**——对应 `vgpu_wait_ioctl` 里 `consumed` 标志的逻辑）。

**④ 验收**：`-e` 输出新增两条 PASS；`submitted/completed` 统计不被超时污染。

**面试一句话**：fence 超时 → fence 本身没 signaled、也没被释放；这既是"为什么等待者要持引用"的答案，也是 watchdog 存在的理由。

---

### Lab 1 — per-context Ring：`drm_file` 的第一次（1–1.5 h）

**概念**：context（= 一个打开的 fd）是 GPU 驱动里隔离、调度、复位的基本单位。

**① 先看现状**：当前 `vgpu_ring_get_ioctl()` 把**同一个** `v->ring_bo` 的 handle 发给每个 fd；`vgpu_test.c` 按 pid 分槽位。跑 4 进程测试后看 debugfs：

```sh
cat /sys/kernel/debug/vgpu/ring      # 只有一份环，4 个进程的命令混在同一页里
```
这就是"共享 scratch"的字面含义——**一个进程写错槽位就能踩别人的命令**。

**② 动手**（按序）：

1. `struct vgpu_driver` 加 `.open` / `.postclose`（现在没有，看 `drivers/gpu/drm/drm_drv.c` 里谁实现了这俩找范式）；
2. 每个 `drm_file` 私有链表/xarray 存自己的 ring BO（参考主线的 per-file GEM 对象管理）；
3. `VGPU_RING_GET`：该 fd 没有 ring 就建一个（`drm_gem_shmem_create` + `drm_gem_create_mmap_offset` + handle），有就直接返回；
4. **最难也最有收获的一步**：`ring_used`/scratch 当前是**设备级**的，现在要变成 per-ring 的——`vgpu_cs_ioctl` 必须先从 `file` 找到"这个 fd 的 ring"再校验；
5. `.postclose` 里逐个释放该 fd 的 ring BO（注意与 rmmod drain 的竞态：remove 时可能仍有 fd 没关——这正是真实驱动 "lastclose" 处理的事）；
6. `vgpu_test.c`：删掉 slot 约定，所有进程 `ring_off=0` 用自己的环。

**③ Trace**：
```sh
# 4 进程跑起来时，另开一串口执行：
cat /sys/kernel/debug/vgpu/stats     # ring_used 变成"每环各自计数"的语义
```
本关结束后 debugfs 应新增一个可观测点（自己加，比如 `rings` 文件列出每个 fd 的环大小/已用字节/seqno 水位线）。
ftrace：`echo 'vgpu_*open* vgpu_*close* vgpu_ring_get*' > set_ftrace_filter`，看 4 个进程各 open 一次、各拿各的环。

**④ 验收**：两个进程的 `ring_off=0` 互不干扰；恶意测试：故意让进程 A 写 64 条、进程 B 也写 64 条，都成功（以前会 `-EBUSY`）。

**面试一句话**：为什么每 context 独立 ring？——隔离（一个 context 的 bug/写爆不能影响别人）、调度（优先级按 context 给）、复位粒度（只 reset 出事的 context，而不是整卡）。

**参考**：`drivers/gpu/drm/vgem/vgem_drv.c`（per-file 对象）；`drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c`（注释里把多环调度讲得很清楚）。

---

### Lab 2 — `dma_resv`：让 BO 知道谁在用它（1.5 h，最难）

**概念**：reservation object = BO 的"正在被谁读/写"登记簿；执行前等依赖 = 隐性同步。

**① 先看现状**（**先制造破绽再修**）：写一个测试：两个进程对**同一对** src/dst BO 反复 BLIT 不同 pattern，100 轮后校验 dst 内容 == 最后一次 pattern。现在这个测试会**时对时错**——因为第二个 submission 的 memcpy 和第一个可以交错（KASAN 不一定报，但数据错乱是必现的偶发）。这就是"没有同步"的活样本。先跑出来，存好日志。

**② 动手**：

1. `vgpu_resolve_blit()` 里对 src/dst 调 `drm_gem_lock_reservations()`（学锁序：先 src 后 dst，永远一致，防 AB-BA）；
2. 拿到 submission 的 fence 后 `dma_resv_add_fence()` 加到两个 BO 的 resv 上（`vgpu_finish_submission` 的 unpin 处就是现实里 fence 生命周期的镜像）；
3. **执行器要消费依赖**：`vgpu_blit()` 前，对 resv 里的既有 fence `dma_fence_wait()`/`dma_fence_chain` 思想的最小版——ENGINE 在跑 BLIT 前发现 BO 上挂着别人的未完成 fence，就等它（教学版：直接等；生产版：拆 job 挂依赖让 scheduler 排）；
4. 把 pin 从 submit 期挪到执行前后（对照缺口 2），体会"提交"与"执行"的边界。

**③ Trace**：给自己加个 debugfs 文件 `resv`，打印每个已创建 BO 上挂的 fence（seqno + signaled?）。然后跑第 1 步那个偶发测试：这次应看到第二个 submission 在 `dma_fence_wait` 里挂住，ftrace 里能看到它的 `vgpu_engine_work` **等**第一个 bh signal 后才被唤醒。

**④ 验收**：偶发测试 100/100 稳定通过（贴出前后对比日志）。

**面试一句话**：隐性同步（内核自动串行化同一显存的访问）简单但锁死并行度——这正是多年业界（amdgpu/Mesa）往**显式 sync**（用户态管 fence 依赖）迁的原因；面试能讲清这个 trade-off 非常加分。

**参考**：`dma_resv` API 文档；`drivers/gpu/drm/v3d/v3d_gem.c`（小驱动，fence 管理很清楚）。

---

### Lab 3 — sync_file：把 fence 递出内核（1 h）

**概念**：dma_fence 是内核对象；用户态要的是 fd + `poll()`。

**① 先看现状**：`VGPU_WAIT` 把等待关在内核里；`vgpu_test` 只能在 ioctl 里阻塞。

**② 动手**：新增 ioctl `VGPU_SYNC_FILE {seqno, fd}`：

1. `idr_find` 拿 fence → `sync_file_create(fence)` → `fd_install()` 返回 fd（`include <linux/sync_file.h>`）；
2. 用户态 `vgpu_test -p` 模式：`poll()` 等这个 fd，`revents & POLLIN` = 完成；再 `SYNC_IOC_FILE_INFO`（或直接 close）拿状态；
3. 处理 fence 已被 WAIT 消费出 idr 的竞态（FENCE 生命周期管理，衔接 Lab 1 的引用计数训练）。

**③ Trace**：ftrace `sync_file_create` + 用户态 poll 唤醒链；`/proc/self/fdinfo` 看导出的 fence 状态。

**④ 验收**：`-p` 模式等到完成且拿到正确状态；把 fd 传给 `poll` 之外再试 `read()`（sync_file 的 READ_ONLY 语义）。

**面试一句话**：所有现代用户态 GPU API（Vulkan timeline semaphore、EGL、VAAPI）底层都是 sync_file fd——"用户态为什么能等 GPU"的标准答案就在这一层。

**参考**：`drivers/dma-buf/sync_file.c`（全中文注释级的短文件）；`drivers/gpu/drm/panfrost` 的 job done ioctl。

---

### Lab 4 — hang 注入 + 完整 Reset（1.5 h，面试高频）

**概念**：GPU hang 恢复的标准流程：检测 → 停引擎 → 排空在飞任务 → 错误 fence → 恢复。

**① 先看现状**：CONTEXT.md 定义了 **Reset**（"Stop the Engine, drain the Ring, mark outstanding Fences with an error"），但唯一触发路径是 rmmod；`vgpu_watchdog_work()` 超时只 `dma_fence_signal(-ETIMEDOUT)`，**engine 还在傻等**。也就是说：现在如果 engine 卡死，watchdog 是个只能"放等待者走"的半成品。

**② 动手**：

1. debugfs 加故障注入：`echo 1 > /sys/kernel/debug/vgpu/inject_hang` → engine 在下一条命令上永睡（模拟真 hang）；
2. 扩展 `vgpu_watchdog_work()`：触发时执行真 reset——停 engine、flush 两个 workqueue、把剩余 submission 以 `-EIO` signal（复用 `vgpu_drain()` 的逆序释放逻辑）、清 resv（衔接 Lab 2）、重新 arm watchdog；
3. 教学闭环：reset 后**再提交一次**，必须正常完成（设备恢复的证明）。

**③ Trace**：hang 期间观察 `/sys/kernel/debug/vgpu/pending`（在飞 seqno 卡住不动）；reset 瞬间 ftrace `vgpu_watchdog_work vgpu_drain vgpu_finish_submission` 的完整调用图；dmesg 里应有结构化的 `[vgpu:reset]` 日志（seqno + ktime，参考现有 pr_info 风格）。

**④ 验收**：注入 → 等待者拿到错误 → 后续提交 100% 成功 → `stats_errors` 与事实一致 → 无 KASAN 告警。

**面试一句话**：GPU hang 恢复 =检测（看门狗/心跳）→ 停引擎 → 排空 + 错误 fence → 重置 → 让应用重试；能讲出"在飞的 fence 必须以错误 signal 而不是静默丢弃"就赢了。

**参考**：`drivers/gpu/drm/vgem/vgem_drv.c` 的 fake hang；`drivers/gpu/drm/amd/amdgpu/amdgpu_job.c` 的 timeout 回调。

---

### Lab 5 — 双引擎 + 最朴素的调度（2 h）

**概念**：为什么会有 `drm_gpu_scheduler`：多硬件队列 + 公平 + 优先级。

**① 先看现状**：一个 ordered workqueue，提交天然串行；ftrace 里 `vgpu_engine_work` 永远只在一个 CPU 上。guest 有 4 个 CPU，闲着 3 个。

**② 动手**：

1. 第二个 workqueue（"copy 引擎"）+ `vgpu_cmd` 的 UAPI 加 `engine` 字段（前后兼容：默认 0）；
2. CS ioctl 按 engine 分发；一条 submission 内可以混排两个引擎的命令（跨引擎顺序由 Lab 2 的 resv 保证——**两关合起来用**）；
3. 故意造一个跨引擎场景：引擎 A BLIT src→tmp、引擎 B BLIT tmp→dst，同一 BO `tmp`——验证 resv 把两条命令排对了序，而两条命令**本身**在两个 CPU 上并行处理。

**③ Trace**：这是最漂亮的一幕——
```sh
echo 'vgpu_engine_work vgpu_engine_copy_work vgpu_blit' > set_ftrace_filter
# function_graph 下看两条工作项落在不同 CPU、时间交叠
```
debugfs 加 per-engine 深度/利用率。

**④ 验收**：交叠的 ftrace 时间线截图 + 结果正确。

**面试一句话**：单队列永远串行 → 多引擎需要调度 → 引入 entity/rq/优先级 → 这就是 `drm_gpu_scheduler` 存在的全部理由；再说一句"再往下就是 credit/gang submit"，收尾。

**参考**：`drivers/gpu/drm/scheduler/sched_main.c` 的文档级注释。

---

### Lab 6 — platform → PCI → 真 IRQ（半天，ADR-0001 兑现）

分两半，每半天都可独立验收：

**6a 驱动侧 PCI 化**：`platform_driver` → `pci_driver`，probe 里 `ioremap` BAR；中断路径改为 `request_irq` 注册的 handler，engine 完成不再直接调函数，而是写 doorbell 寄存器"按门铃"。此步中断仍由测试代码 `generic_handle_irq` 模拟触发，但驱动侧代码形状已经和真驱动一致（probe/remove/irq 注册/BAR 映射）。

**6b QEMU 侧设备**：写一个最小 QEMU C 设备（`memory_region_init_io` 的 BAR + `pci_irq_raise`），`scripts/30-run.sh` 加 `-device vgpu`。门铃写下 → QEMU raise → guest hardirq → 我们的上半部。**ADR-0001 到此兑现**，DESIGN §7 的"上半部跑在 workqueue 上下文"注释可以删掉。

**Trace**：`ftrace` 看 `handle_irq` → `vgpu_irq_handler`；`/proc/interrupts` 出现 vgpu 行；`dmesg` 里 KVM 虚拟中断计数。

**面试一句话**：从"直接调 handler"到"写 BAR→设备 raise IRQ→hardirq"——结构一行没变，变的只是触发源；这句话就是 ADR-0001 的 Consequences 段落最好的执行注解。

---

### Lab 7 — IOMMU：让 DMA 不再是 no-op（1.5 h）

**① 动手**：`scripts/30-run.sh` 的 `-append` 加 `intel_iommu=on iommu=pt`（改前先备份，启动确实变慢，但 2G 内存够）。

**② 观察**：dmesg 出现 DMAR/IOMMU 初始化；此时 `dma_map_single` 不再是 identity——用一个 debugfs  dump 把 BLIT 用的 `dma_addr_t` 打出来和物理地址对比（教学点：同一份驱动代码，语义跟着平台变）。

**③ 注入 fault**：临时把 `vgpu_blit()` 的一个 `daddr` 改成一个越界值，`dma_unmap_single`/设备访问时看 DMA fault 日志（`dmesg | grep -i dmar`），然后把代码改回来。

**面试一句话**：面试故事 #2（一致性 vs 流式）的完整闭环——x86 无 IOMMU 时两者都 no-op，"在 ARM/IOMMU 上区别决定正确性"这句话你要能拿出这次 fault 日志当证据。

---

### Lab 8 — KASAN-off 性能基线（2 h）

**① 动手**：copy 一份 `~/linux/.config` 关掉 `CONFIG_KASAN` 另编 bzImage；`vgpu_test` 加 `-b N` 基准模式：N 次 submit→wait，用 `ktime_get_ns()` 记 submit/finish 时间戳，输出 p50/p99/max。

**② 观察**：两内核对比；再关掉驱动里的 `pr_info`（保留一套 behind `debug` 开关）对比第三组——**你会亲眼看到一条 pr_info ≈ 几 µs，而 ftrace 环形缓冲是 ns 级**，这决定了你以后怎么选测量工具。

**面试一句话**：测量三原则——warmup、看分位不看均值、**测量工具自身的成本必须单独测一次**（这一关就是活教材）。

---

## 4. 调试工具箱速查（每关都要用）

```sh
# ---- guest 内 ----
dmesg -w | grep vgpu                    # 结构化日志流（每步带 seqno+ktime）
cat /sys/kernel/debug/vgpu/stats        # submitted/completed/errors/ring_used
cat /sys/kernel/debug/vgpu/pending      # 在飞 submission + 错误码
cat /sys/kernel/debug/vgpu/regs         # 虚拟 BAR0：status/seqno/error/user
cat /sys/kernel/debug/vgpu/ring         # ring hexdump

# ftrace 函数图（只过滤我们的函数，输出最短最好读）
cd /sys/kernel/debug/tracing
echo function_graph > current_tracer
echo 'vgpu_*' > set_graph_function
echo > trace && echo 1 > tracing_on
/root/vgpu_test >/dev/null 2>&1
echo 0 > tracing_on && cat trace

# ---- host 上（另开终端）----
DEBUG=1 scripts/30-run.sh               # guest 停在起点，gdb 接 :1234
gdb-multiarch ~/linux/vmlinux -ex 'target remote :1234'
(gdb) b vgpu_finish_submission          # 下半部第一现场
(gdb) b vgpu_wait_ioctl                 # 看等待者被 fence 唤醒的栈
(gdb) p *v                              # 整个设备私有结构一览
```

**KASAN 读法**：oom/越界/UAF 会当场红报告；`submission 已 pending 才发布 fence`（commit 452872a 之前的问题）这类竞态只有 KASAN 会告诉你——看到报告先用 `gdb` 复现现场再改，不要盲改。

## 5. 每关的共同纪律

1. **先跑现状，把破绽存下来**（偶发测试先跑出失败日志）——没有 before 就没有说服力；
2. 一次只改一个机制，**每关一个 commit**（信息量 = 一个概念）；
3. 每关结束贴出：ftrace 片段 / debugfs 输出 / 验收日志——这就是简历里"对 GPU 驱动栈的理解"的物证；
4. 卡住先看 `docs/adr/`（0001/0002/0003 记录了三个"为什么这么做"的选择，多数坑里面写过）。
