# 功能缺口分析：代码实现 vs 需求文档

- 生成日期：2026-10-14
- 仓库：`/mnt/c/Users/cheng/templete`（UnifiedSatNet） · 基线提交：`e3115e8`
- 对照文档：
  - `docs/HyperSim-Net_Cpp_SDD_v2.3_Ling-Can-Shu-Mo-Ren-Zhi-Zheng-He-Ban.txt`（HSN-SDD-005 v2.3.0，1296 行）
  - `docs/satpred_satnet_Chun-C-Xu-Qiu-Gui-Ge-_v3.4_Luo-Ji-Tong-Yi-Ban.txt`（SATPRED-SATNET-CPP v3.4，1342 行）
- 代码：`src/simulation.cpp`、`include/hsn/*`、`src/*.inc`（hypersim）；`satpred/src/core.cpp`（satpred/satnet/satread/netread/satdemo/orbit_engine）
- 方法：`grep -n` / `sed -n` 切片阅读 + 前台 `timeout` CLI 实测（见 §E），未改动任何已封版源码，未污染 Git 工作区。

图例：✅ 已实现 · 🟡 部分实现 · ❌ 未实现 · ⛔ 未实现（属文档"未来功能/须明确拒绝"） · ⚪ 不适用/结构性差异

---

## A. HyperSim-Net SDD v2.3（hypersim）逐段对照

