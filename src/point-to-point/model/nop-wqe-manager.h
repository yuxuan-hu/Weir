/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*[hyx]*/
// Janus: NOP WQE Manager
#ifndef NOP_WQE_MANAGER_H
#define NOP_WQE_MANAGER_H

#include "ns3/nstime.h"
#include "ns3/simulator.h"
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <iostream>

namespace ns3 {

class QbbNetDevice;
class QpcPrefetchPredictor;

class NopWqeManager {
public:
    // NOP WQE 状态记录
    struct NopWqeRecord {
        uint32_t qpc_id;
        uint64_t issue_time;       // 发出时间 (ns)
        uint64_t complete_time;    // 预期完成时间 (ns)
        bool completed;
        NopWqeRecord() : qpc_id(0), issue_time(0), complete_time(0), completed(false) {}
    };

    NopWqeManager();
    ~NopWqeManager();

    // 发出 NOP WQE 进行预取
    void IssueNopWqe(uint32_t qpc_id);

    // NOP WQE 完成回调
    void OnNopWqeComplete(uint32_t qpc_id);

    // 检查 QPC 是否正在预取中
    bool IsPrefetching(uint32_t qpc_id);

    // 统计输出
    void PrintStats();

    // 统计数据
    uint64_t m_totalNopWqeCount;        // 总 NOP WQE 数量
    uint64_t m_successfulPrefetchCount; // 成功预取次数（QPC 被使用时仍在缓存）
    uint64_t m_uselessPrefetchCount;    // 无效预取次数
    uint64_t m_nopWqeInProgressCount;   // 当前正在进行的 NOP WQE 数量

    // 标记预取成功（当 QPC 被真正使用时调用）
    void MarkPrefetchUsed(uint32_t qpc_id);

    // 检查并记录无效预取（预取的 QPC 被替换出缓存时调用）
    void CheckAndMarkUselessPrefetch(uint32_t qpc_id);

private:
    // 正在预取中的 QPC 集合
    std::unordered_set<uint32_t> m_prefetchingQpcs;

    // NOP WQE 记录（按发出时间索引）
    std::unordered_map<uint32_t, NopWqeRecord> m_nopWqeRecords;
};

} // namespace ns3

#endif // NOP_WQE_MANAGER_H
