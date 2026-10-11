# vgpu 完整上手与实例教学

> 这份文档回答四个问题：**怎么入门、怎么跑测试、怎么抓 trace、怎么让一段真正的计算跑起来**。
> 最后一节是一个完整实例：把经典的 CUDA SAXPY 搬到 vgpu 上，从内核代码一直跑通到校验结果。
> 环境细节见 [DESIGN.md](DESIGN.md)，逐机制深入学习见 [LEARNING-PATH.md](LEARNING-PATH.md)，
> ftrace 完整教程见 [FTRACE-GUIDE.md](FTRACE-GUIDE.md)。

---

## 1. 入门

### 1.1 你会得到什么

一个**纯软件模拟的 GPU 驱动**，跑在 QEMU/KVM guest 里：`platform + DRM 设备注册 → GEM 缓冲对象 →
队列与门铃提交 → 引擎执行 → 模拟中断上下半部 → dma_fence 完成通知`。全程不碰真实 GPU 硬件，
Host 只多一个 QEMU 进程（vLLM 零影响，ADR-0003）。

### 1.2 前置条件

| 项 | 要求 | 验证 |
|---|---|---|
| Host | x86_64 Linux，`qemu-system-x86_64`、`/dev/kvm` 可用 | `ls /dev/kvm` |
| 内核树 | `~/linux`（vanilla 6.17，KASAN 已开），**模块与 guest 内核同源编译** | `ls ~/linux/arch/x86/boot/bzImage` |
| 工具 | `gcc`、`make`、`busybox`、`cpio`、`gh`（推送用） | `which gcc make cpio` |

### 1.3 三条命令跑起来

```bash
scripts/10-build.sh        # ① 编译 vgpu.ko（对 ~/linux）+ 静态用户态程序
scripts/20-initramfs.sh    # ② 打包 busybox initramfs（vgpu.ko + 测试 + /init + saxpy）
scripts/30-run.sh          # ③ 起 guest；autorun 存在时开机自动跑全部测试，最后停在 ~ #
```

退出 guest：`Ctrl-A` 然后 `X`。guest 里只有 busybox shell——模块、测试、示例都在 `/root/`。

改代码后的循环永远是这三条（增量编译，第二次很快）。

---

## 2. 跑测试

### 2.1 一键全测（开机自动）

`autorun` 标记存在时，guest 启动后 `/root/run_test.sh` 依次跑各阶段：

| 阶段 | 命令 | 验证什么 |
|---|---|---|
| 单进程全链路 | `/root/vgpu_test` | BO 创建 → 写命令 → doorbell → engine → 上下半部 → fence → 读回校验 |
| 4 进程并发 | `/root/vgpu_test -m 4` | 每 fd 独立队列、并发提交、seqno/fence 无串扰 |
| 多引擎多队列 | `/root/vgpu_test -q` | 一个 fd 三条流绑三个引擎；引擎类别在门铃处被拒 |
| **计算：SAXPY** | `/root/saxpy` | kernel launch 全流程（本档 §5 的实例） |
| 负路径 | `/root/vgpu_test -e` | 未知 seqno/坏 handle/引擎级错误/ring 满/门铃坏输入 |
| 时间片/抢占 | `/root/vgpu_test -f` | 两个 context 时间片轮转、高优先级抢下在跑的 kernel |
| 同步/图 | `/root/vgpu_test -g` | timeline semaphore、DAG 一次门铃 |
| 虚拟内存 | `/root/vgpu_test -v` | 两个 context 同一 VA 各自 buffer、按需重映射 |
| **统一内存** | `/root/uvm_saxpy` | managed VA 被引擎逐访存解析、缺页即迁回（本篇 §6） |
| ftrace + stats + rmmod | 脚本内 | 函数图抓取、debugfs 统计、rmmod drain 干净 |

**判定标准**：日志出现 `=== all tests passed ===`，且 `dmesg` 里**没有** WARNING / KASAN / BUG。
任一阶段 FAIL 会让脚本停下（`|| exit 1`），从第一个 FAIL 读起。

### 2.2 单独跑某项

guest 的 `~ #` 下：

