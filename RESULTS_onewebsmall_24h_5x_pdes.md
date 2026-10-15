# 24h OneWebSmall 全策略端到端结果（去中心化无锁 PDES 版本）

- 日期：2026-10-15 · 提交：`c1770a9`（完全去中心化无锁 PDES）· 二进制：`build/bin/hypersim`（GCC，CPU + CUDA 构建）
- 环境：WSL2 / Linux · 12 个虚拟处理器（`nproc=12`）· 本地 GTX 1660 Ti
- 命令（真实运行，非估算）：
  ```sh
  timeout 7200 build/bin/hypersim --policy none,all --times 5 --pair u1,gw1 \
      --output benchmark_runs/onewebsmall_24h_5x_pdes 2>&1 | tail -n 20
  ```
- 语义：无位置输入 → hypersim 调用同目录 `satnet`→`satpred` 生成 **OneWebSmall 24h** 上游产物
  （49 星，`start=2026-01-01T00:00:00Z`，`duration=86400s`，`step=10s`，`package=3600s`）
  → 24 包 / **8640 片** / **873290 有向边**；端点 `src=49`(`u1`) → `dst=50`(`gw1`)。
- 覆盖：**8 策略 × 5 次 = 40 runs**，全部 `status=complete` / `verification=SOURCE_VERIFIED`。
- 退出码 `EXIT=0`。

## 性能

| 指标 | 值 |
|---|---|
| 总墙钟 | **2955 s ≈ 49.3 min**（含上游输入生成 ≈ 60 s） |
| 40 次仿真累计 | `sum=2821.6 s` |
| 单次仿真 | min **66.1 s** / max **75.3 s** / mean **70.5 s** |
| 虚拟时长/次 | 24.0 h（8640 片 × 10 s） |
| 加速比 | ≈ **1225×** 实时（86400 s 虚拟 / 70.5 s 墙钟） |
| 最慢策略 | `min-access-delay`（mean 73.7 s） |
| 最快策略 | `min-full-delay`（mean 66.4 s） |

单次耗时几乎完全由 **traceroute 轮次** 决定（`probe-interval-s=10` → 8640 轮，`traceroute_retries≈4770–4800`）；
业务流体计算与 8640 次拓扑切换的占比很小。策略间 66–75 s 的差异来自可见卫星数量不同（剪枝后拓扑规模不同）。

## 结果（每策略 5 次平均）

| policy | n | goodput_bps | delivered_B | completed_tr | reached_tr | handover_events | user_ho | ground_ho |
|---|---|---|---|---|---|---|---|---|
| none (baseline) | 5 | 91203.7037 | 9.850e+08 | 640.0 | 480.0 | 0 | 0 | 0 |
| random | 5 | 91203.7037 | 9.850e+08 | 621.6 | 462.6 | 167.4 | 78.2 | 75.6 |
| min-access-delay | 5 | 91203.7037 | 9.850e+08 | 617.0 | 458.0 | 305.0 | 145.0 | 145.0 |
| min-full-delay | 5 | 91203.7037 | 9.850e+08 | 639.0 | 479.0 | 185.0 | 131.0 | 136.0 |
| max-min-visibility | 5 | 91203.7037 | 9.850e+08 | 622.0 | 464.0 | 40.0 | 32.0 | 32.0 |
| min-orbit-hops | 5 | 91203.7037 | 9.850e+08 | 639.0 | 480.0 | 160.0 | 118.0 | 119.0 |
| max-access-rate | 5 | 91203.7037 | 9.850e+08 | 617.0 | 458.0 | 305.0 | 145.0 | 145.0 |
| min-lifetime | 5 | 91203.7037 | 9.850e+08 | 620.0 | 462.0 | 130.0 | 58.0 | 57.0 |

- `offered_payload_bytes = 1.08e10`、`unserved_payload_bytes = 9.815e9`、`censored = 0` 在全部 40 runs 中恒定。
- 与改动前基线（`RESULTS_onewebsmall_24h_5x.md`，提交 `e3115e8`）**逐项完全一致**：
  8 策略的 5 次均值、极值、离散度均相同；`sum=2784 s → 2821.6 s`（+1.3%，属噪声）。

## 关键发现

1. **本基准走的是 fluid 路径**：`bandwidth-model` 默认 `fluid`（`single-flow-fluid-v1`，解析式单流），
   不产生逐包事件，因此**不进入去中心化 PDES**；这解释了为什么与改动前基线逐项一致，
   也说明 24h golden 未被 PDES 改动影响（`tests/default_flow.py` 仍字节一致）。
