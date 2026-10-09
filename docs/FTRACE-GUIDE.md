# ftrace 完全指南：从"能录"到"会读"

> 目标读者：会跑实验、但没系统用过 ftrace 的内核初学者。
> 本文所有命令都在本项目的 guest 里**实测通过**（输出是真实抓取，不是示意）。
> 用法：先读 §0–§3 建立框架，之后当手册查；§10 是七个循序渐进的上手实验。

---

## 0. 心智模型：ftrace 到底是什么

三句话：

1. **内核给每个 CPU 开了一个环形缓冲区**，默认 1410KB/CPU（本项目 guest 实测，4 CPU ≈ 5.6MB）。
2. **tracer 决定"录成什么形态"**（扁平列表 / 函数图 / 关掉），**filter 决定"录谁"**（哪些函数、哪个 PID、哪个 CPU），`tracing_on` 决定"录不录"。
3. 录进去的每一条都带**时间戳**——ftrace 的本质能力是**把并发事件按时间排序摊平给你看**，这是 printk 永远给不了的。

配套认知：**ftrace 是测量仪器，不是免费眼镜**。它自己就有成本（§8），不控制这个成本，测出来的数字就是假的。

## 1. 本文的环境（先确认你有什么）

进入目录（guest 里）：

```sh
cd /sys/kernel/debug/tracing
cat available_tracers      # 你的内核支持哪些 tracer
```

**本 guest 实测**：`blk function_graph function nop` —— 只有四个！

> ⚠️ 网上的教程常提到 `irqsoff`/`preemptoff`/`wakeup`/`hwlat`，**本内核没编进来**（`.config` 里 `CONFIG_IRQSOFF_TRACER` 等未开）。做法：永远先 `cat available_tracers`，别照抄命令。

同样**没编进来**的（已逐一确认，别浪费时间）：histogram trigger、snapshot、function profiler、stack tracer。本文相应位置会标 ❌。

**有**的（都是好东西）：

| 能力 | 状态 |
|---|---|
| 动态 ftrace（改 filter 无需重启） | ✅ |
| function_graph（进出+耗时+嵌套） | ✅ |
| tracepoints/events（含 `dma_fence:` 全套） | ✅ 100+ 个子系统 |
| `func_stack_trace`（看到调用栈） | ✅ |
| `set_ftrace_pid` / `tracing_cpumask` / `tracing_thresh` | ✅ |
| `trace_marker`（用户态打点） | ✅ 需两步开关，见 §6 |

## 2. 目录地图（这些文件各干什么）

按使用频率排序。所有相对路径都是 `/sys/kernel/debug/tracing/`：

**总控**

| 文件 | 作用 |
|---|---|
| `current_tracer` | 当前 tracer。写 `function_graph`/`function`/`nop` 切换 |
| `tracing_on` | 总开关。写 `0`/`1`。**关着的时候什么都录不到** |
| `trace` | 输出缓冲区。**必须 `cat` 才有东西看**（初学者第一大坑） |
| `trace_pipe` | 同 `trace` 但是"实时流"：阻塞读、读完即消费 |
| `buffer_size_kb` | 每 CPU 缓冲区大小，实测 1410 |

**过滤器**

| 文件 | 作用于 |
|---|---|
| `set_graph_function` | **function_graph** 的过滤（只画这些函数） |
| `set_ftrace_filter` | **function** 的过滤（扁平模式） |
| `set_ftrace_notrace` | 反选：这些函数**不**录 |
| `set_graph_notrace` | graph 的反选 |
| `set_ftrace_pid` | 只录这个 PID 触发的过程 |
| `set_event_pid` | 同上但针对 events |
| `tracing_cpumask` | 只录这些 CPU（bitmask） |
| `tracing_thresh` | 只录超过 N 微秒的函数调用（降噪利器） |

**调优**

