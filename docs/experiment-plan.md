# Janus QPC Prefetch 实验对比方案

## 0. 复核发现与修正

### 关键问题 1：RNIC_CACHE_MODE 是必要前提

代码复核发现：`Settings::use_rnic_cache`（对应 `RNIC_CACHE_MODE=1`）是所有 QPC 缓存建模逻辑的总开关。在 `DequeueAndTransmit()` 中，第 524 行的 `if(Settings::use_rnic_cache)` 控制了整个 QPC cache hit/miss 建模分支。

**影响**：原始 `config-dcqcn.ini` 中没有 `RNIC_CACHE_MODE` 字段（默认 false），即原始配置连 QPC 缓存延迟都不建模。

### 关键问题 2：不与 Weir 对比

本实验聚焦 Janus QPC Prefetch 方案本身的对比，不需要与 Weir 的 Rate Limiter 对比。因此所有实验统一 WEIR=0。

### 关键问题 3：INCAST 流量控制

INCAST_MIX=720 会在短时间内产生大量同目标流，造成极端的 QPC cache 压力峰值。建议分两组实验：
- **Pure-Poisson 组** (INCAST_MIX=0)：更干净，便于归因 QPC cache 策略效果
- **Mixed 组** (INCAST_MIX=720)：更接近真实场景，包含 incast 突发

### 关键问题 4：FCT 输出格式确认

9 列 Tab 分隔：`src, dst, size, start_time, qpid, msgSeq, fct_us, oracle_us, slowdown`

### 关键问题 5：QPC Cache 容量不可配置

`MAX_QPC_NUM=1024` 是硬编码宏，修改需改源代码重新编译。XRC=1 可使容量翻倍到 2048。

---
## 1. 实验目标

系统评估 Janus QPC Prefetch 方案在不同配置维度下的性能表现，具体回答以下问题：

1. **QPC 缓存建模的代价**：开启 RNIC_CACHE_MODE=1 后，QPC 缓存延迟对 FCT 的影响有多大？
2. **预取模式有效性**：Oracle/Statistical/LRU 三种预取模式各带来多少性能提升？
3. **预取 vs 不预取**：开启预取后 FCT、Cache Miss Rate、PCIe 开销的改善幅度？
4. **LRU 替换策略的独立贡献**：仅将 QPC Cache 从随机替换升级为 LRU，效果如何？
5. **流量模式敏感性**：不同工作负载（小包 KV、大流 Hadoop、混合流等）下预取效果差异？
6. **拓扑敏感性**：Leaf-Spine vs Fat-tree 两种拓扑下预取效果差异？
7. **负载敏感性**：不同网络负载（0.5/0.6/0.7/0.8/0.9）下预取效果变化？

---

## 2. 实验维度设计

### 2.1 维度 A：策略配置（6 种配置）

| 实验名 | RNIC_CACHE_MODE | WEIR | JANUS | PREFETCH_MODE | QPC_CACHE_LRU | 说明 |
|--------|----------------|------|-------|---------------|---------------|------|
| **NoCache** | 0 | 0 | 0 | - | 0 | 什么都不开启，无缓存建模 |
| **Baseline** | 1 | 0 | 0 | - | 0 | RNIC_CACHE_MODE=1，什么都不开启（随机替换，无预取） |
| **LRU-Only** | 1 | 0 | 0 | - | 1 | RNIC_CACHE_MODE=1 + LRU 替换，但无预取 |
| **Oracle** | 1 | 0 | 1 | 1 | 1 | Janus Oracle 预取（理想上界） |
| **Statistical** | 1 | 0 | 1 | 2 | 1 | Janus Statistical 预取 |
| **LRU-Prefetch** | 1 | 0 | 1 | 3 | 1 | Janus LRU 预取 |

