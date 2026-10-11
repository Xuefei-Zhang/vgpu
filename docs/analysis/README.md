# dequant_kv 实例分析：一个 LLM 形态的 kernel 在 vGPU 上完整跑通

> 本目录是一次完整“实例教学”的产物：一个 **LLM serving 形态的 kernel**（反量化 + 累加 + 缓存簿记的融合）在 vgpu 上跑通，
> 并用 ftrace 抓下它的**全链路函数图**，逐段分析。
>
> - [dequant-kv.md](dequant-kv.md) —— 这个例子是什么、kernel 逐条讲、它证明了什么
> - [trace-analysis.md](trace-analysis.md) —— 抓 trace 的方法 + 函数图逐段标注分析
> - [raw/dequant-kv-trace.log](raw/dequant-kv-trace.log) —— 原始 trace（5686 行，function_graph）

## 一分钟复现

```bash
# host 上
scripts/10-build.sh && scripts/20-initramfs.sh && scripts/30-run.sh
# guest 里（autorun 已包含，或手动）
/root/dequant_kv
```

抓本目录里的那份 trace 的命令（guest 内）：

```sh
cd /sys/kernel/debug/tracing
FNS='vgpu_doorbell_ioctl vgpu_engine_work_compute vgpu_compute_run vgpu_run_warp vgpu_mem_word vgpu_irq_handler vgpu_irq_bh_work vgpu_finish_submission vgpu_wait_ioctl'
echo function_graph > current_tracer
echo $FNS > set_ftrace_filter      # 两个都要设，只设 set_graph_function 会什么都不记
echo $FNS > set_graph_function
echo > trace && echo 1 > tracing_on
/root/dequant_kv
echo 0 > tracing_on
cat trace                            # 先 dump，再 echo nop > current_tracer（复位会清缓冲）
```

## 这个例子在 LLM 里的位置

量化推理的每个 decode step 都要做这件事：权重量化值（INT8）带着 per-channel 的 scale/zero-point，
kernel 必须**一次遍历内**完成“反量化 → 累加进 KV cache → 更新缓存簿记”。拆成三个 kernel 就是三次 launch
和三次显存往返——融合正是为了消掉它们。这个例子把那个融合 shape 原样跑在 vgpu 上。

## 一句话结论

**跑通了，而且全链路可观测**：一次 doorbell、一个 launch、512 个 slot 全部正确；
trace 里能清楚看到提交路径（CPU0）、引擎执行（CPU1）、模拟中断上下半部、fence 唤醒阻塞的等待者
——这正是这个项目要教的那条链，只不过这次上面跑的是一个“长得像 LLM kernel”的东西。

## 它能跑 / 不能跑什么（诚实边界）

| LLM kernel 类别 | 在 vgpu 上 |
|---|---|
| embedding lookup / gather、bias/scale add、RoPE、量化反量化 | ✅ element-wise 整数运算，现在就能跑 |
| 本例子：dequant + accumulate + 簿记融合 | ✅ 见 dequant-kv.md |
| KV cache 写入/拷贝 | ✅ DMA 引擎（BLIT）更快 |
| 小矩阵乘（K ≤ 16 展开） | 🟡 指令预算内可演示 |
| LayerNorm/softmax（规约） | 🟡 只能多趟 launch 凑，没有原子操作 |
| 真实 GEMM、FlashAttention、GELU/SiLU | ❌ 没有循环/分支/浮点/超越函数 |

结构性缺口和补齐成本见 [GPGPU-EVOLUTION-PLAN.md](../GPGPU-EVOLUTION-PLAN.md) 的护栏章节：
循环+分支约 1 天、原子加半天、shared memory + barrier 一天——每一项都能让可跑类别跳一档。
