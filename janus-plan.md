# Janus: QPC Prefetch 技术方案设计与 Weir 仿真实现

> 项目代号：Janus  
> 仿真框架：Weir (NS-3 based RDMA simulator)  
> 文档版本：v2.0 (从代码和实现记录重建)  
> 最后更新：2026-07-01

---

## 1. 项目背景与动机

### 1.1 问题背景

RDMA 网卡中存在多种类型的缓存资源：

| 缓存类型 | 全称 | 作用 |
|----------|------|------|
| ICM/QPC | Queue Pair Context | 存储 QP 连接状态信息（目的IP、端口、PSN等） |
| WQE | Work Queue Element | 工作队列元素缓存 |
| MTT | Memory Translation Table | 地址转换表 |
| MPT | Memory Protection Table | 地址保护表 |

随着网络规模扩大和业务需求增长，单张 RDMA 网卡（如 Mellanox ConnectX-6, 100Gbps）需要维护最多 1024 个 QPC 缓存条目。当活跃 QP 数超过缓存容量时，新的 QP 访问会导致 QPC Cache Miss，引发额外的 PCIe 延迟（6us+）和带宽开销（6Gbps+），严重影响数据传输性能。

### 1.2 Weir 仿真框架的 QPC 建模现状

Weir 框架已具备 QPC 缓存的基本建模：

- **QpcCache 类**：以链表结构模拟 1024 条 QPC 缓存，满时采用**随机替换**策略
- **WqeCache 类**：模拟 WQE 缓存容量限制
- **延迟路径**（`DequeueAndTransmit`）：QPC miss → 完整 PCIe 延迟（6us）+ MTT/MPT 可能额外延迟（6us）
- **带宽路径**（`GetNxtPacket`）：QPC miss → 额外 6Gbps PCIe 带宽开销 + MTT/MPT 额外 4Gbps
- **ACK 路径**（`ReceiveAck`）：QPC miss → 设置 delay flag
- **创建路径**（`AddQueuePair`）：新建 QP 时自动 insert 到缓存

### 1.3 核心思路

**Janus** 方案通过分析历史 QPC 访问模式，预测即将需要的 QPC，在数据传输开始前的适当时机，通过下发 NOP WQE（Message Size = 0 的 WQE）将 QPC 提前加载到缓存中，从而消除或减轻 Cache Miss 带来的性能损失。

选择 NOP WQE 而非普通 message size=0 的 WQE 的原因：NOP WQE 语义更精确，不会真正传输数据，但会触发 QPC 的 PCIe 读取操作，将 QPC 加载到 NIC 缓存中。

---

## 2. 系统架构设计

### 2.1 整体架构

Janus 系统由三个核心组件构成：

```
+------------------+     +-----------------------+     +-------------------+
| QpcPrefetchPred  |     | NopWqeManager         |     | QpcCache          |
| ictor            |     |                       |     | (LRU 增强)        |
|------------------|     |-----------------------|     |-------------------|
| - RecordAccess() |---->| - IssueNopWqe()       |---->| - insert(id,      |
| - GetPrefetchTar |     | - OnNopWqeComplete()  |     |    is_prefetched) |
|   gets()         |     | - IsPrefetching()     |     | - touch(id)       |
| - UpdateCacheSta |     | - MarkPrefetchUsed()  |     | - contains(id)    |
|   tus()          |     | - CheckAndMarkUseless |     | - remove(id)      |
| - AddFutureFlow()|     |   Prefetch()          |     | - LRU 双向链表    |
+------------------+     +-----------------------+     +-------------------+
         |                        |                            |
         v                        v                            v
    访问历史记录              NOP WQE 生命周期管理         缓存容量与替换策略
```

### 2.2 组件职责

