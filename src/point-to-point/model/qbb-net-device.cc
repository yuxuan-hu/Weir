/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
* Copyright (c) 2006 Georgia Tech Research Corporation, INRIA
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation;
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
* Author: Yuliang Li <yuliangli@g.harvard.com>
*/

#define __STDC_LIMIT_MACROS 1
#include <stdint.h>
#include <stdio.h>
#include "ns3/qbb-net-device.h"
#include "ns3/log.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/data-rate.h"
#include "ns3/object-vector.h"
#include "ns3/pause-header.h"
#include "ns3/drop-tail-queue.h"
#include "ns3/assert.h"
#include "ns3/ipv4.h"
#include "ns3/ipv4-header.h"
#include "ns3/simulator.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/qbb-channel.h"
#include "ns3/random-variable.h"
#include "ns3/flow-id-tag.h"
#include "ns3/qbb-header.h"
#include "ns3/error-model.h"
#include "ns3/cn-header.h"
#include "ns3/ppp-header.h"
#include "ns3/udp-header.h"
#include "ns3/seq-ts-header.h"
#include "ns3/pointer.h"
#include "ns3/custom-header.h"
#include "ns3/settings.h"
/*[hyx]*/
// Janus: QPC Prefetch
#include "ns3/qpc-prefetch-predictor.h"
#include "ns3/nop-wqe-manager.h"

#include <iostream>

NS_LOG_COMPONENT_DEFINE("QbbNetDevice");

namespace ns3 {

	/*[hyx]*/
	// MTT MPT建模
	int generateMttMptProbability(){
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dis(1, 100);
		int randomNum = dis(gen);
		// 返回值为0表示MTT/MPT缓存不在网卡缓存中，需要重新获取，返回1表示在网卡缓存中，不需要重新获取
		return (randomNum <= 2 * MTT_MPT_PROBABILITY) ? 0 : 1;
	};
	// for wqe cache miss
	int generateMttMptProbability1(){
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dis(1, 100);
		int randomNum = dis(gen);
		// 返回值为0表示MTT/MPT缓存不在网卡缓存中，需要重新获取，返回1表示在网卡缓存中，不需要重新获取
		return (randomNum <= 2 * MTT_MPT_PROBABILITY) ? 0 : 1;
	};
	// for qpc cache miss
	int generateMttMptProbability2(){
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dis(1, 100);
		int randomNum = dis(gen);
		// 返回值为0表示MTT/MPT缓存不在网卡缓存中，需要重新获取，返回1表示在网卡缓存中，不需要重新获取
		return (randomNum <= 1 * MTT_MPT_PROBABILITY) ? 0 : 1;
	};

	// WQE建模
	void WqeCache::increment(){
		wqe_num++;
	}
	void WqeCache::decrement(){
		if(wqe_num > 0){
			wqe_num--;
		}
	}
	bool WqeCache::contains(){
		return (wqe_num >= MAX_WQE_NUM)? false : true;
	}

	// QPC建模
	/*[hyx]*/
	// Janus: LRU 模式支持
	bool QpcCache::contains(uint32_t value){
		if(Settings::qpc_cache_lru){
			return lru_map.find(value) != lru_map.end();
		}
		QpcInfo* cur = head;
		while(cur != nullptr){
			if(cur->qpc_id == value){
				return true;
			}
			cur = cur->next;
		}
		return false;
	}

	/*[hyx]*/
	// LRU 辅助：将节点移到链表头部 (Janus)
	static void LruMoveToHead(QpcNode*& lru_head, QpcNode*& lru_tail, QpcNode* node){
		if(node == lru_head) return; // 已经在头部
		// 从当前位置摘除
		if(node->prev) node->prev->next = node->next;
		if(node->next) node->next->prev = node->prev;
		if(node == lru_tail) lru_tail = node->prev;
		// 插入头部
		node->prev = nullptr;
		node->next = lru_head;
		if(lru_head) lru_head->prev = node;
		lru_head = node;
		if(!lru_tail) lru_tail = node;
	}

	uint32_t QpcCache::insert(uint32_t value, bool is_prefetched){
		uint32_t evicted_id = 0xFFFFFFFF; // 0xFFFFFFFF 表示无替换
		if(Settings::qpc_cache_lru){
			// LRU 模式
			// 更新容量（XRC 模式翻倍）
			if(Settings::use_xrc){
				lru_capacity = MAX_QPC_NUM * 2;
			}else{
				lru_capacity = MAX_QPC_NUM;
			}
			// 如果已存在，移到头部并更新访问时间
			auto it = lru_map.find(value);
			if(it != lru_map.end()){
				it->second->last_access_time = Simulator::Now().GetNanoSeconds();
				LruMoveToHead(lru_head, lru_tail, it->second);
				return evicted_id;
			}
			// 创建新节点
			QpcNode* newNode = new QpcNode(value);
			newNode->is_prefetched = is_prefetched;
			newNode->insert_time = Simulator::Now().GetNanoSeconds();
			newNode->last_access_time = Simulator::Now().GetNanoSeconds();
			// 检查是否满
			if(lru_num >= lru_capacity){
				// 替换 LRU 尾部节点
				QpcNode* victim = lru_tail;
				if(victim){
					evicted_id = victim->qpc_id;
					lru_map.erase(victim->qpc_id);
					if(victim->prev) victim->prev->next = nullptr;
					lru_tail = victim->prev;
					if(lru_tail == nullptr) lru_head = nullptr;
					delete victim;
					lru_num--;
				}
			}
			// 插入头部
			newNode->next = lru_head;
			if(lru_head) lru_head->prev = newNode;
			lru_head = newNode;
			if(!lru_tail) lru_tail = newNode;
			lru_map[value] = newNode;
			lru_num++;
		}else{
			// 原始随机替换模式
			if(QpcCache::contains(value)){
				return evicted_id;
			}
			if(!Settings::use_xrc){
				if(qpc_num < MAX_QPC_NUM){
					qpc_num++;
					QpcInfo* newQpc = new QpcInfo(value);
					if(head == nullptr){
						head = newQpc;
						tail = newQpc;
					}else{
						tail->next = newQpc;
						tail = newQpc;
					}
				}else{
					std::random_device rd;
					std::mt19937 gen(rd());
					std::uniform_int_distribution<> dis(0, MAX_QPC_NUM-1);
					uint32_t index = dis(gen);
					QpcInfo* cur = head;
					for(uint32_t i=0; i<index; i++){
						cur = cur->next;
					}
					evicted_id = cur->qpc_id;
					cur->qpc_id = value;
				}
			}else{
				if(qpc_num < MAX_QPC_NUM * 2){
					qpc_num++;
					QpcInfo* newQpc = new QpcInfo(value);
					if(head == nullptr){
						head = newQpc;
						tail = newQpc;
					}else{
						tail->next = newQpc;
						tail = newQpc;
					}
				}else{
					std::random_device rd;
					std::mt19937 gen(rd());
					std::uniform_int_distribution<> dis(0, MAX_QPC_NUM*2-1);
					uint32_t index = dis(gen);
					QpcInfo* cur = head;
					for(uint32_t i=0; i<index; i++){
						cur = cur->next;
					}
					evicted_id = cur->qpc_id;
					cur->qpc_id = value;
				}
			}
		}
		return evicted_id;
	}

