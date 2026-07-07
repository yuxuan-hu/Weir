#!/usr/bin/env python3
"""
Janus QPC Prefetch — Publication-Quality Architecture Design Figure (Final)
Output: janus-architecture-design.jpg (300dpi)
"""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.font_manager as fm
from matplotlib.patches import FancyBboxPatch, Circle
import os

# ── Fonts ────────────────────────────────────────────────────
fm._load_fontmanager(try_read_cache=False)
CJK = '/Library/Fonts/Arial Unicode.ttf'
if not os.path.exists(CJK):
    CJK = '/System/Library/AssetsV2/com_apple_MobileAsset_Font7/3419f2a427639ad8c8e139149a287865a90fa17e.asset/AssetData/PingFang.ttc'

def fp(sz, bold=False, italic=False):
    return fm.FontProperties(fname=CJK, size=sz,
        weight='bold' if bold else 'normal',
        style='italic' if italic else 'normal')

# ── Colors ───────────────────────────────────────────────────
BG     = '#FFFFFF'
LBG    = ['#E8EAF6','#E8F5E9','#FFF8E1','#FBE9E7','#F3E5F5','#E0F2F1']
LEDGE  = ['#3F51B5','#2E7D32','#E65100','#BF360C','#6A1B9A','#00695C']
MBG    = ['#C5CAE9','#A5D6A7','#FFE082','#EF9A9A','#CE93D8','#80DEEA']
MEDGE  = ['#1A237E','#1B5E20','#BF360C','#7F0000','#4A148C','#004D40']
ICLR   = ['#283593','#2E7D32','#E65100','#BF360C','#6A1B9A','#00695C']
BUSBG  = '#ECEFF1'; BUSE = '#546E7A'
BLK='#212121'; GRY='#616161'; WHT='#FFFFFF'

FW, FH = 38, 30
fig, ax = plt.subplots(figsize=(FW, FH), dpi=300)
ax.set_xlim(0, FW); ax.set_ylim(0, FH); ax.set_aspect('equal'); ax.axis('off')
fig.patch.set_facecolor(BG)

# ═════════════════════════════════════════════════════════════
# Drawing Primitives
# ═════════════════════════════════════════════════════════════
def layer_bg(x, y, w, h, ci, label):
    r = FancyBboxPatch((x,y),w,h,boxstyle="round,pad=0.22",
        fc=LBG[ci],ec=LEDGE[ci],lw=2.8,alpha=0.72,zorder=1)
    ax.add_patch(r)
    bw = max(len(label)*0.155+0.5, 4.0)
    b = FancyBboxPatch((x+0.12,y+h-0.36),bw,0.28,boxstyle="round,pad=0.06",
        fc=MBG[ci],ec=LEDGE[ci],lw=1.0,alpha=0.92,zorder=5)
    ax.add_patch(b)
    ax.text(x+0.18,y+h-0.22, label, fontproperties=fp(10.5,True),
            color=MEDGE[ci], va='center', zorder=6)

def icon_c(cx,cy,r,ci,txt):
    ax.add_patch(Circle((cx,cy),r,fc=ICLR[ci],ec=WHT,lw=1,zorder=10))
    ax.text(cx,cy,txt,fontsize=8,color=WHT,ha='center',va='center',fontweight='bold',zorder=11)

def mod(x,y,w,h,title,lines,ci,ic='',en=''):
    ax.add_patch(FancyBboxPatch((x+0.06,y-0.06),w,h,boxstyle="round,pad=0.06",
        fc='#D5D5D5',ec='none',alpha=0.18,zorder=2))
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle="round,pad=0.06",
        fc=WHT,ec=MEDGE[ci],lw=1.6,alpha=0.97,zorder=3))
    th=0.34
    ax.add_patch(FancyBboxPatch((x+0.02,y+h-th),w-0.04,th-0.02,boxstyle="round,pad=0.03",
        fc=MBG[ci],ec=MEDGE[ci],lw=1.0,alpha=0.90,zorder=4))
    tx=x+0.15
    if ic:
        icon_c(x+0.26,y+h-th/2,0.12,ci,ic)
        tx=x+0.48
    ax.text(tx,y+h-th/2+0.02, title, fontproperties=fp(9.2,True),
            color=MEDGE[ci],va='bottom',zorder=5)
    if en:
        ax.text(tx,y+h-th/2-0.07, en, fontproperties=fp(6.2,italic=True),
                color=GRY,va='top',zorder=5)
    by=y+h-th-0.17
    for ln in lines:
        indent=0
        if ln.startswith('  '): ln=ln.strip(); indent=0.18
        ax.text(x+0.12+indent,by, ln, fontproperties=fp(7.2), color=BLK, va='top', zorder=5)
        by-=0.22