| 章节 | 文档要求 | 代码位置 | 状态 | 证据 / 说明 |
|---|---|---|---|---|
| §1.2 | CMake 为 HyperSim 唯一构建入口 | `CMakeLists.txt`、`build_gcc.py` | 🟡 | CMake 注册 `hsncore/hypersim/hsn-traceroute` 与 5 个 CTest；本环境实际用 `build_gcc.py`，CMake 未实测（REFACTOR_NOTES 声明 NOT_TESTED） |
| §1.3 | 目录 `include/hsn/{core,graph,vtime,coro,netstack,gpu}`、`frontend/pyhsn`、`tools/{hsn-verify,hsn-bench}` | `include/hsn/`（扁平 8 个头） | ❌ | 无 vtime/coro/netstack/gpu 分层；无 Python 前端目录；无 hsn-verify/hsn-bench 工具 |
| §1.4 | 调度主线程唯一提交 SimClock/跨分区状态 | `src/simulation.cpp` 单线程主循环 | 🟡 | 单线程事件循环满足"唯一提交"精神，但无 `SimClock` 对象与显式提交点 |
| §2.1 | 输入 = SATNET 目录/manifest + 绑定源 `.sbin` + **可选 Python 脚本** | `include/hsn/reader_internal.hpp` | 🟡 | 目录/manifest/单 `.ngb` 均支持（`Input` 类）；`--script`/Python 脚本输入 ❌ |
| §2.2 | 废弃旧 HSNG | — | ✅/⚪ | 代码未使用任何 HSNG 结构 |
| §2.3–2.5 | 256B 头/32B 索引/32B 边/CRC32C/`EdgeView` 只读视图 | `satpred/src/core.cpp`（graphread） | 🟡 | 字节布局与 CRC 由 satpred 实现并被复用；无独立 `EdgeView` 类，未按 §2.3 `#pragma pack` 断言结构 |
| §3.1 | JSON 严格：拒重复键/NaN/Inf/无效 UTF8 | satpred `json()` | ✅ | `--selftest` 断言重复键被拒 |
| §3.2 | nodes 按 graph id 数组，kind 0/1/2 | `reader_internal.hpp`（nodes） | ✅ | 与 satpred 图 metadata 一致 |
| §3.3 | 正式 manifest SATNET-1.1 + products 字段 | satpred 写、hypersim 读 | 🟡 | 读侧校验 `format/run_id/products[].graph_sha256/source_sha256`；hypersim 自身 manifest 为自有格式 `HYPERSIM-UNIFIED-MVP-1`，字段仅部分对齐 |
| §3.4 | 多包按 k0 排序，重复/缺口/重叠/stem 冲突拒绝 | `reader_internal.hpp` L39–41 | ✅ | `gap/overlap`/`incomplete run`/`annotation mismatch` 检查 |
| §4 | SATC 轨道与状态（SAMPLES major3/minor0） | satpred `readorbit/write` | ✅ | 覆盖校验、CRC、NaN 语义由 satpred 提供 |
| §4.2 | Q32/CHEB/INITIAL/CHECKPOINT 扩展 | — | ⛔ | 未实现，且未用 f32/改名伪装（符合"合法未实现须拒绝"） |
| §5.1–5.3 | 加载顺序 / CRC32C / SHA256 | satpred | ✅ | `--selftest`：`123456789→E3069283`、SHA256("abc"/"") FIPS 向量 |
| §5.4 | 统一 Error `code/where/message/line/col` | `hypersim_main`/`application` | 🟡 | `code/where/message` 有；`line/col` 恒为 `-1`（无行列定位） |
| §5.5 | 内存预算 / 主推进零分配 / 不全 `read_bytes` | `simulation.cpp`（`std::priority_queue`/`std::map`） | ❌ | 事件队列、链路表、路由缓存均动态分配；无预算分项预检；README/manifest 明确不承诺 |
| §6.2 | `SimClock`/`HostPerfClock` 强类型隔离 | — | ❌ | 使用裸 `int64_t` 时间 + `std::chrono::steady_clock`，无强类型封装 |
| §6.3–6.4 | 业务记录字段（node/flow/topology_index）+ ISO 9 位 | `Simulation::log()` | 🟡 | `sim_timestamp_ns/sim_elapsed_ns/event_id/type/payload` 有；`node/flow/topology_index` 未统一写入 |
| §7 | 主循环（起点事件/边界/同刻闭包/断边策略 v1） | `run()`/`dispatch()` | 🟡 | 主循环与同刻顺序（topology→completion→timeout→traffic）已实现；断边策略简化为 drop，未实现文档级 `edge-down-policy-v1` 版本化 |
| §8.1 | 5 层 256 槽定时轮（L0=1ms） | — | ❌ | 用 `std::priority_queue` 替代，无 5 层轮/溢出策略 |
| §8.3 | 双缓冲 macro 步（读 T-1 写 T，提交点 swap） | — | ❌ | 未实现双缓冲 |
| §8.4 | `Pool<T>` 固定索引对象池 | — | ❌ | 未实现；Event/Packet/Flow/Probe 容量与耗尽策略缺失 |
| §9 | Python 前端 + 协程（AST 子集、TaskFrame、FrameArena） | 内置 C++ `builtin:default_probe` 逻辑 | ❌ | 无 `--script`/AST/协程；内置探针语义以 C++ 内联实现（未提供脚本时的等价行为） |
| §10.1 | 最短时延路由、整数权重、tie 节点 id 升序 | `hsn::trace::shortest` | ✅ | 有向 CSR 最短传播时延 |
| §10.1 | loss/jitter 模型（`ideal-loss-v1`/`bernoulli-v1`/`gaussian-v1`） | — | ❌ | 无 `--loss-model/--loss-rate/--jitter-*`（实测 unknown option） |
| §10.2 | Fluid 多流 Max-Min 公平 | 单流流体 | ❌ | manifest warning："fluid implements one active flow, not general multi-flow max-min" |
| §10.2 | Packet 模型（DropTail100/MTU1500/UDP1200） | `packet_mode` | 🟡 | DropTail/MTU/1200B 有；TCP 算法/cwnd/RTO 无 |
| §10.3 | iperf `-t/-i/-b/-J`，冻结 golden | `run_iperf_loopback` | 🟡 | 产出 `iperf3-compat-v1` JSON 且为**真实 loopback TCP 套接字**测量；未做官方全字段/golden 版本锁定 |
| §11 | CUDA 与时间并行（TPS exact/approx、cuda-graph-batch） | — | ❌ | hypersim 无 CUDA/TPS/checkpoint |
| §12 | 测试数据 D1–D11、格式/时间/主循环专项用例 | `tests/fixtures.py` 合成夹具 | 🟡 | 仅合成拓扑；无真实 648 星 D4/D5、10GiB D7、恶意变异 D9 等 |
| §13 | 验收、加速比计量、追溯 | `reports/`、`provenance/` | ❌ | 无正式验收报告；README/manifest 明确"不声称通过完整需求验收" |
| §14.2 | 全参数默认值表（约 80 项） | `include/hsn/defaults.hpp`（30 项） | ❌ | 见 §C 缺失参数清单 |
| §14.3 | `defaults.hpp` 唯一来源 + `--config`/`HSN_*` 链 + 构建期一致性测试 | `defaults.hpp` + `src/options.inc` | 🟡 | 有 typed canonical 注册表与 `--dump-defaults`；缺 `--config`、`HSN_*` 覆盖、构建期一致性测试 |
| §14.4 | `builtin:default_probe` 语义（全部参数来自 EffectiveConfig） | `simulation.cpp` | ✅ | C++ 等价实现：无 user/ground 记 `no_traffic`，否则 start_flow + 周期 traceroute |
| §14.5 | 零参数默认流程 | `run()` 无输入分支 | ✅ | 生成 OneWebSmall 24h → 模拟 → 输出 |
| ZERO004 | 优先级 内置→`--config`→`HSN_*`→CLI | `options.inc` | ❌ | 实测 `--config` 返回 `Arg`（unknown option）；无 HSN_* 覆盖 |
| ZERO005 | effective_config 来源 default/file/env/cli | `config()` | 🟡 | 仅 `"default/computed"` 与 `"cli"` 两类来源 |
| ZERO003 | `--help`/defaults.json/effective_config 同一注册表 | `--help`/`--dump-defaults` | 🟡 | 二者均源自 `defaults[]`；但 `--help` 仅列 name/value，不含 type/unit；`--dump-defaults` 固定写 `./simulation_defaults.json`（不可指定路径） |
| ZERO008 | 自动发现源轨道，找不到 strict 下失败 | `reader_internal.hpp` | ✅ | 单 `.ngb` 同级 `orbits[/samples]` 查找，找不到报 `source` 错误 |
| ZERO009 | 输出 `<HSN_HOME>/runs/run_NNNNNN` | `src/runtime_io.inc` | ✅ | 支持 `HSN_HOME`，最小未用序号，`--output` 非空拒绝 |
| §14.6 | ZERO-T01..T14 | `tests/default_flow.py`、`tests/regression.py` | 🟡 | 仅 `ZERO-T14`、`ZERO-T15` 落地 |
| 附录 A/B/C | 上游字节规范/run_id/读取验收原文 | 由 satpred 实现 | ⚪ | 规范原文，不单独实现 |