	/*[hyx]*/
	// Janus: LRU touch - 标记最近使用
	void QpcCache::touch(uint32_t value){
		if(!Settings::qpc_cache_lru) return;
		auto it = lru_map.find(value);
		if(it != lru_map.end()){
			it->second->last_access_time = Simulator::Now().GetNanoSeconds();
			it->second->is_prefetched = false;
			LruMoveToHead(lru_head, lru_tail, it->second);
		}
	}

	/*[hyx]*/
	// Janus: 从缓存中移除 QPC
	void QpcCache::remove(uint32_t value){
		if(!Settings::qpc_cache_lru) return;
		auto it = lru_map.find(value);
		if(it != lru_map.end()){
			QpcNode* node = it->second;
			lru_map.erase(it);
			if(node->prev) node->prev->next = node->next;
			if(node->next) node->next->prev = node->prev;
			if(node == lru_head) lru_head = node->next;
			if(node == lru_tail) lru_tail = node->prev;
			delete node;
			lru_num--;
		}
	}

	/*[hyx]*/
	// Janus: 当前缓存中的 QPC 数量
	uint32_t QpcCache::size(){
		if(Settings::qpc_cache_lru) return lru_num;
		return qpc_num;
	}

	/*[hyx]*/
	// Janus: 打印 QPC 缓存内容
	void QpcCache::print(){
		if(Settings::qpc_cache_lru){
			QpcNode* cur = lru_head;
			while(cur != nullptr){
				std::cout<<"Qpc id: "<<cur->qpc_id
					<<" prefetched: "<<cur->is_prefetched
					<<" last_access: "<<cur->last_access_time<<std::endl;
				cur = cur->next;
			}
		}else{
			QpcInfo* cur = head;
			while(cur != nullptr){
				std::cout<<"Qpc id: "<<cur->qpc_id<<std::endl;
				cur = cur->next;
			}
		}
		std::cout<<std::endl;
	}
	
	uint32_t RdmaEgressQueue::ack_q_idx = 3;
	// RdmaEgressQueue
	TypeId RdmaEgressQueue::GetTypeId (void)
	{
		static TypeId tid = TypeId ("ns3::RdmaEgressQueue")
			.SetParent<Object> ()
			.AddTraceSource ("RdmaEnqueue", "Enqueue a packet in the RdmaEgressQueue.",
					MakeTraceSourceAccessor (&RdmaEgressQueue::m_traceRdmaEnqueue))
			.AddTraceSource ("RdmaDequeue", "Dequeue a packet in the RdmaEgressQueue.",
					MakeTraceSourceAccessor (&RdmaEgressQueue::m_traceRdmaDequeue))
			;
		return tid;
	}

	RdmaEgressQueue::RdmaEgressQueue(){
		// for weir
		total_active_qp_num = 0;

		m_rrlast = 0;
		m_qlast = 0;
		m_mtu = 1000;
		m_ackQ = CreateObject<DropTailQueue>();
		m_ackQ->SetAttribute("MaxBytes", UintegerValue(0xffffffff)); // queue limit is on a higher level, not here
	}

	Ptr<Packet> RdmaEgressQueue::DequeueQindex(int qIndex){
		if (qIndex == -1){ // high prio
			Ptr<Packet> p = m_ackQ->Dequeue();
			m_qlast = -1;
			m_traceRdmaDequeue(p, 0);
			return p;
		}
		if (qIndex >= 0){ // qp
			Ptr<Packet> p = m_rdmaGetNxtPkt(m_qpGrp->Get(qIndex));
			m_rrlast = qIndex;
			m_qlast = qIndex;
			m_traceRdmaDequeue(p, m_qpGrp->Get(qIndex)->m_pg);
			return p;
		}
		return 0;
	}
	int RdmaEgressQueue::GetNextQindex(bool paused[], bool pfc_queue){
		bool found = false;
		uint32_t qIndex;
		if (!paused[ack_q_idx] && m_ackQ->GetNPackets() > 0)
			return -1;

		// no pkt in highest priority queue, do rr for each qp
		uint32_t fcount = m_qpGrp->GetN();
		/*[Active QP Monitor]*/
		// init to 0
		if(Settings::use_weir){
			total_active_qp_num = 0;
		}

		for (qIndex = 1; qIndex <= fcount; qIndex++){
			Ptr<RdmaQueuePair> qp = m_qpGrp->Get((qIndex + m_rrlast) % fcount);
			// for weir
			if(Settings::use_weir){
				if(qp->GetBytesLeft() > 0){
					total_active_qp_num++;
				}
			}

			uint32_t pfc_fine = qp->m_pg;
			if (!pfc_queue) pfc_fine = qp->m_dst;
			if (!paused[pfc_fine] && qp->GetBytesLeft() > 0 && !qp->IsWinBound()){
				if (m_qpGrp->Get((qIndex + m_rrlast) % fcount)->m_nextAvail.GetTimeStep() > Simulator::Now().GetTimeStep()) //not available now
					continue;

				return (qIndex + m_rrlast) % fcount;
			}
		}
		return -1024;
	}