def arr(x1,y1,x2,y2,c='#455A64',lw=1.5,ls='-',lbl=''):
    ax.annotate('',xy=(x2,y2),xytext=(x1,y1),
        arrowprops=dict(arrowstyle='->',color=c,lw=lw,linestyle=ls),zorder=8)
    if lbl:
        mx,my=(x1+x2)/2,(y1+y2)/2
        ax.text(mx,my+0.12, lbl, fontproperties=fp(6.5),color=c,
                ha='center',va='bottom',zorder=9,
                bbox=dict(fc=WHT,ec='none',alpha=0.85,pad=0.4))

def busbar(x,y,w,lbl,ci):
    ax.add_patch(FancyBboxPatch((x,y),w,0.26,boxstyle="round,pad=0.05",
        fc=BUSBG,ec=BUSE,lw=1.2,alpha=0.82,zorder=2))
    ax.annotate('',xy=(x+0.35,y+0.13),xytext=(x,y+0.13),
        arrowprops=dict(arrowstyle='->',color=BUSE,lw=0.8),zorder=3)
    ax.annotate('',xy=(x+w,y+0.13),xytext=(x+w-0.35,y+0.13),
        arrowprops=dict(arrowstyle='->',color=BUSE,lw=0.8),zorder=3)
    ax.text(x+w/2,y+0.13, lbl, fontproperties=fp(7.5,True),
            color=BUSE,ha='center',va='center',zorder=4)

# ═════════════════════════════════════════════════════════════
# Title
# ═════════════════════════════════════════════════════════════
ax.add_patch(FancyBboxPatch((0.5,FH-1.5),FW-1,1.2,boxstyle="round,pad=0.12",
    fc='#1A237E',ec='none',alpha=0.95,zorder=10))
ax.text(FW/2,FH-0.7, 'Janus: QPC Prefetch Architecture Design',
        fontproperties=fp(22,True),color=WHT,ha='center',va='center',zorder=11)
ax.text(FW/2,FH-1.12, 'QPC Cache Miss Optimization for RDMA NIC in Weir (NS-3) Simulator',
        fontproperties=fp(12),color='#B3E5FC',ha='center',va='center',zorder=11)

# ═════════════════════════════════════════════════════════════
# Layout
# ═════════════════════════════════════════════════════════════
LX,LW = 1.0, FW-2.0
BGAP=0.72; BH=0.26
LH = [2.6, 4.0, 3.2, 3.2, 2.6, 2.4]
yc = FH - 2.0

# ═════════════════════════════════════════════════════════════
# L0: Configuration
# ═════════════════════════════════════════════════════════════
L0=yc-LH[0]
layer_bg(LX,L0,LW,LH[0],0,'Layer 1: 仿真配置与实验入口')
my=L0+0.42; mh=1.9; mw=7.5; mg=0.6

mod(LX+0.5,my,mw,mh,'Janus 配置文件',[
    'JANUS=1/0 (总开关)',
    'PREFETCH_MODE=1(Oracle)/2(Stat)/3(LRU)',
    'PREFETCH_AHEAD_TIME=1650ns',
    'PREFETCH_INTERVAL=1ms',
    'PREFETCH_HOT_THRESHOLD=5',
    'PREFETCH_HISTORY_WINDOW=10ms',
    'MAX_PREFETCH_PER_CYCLE=16',
    'QPC_CACHE_LRU=0/1',
],0,'C','config-dcqcn-prefetch.ini')

mod(LX+0.5+mw+mg,my,mw,mh,'仿真主程序',[
    '解析 8 项 Janus 配置参数',
    '初始化各 Host 预取模块',
    'Oracle: 遍历 flows_map',
    '  AddFutureFlow(qp_id, start_ns)',
    '  注册到发送端 Predictor',
    '仿真结束: 输出预取统计',
],0,'R','scratch/third.cc')

mod(LX+0.5+2*(mw+mg),my,mw,mh,'全局参数存储',[
    '8 个 Janus 静态成员变量',
    '供 QbbNetDevice / RdmaHw 读取',
    '供 Predictor 读取',
    '默认值在 settings.cc 初始化',
],0,'S','settings.h/.cc')

mod(LX+0.5+3*(mw+mg),my,mw+0.5,mh,'全局流信息 (Oracle Prior)',[
    'flows_map: 全量流描述',
    '  QP ID (m_qpid 全局唯一)',
    '  源 Host / 目的 Host',
    '  流开始时间 start_time',
    'Oracle 模式的先验输入',
],0,'F','flows_map')