> **核心对比逻辑**：
> - **NoCache vs Baseline**：量化 QPC 缓存延迟建模对 FCT 的代价（Baseline FCT 应比 NoCache 差，因为引入了 cache miss 延迟）
> - **Baseline vs LRU-Only**：量化 LRU 替换策略本身的独立贡献（无预取，仅替换策略差异）
> - **LRU-Only vs Oracle/Statistical/LRU-Prefetch**：量化预取在 LRU 基础上带来的额外改善
> - **Oracle vs Statistical vs LRU-Prefetch**：对比不同预取策略的效果递减
>
> **关于 WEIR=0**：所有实验统一 WEIR=0，不与 Weir 对比，聚焦于 Janus 预取方案本身的纯粹对比。
>
> **关于 LRU-Only**：这个组是必要的，因为 Janus 组同时开启了 QPC_CACHE_LRU=1 和预取，如果不单独控制 LRU 替换变量，就无法区分预取和 LRU 替换各自的贡献。

### 2.2 维度 B：流量模式（4 种工作负载）

| 流量名 | FLOW_CDF | 对应分布 | 平均消息大小 | 特征 |
|--------|----------|---------|-------------|------|
| **FB-KV** | 4 | FacebookKeyValue | 185 B | 小包密集，QPC 访问频繁但驻留短 |
| **Google-Search** | 5 | Google_SearchRPC | 440.79 KB | 中等流，典型数据中心搜索负载 |
| **Google-AllRPC** | 6 | Google_AllRPC | 2927.35 KB | 较大流，QPC 驻留时间长 |
| **Hadoop** | 7 | Facebook_HadoopDist_All | 127796.6 KB | 大流为主，QPC Miss 影响被带宽掩盖 |

> **选流理由**：从 185B 到 ~128MB，跨越 3 个数量级，覆盖小包/中流/大流/超大流场景。不同流量模式下 QPC Cache Miss 的代价占比不同——小包场景下 QPC Miss 延迟占比极高，大流场景下占比微乎其微。

### 2.3 维度 C：网络拓扑（2 种）

| 拓扑名 | 文件 | 规模 | 结构特征 |
|--------|------|------|---------|
| **Leaf-Spine** | `mix/leafspine-10ToR-4Core.topo` | 174 节点，160 主机，10 ToR + 4 Core | 非对称带宽（主机 100Gbps，Spine 400Gbps），14 交换机 |
| **Fat-Tree** | `mix/fattree-k8.topo` | 208 节点，128 主机，80 交换机 | 对称 100Gbps，3 层树结构 |

> **选拓扑理由**：Leaf-Spine 代表超大规模 PoD 互联场景，每 ToR 下主机更多（16 vs 4），QPC 缓存压力更大；Fat-Tree 代表传统多根树，路径对称但交换机数量多（80 vs 14）。

### 2.4 维度 D：网络负载（5 级）

| LOAD | 描述 |
|------|------|
| 0.5 | 低负载，排队少 |
| 0.6 | 中低负载 |
| 0.7 | 中负载 |
| 0.8 | 高负载（当前默认值） |
| 0.9 | 极高负载，排队严重 |

### 2.5 维度 E：Incast 流量（2 种）

| 配置 | INCAST_MIX | 说明 |
|------|-----------|------|
| **Pure-Poisson** | 0 | 纯 Poisson 流量，无 incast 突发，更干净 |
| **Mixed** | 720 | Poisson + Incast 混合流量，更接近真实场景 |

> **Incast 的影响**：INCAST_MIX=720 会在短时间内产生 720 条指向同一接收者的流，造成 QPC cache 压力峰值。建议优先跑 Pure-Poisson 组验证基本趋势，再跑 Mixed 组验证 incast 场景下的鲁棒性。

---

## 3. 实验矩阵

### 3.1 核心矩阵

**Pure-Poisson 组**：6 (策略) x 4 (流量) x 2 (拓扑) x 5 (负载) = **240 组实验**

**Mixed 组**：6 (策略) x 4 (流量) x 2 (拓扑) x 5 (负载) = **240 组实验**

每组实验使用 SEED=1,2,3 做误差棒，总计约 480 x 3 = **1440 次仿真**。

> 实际执行时可以分阶段，先跑 Pure-Poisson 组的核心子集。