```sh
/root/vgpu_test           # 单进程
/root/vgpu_test -m 4      # 并发
/root/vgpu_test -q        # 多引擎
/root/vgpu_test -p        # packet 门铃两批提交
/root/vgpu_test -e        # 负路径
/root/saxpy               # 计算示例（SAXPY）
/root/uvm_saxpy           # 统一内存示例（managed VA + 按需迁移）

cat /sys/kernel/debug/vgpu/stats      # 提交/完成/错误 + 每引擎 scratch 与 backlog
cat /sys/kernel/debug/vgpu/queues     # 每条队列：引擎/优先级/ring 占用/计数
cat /sys/kernel/debug/vgpu/compute    # 计算统计：warps/threads/waves/合并率 + 引擎侧缺页数
cat /sys/kernel/debug/vgpu/vm         # 每 context 的 VA 区间/budget/逐页/存留 + demand-faults
cat /sys/kernel/debug/vgpu/pending    # 在飞 submission
cat /sys/kernel/debug/vgpu/regs       # 虚拟 BAR0 当前值
```

注意：autorun 最后会 `rmmod` 再重新加载，所以落到 shell 时设备是可用的；若你自己 `rmmod` 过，
先 `insmod /root/vgpu.ko` 并用 sysfs 里的 dev 号重建 `/dev/dri/*` 节点（initramfs 的 /init 里有同样的逻辑）。

---

## 3. 抓 trace

三板斧（详细教程在 [FTRACE-GUIDE.md](FTRACE-GUIDE.md)，这里只留最常用组合）：

```sh
# ① 函数图：一条命令从 doorbell 到 fence 的完整生命周期
cd /sys/kernel/debug/tracing
echo function_graph > current_tracer
echo 'vgpu_*' > set_graph_function
echo > trace && echo 1 > tracing_on
/root/saxpy >/dev/null 2>&1        # 或 vgpu_test -p
echo 0 > tracing_on && cat trace
echo nop > current_tracer          # 复位！

# ② dma_fence 事件：等待者在哪个 CPU 睡、信号者在哪个 CPU 完成
echo 1 > events/dma_fence/dma_fence_wait_start/enable
echo 1 > events/dma_fence/dma_fence_wait_end/enable
echo 1 > events/dma_fence/dma_fence_signaled/enable
echo > trace && echo 1 > tracing_on
/root/saxpy >/dev/null 2>&1
echo 0 > tracing_on && grep dma_fence trace

# ③ 谁调用了谁（带调用栈）
echo function > current_tracer
echo 'vgpu_compute_run' > set_ftrace_filter
echo 1 > options/func_stack_trace
echo > trace && echo 1 > tracing_on
/root/saxpy >/dev/null 2>&1
echo 0 > tracing_on && head -20 trace
echo 0 > options/func_stack_trace
```

读图要点：行首是 **CPU 编号**（跨 CPU 并发的直接证据）；`{ }` 是调用嵌套；`!` 表示超过阈值。
**每次做完必须 `echo nop > current_tracer`**，否则 tracer 一直开着，既拖慢 guest 又污染下次数据。

---

## 4. 一个诚实的边界：CUDA 为什么不能"直接"跑

先说清楚，再上实例。

一段标准 CUDA 代码：

```cuda
// saxpy.cu —— CUDA 版
__global__ void saxpy(float a, float *x, float *y)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    y[i] = a * x[i] + y[i];
}

int main(void)
{
    float *x, *y;
    cudaMalloc(&x, N * sizeof(float));
    cudaMalloc(&y, N * sizeof(float));
    cudaMemcpy(x, h_x, N * sizeof(float), cudaMemcpyHostToDevice);
    saxpy<<<(N + 255) / 256, 256>>>(2.0f, x, y);
    cudaDeviceSynchronize();
    cudaMemcpy(h_y, y, N * sizeof(float), cudaMemcpyDeviceToHost);
}
```

`nvcc` 编译它得到的是：PTX/SASS 指令流 + 一份**给 libcuda 的启动描述符**（grid/block/参数缓冲）。
运行时链路是：`libcuda` → `nvidia.ko` 的 ioctl（`NV_ESC_REGISTER_ALLOC` / `UVMIOC` 一大家子）→
GPU 的 host channel scheduler 取指执行。

**vgpu 目前做不到"加载真正的 cubin 直接跑"**——我们的计算 ISA 只有 7 条指令（没有 PTX 前端、
没有浮点、没有 shared memory 语义）。要实现"真 CUDA 二进制跑在 vgpu 上"，需要：PTX→我们的 ISA
的翻译器 + libcuda 的 ioctl shim + UVM 语义，那是另一个项目（GPGPU-EVOLUTION-PLAN 的远期边界）。

**但 SAXPY 这个概念完全可以跑**，而且跑法 itself 就是教学本体：下面四步把同一个算法、同一条
CUDA 调用链，逐一对映到 vgpu 的机制上，最后真跑出来、真校验。

