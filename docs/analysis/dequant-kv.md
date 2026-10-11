# dequant_kv：这个 kernel 是什么，它证明了什么

源码：[userspace/examples/dequant_kv.c](../../userspace/examples/dequant_kv.c)（约 210 行，静态链接，guest 里 `/root/dequant_kv`）。

## 1. 它在 LLM serving 里的原型

量化推理（INT8/INT4 权重）的每个 decode step，每个被 attention 读到的权重块都要：

```
cache[i]  += scale * q[i] + zero_point      // 反量化 + 累加
version[i] += 1                              // 缓存簿记（哪一份 KV 是新的）
```

生产实现会把这三件事融进**一个 kernel**：拆开就是三次 launch、三次显存往返。这个例子原样实现这个融合 shape。

## 2. kernel 逐条（20 条指令，每线程负责一个 slot）

ISA 只有 6 条可用指令（MOVI/ADD/MUL/LD/ST/EXIT），寄存器 16 个、`r0` 预置线程号、`r15` 置 0 当基址。
指令表见 [DESIGN.md §5](../DESIGN.md)；完整参考在 `driver/uapi/vgpu_drm.h`。

```
 0: MOVI r15, 0            ; 基址（header 在 word 0..6）
 1: LD   r1,  [r15+1]      ; scale
 2: LD   r2,  [r15+2]      ; zero_point
 3: LD   r3,  [r15+3]      ; q_base     ┐
 4: LD   r4,  [r15+4]      ; out_base   │ 三条 region 的基址
 5: LD   r5,  [r15+5]      ; ver_base   ┘
 6: LD   r6,  [r15+6]      ; 常数 1（没有“立即数加”，只能从内存拿）
 7: ADD  r7,  r3, r0       ; &q[i]
 8: LD   r8,  [r7+0]       ; q[i]（量化值，一个 word 存一个）
 9: MUL  r9,  r1, r8       ; scale * q
10: ADD  r10, r9, r2       ; + zero_point  ← 反量化完成
11: ADD  r11, r4, r0       ; &out[i]
12: LD   r12, [r11+0]      ; cache[i]（旧值）
13: ADD  r13, r12, r10     ; 累加
14: ST   [r11+0], r13      ; 写回 cache
15: ADD  r14, r5, r0       ; &ver[i]
16: LD   r12, [r14+0]      ; version[i]
17: ADD  r13, r12, r6      ; + 1
18: ST   [r14+0], r13      ; 写回 version
19: EXIT
```

**内存访问的形状**（这是 trace 分析的依据）：每线程 11 次访存 = 9 LD + 2 ST，
其中 6 次是**广播**（所有 lane 读同一个 header word）+ 5 次是**合并**（lane 读连续 word：
q[i]、out[i]、ver[i] 及其写回）。16 个 warp × 32 lane × 11 = **5632 次访存**，与 trace 里
`vgpu_mem_word` 的出现次数逐字相符（见 trace-analysis.md）。

## 3. 与 CUDA 的对应

| CUDA | vgpu | 本例子 |
|---|---|---|
| `__global__ void dequant_kv(...)` | `code[]`（用户态“汇编”） | 20 条指令 |
| `<<<grid, block>>>` | `VGPU_PKT_LAUNCH` packet | grid=8 block=64 → 512 线程 |
| 量化参数随权重走来 | 一个 BO 的 header（word 0..6） | scale/zp/三个基址/常数 1 |
| `cudaDeviceSynchronize()` | `VGPU_WAIT`（内核态等 fence） | doorbell 之后一次 ioctl |
| launch 前 CPU 写 in[] | mmap 该 BO 直接写 | 同一份内存 |

## 4. 它证明了什么

1. **“element-wise + 可展开累加”这一类 LLM kernel 在这里是真实可跑的**：不是概念演示——512 个 slot、
   三个内存区域、每次 launch 全部验证通过（`cache[i] == 1000 + 3*q[i] + 7`）。
2. **融合的 shape 是真的**：一次 doorbell 里完成反量化、累加、簿记——trace 里只有一次
   `vgpu_doorbell_ioctl`、一次 `vgpu_compute_run`。
3. **这条驱动链对“计算”是中立的**：提交路径、地址空间、访存合并、fence、中断两半部，
   对 saxpy 和对这个 kernel 是同一套。这正是项目的主张——它模拟的是**驱动基座**，
   计算语义是“足够真”的替身。

## 5. 诚实的边界

- **没有位运算**（无 shift/mask），所以真实 kernel 的“打包 INT8（一个 word 两个值）”无法表达；
  例子里量化值**一个 word 存一个**。融合的 shape 不变，存储布局是简化。
- **没有循环/分支**：点积、softmax 这类需要累加循环的 kernel 只能靠“用户态展开”，
  而 kernel 上限 64 条指令 → 展开深度约 20。这就是“小矩阵乘只能演示”的原因。
- **没有原子操作**：跨线程规约（LayerNorm 的均值）只能多趟 launch 凑。
- **没有浮点/超越函数**：GELU/SiLU 的 exp/tanh 不在指令集里。
- 这些缺口的补齐成本和顺序见 [GPGPU-EVOLUTION-PLAN.md](../GPGPU-EVOLUTION-PLAN.md)。
