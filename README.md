# 本版本

新增 handover policy（`--policy random|min-access-delay|min-full-delay|max-min-visibility|min-orbit-hops|max-access-rate|min-lifetime`，`--pair USER,GROUND`），
结果文件统一命名 `星座-policy-工具-序号.后缀`（如 `onewebsmall-random-traceroute-1.txt`，无策略时为 `baseline`）。详见 HANDOVER_NOTES.md。
上游 satnet 现在保留所有可见接入链路（终端天线数不限制候选卫星数），卫星选择由模拟器的 policy 完成；`--times N` 在同一输入上重复运行 N 次，序号 1..N；`--policy a,b,c|all` 对每个策略各运行 N 次。

基于缺陷修复版（RELEASE_NOTES_FIX.md）的结构性重构，详见 REFACTOR_NOTES.md；本次测试证据在 reports/，历史报告在 provenance/。
无 CMake 环境：`python3 build_gcc.py` 构建，`python3 run_tests.py build/bin` 全量测试。

---

# UnifiedSatNet：三份代码合并后的统一 C++ 工程

本包合并用户最新TXT中的 satpred_cpp17_mvp.zip、OneWebSmall_incremental_update.zip、HSN_traceroute_fix.zip。
实际上传的三包是“上游轨道/网络 + 默认案例增量 + traceroute修复”，没有上一轮独立HyperSim engine.cpp。本次补充了统一网络仿真主程序，直接在主程序事件调度器内提交修复后的探针，不调用外部traceroute进程。

目标优先正确输入/输出，不作性能目标、完整traceroute工具兼容或v3.4/v2.3全量验收承诺。

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUNIFIED_TESTS=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

