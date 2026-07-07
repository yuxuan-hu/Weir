/*[hyx]*/
// Janus: QPC Prefetch Predictor Implementation
#include "ns3/qpc-prefetch-predictor.h"
#include "ns3/qbb-net-device.h"
#include "ns3/simulator.h"
#include <cmath>

namespace ns3 {

QpcPrefetchPredictor::QpcPrefetchPredictor()
    : m_totalAccessCount(0),
      m_cacheMissCount(0),
      m_prefetchIssuedCount(0) {
}

QpcPrefetchPredictor::~QpcPrefetchPredictor() {
}

void QpcPrefetchPredictor::RecordAccess(uint32_t qpc_id, bool is_in_cache) {
    m_totalAccessCount++;
    
    auto& record = m_accessHistory[qpc_id];
    record.qpc_id = qpc_id;
    record.access_count++;
    record.last_access_time = Simulator::Now().GetNanoSeconds();
    record.is_in_cache = is_in_cache;

    if(!is_in_cache){
        m_cacheMissCount++;
    }

    // LRU 模式：更新 LRU 列表
    if(Settings::prefetch_mode == PREFETCH_LRU){
        auto it = m_lruMap.find(qpc_id);
        if(it != m_lruMap.end()){
            m_lruList.erase(it->second);
        }
        m_lruList.push_front(qpc_id);
        m_lruMap[qpc_id] = m_lruList.begin();
        // 限制列表大小
        while(m_lruList.size() > LRU_LIST_SIZE){
            uint32_t old = m_lruList.back();
            m_lruList.pop_back();
            m_lruMap.erase(old);
        }
    }
}

std::vector<uint32_t> QpcPrefetchPredictor::GetPrefetchTargets(QbbNetDevice* dev) {
    std::vector<uint32_t> targets;
    
    if(Settings::prefetch_mode == PREFETCH_ORACLE){
        // Oracle 模式：由 GetUpcomingFlows 驱动，不需要额外计算
        return targets;
    }

    if(Settings::prefetch_mode == PREFETCH_STATISTICAL){
        uint64_t now = Simulator::Now().GetNanoSeconds();
        uint64_t window_start = now - Settings::prefetch_history_window;

        for(auto& [qpc_id, record] : m_accessHistory){
            // 条件1：不在缓存中
            if(record.is_in_cache) continue;
            // 条件2：在历史窗口内有足够多的访问
            if(record.access_count >= Settings::prefetch_hot_threshold){
                // 条件3：最近有访问（时间局部性）
                if(record.last_access_time >= window_start){
                    targets.push_back(qpc_id);
                }
            }
        }
        // 按访问次数降序排列（优先预取高频访问的 QPC）
        std::sort(targets.begin(), targets.end(), [this](uint32_t a, uint32_t b){
            return m_accessHistory[a].access_count > m_accessHistory[b].access_count;
        });
    }

    if(Settings::prefetch_mode == PREFETCH_LRU){
        // LRU 模式：对最近访问过但不在缓存中的 QPC 进行预取
        for(uint32_t qpc_id : m_lruList){
            auto it = m_accessHistory.find(qpc_id);
            if(it != m_accessHistory.end() && !it->second.is_in_cache){
                targets.push_back(qpc_id);
            }
        }
    }

    return targets;
}

void QpcPrefetchPredictor::AddFutureFlow(uint32_t qpc_id, uint64_t start_time) {
    FutureFlowInfo info;
    info.qpc_id = qpc_id;
    info.start_time = start_time;
    m_futureFlows.push(info);
}

std::vector<uint32_t> QpcPrefetchPredictor::GetUpcomingFlows(uint64_t now, uint64_t ahead_time, QbbNetDevice* dev) {
    std::vector<uint32_t> targets;
    uint64_t prefetch_time = now + ahead_time;

    // 注意：不能直接修改 priority_queue，需要逐个弹出再保存
    std::vector<FutureFlowInfo> saved;
    while(!m_futureFlows.empty()){
        FutureFlowInfo top = m_futureFlows.top();
        if(top.start_time <= prefetch_time){
            // 这个流即将开始，预取其 QPC
            if(dev && !dev->qpccache.contains(top.qpc_id)){
                targets.push_back(top.qpc_id);
            }
            saved.push_back(top); // 已经弹出的，Oracle 只需处理一次
        }else{
            saved.push_back(top); // 还没到预取时间，保留
            // 把这个和之后的所有都保留
            while(!m_futureFlows.empty()){
                saved.push_back(m_futureFlows.top());
                m_futureFlows.pop();
            }
            break;
        }
        m_futureFlows.pop();
    }
    // 重新构建优先队列（只保留还没到时间的流）
    for(auto& info : saved){
        if(info.start_time > prefetch_time){
            m_futureFlows.push(info);
        }
    }
    return targets;
}

void QpcPrefetchPredictor::UpdatePredictions() {
    // 统计模式：清理过期的访问记录
    uint64_t now = Simulator::Now().GetNanoSeconds();
    uint64_t window_start = now - Settings::prefetch_history_window * 2; // 双倍窗口后清理

    for(auto it = m_accessHistory.begin(); it != m_accessHistory.end(); ){
        if(it->second.last_access_time < window_start && it->second.access_count < 2){
            it = m_accessHistory.erase(it);
        }else{
            ++it;
        }
    }
}

bool QpcPrefetchPredictor::ShouldPrefetch(uint32_t qpc_id) {
    auto it = m_accessHistory.find(qpc_id);
    if(it == m_accessHistory.end()) return false;
    
    // 如果 QPC 已在缓存中，不需要预取
    if(it->second.is_in_cache) return false;
    // 如果已经在预取中，不需要重复预取
    if(it->second.is_prefetched) return false;

    if(Settings::prefetch_mode == PREFETCH_STATISTICAL){
        return it->second.access_count >= Settings::prefetch_hot_threshold;
    }
    if(Settings::prefetch_mode == PREFETCH_LRU){
        return m_lruMap.find(qpc_id) != m_lruMap.end();
    }
    return false;
}

void QpcPrefetchPredictor::UpdateCacheStatus(uint32_t qpc_id, bool in_cache) {
    auto it = m_accessHistory.find(qpc_id);
    if(it != m_accessHistory.end()){
        it->second.is_in_cache = in_cache;
        // QPC 进入缓存或被驱逐时，都重置预取标记
        // 进入缓存：预取已完成，标记过期
        // 被驱逐：允许重新预取
        it->second.is_prefetched = false;
    }
}

void QpcPrefetchPredictor::MarkPrefetching(uint32_t qpc_id) {
    auto& record = m_accessHistory[qpc_id];
    record.qpc_id = qpc_id;
    record.is_prefetched = true;
}

void QpcPrefetchPredictor::PrintStats() {
    std::cout << "[QpcPrefetchPredictor] Stats at " << Simulator::Now().GetNanoSeconds() << " ns:" << std::endl;
    std::cout << "  Total Access: " << m_totalAccessCount << std::endl;
    std::cout << "  Cache Miss: " << m_cacheMissCount << std::endl;
    std::cout << "  Prefetch Issued: " << m_prefetchIssuedCount << std::endl;
    // 预取成功/失败计数在 NopWqeManager::PrintStats 中输出
    if(m_totalAccessCount > 0){
        std::cout << "  Cache Miss Rate: " << (double)m_cacheMissCount / m_totalAccessCount * 100 << "%" << std::endl;
    }
    if(m_prefetchIssuedCount > 0){
        std::cout << "  Prefetch Coverage: " << (double)m_prefetchIssuedCount / m_totalAccessCount * 100 << "%" << std::endl;
    }
    std::cout << "  Access History Size: " << m_accessHistory.size() << std::endl;
}

} // namespace ns3
