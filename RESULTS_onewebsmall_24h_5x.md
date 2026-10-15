# 24h OneWebSmall 全策略端到端结果（usr1 → grd1）

- 日期：2026-10-14 · 基线提交：`e3115e8` · 二进制：`build/bin/hypersim`（GCC，CPU）
- 命令（真实运行，非估算）：
  ```sh
  timeout 5400 build/bin/hypersim --policy none,all --times 5 --pair u1,gw1 \
      --output benchmark_runs/onewebsmall_24h_5x
  ```
- 语义：无位置输入 → hypersim 调用同目录 `satnet`→`satpred` 生成 **OneWebSmall 24h** 上游产物
  （49 星，`start=2026-01-01T00:00:00Z`，`duration=86400s`，`step=10s`，`package=3600s`）
  → 24 包 / **8640 片** / 873290 有向边 → 在剪枝拓扑上跑业务+traceroute+iperf。
- 端点：`src=49`（`u1`）、`dst=50`（`gw1`），即 **usr1 → grd1**。
- 覆盖：**8 策略 × 5 次 = 40 runs**，全部 `SOURCE_VERIFIED` / `status=complete`。
- 总耗时：约 **43 分钟**（输入生成 ≈ 60s + 40 × 平均 69.6s 仿真，`sum=2784s`，min 61.7s / max 75.7s）。

## 结果（每策略 5 次平均）

| policy | n | goodput_bps | delivered_bytes | completed_tr | reached_tr | handover_events | user_ho | ground_ho |
|---|---|---|---|---|---|---|---|---|
| none (baseline) | 5 | 91203.7 | 9.850e+08 | 640.0 | 480.0 | 0 | 0 | 0 |
| random | 5 | 91203.7 | 9.850e+08 | 621.6 | 462.6 | 167.4 | 78.2 | 75.6 |
| min-access-delay | 5 | 91203.7 | 9.850e+08 | 617.0 | 458.0 | 305.0 | 145.0 | 145.0 |
| min-full-delay | 5 | 91203.7 | 9.850e+08 | 639.0 | 479.0 | 185.0 | 131.0 | 136.0 |
| max-min-visibility | 5 | 91203.7 | 9.850e+08 | 622.0 | 464.0 | 40.0 | 32.0 | 32.0 |
| min-orbit-hops | 5 | 91203.7 | 9.850e+08 | 639.0 | 480.0 | 160.0 | 118.0 | 119.0 |
| max-access-rate | 5 | 91203.7 | 9.850e+08 | 617.0 | 458.0 | 305.0 | 145.0 | 145.0 |
| min-lifetime | 5 | 91203.7 | 9.850e+08 | 620.0 | 462.0 | 130.0 | 58.0 | 57.0 |

## 关键发现

1. **吞吐/交付量与策略无关**：`goodput_bps=91203.7037`、`delivered_payload_bytes=9.85e8` 在全部 8 策略 × 5 次中**完全一致**。
   原因：当前为 `single-flow-fluid-v1` 单流流体模型，端到端交付由该 (u1,gw1) 路径的瓶颈容量决定，
   切换哪颗卫星不改变路径瓶颈；`offered=1.08e10`、`unserved=9.815e9` 亦恒定。
2. **切换频率差异显著**：`max-min-visibility` 最少（40 events，32/32），`min-access-delay` 与 `max-access-rate` 最多（305 events，145/145）。
   两者结果完全一致（直接接入时延最小 ↔ 接入带宽最大在本样例中同解）。
3. **确定性可复现**：确定性策略 5 次逐位一致（如 `min-access-delay` 恒为 617/458/145）；
   `random` 因 seed=seed+rep 而变化（completed_tr 621–622，user_ho 75–83），符合设计。
4. **traceroute 计数**：baseline 640/480，剪枝后 617–639，说明单星约束确实改变了逐跳可见性，但未改变端到端交付量（见 1）。
5. **iperf 为真实 socket 实测**：loopback TCP 0.1s，实测 ≈ **12.65 Gbps**（158105600 B / 0.1000s），非公式推算。
6. **handover 摘要（min-access-delay 示例）**：`slices=8640`，`u1` 可见 1993 片、`gw1` 可见 1977 片、双端同连 788 片，
   `user_disconnects=5`、`ground_disconnects=3`、`threshold_relaxed_slices=0`。

## 产物（`benchmark_runs/onewebsmall_24h_5x/`，gitignored）