### 3.2 分阶段实验设计

考虑到仿真时间成本，分三个阶段逐步展开：

#### 阶段 1：核心对比（11 组实验，SEED=1）

固定拓扑=Leaf-Spine，负载=0.8，INCAST_MIX=0（纯 Poisson）：

| 实验编号 | 策略 | 流量 | 拓扑 | 负载 | Incast |
|---------|------|------|------|------|--------|
| E01 | NoCache | FB-KV | Leaf-Spine | 0.8 | 0 |
| E02 | Baseline | FB-KV | Leaf-Spine | 0.8 | 0 |
| E03 | LRU-Only | FB-KV | Leaf-Spine | 0.8 | 0 |
| E04 | Oracle | FB-KV | Leaf-Spine | 0.8 | 0 |
| E05 | Statistical | FB-KV | Leaf-Spine | 0.8 | 0 |
| E06 | LRU-Prefetch | FB-KV | Leaf-Spine | 0.8 | 0 |
| E07 | NoCache | Google-Search | Leaf-Spine | 0.8 | 0 |
| E08 | Baseline | Google-Search | Leaf-Spine | 0.8 | 0 |
| E09 | Oracle | Google-Search | Leaf-Spine | 0.8 | 0 |
| E10 | Baseline | Hadoop | Leaf-Spine | 0.8 | 0 |
| E11 | Oracle | Hadoop | Leaf-Spine | 0.8 | 0 |

> 阶段 1 目的：(1) 验证 QPC 缓存建模的代价（NoCache vs Baseline）；(2) 验证 LRU 替换的独立贡献（Baseline vs LRU-Only）；(3) 验证预取效果的基本趋势——小包场景预取收益最大，大流场景收益最小。

#### 阶段 2：维度扩展（48 组实验）

在阶段 1 基础上，扩展到全部 4 种流量 x 6 种策略（24 组），再补充 Fat-Tree 拓扑下的对应实验（24 组）。

#### 阶段 3：负载扫描 + Incast 扩展

在阶段 2 确定最优策略后，对全部 5 级负载进行扫描，每个 (策略, 流量, 拓扑) 组合跑 5 级负载。然后补充 Mixed (INCAST_MIX=720) 组实验。

---

## 4. 评估指标体系

### 4.1 一级指标（直接输出）

| 指标 | 来源 | 公式/说明 |
|------|------|----------|
| **FCT 分布** | `mix/*-fct.txt` | Poisson 流和 Incast 流的 FCT，按流大小分桶统计 |
| **QPC Cache Miss Rate** | `QpcPrefetchPredictor.PrintStats()` | `m_cacheMissCount / m_totalAccessCount` |
| **Prefetch Hit Rate** | `NopWqeManager.PrintStats()` | `m_successfulPrefetchCount / m_totalNopWqeCount` |
| **Useless Prefetch Rate** | `NopWqeManager.PrintStats()` | `m_uselessPrefetchCount / m_totalNopWqeCount` |
| **QPC Cache Size** | 仿真结束输出 | 各 host 设备的 QPC 缓存占用 |

### 4.2 二级指标（计算得出）

| 指标 | 计算方式 |
|------|---------|
| **FCT 归一化改善** | `(FCT_baseline - FCT_janus) / FCT_baseline` |
| **FCT 百分位数** | avg, P50, P90, P95, P99（按流大小分桶） |
| **PCIe 带宽节省率** | 从 `GetNxtPacket` 中 `extra_pcie_bw` 累计推算 |
| **有效预取覆盖率** | `Prefetch Hit Rate / (1 - Cache Miss Rate)` — 衡量预取对 Cache Miss 的消除程度 |

---

## 5. 配置文件设计

### 5.1 命名规范

```
config-{cc}-{topo}-{traffic}-{strategy}-{load}.ini
```

示例：
- `config-dcqcn-ls-fbkv-nocache-l80.ini`
- `config-dcqcn-ls-fbkv-baseline-l80.ini`
- `config-dcqcn-ls-fbkv-oracle-l80.ini`
- `config-dcqcn-ft-gsearch-statistical-l70.ini`