| 组件 | 文件 | 职责 |
|------|------|------|
| **QpcPrefetchPredictor** | `qpc-prefetch-predictor.h/.cc` | 记录 QPC 访问历史，预测需要预取的 QPC |
| **NopWqeManager** | `nop-wqe-manager.h/.cc` | 管理 NOP WQE 的发出、完成、成功/无效统计 |
| **QpcCache (增强)** | `qbb-net-device.h/.cc` | LRU 替换策略，支持 `touch()`/`is_prefetched` 标记 |
| **QbbNetDevice (集成)** | `qbb-net-device.h/.cc` | 预取周期调度、NOP WQE 发出与完成处理 |
| **Settings** | `settings.h/.cc` | 8 个 Janus 配置参数 |
| **RdmaHw** | `rdma-hw.cc` | ACK 路径/GetNxtPacket 路径的预取状态处理 |
| **third.cc** | `scratch/third.cc` | 配置解析、Oracle 流注册、统计输出 |

---

## 3. 预取模式设计

### 3.1 Oracle 模式 (prefetch_mode = 1)

**理想上界**：利用仿真框架的全局视角，在仿真初始化时注册所有未来流的信息（QP ID + 开始时间），预测器据此在流开始前 `prefetch_ahead_time` 的时间窗口内发出预取。

**实现方式**：
- 仿真初始化阶段，遍历 `flows_map`，对每个流调用 `AddFutureFlow(qp_id, start_time_ns)` 注册到发送端 host 的 predictor
- 内部使用 `priority_queue`（最小堆，按开始时间排序）
- 预取周期到来时，`GetUpcomingFlows()` 弹出开始时间在 `[now, now + ahead_time]` 范围内的流
- 已处理的流从优先队列永久移除（每个流只预取一次）

### 3.2 Statistical 模式 (prefetch_mode = 2)

**基于访问频率的统计预取**：维护 QPC 的历史访问计数，对满足以下条件的 QPC 进行预取：

1. **不在缓存中**（`is_in_cache == false`）
2. **访问频率超过阈值**（`access_count >= prefetch_hot_threshold`，默认 5）
3. **最近有访问**（`last_access_time >= now - prefetch_history_window`，时间局部性约束）

**访问频率**采用累计值（非滑动窗口），配合 `last_access_time` 时间约束实现时间局部性。目标列表按访问次数降序排列，优先预取高频 QPC。

### 3.3 LRU 模式 (prefetch_mode = 3)

**基于最近访问的预取**：维护一个最近访问的 QPC 双向链表（最大 2048 条），对最近访问过但不在缓存中的 QPC 进行预取。

- 每次 `RecordAccess()` 时更新 LRU 链表（移到头部）
- 超过 `LRU_LIST_SIZE` 时淘汰尾部最久未访问的 QPC
- 预取目标：链表中 `is_in_cache == false` 的 QPC，按最近访问时间排序

---

## 4. 关键问题与设计决策

### 4.1 QpcCache 替换策略改进

**问题**：原始 `QpcCache` 采用随机替换策略，对预取极不友好——预取的 QPC 可能立即被随机替换。

**决策**：引入 LRU 替换策略选项（`QPC_CACHE_LRU` 配置项）。LRU 模式下：
- `QpcCache` 使用双向链表 + 哈希表结构
- `insert()` 在缓存满时替换 LRU 尾部节点
- `touch()` 将已缓存节点移到头部
- `QpcNode` 结构增加 `is_prefetched`、`insert_time`、`last_access_time` 字段
- 兼容原有随机替换模式（`qpc_cache_lru = false` 时走原始链表逻辑）

### 4.2 NOP WQE 延迟模型

**问题**：NOP WQE 的真实开销是 PCIe 事务延迟，而非带宽占用。

**决策**：NOP WQE 开销建模为延迟模型：

| 阶段 | 延迟 | 说明 |
|------|------|------|
| WQE Doorbell 写入 | 0.5 us | CPU → NIC 的 MMIO 写 |
| QPC DMA 读取 | 1.0 us | NIC 通过 PCIe 从主机内存读取 QPC |
| MTT/MPT DMA (10% 概率) | 1.0 us | 地址转换/保护表可能缺失 |
| CQE 回收 | 0.3 us | 完成队列写入通知 |
| **总计（最佳情况）** | **0.8 us** | QPC 已在缓存，仅 Doorbell + CQE |
| **总计（标准情况）** | **1.8 us** | QPC 不在缓存，需 DMA |
| **总计（最坏情况）** | **2.8 us** | QPC 不在缓存 + MTT/MPT 缺失 |

