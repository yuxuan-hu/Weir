/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*[hyx]*/
// Janus: QPC Prefetch Predictor
#ifndef QPC_PREFETCH_PREDICTOR_H
#define QPC_PREFETCH_PREDICTOR_H

#include "ns3/nstime.h"
#include "ns3/simulator.h"
#include "ns3/settings.h"
#include <unordered_map>
#include <vector>
#include <set>
#include <queue>
#include <algorithm>
#include <iostream>
#include <functional>

namespace ns3 {

// 前向声明
class QbbNetDevice;

class QpcPrefetchPredictor {
public:
    // 预取模式枚举
    enum PrefetchMode {
        PREFETCH_OFF = 0,
        PREFETCH_ORACLE = 1,
        PREFETCH_STATISTICAL = 2,
        PREFETCH_LRU = 3
    };

    // QPC 访问记录
    struct QpcAccessRecord {
        uint32_t qpc_id;
        uint64_t access_count;         // 总访问次数
        uint64_t last_access_time;     // 最后访问时间 (ns)
        uint64_t last_prefetch_time;   // 最后预取时间 (ns)
        bool is_in_cache;              // 是否在缓存中
        bool is_prefetched;            // 是否已预取（且未过期）
        QpcAccessRecord() : qpc_id(0), access_count(0), last_access_time(0),
            last_prefetch_time(0), is_in_cache(false), is_prefetched(false) {}
    };

    // Oracle 模式专用：未来流量信息
    struct FutureFlowInfo {
        uint32_t qpc_id;
        uint64_t start_time;  // 流开始时间 (ns)
        bool operator>(const FutureFlowInfo& other) const {
            return start_time > other.start_time; // 最小堆（按时间排序）
        }
    };

    QpcPrefetchPredictor();
    ~QpcPrefetchPredictor();

    // 记录 QPC 访问事件
    void RecordAccess(uint32_t qpc_id, bool is_in_cache);

    // 获取当前需要预取的 QPC 列表
    std::vector<uint32_t> GetPrefetchTargets(QbbNetDevice* dev);

    // Oracle 模式：注册未来流
    void AddFutureFlow(uint32_t qpc_id, uint64_t start_time);

    // 更新预测
    void UpdatePredictions();

    // 判断某个 QPC 是否应该预取
    bool ShouldPrefetch(uint32_t qpc_id);

    // 更新 QPC 的缓存状态
    void UpdateCacheStatus(uint32_t qpc_id, bool in_cache);

    // 标记 QPC 已发起预取（防止重复预取）
    void MarkPrefetching(uint32_t qpc_id);

    // Oracle 模式：获取即将到来的流（供 PerformPrefetch 使用）
    std::vector<uint32_t> GetUpcomingFlows(uint64_t now, uint64_t ahead_time, QbbNetDevice* dev);

    // 统计输出
    void PrintStats();

    // 统计数据
    uint64_t m_totalAccessCount;       // 总访问次数
    uint64_t m_cacheMissCount;         // 缓存未命中次数
    uint64_t m_prefetchIssuedCount;    // 发出预取次数
    // 注意：预取成功/失败计数在 NopWqeManager 中维护（m_successfulPrefetchCount / m_uselessPrefetchCount）

private:
    // 访问历史记录
    std::unordered_map<uint32_t, QpcAccessRecord> m_accessHistory;

    // LRU 模式：最近访问的 QPC 列表（按访问时间排序）
    std::list<uint32_t> m_lruList;
    std::unordered_map<uint32_t, std::list<uint32_t>::iterator> m_lruMap;
    static const uint32_t LRU_LIST_SIZE = 2048; // LRU 列表最大容量

    // Oracle 模式：未来流量优先队列（最小堆，按开始时间排序）
    std::priority_queue<FutureFlowInfo, std::vector<FutureFlowInfo>, 
                        std::greater<FutureFlowInfo>> m_futureFlows;
};

} // namespace ns3

#endif // QPC_PREFETCH_PREDICTOR_H
