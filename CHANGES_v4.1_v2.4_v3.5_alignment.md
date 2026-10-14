# v4.1 变更清单（对齐 HyperSim SDD v2.4 与 satpred/satnet 规格 v3.5）

日期：2026-10-13。本次只做三件事，不改仿真算法、字节协议与默认参数值。

## 1. hypersim manifest 新增 upstream_case（SDD v2.4 §3.1，ZERO-T14）
- include/hsn/reader_internal.hpp：`fixedtrace::Input` 新增公开成员 `J upstream_case`，在读取第 0 个已验证图包时从 graph metadata 收集可选键 `case_name`、`rf_parameters`；其余包必须携带相同注释，否则 `need(...,7,"packages","upstream case annotation mismatch across packages")`。
- src/simulation.cpp：
  - 输出文件名的星座 slug 改为取自 `reader.upstream_case.case_name`（原先只在目录/manifest 输入时读上游 manifest，单个 .ngb 输入会退化为 `satnet`）。
  - 写 manifest 时，若注释存在则写出
    `upstream_case = {format:"hsn-upstream-case-1", case_name, rf_parameters, interpretation, rf_model_applied:false}`；
    不存在则省略该字段，不报错。注释不进入 `identity`/`simulation_run_id`、effective_config、业务输出或任何模型参数。

## 2. 文档更新到 24h / v3.5
- satpred/README.md：默认样例/区间改为 OneWebSmall 49 星、86400 s / 3600 s 包、8640 片 / 24 包；说明 case.json 注释规则、OneWeb648 降级为回归数据、L2-01b 快速路径；需求版本 v3.5。
- satpred/docs/IMPLEMENTATION_STATUS.md：需求版本 3.5；新增“已实现”条目（v3.5 默认值、注释键、测试覆盖）；缺口第 13 条补充 L0-11 构建期一致性测试与 OneWebSmall 8 星 L1-03 集合未运行。
- README.md：新增 v4.1 小节。

## 3. 测试补齐
- tests/fixtures.py：`make(..., case=None)` 可向合成图 metadata 与 manifest 写入 case_name/rf_parameters。
- tests/regression.py：新增 `ZERO-T14-upstream-case-passthrough`：
  - 带注释夹具 → manifest.upstream_case 逐值等于输入，文件名前缀 `syntheticcase-baseline-`；
  - 与无注释夹具相比，business/statistics/traceroute/result 五个业务文件字节一致，effective-config 除 `constellation` 外一致；
  - 无注释 → 无 `upstream_case` 字段；单个 .ngb 输入亦透传。
- tests/default_flow.py（重写）：真正无参数运行 `hypersim`（仅设 HSN_HOME 以免写入真实主目录）：
  - ZERO-T15：`<HSN_HOME>/runs/run_000001`、SOURCE_VERIFIED、24 个 input_products、上游 manifest 24×360=8640 片、无尾包；复制 hypersim 到无 satnet 的目录再运行 → 退出码 3 且无 `*-manifest-*.json`；
  - ZERO-T14：`upstream_case.case_name=="OneWebSmall"`、`rf_parameters` 与上游 manifest 相同、`rf_model_applied==false`、effective-config 中无 rf/frequency 参数。
- satpred/tests/regression.py：
  - L2-01：run manifest 与 summary.json 含 `case_name="OneWebSmall"`、`rf_parameters.format=="satnet-rf-annotation-1"`、`rf_model_applied==false`、每包 360 片；
  - L2-01b：`satnet --duration 600 --package 60` → 10 包 × 6 片 = 60 片、10 .ngb / 10 .sbin，netread verify 通过，注释键仍存在；
  - F.3-3：用 `examples/OneWeb648` 的 XML 运行 → manifest 不含 case_name/rf_parameters。

## 验证状态（如实）
- `g++ -std=c++20 -fsyntax-only` 通过，Python 测试文件可编译；本次打包环境中未完成完整构建与 24h 运行，`reports/` 中的报告仍是 v4 的结果。
- 运行方式：`python3 build_gcc.py`（或 CMake）后 `python3 run_tests.py build/bin`；`--skip-default-flow` 可跳过约 1.5–2 分钟的 ZERO-T15/T14 真实流程。
