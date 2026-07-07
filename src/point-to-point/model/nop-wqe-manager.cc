/*[hyx]*/
// Janus: NOP WQE Manager Implementation
#include "ns3/nop-wqe-manager.h"
#include "ns3/qpc-prefetch-predictor.h"
#include "ns3/qbb-net-device.h"
#include "ns3/settings.h"
#include "ns3/simulator.h"

namespace ns3 {

NopWqeManager::NopWqeManager()
    : m_totalNopWqeCount(0),
      m_successfulPrefetchCount(0),
      m_uselessPrefetchCount(0),
      m_nopWqeInProgressCount(0) {
}

NopWqeManager::~NopWqeManager() {
}

void NopWqeManager::IssueNopWqe(uint32_t qpc_id) {
    if(m_prefetchingQpcs.find(qpc_id) != m_prefetchingQpcs.end()){
        return; // 已经在预取中，不重复发出
    }

    m_totalNopWqeCount++;
    m_nopWqeInProgressCount++;

    NopWqeRecord record;
    record.qpc_id = qpc_id;
    record.issue_time = Simulator::Now().GetNanoSeconds();
    record.completed = false;
    m_nopWqeRecords[qpc_id] = record;
    m_prefetchingQpcs.insert(qpc_id);
}

void NopWqeManager::OnNopWqeComplete(uint32_t qpc_id) {
    auto it = m_nopWqeRecords.find(qpc_id);
    if(it != m_nopWqeRecords.end()){
        it->second.completed = true;
        it->second.complete_time = Simulator::Now().GetNanoSeconds();
    }
    m_prefetchingQpcs.erase(qpc_id);
    if(m_nopWqeInProgressCount > 0){
        m_nopWqeInProgressCount--;
    }
}

bool NopWqeManager::IsPrefetching(uint32_t qpc_id) {
    return m_prefetchingQpcs.find(qpc_id) != m_prefetchingQpcs.end();
}

void NopWqeManager::MarkPrefetchUsed(uint32_t qpc_id) {
    auto it = m_nopWqeRecords.find(qpc_id);
    if(it != m_nopWqeRecords.end() && it->second.completed){
        m_successfulPrefetchCount++;
        m_nopWqeRecords.erase(it);
    }
}

/*[hyx]*/
// Janus: 检查并记录无效预取（预取的 QPC 被替换出缓存）
// 无论 NOP WQE 是否已完成，QPC 被驱逐都意味着预取无用（若未完成则更无意义）
void NopWqeManager::CheckAndMarkUselessPrefetch(uint32_t qpc_id) {
    auto it = m_nopWqeRecords.find(qpc_id);
    if(it != m_nopWqeRecords.end()){
        if(it->second.completed){
            m_uselessPrefetchCount++;
        }
        // 无论 completed 与否，QPC 已被驱逐，清理记录避免僵尸条目
        m_nopWqeRecords.erase(it);
    }
}

void NopWqeManager::PrintStats() {
    std::cout << "[NopWqeManager] Stats at " << Simulator::Now().GetNanoSeconds() << " ns:" << std::endl;
    std::cout << "  Total NOP WQE: " << m_totalNopWqeCount << std::endl;
    std::cout << "  Successful Prefetch: " << m_successfulPrefetchCount << std::endl;
    std::cout << "  Useless Prefetch: " << m_uselessPrefetchCount << std::endl;
    std::cout << "  NOP WQE In Progress: " << m_nopWqeInProgressCount << std::endl;
    // 命中率分母使用已完成的预取数（成功 + 无效），不含进行中的
    uint32_t completed_count = m_successfulPrefetchCount + m_uselessPrefetchCount;
    if(completed_count > 0){
        std::cout << "  Prefetch Hit Rate: " << (double)m_successfulPrefetchCount / completed_count * 100 << "%" << std::endl;
    }
}

} // namespace ns3
