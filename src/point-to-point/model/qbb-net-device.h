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
* Author: Yibo Zhu <yibzh@microsoft.com>
*/
#ifndef QBB_NET_DEVICE_H
#define QBB_NET_DEVICE_H

#include "ns3/point-to-point-net-device.h"
#include "ns3/broadcom-node.h"
#include "ns3/qbb-channel.h"
//#include "ns3/fivetuple.h"
#include "ns3/event-id.h"
#include "ns3/broadcom-egress-queue.h"
#include "ns3/ipv4.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"
#include "ns3/rdma-queue-pair.h"
#include <vector>
#include<map>
#include <ns3/rdma.h>
/*[hyx]*/
// 导入随机种子
#include <random>

namespace ns3 {

// 前向声明 (Janus)
class QpcPrefetchPredictor;
class NopWqeManager;

/*[hyx]*/
// for weir
#define INC 5  // ns
#define DEC 5

// PCIe相关参数
#define PCIeDelay 1  // us
#define PCIeBW 128  // Gbps

// MTT/MPT建模（数据传输相关），以概率的方式丢失表项
#define MTT_MPT_PROBABILITY 10  // 10%的概率丢失表项
int generateMttMptProbability();
// for wqe cache miss
int generateMttMptProbability1();
// for qpc cache miss
int generateMttMptProbability2();

// WQE建模，以Mellanox CX6为例，（经过测试）网卡最多可以容纳4096个WQE，超出的话随机丢弃
#define MAX_WQE_NUM 4096
// 为了简化模型，这里假设Inflight WQE数量大于MAX_WQE_NUM的时候，直接丢弃，并且不区分具体的WQE
class WqeCache
{
  private:
    uint32_t wqe_num;
  public:
    WqeCache(): wqe_num(0) {};
    ~WqeCache(){};
    void increment();
    void decrement();
    bool contains();
};

// 构建QPC缓存资源的建模，以Mellanox CX6（100Gbps）为例，网卡最多可以容纳1024个QPC，超出的话随机丢弃
#define MAX_QPC_NUM 1024

/*[hyx]*/
// NOP WQE 相关参数 (Janus)
#define NOP_WQE_DOORBELL_LATENCY 0.5  // us, WQE Doorbell 写入延迟
#define NOP_WQE_CQE_LATENCY 0.3       // us, CQE 回收延迟

struct QpcInfo
{
	uint32_t qpc_id;
  QpcInfo* next;
  QpcInfo(uint32_t value): qpc_id(value), next(nullptr) {}
};

/*[hyx]*/
// LRU 双向链表节点 (Janus)
struct QpcNode
{
    uint32_t qpc_id;
    QpcNode* prev;
    QpcNode* next;
    bool is_prefetched;          // 是否通过预取进入
    uint64_t insert_time;        // 插入时间 (ns)
    uint64_t last_access_time;   // 最后访问时间 (ns)
    QpcNode(uint32_t value) : qpc_id(value), prev(nullptr), next(nullptr),
        is_prefetched(false), insert_time(0), last_access_time(0) {}
};

class QpcCache
{
  private:
    // 原始随机替换模式（向后兼容）
    QpcInfo* head;
    QpcInfo* tail;
    uint32_t qpc_num;
    /*[hyx]*/
    // LRU 模式 (Janus)
    QpcNode* lru_head;   // LRU 头（最近使用）
    QpcNode* lru_tail;   // LRU 尾（最久未使用，替换目标）
    std::map<uint32_t, QpcNode*> lru_map; // 快速查找
    uint32_t lru_num;
    uint32_t lru_capacity;  // LRU 容量（根据 XRC 模式动态调整）
  public:
    QpcCache(): head(nullptr), tail(nullptr), qpc_num(0),
        lru_head(nullptr), lru_tail(nullptr), lru_num(0), lru_capacity(MAX_QPC_NUM) {};
    ~QpcCache(){
      while(head != nullptr){
        QpcInfo* temp = head;
        head = head->next;
        delete temp;
      }
      while(lru_head != nullptr){
        QpcNode* temp = lru_head;
        lru_head = lru_head->next;
        delete temp;
      }
    };
    bool contains(uint32_t value);
    uint32_t insert(uint32_t value, bool is_prefetched = false); // 返回被替换的 QPC ID，0xFFFFFFFF 表示无替换
    /*[hyx]*/
    void touch(uint32_t value);      // 标记最近使用（移到 LRU 链表头部）(Janus)
    void remove(uint32_t value);     // 从缓存中移除 (Janus)
    void print();
    uint32_t size();                 // 当前缓存中的 QPC 数量 (Janus)
};

class RdmaEgressQueue : public Object{
public:
  // for weir
  uint64_t total_active_qp_num;

