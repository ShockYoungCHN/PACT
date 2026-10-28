# Btree 实验收束记录（2026-10-28）

状态：按用户决定暂时收束；不继续启动 Btree 的调权、布局搜索或采样诊断。
数据保留。收束时未发现正在运行的 Btree/PACT 实验服务。

## 当前结论

当前结果尚未证明静态 PC weight 相比 PACT 具有明确的冻结布局性能优势。
在最后的分池 single-thread 对照中，PACT main 换成固定样本增量后，在线和冻结
吞吐均与原 PAC 很接近。这个结果支持“本例中动态 PAC 系数未展示额外收益”，
不证明其观测准确、不证明其全局最优，也不证明静态信息一般无效。

最初 PC 的部分差距来自已验证的后端粒度/页面选择问题。修正后差距大幅缩小。
继续在本 Btree 上手工微调 weight 的收益证据不足。

## 最终主要对照：分池、1-thread、900+180 秒

两种 PACT 及校准 PC 使用完全相同的335,544,320-key分池 Btree 二进制。
每种策略一次独立运行：0–900s开启 tiering，900s停止 runtime 并关闭内核迁移，
915–1080s用于冻结吞吐，排除关停瞬态；没有重复实验置信区间。

| 方法 | 在线0–900s QPS | 冻结915–1080s QPS | 冻结相对原 PACT |
|---|---:|---:|---:|
| 原 PACT main/PAC | 260,825 | 295,234 | 基线 |
| PACT-freq（固定262143/样本） | 260,051 | 295,093 | −0.048% |
| 校准 PC、4KiB、零分不提升 | 248,451 | 293,701 | −0.519% |

PC 在线比原 PACT 慢4.74%；PACT-freq 在线比原 PACT 慢0.297%。
这些小差异不是独立复测得到的确定收益/损失。

分池使初始匿名内存从原布局约26,632MiB降为约23,595MiB，实际快层比例约11.3%。
配置中0.10是沿用的目标标签，不能把这次结果报告成精确10%容量实验。

### 冻结快层区域分配（1070s，MiB）

| 区域 | 原 PACT | PACT-freq | 校准 PC |
|---|---:|---:|---:|
| header | 1329.52 | 1328.73 | 1245.63 |
| internal keys | 502.35 | 502.37 | 472.25 |
| leaf keys | 57.42 | 53.71 | 0.00 |
| pointer arrays | 643.32 | 642.44 | 710.86 |
| record | 61.70 | 58.24 | 21.84 |
| payload | 75.95 | 73.41 | 214.25 |
| 总快层 | 2670.25 | 2658.91 | 2664.83 |

区域分配接近不表示具体页集合相同；容量也不是严格相等的同进程回放。

PC 记录1,599,088次提升尝试、1,282,209次成功、316,879次失败，失败为ENOMEM。
原 PACT 记录3,369,495次提升成功、0次提升失败；PACT-freq为3,364,967次成功、0失败。
PACT降级由kernel LRU执行，其失败不包含在runtime提升失败口径中。
这些统计不能单独解释全部在线性能差异。

## 已验证的机制与实现

### 原布局的页面混放

对完整树的对象标注统计，6,759,897张含对象的4KiB页中，53.23%混放多种对象。
混放集中在internal keys、leaf keys与pointer arrays；header、record、payload
三组不与它们跨组混放。45.16%的对象页同时包含三种keys/指针对象；另外8.08%
包含leaf keys和pointer arrays。分母是含标注对象的页，不是全部RSS。

这个事实限制独立放置不同访问角色的能力，但不能据此断言PC权重不可能有效：
不同页的访问组成比例仍可能不同。

### 静态权重校准到底做了什么

校准版直接修改C分配器，分别控制六类对象的内存池。固定查询序列，测“全部慢层”
与“仅某一整类放快层”的时间差，除以估计的该类基线DRAM访问数，再以header归一化。
约8M keys两个种子取平均；约16M keys另一个种子作稳定性检查。

该实验测得对象类别平均收益，不是每个PC的独立因果收益。
原程序的PC类别通过源码行号和反汇编规则人工映射；同类的多个PC直接共享类别系数，
不按PC贡献再拆分；没有自动解决一条PC跨多类对象的情形。

最终分池PC沿用1-thread系数：header1、internal keys0.5648063、leaf keys0.296671865、
pointer arrays1.635367772、record0.758527473。payload与未覆盖访问保留原类别权重。
新二进制的PC地址已重新映射，没有复用旧偏移；没有为分池正式大树重新校准系数。

### PACT、频率与多线程

当前PACT约每20ms聚合一次。每窗口、每内存层的样本共用一个分值：

    sample_increment = clamp_uint18(k * misses / (MLP * samples))
    page.PAC += sample_increment  # 每处理一条该页样本更新一次

