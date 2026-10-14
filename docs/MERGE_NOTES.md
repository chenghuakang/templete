# 合并追溯

输入三压缩包的SHA及大小见provenance/restored_archives.json，TXT附带恢复脚本未执行。

唯一上游解析/hash/格式逻辑在satpred/src/core.cpp。include/hsn/reader_internal.hpp借用它；新两个可执行各使用同一源码实现，不维护独立的另一套字节协议。
主程序Simulation在src/simulation.cpp，事件队列只由调度线程提交，traceroute.hpp不直接推进主clock。

未优化事项：每包验证/映射缓存、分配峰值、线程并行、路由缓存策略、所有源码目录分层均不作为已完成验收。