	static const uint32_t qCnt = 8;
	static uint32_t ack_q_idx;
	uint32_t m_mtu;
	int m_qlast;
	uint32_t m_rrlast;
	Ptr<DropTailQueue> m_ackQ; // highest priority queue
	Ptr<RdmaQueuePairGroup> m_qpGrp; // queue pairs

	// callback for get next packet
	typedef Callback<Ptr<Packet>, Ptr<RdmaQueuePair> > RdmaGetNxtPkt;
	RdmaGetNxtPkt m_rdmaGetNxtPkt;

	static TypeId GetTypeId (void);
	RdmaEgressQueue();
	Ptr<Packet> DequeueQindex(int qIndex);
	int GetNextQindex(bool paused[], bool pfc_queue);
	int GetLastQueue();
	uint32_t GetNBytes(uint32_t qIndex);
	uint32_t GetFlowCount(void);
	Ptr<RdmaQueuePair> GetQp(uint32_t i);
	void RecoverQueue(uint32_t i);
	void EnqueueHighPrioQ(Ptr<Packet> p);
	void CleanHighPrio(TracedCallback<Ptr<const Packet>, uint32_t> dropCb);

	TracedCallback<Ptr<const Packet>, uint32_t> m_traceRdmaEnqueue;
	TracedCallback<Ptr<const Packet>, uint32_t> m_traceRdmaDequeue;
};

/**
 * \class QbbNetDevice
 * \brief A Device for a IEEE 802.1Qbb Network Link.
 */
class QbbNetDevice : public PointToPointNetDevice 
{
public:
  static const uint32_t qCnt = 8;	// Number of queues/priorities used
  static const uint32_t pCnt = 257;	// Number of ports used
  static const uint32_t fCnt = 128; // Max number of flows on a NIC, for TX and RX respectively. TX+RX=fCnt*2
  static const uint32_t maxHop = 1; // Max hop count in the network. should not exceed 16 


	/*[hyx]*/
	// 网卡缓存资源建模
	WqeCache wqecache;
	QpcCache qpccache;

	/*[hyx]*/
	// Janus: QPC 预取相关
	QpcPrefetchPredictor* m_prefetchPredictor;
	NopWqeManager* m_nopWqeManager;

	/*[hyx]*/
	// Janus: QPC 预取方法
	void PerformPrefetch();              // 执行预取
	void ScheduleNextPrefetchCycle();    // 调度下一个预取周期
	void IssueNopWqeForPrefetch(uint32_t qpc_id); // 发出 NOP WQE 进行预取
	void OnNopWqeComplete(uint32_t qpc_id);  // NOP WQE 完成回调
	void InitPrefetch();                 // 初始化预取系统

  // for DCT
  int last_qIndex;

  /**
   * The queues for each priority class.
   * @see class Queue
   * @see class InfiniteQueue
   */
  Ptr<BEgressQueue> m_queue;

  static TypeId GetTypeId (void);

  QbbNetDevice ();
  virtual ~QbbNetDevice ();

  /**
   * Receive a packet from a connected PointToPointChannel.
   *
   * This is to intercept the same call from the PointToPointNetDevice
   * so that the pause messages are honoured without letting
   * PointToPointNetDevice::Receive(p) know
   *
   * @see PointToPointNetDevice
   * @param p Ptr to the received packet.
   */
  virtual void Receive (Ptr<Packet> p);

  /**
   * Send a packet to the channel by putting it to the queue
   * of the corresponding priority class
   *
   * @param packet Ptr to the packet to send
   * @param dest Unused
   * @param protocolNumber Protocol used in packet
   */
  virtual bool Send(Ptr<Packet> packet, const Address &dest, uint16_t protocolNumber);
  virtual bool SwitchSend (uint32_t qIndex, Ptr<Packet> packet, CustomHeader &ch);