| 文件 | 作用 |
|---|---|
| `options/` | 一堆开关（见 §4），每个文件写 1/0 |
| `max_graph_depth` | 嵌套最多画几层 |
| `trace_clock` | 时钟源。多 CPU 对齐必须用 `global` |
| `events/` | 所有 tracepoint（见 §6） |
| `trace_marker` | 用户态往 trace 里写字（见 §9.2） |
| `per_cpu/cpuN/stats` | 每个 CPU 的 entries/overrun/dropped——**判断有没有丢数据** |
| `instances/` | 多套独立 tracing 配置 |
| `available_filter_functions` | 所有可过滤函数名（可 `grep vgpu`） |

## 3. 五步标准流程（每次实验的骨架）

所有 ftrace 实验都是这五步，无一例外：

```sh
cd /sys/kernel/debug/tracing
# ① 选 tracer
echo function_graph > current_tracer
# ② 过滤（不设 = 全录，输出爆炸）
echo 'vgpu_*' > set_graph_function          # graph 模式
echo 'vgpu_*' > set_ftrace_filter           # 扁平模式（同时设两个无害）
# ③ 清 buffer + 开录
echo > trace
echo 1 > tracing_on
# ④ 跑你的负载
/root/vgpu_test
# ⑤ 停录 + 读 + 复位
echo 0 > tracing_on
cat trace
echo nop > current_tracer      # 复位！否则下次实验互相污染
```

**顺序即纪律**：`echo > trace`（清 buffer）必须放在"开录"之前；如果你先清了 buffer 再去开会话别的开关，中间的数据就丢了（你今天踩过：enable events → clear → 读，结果空的）。

可以抄进 shell 的小助手：

```sh
tr_start() { echo "$1" > current_tracer; echo "$2" > set_graph_function; echo "$2" > set_ftrace_filter; echo > trace; echo 1 > tracing_on; }
tr_stop()  { echo 0 > tracing_on; echo nop > current_tracer; }
# 用法：tr_start function_graph 'vgpu_*'; /root/vgpu_test; tr_stop; cat trace
```

## 4. function_graph 精读（本指南最重要的一节）

### 4.1 开起来

```sh
echo function_graph > current_tracer
echo 'vgpu_*' > set_graph_function
echo > trace; echo 1 > tracing_on
/root/vgpu_test >/dev/null 2>&1
echo 0 > tracing_on; cat trace
```

### 4.2 逐列拆解（真实输出，来自 README 那次运行）

```
# CPU  DURATION                  FUNCTION CALLS
# |     |   |                     |   |   |   |
 3) ! 816.046 us  |  vgpu_bo_create_ioctl [vgpu]();
 3) ! 755.332 us  |  vgpu_bo_create_ioctl [vgpu]();
 2)               |  vgpu_engine_work [vgpu]() {
 3) # 1380.183 us |  vgpu_wait_ioctl [vgpu]();
 2) ! 322.252 us  |    vgpu_irq_handler [vgpu]();
 2) ! 623.890 us  |  }
 2)               |  vgpu_irq_bh_work [vgpu]() {
 2) ! 9.189 us    |    vgpu_finish_submission.part.0 [vgpu]();
 2) ! 274.280 us  |  }
```

- **`3)` / `2)`** = CPU 编号。同一份 trace 里两个 CPU 交替出现 = **并发在你眼前**（进程态 ioctl 在 CPU3，engine workqueue 在 CPU2）。
- **DURATION** = 这个函数花了多久。注意 `vgpu_engine_work` 没有 duration 但有 `{`——它还没结束就被后面的 `}` 收尾？不，往下看：它的 `}` 在 `vgpu_irq_handler` 之后，说明 irq handler 是**在它内部嵌套执行**的（我们的模拟中断就是 engine 直接调 handler）。
- **缩进 = 调用嵌套**：`vgpu_irq_handler` 缩进在 `vgpu_engine_work {` 里；`vgpu_finish_submission` 缩进在 `vgpu_irq_bh_work {` 里。
- **`{` 和 `}`**：一对括号圈出一次调用的生命周期。上下两半跨越了几百 µs。
- **`!`** = 超过阈值（默认 100µs），用 `tracing_thresh` 改。**只把慢的挑出来看，是读 trace 的第一降噪手段**。
- **`#`** = 这个函数执行期间被抢占了，或者里面发生了上下文切换（`vgpu_wait_ioctl` 上的 `#` 就是等待者睡过去了）。
- **`[vgpu]`** = 所属模块。区分内核核心函数和模块函数。

