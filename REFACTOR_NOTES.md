# 重构说明（基于缺陷修复版 97098cdb…233df9）

## 范围
保持业务语义的结构性重构。不新增 TCP、多流公平调度、CUDA 或完整需求验收能力。
基线：UnifiedSatNet_release_fixed.zip（含默认流程退出码 3 的修复），其 SHA-256 已在打包前核对。

| 位置 | 变化 |
|---|---|
| include/hsn/options.hpp + src/options.inc | 参数模型：解析、有效配置、数值读取、校验 |
| include/hsn/runtime_io.hpp + src/runtime_io.inc | 运行目录、环境变量（SATPRED_HOME 隔离）、同目录程序定位、输入快照 |
| include/hsn/events.hpp | Packet / LinkState / Event / Compare；EventKind、EventPhase 具名枚举替代 0..5 / 0..3 数字 |
| src/simulation.cpp | 保留调度核心、模型积分、探针提交与运行编排；所有 `kind==N` / `phase=N` 改为枚举 |
| src/trace_main.cpp | 仅格式展开 |
| tests/test_options.cpp（新） | 24 项检查：参数边界、来源标注、同时刻事件排序 |
| build_gcc.py / run_tests.py（新） | 无 CMake 环境的构建与全套测试入口 |
| CMakeLists.txt | 注册 options_and_event_order 测试；注释单编译单元约束 |

密集单行语句已展开为多行；展开器只重排空白，并对每个文件做词法 token 一致性断言
（字符串、字符、注释、标识符、标点完全相同），不改变任何业务 token。

## 架构约束
include/hsn/reader_internal.hpp 直接 `#include "../../satpred/src/core.cpp"`，上游尚未分离声明与实现。
因此 options.inc / runtime_io.inc 只由 simulation.cpp 包含，保持单编译单元，避免重复定义。
这是模块边界整理，不是独立链接库；后续若拆分上游 core.cpp，这两个 .inc 可直接改为 .cpp。
test_options.cpp 同样直接包含 options.inc，不能再链接 hsncore。

## 构建与测试
优先 CMake（见 README）。无 CMake：
```sh
python3 build_gcc.py --output build/bin
python3 run_tests.py build/bin --reports build/verification   # 含约 2 分钟的清洁环境默认 24h 流程
```

## 等价性证据（reports/refactor_equivalence.json）
用同一测试拓扑分别运行重构前（修复版）与重构后二进制，fluid / packet 两种模式下
business.jsonl、statistics.jsonl、traceroute.txt、traceroute.jsonl、result.json 逐字节相同；
effective_config.json 除 output 路径外相同。默认 24h 的 result.json 亦与修复版报告一致。

## 边界
Linux/GCC、root 用户下验证；CMake/CTest、Windows/MSVC 未执行。
GCC 构建保留上游 filesystem::u8path 的弃用警告（新代码以 -Wno-deprecated-declarations 编译），未伪装为零警告。