---

## B. satpred / satnet v3.4（satpred 系列）逐段对照

| 章节 | 文档要求 | 代码位置 | 状态 | 证据 / 说明 |
|---|---|---|---|---|
| §1.1 | 源码分层 `src/{base,text,xml,json,hash,npy,cfg,time,earth,env,dyn,io,net}/...` | 单文件 `satpred/src/core.cpp`（3794 行） | ❌ | 未按文档目录拆分；声明与实现未分离（`reader_internal.hpp` 直接 `#include core.cpp`） |
| §1.2 | 唯一构建系统 CMake（>=3.20），不用 Python 构建脚本 | `satpred/CMakeLists.txt`、`build_gcc.py` | 🟡 | CMake 目标齐全，但本环境用 `build_gcc.py`；CMake 未实测 |
| §2.1 | `Result<T>` 恰含 value/Error，禁异常跨模块 | `sp::Err` 异常 + `application()` 统一转换 | 🟡 | ErrorCode 0–7 齐全、`main` 统一捕获；但用异常而非 `Result<T>`；`line/col` 恒 `-1` |
| §2.2 | UTC/十进制秒 nearest-even/`duration`/`step` 校验 | `utc/seconds/iso` | ✅ | `--selftest` 覆盖 `0.0000000005→0`、`1e-3`、UTC 往返 |
| §2.4 | 最短往返浮点、JSON/CSV 禁 NaN/Inf 字面量 | `numfmt`/写出器 | 🟡 | 基本实现；未做完整 Ryu 等价审计 |
| §2.5 | 受限 XML：行列号、深度>64、>16MiB、DTD 拒绝 | `xml` | 🟡 | 限制项基本覆盖；**行列定位不完整**（IMPLEMENTATION_STATUS 第 5 条自述） |
| §2.6 | JSON 自实现（拒重复键/尾逗号/NaN/深度） | `json` | ✅ | `--selftest` 重复键拒绝 |
| §2.7 | `.npy` v1/v2 `<f8` C-order `(N,6)` | `npy()` L2368 | ✅ | 头部/dtype/order/shape/size 全校验 |
| §3.1 | `--help/--version/--dump-defaults` 互斥、只输出不运行 | `options()` L1926 | ✅ | 显式互斥校验 |
| §3.2 | satpred CLI（`-c/--config/--settings/--start/--duration/--step/--package/--max-h/--env-step/--batch/--threads/--frame/--output/--initial-npy/--exclude-*`） | `defaults("satpred")` | ✅ | 实测 `--help` 列全 |
| §3.3 | satnet CLI + §3.3.1 数值范围校验 | `defaults/validate` | ✅ | `--help` 列全；范围先校验再转换 |
| §3.4 | satread/netread/satdemo（info/verify/csv） | `mainapp` L3738+ | ✅ | 无参读 `last_success.json`，无标记时调 satdemo |
| §4.1 | XML 层/Walker/explicit/几何公式/名称 | `constellation()` | ✅ | Walker star/delta/explicit，逐星覆写已实现 |
| §4.2 | radio/phys 继承、系统默认值 | `constellation`/`phys` | ✅ | 继承链完整 |
| §4.3/§4.3.1 | faults 静态/事件、包区间唯一语义 | `constellation`/`write` | ✅ | 状态事件半开区间、status0、STATUS_EVENTS |
| §4.4 | 地面 XML、允许零地面 | `network_xml` | ✅ | |
| §4.5 | 自带样例 OneWebSmall/OneWeb648 | `satpred/examples/` | ✅ | 24h 默认 49 星；648 为回归数据 |
| §5.1 | settings.json 14 键、未知键拒绝 | `settings()` L1425 | ✅ | 未知键 `Config` |
| §5.2 | CSV/JSON 外部覆盖（eop/ephemeris/density/gravity/phys） | `settings()`/`extread` | ✅ | 覆盖率/严格递增/SHA/size 校验，写入 `inputs.external_data[]`/`phys_sources[]` |
| §5.3 | 初值（解析/`.npy`） | `init`/`npy` | ✅ | |
| §6 | 数学模型与积分（12 级 DOP853、J2–J4、drag、第三体、SRP、相对论） | `rk8`/`force` | ✅ | 真实数值积分（非占位）；**精度未独立验证**（无 SciPy/ERFA oracle） |
| §6.4 | 日月 Schlyter 类低精度解析 | `sun_moon` | ✅ | 与文档口径一致（非 DE440） |
| §7.1.1 | 环境覆盖与精确时间（EOP ±0.5s、节点覆盖预检） | `settings`/`env` | ✅ | 覆盖不足 `Config` |
| §7.2/§7.2.1 | orbit_engine 进程内接口 + 续接/首次失败 | `engine` | ✅ | 窗口续接、失败集合累计、M1 只输出初态 |
| §7.3/§7.3.1 | 内存预算与有界资源 | `predict` L2137 | 🟡 | 按包缓冲 + 预检，超预算 `Config`；**非严格流式有界内存**（自述第 4 条） |
| §7.4 | SATC SAMPLES 写出（major3/minor0/CRC/NaN） | `write` | ✅ | |
| §7.5 | SATC 读取判定（段/CRC/跨包） | `readorbit` | ✅ | |
| §7.6 | 单星失败语义与退出码 | `predict` | ✅ | `complete_with_failures`/退出 4 |
| §8.0–8.9 | 网络算法（网格索引/mesh/knn/天线/带宽/延迟/整数化） | `network`/`bandwidth`/`delay` | ✅ | `duplex-shared-antennas-isl-only`、`distance-power`、`ellipsoid-segment` |
| §8.5/§8.5.1 | 拓扑 `Result` 注册表（插件式 Topology） | — | ❌ | mesh/knn 为内建分支，无注册表/`LegacyTopology` 兼容层 |
| §9.1–9.4 | SATNET/源绑定/读取器/输出目录 | `graph`/`write` | ✅ | `.part` 原子发布、源 SHA 绑定 |
| §10.1 | 注册表 `{name,type,default,unit,min,max,stage,scope}`，`--dump-defaults` 输出注册表 JSON，`simulation_defaults.json`（仅输出不执行） | `defaults(app)` L1815、`sp::mainapp` L3763 | ❌ | `defaults()` 仅 `name→string`；实测 `--dump-defaults` 输出**有效值字符串 map**（无 type/unit/min/max/stage）；satpred 侧**无** `simulation_defaults.json`（仅 hypersim 侧有） |
| §10.2 | links.json 导出（rate 6 位/delay 3 位/pos WGS84） | `write` L3027 | ✅ | `satnet-links-1`，含 raw 字段 |
| §10.3 | 导出有效期与流式约束 | `write` | ✅ | 按片流式，尾片不延长 |
| §11.1 | canonical run_id（`satpred-run-v1`/`satnet-run-v1` 规范字节） | `predict`/`network` L2438+ | 🟡 | 采用 **MVP 身份**，非 §11.1 规范 canonical；manifest 写 warning（自述第 3 条） |
| §11.2 | manifest（`SATPRED-B-1`/`SATNET-1.1`） | `manifest()` | ✅ | 字段基本齐全 |
| §11.3 | 事务/`.part`/取消清理 | `write` | 🟡 | 进程级原子可见；**断电持久性未认证**（`crash_durability_certified=false`） |
| §11.4 | last_success 与自家子进程编排 | `child()`/`updatesuccess()` | ✅ | 子进程参数数组，无 shell 拼接 |
| §12/§13 | 测试体系与门槛 L0–L3 | `satpred/tests/regression.py`、`--selftest` | 🟡 | L0 部分（CRC/SHA/时间/JSON）；**L1-01..L1-12 全部未运行**（仓库无 SciPy/ERFA/numpy oracle）；L2-01/L2-01b/F.3-3 有，L2-02..L2-07 部分；L3-01（GPU/CPU）有；L3-02 性能未做；L3-03 F 项 NOT_TESTED |
| §13 L0-11 | 产品内案例常量与 `examples/case.json` 逐值一致的构建期测试 | — | ❌ | 未加入构建（自述第 13 条） |
| 附录 D | orbit_engine 诊断协议 v1（ORBSTAT1/ORBENV01/ORBST01 帧+CRC） | `orbitEngine()` L3552 | ✅ | 头/CRC/边界、slice/result 帧、窗口续接逐位一致 |
| §14 | 未来义务 F（F-DATA/TIME/COORD/ENV/MATH/INIT/DYN/CHEB/IO/SCHED/TOOL/NET-EXT/GPU/QA/PERF/OPTIONAL） | — | ⛔ | 全部未实现；按 §14.0 未实现请求应返回 `Arg`（已实现 F 请求拒绝，未生成伪产物） |