如果 QPC 已在缓存中（不需要 DMA 读取），NOP WQE 只需 Doorbell + CQE = 0.8us。

### 4.3 QPC 访问路径的三路分支

**DequeueAndTransmit（延迟路径）**：
```
QPC hit        → delay += 0, touch + RecordAccess(true) + MarkPrefetchUsed
QPC prefetching → delay += PCIeDelay/2, RecordAccess(false)
QPC miss       → delay += PCIeDelay, + MTT/MPT, RecordAccess(false)
```

**GetNxtPacket（带宽路径）**：
```
QPC hit        → extra_pcie_bw += 0, touch
QPC prefetching → extra_pcie_bw += 2  (半带宽惩罚)
QPC miss       → extra_pcie_bw += 6 + random + MTT/MPT
```

**ReceiveAck（ACK 路径）**：
```
QPC miss → insert + evict通知 + RecordAccess(false)
QPC hit  → RecordAccess(true)  (新增 else 分支)
```

### 4.4 qIndex vs m_qpid

**问题**：`DequeueAndTransmit` 中 `qIndex` 是队列槽索引（0~fCnt-1），而 QPC 缓存应使用全局唯一的 `m_qpid`。原始 Weir 代码中 `contains()` 用 `qIndex` 但 `insert()` 用 `m_qpid`，不一致。

**决策**：在 QPC 检查之前获取 `lastQp = m_rdmaEQ->GetQp(qIndex)`，统一使用 `qpc_id = qpcQp->m_qpid` 作为缓存键。

### 4.5 Oracle 模式流注册时机

**问题**：在 `AddQueuePair()` 中注册未来流时机太晚——此时流已开始传输，无提前预取窗口。

**决策**：在 `third.cc` 的仿真初始化阶段（`InitPrefetch()` 之后），批量遍历 `flows_map` 注册所有未来流到对应 host 的 predictor。

### 4.6 GetNxtPacket 不调用 RecordAccess

**设计意图**：`GetNxtPacket` 和 `DequeueAndTransmit` 在每个包发送时都会被调用，如果在 `GetNxtPacket` 中也调用 `RecordAccess`，会导致同一个 QPC 的访问被双重计数。因此，`RecordAccess` 仅在 `DequeueAndTransmit` 和 `ReceiveAck` 中调用。

### 4.7 AddQueuePair 不调用 RecordAccess

**设计意图**：QP 创建不算"使用"事件，只是资源分配。此时 QPC 尚未被数据传输访问。

### 4.8 驱逐通知链

在所有 4 个 `QpcCache::insert()` 调用点，都需要在发生驱逐时通知预取系统：

| 调用点 | 文件 | 通知链 |
|--------|------|--------|
| `AddQueuePair` | `rdma-hw.cc:296` | `CheckAndMarkUselessPrefetch` + `UpdateCacheStatus` |
| `ReceiveAck` | `rdma-hw.cc:643` | `CheckAndMarkUselessPrefetch` + `UpdateCacheStatus` + `RecordAccess` |
| `DequeueAndTransmit` | `qbb-net-device.cc:669` | 驱逐通知在 insert 调用点 + `RecordAccess` 在 QPC 检查分支 |
| `OnNopWqeComplete` | `qbb-net-device.cc:1179` | `CheckAndMarkUselessPrefetch` + `UpdateCacheStatus` |

---

## 5. 预取生命周期

### 5.1 NOP WQE 完整生命周期