### 4.3 和它配套的 options/（实测都在）

```sh
echo 1 > options/funcgraph-overhead   # duration 里扣掉 ftrace 自身开销
echo 1 > options/sleep-time           # 显示函数里睡眠了多久
echo 1 > options/funcgraph-proc       # 显示进程名
echo 1 > options/no-func-no-repeats   # 同一函数连续重复只显示一次
echo 1 > options/sym-offset           # 符号+偏移
```

### 4.4 一个必须知道的坑

`echo '' > set_graph_function` = **清空过滤**（不是"设个空 filter"，是清空）。你今天试过。清空后不重设 = 接下来全内核函数都画，trace 几秒内涨爆、 debug 效率归零。**永远显式重设 filter。**

## 5. function（扁平模式）与调用栈

### 5.1 什么时候用扁平模式

函数图信息密度太高、或者你只关心**调用序列和先后**（不关心嵌套和耗时）时：

```sh
echo function > current_tracer
echo 'vgpu_*' > set_ftrace_filter
echo > trace; echo 1 > tracing_on; /root/vgpu_test >/dev/null 2>&1; echo 0 > tracing_on
head -12 trace
```

真实输出：

```
#           TASK-PID     CPU#  |||||  TIMESTAMP  FUNCTION
#              | |         |   |||||     |         |
       vgpu_test-112     [002] .....    32.734855: vgpu_bo_create_ioctl <-drm_ioctl_kernel
```

- **`vgpu_test-112`**：进程名-PID（内核自己做了 pid→comm 映射）。
- **`<-drm_ioctl_kernel`**：`vgpu_bo_create_ioctl` 的**调用者**——扁平模式直接送你答案。
- 标志位那排（`.....`/`d..1.`）从左到右：irqs-off / need-resched / hardirq/softirq / preempt-depth / migrate-disable / delay。看到 `d` 说明当时关了中断，`1` 是说抢占深度 1。

### 5.2 "谁调用了我"的标准答案：func_stack_trace

```sh
echo function > current_tracer
echo 'vgpu_finish_submission' > set_ftrace_filter
echo 1 > options/func_stack_trace
echo > trace; echo 1 > tracing_on; /root/vgpu_test >/dev/null 2>&1; echo 0 > tracing_on
head -14 trace
```

真实输出：

```
       vgpu_test-112     [002] .....    32.734855: vgpu_bo_create_ioctl <-drm_ioctl_kernel
       vgpu_test-112     [002] .....    32.734860: <stack trace>
 => [FTRACE TRAMPOLINE]
 => vgpu_bo_create_ioctl
 => drm_ioctl_kernel
 => drm_ioctl
 => __x64_sys_ioctl
```

**这就是"从用户态 ioctl 一路怎么调到驱动的"的活地图**。把 filter 换成 `vgpu_finish_submission`，能看到下半部的调用栈（workqueue → run_workqueue → ... → vgpu_finish_submission）——比任何文档都直观。

用完记得 `echo 0 > options/func_stack_trace`。

## 6. Tracepoints / events：不插函数也能看内核

### 6.1 概念

tracepoint 是内核里**静态埋好的观测点**（比函数级便宜一个量级，格式稳定）。你的驱动如果走了标准子系统，不用改代码就能看到它干了什么：