---

## 5. 实例：SAXPY 完整 flow（CUDA → vgpu → 跑通）

目标：`out[i] = a * in[i] + b`（i = 线程号，共 4096 个元素）。

### 5.1 CUDA 概念 → vgpu 机制映射

| CUDA | vgpu | 说明 |
|---|---|---|
| `cudaMalloc` | `VGPU_BO_CREATE` | shmem GEM 对象 = 一块"显存" |
| `cudaMemcpy(H2D)` | `mmap(BO)` 后直接写 | 用户态映射同一份内存 |
| `__global__` kernel | 7 指令 ISA 程序（16B/条） | 见下 |
| 编译期寄存器分配 | `nregs` 字段 | module load 时申报 |
| `<<<grid, block>>>` | `VGPU_PKT_LAUNCH` packet | grid/block/参数缓冲/内核 id |
| `cudaLaunchKernel` | `VGPU_DOORBELL`（写 put 偏移） | 一次系统调用 = 一次批量提交 |
| 驱动加载模块 | `VGPU_MODULE_LOAD` | blob 拷贝进驱动，发 kid |
| `cudaDeviceSynchronize` | `VGPU_WAIT` | 内核态等 dma_fence |
| 读回结果 | 读同一块 mmap | fence 已 signal，读取即最终值 |

### 5.2 步骤 1：写 vgpu 版 kernel 并编译

kernel 是我们的 ISA（`driver/uapi/vgpu_drm.h` 有完整参考）：`r0` 预置线程号，`r15` 自由通用；
参数缓冲按**字**寻址（word 0 = N, 1 = a, 2 = b, 3 = in_base, 4 = out_base）。

`userspace/examples/saxpy.c` 里这段就是全部：

```c
static const struct vgpu_instr code[] = {
    { .op = VGPU_ISA_MOVI, .dst = 15, .a = 0 },        /* r15 = 0            */
    { .op = VGPU_ISA_LD,   .dst = 1,  .a = 15, .b = 1 }, /* r1  = a           */
    { .op = VGPU_ISA_LD,   .dst = 2,  .a = 15, .b = 2 }, /* r2  = b           */
    { .op = VGPU_ISA_LD,   .dst = 3,  .a = 15, .b = 3 }, /* r3  = in_base     */
    { .op = VGPU_ISA_LD,   .dst = 4,  .a = 15, .b = 4 }, /* r4  = out_base    */
    { .op = VGPU_ISA_ADD,  .dst = 5,  .a = 3,  .b = 0 }, /* r5  = in+tid      */
    { .op = VGPU_ISA_LD,   .dst = 6,  .a = 5,  .b = 0 }, /* r6  = in[tid]     */
    { .op = VGPU_ISA_MUL,  .dst = 7,  .a = 1,  .b = 6 }, /* r7  = a * in[tid] */
    { .op = VGPU_ISA_ADD,  .dst = 8,  .a = 7,  .b = 2 }, /* r8  = ... + b     */
    { .op = VGPU_ISA_ADD,  .dst = 9,  .a = 4,  .b = 0 }, /* r9  = out+tid     */
    { .op = VGPU_ISA_ST,   .dst = 8,  .a = 9,  .b = 0 }, /* out[tid] = r8     */
    { .op = VGPU_ISA_EXIT },
};
```

编译（host 上）：`make -C userspace` → `userspace/examples/saxpy`（静态链接）。

### 5.3 步骤 2：跑起来 / 拷进 guest（对应"拷进 QEMU guest"的两种做法）

**路线 A（推荐，用户态程序）**——对应你说的"通过用户态 test 把 memory 拷贝进去"：

```bash
scripts/10-build.sh && scripts/20-initramfs.sh && scripts/30-run.sh
# guest 内：/root/saxpy
```

`20-initramfs.sh` 会把 `userspace/examples/saxpy` 自动打进 initramfs 的 `/root/`。

**路线 B（"编进 KO"）**——把 kernel blob 变成驱动自带，用户态只发 LAUNCH：
把 `code[]` 数组移进 `driver/vgpu_compute.c`，在 probe 里用一个固定 kid 注册（`vgpu_kernel` 结构已支持），
然后让 `VGPU_DOORBELL` 的 `launch.kid` 指向它。练习这个改造是理解"驱动侧模块注册"的好机会；
示例仓库保持路线 A，因为**用户态加载**才对应 CUDA 的"模块由运行时加载"语义。

### 5.4 步骤 3：把数据搬进"显存"

