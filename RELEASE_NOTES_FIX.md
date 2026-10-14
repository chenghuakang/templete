# 缺陷修复发布说明（2026-10-13）

## 缺陷
默认流程 `hypersim`（无输入参数）在轨道和网络产物全部生成并校验通过后，仍以退出码 3 结束，
stderr 末行为 `child failed: .../satnet exit 3`。

根因：`satnet` 在产物完成后调用 `updatesuccess()`，向用户状态目录
（`SATPRED_HOME`，默认 `HOME/.satpred`，Windows 为 `USERPROFILE/.satpred`）写入 `last_success.json`。
该目录不可用（环境变量缺失、路径不可创建、不可写）时，这个辅助索引文件的写入失败被当作致命错误，
已完整的运行被判定失败。原始工程即存在此缺陷，与任何重构无关；在以下环境可稳定复现：
- 未设置 `SATPRED_HOME` 且未设置 `HOME`；
- `SATPRED_HOME` 指向无法创建的路径（例如普通文件下的子路径）。

## 修改
1. `satpred/src/core.cpp` `updatesuccess()`：改为尽力而为；写入失败输出一行 JSON 警告
   `{"warning":"last_success_not_recorded",...}` 到 stderr，不改变退出码。运行取消（where=run）仍照常抛出。
2. `satpred/src/core.cpp` `child()`：子进程失败消息补充信号编号／exec 失败提示，并指明子程序原始错误在前一行。
3. `src/simulation.cpp`：默认流程调用 `satnet` 前，将 `SATPRED_HOME` 设置为运行目录内的 `upstream_state/`，
   消除对宿主机 home 目录的隐式依赖；所有写入均落在 `--output` 目录内。
4. 测试：`satpred/tests/regression.py` 新增 3 个状态文件回归用例（无 home、坏路径、正常路径）；
   新增 `tests/default_flow.py`（清空 HOME/SATPRED_HOME 的端到端默认流程）；CMake 注册
   `default_flow_clean_env` 与 `upstream_regression` 两个 CTest 用例。

## 行为变化
- `satread`/`netread` 无参数时仍从 `SATPRED_HOME/last_success.json` 选择输入；若上一次运行未能写入该文件，
  这两个工具会按原有逻辑报告找不到默认输入。
- 默认流程生成的 `last_success.json` 现在位于 `<run>/upstream_state/`，不再写入 `~/.satpred`。
  显式调用 `satnet` 的行为不变。

## 本次验证（Linux, GCC, root）
见 `reports/`。未执行：CMake/CTest、Windows/MSVC、非 root 用户下的权限型失败。
在 root 环境下 `chmod 500` 不能构造“不可写目录”，权限型场景需在普通用户账户复测。

## 未包含
前一轮对话中的结构性重构（options/runtime_io/events 拆分）工作文件已丢失，未包含在本包中。
本包 = 原始发布包 + 本缺陷修复 + 回归测试。