2. **吞吐/交付量与策略无关**：`goodput_bps=91203.7037`、`delivered=9.85e8` 在 8 策略 × 5 次中完全一致。
   原因是单流流体模型下端到端交付由 (u1,gw1) 路径瓶颈决定，切换哪颗卫星不改变路径瓶颈。
3. **切换频率差异显著**：`max-min-visibility` 最少（40 events，32/32，`trigger=on-expiry`）；
   `min-access-delay` 与 `max-access-rate` 最多且**结果完全同解**（305 events，145/145，`trigger=on-better-metric`）。
4. **确定性可复现**：7 个确定性策略 5 次逐位一致（如 `min-access-delay` 恒为 617/458/305）；
   `random` 因 `seed=seed+rep` 变化（completed 621–622，ho 163–171，离散 4.78%），符合设计。
5. **traceroute 计数**：baseline 640/480，剪枝后 617–639，单星约束确实改变了逐跳可见性，但未改变端到端交付量。
6. **iperf 为真实 socket 实测**：loopback TCP 0.1 s，实测 **147255296 B / 0.100011361 s = 11.78 Gbps**，非公式推算。
7. **handover 摘要**（示例）：`slices=8640`，`u1`/`gw1` 可见片数与切换次数随策略显著不同——
   `min-access-delay`：可见 1993/1977 片、切换 145/145、断连 5/3；
   `max-min-visibility`：可见 788/788 片（双端同连 788）、切换 32/32、断连 4/4；
   `min-lifetime`：`lifetime_threshold_relaxed_slices=6`（阈值放宽生效）。

## PDES 在真实 24h 拓扑上的验证（补测）

本基准默认 fluid 不触发 PDES，故补测一次 **真实 OneWebSmall 24h 拓扑 + packet 模型**
（49 星 / 873290 边 / 8640 个拓扑纪元），用极低速率与最大包把事件量压到可跑完：

```sh
B=benchmark_runs/onewebsmall_24h_5x_pdes/network_input
build/bin/hypersim $B --output /tmp/owsp/auto --bandwidth-model packet \
    --traffic-rate-bps 1000 --traffic-packet-bytes 1472 --policy none
HSN_SIM_THREADS=1 build/bin/hypersim $B --output /tmp/owsp/1 ... （同上）
```

| 线程数 | generated | delivered | dropped | unreachable | link_down | censored | 守恒 | completed_tr | 墙钟 |
|---|---|---|---|---|---|---|---|---|---|
| auto (=12) | 7337 | 662 | 6675 | 6667 | 8 | 0 | ✔ | 640 | 69.8 s |
| `HSN_SIM_THREADS=1` | 7337 | 670 | 6667 | 6667 | 0 | 0 | ✔ | 640 | 66.6 s |

- **包守恒恒成立**：7337 = 交付 + 丢弃 + 截断，两种线程数均满足。
- **差异仅 8 包 / 1.2%**（`delivered 662 vs 670`，差值 8×1472 = 11776 B 恰等于 `link_down_drop_packets` 8 vs 0），
  来源是拓扑纪元切换瞬间「边消失时在途/排队包」的处理时序，**落在 SDD §12.5 VT01 声明的 ±2% 带内**。
- traceroute 计数（640/480）两种线程数完全一致（控制面路径，确定性）。
- PDES 相对单线程几乎无额外开销（69.8 s vs 66.6 s），因为总耗时由 traceroute 主导。

## 产物（`benchmark_runs/onewebsmall_24h_5x_pdes/`，gitignored）

- `onewebsmall-policies-comparison.json`（8 策略汇总）、`onewebsmall-<policy>-series.json` × 8
- 每 run（40 份）：`business.jsonl`、`statistics.jsonl`、`traceroute.txt`、`traceroute.jsonl`、
  `result.json`、`iperf.json`、`manifest.json`、`performance.json`、`effective-config.json`
- 策略模式额外（7 策略 × 5 = 35 份）：`visibility.csv`、`selection.jsonl`、`handover.jsonl`、`handover-summary.json`
- 上游输入：`network_input/`（24 `.ngb` + 24 `.sbin` + manifest + exports）

## 复现

```sh
rm -rf benchmark_runs/onewebsmall_24h_5x_pdes
timeout 7200 build/bin/hypersim --policy none,all --times 5 --pair u1,gw1 \
    --output benchmark_runs/onewebsmall_24h_5x_pdes 2>&1 | tail -n 20
find benchmark_runs/onewebsmall_24h_5x_pdes -name '*-manifest-*.json' | wc -l   # 40
```