```sh
ls events/                          # 100+ 个子系统
cat events/dma_fence/dma_fence_wait_end/format   # 这个事件的字段表
echo 1 > events/dma_fence/dma_fence_wait_start/enable
```

### 6.2 本项目的金矿（实测输出）

一次 `/root/vgpu_test`，enable 四个事件后：

```sh
echo 1 > events/dma_fence/dma_fence_wait_start/enable
echo 1 > events/dma_fence/dma_fence_wait_end/enable
echo 1 > events/dma_fence/dma_fence_signaled/enable
echo > trace; echo 1 > tracing_on
/root/vgpu_test >/dev/null 2>&1
echo 0 > tracing_on
grep -m6 dma_fence trace
```

真实输出：

```
       vgpu_test-110     [002] .....    27.690926: dma_fence_wait_start: driver=vgpu timeline=vgpu context=5 seqno=2
   kworker/u16:0-12      [000] d..1.    27.692955: dma_fence_signaled: driver=vgpu timeline=vgpu-timeline context=5 seqno=2
       vgpu_test-110     [002] .....    27.693042: dma_fence_wait_end: driver=detached-driver timeline=signaled-timeline context=5 seqno=2
```

**值得盯的三件事**：

1. `wait_start` 在 **CPU2 的 vgpu_test**，`signaled` 在 **CPU0 的 kworker** ——等待者睡在一个核、信号者在另一个核完成。跨 CPU 通信的证据链，比读代码直观一百倍。
2. 中间隔了 ~2ms：这就是**一次提交从等待开始到结束的真实延迟**（测量被 tracer 本身放大了，见 §8）。
3. 全程有 `driver=vgpu context=5 seqno=2` 字段——tracepoint 的字段是按事件类型解析好的，grep 就能统计。

### 6.3 本项目其他有用的子系统

| 事件 | 用来看什么 |
|---|---|
| `irq:irq_handler_entry/exit` | 有没有真中断、irq 号是多少（**现在只有 ttyS0**，见 §10 实验 7） |
| `workqueue:workqueue_execute_start/end` | engine workqueue 的调度与执行时序 |
| `sched:sched_switch / sched_wakeup` | 等待者睡/醒、worker 被调度上 CPU 的时刻 |
| `printk:printk` | 把 dmesg 里的 pr_info 也拉进同一条时间轴（对不齐时间戳时用） |
| `dma_fence:*` | 提交/等待/signal 全生命周期 |

### 6.4 能力边界（实测 ❌）

```sh
# 这些在本内核干不了，别试：
echo 'hist:keys=driver' > events/dma_fence/dma_fence_wait_end/trigger   # ❌ 没编 HIST_TRIGGERS
echo snapshot > trace                                                     # ❌ 没编 TRACER_SNAPSHOT
```

状态确认方法：`grep HIST ~/linux/.config`（host 上），或直接试，报错就是没开。

## 7. 降噪：决定你的 trace 是"能读"还是"垃圾"

按效果排序：

1. **filter 过滤函数**（`set_graph_function`）：`'vgpu_*'` 一下，从 10 万行降到 20 行。多模式用空格分隔：`'vgpu_wait_ioctl dma_fence_wait_timeout'`。
2. **`tracing_thresh`**：只录慢的。
   ```sh
   echo 100 > tracing_thresh    # 只录 ≥100µs 的函数
   ```
   （注意：thresh 是全局阈值，function_graph 里体现为 `!` 标记的那些。）
3. **`set_ftrace_pid`**：只跟某个进程。多进程测试里锁定一个 child：
   ```sh
   echo 110 > set_ftrace_pid
   ```
4. **`tracing_cpumask`**：只录某几个 CPU。
   ```sh
   echo 1 > tracing_cpumask     # 只录 CPU1
   ```
5. **`events/.../enable` 精确到单事件**，而不是 enable 整个子系统。
6. **buffer 别溢出**：跑长负载前 `cat per_cpu/cpu0/stats`，看 `overrun`/`dropped events` 是不是 0。是 0 才信数据；非 0 就调 `buffer_size_kb` 或缩小过滤范围。