这不是每页平均分相同数额，而是按样本次数比例分摊。
`pac_update`是逐样本更新日志，不是20ms一次全量页面快照。
当前正式构建没有启用逐事件PAC日志；原始PEBS未包含完整PAC和实际窗口编号。

单线程20ms内也经历大量完整查询，混合header/短扫/指针等访问。
“聚合MLP稳定”不能推出“窗口内每次访问暴露成本相同”。
多线程可能进一步混合，但我们尚未完成证明其观测受干扰的因果实验。

硬件MLP来自CHA TOR occupancy与active cycles之比，再汇总。
CHA是地址分片相关的共享uncore单元，不是core私有队列。
源码启用了TID_EN和硬件Thread/Core ID过滤；这不是Linux TID。
过滤映射的准确性、请求覆盖以及聚合损失尚未独立实测验证。
不能只凭CHA共享或线程数增加就宣称PACT必然计入所有core流量。

原PACT此次3,369,568次PAC更新与3,369,495次提升成功非常接近，提示它可能接近
“采到慢层页就提升”。但没有记录逐次旧分数、增量和门槛，尚不能确证这一机制。

PACT-freq用编译宏固定每条样本加262143，保留原采样/PMU读取/分桶/冷却/提升/kernel LRU。
这个常数是缩放单位，仍会影响门槛行为；不能把实验称为与分值尺度无关。
它也不是我们userspace rerank控制器里的freq模式。

### 原布局的改进和布局搜索

早期PC为2MiB区域原始分数求和，在稀疏header区域与密集payload间存在占用成本差异。
同进程等容量交换把部分payload换成keys/指针与header，旧PC基线相对提高约9%。
这是特定交换/窗口的结果，不是普适类别价值排序。

4KiB后端修正了页面身份键、批量驻留刷新、扫描续跑与迁移粒度等问题。
后来又发现启动时会用零分页填满预算，已增加target=fast且score<=0时不入队的过滤；
降级不受该过滤影响。该修复与分池同时进入最后PC运行，未独立量化其性能收益。

原布局5-thread有限搜索测试4个起点与7个实际交换候选；同进程固定2627.51MiB快层，
独立复测候选比PACT平均高0.471%，未达到预设>0.5%的规则。
这只能说该轮未发现明确改善，不能说PACT全局最优。
搜索使用未分池布局，结论不能直接推广到新的pool版本；pool版本未做同类搜索。

## 热度统计的口径

对原PACT与PACT-freq的慢层PEBS，按全部pool驻留页作分母，top20%页分别覆盖
57.63%和57.69%的观测样本；top10%约39.7%。存在集中，未观察到严格20%/80%。

页面提升后不再产生慢层样本，因此这是被放置策略影响的观测分布，不是全部访问频率。
不能据此确认或否认真实访问的二八规律；未采到也不等于实际没有访问。

## 数据、源码和分支入口

分支：`btree/pooled-singlethread-20261028`。

- 最后pool版PC/PAC：`results/userspace_all/btree_pooled_t1_pc_pact_900plus180_20261028/REPORT.md`
- 最后PACT-freq：`results/userspace_all/btree_pooled_pact_freq_t1_900plus180_20261028/REPORT.md`
- 原布局有限搜索：`results/userspace_all/btree_layout_search_5hot_20261027/REPORT.md`
- 早期冻结因果诊断：`results/userspace_all/btree_frozen_attribution_20261027/CONCLUSIONS_ZH.md`
- 热度统计：`results/userspace_all/btree_pact_vs_freq_heat_20261028/REPORT.md`
- 完整对象混放：`/users/samuraiy/btree_object_page_mixing_20261028.json`
- PC采样页重叠：`/users/samuraiy/btree_pc_overlap_20261027/overlap.json`
- 分池源码/生成/构建/运行：`experiments/btree_pooled_20261028/README.md`
- PACT-freq宏、独立构建、测试：`experiments/pact_main_freq_20261028/README.md`
- 校准实现：`experiments/btree_pc_groups_20261027/`
- 当前4KiB后端：`experiments/btree_ratio_grid/runtime/rerank.c`

原始PEBS继续保留在`/users/samuraiy/pebs`，各结果manifest/summary有精确路径。
中断的900+900pool启动目录标记为STOPPED_BY_USER，不用于完成运行的性能比较。
本轮收束不删除、覆盖或恢复任何历史数据。

## 保留的未解问题（暂不继续执行）

1. 固定布局下的多线程观测差异：先验证硬件过滤，再记录实际窗口及PAC增量，
   不能把1-thread自动当作正确归因的gold standard。
2. PACT是否在本例几乎每次采样就提升，以及分数缩放/门槛是否使MLP变化失去决策影响。
3. 在相同控制器和采样口径中，校准PC weight是否比等权真正改善页面排名。
4. pool布局是否还有可重复的等容量放置收益，以及PC提升ENOMEM的直接触发条件。

这些问题没有通过当前数据解决；不把它们写成已证明的静态信息优势。