	int RdmaEgressQueue::GetLastQueue(){
		return m_qlast;
	}

	uint32_t RdmaEgressQueue::GetNBytes(uint32_t qIndex){
		NS_ASSERT_MSG(qIndex < m_qpGrp->GetN(), "RdmaEgressQueue::GetNBytes: qIndex >= m_qpGrp->GetN()");
		return m_qpGrp->Get(qIndex)->GetBytesLeft();
	}

	uint32_t RdmaEgressQueue::GetFlowCount(void){
		return m_qpGrp->GetN();
	}

	Ptr<RdmaQueuePair> RdmaEgressQueue::GetQp(uint32_t i){
		return m_qpGrp->Get(i);
	}
 
	void RdmaEgressQueue::RecoverQueue(uint32_t i){
		NS_ASSERT_MSG(i < m_qpGrp->GetN(), "RdmaEgressQueue::RecoverQueue: qIndex >= m_qpGrp->GetN()");
		m_qpGrp->Get(i)->snd_nxt = m_qpGrp->Get(i)->snd_una;
	}

	void RdmaEgressQueue::EnqueueHighPrioQ(Ptr<Packet> p){
		m_traceRdmaEnqueue(p, 0);
		m_ackQ->Enqueue(p);
	}

	void RdmaEgressQueue::CleanHighPrio(TracedCallback<Ptr<const Packet>, uint32_t> dropCb){
		while (m_ackQ->GetNPackets() > 0){
			Ptr<Packet> p = m_ackQ->Dequeue();
			dropCb(p, 0);
		}
	}

	/******************
	 * QbbNetDevice
	 *****************/
	NS_OBJECT_ENSURE_REGISTERED(QbbNetDevice);

	TypeId
		QbbNetDevice::GetTypeId(void)
	{
		static TypeId tid = TypeId("ns3::QbbNetDevice")
			.SetParent<PointToPointNetDevice>()
			.AddConstructor<QbbNetDevice>()
			.AddAttribute("QbbEnabled",
				"Enable the generation of PAUSE packet.",
				BooleanValue(true),
				MakeBooleanAccessor(&QbbNetDevice::m_qbbEnabled),
				MakeBooleanChecker())
			.AddAttribute("QcnEnabled",
				"Enable the generation of PAUSE packet.",
				BooleanValue(false),
				MakeBooleanAccessor(&QbbNetDevice::m_qcnEnabled),
				MakeBooleanChecker())
			.AddAttribute("DynamicThreshold",
				"Enable dynamic threshold.",
				BooleanValue(false),
				MakeBooleanAccessor(&QbbNetDevice::m_dynamicth),
				MakeBooleanChecker())
			.AddAttribute("PauseTime",
				"Number of microseconds to pause upon congestion",
				UintegerValue(5),
				MakeUintegerAccessor(&QbbNetDevice::m_pausetime),
				MakeUintegerChecker<uint32_t>())
			.AddAttribute ("TxBeQueue", 
					"A queue to use as the transmit queue in the device.",
					PointerValue (),
					MakePointerAccessor (&QbbNetDevice::m_queue),
					MakePointerChecker<Queue> ())
			.AddAttribute ("RdmaEgressQueue", 
					"A queue to use as the transmit queue in the device.",
					PointerValue (),
					MakePointerAccessor (&QbbNetDevice::m_rdmaEQ),
					MakePointerChecker<Object> ())
			.AddTraceSource ("QbbEnqueue", "Enqueue a packet in the QbbNetDevice.",
					MakeTraceSourceAccessor (&QbbNetDevice::m_traceEnqueue))
			.AddTraceSource ("QbbDequeue", "Dequeue a packet in the QbbNetDevice.",
					MakeTraceSourceAccessor (&QbbNetDevice::m_traceDequeue))
			.AddTraceSource ("QbbDrop", "Drop a packet in the QbbNetDevice.",
					MakeTraceSourceAccessor (&QbbNetDevice::m_traceDrop))
			.AddTraceSource ("RdmaQpDequeue", "A qp dequeue a packet.",
					MakeTraceSourceAccessor (&QbbNetDevice::m_traceQpDequeue))
			.AddTraceSource ("QbbPfc", "get a PFC packet. 0: resume, 1: pause",
					MakeTraceSourceAccessor (&QbbNetDevice::m_tracePfc))
			;

		return tid;
	}

	QbbNetDevice::QbbNetDevice()
	{
		NS_LOG_FUNCTION(this);
		m_ecn_source = new std::vector<ECNAccount>;
		for (uint32_t i = 0; i < Settings::NODESCALE; i++){
			m_paused[i] = false;
		}
		m_pfc_on_queue = true;

		/*[hyx]*/
		// for DCT
		if(Settings::use_dct){
			// init 初始值只会影响第一次的数据传输，对后续并没有影响，因此可以设置为 0
			last_qIndex = 0;
		}

		/*[hyx]*/
		// Janus: QPC 预取初始化
		m_prefetchPredictor = nullptr;
		m_nopWqeManager = nullptr;

		m_rdmaEQ = CreateObject<RdmaEgressQueue>();
	}

	QbbNetDevice::~QbbNetDevice()
	{
		NS_LOG_FUNCTION(this);
		/*[hyx]*/
		// Janus: 清理预取系统
		if(m_prefetchPredictor) delete m_prefetchPredictor;
		if(m_nopWqeManager) delete m_nopWqeManager;
	}

	void
		QbbNetDevice::DoDispose()
	{
		NS_LOG_FUNCTION(this);

		PointToPointNetDevice::DoDispose();
	}

	void
		QbbNetDevice::TransmitComplete(void)
	{
		NS_LOG_FUNCTION(this);
		NS_ASSERT_MSG(m_txMachineState == BUSY, "Must be BUSY if transmitting");
		m_txMachineState = READY;
		NS_ASSERT_MSG(m_currentPkt != 0, "QbbNetDevice::TransmitComplete(): m_currentPkt zero");
		m_phyTxEndTrace(m_currentPkt);
		m_currentPkt = 0;
		DequeueAndTransmit();
	}