- `onewebsmall-policies-comparison.json`（8 策略汇总）、`onewebsmall-<policy>-series.json` × 8
- 每 run（40 份）：`business.jsonl`、`statistics.jsonl`、`traceroute.txt`、`traceroute.jsonl`、
  `result.json`、`iperf.json`、`manifest.json`、`performance.json`、`effective-config.json`
- 策略模式额外（7 策略 × 5 = 35 份）：`visibility.csv`、`selection.jsonl`、`handover.jsonl`、`handover-summary.json`
- 上游输入：`network_input/`（24 `.ngb` + 24 `.sbin` + manifest + exports）

## 复现

```sh
rm -rf benchmark_runs/onewebsmall_24h_5x
timeout 5400 build/bin/hypersim --policy none,all --times 5 --pair u1,gw1 \
    --output benchmark_runs/onewebsmall_24h_5x 2>&1 | tail -n 20
# 校验：40 份 manifest
find benchmark_runs/onewebsmall_24h_5x -name '*-manifest-*.json' | wc -l   # 40
```

## 三工具（business / traceroute / statistics）相互影响实测

同一 24h OneWebSmall 输入（`network_input/`，baseline），4 组对照，各跑 1 次：

| 组 | 参数 | delivered | goodput | offered | unserved | completed_tr | stats 行 | trace 行 | 墙钟 exec |
|---|---|---|---|---|---|---|---|---|---|
| all | 默认（tr 10s / stats 1s） | 985000000.0 | 91203.70370370371 | 10800000000.0 | 9815000000.0 | 640 | 86400 | 641 | 67.4s |
| notrace | `--probe-interval-s 100000` | 985000000.0 | 91203.70370370371 | 10800000000.0 | 9815000000.0 | 1 | 86400 | 1 | 3.4s |
| nostats | `--report-interval-s 100000` | 985000000.0 | 91203.70370370371 | 10800000000.0 | 9815000000.0 | 640 | 1 | 641 | 67.6s |
| neither | 两者都关 | 985000000.0 | 91203.70370370371 | 10800000000.0 | 9815000000.0 | 1 | 1 | 1 | 1.5s |

产物 sha256(前16) 对比：

| 文件 | all | notrace | nostats | neither |
|---|---|---|---|---|
| traceroute-1.txt | abf33a49dddfed25 | a4505199273047b7 | **abf33a49dddfed25** | a4505199273047b7 |
| traceroute-1.jsonl | 5e163e1606bec17f | 9069e3231aa76a11 | **5e163e1606bec17f** | 9069e3231aa76a11 |
| business-1.jsonl | 4cdf7040ace334a2 | b5011df8fce379d7 | **4cdf7040ace334a2** | b5011df8fce379d7 |
| result-1.json | 4fca8100f34538e5 | 8844ed899fda729a | **4fca8100f34538e5** | 8844ed899fda729a |
| statistics-1.jsonl | 1af68838efb41f78 | 3e2408ff7bb3fc27 | 34c237198810dbbe | 34c237198810dbbe |

### 结论：业务层面无相互影响

1. **业务结果逐位一致**：`delivered / goodput / offered / unserved` 在 4 组中**完全相同**（traceroute、statistics 开关均不改变业务）。
2. **statistics 不影响 traceroute**：`all` 与 `nostats` 的 `traceroute.txt`、`traceroute.jsonl` sha **逐位相同**。
3. **statistics 不影响 business**：`all` 与 `nostats` 的 `business.jsonl`、`result.json` sha **逐位相同**。
4. **唯一差异在 `statistics.jsonl`**：`all` vs `notrace` 有 6778/345600 个数值单元不同，**最大绝对差 2.33e-9**（goodput 相对 ~2.5e-14），且 `route_reachable`/`bottleneck_bps` 无差异 —— 属 **long double 浮点累加舍入**（探测边界把积分区间切得更细，求和次序不同），**非物理干扰**。

### 机理（代码）

- `business`：事件队列 + 链路队列/带宽；`integrate()` 为**线性**累加 `rate*dt`。
- `traceroute`：独立解析模型 `hsn::trace::Engine`，只读拓扑（`source.at(t)`），**不占用业务队列/带宽**、不修改业务累加器。
- `statistics`：`stats_row()` 纯读取累加器后写出，**不改状态**。
- 三者共享的只有虚拟时钟边界与墙钟 CPU：线性积分 ⇒ 边界切分不改变总量（业务逐位一致）；墙钟开销可叠加（traceroute 占 ~64s / 67s，statistics 近零），但不进入业务结果。

### 边界与前提