---

## C. 缺口汇总（按优先级 / 影响面）

### C1. 影响"一键基准测试"或"真实网络实测"的缺口（高优先级）
1. **hypersim 参数注册表不完整**：`include/hsn/defaults.hpp` 仅 30 项，SDD §14.2 要求约 80 项。缺失（实测 unknown option）：`--config`、`--end`、`--script`、`--source-mode`、`--source-dir`、`--legacy-minor0`、`--verify-checksum`、`--log-format`、`--log-level`、`--performance-report`、`--time-semantics`、`--realtime-pacing`、`--topology-hold`、`--position-hold`、`--max-same-time-microsteps`、`--timer-wheel-levels`、`--seed`、`--route-policy`、`--route-recompute`、`--edge-down-policy`、`--queue-model`、`--loss-model`、`--loss-rate`、`--jitter-model`、`--jitter-sigma-ns`、`--traffic-start-s`、`--traffic-end-s`、`--tcp-*`、`--traceroute-facade`、`--ip-mapping`、`--fluid-recompute`、`--threads`、`--event-pool`、`--packet-pool`、`--flow-pool`、`--probe-pool`、`--frame-arena-bytes`、`--csr-cache-slices`、`--backend`、`--cuda-graph-batch`、`--tps-mode`、`--tps-slices`、`--tps-warmup-s`、`--allow-unverified-source`、`--watchdog-s`、`--checkpoint`、`--checkpoint-interval-s`、`--resume`、`--fuzz`。
2. **ZERO004 配置优先级链缺失**：无 `--config` 文件、无 `HSN_*` 环境覆盖；`effective_config` 来源只有 `default/computed` 与 `cli`（缺 `file/env`）。
3. **satpred §10.1 注册表不符**：`--dump-defaults` 输出有效值字符串 map，非含 `type/unit/min/max/stage/scope` 的注册表 JSON；satpred 侧不生成 `simulation_defaults.json`。
4. **hypersim 调度/内存架构未达文档**：无 5 层 256 槽定时轮（用 `std::priority_queue`）、无双缓冲、无 `Pool<T>`、无零分配预算审计。