	void
		QbbNetDevice::DequeueAndTransmit(void)
	{
		NS_LOG_FUNCTION(this);
		if (!m_linkUp) return; // if link is down, return
		if (m_txMachineState == BUSY) return;	// Quit if channel busy
		Ptr<Packet> p;
		if (m_node->GetNodeType() == 0){
			int qIndex = m_rdmaEQ->GetNextQindex(m_paused, m_pfc_on_queue);
			if (qIndex != -1024){

				/*[hyx]*/
				double delay = 0;
				if(Settings::use_rnic_cache){
					// for Weir
					/*[Rate Limiter]*/
					if(Settings::use_weir){
						// std::cout<<"[Weir] qIndex: "<<qIndex<<std::endl;
						// 这里 qIndex 有可能为 -1 ，因此需要加上高优先级队列的处理，否则会出现错误，类似于溢出那种的错误 SIGSEGV ，超出了 qIndex 的可选范围      
						/*
						Command ['/mnt/c/Users/DELL/Desktop/infocom2025/simulation/Floodgate-NS3/build/scratch/third', 
						'mix/config-dcqcn.ini'] terminated with signal SIGSEGV. 
						Run it under a debugger to get more information (./waf --run <program> --command-template="gdb --args %s <args>").
						*/
						if(qIndex == -1){
							p = m_rdmaEQ->DequeueQindex(qIndex);
							m_traceDequeue(p, 0);
							// rnic cache mode
							TransmitStartWithDelay(p, delay);
							return;  // 直接返回，不考虑后续的时延
						}
						Ptr<RdmaQueuePair> lastQp = m_rdmaEQ->GetQp(qIndex);
						if(lastQp->cur_rtt > lastQp->pre_rtt){	// Current RTT > Previous RTT => increase sending gap
							uint64_t ActiveQpNum = m_rdmaEQ->total_active_qp_num;
							lastQp->cur_gap = (ActiveQpNum >> 9) * INC + lastQp->pre_gap;
							lastQp->cur_gap = (lastQp->cur_gap > (lastQp->base_gap + 12 * INC)) ? (lastQp->base_gap + 12 * INC) : lastQp->cur_gap;
							lastQp->pre_gap = lastQp->cur_gap;
						}else{	// Current RTT <= Previous RTT => decrease sending gap
							uint64_t ActiveQpNum = m_rdmaEQ->total_active_qp_num;
							lastQp->cur_gap = (lastQp->pre_gap > ((ActiveQpNum >> 9) * DEC)) ? (lastQp->pre_gap - ((ActiveQpNum >> 9) * DEC)) : 0;
							lastQp->pre_gap = lastQp->cur_gap;
						}

						// update wqe posting delay 
						delay += lastQp->cur_gap / 1e9;  // ns -> s
					}

					// for DCT
					// DCT 中假定为在传输的时候进行延迟，这是由于QP切换造成的，但是，对于接收数据的时候我们默认为收到ACK的时候QP不会切换，这是由于数据传输完成才会切换QP，因此只有传输数据时切换QP会造成时延
					if(Settings::use_dct){
						if(qIndex == last_qIndex){
							// QP 没有发生切换
							delay += 0;
						}else{
							// QP 发生切换
							delay += 2 * PCIeDelay / 1e6;  // us -> s
							// 额外的 PCIe 时延
						}
						// 完成 QP 的切换
						last_qIndex = qIndex;
						// update qp cache
					}

					// mtt mpt
					int mtt_mpt_probability = generateMttMptProbability();
					if(mtt_mpt_probability == 0){
						delay += PCIeDelay / 1e6;  // us -> s
					}else{
						delay += 0;	 // no delay
					}
					// wqe
				if(!Settings::use_weir){  // for weir
				// 没有使用 weir 才需要考虑 wqe cache ，使用 weir 之后，认为 wqe cache 很少，可以忽略不记其 cache miss  
					if(wqecache.contains()){
						delay += 0;
					}else{
						delay += PCIeDelay / 1e6;
						// extra pcie delay for mtt mpt related with wqe
						int mtt_mpt_probability1 = generateMttMptProbability1();
						if(mtt_mpt_probability1 == 0){
							delay += PCIeDelay / 1e6;  // us -> s
						}else{
							delay += 0;	 // no delay
						}
					}
				}
					// qpc
					// for DCT
					if(!Settings::use_dct){
						// DCT 不会造成 qpc 的丢失，因此只有非 DCT 的方案才需要考虑 qpc cache 是否在 list 之中的情况
						/*[hyx]*/
						// 获取 QP 的 qpid（注意 qIndex 是队列索引，m_qpid 是 QPC 全局 ID）
						Ptr<RdmaQueuePair> qpcQp = m_rdmaEQ->GetQp(qIndex);
						uint32_t qpc_id = qpcQp ? qpcQp->m_qpid : (uint32_t)qIndex;
						if(qpccache.contains(qpc_id)){
							delay += 0;
							/*[hyx]*/
							// Janus: LRU 标记最近使用
							qpccache.touch(qpc_id);
							// Janus: 记录 cache hit
							if(m_prefetchPredictor && Settings::use_janus){
								m_prefetchPredictor->RecordAccess(qpc_id, true);
								m_nopWqeManager->MarkPrefetchUsed(qpc_id);
							}
						}else if(Settings::use_janus && m_nopWqeManager && 
						         m_nopWqeManager->IsPrefetching(qpc_id)){
							/*[hyx]*/
							// Janus: QPC 正在预取中，使用部分延迟
							delay += PCIeDelay / (2 * 1e6);
							// 不需要 MTT/MPT 额外延迟（预取的 NOP WQE 已经包含了）
							if(m_prefetchPredictor){
								m_prefetchPredictor->RecordAccess(qpc_id, false);
							}
						}else{
							delay += PCIeDelay / 1e6;
							// extra pcie delay for mtt mpt related with qpc
							int mtt_mpt_probability2 = generateMttMptProbability2();
							if(mtt_mpt_probability2 == 0){
								delay += PCIeDelay / 1e6;  // us -> s
							}else{
								delay += 0;	 // no delay
							}
							/*[hyx]*/
							// Janus: 记录 cache miss
							if(m_prefetchPredictor && Settings::use_janus){
								m_prefetchPredictor->RecordAccess(qpc_id, false);
							}
						}
					}
				}

				if (qIndex == -1){ // high prio
					p = m_rdmaEQ->DequeueQindex(qIndex);
					m_traceDequeue(p, 0);
//					std::cout << "host " << m_node->m_id << " " << Simulator::Now().GetNanoSeconds() << " 0" << std::endl;

					/*[hyx]*/
					if(Settings::use_rnic_cache){
						TransmitStartWithDelay(p, delay);
					}else{
						TransmitStart(p);
					}

					return;
				}
				// a qp dequeue a packet
				Ptr<RdmaQueuePair> lastQp = m_rdmaEQ->GetQp(qIndex);
				p = m_rdmaEQ->DequeueQindex(qIndex);
				/*[hyx]*/
				// update rnic cache
				if(Settings::use_rnic_cache){
					// mtt mpt nothing to do
					// wqe add 1
				if(!Settings::use_weir){
					wqecache.increment();
				}
				/*[hyx]*/
				// Janus: qpc insert qpid for lastQp
				// 如果 QPC 正在预取中，跳过 insert（NOP WQE 完成后会自动 insert）
				// 否则会造成双重 insert 和延迟模型语义矛盾
				bool is_prefetching = (Settings::use_janus && m_nopWqeManager &&
				                       m_nopWqeManager->IsPrefetching(lastQp->m_qpid));
				if(!is_prefetching){
					uint32_t evicted_id = qpccache.insert(lastQp->m_qpid);
					// 如果有 QPC 被替换，更新预取管理器和预测器
					if(evicted_id != 0xFFFFFFFF){
						if(m_nopWqeManager){
							m_nopWqeManager->CheckAndMarkUselessPrefetch(evicted_id);
						}
						if(m_prefetchPredictor){
							m_prefetchPredictor->UpdateCacheStatus(evicted_id, false);
						}
					}
				}
				}

				// transmit
				m_traceQpDequeue(p, lastQp);
//				std::cout << "host " << m_node->m_id << " " << Simulator::Now().GetNanoSeconds() << " 1" << std::endl;

				/*[hyx]*/
				if(Settings::use_rnic_cache){
					TransmitStartWithDelay(p, delay);
				}else{
					TransmitStart(p);
				}

				// update for the next avail time
				m_rdmaPktSent(lastQp, p, m_tInterframeGap);
			}else { // no packet to send
				NS_LOG_INFO("PAUSE prohibits send at node " << m_node->GetId());
				Time t = Simulator::GetMaximumSimulationTime();
				for (uint32_t i = 0; i < m_rdmaEQ->GetFlowCount(); i++){
					Ptr<RdmaQueuePair> qp = m_rdmaEQ->GetQp(i);
					if (qp->GetBytesLeft() == 0)
						continue;
					t = Min(qp->m_nextAvail, t);
				}
				if (m_nextSend.IsExpired() && t < Simulator::GetMaximumSimulationTime() && t > Simulator::Now()){
					m_nextSend = Simulator::Schedule(t - Simulator::Now(), &QbbNetDevice::DequeueAndTransmit, this);
				}
			}
			return;
		}else{   //switch, doesn't care about qcn, just send
			p = m_queue->DequeueRR(m_paused, m_pfc_on_queue, Settings::hostIp2IdMap);		//this is round-robin
			if (p != 0){
				m_snifferTrace(p);
				m_promiscSnifferTrace(p);
				Ipv4Header h;
				Ptr<Packet> packet = p->Copy();
				uint16_t protocol = 0;
				ProcessHeader(packet, protocol);
				packet->RemoveHeader(h);
				FlowIdTag t;
				p->RemovePacketTag(t);
				TransmitStart(p);
				uint32_t qIndex = m_queue->GetLastQueue();
				m_node->SwitchNotifyDequeue(m_ifIndex, t.GetFlowId(), qIndex, p);
				m_traceDequeue(p, qIndex);

				//to analysis queuing time
				QueueingTag q;
				if(p->RemovePacketTag(q)){ q.Dequeue(); p->AddPacketTag(q); }

				return;
			}else{ //No queue can deliver any packet
				NS_LOG_INFO("PAUSE prohibits send at node " << m_node->GetId());
				if (m_node->GetNodeType() == 0 && m_qcnEnabled){ //nothing to send, possibly due to qcn flow control, if so reschedule sending
					Time t = Simulator::GetMaximumSimulationTime();
					for (uint32_t i = 0; i < m_rdmaEQ->GetFlowCount(); i++){
						Ptr<RdmaQueuePair> qp = m_rdmaEQ->GetQp(i);
						if (qp->GetBytesLeft() == 0)
							continue;
						t = Min(qp->m_nextAvail, t);
					}
					if (m_nextSend.IsExpired() && t < Simulator::GetMaximumSimulationTime() && t > Simulator::Now()){
						m_nextSend = Simulator::Schedule(t - Simulator::Now(), &QbbNetDevice::DequeueAndTransmit, this);
					}
				}
			}
		}
		return;
	}