其中：
- `ls` = Leaf-Spine, `ft` = Fat-Tree
- `fbkv` = FB-KV (FLOW_CDF=4), `gsearch` = Google-Search (5), `gallrpc` = Google-AllRPC (6), `hadoop` = Hadoop (7)
- `nocache/baseline/lru-only/oracle/statistical/lru-prefetch` = 策略
- `l50/l60/l70/l80/l90` = LOAD 0.5~0.9

### 5.2 关键差异参数矩阵

以下列出各组实验间需要修改的参数：

| 参数 | NoCache | Baseline | LRU-Only | Oracle | Statistical | LRU-Prefetch |
|------|---------|----------|----------|--------|-------------|--------------|
| RNIC_CACHE_MODE | 0 | 1 | 1 | 1 | 1 | 1 |
| WEIR | 0 | 0 | 0 | 0 | 0 | 0 |
| JANUS | 0 | 0 | 0 | 1 | 1 | 1 |
| PREFETCH_MODE | - | - | - | 1 | 2 | 3 |
| QPC_CACHE_LRU | 0 | 0 | 1 | 1 | 1 | 1 |
| FLOW_CDF | 4/5/6/7 | 4/5/6/7 | 4/5/6/7 | 4/5/6/7 | 4/5/6/7 | 4/5/6/7 |
| INCAST_MIX | 0/720 | 0/720 | 0/720 | 0/720 | 0/720 | 0/720 |
| LOAD | 0.5~0.9 | 0.5~0.9 | 0.5~0.9 | 0.5~0.9 | 0.5~0.9 | 0.5~0.9 |
| TOPOLOGY_FILE | .topo 文件 | .topo 文件 | .topo 文件 | .topo 文件 | .topo 文件 | .topo 文件 |
| FCT_OUTPUT_FILE | 差异化 | 差异化 | 差异化 | 差异化 | 差异化 | 差异化 |
| SIMULATOR_STOP_TIME | 20.0 | 20.0 | 20.0 | 20.0 | 20.0 | 20.0 |

### 5.3 Statistical 模式调参子实验

作为补充，可以在默认参数基础上做参数敏感性扫描：

| 参数 | 扫描范围 | 默认值 | 步长 |
|------|---------|--------|------|
| PREFETCH_HOT_THRESHOLD | 1, 3, 5, 7, 10 | 5 | - |
| PREFETCH_HISTORY_WINDOW | 1ms, 5ms, 10ms, 20ms, 50ms | 10ms | - |
| PREFETCH_INTERVAL | 0.5ms, 1ms, 2ms, 5ms | 1ms | - |
| MAX_PREFETCH_PER_CYCLE | 4, 8, 16, 32, 64 | 16 | - |

> 此部分作为可选扩展，不纳入核心实验矩阵。

---

## 6. 实验脚本设计

### 6.1 自动化实验运行脚本

需要编写一个 Python 实验运行脚本 `run-experiments.py`，功能包括：

1. **参数化配置生成**：根据 (策略, 流量, 拓扑, 负载, 种子) 自动生成 `.ini` 配置文件
2. **批量仿真执行**：调用 `./waf --run 'third mix/<config>'` 依次执行
3. **结果收集**：解析 FCT 输出文件和 Janus 统计输出
4. **结果汇总**：生成 CSV 格式的汇总表

脚本核心逻辑：