for i in range(3):
    _x=LX+0.5+(i+1)*mw+i*mg+mg*0.4
    arr(_x,my+mh/2,_x+mg*0.2,my+mh/2,LEDGE[0],1.3)

b0=L0-BGAP/2-BH/2
busbar(LX+0.8,b0,LW-1.6,'Config & Initialization Channel',0)
arr(FW/2,L0,FW/2,b0+BH,LEDGE[0],2)

# ═════════════════════════════════════════════════════════════
# L1: Prefetch Control Plane
# ═════════════════════════════════════════════════════════════
L1=b0-BGAP/2-BH/2-LH[1]
layer_bg(LX,L1,LW,LH[1],1,'Layer 2: Janus 预取控制面 (Per-Host Sender)')

# Predictor
pw,ph=7.8,3.2; px=LX+0.5; py=L1+0.6
mod(px,py,pw,ph,'预取预测器',[
    'RecordAccess(qpc_id, hit)',
    'UpdateCacheStatus(id, in_cache)',
    'GetPrefetchTargets() [Stat/LRU]',
    'GetUpcomingFlows()  [Oracle]',
    'AddFutureFlow(qp_id, start_ns)',
    '统计: m_cacheMissCount',
    '       m_totalAccessCount',
],1,'P','QpcPrefetchPredictor')

# Three modes (bottom, wider)
mdw=6.0; mdh=1.8; mgy=L1+0.42; mgx=0.45
mod(px,mgy,mdw,mdh,'Oracle 预测源 (Mode 1)',[
    '理想上界: 仿真初始化获得未来流',
    '最小堆按 start_time 排序',
    '窗口 [now, now+ahead_time]',
    '每条流只预取一次',
],1,'1','Oracle')
mod(px+mdw+mgx,mgy,mdw,mdh,'Statistical 预测源 (Mode 2)',[
    '访问频率统计: access_count>=5',
    '最近访问在 10ms 窗口内',
    '按频次降序选择目标',
],1,'2','Statistical')
mod(px+2*(mdw+mgx),mgy,mdw,mdh,'LRU 预测源 (Mode 3)',[
    '时间局部性预测',
    '最近访问链表 MAX 2048',
    '取 is_in_cache==false',
    '按最近访问时间排序',
],1,'3','LRU')
for i in range(3):
    mx=px+i*(mdw+mgx)+mdw/2
    arr(mx,mgy+mdh,px+pw*(i+1)/4,py,LEDGE[1],1.0,'--')

# NopWqeManager
nw,nh=7.2,3.2; nx=px+pw+1.0; ny=py
mod(nx,ny,nw,nh,'NOP WQE 管理器',[
    'IssueNopWqe(id) → 标记 prefetching',
    'IsPrefetching(id) → 查询中间态',
    'OnNopWqeComplete(id) → 移出集合',
    'MarkPrefetchUsed(id)',
    '  → successfulPrefetchCount++',
    'CheckAndMarkUselessPrefetch(id)',
    '  → uselessPrefetchCount++',
],1,'N','NopWqeManager')
arr(px+pw,py+ph*0.55,nx,ny+nh*0.55,LEDGE[1],2.5,lbl='Prefetch Targets')

# State Machine
sw,sh=6.5,3.2; sx=nx+nw+0.9; sy=py
mod(sx,sy,sw,sh,'预取状态机',[
    '[Not Prefetched]',
    '    | IssueNopWqe',
    '    v',
    '[Prefetching] (latency)',
    '    | OnNopWqeComplete',
    '    v',
    '[Prefetched in Cache]',
    '  /           \\',
    'MarkUsed      Evicted',
    '[Used]    [Useless Prefetch]',
],1,'SM','State Machine')

b1=L1-BGAP/2-BH/2
busbar(LX+0.8,b1,LW-1.6,'Janus Prefetch Control Bus / Candidate QPC IDs',1)
arr(FW/2,L1,FW/2,b1+BH,LEDGE[1],2)

# ═════════════════════════════════════════════════════════════
# L2: QbbNetDevice
# ═════════════════════════════════════════════════════════════
L2=b1-BGAP/2-BH/2-LH[2]
layer_bg(LX,L2,LW,LH[2],2,'Layer 3: RDMA NIC 仿真设备层 (QbbNetDevice)')
dy=L2+0.50; dw=6.8; dh=2.3; dg=0.45

mod(LX+0.5,dy,dw,dh,'预取初始化',[
    'InitPrefetch()',
    '创建 Predictor / NopWqeManager',
    '绑定到发送端 QbbNetDevice',
    'ScheduleNextPrefetchCycle()',
],2,'I','InitPrefetch')