	void
		QbbNetDevice::Resume(unsigned qIndex)
	{
		NS_LOG_FUNCTION(this << qIndex);
		NS_ASSERT_MSG(m_paused[qIndex], "Must be PAUSEd");
		m_paused[qIndex] = false;
		NS_LOG_INFO("Node " << m_node->GetId() << " dev " << m_ifIndex << " queue " << qIndex <<
			" resumed at " << Simulator::Now().GetSeconds());
		DequeueAndTransmit();
	}

	void
		QbbNetDevice::Receive(Ptr<Packet> packet)
	{
		NS_LOG_FUNCTION(this << packet);
		if (!m_linkUp){
			m_traceDrop(packet, 0);
			return;
		}

		if (m_receiveErrorModel && m_receiveErrorModel->IsCorrupt(packet))
		{
			// 
			// If we have an error model and it indicates that it is time to lose a
			// corrupted packet, don't forward this packet up, let it go.
			//
			m_phyRxDropTrace(packet);
			return;
		}

		m_macRxTrace(packet);
		CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
		ch.getInt = 1; // parse INT header
		packet->PeekHeader(ch);
		if (ch.l3Prot == 0xFE){ // PFC
			if (!m_qbbEnabled) return;
			unsigned qIndex = ch.pfc.qIndex;
			if (ch.pfc.time > 0){
				m_tracePfc(1);
				m_paused[qIndex] = true;
//				std::cout << Simulator::Now() << " switch-" << m_node->m_id << "-" << m_ifIndex << " pause: " << qIndex << std::endl;
			}else{
				m_tracePfc(0);
				Resume(qIndex);
//				std::cout << Simulator::Now() << " switch-" << m_node->m_id << "-" << m_ifIndex << " resume: " << qIndex << std::endl;
			}
		}else { // non-PFC packets (data, ACK, NACK, CNP, Switch-ACK...)
			if (m_node->GetNodeType() > 0){ // switch
				packet->AddPacketTag(FlowIdTag(m_ifIndex));

				// to anlysis queuing time
				QueueingTag q;
				if(packet->RemovePacketTag(q)){q.Enqueue(); packet->AddPacketTag(q); }

				m_node->SwitchReceiveFromDevice(this, packet, ch);
			}else { // NIC
				// send to RdmaHw
				int ret = m_rdmaReceiveCb(packet, ch);
				// TODO we may based on the ret do something
			}
		}
		return;
	}