- 上述"无干扰"成立的前提是当前模型：**无排队 ICMP traceroute**（README/SDD 明确）+ 单流 Fluid。若将来引入真实 ICMP 排队/限速、多流 Max-Min 或 TCP，traceroute/业务将竞争队列，需重新验证。
- 墙钟层面并非真正并发：三者在同一进程主循环中交错执行，故无数据竞争；仅 CPU 时间可叠加。


## 平均端到端延迟与带宽

数据来源：40 次运行的 `traceroute.jsonl`（到达目的地的探针 RTT）与 `business.jsonl` 的 `route_change` 事件
（`propagation_delay_ns`=单向路径时延，`bottleneck_bps`=路径各链路容量最小值）。

### 总平均

| 指标 | 值 |
|---|---|
| 单向路径时延（业务路径，route_change） | **≈ 86.9 ms**（p50 86.8，范围 83.1–92.1 ms） |
| traceroute RTT（往返） | **≈ 175.3 ms**（p50 ~175，范围 166.3–187.2 ms；单向 ≈ 87.6 ms） |
| 路径瓶颈带宽 | **≈ 11.8 Mbps**（p50 11.1，范围 9.85–18.19 Mbps） |
| 实际业务 goodput（交付吞吐） | **≈ 91.2 kbps**（offered=1 Mbps；u1↔gw1 同时可见仅 788/8640=9.1% 时间） |
| 平均路径跳数 | ≈ 25–27 节点（单平面 49 星 Walker star，需绕行约半环） |

### 分策略（延迟 vs 带宽权衡）

| policy | 单向时延 ms | 瓶颈 Mbps | 跳数 | traceroute RTT ms |
|---|---|---|---|---|
| baseline | 85.43 | 10.96 | 25.2 | 170.6 |
| random | 88.03 | 12.16 | 26.4 | 177.5 |
| min-access-delay | 89.22 | **13.53** | 27.0 | 178.4 |
| min-full-delay | 85.42 | 10.95 | 25.2 | **170.6** |
| max-min-visibility | 89.70 | 11.43 | 26.6 | 178.4 |
| min-orbit-hops | **85.36** | 10.99 | 25.1 | 170.9 |
| max-access-rate | 89.22 | **13.53** | 27.0 | 178.4 |
| min-lifetime | 87.90 | 11.55 | 26.2 | 177.7 |

**结论**：存在明确的**延迟 ↔ 带宽权衡**——
`min-full-delay`/`min-orbit-hops` 选到最短路径（~85.4 ms、25 跳）但瓶颈较低（~10.95 Mbps）；
`min-access-delay`/`max-access-rate` 选到接入带宽最大的星（~13.5 Mbps）但路径更长（~89.2 ms、27 跳）。
差距约 **4.5% 延迟 / 23% 带宽**。

> 注：`goodput` 远低于瓶颈（91 kbps vs 11.8 Mbps）并非链路受限，而是该 (u1,gw1) 地理对在单平面星座下**仅 9.1% 时间同时可见**；可达时 1 Mbps 全额交付。


## 95% 置信区间与显著性判定（差异是真实还是误差）

方法：以**每次运行**为独立单元（n=5），用 t(0.975, df=4)=2.776 计算运行级 CI；对均值型指标（时延/带宽/RTT）另计算**单次运行内时序样本**的 95% CI，并用 lag-1 自相关做有效样本量修正（`n_eff=n(1-ρ)/(1+ρ)`）。

### 运行级 95% CI（n=5 runs）