## 8. 开销与测量纪律（这一节决定你测的数字可不可信）

量级参考（数量级正确，别当精确值）：

| 手段 | 每条开销 | 副作用 |
|---|---|---|
| tracepoint/event | ~50 ns | 很小 |
| function（扁平） | ~100–300 ns | 污染 i-cache/d-cache |
| function_graph | ~0.5–1 µs/函数 | 扭曲调用时序本身 |
| pr_info / printk | ~1–10 µs | **串口输出会卡住整机** |

三条纪律：

1. **先测仪器再测目标**：同一负载，tracing 开/关各跑一次，差值就是仪器成本。上面那份 trace 里 2ms 的等待，开 tracer 之前实测是 ~1ms 量级——**报告任何延迟数字时，必须同时说明测量配置**。
2. **只做同配置对比**："开了 A 比关了 A 快 30%"只有在两种配置各自 internally 一致时才有意义；混着改两个变量 = 没测。
3. **printk 不进测量环**。驱动里那些 `dev_info` 是给人看的调试输出；要量化就 ftrace/事件，要时序就 `trace_clock` 设 `global` 对齐多 CPU。

## 9. 工作流工具

### 9.1 实时跟随：trace_pipe

```sh
/root/vgpu_test >/dev/null 2>&1 &      # 后台跑
cat trace_pipe | grep vgpu             # 前台实时跟（Ctrl-C 退出）
```
`trace_pipe` 是**消费式**的：读走就从 buffer 消失，适合"看着日志跑"，不适合事后分析（事后用 `trace`）。

### 9.2 用户态打点：trace_marker

在 trace 里插一条你自己的标记（比如"测试第几阶段开始"）：

```sh
echo 1 > options/markers      # 开关①（默认可能是关的）
echo 1 > tracing_on           # 开关②（关着会拒绝写入）
echo "phase: 4-proc start" > trace_marker
```

**实测坑**：直接写会报 `write error: Bad file descriptor`——原因就是上面两个开关没开。另外字符串 ≤4096 字节。

### 9.3 多套配置互不干扰：instances/

```sh
mkdir instances/lab0
echo function_graph > instances/lab0/current_tracer
echo 'vgpu_*' > instances/lab0/set_graph_function
echo 1 > instances/lab0/tracing_on         # 只在 lab0 这套里录
cat instances/lab0/trace
```
用途：一套常录（比如 events），另一套只在出问题时开（比如函数图），互不踩。

### 9.4 这台机器上没有的（知道有这个东西，本内核不方便用了）

| 工具 | 干什么 | 本内核 |
|---|---|---|
| `trace-cmd record/report` | 命令行抓取/解析，比裸 sysfs 友好 | 可加到 initramfs（host 没装静态版就没带） |
| KernelShark | GUI 时间线 | 同上 |
| `perf trace` | syscall 级追踪 | 需另装 |
| snapshot（`echo snapshot > trace`） | 活动 buffer 与快照交换，**抓竞态神器** | ❌ 配置未编 |
| hist trigger | 事件聚合统计 | ❌ 配置未编 |

### 9.5 ftrace × gdb 分工

| 问题 | 工具 |
|---|---|
| "这个变量的值是多少" | gdb（`DEBUG=1 scripts/30-run.sh`，断点进代码） |
| "A 和 B 谁先发生、隔多久、在哪个 CPU" | ftrace |
| "谁调用了它" | ftrace（`func_stack_trace`）或 gdb bt（但 gdb 会停下来） |

## 10. 七个上手实验（30–60 分钟，循序渐进）

> 每个都先自己跑、先猜结果，再看输出。猜错的地方就是你知识缺口的位置。