	bool QbbNetDevice::Send(Ptr<Packet> packet, const Address &dest, uint16_t protocolNumber)
	{
		NS_ASSERT_MSG(false, "QbbNetDevice::Send not implemented yet\n");
		return false;
	}

	bool QbbNetDevice::SwitchSend (uint32_t qIndex, Ptr<Packet> packet, CustomHeader &ch){
		m_macTxTrace(packet);
		m_traceEnqueue(packet, qIndex);
		m_queue->Enqueue(packet, qIndex);
		DequeueAndTransmit();
		return true;
	}

	void QbbNetDevice::SendPfc(uint32_t qIndex, uint32_t type){
		Ptr<Packet> p = Create<Packet>(0);
		PauseHeader pauseh((type == 0 ? m_pausetime : 0), m_queue->GetNBytes(qIndex), qIndex);
		p->AddHeader(pauseh);
		Ipv4Header ipv4h;  // Prepare IPv4 header
		ipv4h.SetProtocol(0xFE);
		ipv4h.SetSource(m_node->GetObject<Ipv4>()->GetAddress(m_ifIndex, 0).GetLocal());
		ipv4h.SetDestination(Ipv4Address("255.255.255.255"));
		ipv4h.SetPayloadSize(p->GetSize());
		ipv4h.SetTtl(1);
		ipv4h.SetIdentification(UniformVariable(0, 65536).GetValue());
		p->AddHeader(ipv4h);
		AddHeader(p, 0x800);
		CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
		p->PeekHeader(ch);
		SwitchSend(0, p, ch);
	}

	Ptr<Packet> QbbNetDevice::GetSwitchACKPacket(SwitchACKTag acktag, uint32_t src = 0, uint32_t dst = 0){
		assert(Settings::switch_ack_mode == Settings::SWITCH_INGRESS_CREDIT || Settings::switch_ack_mode == Settings::SWITCH_DST_CREDIT);

		uint32_t sz = std::min(Settings::packet_payload, acktag.GetPacketSize());	// todo: when acktag's size > MTU, should split packet
		Ptr<Packet> p = Create<Packet>(sz);
		Ipv4Header ipv4h;  // Prepare IPv4 header
		ipv4h.SetProtocol(0xFB); // Switch-ACK, has no L4 header

		if (src == 0){
			assert(Settings::switch_ack_mode != Settings::SWITCH_DST_CREDIT); // under SWITCH_DST_CREDIT, switchACKTag does not carry dst info and src will be used as dst when recovery window
			ipv4h.SetSource(m_node->GetObject<Ipv4>()->GetAddress(m_ifIndex, 0).GetLocal());
		}else
			ipv4h.SetSource(Ipv4Address(src));

		if (dst == 0){
			assert(!Settings::reset_only_ToR_switch_win);
			ipv4h.SetDestination(Ipv4Address("255.255.255.255"));	// dst address has no sense
		}else
			ipv4h.SetDestination(Ipv4Address(dst));

		if (Settings::reset_only_ToR_switch_win)
			ipv4h.SetTtl(64);
		else
			ipv4h.SetTtl(1);

		ipv4h.SetPayloadSize(p->GetSize());
		ipv4h.SetIdentification(UniformVariable(0, 65536).GetValue());
		p->AddHeader(ipv4h);
		AddHeader(p, 0x800);
		p->AddPacketTag(acktag);

		if (Settings::qp_mode){
			QPTag qptag;
			qptag.SetQPID(rand());
			p->AddPacketTag(qptag);
		}
		return p;
	}

	void QbbNetDevice::SendSwitchACK(SwitchACKTag acktag, uint32_t src = 0, uint32_t dst = 0){
		assert(Settings::switch_ack_mode == Settings::SWITCH_INGRESS_CREDIT || Settings::switch_ack_mode == Settings::SWITCH_DST_CREDIT);
		Ptr<Packet> p = GetSwitchACKPacket(acktag, src, dst);
		CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
		p->PeekHeader(ch);
		SwitchSend(0, p, ch);
	}