### C2. 业务/模型能力缺口（中优先级）
5. **Fluid 仅单流**：SDD §10.2 要求多流 Max-Min 公平；当前 manifest 自述单流。
6. **TCP 未实现**：`validate()` 显式拒绝 `--traffic-protocol tcp`；SDD §10.2 TCP cubic/MSS/RTO 未实现。
7. **loss/jitter 模型未实现**：无 `--loss-model/--loss-rate/--jitter-*`。
8. **Python 前端与协程未实现**（SDD §9）：无 `--script`、AST 子集、TaskFrame/FrameArena；仅内置 C++ 等价探针。
9. **CUDA/TPS/checkpoint 未实现**（SDD §11）：hypersim 无 GPU、无时间并行、无断点续算（satpred 侧仅 G 候选评估）。
10. **satpred 拓扑插件注册表未实现**（§8.5/§8.5.1）：mesh/knn 内建，无 `Result<Topology>` 注册。

### C3. 身份/精度/验收缺口（中低优先级）
11. **规范 canonical run_id 未实现**（satpred §11.1）：现为 MVP 身份，manifest 带 warning。
12. **数值精度未认证**：satpred §6/§13 要求对 SciPy/ERFA 独立误差报告（L1-03 等），仓库无 oracle，`numeric_accuracy_certified=false`。
13. **行列号定位不完整**：XML 错误无完整 `line/col`；统一 Error 的 `line/col` 恒 `-1`。
14. **构建期一致性测试缺失**：satpred §13 L0-11、SDD §14.3 构建期注册表测试未加入构建。
15. **测试矩阵覆盖不足**：satpred L1 全缺、L2 部分、L3-02/L3-03 未做；hypersim 仅 ZERO-T14/T15。

