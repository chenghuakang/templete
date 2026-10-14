# satpred / satnet C++17 端到端 MVP

本工程按需求文档 v3.5（OneWebSmall 24h 基线修订版；产品规则沿用 v3.4 原文）实现可运行的第一版，目标是先打通合法输入 → 轨道传播 → SATC 分包 → 网络图 → 验证 → JSON/CSV 导出。它不是完整 v3.4 验收版。请先阅读 docs/IMPLEMENTATION_STATUS.md。没有把未完成的精度测试或功能标成 PASS。

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSATPRED_CUDA=OFF -DSATPRED_TESTS=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

CMake >=3.20，C++17 编译器。OpenMP 可选。产品不需要 Python、numpy、SciPy、网络或数据下载。tests/ 中 Python 脚本仅用于独立验证，不进入产品构建或运行。

Linux 单配置构建的程序位于 build/bin；Windows 多配置构建通常位于 build/bin/Release，运行样例必须和 exe 的 examples 目录同级。CMake 已设置所有配置统一 bin 输出。

```sh
./build/bin/satdemo
./build/bin/satpred
./build/bin/satnet
./build/bin/satread
./build/bin/netread
```

默认用户目录为 $HOME/.satpred，Windows 为 %USERPROFILE%/.satpred；SATPRED_HOME 可覆盖。各次运行原子创建独立 run_NNNNNN 目录。satpred 不更新 last_success，satnet/satdemo 验证通过后才更新。

## 参数示例

```sh
./build/bin/satpred --config examples/satellites.xml --settings examples/settings.json --start 2026-01-01T00:00:00Z --duration 125 --step 10 --package 60 --frame ecef --output orbit_result
./build/bin/satnet --satellites examples/satellites.xml --nodes examples/nodes.xml --start 2026-01-01T00:00:00Z --duration 125 --step 10 --package 60 --orbit-dir orbit_result --output network_result
./build/bin/satread verify orbit_result/pred_000000.sbin
./build/bin/netread verify network_result/graphs/pred_000000.ngb --source-orbit network_result/orbits/pred_000000.sbin
./build/bin/satread csv orbit_result/pred_000000.sbin orbit.csv
./build/bin/netread csv network_result/graphs/pred_000000.ngb graph.csv
```

显式输出目录必须不存在或为空，显式 CSV 文件必须不存在。未知参数、重复参数和未实现功能明确失败。路径允许空格；shell 使用者需自己引用带空格的参数。程序自身调用子进程不经 shell。

## 输出

```
run_NNNNNN/
  orbits/pred_000000.sbin ... manifest.json effective_config.json
  graphs/pred_000000.ngb ...
  exports/pred_000000.links.json ...
  nodes.json
  effective_config.json
  run.json
  run.log
  manifest.json
  manifest.csv
  summary.json            # satdemo 产生
```

默认样例为 OneWebSmall（单轨道面 star，P=1 S=49，1200 km / 87.9°，地面 u1/gw1 各 50 Mbps）；默认区间 start 2026-01-01T00:00:00Z / duration 86400 s / step 10 s / package 3600 s，得到 8640 片、24 轨道包、24 图包（每包 360 片，无尾包）。`--dump-defaults` 与 `--help` 输出同一注册表（v3.5 F.3-1）。

当两个 XML 的原始字节 SHA256 同时等于 `examples/case.json`（satpred-case-1）登记值时，satnet 向图 metadata、manifest、links.json 以及 satdemo 的 summary.json 追加 `case_name="OneWebSmall"` 与 `rf_parameters`（format `satnet-rf-annotation-1`，`rf_model_applied=false`）。这些键只是案例注释：2.1/1.3 GHz 频率与 50 Mbps 上限不进入第 8 章链路模型、身份或字节布局；SHA 不匹配时不写任何注释键，也不报错（v3.5 §4.6 / F.3-3）。

历史 648 星样例降级为 `examples/OneWeb648/`，仅作 L1-03 / L2-03b 回归数据，不是零参数路径（v3.5 F.2-04）。快速 CI 路径 L2-01b：`satnet --duration 600 --package 60` 得到 60 片 / 10 包 / 10 图，不替代 L2-01 的 24h 正式口径。参考回归值（非门槛）：24h 默认运行在参考主机约 70–95 s。

SATC/SATNET 按字节偏移手工序列化，不直接写 C++ struct。使用小端、CRC32C、SHA256、规范 NaN、64/4096 字节对齐。图源 SHA 为完整轨道包摘要。读端验证时间、段界、CRC、边排序及双工天线占用。

## 源码入口

- apps/main.cpp：六个程序共用入口，APP_NAME 区分程序。
- src/core.cpp：唯一共享实现，编译为 satcore 静态库。内部使用 namespace sp；尚未拆成文档要求的全部子命名空间。
- include/rk8.hpp：直接取自需求附录 C 的 12 级 DOP853 主表。
- examples/：自带合法输入。
- tests/independent_format.py：独立 Python struct/CRC32C/SHA256 解码验证。
- tests/regression.py：可复跑的流程回归（含 L2-01 注释键、L2-01b 快速路径、F.3-3 非默认样例不写注释键）。
- reports/smoke_report.json：实际执行日志和未测试事项。
- docs/requirements_v3.4.txt：原需求完整副本，保留全部未来义务。

传播路径：constellation/settings → kepler 初态 → Environment → derivative → advance(DOP853) → writeorbit/finalizeorbit。
网络路径：readorbit → Spatial → connect(mesh/knn) → bandwidth/visible/antenna greedy → graphwrite → graphread → links。

## 验证范围

当前环境 GCC 编译成功，-Wall -Wextra -Werror，OpenMP 1/2 线程；31 项流程/格式 smoke 检查通过。默认 satdemo 已真实运行，非模拟日志。包含独立 struct 解码、Castagnoli CRC 和 Python hashlib 校验。

当前环境未安装 CMake，不能宣称 CMake 配置/构建已执行；使用 g++ 直接编译公共对象和六个入口验证源码，发布工程仍只提供 CMake 构建图。Windows/MSVC、CUDA、精度门槛、严格预算、全部恶意输入与规范身份验收均未完成。

可选测试：

```sh
python tests/regression.py build/bin
python tests/independent_format.py /absolute/path/to/network_run
```

两者仅为测试工具，不是产品运行依赖。


## 接入链路候选（本版本变更）

接入链路（终端/地面站 ↔ 卫星，kind=1）不再参与天线贪心分配：所有满足距离、仰角、遮挡与带宽条件的可见接入链路全部写入图，
两端都不消耗天线。终端的 `antennas` 不表示上游为其保留的卫星数；卫星天线预算只约束星间链路（kind=0）。
具体连接哪颗卫星由下游网络模拟器（hypersim `--policy`）选择。图元数据 `models.access = all-visible-candidates-selection-in-simulator`，
`models.resource = duplex-shared-antennas-isl-only`。netread 校验的“antenna overuse”只统计星间链路。