mod(LX+0.5+dw+dg,dy,dw,dh,'周期调度器',[
    'ScheduleNextPrefetchCycle()',
    '周期: PREFETCH_INTERVAL (1ms)',
    'Simulator::Schedule 递归调度',
],2,'T','Scheduler')

mod(LX+0.5+2*(dw+dg),dy,dw,dh,'预取执行器',[
    'PerformPrefetch()',
    'GetPrefetchTargets / UpcomingFlows',
    '每周期上限 MAX=16',
    '过滤: 已缓存 / 已在预取中',
],2,'X','PerformPrefetch')

mod(LX+0.5+3*(dw+dg),dy,dw,dh,'NOP WQE 发出器',[
    'IssueNopWqeForPrefetch(id)',
    'Doorbell 0.5us + DMA 1.0us',
    '+ MTT/MPT(10%): 1.0us + CQE 0.3us',
    'Total: 0.8 / 1.8 / 2.8us',
],2,'W','IssueNopWqe')

mod(LX+0.5+4*(dw+dg),dy,dw+0.2,dh,'NOP 完成处理',[
    'OnNopWqeComplete(id)',
    'QpcCache.insert(id, is_prefetched=true)',
    '驱逐: CheckAndMark + Update',
    'UpdateCacheStatus(id, true)',
],2,'D','OnComplete')

for i in range(4):
    _x=LX+0.5+(i+1)*dw+i*dg+dg*0.3
    arr(_x,dy+dh/2,_x+dg*0.4,dy+dh/2,LEDGE[2],1.8)

b2=L2-BGAP/2-BH/2
busbar(LX+0.8,b2,LW-1.6,'NIC Internal Cache Access Bus',2)
arr(FW/2,L2,FW/2,b2+BH,LEDGE[2],2)

# ═════════════════════════════════════════════════════════════
# L3: QPC Cache
# ═════════════════════════════════════════════════════════════
L3=b2-BGAP/2-BH/2-LH[3]
layer_bg(LX,L3,LW,LH[3],3,'Layer 4: QPC Cache 资源层 (MAX_QPC_NUM=1024)')
cy=L3+0.50; cw=8.2; ch=2.4
cx=LX+LW/2-cw/2

mod(cx,cy,cw,ch,'QPC Cache (LRU 增强版)',[
    '双向链表 + 哈希表结构',
    '接口: contains / insert / touch / remove',
    'QPC_CACHE_LRU=0 → 随机替换 (原始 Weir)',
    'QPC_CACHE_LRU=1 → LRU 替换 (Janus)',
],3,'Q','QpcCache')

# QpcNode
ndw=5.5
mod(cx-ndw-0.7,cy+0.35,ndw,1.8,'QpcNode 元数据',[
    'qpc_id: 全局 QP 标识',
    'is_prefetched: 预取标记',
    'insert_time / last_access_time',
    'prev / next: LRU 双链指针',
],3,'N','QpcNode struct')

# Hit / Prefetching / Miss
subw=5.5; subh=0.7; subx=cx+cw+0.7
mod(subx,cy+1.5,subw,subh,'Cache Hit',[
    'delay=0, bw=0, touch(id)',
],3,'H','Hit Path')
mod(subx,cy+0.65,subw,subh,'Prefetching 中间态',[
    'delay=PCIe/2, bw=2Gbps',
],3,'W','Prefetching')
mod(subx,cy-0.2,subw,subh,'Cache Miss',[
    'delay=6us, bw=6Gbps+MTT/MPT',
],3,'M','Miss Path')

# Evict
evw=8.5; evh=0.85
mod(cx-evw/2,cy-0.35,evw,evh,'驱逐通知链 (Evict Notification)',[
    'CheckAndMarkUselessPrefetch(evicted) → NopWqeManager',
    'UpdateCacheStatus(evicted, false) → Predictor',
],3,'E','Evict Notification Chain')

arr(cx-0.3,cy+1.0,cx,cy+1.0,LEDGE[3],1.3)
for i in range(3):
    _sy=cy+ch-i*0.75-0.15
    arr(cx+cw,_sy,subx,_sy,LEDGE[3],1.0)

b3=L3-BGAP/2-BH/2
busbar(LX+0.8,b3,LW-1.6,'RDMA Data Access Path',3)
arr(FW/2,L3,FW/2,b3+BH,LEDGE[3],2)

# ═════════════════════════════════════════════════════════════
# L4: Data Path
# ═════════════════════════════════════════════════════════════
L4=b3-BGAP/2-BH/2-LH[4]
layer_bg(LX,L4,LW,LH[4],4,'Layer 5: RDMA 数据路径接入层')
dpy=L4+0.42; dpw=8.0; dph=1.9; dpg=0.5