### C4. 明确属"未来功能 F"（须拒绝，非缺陷）
16. satpred §14 全部 F 义务（数据锁定/完整时间尺度/IAU 定向/DE440/NRLMSISE/自主数学/Brouwer-Lyddane/Cheb/Q32/全容器/调度恢复/工具/网络扩展/GPU 传播/QA/性能/压缩）：均为 ⛔，产品对未实现 F 请求返回 `Arg`。
17. SATC Q32/CHEB/INITIAL/CHECKPOINT 段、SATNET minor0 legacy 迁移：⛔ 未实现且未伪装。

---

## D. 已确认实现项（避免误判为缺口）

- **真实网络实测链路**：hypersim `run_iperf_loopback` 为真实 loopback TCP 套接字测量（`sys/socket.h`、`SO_RCVTIMEO/SO_SNDTIMEO`、`select`），非公式推算/Mock；`iperf3-compat-v1` JSON 输出。
- **traceroute**：主程序内建定向无排队 ICMP 模型（`hsn::trace`），正向/返回路分别计算，超时/不可达/censored 明确。
- **handover policy**：7 种策略 + baseline，`--pair`/`--times`/`--policy all`/`--policy-dump-links`，可见性 CSV/选择/切换/汇总/links 导出，63+ 回归项。
- **上游 satpred**：外部数据 5 类（eop/ephemeris/density CSV + gravity/phys JSON）、Walker 逐星覆写、orbit_engine 附录 D 诊断协议、CUDA G 候选评估（`--backend cpu|cuda|auto`）、links.json、SATC/SATNET 读写与源绑定、`last_success` 与子进程编排。
- **hypersim 零参数默认流程**：生成 OneWebSmall 24h（24 包/8640 片）→ 模拟 → 输出 → 成功 manifest（`tests/default_flow.py`）。
- **基准脚本**：`run_benchmark.sh`（8 策略 × 5 次 × 3 工具 = 40 runs，真实 socket）。
- **基础原语**：自实现 XML/JSON/SHA256/CRC32C/UTC 十进制秒，`--selftest` 校验 RFC 3720 与 FIPS 向量。