	Ptr<Packet> QbbNetDevice::GetSwitchSYNPacket(SwitchSYNTag syntag, uint32_t src = 0, uint32_t dst = 0){
		assert(Settings::switch_absolute_psn && Settings::switch_syn_timeout_us);

		uint32_t sz = std::min(Settings::packet_payload, syntag.GetPacketSize());	// todo: when tag's size > MTU, should split packet
		Ptr<Packet> p = Create<Packet>(sz);
		Ipv4Header ipv4h;  // Prepare IPv4 header
		ipv4h.SetProtocol(0xFA); // Switch-ACK, no L4 header

		if (src == 0){
			ipv4h.SetSource(m_node->GetObject<Ipv4>()->GetAddress(m_ifIndex, 0).GetLocal());
		}else
			ipv4h.SetSource(Ipv4Address(src));

		if (dst == 0){
			assert(!Settings::reset_only_ToR_switch_win);
			ipv4h.SetDestination(Ipv4Address("255.255.255.255"));
		}else
			ipv4h.SetDestination(Ipv4Address(dst));

		if (Settings::reset_only_ToR_switch_win)
			ipv4h.SetTtl(64);
		else
			ipv4h.SetTtl(1);

		ipv4h.SetPayloadSize(p->GetSize());
		ipv4h.SetIdentification(UniformVariable(0, 65536).GetValue());
		p->AddHeader(ipv4h);
		AddHeader(p, 0x800);
		p->AddPacketTag(syntag);

		if (Settings::qp_mode){
			QPTag qptag;
			qptag.SetQPID(rand());
			p->AddPacketTag(qptag);
		}
		return p;
	}

	void QbbNetDevice::SendSwitchSYN(SwitchSYNTag syntag, uint32_t src = 0, uint32_t dst = 0){
		assert(Settings::switch_ack_mode == Settings::SWITCH_INGRESS_CREDIT || Settings::switch_ack_mode == Settings::SWITCH_DST_CREDIT);
		Ptr<Packet> p = GetSwitchSYNPacket(syntag, src, dst);
		CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
		p->PeekHeader(ch);
		SwitchSend(0, p, ch);
	}

	bool
		QbbNetDevice::Attach(Ptr<QbbChannel> ch)
	{
		NS_LOG_FUNCTION(this << &ch);
		m_channel = ch;
		m_channel->Attach(this);
		NotifyLinkUp();
		return true;
	}

	bool
		QbbNetDevice::TransmitStart(Ptr<Packet> p)
	{
		NS_LOG_FUNCTION(this << p);
		NS_LOG_LOGIC("UID is " << p->GetUid() << ")");
		//
		// This function is called to start the process of transmitting a packet.
		// We need to tell the channel that we've started wiggling the wire and
		// schedule an event that will be executed when the transmission is complete.
		//
		NS_ASSERT_MSG(m_txMachineState == READY, "Must be READY to transmit");
		m_txMachineState = BUSY;
		m_currentPkt = p;
		m_phyTxBeginTrace(m_currentPkt);
		Time txTime = Seconds(m_bps.CalculateTxTime(p->GetSize()));
		Time txCompleteTime = txTime + m_tInterframeGap;
		NS_LOG_LOGIC("Schedule TransmitCompleteEvent in " << txCompleteTime.GetSeconds() << "sec");
		Simulator::Schedule(txCompleteTime, &QbbNetDevice::TransmitComplete, this);

		bool result = m_channel->TransmitStart(p, this, txTime);
		if (result == false)
		{
			m_phyTxDropTrace(p);
		}
		return result;
	}

	/*[hyx]*/
	// transmit with delay
	bool
		QbbNetDevice::TransmitStartWithDelay(Ptr<Packet> p, double delay)
	{
		NS_LOG_FUNCTION(this << p);
		NS_LOG_LOGIC("UID is " << p->GetUid() << ")");
		//
		// This function is called to start the process of transmitting a packet.
		// We need to tell the channel that we've started wiggling the wire and
		// schedule an event that will be executed when the transmission is complete.
		//
		NS_ASSERT_MSG(m_txMachineState == READY, "Must be READY to transmit");
		m_txMachineState = BUSY;
		m_currentPkt = p;
		m_phyTxBeginTrace(m_currentPkt);
		// add delay to txTime
		Time txTime = Seconds(delay + m_bps.CalculateTxTime(p->GetSize()));
		Time txCompleteTime = txTime + m_tInterframeGap;
		NS_LOG_LOGIC("Schedule TransmitCompleteEvent in " << txCompleteTime.GetSeconds() << "sec");
		Simulator::Schedule(txCompleteTime, &QbbNetDevice::TransmitComplete, this);

		bool result = m_channel->TransmitStart(p, this, txTime);
		if (result == false)
		{
			m_phyTxDropTrace(p);
		}
		return result;
	}

	Ptr<Channel>
		QbbNetDevice::GetChannel(void) const
	{
		return m_channel;
	}

   bool QbbNetDevice::IsQbb(void) const{
	   return true;
   }

   void QbbNetDevice::NewQp(Ptr<RdmaQueuePair> qp){
	   qp->m_nextAvail = Simulator::Now();
	   DequeueAndTransmit();
   }
   void QbbNetDevice::ReassignedQp(Ptr<RdmaQueuePair> qp){
	   DequeueAndTransmit();
   }
   /*[hyx]*/
   // 对 xrc 进行建模    
   void QbbNetDevice::TriggerTransmit(void){
		if(!Settings::use_xrc){
	   		DequeueAndTransmit();
		}else{
			// 如果使用了 xrc 模式，那么增加 PCIe 延迟的一半作为共享接收队列处理的时延大小
			Simulator::Schedule(Seconds((static_cast<double>(PCIeDelay)/2)/1e6), &QbbNetDevice::DequeueAndTransmit, this);
		}
   }

   /*[hyx]*/
   void QbbNetDevice::TriggerTransmitWithDelay(void){
		// DequeueAndTransmit();
		if(!Settings::use_xrc){
			Simulator::Schedule(Seconds(static_cast<double>(PCIeDelay)/1e6), &QbbNetDevice::DequeueAndTransmit, this);
		}else{
			// 如果使用了 xrc 模式，那么增加 PCIe 延迟的一半作为共享接收队列处理的时延大小
			Simulator::Schedule(Seconds((static_cast<double>(PCIeDelay)*1.5)/1e6), &QbbNetDevice::DequeueAndTransmit, this);
		}
   }

	void QbbNetDevice::SetQueue(Ptr<BEgressQueue> q){
		NS_LOG_FUNCTION(this << q);
		m_queue = q;
	}

	Ptr<BEgressQueue> QbbNetDevice::GetQueue(){
		return m_queue;
	}

	Ptr<RdmaEgressQueue> QbbNetDevice::GetRdmaQueue(){
		return m_rdmaEQ;
	}