```
1. 预取周期触发 (PerformPrefetch)
   ↓
2. 预测器给出目标列表 (GetPrefetchTargets / GetUpcomingFlows)
   ↓
3. 检查: QPC 是否已在缓存? 是否已在预取中?
   ↓
4. 发出 NOP WQE (IssueNopWqeForPrefetch)
   - 计算延迟: Doorbell(0.5us) + DMA(1us) + MTT/MPT(10%: 1us) + CQE(0.3us)
   - NopWqeManager.IssueNopWqe(qpc_id): 标记为 prefetching
   - Simulator::Schedule(MicroSeconds(latency), OnNopWqeComplete)
   ↓
5. NOP WQE 完成 (OnNopWqeComplete)
   - QpcCache.insert(qpc_id, is_prefetched=true)
   - NopWqeManager.OnNopWqeComplete(): 移出 prefetching 集合
   - 如果有驱逐: CheckAndMarkUselessPrefetch + UpdateCacheStatus
   - UpdateCacheStatus(qpc_id, true)
   ↓
6a. QPC 被正常使用 (DequeueAndTransmit QPC hit)
   - NopWqeManager.MarkPrefetchUsed(qpc_id): 成功预取计数 +1
   - QpcPrefetchPredictor.RecordAccess(qpc_id, true)
   ↓
6b. 预取的 QPC 被替换 (insert 驱逐)
   - NopWqeManager.CheckAndMarkUselessPrefetch(evicted_id): 无效预取计数 +1
   - QpcPrefetchPredictor.UpdateCacheStatus(evicted_id, false)
```

### 5.2 预取状态机

```
                  IssueNopWqe
  [Not Prefetched] ──────────> [Prefetching]
       ↑                              │
       │                         OnNopWqeComplete
       │                              ↓
       │                        [Prefetched in Cache]
       │                         /              \
       │                MarkPrefetchUsed    被替换(evicted)
       │                       /                    \
       │              [Successfully Used]    [Useless Prefetch]
       │                                          \
       │                                    UpdateCacheStatus(false)
       │                                          \
       └──────────────────────────────────── [Not Prefetched]
```

---

## 6. NOP WQE 建模的精确延迟

### 6.1 延迟模型细节

NOP WQE 在仿真中通过 `Simulator::Schedule` 建模延迟，而非修改带宽参数。具体延迟计算在 `IssueNopWqeForPrefetch()` 中：

```cpp
double nop_latency = NOP_WQE_DOORBELL_LATENCY; // 0.5 us (必然发生)

if(!qpccache.contains(qpc_id)){
    nop_latency += PCIeDelay; // 1.0 us (QPC DMA 读取)
    int mtt_mpt_prob = generateMttMptProbability2();
    if(mtt_mpt_prob == 0){
        nop_latency += PCIeDelay; // 1.0 us (MTT/MPT 可能缺失, 10% 概率)
    }
}

nop_latency += NOP_WQE_CQE_LATENCY; // 0.3 us (必然发生)

// 最佳情况: 0.5 + 0.3 = 0.8 us (QPC 已在缓存)
// 标准情况: 0.5 + 1.0 + 0.3 = 1.8 us (QPC 需 DMA)
// 最坏情况: 0.5 + 1.0 + 1.0 + 0.3 = 2.8 us (QPC 需 DMA + MTT/MPT 缺失)
```

### 6.2 宏定义

```cpp
#define NOP_WQE_DOORBELL_LATENCY 0.5  // us, WQE Doorbell 写入延迟
#define NOP_WQE_CQE_LATENCY 0.3       // us, CQE 回收延迟
#define MAX_QPC_NUM 1024              // QPC 缓存最大条目数
```

---

## 7. 预期效果与评估指标

### 7.1 预期效果

- **Oracle 模式**：理论上界，消除所有可预测的 QPC Cache Miss
- **Statistical/LRU 模式**：接近 Oracle 的效果，取决于流量模式的时间/空间局部性
- **延迟改善**：预取命中后 QPC 访问延迟从 6us 降至 0us（hit）或 3us（prefetching 中）
- **带宽改善**：预取命中后 PCIe 带宽开销从 6Gbps 降至 0 或 2Gbps
- **FCT 改善**：QPC Miss 减少 → 首包延迟降低 → 整体 FCT 缩短

### 7.2 评估指标

| 指标 | 说明 | 来源 |
|------|------|------|
| Cache Miss Rate | QPC 缓存未命中率 | `QpcPrefetchPredictor.m_cacheMissCount / m_totalAccessCount` |
| Prefetch Hit Rate | 预取命中率 | `NopWqeManager.m_successfulPrefetchCount / m_totalNopWqeCount` |
| Useless Prefetch Rate | 无效预取率 | `NopWqeManager.m_uselessPrefetchCount / m_totalNopWqeCount` |
| FCT 分布 | Flow Completion Time | Weir 原有 FCT 统计 |
| 平均吞吐量 | 网络整体吞吐 | Weir 原有吞吐统计 |