  /**
   * Get the size of Tx buffer available in the device
   *
   * @return buffer available in bytes
   */
  //virtual uint32_t GetTxAvailable(unsigned) const;

  /**
   * TracedCallback hooks
   */
  void ConnectWithoutContext(const CallbackBase& callback);
  void DisconnectWithoutContext(const CallbackBase& callback);

  bool Attach (Ptr<QbbChannel> ch);

   virtual Ptr<Channel> GetChannel (void) const;

   void SetQueue (Ptr<BEgressQueue> q);
   Ptr<BEgressQueue> GetQueue ();
   virtual bool IsQbb(void) const;
   void NewQp(Ptr<RdmaQueuePair> qp);
   void ReassignedQp(Ptr<RdmaQueuePair> qp);
   void TriggerTransmit(void);
   /*[hyx]*/
   void TriggerTransmitWithDelay(void);

	void SendPfc(uint32_t qIndex, uint32_t type); // type: 0 = pause, 1 = resume
	void SendSwitchACK(SwitchACKTag acktag, uint32_t src, uint32_t dst);
	Ptr<Packet> GetSwitchACKPacket(SwitchACKTag acktag, uint32_t src, uint32_t dst);
	void SendSwitchSYN(SwitchSYNTag acktag, uint32_t src, uint32_t dst);
	Ptr<Packet> GetSwitchSYNPacket(SwitchSYNTag acktag, uint32_t src, uint32_t dst);

	TracedCallback<Ptr<const Packet>, uint32_t> m_traceEnqueue;
	TracedCallback<Ptr<const Packet>, uint32_t> m_traceDequeue;
	TracedCallback<Ptr<const Packet>, uint32_t> m_traceDrop;
	TracedCallback<uint32_t> m_tracePfc; // 0: resume, 1: pause
	bool m_pfc_on_queue;		// Whether PFC_ON_QUEUE mode
protected:

	//Ptr<Node> m_node;

  bool TransmitStart (Ptr<Packet> p);
  /*[hyx]*/
  // transmit with delay 
  bool TransmitStartWithDelay (Ptr<Packet> p, double delay);
  
  virtual void DoDispose(void);

  /// Reset the channel into READY state and try transmit again
  virtual void TransmitComplete(void);

  /// Look for an available packet and send it using TransmitStart(p)
  virtual void DequeueAndTransmit(void);

  /// Resume a paused queue and call DequeueAndTransmit()
  virtual void Resume(unsigned qIndex);

  Ptr<QbbChannel> m_channel;
  
  //pfc
  bool m_qbbEnabled;	//< PFC behaviour enabled
  bool m_qcnEnabled;
  bool m_dynamicth;
  uint32_t m_pausetime;	//< Time for each Pause
  bool m_paused[Settings::NODESCALE];	//< Whether a queue/dst paused

  //qcn

  /* RP parameters */
  EventId  m_nextSend;		//< The next send event
  /* State variable for rate-limited queues */

  //qcn

  struct ECNAccount{
	  Ipv4Address source;
	  uint32_t qIndex;
	  uint32_t port;
	  uint8_t ecnbits;
	  uint16_t qfb;
	  uint16_t total;
  };

  std::vector<ECNAccount> *m_ecn_source;

public:
	Ptr<RdmaEgressQueue> m_rdmaEQ;
	void RdmaEnqueueHighPrioQ(Ptr<Packet> p);

	// callback for processing packet in RDMA
	typedef Callback<int, Ptr<Packet>, CustomHeader&> RdmaReceiveCb;
	RdmaReceiveCb m_rdmaReceiveCb;
	// callback for link down
	typedef Callback<void, Ptr<QbbNetDevice> > RdmaLinkDownCb;
	RdmaLinkDownCb m_rdmaLinkDownCb;
	// callback for sent a packet
	typedef Callback<void, Ptr<RdmaQueuePair>, Ptr<Packet>, Time> RdmaPktSent;
	RdmaPktSent m_rdmaPktSent;

	Ptr<RdmaEgressQueue> GetRdmaQueue();
	void TakeDown(); // take down this device
	void UpdateNextAvail(Time t);

	TracedCallback<Ptr<const Packet>, Ptr<RdmaQueuePair> > m_traceQpDequeue; // the trace for printing dequeue
};

} // namespace ns3

#endif // QBB_NET_DEVICE_H
