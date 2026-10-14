# 实现范围与缺口（必须阅读）

状态：0.1.0-mvp。需求版本：3.5（OneWebSmall 24h 基线修订版；v3.5 相对 v3.4 只修订 §0/§3.2/§3.3/§4.5/§4.6/§10.1/§13 与附录 E.3，其余产品规则沿用 v3.4 原文）。本交付优先实际端到端流程，但不能处理文档允许的所有合法输入，也不能宣称严格 v3.5 合规。

## 已实现并有流程证据

- C++17 共享静态库和 satpred/satnet/satread/netread/satdemo 入口；自家子进程参数数组编排。
- 自实现 XML/JSON、SHA256、CRC32C、UTC 和十进制秒 nearest-even 换算。
- Walker star/delta（无逐星覆写）和 explicit 层，基础 radio/phys 继承；固定地面节点。
- 永久 missing/damaged、真实时间半开事件、包内短事件保留及状态恢复；MISSING 遮蔽不停止名义传播。
- 解析初始化，.npy v1/v2 的基础 <f8/C-order/(N,6) 导入。
- 使用需求 12 级固定步 DOP853 主表；J2/J3/J4、阻力、第三体、SRP、相对论路径；精度尚未独立验证。
- 工程近似岁差/GMST/ECEF 位置速度变换，环境节点线性插值，节点按全局时间编号生成。
- SATC SAMPLES 及 SATNET 1.1 基础布局、CRC/保留字节/规范 NaN、读端校验。
- mesh/knn、空间网格接入检索、WGS84 法线、椭球遮挡、精确 u64 衰减整数化、共享双工天线。
- 外部轨道目录快照复用和源 SHA 绑定，跨包时间/几何/运行身份检查。
- links.json 按片写出、CSV、清单、原子 .part 发布、成功标记、无参数样例。
- v3.5 基线：defaults() 中 satpred/satnet 的 duration/package 默认值为 86400/3600（start 2026-01-01T00:00:00Z、step 10 不变），零参数运行得到 8640 片 / 24 包 / 24 图；`examples/{satellites,nodes,settings,case}.json|xml` 为 OneWebSmall，`examples/OneWeb648/` 为回归数据集（F.3-1/F.3-2）。
- v3.5 案例注释：两个 XML SHA256 同时匹配 case.json 时，graph metadata / manifest / links.json / summary.json 写入 `case_name` 与 `rf_parameters`（satnet-rf-annotation-1，rf_model_applied=false）；不匹配时不写、不报错；注释不参与链路模型、run_id 或字节布局（§4.6、F.3-3、F.3-4）。summary.json 的 numeric_accuracy_certified 仍为 false。
- 测试：tests/regression.py 覆盖 L2-01 注释键断言、L2-01b 快速路径（--duration 600 --package 60 → 60 片/10 包/10 图）与 F.3-3 非默认样例（OneWeb648）不含注释键。

## 未实现或不完整：不属于精度可以延期的事项

以下也是后续实现任务，不能因为全流程 demo 成功就忽略：

1. 外部 eop_csv、ephemeris_csv、density_csv、phys_json：收到请求返回 Arg。gravity_json 支持有限覆盖，但外部文件快照/全身份记录未完整实现。
2. Walker 层中 satellite 逐星覆写：当前 Config 拒绝；需要使用 explicit 层，不会静默丢弃覆写。
3. 文档第11章 canonical JSON 与 run_id：当前明确采用 MVP 身份，而非标准身份；manifest 写警告。geometry/config 原始摘要分离已实现。
4. 内存：当前按包缓冲轨道/图，并保守预检部分缓冲和候选。batch 仅解析，不改变规划。不承诺覆盖所有自有分配峰值；并非文档严格有界内存实现。大包可以 Config 拒绝，即使正式版本应能够流式接受。
5. Result<T> 错误通道、全部指定命名空间/源码分层：当前共享库内部使用 Err 异常，由 application 统一转换。XML 错误没有完整行列定位；部分 CLI 数值错误归类仍为 Config。
6. 独立 orbit_engine 附录D诊断协议：该可执行入口明确返回 Arg，没有伪造成功流。satpred 的库内 DOP853 实际传播已实现。
7. 全默认值注册表、schema、build_info 嵌入、phys_sources/完整外部数据记录、完整 manifest 字段、simulation_defaults.json：尚不完整。effective_config 主要存 CLI 文本，而非全部规范类型。
8. CUDA G、全 F 功能：未实现。cuda 返回 Device，auto 选择 CPU；未来义务见完整需求副本。
9. reader 并非完整恶意输入安全实现：规范保留字段/UTF8 控制字符/所有未知段边界/未知minor与legacy标签、极端有符号时刻溢出、JSON模型完整语义仍需加强。不得对不可信文件宣称已安全审计。
10. 网络复用路径尚未全量核对源清单所有摘要/状态基线；普通 size/mtime 快照检测已实现，不能证明抗恶意并发修改。
11. 单星 failure 的记录只有 gid/time/reason，未完整写 stage/global_index/substep；窗口重叠诊断协议和 batch调度未实现。
12. SIGINT 仅有安全点与当前 .part 清理路径；子进程取消竞态、Windows CTRL_C/长路径等未专项验证。
13. 新图 metadata 必需键基础覆盖已写出，但 effective_policy 当前含路径/调度等多余信息；run_id 排除规则与 typed canonical 仍需对齐。v3.5 L0-11（产品内案例常量与 examples/case.json 逐值一致的构建期测试）尚未加入构建；OneWebSmall 8 星 L1-03 积分精度集合未运行。
14. 未实现网络拓扑插件 Result 注册表；mesh/knn 为内建分支。未完成释放构建/迁移/平台审计。

## 精度暂缓

当前采用文档 DOP853 表而不是每片 Kepler 替代传播，输出是真实数值积分结果，不是占位数组。但太阳/月球模型、力函数、坐标及长期积分还没有 SciPy/ERFA 独立误差报告，不能认证满足 0.02m/0.05m 等阈值或真实轨道厘米级精度。numeric_accuracy_certified=false。

## 建议后续顺序

1. 先补严格流式写读/分配预留、全部合法 XML 输入、外部数据冻结。
2. 补规范 typed canonical identity、完整 manifest/schema 和附录D诊断协议。
3. 补规范 CLI/Error/Result/行列号、reader fuzz/溢出测试、Windows实际构建部署。
4. 最后做独立逐力/坐标/初值/DOP853精度回归；不修改阈值掩盖失败。

31 项 smoke PASS 与 tests/regression.py 的 L2-01/L2-01b 断言仅表示这些具体样本通过，不是第12/13章全验收 PASS。CMake 不在当前环境中，配置构建为 BLOCKED；Windows 为 NOT_TESTED，CUDA未实现，精度为 NOT_TESTED。