### 7.3 对比实验方案

| 实验 | 配置 | 目的 |
|------|------|------|
| Baseline | `JANUS 0`, `QPC_CACHE_LRU 0` | 原始随机替换，无预取 |
| LRU Only | `JANUS 0`, `QPC_CACHE_LRU 1` | 仅 LRU 替换策略改善 |
| Oracle | `JANUS 1`, `PREFETCH_MODE 1`, `QPC_CACHE_LRU 1` | 理想上界 |
| Statistical | `JANUS 1`, `PREFETCH_MODE 2`, `QPC_CACHE_LRU 1` | 统计预取 |
| LRU Prefetch | `JANUS 1`, `PREFETCH_MODE 3`, `QPC_CACHE_LRU 1` | LRU 预取 |

---

## 8. 配置文件格式

### 8.1 Janus 配置项

在 Weir 配置文件（`.ini`）中新增以下配置项：

```ini
# Janus: QPC Prefetch Configuration
JANUS 1                          # 是否启用 Janus 预取 (0/1)
PREFETCH_MODE 2                  # 预取模式 (0=off, 1=oracle, 2=statistical, 3=lru)
PREFETCH_AHEAD_TIME 1650         # Oracle 模式提前预取时间 (ns)
PREFETCH_INTERVAL 1000000        # 预取周期间隔 (ns), 1ms
PREFETCH_HOT_THRESHOLD 5         # Statistical 模式热点阈值 (访问次数)
PREFETCH_HISTORY_WINDOW 10000000 # Statistical 模式历史窗口 (ns), 10ms
MAX_PREFETCH_PER_CYCLE 16        # 每周期最大预取数
QPC_CACHE_LRU 1                  # QPC 缓存使用 LRU 替换策略 (0/1)
```

### 8.2 配置项详解

| 配置键 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `JANUS` | bool | false | 总开关，为 false 时所有预取逻辑跳过 |
| `PREFETCH_MODE` | uint32 | 0 | 0=关闭, 1=Oracle, 2=Statistical, 3=LRU |
| `PREFETCH_AHEAD_TIME` | uint64 | 1650 | Oracle 模式提前量，需 > NOP WQE 延迟(1.8us=1800ns) |
| `PREFETCH_INTERVAL` | uint64 | 1000000 | 预取调度周期，1ms 为默认值 |
| `PREFETCH_HOT_THRESHOLD` | uint32 | 5 | Statistical 模式判定热点的访问次数阈值 |
| `PREFETCH_HISTORY_WINDOW` | uint64 | 10000000 | Statistical 模式的时间局部性窗口 |
| `MAX_PREFETCH_PER_CYCLE` | uint32 | 16 | 单周期最大预取数，防 PCIe 过载 |
| `QPC_CACHE_LRU` | bool | false | LRU 替换策略开关，建议与 Janus 一起开启 |

### 8.3 示例配置文件

参见 `mix/config-dcqcn-prefetch.ini`，基于 `config-dcqcn.ini` 扩展，在末尾添加 Janus 配置段。

---

## 9. 修改文件总览

### 9.1 新增文件（5 个）

| 文件 | 行数 | 内容 |
|------|------|------|
| `src/point-to-point/model/qpc-prefetch-predictor.h` | 105 | 预测器头文件：Oracle/Statistical/LRU 三种模式 |
| `src/point-to-point/model/qpc-prefetch-predictor.cc` | 193 | 预测器实现：RecordAccess、GetPrefetchTargets、AddFutureFlow 等 |
| `src/point-to-point/model/nop-wqe-manager.h` | 68 | NOP WQE 管理器头文件 |
| `src/point-to-point/model/nop-wqe-manager.cc` | 83 | NOP WQE 管理器实现 |
| `mix/config-dcqcn-prefetch.ini` | 109 | 预取方案配置文件 |

### 9.2 修改文件（6 个）