`cudaMalloc + cudaMemcpy` 在 vgpu 里就是一次 BO 创建 + mmap + 直接写（同一份内存，无需拷贝）：

```c
memset(&bo, 0, sizeof(bo));
bo.size = (PARAM_WORDS + 2 * N_ELEMS) * 4;
ioctl(fd, DRM_IOCTL_VGPU_BO_CREATE, &bo);
args = mmap(NULL, bo.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, bo.mmap_offset);

args[0] = N_ELEMS;                              /* N   */
args[1] = 3;                                    /* a   */
args[2] = 7;                                    /* b   */
args[3] = PARAM_WORDS;                          /* in[] 起始字下标   */
args[4] = PARAM_WORDS + N_ELEMS;                /* out[] 起始字下标  */
for (i = 0; i < N_ELEMS; i++) {
    args[PARAM_WORDS + i] = i * 2 + 1;          /* in[i]            */
    args[PARAM_WORDS + N_ELEMS + i] = 0xdeadbeef; /* out[i] 预置毒丸 */
}
```

### 5.5 步骤 4：执行 + 校验

```c
/* module load：驱动保存 kernel 副本并发放 kid */
ml.ninstr = sizeof(code) / sizeof(code[0]);  ml.code = (uint64_t)(uintptr_t)code;
ioctl(fd, DRM_IOCTL_VGPU_MODULE_LOAD, &ml);

/* compute stream + LAUNCH packet + doorbell */
pk[0].type = VGPU_PKT_LAUNCH;
pk[0].launch.kid = ml.kid;  pk[0].launch.grid = 64;  pk[0].launch.block = 64;
pk[0].launch.args_bo = bo.handle;
db.qid = qc.qid;  db.put = sizeof(*pk);
ioctl(fd, DRM_IOCTL_VGPU_DOORBELL, &db);

/* cudaDeviceSynchronize */
w.seqno = db.seqno;  w.timeout_ms = 5000;
ioctl(fd, DRM_IOCTL_VGPU_WAIT, &w);

/* 校验：out[i] == a*in[i] + b */
```

真实运行输出（guest 内 `/root/saxpy`，autorun 里有同一段）：

```
saxpy: a=3 b=7 n=4096 (out[i] = a * in[i] + b)
launch: grid=64 block=64 -> seq#13
PASS: saxpy correct on the compute engine (4096 elements verified)
--- compute stats (cat /sys/kernel/debug/vgpu/compute) ---
kernels   1
blocks    64
warps     128
threads   4096
waves     8
instrs    49152  (12.0 per thread)
mem_ops   24576
broadcast warps    512
coalesced warps    256
strided warps      0
modelled mem latency 266240 ns
```

### 5.6 读懂计算统计

`cat /sys/kernel/debug/vgpu/compute` 解读（这台机器每次 launch 的典型数字）：

| 字段 | 含义 | 教学点 |
|---|---|---|
| `warps` / `threads` | warp=32 lane；threads = grid × block | GPGPU 的执行单位是 warp 不是线程 |
| `waves` | `ceil(threads / (SM数×每SM线程上限))` | 为什么 occupancy 决定"要跑几轮" |
| `instrs (per thread)` | 指令退役数 | 寄存器/内存指令比 |
| `mem_ops` | 全局访存次数 | saxpy = 每线程 6 次 |
| `broadcast warps` | 所有 lane 读同一个字 | 1 个事务（参数加载就是这种） |
| `coalesced warps` | 32 lane 读连续字 | 1 个事务——**saxpy 的两次数据访问都在这里** |
| `strided warps` | 跨步/散列访问 | 32 个事务，性能塌方 |

上面这组数字的真实含义：`4` 次参数加载/ warp（全部 broadcast）+ `2` 次数据访问/warp（全部
coalesced）= 每 warp 6 次访存全部**单事务**。把 `code[]` 里第 7 条 `LD r6, [r5 + 0]` 的 `.b = 0`
改成一个大跨步常量（等价于用户态把 `in[]` 按 1024 元素跨度排布），再看这个文件——
`strided warps` 立刻从 0 变成 128，而 `mem_ops` 不变。**"内存合并"从抽象概念变成可观测数字，
就是这一刻。**

---

## 6. 实例二：统一内存（managed VA + 按需迁移）

SAXPY 把参数 buffer 用 **GEM handle** 命名（`launch.args_bo`）；phase E 增加了一种命名方式：
**GPU 虚拟地址**（`VGPU_PKT_LAUNCH_VA`）。`userspace/examples/uvm_saxpy.c` 与 `saxpy.c` 用的是**同一份
ISA 程序**，差别只在 packet 里那个字段：