```python
# 实验参数定义
STRATEGIES = {
    'nocache':      {'RNIC_CACHE_MODE': 0, 'WEIR': 0, 'JANUS': 0, 'QPC_CACHE_LRU': 0},
    'baseline':     {'RNIC_CACHE_MODE': 1, 'WEIR': 0, 'JANUS': 0, 'QPC_CACHE_LRU': 0},
    'lru-only':     {'RNIC_CACHE_MODE': 1, 'WEIR': 0, 'JANUS': 0, 'QPC_CACHE_LRU': 1},
    'oracle':       {'RNIC_CACHE_MODE': 1, 'WEIR': 0, 'JANUS': 1, 'PREFETCH_MODE': 1, 'QPC_CACHE_LRU': 1},
    'statistical':   {'RNIC_CACHE_MODE': 1, 'WEIR': 0, 'JANUS': 1, 'PREFETCH_MODE': 2, 'QPC_CACHE_LRU': 1},
    'lru-prefetch':  {'RNIC_CACHE_MODE': 1, 'WEIR': 0, 'JANUS': 1, 'PREFETCH_MODE': 3, 'QPC_CACHE_LRU': 1},
}

TRAFFICS = {
    'fbkv':     {'FLOW_CDF': 4},
    'gsearch':  {'FLOW_CDF': 5},
    'gallrpc':  {'FLOW_CDF': 6},
    'hadoop':   {'FLOW_CDF': 7},
}

TOPOS = {
    'ls': 'mix/leafspine-10ToR-4Core.topo',
    'ft': 'mix/fattree-k8.topo',
}

LOADS = [0.5, 0.6, 0.7, 0.8, 0.9]
INCAST_MIXES = [0, 720]  # 0 = Pure-Poisson, 720 = Mixed
SEEDS = [1, 2, 3]
```

### 6.2 结果分析脚本

需要编写 `analyze-results.py`，功能包括：

1. **FCT 解析**：从 `*-fct.txt` 中提取 Poisson 流和 Incast 流的 FCT
2. **Janus 统计解析**：从仿真标准输出中提取 Cache Miss Rate / Prefetch Hit Rate / Useless Prefetch Rate
3. **FCT 百分位计算**：按流大小分桶（<10KB, 10KB-100KB, 100KB-1MB, 1MB-10MB, >10MB）计算 P50/P90/P99
4. **归一化处理**：以 Baseline 为基准计算 FCT 归一化改善
5. **图表生成**：生成论文级质量的对比图

### 6.3 图表规格

| 图表类型 | 内容 | X 轴 | Y 轴 | 分面 |
|---------|------|------|------|------|
| 柱状图 | FCT 归一化改善 | 策略 | FCT Improvement (%) | 流量模式 |
| CDF 图 | FCT 分布 | FCT | CDF | 流量模式 x 策略 |
| 折线图 | Cache Miss Rate vs 负载 | LOAD | Miss Rate | 流量模式 x 策略 |
| 折线图 | Prefetch Hit Rate vs 负载 | LOAD | Hit Rate | 流量模式 x 策略 |
| 柱状图 | FCT 分桶改善 | 流大小桶 | FCT Improvement (%) | 策略 |
| 折线图 | 拓扑对比 | 策略 | FCT Improvement (%) | 拓扑 |

---

## 7. 实验执行计划

### 7.1 时间估算

单次仿真时间估算（基于 Weir 过往经验）：
- Leaf-Spine + 50K 流 + 20s 仿真：约 10-30 分钟
- Fat-Tree + 50K 流 + 20s 仿真：约 20-60 分钟（交换机数量多）

### 7.2 执行顺序

| 阶段 | 实验数 | 估计时间 | 产出 |
|------|--------|---------|------|
| Phase 0: 编译验证 | 6 | 30min | 确保所有配置可运行 |
| Phase 1: 核心对比 | 11 | 3-5h | 初步结论：缓存代价 + 预取收益量级 |
| Phase 2: 维度扩展 | 48 | 12-20h | 完整流量/拓扑对比 |
| Phase 3: 负载扫描+Incast | 240 | 50-100h | 负载敏感性曲线 + Incast 鲁棒性 |

### 7.3 验证检查点

每个阶段完成后需要验证：

1. **NoCache vs Baseline**：Baseline 的 FCT 应差于 NoCache（因为 QPC 缓存延迟建模引入了 cache miss 开销），说明 QPC 缓存建模是必要的
2. **LRU-Only vs Baseline**：LRU-Only 的 FCT 应略优于 Baseline（LRU 替换策略本身有帮助）
3. **QPC Cache Miss Rate** 是否合理（Baseline 应 > 10%，Oracle 应接近 0%）
4. **Oracle Prefetch Hit Rate** 是否 > 90%（否则代码有 bug）
5. **大流场景** 预取改善应小于小包场景（符合直觉）

