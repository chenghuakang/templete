# Handover policy 功能说明

## 概览
模拟器原生支持为一对 **用户终端(USS侧) – 地面站(GSS侧)** 选择点对点 handover policy。程序流程：

1. 读取原始网络数据（satnet 产物目录 / manifest / 单个 .ngb）。
2. 对该对节点，在**整个输入数据时间范围**内逐片收集所有可见接入链路，并反向扫描计算每条链路的
   **剩余可见时间**（连续可见的后续切片数 × step），生成第一批新数据 `*-visibility-*.csv`。
3. 按策略对每片剪枝：用户和地面站各自仅保留一颗卫星（其它接入边删除；ISL 与其它节点不变），
   生成第二批新数据 `*-selection-*.jsonl`（逐片选择）、`*-handover-*.jsonl`（仅切换事件）、
   `*-handover-summary-*.json`（统计）；`--policy-dump-links enabled` 时额外导出剪枝后全部边 `*-links-*.csv`。
4. 主模拟器（UDP 业务、traceroute、统计）在剪枝后的拓扑上运行；切换同时记入 business 日志（`handover_initial`/`handover`），
   result 中给出 `handover_policy`、`user_handovers`、`ground_handovers`。

不加 `--policy` 时使用原始数据（含全部候选接入边，逐跳最短时延路由在其中选路，不施加单星约束），policy 名记为 `baseline`。

## 命令行
```
hypersim [input] --policy <name> [--pair USER,GROUND | --pair USER GROUND] [--lifetime-threshold-s 120]
                 [--policy-seed 1] [--times N] [--policy-dump-links enabled]
                 [--constellation auto|name] [--run-index auto|N]
```
- 只有一个用户和一个地面站时 `--pair` 可省略（auto）；多个时必须指定。接受 nodes.xml 中的 id 名（如 `u1,gw1`）或图中数字 id，顺序可颠倒。
- 指定 `--policy` 且未显式给 `--traffic-src/--traffic-dst` 时，业务流自动取该对节点。
- 候选集合 = 输入数据中该端点的全部接入边。上游 satnet 已改为**保留所有可见接入链路**（终端/地面站的 `antennas`
  不再限制候选卫星数，天线预算只约束星间链路），链路选择完全下移到模拟器的 policy 中。
- `--policy` 可同时给多个策略：`--policy random,min-lifetime`、`--policy random min-lifetime`（多个 token）或 `--policy all`（全部 7 种）。
  每个策略独立运行 `--times` 次：共 策略数 × times 次仿真，每个策略的序号都是 1..times，文件名互不冲突
  （`星座-random-traceroute-1.txt`、`星座-min-lifetime-traceroute-1.txt`…）。每个策略写出自己的 `星座-policy-series.json`，
  另有 `星座-policies-comparison.json` 汇总全部策略各次的 result。列表中可包含 `none`（baseline）；重复或未知名称报错。
- `--times N`：在同一份输入上运行 N 次，结果序号为 1..N（`星座-policy-工具-N.后缀`），每次使用 seed、seed+1、…
  （random 策略因此每次不同；确定性策略各次结果相同，可用于复现/计时）。另写出 `星座-policy-series.json` 汇总各次 result。
  `--times` 与 `--run-index` 互斥。

## 策略定义（上帝视角，切片粒度 = 数据 step）
| 名称 | 触发 | 规则 |
|---|---|---|
| `random` | 当前卫星到期断连 | 在可见卫星中均匀随机（`--policy-seed` 可复现），用户/地面站各自独立 |
| `min-access-delay` | 出现指标更好的卫星 | 选直接接入时延最小的卫星，各端独立 |
| `min-full-delay` | 出现指标更好的 (USS,GSS) 对 | 端到端时延 = d(用户,USS)+ISL 最短时延(USS→GSS)+d(GSS,地面站) 最小 |
| `max-min-visibility` | 当前对任一侧到期 | 对所有 (USS,GSS) 对取 min(USS 剩余可见, GSS 剩余可见)，仅保留该值最大的对 |
| `min-orbit-hops` | 出现跳数更少的对 | 端到端跳数 = 1+ISL 跳数(USS→GSS)+1 最小 |
| `max-access-rate` | 出现带宽更大的卫星 | 选直接接入带宽（双向瓶颈）最大的卫星，各端独立 |
| `min-lifetime` | 当前卫星到期断连 | 先筛掉剩余可见 < 阈值（默认 120 s）的链路，再取剩余可见最长者；无链路通过阈值时退化为全体最长并标 `threshold_relaxed` |

并列时优先保持当前卫星，其次取 id 较小者。`trigger`：`initial`/`hold`/`expiry`/`better`/`none`/`unreachable_fallback`
（端到端策略下无可达对时保留接入时延最短的链路）。剩余可见时间以**输入数据末尾**为上限，不以仿真窗口为上限。

## 输出命名
所有结果按 `星座名称-policy名称-工具-序号.后缀` 命名，如 `onewebsmall-random-traceroute-1.txt`、`onewebsmall-random-result-1.json`。
- 星座名称：`--constellation auto` 取网络 manifest 的 `case_name`（默认流程 OneWebSmall → `onewebsmall`），默认流程回退到 satellites.xml 的 `name`，否则 `satnet`；可 `--constellation oneweb` 覆盖。
- 序号：`--run-index auto` = 输出目录中同前缀文件最大序号 + 1（新目录即 1），或手工指定。
- 工具：business / statistics / traceroute（.txt 与 .jsonl）/ result / performance / effective-config / manifest；策略模式另有 visibility / selection / handover / handover-summary / links。
- manifest 的 `files` 字段列出全部文件名，`output_sha256` 覆盖所有数据文件，`handover` 字段含策略摘要。

## 测试
`tests/regression.py` 共 63 项检查（新增 7 种策略在 3 星 + u1/gw1 合成拓扑上的确定性选择、触发标签、可见性 CSV、阈值退化、随机复现、
`--pair` 两种写法、命名、baseline 与非法输入）；`tests/test_options.cpp` 增至 43 项；`tests/fixtures.py` 的 `make()` 新增 `ground=[(name,kind)]`。
验证环境：Linux/GCC 15，`python3 build_gcc.py` 构建；CMake/Windows 未实测。

## 代码位置
- `include/hsn/handover.hpp` / `src/handover.inc`：策略枚举、pair 解析、可见性计算、选择与剪枝（`HandoverPlan`）、`Topology` 视图。
- `src/simulation.cpp`：`Simulation` 改用 `Topology`；切换事件日志；`OutputNames` 命名；`run()` 中的策略流程与数据导出。
- `include/hsn/defaults.hpp` / `src/options.inc`：新选项与校验（`--pair A B` 双 token 解析）。

## 上游 satnet 的配套变更（satpred/src/core.cpp）
- 链路分配：接入链路（kind=1）不再参与天线贪心，所有可见接入链路全部写入图，两端均不消耗天线；星间链路仍共享卫星天线预算。
- 图校验（graphread/netread）：“antenna overuse” 只统计星间链路。
- 图元数据：`models.resource = duplex-shared-antennas-isl-only`，`models.access = all-visible-candidates-selection-in-simulator`。
- 已移除模拟器侧的临时选项 `--candidate-antennas`。