```c
pk->type = VGPU_PKT_LAUNCH_VA;   /* 不是 VGPU_PKT_LAUNCH */
pk->launch_va.kid     = ml.kid;
pk->launch_va.base_va = va;      /* VM_ALLOC 拿到的地址，page 对齐 */
pk->launch_va.size    = ARGS_BYTES;
```

配套的两步（这就是 CUDA `cudaMallocManaged` 在 vgpu 的对应物）：

```c
struct vgpu_vm_alloc a = { .size = ARGS_BYTES };
ioctl(fd, DRM_IOCTL_VGPU_VM_ALLOC, &a);        /* a.va: 我的 context 的一个 VA */
struct vgpu_vm_bind b = { .va = a.va, .bo_handle = bo.handle,
                          .flags = VGPU_VM_BIND_MANAGED };   /* 关键：managed */
ioctl(fd, DRM_IOCTL_VGPU_VM_BIND, &b);
```

之后**每一个** LD/ST 都走这个 fd 的页表：命中就直接访问，缺 PTE 就走 phase C 的按需重映射把区间迁回
驻留、再把这次访问重放。kernel 完全不知道刚才发生过缺页——这就是 Unified Memory 对程序员的样子
（对驱动就是"缺页迁移"，和 CPU swap 同构）。

示例的做法是把证据逼出来：budget 通过可写 debugfs 压到 0（区间被驱逐）→ launch → 首次访存缺页迁回 →
校验 out[] 全对 → 断言 `demand-faults` / `engine vm faults` 计数上涨 → 再来一轮。典型输出：

```
[vgpu:vm] vmid 1: fault va 0x1000 -> demand mapped 20480 B back into VRAM
[vgpu:uvm] seq#39: 1 demand fault(s) on va 0x1000 + 20480 B (migrated in mid-kernel)
PASS: round 1: 1 range(s) evicted, 1 demand fault(s) serviced mid-kernel, 5 page(s) remapped, 1 engine fault(s)
PASS: unified memory — the device reached va 0x1000 after its pages were evicted, transparently (2048 elements verified...)
PASS: the same buffer bound WITHOUT VGPU_VM_BIND_MANAGED rejects a VA-named launch (-ENOENT)
```

最后一行是这条机制的边界：不带 `MANAGED` 标志绑的同一个 buffer 不是统一内存，VA 命名 launch 寻址它
会在门铃处被 `-ENOENT` 拒绝——"没声明 managed 的地址是 fault，不是巧合"。

---

## 7. 排障

| 现象 | 原因 | 处理 |
|---|---|---|
| `open /dev/dri/*: No such device` | 模块没加载 / 节点没建 | `insmod /root/vgpu.ko` + 用 sysfs 的 dev 号 `mknod` |
| `doorbell: -EINVAL` | put 未 40B 对齐 / 没前进 / 越界 / block 不是 32 倍数 / kid 不存在 / VA 未 page 对齐或越界 | 看 §5.5 的校验清单 |
| `doorbell: -ENOENT` | VA 命名 launch 寻址的区间没绑、或没带 `MANAGED` 标志绑 | 看 §6；PROBE/隔离用 `vm` 视图确认区间状态 |
| `wait: -ETIMEDOUT` | engine 卡住或超时太短 | 查 `pending`；ring 满会 `-EBUSY` |
| `wait: -22 (-EINVAL)` | engine 级错误（如错位 SET） | `dmesg` 里 `[vgpu:engine] ... ERROR` |
| KASAN 报告 | 真 bug（本 repo 的规矩：先看现场再改） | 用 FTRACE-GUIDE 的 `func_stack_trace` / gdb |
| autootest 停在某阶段 | 该阶段 FAIL | 从第一个 FAIL 往上读；guest shell 还在 |

## 8. 文档导航

| 想知道 | 去看 |
|---|---|
| 每个机制为什么这么设计 | [DESIGN.md](DESIGN.md) + [adr/](adr/) |
| 逐机制动手（9 个实验） | [LEARNING-PATH.md](LEARNING-PATH.md) |
| ftrace 系统教程 | [FTRACE-GUIDE.md](FTRACE-GUIDE.md) |
| 下一站：GPGPU 化路线 | [GPGPU-EVOLUTION-PLAN.md](GPGPU-EVOLUTION-PLAN.md) |
| 术语的严格定义 | [../CONTEXT.md](../CONTEXT.md) |