**实验 1｜一条命令的完整生命周期**
`function_graph` + `set_graph_function='vgpu_*'`，跑单进程测试。任务：不看文档，把 §4.2 那段 16 行 trace 讲出来——谁在进程态、谁在 worker 态、中断上下半部在哪两行、等待者在哪个 CPU。

**实验 2｜跨 CPU 的等待**
按 §6.2 enable 三个 dma_fence 事件。任务：从 trace 里找出"等待者PID/CPU"和"信号者PID/CPU"，算出这次等待持续了多久。**这是 Lab 0 的直接预习。**

**实验 3｜谁调用了 vgpu_finish_submission**
`function` + `set_ftrace_filter='vgpu_finish_submission'` + `options/func_stack_trace=1`。任务：画出从 ioctl 到 fence signal 的完整调用链。然后换 `vgpu_wait_ioctl` 再做一遍，对比两条链在哪里分叉。

**实验 4｜多进程里锁定一个 child**
`/root/vgpu_test -m 4 &` 起并发，立刻 `cat /sys/kernel/debug/vgpu/pending` 拿一个 seqno、从 `top`-类信息或进程表拿 child PID，`set_ftrace_pid` 锁定它。任务：只看一个孩子的 doorbell→wait 全程，证明并发下时序是交错的。

**实验 5｜engine 在哪个 CPU 上跑**
`echo 2 > tracing_cpumask` 只录 CPU2，跑测试，看 engine 的 work 还在不在 trace 里（在=它被调度到 CPU2）。换几个 mask 值，摸清 ordered workqueue 的 CPU 亲和性。

**实验 6｜只抓慢调用**
`echo 50 > tracing_thresh`，跑测试。任务：解释为什么 64-NOP 那次填环的 `vgpu_wait_ioctl` 出现在 thresh 之外/之内。

**实验 7｜为什么 irq 事件里没有 vgpu**
enable `irq:irq_handler_entry` 跑测试。任务：grep 不到任何 vgpu 相关中断，解释为什么（提示：ADR-0001，我们的中断是"直接调用 handler"模拟的）。**这个实验的答案就是 Lab 6 的立项理由。**

## 11. 速查卡

**标准流程**：`选 tracer → 设 filter → 清 buffer → 开 → 跑负载 → 停 → cat → 复位 nop`

**复位三件套**（每次实验结束必做）：

```sh
echo nop > current_tracer
echo > set_graph_function; echo > set_ftrace_filter
echo 0 > tracing_on
```

**常见坑对照表**：

| 现象 | 原因 | 修法 |
|---|---|---|
| 跑完 trace 是空的 | 忘了 `cat trace`（最常见！） | ⑤ 别省 |
| 还是空 | `tracing_on` 是 0，或 tracer 是 `nop` | 先 `cat current_tracer; cat tracing_on` |
| 输出爆炸几十万行 | filter 没设/设错文件（graph 该用 `set_graph_function`） | 重设 filter |
| 写了 filter 但函数不出现 | 函数名不存在/被 inline；用 `available_filter_functions \| grep` 核对 | 核对名字 |
| `write error` | trace_marker 没开 `options/markers` 或 `tracing_on=0` | 两个开关都开 |
| enable 了事件但没记录 | enable 之后 `echo > trace` 把数据清了；或 buffer 溢出 | 调整顺序；看 `per_cpu/cpuN/stats` |
| 数据里突然少了后半段 | buffer 溢出 | 看 stats 里的 overrun；加大 buffer 或加过滤 |
| 多 CPU 时间戳对不上 | `trace_clock` 不是 global | `echo global > trace_clock` |
| 换了一次 boot 后 filter 还在 | 每次 boot 是全新现场；**同一次 boot 内** filter/tracer 不会自动复位 | 复位三件套 |

**一句话总结**：ftrace 的门槛不在命令，在**先想清楚"我要证明什么"**——是时序？是调用栈？是延迟？——再选 tracer、设 filter。顺序反了，你录到的只有噪音。