一个根CMake构建图，目标包含原六个上游工具、hypersim、hsn-traceroute、测试工具。上游C++17，新主程序/探针模块C++20。
本次当前环境无CMake，源码用GCC直接编译和运行验证；CMake配置未实测，Windows未测试。生产程序不需要Python，tests/*.py只是测试驱动。

## 一个入口跑全流程

```sh
./build/bin/hypersim
```

无输入时：创建HSN_HOME运行目录 → 调用同目录satnet（satnet调用satpred） → OneWebSmall 24h产物 → 验证 → 主程序模拟UDP → traceroute/统计 → 成功manifest。

上游默认为49星、单轨道1200km/87.9°，两个指定终端，地面发送/接收各50Mbps，RF2.1/1.3GHz独立注释。时间86400s/step10s/package3600s。旧648星样例在 satpred/examples/OneWeb648。

指定旧程序输出作为输入：

```sh
./build/bin/hypersim /path/to/network_run --output simulation_result
./build/bin/hypersim /path/to/network_run --bandwidth-model packet --output packet_result
./build/bin/hypersim /path/to/network_run --start 2026-01-01T04:40:00Z --duration 20 --output trace_check
```

默认src=first-user、dst=first-ground，UDP1Mbps、载荷1200B、DropTail100包，流体模式。可选 --traffic-src/--traffic-dst 显式图ID。
默认每次trace结束后再虚拟等待10秒；每TTL3探针、最多32跳、单探针超时5秒。业务统计每1秒。

## 输出

实际文件名形如 `onewebsmall-baseline-result-1.json`（manifest 的 `files` 字段列出全部）；下面按工具说明。

- result.json：网络业务汇总（offered/delivered/unserved、平均goodput；packet模式额外包数分类与守恒）。
- statistics.jsonl：逐虚拟秒统计，不用主机计算时间作分母。
- business.jsonl：拓扑/业务/traceroute事件，整数模拟UTC纳秒。
- traceroute.txt：主程序直接写标准TTL/IP/三RTT/*文本块。
- traceroute.jsonl：每probe的真实发送/完成ns、Reply/Timeout/Censored、响应者和到达状态。
- effective_config.json、performance.json、manifest.json：有效参数、独立主机性能、输入/业务摘要。
- 无参数生成时 network_input/ 是上游完整产物；显式目录输入时 source_snapshot/ 保存图/源快照。

## 合并关键修复

1. 消除C++17/20 filesystem::u8string的char8_t不兼容；UTF8路径通过共同pathtext桥接。
2. traceroute探针只预规划纯图模型响应，响应提交为内核Completion/Timeout事件；主clock不提前跳到trace末。
3. 虚拟等待期间继续逐片拓扑、流体积分、packet服务/到达和统计；同刻先拓扑，再完成/超时，最后统计。
4. 每TTL3次探针；目的回复才reached，不再用ttl>=path.size()判到达；正向和ICMP返回独立逐跳。
5. 断边：排队/未完成发送drop，已经完成发送传播中包继续到达；服务完成恰好拓扑切片时正确锁定旧传播延迟。
6. 修正packet统计与包数守恒；无路由、队列满、断边、hop limit分别记录。loss=0并不意味着端到端无丢包。
7. end半开：end新业务不执行；未完成trace标censored，不伪timeout；未完成packet另计censored。
8. 输入篡改/参数非法返回错误，未完成不写成功manifest；当前.part清理。

## 模型限制

- Fluid是单流，超额/无路由计unserved流体量，不冒充实测丢包率或多流Max-Min。
- Packet是显式UDP/IP载荷+28B开销的逐包服务模型；TCP未实现且请求明确拒绝。
- traceroute是无排队ICMP模型（60B探针、56B回复，可序列化），不受业务队列竞争、ICMP限速或随机误包；不是主机网络traceroute。
- 地址10.x.x.x是synthetic-v1，不是真实运营地址。
- mmap/零分配/全面预算审计、CUDA/TPS、Python AST前端、checkpoint等未实现；不声称通过完整需求验收。
- 优先队列和按包reader旨在先正确输出，不在此承诺超实时性能。source_snapshot普通并发检测不能抵御恶意写方。

## 测试

根tests/test_traceroute.cpp是所上传修复包的25项C++测试。根tests/regression.py会创建独立SATC/SATNET fixture并验证主程序/独立trace一致、时间隔离、队列、断链、包守恒及非法输入。可复跑：

```sh
python tests/regression.py build/bin
```

本次测试实际结果在reports/；provenance/old_reports是原包历史日志，不作为合并后验收证据。

## v4.1：对齐 SDD v2.4 / satpred v3.5（OneWebSmall 24h 基线）

- hypersim manifest 新增 `upstream_case`（format `hsn-upstream-case-1`）：当上游 SATNET 图 metadata 含 v3.5 可选注释键 `case_name`/`rf_parameters` 时原样透传，并附 `rf_model_applied=false`；缺失时省略该字段、不报错（SDD v2.4 §3.1、ZERO-T14）。注释不进入链路/路由/流量模型，不改变 effective_config 与业务输出；多包间注释不一致按 Integrity(7) 拒绝。输出文件名前缀的星座名现在取自已验证的图 metadata（目录、manifest、单个 .ngb 输入一致）。
- tests/regression.py 新增 ZERO-T14 合成夹具用例（透传、省略、业务输出字节一致）；tests/default_flow.py 改为真正的零参数运行并断言 ZERO-T15（24 包/8640 片、SOURCE_VERIFIED、缺 satnet → Io(3) 无成功清单）与 ZERO-T14（`upstream_case.case_name=="OneWebSmall"`）；satpred/tests/regression.py 新增 L2-01 注释键、L2-01b 快速路径与 F.3-3 用例。
- satpred/README.md 与 satpred/docs/IMPLEMENTATION_STATUS.md 的默认值与需求版本号更新到 24h / v3.5。
- 本版未改变仿真算法、字节协议或任何默认参数值；SDD v2.4 附录 D.4 列出的待办（完整默认值注册表、--config/HSN_* 链、Python 前端、TCP、多流 Fluid、CUDA/TPS/checkpoint）仍未实现。