| 文件 | 修改内容 |
|------|---------|
| `src/point-to-point/model/settings.h` | 新增 8 个 Janus 静态成员变量声明 |
| `src/point-to-point/model/settings.cc` | 新增 8 个 Janus 默认值初始化 |
| `src/point-to-point/model/qbb-net-device.h` | QpcCache 升级为 LRU 双链表+哈希表；新增 QpcNode 结构；新增 NOP WQE 宏；新增 Predictor/NopWqeManager 指针和 5 个预取方法声明；前向声明 |
| `src/point-to-point/model/qbb-net-device.cc` | QpcCache LRU 实现（contains/insert/touch/remove）；DequeueAndTransmit 三路分支+qIndex→qpc_id 修正；5 个预取方法实现（InitPrefetch/PerformPrefetch/ScheduleNextPrefetchCycle/IssueNopWqeForPrefetch/OnNopWqeComplete） |
| `src/point-to-point/model/rdma-hw.cc` | GetNxtPacket 三路分支（touch/半带宽/全带宽）；ReceiveAck 增加 RecordAccess（cache miss + cache hit 分支）；AddQueuePair 驱逐通知链 |
| `scratch/third.cc` | 8 个配置项解析；Janus 初始化；Oracle 模式流注册；统计输出 |

### 9.3 修改文件（编译配置 1 个）

| 文件 | 修改内容 |
|------|---------|
| `src/point-to-point/wscript` | 源文件列表添加 `qpc-prefetch-predictor.cc` 和 `nop-wqe-manager.cc`；头文件列表添加对应 `.h` |

---

## 10. 实现步骤（原始 9 步计划）

### Step 1: 基础设施搭建 — Settings 变量

在 `settings.h` 中声明 8 个 Janus 静态成员变量，在 `settings.cc` 中初始化默认值。

### Step 2: QpcCache 升级为 LRU

将 `QpcCache` 从单链表升级为支持 LRU 的双向链表+哈希表结构：
- 新增 `QpcNode` 结构（含 `is_prefetched`、`insert_time`、`last_access_time`）
- `contains()`: LRU 模式查哈希表，非 LRU 模式遍历链表
- `insert()`: LRU 模式淘汰尾部节点，非 LRU 保留原始随机替换
- `touch()`: LRU 模式移到头部并清除 `is_prefetched` 标记
- `remove()`: LRU 模式从链表和哈希表中移除

### Step 3: 创建 QpcPrefetchPredictor 类

新建 `qpc-prefetch-predictor.h/.cc`，实现三种预取模式的预测逻辑。

### Step 4: 创建 NopWqeManager 类

新建 `nop-wqe-manager.h/.cc`，实现 NOP WQE 生命周期管理。

### Step 5: 集成预取到 QbbNetDevice

在 `QbbNetDevice` 中：
- 添加 `m_prefetchPredictor` 和 `m_nopWqeManager` 指针
- 添加 `InitPrefetch()`/`PerformPrefetch()`/`ScheduleNextPrefetchCycle()`/`IssueNopWqeForPrefetch()`/`OnNopWqeComplete()` 方法
- 在 `DequeueAndTransmit` 中集成 QPC 三路分支和 `qIndex→qpc_id` 修正

### Step 6: 修改 RdmaHw — GetNxtPacket 和 ReceiveAck

- `GetNxtPacket`：添加 QPC 三路分支（hit/prefetching/miss）
- `ReceiveAck`：添加 `RecordAccess` 调用（cache miss 和 cache hit）
- `AddQueuePair`：添加驱逐通知链

### Step 7: 修改 third.cc — 配置解析和初始化

- 添加 8 个配置项的解析逻辑
- 仿真初始化阶段调用 `InitPrefetch()`
- Oracle 模式下注册未来流
- 仿真结束时输出预取统计

### Step 8: 修改 wscript — 编译配置

在 `src/point-to-point/wscript` 的源文件和头文件列表中添加新文件。

### Step 9: 创建配置文件

创建 `mix/config-dcqcn-prefetch.ini`，基于 `config-dcqcn.ini` 扩展 Janus 配置段。

---

## 11. 预取场景覆盖验证