| 参数 | baseline | random | min-access-delay | min-full-delay | max-min-visibility | min-orbit-hops | max-access-rate | min-lifetime |
|---|---|---|---|---|---|---|---|---|
| completed_tr | 640.00±0.00 | 621.60±0.68 | 617.00±0.00 | 639.00±0.00 | 622.00±0.00 | 639.00±0.00 | 617.00±0.00 | 620.00±0.00 |
| reached_tr | 480.00±0.00 | 462.60±1.42 | 458.00±0.00 | 479.00±0.00 | 464.00±0.00 | 480.00±0.00 | 458.00±0.00 | 462.00±0.00 |
| handover_ev | 0.00±0.00 | 167.40±4.17 | 305.00±0.00 | 185.00±0.00 | 40.00±0.00 | 160.00±0.00 | 305.00±0.00 | 130.00±0.00 |
| user_ho | 0.00±0.00 | 78.20±3.87 | 145.00±0.00 | 131.00±0.00 | 32.00±0.00 | 118.00±0.00 | 145.00±0.00 | 58.00±0.00 |
| ground_ho | 0.00±0.00 | 75.60±3.58 | 145.00±0.00 | 136.00±0.00 | 32.00±0.00 | 119.00±0.00 | 145.00±0.00 | 57.00±0.00 |
| goodput_bps | 91203.70±0.00 | 91203.70±0.00 | 91203.70±0.00 | 91203.70±0.00 | 91203.70±0.00 | 91203.70±0.00 | 91203.70±0.00 | 91203.70±0.00 |
| 1way_delay_ms | 85.43±0.00 | 88.03±0.13 | 89.22±0.00 | 85.42±0.00 | 89.70±0.00 | 85.36±0.00 | 89.22±0.00 | 87.90±0.00 |
| bottleneck_Mbps | 10.96±0.00 | 12.16±0.15 | 13.53±0.00 | 10.95±0.00 | 11.43±0.00 | 10.99±0.00 | 13.53±0.00 | 11.55±0.00 |
| hops | 25.21±0.00 | 26.36±0.05 | 27.00±0.00 | 25.21±0.00 | 26.61±0.00 | 25.11±0.00 | 27.00±0.00 | 26.24±0.00 |
| rtt_ms | 170.63±0.00 | 177.51±0.26 | 178.40±0.00 | 170.64±0.00 | 178.39±0.00 | 170.94±0.00 | 178.40±0.00 | 177.70±0.00 |

> **除 `random` 外全部策略 5 次运行逐位相同 → 运行间方差 = 0**（确定性）。`random` 是唯一有运行间方差的策略（seed=seed+rep）。

### 样本级 95% CI（单次运行内，含自相关修正）

| policy | 单向时延 ms | 瓶颈 Mbps | RTT ms |
|---|---|---|---|
| baseline | 85.427±0.261 | 10.956±0.143 | 170.634±0.564 |
| random | 87.925±0.357 | 12.075±0.591 | 177.463±0.898 |
| min-access-delay | 89.217±0.152 | **13.534±2.470** | 178.396±0.082 |
| min-full-delay | 85.420±0.261 | 10.949±0.143 | 170.638±0.501 |
| max-min-visibility | 89.696±0.830 | 11.434±0.616 | 178.391±1.072 |
| min-orbit-hops | 85.360±0.220 | 10.986±0.147 | 170.938±0.521 |
| max-access-rate | 89.217±0.152 | **13.534±2.470** | 178.396±0.082 |
| min-lifetime | 87.900±0.271 | 11.549±0.524 | 177.700±0.726 |

### 判定结论

**① 真实存在（统计显著，可复现）**
- **端到端时延**：短路径组（baseline / min-full-delay / min-orbit-hops）≈ **85.4 ms（RTT ~170.6–170.9）** vs 带宽优先组（min-access-delay / max-access-rate / max-min-visibility）≈ **89.2–89.7 ms（RTT ~178.4）**。差值 3.8–4.3 ms（RTT 7.5–7.8 ms）**远大于 CI（±0.15–1.07 ms）→ 显著**。
- **切换次数 / 跳数 / traceroute 计数**：确定性精确值，组间差异巨大（如 handover 0/40/130/160/185/305，hops 25.1/27.0）→ **显著**。

**② 不是真实差异（落在误差内）**
- **goodput**：40 次运行**完全相同**（91203.70370370371）→ **零差异**（任何"策略影响吞吐"的说法不成立）。
- **路径瓶颈带宽**：min-access-delay/max-access-rate 13.53 Mbps 看似比 min-full-delay 10.95 Mbps 高 **23%**，但其 95% CI = **±2.470 Mbps**（ρ1=0.941、n_eff≈4），下界 11.06 与 min-full-delay 上界 11.09 **重叠** → **不显著，不能排除是采样误差**。
- **min-full-delay vs min-orbit-hops**：时延 85.420 vs 85.360 ms（Δ=0.06 ms < CI）→ **无显著差异**（同解）。
- **min-access-delay vs max-access-rate**：数值完全相同 → **无差异**。

### 方法论边界
- 结论针对本输入（OneWebSmall 24h、u1→gw1、单平面 49 星）；跨星座/端点需更多输入样本才能外推。
- 确定性策略的运行间方差为 0，故其"不确定度"仅来自单次运行内对时变拓扑的有限采样（已修正自相关）；瓶颈带宽 ρ1≈0.94（切换少 → 独立样本少）正是其 23% 优势不可靠的根因。