	void QbbNetDevice::RdmaEnqueueHighPrioQ(Ptr<Packet> p){
		m_traceEnqueue(p, 0);
		m_rdmaEQ->EnqueueHighPrioQ(p);
	}

	void QbbNetDevice::TakeDown(){
		// TODO: delete packets in the queue, set link down
		if (m_node->GetNodeType() == 0){
			// clean the high prio queue
			m_rdmaEQ->CleanHighPrio(m_traceDrop);
			// notify driver/RdmaHw that this link is down
			m_rdmaLinkDownCb(this);
		}else { // switch
			// clean the queue
			for (uint32_t i = 0; i < qCnt; i++)
				m_paused[i] = false;
			while (1){
				Ptr<Packet> p = m_queue->DequeueRR(m_paused, m_pfc_on_queue, Settings::hostIp2IdMap);
				if (p == 0)
					 break;
				m_traceDrop(p, m_queue->GetLastQueue());
			}
			// TODO: Notify switch that this link is down
		}
		m_linkUp = false;
	}

	void QbbNetDevice::UpdateNextAvail(Time t){
		if (!m_nextSend.IsExpired() && t < m_nextSend.GetTs()){
			Simulator::Cancel(m_nextSend);
			Time delta = t < Simulator::Now() ? Time(0) : t - Simulator::Now();
			m_nextSend = Simulator::Schedule(delta, &QbbNetDevice::DequeueAndTransmit, this);
		}
	}

	/*[hyx]*/
	// ========== Janus: QPC 预取相关方法 ==========

	void QbbNetDevice::InitPrefetch(){
		if(!Settings::use_janus) return;
		
		m_prefetchPredictor = new QpcPrefetchPredictor();
		m_nopWqeManager = new NopWqeManager();
		
		// 调度第一个预取周期（延迟到仿真开始 2s 后，与流量对齐）
		Simulator::Schedule(Seconds(2.0), &QbbNetDevice::ScheduleNextPrefetchCycle, this);
	}

	void QbbNetDevice::PerformPrefetch(){
		if(!Settings::use_janus) return;
		if(!m_prefetchPredictor || !m_nopWqeManager) return;

		uint32_t prefetch_count = 0;

		if(Settings::prefetch_mode == 1){ // Oracle 模式
			uint64_t now = Simulator::Now().GetNanoSeconds();
			std::vector<uint32_t> upcoming = m_prefetchPredictor->GetUpcomingFlows(
				now, Settings::prefetch_ahead_time, this);
			for(uint32_t qpc_id : upcoming){
				if(prefetch_count >= Settings::max_prefetch_per_cycle) break;
				if(qpccache.contains(qpc_id)) continue;
				if(m_nopWqeManager->IsPrefetching(qpc_id)) continue;
				IssueNopWqeForPrefetch(qpc_id);
				prefetch_count++;
			}
		}else{
			// 统计模式 / LRU 模式
			std::vector<uint32_t> targets = m_prefetchPredictor->GetPrefetchTargets(this);
			for(uint32_t qpc_id : targets){
				if(prefetch_count >= Settings::max_prefetch_per_cycle) break;
				if(qpccache.contains(qpc_id)) continue;
				if(m_nopWqeManager->IsPrefetching(qpc_id)) continue;
				IssueNopWqeForPrefetch(qpc_id);
				prefetch_count++;
			}
		}

		// 更新预测器
		m_prefetchPredictor->UpdatePredictions();

		// 调度下一个预取周期
		ScheduleNextPrefetchCycle();
	}

	void QbbNetDevice::ScheduleNextPrefetchCycle(){
		if(Settings::use_janus){
			Simulator::Schedule(NanoSeconds(Settings::prefetch_interval), 
			                    &QbbNetDevice::PerformPrefetch, this);
		}
	}

	void QbbNetDevice::IssueNopWqeForPrefetch(uint32_t qpc_id){
		if(!m_nopWqeManager || !m_prefetchPredictor) return;

		// 1. 记录 NOP WQE 发出
		m_nopWqeManager->IssueNopWqe(qpc_id);
		m_prefetchPredictor->m_prefetchIssuedCount++;
		// 标记该 QPC 已发起预取（防止重复预取）
		m_prefetchPredictor->MarkPrefetching(qpc_id);

		// 2. 计算完成延迟（延迟模型）
		double nop_latency = NOP_WQE_DOORBELL_LATENCY; // 0.5 us

		// 检查 QPC 是否已在缓存（如果已在，NOP WQE 开销极小）
		if(!qpccache.contains(qpc_id)){
			// QPC 不在缓存，需要 DMA 读取
			nop_latency += PCIeDelay; // 1.0 us
			// MTT/MPT 可能缺失
			int mtt_mpt_prob = generateMttMptProbability2();
			if(mtt_mpt_prob == 0){
				nop_latency += PCIeDelay; // 额外 1.0 us
			}
		}
		nop_latency += NOP_WQE_CQE_LATENCY; // 0.3 us

		// 3. 调度 NOP WQE 完成事件
		Simulator::Schedule(MicroSeconds(nop_latency), 
		                    &QbbNetDevice::OnNopWqeComplete, this, qpc_id);
	}

	void QbbNetDevice::OnNopWqeComplete(uint32_t qpc_id){
		/*[hyx]*/
		// Janus: NOP WQE 完成，QPC 已加载到缓存
		// 插入 QPC 缓存，标记为预取
		uint32_t evicted_id = qpccache.insert(qpc_id, true);

		// 通知 NopWqeManager 完成
		m_nopWqeManager->OnNopWqeComplete(qpc_id);

		// 如果有 QPC 被替换，检查是否为预取的 QPC
		if(evicted_id != 0xFFFFFFFF && m_nopWqeManager){
			m_nopWqeManager->CheckAndMarkUselessPrefetch(evicted_id);
		}

		// 通知预测器预取完成，更新缓存状态
		if(m_prefetchPredictor){
			m_prefetchPredictor->UpdateCacheStatus(qpc_id, true);
			// 如果有 QPC 被替换，更新其缓存状态
			if(evicted_id != 0xFFFFFFFF){
				m_prefetchPredictor->UpdateCacheStatus(evicted_id, false);
			}
		}
	}

} // namespace ns3