| 场景 | 覆盖状态 |
|------|----------|
| QPC hit (DequeueAndTransmit) → delay=0, touch, RecordAccess(true), MarkPrefetchUsed | 完成 |
| QPC prefetching (DequeueAndTransmit) → delay=PCIeDelay/2, RecordAccess(false) | 完成 |
| QPC miss (DequeueAndTransmit) → full delay+MTT/MPT, RecordAccess(false) | 完成 |
| QPC hit (GetNxtPacket) → extra_pcie_bw=0, touch | 完成 |
| QPC prefetching (GetNxtPacket) → extra_pcie_bw=2 | 完成 |
| QPC miss (GetNxtPacket) → extra_pcie_bw=6+random+MTT/MPT | 完成 |
| ACK QPC miss (ReceiveAck) → insert + evict notify + RecordAccess(false) | 完成 |
| ACK QPC hit (ReceiveAck) → RecordAccess(true) | 完成 |
| AddQueuePair → insert + evict notify | 完成 |
| NOP WQE complete → insert(true) + OnNopWqeComplete + evict notify + UpdateCacheStatus | 完成 |
| Oracle mode flow pre-registration | 完成 |
| Statistical mode prefetch targeting | 完成 |
| LRU mode prefetch targeting | 完成 |

---

## 12. 已知 Bug 修复记录

### Issue C: qIndex vs m_qpid（已修复）

**文件**：`src/point-to-point/model/qbb-net-device.cc`  
**问题**：`DequeueAndTransmit` 中 QPC 缓存 `contains()` 使用 `qIndex`（队列索引），而 `insert()` 使用 `m_qpid`（全局 QP ID），两者不一致  
**修复**：在 QPC 检查前获取 `Ptr<RdmaQueuePair> qpcQp = m_rdmaEQ->GetQp(qIndex)`，统一使用 `qpc_id = qpcQp->m_qpid`

### Issue G: Oracle 模式 AddFutureFlow 缺失（已修复）

**文件**：`scratch/third.cc`  
**问题**：Oracle 模式的 `m_futureFlows` 优先队列始终为空，`AddFutureFlow()` 从未被调用  
**修复**：在 `InitPrefetch()` 之后批量遍历 `flows_map`，对每个流调用 `AddFutureFlow(qp_id, start_ns)` 注册到发送端 host 的 predictor

### Issue H: ReceiveAck 缺少 RecordAccess（已修复）

**文件**：`src/point-to-point/model/rdma-hw.cc`  
**问题**：ACK 路径 `ReceiveAck()` 有驱逐通知但无 `RecordAccess()` 调用，导致预测器缺少 ACK 路径的访问记录  
**修复**：在 cache miss 分支添加 `RecordAccess(qp->m_qpid, false)`，新增 else 分支添加 `RecordAccess(qp->m_qpid, true)`

---

## 13. 方案限制与假设

1. **仅发送端预取**：第一阶段仅实现发送端 QPC 预取，接收端预取作为未来工作（接收端难以预测将收到哪个 QP 的数据）
2. **仿真环境 Oracle**：Oracle 模式利用仿真框架的全局信息，实际硬件无法实现
3. **NOP WQE 不影响拥塞**：NOP WQE 的延迟建模为独立事件，不占用实际数据传输带宽
4. **QP ID 唯一性**：假设 `m_qpid` 在全网范围内唯一标识 QP
5. **LRU 容量**：Predictor 的 LRU 列表最大容量 2048，为 QPC 缓存大小的 2 倍
6. **预取与 Rate Limiter 交互**：预取消除不必要的延迟噪声，让 Rate Limiter 更准确感知真实 RTT，为正向效果

---

## 14. 编译与运行

### 14.1 编译

```bash
./waf -d optimized configure
./waf build
```

### 14.2 运行

```bash
# Baseline（无预取）
./waf --run 'third mix/config-dcqcn.ini'

# Janus 预取
./waf --run 'third mix/config-dcqcn-prefetch.ini'
```

---

## 15. 所有 Janus 代码修改标注方式

所有 Janus 相关的代码修改均以 `/*[hyx]*/` 注释标记，便于在代码中定位和审查。