---

## E. 复现命令（前台 `timeout`，临时目录，避免污染工作区）

```sh
# 构建（本环境无 CMake 时）
python3 build_gcc.py 2>&1 | tail -n 5

# HSN：参数/默认值实测
timeout 20 build/bin/hypersim --help | head -n 40
timeout 20 build/bin/hypersim --config /tmp/x.json /tmp      # -> Arg: unknown option config
timeout 20 build/bin/hypersim --loss-model bernoulli-v1 /tmp # -> Arg: unknown option loss-model
timeout 20 build/bin/hypersim --time-semantics virtual-utc-v1 /tmp # -> Arg: unknown option time-semantics

# satpred：注册表/默认值实测（cd /tmp 防止写运行目录）
cd /tmp && timeout 20 /mnt/c/Users/cheng/templete/build/bin/satpred --dump-defaults | head -c 400
timeout 20 /mnt/c/Users/cheng/templete/build/bin/satnet --help | head -n 30

# 原语自检 + 回归（全量，约 2 分钟，含清洁环境 24h）
timeout 600 python3 run_tests.py build/bin 2>&1 | tail -n 25

# 一键基准
timeout 900 bash run_benchmark.sh 2>&1 | tail -n 15
```

---

## F. 方法与边界说明

- 本文仅做"代码 vs 文档"的**静态对照 + CLI 冒烟实测**，未做数值精度验收、未运行 L1/L3 外部 oracle 测试，故标注 🟡/❌ 的项不等于"必然错误"，而是"文档要求未在当前仓库中证实"。
- `IMPLEMENTATION_STATUS.md` 自述的 14 项缺口已逐条与代码交叉核对；其中第 1/2/6/8 条（外部数据/Walker 覆写/附录 D/CUDA）经复核**已落地**，第 3/4/5/7/9/10/11/12/13/14 条**仍成立**并纳入 §C。
- 未修改任何已封版源码；Git 工作区除新增本文件外保持干净。