---

## 8. 预期结果与分析方向

### 8.1 预期趋势

| 场景 | Baseline Miss Rate | Oracle 改善 | Statistical 改善 | LRU-Prefetch 改善 |
|------|-------------------|-------------|-----------------|-------------------|
| FB-KV (小包) | 高 (>30%) | 显著 (>60%) | 中等 (30-50%) | 中等 (25-45%) |
| Google-Search (中流) | 中 (15-25%) | 较显著 (40-60%) | 中等 (20-40%) | 中等 (15-35%) |
| Google-AllRPC (大流) | 中低 (10-20%) | 中等 (20-40%) | 低 (10-25%) | 低 (8-20%) |
| Hadoop (超大流) | 低 (<10%) | 微弱 (<15%) | 微弱 (<10%) | 微弱 (<8%) |

### 8.2 关键分析维度

1. **QPC 缓存建模的代价**：NoCache vs Baseline，量化 cache miss 延迟对 FCT 的影响
2. **LRU 替换策略的独立贡献**：Baseline vs LRU-Only，量化仅替换策略改善的效果
3. **预取模式递减效应**：Oracle > Statistical > LRU-Prefetch，三者差距反映可预测性
4. **流量模式决定收益天花板**：小包场景下 QPC Miss 延迟占 FCT 比重大，预取收益高
5. **拓扑对预取的间接影响**：更多交换机/路径 → 更多 ECMP 路径 → QP 连接数更多 → 缓存压力更大
6. **负载与预取的交互**：高负载下排队延迟可能掩盖 QPC Miss 延迟，预取收益可能下降
7. **Incast 场景的鲁棒性**：Mixed 组下预取能否应对突发 QPC 缓存压力

---

## 9. 实施步骤

### Step 1: 编写自动化实验脚本
创建 `run-experiments.py`，实现参数化配置生成和批量仿真执行。

### Step 2: 编写结果分析脚本
创建 `analyze-results.py`，实现 FCT 解析、统计提取、图表生成。

### Step 3: Phase 0 编译验证
用 6 种策略配置分别运行一次短仿真（SIMULATOR_STOP_TIME=4s），确认无编译/运行错误。

### Step 4: Phase 1 核心对比
执行 11 组核心实验，收集初步结果，验证趋势合理性。

### Step 5: Phase 2 维度扩展
扩展到全部流量和拓扑，完成 48 组实验。

### Step 6: Phase 3 负载扫描+Incast 扩展
对全部负载级别扫描，完成 240 组实验。

### Step 7: 结果分析与图表生成
生成完整的对比图表和分析报告。

---

## 10. 涉及的关键文件

| 文件 | 角色 |
|------|------|
| `mix/config-dcqcn-prefetch.ini` | 已有的 Janus 配置模板（需修改 WEIR=0） |
| `mix/config-dcqcn.ini` | 已有的 NoCache 配置模板 |
| `mix/leafspine-10ToR-4Core.topo` | Leaf-Spine 拓扑文件 |
| `mix/fattree-k8.topo` | Fat-Tree 拓扑文件 |
| `scratch/third.cc` | 仿真入口，配置解析和统计输出 |
| `src/point-to-point/model/qpc-prefetch-predictor.h` | 预取模式枚举和预测器 |
| `src/point-to-point/model/nop-wqe-manager.h` | NOP WQE 管理器 |
| `src/point-to-point/model/settings.h` | 全局 Janus 配置参数 |
| `distribution/FacebookKeyValueMsgSizeDist.txt` | FB-KV 流量 CDF |
| `distribution/Google_SearchRPC.txt` | Google Search 流量 CDF |
| `distribution/Google_AllRPC.txt` | Google AllRPC 流量 CDF |
| `distribution/Facebook_HadoopDist_All.txt` | Hadoop 流量 CDF |