mod(LX+0.5,dpy,dpw,dph,'发送延迟路径 (DequeueAndTransmit)',[
    'qIndex→m_qpid 修正 (Bug Fix)',
    'hit: delay+=0, touch, RecordAccess(true), MarkPrefetchUsed',
    'pref: delay+=PCIe/2, RecordAccess(false)',
    'miss: delay+=PCIe+MTT/MPT, insert, RecordAccess(false)',
],4,'Dq','Delay Path')

mod(LX+0.5+dpw+dpg,dpy,dpw,dph,'发送带宽路径 (GetNxtPacket)',[
    '不调用RecordAccess (防止重复计数)',
    'hit: extra_pcie_bw+=0, touch',
    'pref: extra_pcie_bw+=2 (半带宽惩罚)',
    'miss: extra_pcie_bw+=6+random+MTT/MPT',
],4,'Gn','Bandwidth Path')

mod(LX+0.5+2*(dpw+dpg),dpy,dpw,dph,'ACK 路径 (ReceiveAck)',[
    '补齐 ACK 侧访问历史',
    'miss: insert + evict通知 + RecordAccess(false)',
    'hit: RecordAccess(true) [新增else分支]',
],4,'Ak','ACK Path')

mod(LX+0.5+3*(dpw+dpg),dpy,dpw+0.5,dph,'QP 创建路径 (AddQueuePair)',[
    '不调用 RecordAccess (创建≠访问)',
    '新建QP时自动 insert QPC Cache',
    '发生驱逐: CheckAndMark + Update',
],4,'Aq','QP Creation')

b4=L4-BGAP/2-BH/2
busbar(LX+0.8,b4,LW-1.6,'Experiment & Evaluation Output',4)
arr(FW/2,L4,FW/2,b4+BH,LEDGE[4],2)

# ═════════════════════════════════════════════════════════════
# L5: Evaluation
# ═════════════════════════════════════════════════════════════
L5=b4-BGAP/2-BH/2-LH[5]
layer_bg(LX,L5,LW,LH[5],5,'Layer 6: 评估指标与对比实验')
ey=L5+0.42; ew=7.0; eh=1.7

mod(LX+0.5,ey,ew,eh,'Janus 统计指标',[
    'Cache Miss Rate = missCount / totalAccessCount',
    'Prefetch Hit Rate = successCount / totalNopWqeCount',
    'Useless Prefetch Rate = uselessCount / totalNopWqeCount',
],5,'J','Janus Metrics')

mod(LX+0.5+ew+0.4,ey,ew,eh,'Weir 性能指标',[
    'FCT 分布 (Flow Completion Time)',
    '平均吞吐量 / 首包延迟',
    'PCIe 带宽开销',
],5,'W','Weir Performance')

# Experiments
expw=4.8; exph=1.7
exps=[
    ('Baseline',     ['JANUS=0, LRU=0','随机替换, 无预取'],'B'),
    ('LRU Only',     ['JANUS=0, LRU=1','仅替换策略优化'],'L'),
    ('Oracle',       ['JANUS=1, MODE=1','理论上界 (理想)'],'O'),
    ('Statistical',  ['JANUS=1, MODE=2','热点频率预测'],'S'),
    ('LRU Prefetch', ['JANUS=1, MODE=3','时间局部性预测'],'P'),
]
esx=LX+0.5+2*(ew+0.4)+0.4
for i,(nm,ln,ic) in enumerate(exps):
    mod(esx+i*(expw+0.3),ey,expw,exph,nm,ln,5,ic)

# ═════════════════════════════════════════════════════════════
# Cross-layer feedback
# ═════════════════════════════════════════════════════════════
arr(cx-evw/2+1,L3+0.2,px+pw-0.5,L1+LH[1]-0.3,
    '#C62828',1.8,'--','Evict Notify')
arr(subx+subw/2,L3+LH[3]-0.1,nx+nw/2,L1+LH[1]-0.2,
    '#2E7D32',1.2,':','Prefetch Used')
arr(LX+0.5+dpw/2,L4+LH[4],cx+cw/2,L3,
    '#C62828',1.8,'--','QPC Access')

# ── Save ─────────────────────────────────────────────────────
fig.savefig('janus-architecture-design.jpg',dpi=300,bbox_inches='tight',
            facecolor=BG,edgecolor='none',
            pil_kwargs={'optimize':True,'quality':95})
plt.close()
print('[OK] Saved janus-architecture-design.jpg')
