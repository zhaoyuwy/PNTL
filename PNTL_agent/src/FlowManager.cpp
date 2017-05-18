
#include <math.h>       // �����׼��
#include <algorithm>    // ��������
#include <sys/time.h>   // ��ȡʱ��

using namespace std;

#include "Log.h"
#include "AgentJsonAPI.h"
#include "MessagePlatformClient.h"
#include "FlowManager.h"


// ��ʹ��ԭ��: ����������ServerFlowTableˢ�µ�AgentFlowTable. 
// ���Ҫͬʱʹ��������, �����Ȼ�ȡSERVER_WORKING_FLOW_TABLE_LOCK()�ٻ�ȡAGENT_WORKING_FLOW_TABLE_LOCK, 
// �ͷ�ʱ���ͷ�AGENT_WORKING_FLOW_TABLE_UNLOCK(),���ͷ�SERVER_WORKING_FLOW_TABLE_UNLOCK().

// Agent Flow Table
#define AGENT_WORKING_FLOW_TABLE_LOCK() \
        if (stAgentFlowTableLock) \
            sal_mutex_take(stAgentFlowTableLock, sal_mutex_FOREVER)
            
#define AGENT_WORKING_FLOW_TABLE_UNLOCK() \
        if (stAgentFlowTableLock) \
            sal_mutex_give(stAgentFlowTableLock)

#define AGENT_WORKING_FLOW_TABLE  (uiAgentWorkingFlowTable)
#define AGENT_CFG_FLOW_TABLE      ((UINT32)!uiAgentWorkingFlowTable)

// Server Flow Table
#define SERVER_WORKING_FLOW_TABLE_LOCK() \
        if (stServerFlowTableLock) \
            sal_mutex_take(stServerFlowTableLock, sal_mutex_FOREVER)
            
#define SERVER_WORKING_FLOW_TABLE_UNLOCK() \
        if (stServerFlowTableLock) \
            sal_mutex_give(stServerFlowTableLock)

#define SERVER_WORKING_FLOW_TABLE  (uiServerWorkingFlowTable)
#define SERVER_CFG_FLOW_TABLE      ((UINT32) !uiServerWorkingFlowTable)


//Agent Flow Table Entry��uiFlowState bit����
// ��ǰEntry�Ƿ���Ч.
#define FLOW_ENTRY_STATE_ENABLE     (1L << 0)
// ��ǰEntry�Ƿ���׷��ģʽ, �ɶ�������.
#define FLOW_ENTRY_STATE_TRACKING   (1L << 1)
// ��ǰEntry�Ƿ��ڶ���ģʽ, �ɶ�������.
#define FLOW_ENTRY_STATE_DROPPING   (1L << 2)

#define FLOW_ENTRY_STATE_CHECK(state, flag)     ( (state) & (flag) )
#define FLOW_ENTRY_STATE_SET(state, flag)       ( (state) = ((state)|(flag)) )
#define FLOW_ENTRY_STATE_CLEAR(state, flag)     ( (state) = ((state)&(~(flag))) )


// ���캯��, ���г�Ա��ʼ��Ĭ��ֵ.
FlowManager_C::FlowManager_C()
{
    FLOW_MANAGER_INFO("Creat a new FlowManager");

    pcAgentCfg = NULL;

    // Worker ��ʼ��
    pcWorkerTargetUDP = NULL;
    WorkerList_UDP.clear();

    // ��������
    uiAgentWorkingFlowTable = 0;
    stAgentFlowTableLock = NULL;
    AgentClearFlowTable(AGENT_WORKING_FLOW_TABLE);
    AgentClearFlowTable(AGENT_CFG_FLOW_TABLE);

    uiServerWorkingFlowTable = 0;
    stServerFlowTableLock = NULL;
    ServerClearFlowTable(SERVER_WORKING_FLOW_TABLE);
    ServerClearFlowTable(SERVER_CFG_FLOW_TABLE);

    // ҵ�����̴���
    uiNeedCheckResult = 0;
    uiLastCheckTimeCounter = 0;    
    uiLastReportTimeCounter = 0;
    uiLastQuerytTimeCounter = 0;

    uiServerFlowTableIsEmpty = 1;

    pcKafkaClient = NULL;

    semTimer = NULL;
}

// ��������,�ͷ���Դ
FlowManager_C::~FlowManager_C()
{
    FLOW_MANAGER_INFO("Destroy an old FlowManager");

    StopThread();

    // ��ͬ���ź�����Timer������ɾ��.
    pcAgentCfg->DelPollingNotifier(semTimer);

    //����ͬ���ź���
    sal_sem_destroy(semTimer);
    semTimer = NULL;

    // �������
    AgentClearFlowTable(AGENT_WORKING_FLOW_TABLE);
    AgentClearFlowTable(AGENT_CFG_FLOW_TABLE);
    ServerClearFlowTable(SERVER_WORKING_FLOW_TABLE);
    ServerClearFlowTable(SERVER_CFG_FLOW_TABLE);

    // �ͷŻ�����
    if ( stAgentFlowTableLock )
        sal_mutex_destroy(stAgentFlowTableLock);
    stAgentFlowTableLock = NULL;
    
    if ( stServerFlowTableLock )
        sal_mutex_destroy(stServerFlowTableLock);
    stServerFlowTableLock = NULL;
    
    // �ͷ�target worker.
    if (pcWorkerTargetUDP)
        delete pcWorkerTargetUDP;
    pcWorkerTargetUDP = NULL;

    // �ͷ�sender worker.
    vector<DetectWorker_C *>::iterator pcDetectWorker;
    for(pcDetectWorker = WorkerList_UDP.begin(); pcDetectWorker != WorkerList_UDP.end(); pcDetectWorker++)
    {
        if ( NULL != *pcDetectWorker )
        {
            delete *pcDetectWorker;            
        }
    }
    
    WorkerList_UDP.clear();

    // ����kafka�ͻ���
    delete pcKafkaClient;
    pcKafkaClient = NULL;
}

INT32 FlowManager_C::Init(ServerAntAgentCfg_C * pcNewAgentCfg)
{
    INT32 iRet = AGENT_OK;
    
    UINT32 uiCollectorIP = 0; 
    UINT32 uiCollectorPort = 0;

    WorkerCfg_S stNewWorker;
    DetectWorker_C * pcDetectWorker = NULL;

    CollectorProtocolType_E eConnectorProtocol = COLLECTOR_PROTOCOL_NULL;
    
    
    if(NULL == pcNewAgentCfg)
    {
        FLOW_MANAGER_ERROR("Null Point.");
        return AGENT_E_PARA;
    }
    
    if(pcAgentCfg)
    {
        FLOW_MANAGER_ERROR("Do not reinit this FlowManager.");
        return AGENT_E_PARA;
    }

    // ������ʼ��
    pcAgentCfg = pcNewAgentCfg;

    stAgentFlowTableLock = sal_mutex_create("Flow Manager FlowTableLock");
    if( NULL == stAgentFlowTableLock )
    {
        FLOW_MANAGER_ERROR("Create mutex failed");
        return AGENT_E_MEMORY;
    }

    stServerFlowTableLock = sal_mutex_create("Flow Manager ServerFlowTableLock");
    if( NULL == stServerFlowTableLock )
    {
        FLOW_MANAGER_ERROR("Create mutex failed");
        return AGENT_E_MEMORY;
    }


    // ���Agent��Collector֮���ͨѶ��ʽ
    iRet = pcAgentCfg->GetCollectorProtocol(&eConnectorProtocol);
    if(iRet)
    {
        FLOW_MANAGER_ERROR("Get Collector Protocol Info failed[%d]", iRet);
        return AGENT_E_PARA;
    }
    // �Ƿ�ʹ��kafkaͨѶ
    if(COLLECTOR_PROTOCOL_KAFKA == eConnectorProtocol)
    {
        KafkaConnectInfo_S stKafkaConnectInfo;
        
        // ����kafka�ͻ���
        pcKafkaClient = new KafkaClient_C;
        if(NULL == pcKafkaClient)
        {
            FLOW_MANAGER_ERROR("Create kafkaClient failed");
            return AGENT_E_MEMORY;
        }
        // ��ѯKafka������Ϣ
        iRet = pcAgentCfg->GetCollectorKafkaInfo(&stKafkaConnectInfo);
        if(iRet)
        {
            FLOW_MANAGER_ERROR("Get Collector Kafka Info failed[%d]", iRet);
            return AGENT_E_PARA;
        }


// ���Կ��ƿͻ������� Broker ����Ŀ, ʵ��ʧ��, ��ʹֻ����һ��broker, �ͻ���Ҳ��ͨ����broker�õ��������е�broker��Ϣ.
#if 1
        // ��һ��������broker��Ϣ
        vector<string>::iterator pstrBrokerInfo;
        for(pstrBrokerInfo = stKafkaConnectInfo.KafkaBrokerList.begin(); pstrBrokerInfo != stKafkaConnectInfo.KafkaBrokerList.end(); pstrBrokerInfo++)
        {
            iRet = pcKafkaClient->AddBrokerAddress(*pstrBrokerInfo);
            if(iRet)
            {
                FLOW_MANAGER_ERROR("Add broker to kafka failed[%d]", iRet);
                return AGENT_E_PARA;
            }
        }
#else
        // BrokerList�з���������С������uiMaxBrokerNumber, ʹ�����е�Broker
        if ( stKafkaConnectInfo.KafkaBrokerList.size() <= stKafkaConnectInfo.uiMaxBrokerNumber )
        {
            // ��һ����broker��Ϣ
            vector<string>::iterator pstrBrokerInfo;
            for(pstrBrokerInfo = stKafkaConnectInfo.KafkaBrokerList.begin(); pstrBrokerInfo != stKafkaConnectInfo.KafkaBrokerList.end(); pstrBrokerInfo++)
            {
                iRet = pcKafkaClient->AddBrokerAddress(*pstrBrokerInfo);
                if(iRet)
                {
                    FLOW_MANAGER_ERROR("Add broker to kafka failed[%d]", iRet);
                    return AGENT_E_PARA;
                }
            }
        }
        // BrokerList�з�������������uiMaxBrokerNumber, ��BrokerList��ѡ��һЩ������ʹ��.
        else
        {
            struct timespec ts;
            UINT32 uiBrokerCounter = stKafkaConnectInfo.uiMaxBrokerNumber;

            // ׼�������, ���ں�����Broker�б���ѡ�������.
            clock_gettime(CLOCK_REALTIME, &ts);
            srandom(ts.tv_nsec + ts.tv_sec); //��ʱ�������������

            for ( ;0 < uiBrokerCounter;  )
            {
                // ���ѡȡһ�� Broker ������. 
                UINT32 uiBrokerIndex = random() % (stKafkaConnectInfo.KafkaBrokerList.size());
                
                iRet = pcKafkaClient->AddBrokerAddress( stKafkaConnectInfo.KafkaBrokerList[uiBrokerIndex] );
                // �ظ�broker, �����һ��
                if( AGENT_E_EXIST == iRet)
                {
                    continue;
                }
                // �Ƿ���ַ
                else if (iRet)
                {
                    FLOW_MANAGER_ERROR("Add broker to kafka failed[%d]", iRet);
                    return AGENT_E_PARA;
                }
                // ���ӳɹ�һ����Чbroker.
                else
                {
                    uiBrokerCounter --;
                }
            }
        }
#endif
        
        // ����kafka�ͻ���
        iRet = pcKafkaClient->StartKafkaClient(KAFAKA_CLIENT_TYPE_PRODUCER);
        if(iRet)
        {
            FLOW_MANAGER_ERROR("Init kafka failed[%d]", iRet);
            return AGENT_E_PARA;
        }
    }
    else
    {
        FLOW_MANAGER_ERROR("unknown CollectorProtocol [%d]", eConnectorProtocol);
        return AGENT_E_PARA;
    }

    // UDP Э���ʼ��
    {
        UINT32 uiSrcIP;
        UINT32 uiSrcPort;
        UINT32 uiSrcPortMin; 
        UINT32 uiSrcPortMax; 
        UINT32 uiDestPort;

        iRet = pcAgentCfg->GetProtocolUDP(&uiSrcPortMin, &uiSrcPortMax, &uiDestPort);
        if (iRet)
        {
            FLOW_MANAGER_ERROR("Get Protocol UDP cfg failed[%d]", iRet);
            return AGENT_E_PARA;
        }
        
        sal_memset(&stNewWorker, 0, sizeof(stNewWorker));
        stNewWorker.eProtocol  = AGENT_DETECT_PROTOCOL_UDP;
        stNewWorker.uiRole      = WORKER_ROLE_TARGET;
        stNewWorker.uiSrcPort   = uiDestPort;
        stNewWorker.uiSrcIP     = SAL_INADDR_ANY;
        
        pcWorkerTargetUDP = new DetectWorker_C;
        
        iRet = pcWorkerTargetUDP->Init(stNewWorker);
        if (iRet)
        {
            FLOW_MANAGER_ERROR("Init UDP target worker failed[%d], SIP[%s],SPort[%d]", 
                    iRet, sal_inet_ntoa(stNewWorker.uiSrcIP), stNewWorker.uiSrcPort);
            return AGENT_E_PARA;
        }

        stNewWorker.uiRole      = WORKER_ROLE_SENDER;
        for (uiSrcPort = uiSrcPortMin; uiSrcPort <= uiSrcPortMax; uiSrcPort++)
        {
            pcDetectWorker = new DetectWorker_C;
            
            // ˢ��Դ�˿ں�
            stNewWorker.uiSrcPort = uiSrcPort;
            iRet = pcDetectWorker->Init(stNewWorker);
            if (iRet)
            {
                FLOW_MANAGER_ERROR("Init UDP sender worker failed[%d], SIP[%s],SPort[%d]", 
                    iRet, sal_inet_ntoa(stNewWorker.uiSrcIP), stNewWorker.uiSrcPort);
                return AGENT_E_PARA;
            }
            WorkerList_UDP.push_back(pcDetectWorker);
        }
    }
    
    // ׼��������������
    
    // ����ͬ���ź���
    semTimer = sal_sem_create(NULL, 1 , 0);
    if (NULL == semTimer)
    {
        FLOW_MANAGER_ERROR("Create semTimer failed.");
        return AGENT_E_MEMORY;
    }

    // ע��ͬ���ź���
    iRet = pcAgentCfg->AddPollingNotifier(semTimer);
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Add Polling Notifier failed [%d]", iRet);
        return AGENT_E_TIMER;
    }
    
    // ������������
    iRet = StartThread();
    if (iRet)
    {
        FLOW_MANAGER_ERROR("StartThread failed[%d]", iRet);
        return AGENT_E_PARA;
    }
    return iRet;    
}

// Agent��������
// ����ض�����
INT32 FlowManager_C::AgentClearFlowTable(UINT32 uiAgentFlowTableNumber)
{
    if (AGENT_WORKING_FLOW_TABLE == uiAgentFlowTableNumber)
        AGENT_WORKING_FLOW_TABLE_LOCK();

    // ���ÿ�����еĽ����.
    vector<AgentFlowTableEntry_S>::iterator pAgentFlowEntry;    
    for(pAgentFlowEntry = AgentFlowTable[uiAgentFlowTableNumber].begin(); 
        pAgentFlowEntry != AgentFlowTable[uiAgentFlowTableNumber].end(); 
        pAgentFlowEntry ++)
    {
        pAgentFlowEntry->vFlowDetectResultPkt.clear();
    }

    // �����������.
    AgentFlowTable[uiAgentFlowTableNumber].clear();

    if (AGENT_WORKING_FLOW_TABLE == uiAgentFlowTableNumber)
        AGENT_WORKING_FLOW_TABLE_UNLOCK();
    
    return AGENT_OK;
}

// �ύ��������, �����������빤����������, ������Ч.
INT32 FlowManager_C::AgentCommitCfgFlowTable()
{
    // ������ȷ�������޸�����ʹ�õĹ�����.
    AGENT_WORKING_FLOW_TABLE_LOCK();
    uiAgentWorkingFlowTable = !uiAgentWorkingFlowTable;
    AGENT_WORKING_FLOW_TABLE_UNLOCK();
    
    return AGENT_OK;
}

// ��AgentFlowTable������Entry
INT32 FlowManager_C::AgentFlowTableAdd
    (UINT32 uiAgentFlowTableNumber,ServerFlowTableEntry_S * pstServerFlowEntry)
{
    INT32 iRet = AGENT_OK;
    UINT32 uiDestPort   = 0;
    UINT32 uiSrcPort    = 0;
    UINT32 uiAgentIndexCounter    = 0;
    
    AgentFlowTableEntry_S stNewAgentEntry;

    // ��ȡ��ǰAgentȫ��Դ�˿ڷ�Χ.
    iRet = pcAgentCfg->GetProtocolUDP(NULL, NULL, &uiDestPort);
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Get Protocol UDP cfg failed[%d]", iRet);
        return AGENT_E_PARA;
    }

    // stNewAgentEntry�а���C++��, ����ֱ��ʹ��sal_memset�����ʼ��. δ�������ع��ɶ���.
    sal_memset(&(stNewAgentEntry.stFlowKey), 0, sizeof(stNewAgentEntry.stFlowKey));
    stNewAgentEntry.uiFlowState = 0;
    
    sal_memset(&(stNewAgentEntry.stFlowDetectResult), 0, sizeof(stNewAgentEntry.stFlowDetectResult));
    stNewAgentEntry.vFlowDetectResultPkt.clear();    
    stNewAgentEntry.uiFlowDropCounter = 0;
    stNewAgentEntry.uiFlowTrackingCounter= 0;
    stNewAgentEntry.uiUrgentFlowCounter= 0;
    

    // ˢ��key��Ϣ
    stNewAgentEntry.stFlowKey.uiUrgentFlow = pstServerFlowEntry->stServerFlowKey.uiUrgentFlow;
    stNewAgentEntry.stFlowKey.eProtocol = pstServerFlowEntry->stServerFlowKey.eProtocol;
    stNewAgentEntry.stFlowKey.uiSrcIP = pstServerFlowEntry->stServerFlowKey.uiSrcIP;
    stNewAgentEntry.stFlowKey.uiDestIP = pstServerFlowEntry->stServerFlowKey.uiDestIP;
    stNewAgentEntry.stFlowKey.uiDestPort = uiDestPort;
    stNewAgentEntry.stFlowKey.uiDscp = pstServerFlowEntry->stServerFlowKey.uiDscp;
    stNewAgentEntry.stFlowKey.stServerTopo = pstServerFlowEntry->stServerFlowKey.stServerTopo;

    if (AGENT_WORKING_FLOW_TABLE == uiAgentFlowTableNumber)
        AGENT_WORKING_FLOW_TABLE_LOCK();

    // ˢ��������Ϣ
    stNewAgentEntry.stFlowKey.uiAgentFlowTableIndex = AgentFlowTable[uiAgentFlowTableNumber].size();    

    // ˢ��Server Entry��Agent����.
    pstServerFlowEntry->uiAgentFlowIndexMin = stNewAgentEntry.stFlowKey.uiAgentFlowTableIndex;
    pstServerFlowEntry->uiAgentFlowWorkingIndexMin = stNewAgentEntry.stFlowKey.uiAgentFlowTableIndex;
    pstServerFlowEntry->uiAgentFlowWorkingIndexMax = pstServerFlowEntry->uiAgentFlowWorkingIndexMin 
                                                + pstServerFlowEntry->stServerFlowKey.uiSrcPortRange - 1;

    for (uiSrcPort = pstServerFlowEntry->stServerFlowKey.uiSrcPortMin; uiSrcPort <= pstServerFlowEntry->stServerFlowKey.uiSrcPortMax; uiSrcPort++)
    {
        stNewAgentEntry.stFlowKey.uiSrcPort = uiSrcPort;

        // Ĭ�ϴ�uiSrcPortRange����
        if (uiAgentIndexCounter < (pstServerFlowEntry->stServerFlowKey.uiSrcPortRange))
        {   
            FLOW_ENTRY_STATE_SET(stNewAgentEntry.uiFlowState, FLOW_ENTRY_STATE_ENABLE);
        }
        else
        {
            FLOW_ENTRY_STATE_CLEAR(stNewAgentEntry.uiFlowState, FLOW_ENTRY_STATE_ENABLE);
        }
        
        AgentFlowTable[uiAgentFlowTableNumber].push_back(stNewAgentEntry);
        stNewAgentEntry.stFlowKey.uiAgentFlowTableIndex ++ ;
        uiAgentIndexCounter ++ ;
    }
    pstServerFlowEntry->uiAgentFlowIndexMax = stNewAgentEntry.stFlowKey.uiAgentFlowTableIndex - 1;
    
    if (AGENT_WORKING_FLOW_TABLE == uiAgentFlowTableNumber)
        AGENT_WORKING_FLOW_TABLE_UNLOCK();
    
    return iRet;
}

// ˢ��Agent����.
INT32 FlowManager_C::AgentRefreshFlowTable()
{
    INT32 iRet = AGENT_OK;
    
    // ���Agent���ñ�
    iRet = AgentClearFlowTable(AGENT_CFG_FLOW_TABLE);
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Clear Cfg Flow Table failed[%d]", iRet);
        return iRet;
    }
    
    // ������ñ�, 
    SERVER_WORKING_FLOW_TABLE_LOCK();
    // ��������ServerFlowTable
    vector<ServerFlowTableEntry_S>::iterator pServerFlowEntry;
    for(pServerFlowEntry = ServerFlowTable[SERVER_WORKING_FLOW_TABLE].begin(); 
        pServerFlowEntry != ServerFlowTable[SERVER_WORKING_FLOW_TABLE].end(); 
        pServerFlowEntry++)
    {
        iRet = AgentFlowTableAdd(AGENT_CFG_FLOW_TABLE, &(*pServerFlowEntry));
        if (iRet)
        {
            FLOW_MANAGER_ERROR("Agent Cfg Flow Table Add failed[%d]", iRet);
            return iRet;
        }
    }
    SERVER_WORKING_FLOW_TABLE_UNLOCK();
        

    // �ύ���ñ�
    iRet = AgentCommitCfgFlowTable();
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Commit Cfg Flow Table failed[%d]", iRet);
        return iRet;
    }
    return iRet;
}

// ����ض�AgentFlow��̽����
INT32 FlowManager_C::AgentFlowTableEntryClearResult(UINT32 uiAgentFlowIndex)
{
    AGENT_WORKING_FLOW_TABLE_LOCK();
    AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowIndex].uiFlowState = 0;
    AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowIndex].vFlowDetectResultPkt.clear();
    AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowIndex].uiFlowDropCounter = 0;
    AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowIndex].uiFlowTrackingCounter = 0;
    AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowIndex].uiUrgentFlowCounter = 0;

    sal_memset(&(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowIndex].stFlowDetectResult), 0, sizeof(DetectResult_S));
    AGENT_WORKING_FLOW_TABLE_UNLOCK();
    return AGENT_OK;
}

// ����range������һ���ϱ����ڴ���Щ��.
INT32 FlowManager_C::AgentFlowTableEntryAdjust()
{

    INT32 iRet = AGENT_OK;
    UINT32 uiAgentFlowIndex = 0;
    UINT32 uiSrcPortRange = 0;

    SERVER_WORKING_FLOW_TABLE_LOCK();
    // ��������ServerFlowTable
    vector<ServerFlowTableEntry_S>::iterator pServerEntry;
    for(pServerEntry = ServerFlowTable[SERVER_WORKING_FLOW_TABLE].begin(); 
        pServerEntry != ServerFlowTable[SERVER_WORKING_FLOW_TABLE].end(); 
        pServerEntry++)
    {
        // UrgentFlow��ˢ��, ̽����һ����Ͳ���̽��.
        if (pServerEntry->stServerFlowKey.uiUrgentFlow)
            continue;
        
        AGENT_WORKING_FLOW_TABLE_LOCK();
        
        // �رձ�ServerFlow��Ӧ�����е�AgentFlow, ������ն�Ӧ��ͳ�ƺ�״̬
        for ( uiAgentFlowIndex = pServerEntry->uiAgentFlowIndexMin;
              uiAgentFlowIndex <= pServerEntry->uiAgentFlowIndexMax;
              uiAgentFlowIndex ++)
        {
            iRet = AgentFlowTableEntryClearResult(uiAgentFlowIndex);
            if (iRet)
            {
                FLOW_MANAGER_ERROR("Clear Agent Flow Table Entry Result failed[%d]", iRet);
            }
        }
              
        // ����range������һ��̽���AgentFlow
        uiSrcPortRange = pServerEntry->stServerFlowKey.uiSrcPortRange;
        if ( pServerEntry->uiAgentFlowWorkingIndexMax + uiSrcPortRange <= pServerEntry->uiAgentFlowIndexMax)
        {
            pServerEntry->uiAgentFlowWorkingIndexMin = pServerEntry->uiAgentFlowWorkingIndexMax + 1;
            pServerEntry->uiAgentFlowWorkingIndexMax = pServerEntry->uiAgentFlowWorkingIndexMax + uiSrcPortRange;

            // ����һ����Ҫ̽���AgentFlow
            for ( uiAgentFlowIndex = pServerEntry->uiAgentFlowWorkingIndexMin;
                  uiAgentFlowIndex <= pServerEntry->uiAgentFlowWorkingIndexMax;
                  uiAgentFlowIndex ++)
            {
                FLOW_ENTRY_STATE_SET(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowIndex].uiFlowState, 
                                            FLOW_ENTRY_STATE_ENABLE);
            }
        }
        else
        {
            if (pServerEntry->uiAgentFlowWorkingIndexMax + 1 <= pServerEntry->uiAgentFlowIndexMax)
            {
                pServerEntry->uiAgentFlowWorkingIndexMin = pServerEntry->uiAgentFlowWorkingIndexMax + 1;
                pServerEntry->uiAgentFlowWorkingIndexMax = pServerEntry->uiAgentFlowIndexMin + 
                    pServerEntry->uiAgentFlowWorkingIndexMax + uiSrcPortRange - pServerEntry->uiAgentFlowIndexMax - 1;

                // ����һ����Ҫ̽���AgentFlow
                for ( uiAgentFlowIndex = pServerEntry->uiAgentFlowWorkingIndexMin;
                      uiAgentFlowIndex <= pServerEntry->uiAgentFlowIndexMax;
                      uiAgentFlowIndex ++)
                {
                    FLOW_ENTRY_STATE_SET(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowIndex].uiFlowState, 
                                                FLOW_ENTRY_STATE_ENABLE);
                }
                for ( uiAgentFlowIndex = pServerEntry->uiAgentFlowIndexMin;
                      uiAgentFlowIndex <= pServerEntry->uiAgentFlowWorkingIndexMax;
                      uiAgentFlowIndex ++)
                {
                    FLOW_ENTRY_STATE_SET(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowIndex].uiFlowState, 
                                                FLOW_ENTRY_STATE_ENABLE);
                }
                
            }
            else
            {
                pServerEntry->uiAgentFlowWorkingIndexMin = pServerEntry->uiAgentFlowIndexMin;
                pServerEntry->uiAgentFlowWorkingIndexMax = pServerEntry->uiAgentFlowIndexMin + uiSrcPortRange - 1;
                // ����һ����Ҫ̽���AgentFlow
                for ( uiAgentFlowIndex = pServerEntry->uiAgentFlowWorkingIndexMin;
                      uiAgentFlowIndex <= pServerEntry->uiAgentFlowWorkingIndexMax;
                      uiAgentFlowIndex ++)
                {
                    FLOW_ENTRY_STATE_SET(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowIndex].uiFlowState, 
                                                FLOW_ENTRY_STATE_ENABLE);
                }
            }
        }
        AGENT_WORKING_FLOW_TABLE_UNLOCK();
    }
    SERVER_WORKING_FLOW_TABLE_UNLOCK();
    
    return AGENT_OK;
}

// Server��������
// ����ض�����
INT32 FlowManager_C::ServerClearFlowTable(UINT32 uiTableNumber)
{
    if (SERVER_WORKING_FLOW_TABLE == uiTableNumber)
        SERVER_WORKING_FLOW_TABLE_LOCK();
        
    // �����������.
    ServerFlowTable[uiTableNumber].clear();

    if (SERVER_WORKING_FLOW_TABLE == uiTableNumber)
        SERVER_WORKING_FLOW_TABLE_UNLOCK();
    
    return AGENT_OK;
}

// �ύ��������, �����������빤����������, ������Ч.
INT32 FlowManager_C::ServerCommitCfgFlowTable()
{
    INT32 iRet = AGENT_OK;
    UINT32 uiAgentFlowIndex = 0;

    SERVER_WORKING_FLOW_TABLE_LOCK();
    // ����Server��������, �ָ���δ��ɵ�Urgent��, ����Urgent����ʧ.
    vector<ServerFlowTableEntry_S>::iterator pServerFlowEntry;
    for(pServerFlowEntry = ServerFlowTable[SERVER_WORKING_FLOW_TABLE].begin(); 
        pServerFlowEntry != ServerFlowTable[SERVER_WORKING_FLOW_TABLE].end(); 
        pServerFlowEntry++)
    {
        if (pServerFlowEntry ->stServerFlowKey.uiUrgentFlow)
        {
            uiAgentFlowIndex = pServerFlowEntry ->uiAgentFlowWorkingIndexMin;
            if(FLOW_ENTRY_STATE_CHECK(
                AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowIndex].uiFlowState, 
                FLOW_ENTRY_STATE_ENABLE))
                
            {
                ServerFlowTable[SERVER_CFG_FLOW_TABLE].push_back(*pServerFlowEntry);
                FLOW_MANAGER_INFO("Recover unfinished urgent flow successfully. DIP[%s]", sal_inet_ntoa(pServerFlowEntry->stServerFlowKey.uiDestIP));
            }
        }
    }
    
    // ����������������������
    uiServerWorkingFlowTable = !uiServerWorkingFlowTable;
    SERVER_WORKING_FLOW_TABLE_UNLOCK();

    // ˢ��Agent����.
    iRet = AgentRefreshFlowTable();
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Refresh Agent Flow Table failed[%d]", iRet);
    }
    
    return iRet;
}

// ��ServerFlowTable������Entryǰ��Ԥ����, ������μ�鼰������ʼ��
INT32 FlowManager_C::ServerFlowTablePreAdd(ServerFlowKey_S * pstNewServerFlowKey, ServerFlowTableEntry_S * pstNewServerFlowEntry)
{
    INT32 iRet = AGENT_OK;
    UINT32 uiSrcPortMin = 0; 
    UINT32 uiSrcPortMax = 0;
    UINT32 uiAgentSrcPortRange = 0;
    
    sal_memset(pstNewServerFlowEntry, 0, sizeof(ServerFlowTableEntry_S));
    
    if (AGENT_DETECT_PROTOCOL_UDP == pstNewServerFlowKey->eProtocol)
    {
        // ��ȡ��ǰAgentȫ��Դ�˿ڷ�Χ.
        iRet = pcAgentCfg->GetProtocolUDP(&uiSrcPortMin, &uiSrcPortMax, NULL);
        if (iRet)
        {
            FLOW_MANAGER_ERROR("Get Protocol UDP cfg failed[%d]", iRet);
            return AGENT_E_PARA;
        }

        // ���Դ�˿ں�Ĭ��ֵ
        
        if (0 == pstNewServerFlowKey->uiSrcPortMin)
        {
            FLOW_MANAGER_INFO("SrcPortMin is 0, Using Default SrcPortMin[%u].", 
                uiSrcPortMin);
            pstNewServerFlowKey->uiSrcPortMin = uiSrcPortMin;            
        }
        
        if (0 == pstNewServerFlowKey->uiSrcPortMax)
        {
            FLOW_MANAGER_INFO("SrcPortMax is 0, Using Default SrcPortMin[%u].", 
                uiSrcPortMax);
            pstNewServerFlowKey->uiSrcPortMax = uiSrcPortMax;            
        }

        uiAgentSrcPortRange = pstNewServerFlowKey->uiSrcPortMax - pstNewServerFlowKey->uiSrcPortMin + 1;
        
        if (0 == pstNewServerFlowKey->uiSrcPortRange)
        {
            FLOW_MANAGER_INFO("SrcPortRange is 0, Using Default SrcPortMin[%u]: [%u]-[%u].", 
                uiAgentSrcPortRange, pstNewServerFlowKey->uiSrcPortMin, pstNewServerFlowKey->uiSrcPortMax);
            pstNewServerFlowKey->uiSrcPortRange = uiAgentSrcPortRange;
        }

        // ��μ��
        if (pstNewServerFlowKey->uiSrcPortMin > pstNewServerFlowKey->uiSrcPortMax)
            
        {
            FLOW_MANAGER_ERROR("SrcPortMin[%u] is bigger than SrcPortMax[%u]", 
                pstNewServerFlowKey->uiSrcPortMin, pstNewServerFlowKey->uiSrcPortMax);
            return AGENT_E_PARA;
        }

        if (  (uiSrcPortMin > pstNewServerFlowKey->uiSrcPortMin)
            ||(uiSrcPortMax < pstNewServerFlowKey->uiSrcPortMax))
        {
            FLOW_MANAGER_ERROR("SrcPortMin[%u] - SrcPortMax[%u] from server is too larger than this agent: SrcPortMin[%u] - SrcPortMax[%u]", 
                pstNewServerFlowKey->uiSrcPortMin, pstNewServerFlowKey->uiSrcPortMax, uiSrcPortMin, uiSrcPortMax);
            return AGENT_E_PARA;
        }
        // ���Range��Χ
        if ( uiAgentSrcPortRange < pstNewServerFlowKey->uiSrcPortRange )
        {
            FLOW_MANAGER_ERROR("SrcPortRange[%u] from server is too larger than Flow range [%u]: SrcPortMin[%u] - SrcPortMax[%u]", 
                pstNewServerFlowKey->uiSrcPortRange, uiAgentSrcPortRange, pstNewServerFlowKey->uiSrcPortMin, pstNewServerFlowKey->uiSrcPortMax);
            return AGENT_E_PARA;
        }

        // ���dscp�Ƿ�Ϸ�
        if (pstNewServerFlowKey->uiDscp > AGENT_MAX_DSCP_VALUE)  
        {
            FLOW_MANAGER_ERROR("Dscp[%d] is bigger than the max value[%d]", pstNewServerFlowKey->uiDscp, AGENT_MAX_DSCP_VALUE); 
            return AGENT_E_PARA;
        }

        if (pstNewServerFlowKey->uiDscp == pcAgentCfg->GetDetectTrackingDscp())
        {
            FLOW_MANAGER_ERROR("Dscp[%d] is same with the reserved tracking dscp value[%d]", pstNewServerFlowKey->uiDscp, pcAgentCfg->GetDetectTrackingDscp()); 
            return AGENT_E_PARA;
        }
        
        pstNewServerFlowEntry->stServerFlowKey = * pstNewServerFlowKey;
    
    }
    else
    {
        FLOW_MANAGER_ERROR("Unsupported Protocol[%d]", pstNewServerFlowKey->eProtocol);
        iRet = AGENT_E_PARA;
    }
    return iRet;
}

// ��ServerCfgFlowTable������Entry, ��DoQuery()����. һ���������� ��Urgent Entry
INT32 FlowManager_C::ServerCfgFlowTableAdd(ServerFlowKey_S stNewServerFlowKey)
{
    INT32 iRet = AGENT_OK;
    
    ServerFlowTableEntry_S stNewServerFlowEntry;
    
    iRet = ServerFlowTablePreAdd(&stNewServerFlowKey, &stNewServerFlowEntry);
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Server Flow Table Pre Add failed[%d]", iRet);
        return iRet;
    }

    // ������ñ����Ƿ����ظ�����, Urgent�����ظ�
    vector<ServerFlowTableEntry_S>::iterator pServerFlowEntry;
    for(pServerFlowEntry = ServerFlowTable[SERVER_CFG_FLOW_TABLE].begin(); 
        pServerFlowEntry != ServerFlowTable[SERVER_CFG_FLOW_TABLE].end(); 
        pServerFlowEntry++)
    {
        if (  (stNewServerFlowEntry.stServerFlowKey == pServerFlowEntry->stServerFlowKey)
            &&(!stNewServerFlowEntry.stServerFlowKey.uiUrgentFlow))
        {
            FLOW_MANAGER_WARNING("This flow already exist");
            return AGENT_OK;
        }
    }
    
    ServerFlowTable[SERVER_CFG_FLOW_TABLE].push_back(stNewServerFlowEntry);

    return iRet;
}

// ��ServerWorkingFlowTable������Entry, ��Server�·���Ϣ����. һ����������Urgent Entry
INT32 FlowManager_C::ServerWorkingFlowTableAdd(ServerFlowKey_S stNewServerFlowKey)
{
    INT32 iRet = AGENT_OK;
    
    ServerFlowTableEntry_S stNewServerFlowEntry;

    FLOW_MANAGER_INFO("Adding Server Working Flow Now");

    iRet = ServerFlowTablePreAdd(&stNewServerFlowKey, &stNewServerFlowEntry);
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Server Flow Table Pre Add failed[%d]", iRet);
        return iRet;
    }

    SERVER_WORKING_FLOW_TABLE_LOCK();
    // ��鹤�������Ƿ����ظ�����, Urgent�����ظ�
    vector<ServerFlowTableEntry_S>::iterator pServerFlowEntry;
    for(pServerFlowEntry = ServerFlowTable[SERVER_WORKING_FLOW_TABLE].begin(); 
        pServerFlowEntry != ServerFlowTable[SERVER_WORKING_FLOW_TABLE].end(); 
        pServerFlowEntry++)
    {
        if (  (stNewServerFlowEntry.stServerFlowKey == pServerFlowEntry->stServerFlowKey)
            &&(!stNewServerFlowEntry.stServerFlowKey.uiUrgentFlow))
        {
            FLOW_MANAGER_WARNING("This flow already exist");
            return AGENT_OK;
        }
    }

    // ��agent��������������ʱ����,�˲����Ὰ��agent flow ��.
    iRet = AgentFlowTableAdd(AGENT_WORKING_FLOW_TABLE, &stNewServerFlowEntry);
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Agent Working Flow Table Add failed[%d]", iRet);
        return iRet;
    }

    // ServerTable������һ�ݼ�¼, ����query���ڵ���ˢ��agent��ʱ����δ���̽���Urgent�����.
    ServerFlowTable[SERVER_WORKING_FLOW_TABLE].push_back(stNewServerFlowEntry);
    SERVER_WORKING_FLOW_TABLE_UNLOCK();

    // ServerFlow ����Ϊ��
    if ( AGENT_TRUE == uiServerFlowTableIsEmpty )
        uiServerFlowTableIsEmpty = AGENT_FALSE;

    return iRet;
}

// ����ʱ�Ƿ������̽������.
INT32 FlowManager_C::DetectCheck(UINT32 uiCounter)
{
     UINT32 uiTimeCost = 0 ;

    // uiCounter �������
    uiTimeCost = (uiCounter >= uiLastCheckTimeCounter) 
                ? uiCounter - uiLastCheckTimeCounter
                : uiCounter + ((UINT32)(-1) - uiLastCheckTimeCounter + 1);

    if ( uiTimeCost >= pcAgentCfg->GetDetectPeriod())
    {
        uiLastCheckTimeCounter = uiCounter;
        uiNeedCheckResult = AGENT_ENABLE;
        return AGENT_ENABLE;
    }
    else
        return AGENT_DISABLE;
}

// ������̽��.
INT32 FlowManager_C::DoDetect()
{
    INT32 iRet = AGENT_OK;
    UINT32 uiSocketOffset = 0;
    UINT32 uiSocketBase = 0;
    UINT32 uiSocketSize = 0;
    FlowKey_S stFlowKey;
    
    //FLOW_MANAGER_INFO("Start UDP Detect Now");

    iRet = pcAgentCfg->GetProtocolUDP(&uiSocketBase, NULL, NULL);
    if (iRet || (0 == uiSocketBase))
    {
        FLOW_MANAGER_ERROR("Get UDP Protocol Info Failed[%d], SrcPortMin[%d]", iRet, uiSocketBase);
        return AGENT_E_ERROR;
    }

    AGENT_WORKING_FLOW_TABLE_LOCK();
    
    // ��ǰֻ����udpЭ��, ��ȡudp worker list��С
    uiSocketSize = WorkerList_UDP.size();
    vector<AgentFlowTableEntry_S>::iterator pAgentFlowEntry;    
    for(pAgentFlowEntry = AgentFlowTable[AGENT_WORKING_FLOW_TABLE].begin(); 
        pAgentFlowEntry != AgentFlowTable[AGENT_WORKING_FLOW_TABLE].end(); 
        pAgentFlowEntry++)
    {
        // ��ǰֻ����udpЭ��
        if (AGENT_DETECT_PROTOCOL_UDP == pAgentFlowEntry->stFlowKey.eProtocol)
        {
            // ֻ����enable��Entry
            if (FLOW_ENTRY_STATE_CHECK(pAgentFlowEntry->uiFlowState, FLOW_ENTRY_STATE_ENABLE))
            {
                // ����socketƫ����
                uiSocketOffset = pAgentFlowEntry->stFlowKey.uiSrcPort - uiSocketBase;
                // ���Խ��,������ڴ�.
                if (uiSocketOffset < uiSocketSize)
                {
                    stFlowKey = pAgentFlowEntry->stFlowKey;
                    if (FLOW_ENTRY_STATE_CHECK(pAgentFlowEntry->uiFlowState, FLOW_ENTRY_STATE_TRACKING))
                    {
                        // ����׷�ٺ�, ���̷���һ��׷�ٱ���, ֮��ÿ��pcAgentCfg->GetDetectTrackingThresh()��̽�����ڷ���һ��׷�ٱ���.
                        if (0 == pAgentFlowEntry->uiFlowTrackingCounter)
                            stFlowKey.uiDscp = pcAgentCfg->GetDetectTrackingDscp();
                        
                        // ����׷�ٱ��ķ��ͼ��.
                        if (pcAgentCfg->GetDetectTrackingThresh() <= pAgentFlowEntry->uiFlowTrackingCounter)
                            pAgentFlowEntry->uiFlowTrackingCounter = 0;
                        else
                            pAgentFlowEntry->uiFlowTrackingCounter ++ ;
                    }

                    
                    iRet = WorkerList_UDP[uiSocketOffset]->PushSession(stFlowKey);
                    if (iRet && AGENT_E_SOCKET != iRet)
                    {
                        FLOW_MANAGER_ERROR("Push Session to Worker Failed[%d], SrcPort[%u]", iRet, pAgentFlowEntry->stFlowKey.uiSrcPort);
                        continue;
                    }
                }
                else
                {
                    FLOW_MANAGER_ERROR("UDP SocketOffset[%u] is over udp worker list size[%u], SrcPort[%u],SocketBase[%u].", 
                        uiSocketOffset, uiSocketSize, pAgentFlowEntry->stFlowKey.uiSrcPort, uiSocketBase);
                    continue;
                }
            }
        }
    }
    AGENT_WORKING_FLOW_TABLE_UNLOCK();

    if (AGENT_E_SOCKET == iRet)
        iRet = AGENT_OK;

    //FLOW_MANAGER_INFO("UDP Detect Finished");
    return iRet;
}

// ����Ǵ�ʱ��ü��̽����. ̽������ʱ��+timeoutʱ��.
INT32 FlowManager_C::DetectResultCheck(UINT32 uiCounter)
{
    UINT32 uiTimeCost = 0 ;

    // uiCounter �������
    uiTimeCost = (uiCounter >= uiLastCheckTimeCounter) 
                ? uiCounter - uiLastCheckTimeCounter
                : uiCounter + ((UINT32)(-1) - uiLastCheckTimeCounter + 1);
    
    if ( AGENT_ENABLE == uiNeedCheckResult
        &&(uiTimeCost >= pcAgentCfg->GetDetectTimeout()))
    {
        uiNeedCheckResult = AGENT_FALSE;
        return AGENT_ENABLE;
    }
    else
        return AGENT_DISABLE;
}

// �����׼��
INT32 FlowManager_C::FlowComputeSD(
        INT64 * plSampleData,
        UINT32 uiSampleNumber,
        INT64 lSampleMeanValue,
        INT64 * plStandardDeviation)
{
    UINT32 uiSampleIndex = 0;
    INT64 lVariance = 0;  //����
    
    if ((NULL == plSampleData) || (NULL == plStandardDeviation))
    {
        FLOW_MANAGER_ERROR("NULL Pointer. plSampleData[%u], plStandardDeviation[%u]", plSampleData, plStandardDeviation);
        return AGENT_E_PARA;
    }
    if (0 == uiSampleNumber)
    {
        FLOW_MANAGER_ERROR("None Sample[%u]", uiSampleNumber);
        return AGENT_E_PARA;
    }
    
    *plStandardDeviation = 0;

    for (uiSampleIndex = 0; uiSampleIndex < uiSampleNumber; uiSampleIndex ++)
    {
        lVariance += pow((plSampleData[uiSampleIndex] - lSampleMeanValue), 2);
    }
    lVariance = lVariance/uiSampleNumber; // ����
    *plStandardDeviation = sqrt(lVariance); // ��׼��
    
    return AGENT_OK;
}
      
// ����ͳ������,׼���ϱ�
INT32 FlowManager_C::FlowPrepareReport(UINT32 uiFlowTableIndex)
{
    INT32 iRet = AGENT_OK;

    // ̽����������.
    UINT32 uiResultNumber = 0;

    // ��Ч��������.
    UINT32 uiSampleNumber = 0;
    
    INT64 lDataTemp = 0;         // ��������.
    INT64 * plT3Temp = NULL;     // ʱ�� us, Targetƽ������ʱ��(stT3 - stT2)
    INT64 lT3Sum = 0;    
    INT64 * plT4Temp = NULL;     // ʱ�� us, Sender����ƽ������ʱ��(stT4 - stT1), RTT
    INT64 lT4Sum = 0;
    INT64 lStandardDeviation = 0;// ʱ�ӱ�׼��


    AGENT_WORKING_FLOW_TABLE_LOCK();
    // ̽����������.
    uiResultNumber = AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].vFlowDetectResultPkt.size();
    
    // �����������ʱ��
    {
        struct timeval tm;
        sal_memset(&tm, 0, sizeof(tm));
        gettimeofday(&tm,NULL); // ��ȡ��ǰʱ��
        // ��msΪ��λ�����ϱ�.
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lT5 = (INT64)tm.tv_sec * SECOND_MSEC 
                                                                                            + (INT64)tm.tv_usec / MILLISECOND_USEC;
    }
    
    // û��̽������
    if (0 == uiResultNumber)
    {   
        AGENT_WORKING_FLOW_TABLE_UNLOCK();
        return AGENT_OK;
    }

    plT3Temp = new INT64[uiResultNumber];
    if (NULL == plT3Temp)
    {
        AGENT_WORKING_FLOW_TABLE_UNLOCK();
        
        FLOW_MANAGER_ERROR("No enough memory.[%u]", uiResultNumber);
        return AGENT_E_MEMORY;
    }
    
    plT4Temp = new INT64[uiResultNumber];
    if (NULL == plT4Temp)
    {
        delete [] plT3Temp;
        plT3Temp = NULL;
        AGENT_WORKING_FLOW_TABLE_UNLOCK();
        
        FLOW_MANAGER_ERROR("No enough memory.[%u]", uiResultNumber);
        return AGENT_E_MEMORY;
    }

    sal_memset(plT3Temp, 0, sizeof(INT64)*uiResultNumber);
    sal_memset(plT4Temp, 0, sizeof(INT64)*uiResultNumber);

    // ����ʱ��, �޳�û���յ�Ӧ�������
    vector<DetectResultPkt_S>::iterator pDetectResultPkt;
    for(pDetectResultPkt = AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].vFlowDetectResultPkt.begin(); 
        pDetectResultPkt != AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].vFlowDetectResultPkt.end(); 
        pDetectResultPkt ++)
    {
        if ( SESSION_STATE_WAITING_CHECK == pDetectResultPkt->uiSessionState )
        {
            // С��0��ʱ�Ӳ�����, ����os�ڵ���ϵͳʱ��, ���Ը��쳣����.
            // ����ʱ����us��, ʹ��us���м���.
            lDataTemp = pDetectResultPkt->stT3 - pDetectResultPkt->stT2;
            if (0 <= lDataTemp)
            {
                plT3Temp[uiSampleNumber] = lDataTemp; //us
                lT3Sum += lDataTemp;
            }
            
            lDataTemp = pDetectResultPkt->stT4 - pDetectResultPkt->stT1;
            if (0 <= lDataTemp)
            {
                plT4Temp[uiSampleNumber] = lDataTemp; //us
                lT4Sum += lDataTemp;
            }
                
            //FLOW_MANAGER_INFO("Target Latency[%u]us, RTT[%u]us, uiDataIndex[%u]", plT3Temp[uiSampleNumber], plT4Temp[uiSampleNumber], uiSampleNumber);
            uiSampleNumber ++;
        }
    }
    // û���յ�һ����ЧӦ����, û����Чʱ������.
    if (0 == uiSampleNumber)
    {
        delete [] plT3Temp;
        plT3Temp = NULL;    
        delete [] plT4Temp;
        plT4Temp = NULL;

        // ����ʱ����ϢΪ-1.
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lT2 = -1;
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lT3 = -1;
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lT4 = -1;
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lLatencyMin = -1;
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lLatencyMax = -1;
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lLatency50Percentile = -1;
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lLatency99Percentile = -1;
        
        AGENT_WORKING_FLOW_TABLE_UNLOCK();
        return AGENT_OK;
    }
    // ��plT3Temp(Target) ����ƽ��ֵ
    AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lT3 = lT3Sum / uiSampleNumber;
    
    
    // ��plT4Temp(rtt) ����ƽ��ֵ.
    lDataTemp = lT4Sum / uiSampleNumber;
    AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lT4 = lDataTemp;


    // ��plT4Temp(rtt) �����׼��
    iRet = FlowComputeSD(plT4Temp, uiSampleNumber, lDataTemp, &lStandardDeviation);
    if (iRet)
    {
        delete [] plT3Temp;
        plT3Temp = NULL;    
        delete [] plT4Temp;
        plT4Temp = NULL;
        
        AGENT_WORKING_FLOW_TABLE_UNLOCK();

        FLOW_MANAGER_ERROR("Flow Sort Result failed[%d]", iRet);
        return iRet;
    }
    AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lLatencyStandardDeviation = lStandardDeviation;

    // ��plT4Temp(rtt) ���д�С��������
    sort(plT4Temp, plT4Temp + uiSampleNumber);

    // ��ȡplT4Temp(rtt)��Сֵ
    lDataTemp = 0;
    AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lLatencyMin = plT4Temp[lDataTemp];
    
    // ��ȡplT4Temp(rtt)���ֵ
    // uiSampleNumber �� 0
    lDataTemp = uiSampleNumber - 1;
    AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lLatencyMax = plT4Temp[lDataTemp];
    
    // ��ȡplT4Temp(rtt)��λ��
    if( 2 <= uiSampleNumber)
        lDataTemp = uiSampleNumber/2 - 1;
    else
        lDataTemp = 0;
    AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lLatency50Percentile = plT4Temp[lDataTemp];
    
    // ��ȡplT4Temp(rtt)99%λ��
    if( 2 <= uiSampleNumber)
        lDataTemp = uiSampleNumber*99/100 - 1;
    else
        lDataTemp = 0;
    AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lLatency99Percentile = plT4Temp[lDataTemp];

    
    
    delete [] plT3Temp;
    plT3Temp = NULL;    
    delete [] plT4Temp;
    plT4Temp = NULL;
    
    AGENT_WORKING_FLOW_TABLE_UNLOCK();
    return AGENT_OK;
}

// ͳһ�ϱ��ӿ�
INT32 FlowManager_C::FlowReportData(string * pstrReportData)
{
    INT32 iRet = AGENT_OK;
    CollectorProtocolType_E eConnectorProtocol = COLLECTOR_PROTOCOL_NULL;


    // collectorʹ�õ�sparc �� "\n" �����⺬��, �ַ����г���β�Ļ��з�ȫ��Ҫ�滻��.
    string strOld = "\n";
    string strNew = " ";
    sal_string_replace(pstrReportData, &strOld, &strNew);
    //strReportData +="\n";

    // FLOW_MANAGER_INFO("Report Flow info:[%u][%s]", pstrReportData->size(), pstrReportData->c_str());
    
    // ���Agent��Collector֮���ͨѶ��ʽ
    iRet = pcAgentCfg->GetCollectorProtocol(&eConnectorProtocol);
    if(iRet)
    {
        FLOW_MANAGER_ERROR("Get Collector Protocol Info failed[%d]", iRet);
        return AGENT_E_PARA;
    }
    // �Ƿ�ʹ��kafkaͨѶ
    if(COLLECTOR_PROTOCOL_KAFKA == eConnectorProtocol)
    {
        KafkaConnectInfo_S stKafkaConnectInfo;
        
        // ��ѯKafka������Ϣ
        iRet = pcAgentCfg->GetCollectorKafkaInfo(&stKafkaConnectInfo);
        if(iRet)
        {
            FLOW_MANAGER_ERROR("Get Collector Kafka Info failed[%d]", iRet);
            return AGENT_E_PARA;
        }
        
        // ʹ��kafka������Ϣ
        iRet = pcKafkaClient->ProduceKafkaMsg(&(stKafkaConnectInfo.strTopic), pstrReportData);
        if (iRet)
        {
            FLOW_MANAGER_ERROR("Produce kafka msg in topic[%s] failed[%d]", stKafkaConnectInfo.strTopic.c_str(), iRet);
            return iRet;
        }
    }
    else
    {
        FLOW_MANAGER_ERROR("unknown CollectorProtocol [%d]", eConnectorProtocol);
        return AGENT_E_PARA;
    }
    return AGENT_OK;
}

// �����ϱ��ӿ�
INT32 FlowManager_C::FlowDropReport(UINT32 uiFlowTableIndex)
{
    INT32 iRet = AGENT_OK;
    AgentFlowTableEntry_S * pstAgentFlowEntry = NULL;
    stringstream  ssReportData; // ��������json��ʽ�ϱ�����
    string strReportData;       // ���ڻ��������ϱ����ַ�����Ϣ
    string strTopic;            // ͨ��kafka�ϱ�������Ҫ�ṩtopic��Ϣ
    
    iRet = FlowPrepareReport(uiFlowTableIndex);
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Flow Prepare Report failed[%d]", iRet);
        return iRet;
    }

    ssReportData.clear();
    ssReportData.str("");
    
    AGENT_WORKING_FLOW_TABLE_LOCK();
    pstAgentFlowEntry = &(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex]);
    pstAgentFlowEntry->stFlowDetectResult.lDropNotesCounter ++;
    iRet = CreatDropReportData(pstAgentFlowEntry, &ssReportData);
    AGENT_WORKING_FLOW_TABLE_UNLOCK();

    if (iRet)
    {
        FLOW_MANAGER_ERROR("Creat Drop Report Data failed[%d]", iRet);
        return iRet;
    }
    strReportData = ssReportData.str();


    iRet = FlowReportData(&strReportData);
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Flow Report Data failed[%d]", iRet);
        return iRet;
    }
    
#if 0    
    AGENT_WORKING_FLOW_TABLE_LOCK();
    FlowKey_S   stFlowKey = AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowKey;
    DetectResult_S stFlowDetectResult = AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult;
    AGENT_WORKING_FLOW_TABLE_UNLOCK();
        
    FLOW_MANAGER_INFO("Flow info: sip[%s], sport[%u], dscp[%d], Urgent[%d], AgentFlowIndex[%u]",
            sal_inet_ntoa(stFlowKey.uiSrcIP), stFlowKey.uiSrcPort, stFlowKey.uiDscp, stFlowKey.uiUrgentFlow, stFlowKey.uiAgentFlowTableIndex);
    
    FLOW_MANAGER_INFO("           dip[%s], dport[%u]",
            sal_inet_ntoa(stFlowKey.uiDestIP), stFlowKey.uiDestPort);
    
    FLOW_MANAGER_INFO("Drop info: PktSent[%u], PktDrop[%u], ReportTime[%u]",
            stFlowDetectResult.lPktSentCounter, stFlowDetectResult.lPktDropCounter, stFlowDetectResult.lT5);
#endif    
    return AGENT_OK;
}

// ��ʱ�ϱ��ӿ�
INT32 FlowManager_C::FlowLatencyReport(UINT32 uiFlowTableIndex)
{
    INT32 iRet = AGENT_OK;
    AgentFlowTableEntry_S * pstAgentFlowEntry = NULL;
    stringstream  ssReportData; // ��������json��ʽ�ϱ�����
    string strReportData;       // ���ڻ��������ϱ����ַ�����Ϣ
    string strTopic;            // ͨ��kafka�ϱ�������Ҫ�ṩtopic��Ϣ
    
    iRet = FlowPrepareReport(uiFlowTableIndex);
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Flow Prepare Report failed[%d]", iRet);
        return iRet;
    }
    
    ssReportData.clear();
    ssReportData.str("");
    
    AGENT_WORKING_FLOW_TABLE_LOCK();
    pstAgentFlowEntry = &(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex]);
    iRet = CreatLatencyReportData(pstAgentFlowEntry, &ssReportData);
    AGENT_WORKING_FLOW_TABLE_UNLOCK();

    if (iRet)
    {
        FLOW_MANAGER_ERROR("Creat Latency Report Data[%d]", iRet);
        return iRet;
    }
    strReportData = ssReportData.str();

// ���ֱ��Ķ���,��û��ȫ�������ĳ�����¼һ��, ����ʹ��.
#if 0
    if (   pstAgentFlowEntry->stFlowDetectResult.lPktDropCounter 
        && (pstAgentFlowEntry->stFlowDetectResult.lPktDropCounter < pstAgentFlowEntry->stFlowDetectResult.lPktSentCounter))
    {
        FLOW_MANAGER_INFO("Flow info:[%s]", strReportData.c_str());
    }
#endif

    iRet = FlowReportData(&strReportData);
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Flow Report Data failed[%d]", iRet);
        return iRet;
    }
    
    return AGENT_OK;
}

// ��������, �������������ϱ�,ͬʱ����׷�ٱ���.
INT32 FlowManager_C::FlowDropNotice(UINT32 uiFlowTableIndex)
{
    INT32 iRet = AGENT_OK;
    AGENT_WORKING_FLOW_TABLE_LOCK();
    
    //FLOW_MANAGER_INFO("Flow Enter Drop State");

    FLOW_ENTRY_STATE_SET(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].uiFlowState, FLOW_ENTRY_STATE_DROPPING);
    
    FLOW_ENTRY_STATE_SET(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].uiFlowState, FLOW_ENTRY_STATE_TRACKING);
    AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].uiFlowTrackingCounter = 0;

    iRet = FlowDropReport(uiFlowTableIndex);
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Flow Drop Report failed[%d]", iRet);
    }
    
    AGENT_WORKING_FLOW_TABLE_UNLOCK();

    return iRet;
}

// Urgent��̽�����,���������ϱ�.
INT32 FlowManager_C::FlowUrgentNotice(UINT32 uiFlowTableIndex)
{
    INT32 iRet = AGENT_OK;
    
    // ���̽��, �رո���
    AGENT_WORKING_FLOW_TABLE_LOCK();
    FLOW_ENTRY_STATE_CLEAR(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].uiFlowState, FLOW_ENTRY_STATE_ENABLE);

    //FLOW_MANAGER_INFO("Start Urgent Flow Notice");
    
    iRet = FlowLatencyReport(uiFlowTableIndex);
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Flow Latency Report failed[%d]", iRet);
    }
    
    AGENT_WORKING_FLOW_TABLE_UNLOCK();
    return iRet;
}

// ÿһ��̽����ɺ�ĺ�������.
INT32 FlowManager_C::DetectResultProcess(UINT32 uiFlowTableIndex)
{
    INT32 iRet = AGENT_OK;
    DetectResultPkt_S stDetectResultPkt;
    UINT32 uiUrgentFlow = 0;
    
    AGENT_WORKING_FLOW_TABLE_LOCK();

    // �Ƿ�Urgent̽����.
    uiUrgentFlow = AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowKey.uiUrgentFlow;
    // ��ȡ�ո�ѹ�������̽����.
    stDetectResultPkt = AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].vFlowDetectResultPkt.back();

    // ����ǵ�һ��̽�ⱨ��, ˢ��̽��ʱ��.
    // ����һ�����ķ���ʧ��,��lT1=0.
    // ����һ�����ķ��ͳɹ�,���Ƕ���. ��lT1Ϊ����ʱ��, lT2=0.
    if (1 == AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].vFlowDetectResultPkt.size())
    {
        //AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lT1 = stDetectResultPkt.stT1.uiSec;
        //AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lT2 = stDetectResultPkt.stT2.uiSec;

        // ʱ�����msΪ��λ�����ϱ�.
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lT1 = (INT64)stDetectResultPkt.stT1.uiSec * SECOND_MSEC 
                                                                                            + (INT64)stDetectResultPkt.stT1.uiUsec / MILLISECOND_USEC;
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lT2 = (INT64)stDetectResultPkt.stT2.uiSec * SECOND_MSEC 
                                                                                            + (INT64)stDetectResultPkt.stT2.uiUsec / MILLISECOND_USEC;
    }
    
    // ˢ������ͳ����Ϣ
    // ���ͳɹ�
    if ( SESSION_STATE_SEND_FAIELD != stDetectResultPkt.uiSessionState)
    {
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lPktSentCounter ++;
    }

    // ����
    if ( SESSION_STATE_TIMEOUT == stDetectResultPkt.uiSessionState )
    {
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowDetectResult.lPktDropCounter ++;
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].uiFlowDropCounter ++;
        /*
        // ����ʹ��
        FLOW_MANAGER_WARNING("Packet Timeout, T1[%u][%u], T4[%u][%u], Latency[%u]us, index[%u]", 
            stDetectResultPkt.stT1.uiSec,stDetectResultPkt.stT1.uiUsec, stDetectResultPkt.stT4.uiSec,stDetectResultPkt.stT4.uiUsec,
            stDetectResultPkt.stT4 - stDetectResultPkt.stT1, uiFlowTableIndex);
        */
    }
    else if ( SESSION_STATE_WAITING_CHECK == stDetectResultPkt.uiSessionState ) //δ����
    {
        // ȡ������״̬.
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].uiFlowDropCounter = 0;
        FLOW_ENTRY_STATE_CLEAR(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].uiFlowState, FLOW_ENTRY_STATE_DROPPING);
        // ȡ��׷��״̬
        FLOW_ENTRY_STATE_CLEAR(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].uiFlowState, FLOW_ENTRY_STATE_TRACKING);
    }

    // ˢ��UrgentFlowCounter
    if (AGENT_TRUE == uiUrgentFlow)
    {
        AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].uiUrgentFlowCounter ++ ;
    }

    // ���ٴ����¼�

    // ��ͨ����������, �������������ϱ�,ͬʱ����׷�ٱ���.
    if ( !(FLOW_ENTRY_STATE_CHECK(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].uiFlowState, FLOW_ENTRY_STATE_DROPPING))
         && (pcAgentCfg->GetDetectDropThresh() <= AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].uiFlowDropCounter)
         && (AGENT_FALSE == uiUrgentFlow))
    {
        // ���붪��״̬
        iRet = FlowDropNotice(uiFlowTableIndex);
        if (iRet)
        {
            FLOW_MANAGER_ERROR("FlowDropNotice failed[%d], index[%d]", iRet, uiFlowTableIndex);
        }
    }
    // Urgent��̽�����,���������ϱ�.
    if (  pcAgentCfg->GetDetectUrgentThresh()<= AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].uiUrgentFlowCounter )
    {
        iRet = FlowUrgentNotice(uiFlowTableIndex);
        if (iRet)
        {
            FLOW_MANAGER_ERROR("FlowDropNotice failed[%d], index[%d]", iRet, uiFlowTableIndex);
        }
    }
    
    AGENT_WORKING_FLOW_TABLE_UNLOCK();
}
    
// �����ռ���̽����.
INT32 FlowManager_C::GetDetectResult()
{
    INT32 iRet = AGENT_OK;
    UINT32 uiAgentFlowTableSize = 0;
    UINT32 uiAgentFlowTableIndex = 0;
    DetectWorkerSession_S stWorkerSession;
    DetectResultPkt_S   stDetectResultPkt;
    //FLOW_MANAGER_INFO("Start Collect Result Now");

    // ̽�������д����.
    AGENT_WORKING_FLOW_TABLE_LOCK();

    uiAgentFlowTableSize = AgentFlowTable[AGENT_WORKING_FLOW_TABLE].size();
    
    // ��ǰֻ����udpЭ��, ���� udp worker list
    vector<DetectWorker_C *>::iterator pWorker;
    for(pWorker = WorkerList_UDP.begin(); pWorker != WorkerList_UDP.end(); pWorker++)
    {
        // ������worker�����д��ռ��ĻỰ.
        do
        {
            sal_memset(&stWorkerSession, 0, sizeof(stWorkerSession));
            
            iRet = (*pWorker)->PopSession(&stWorkerSession);
            if( iRet && (AGENT_E_NOT_FOUND != iRet))
            {
                FLOW_MANAGER_ERROR("Worker PopSession Failed[%d]", iRet);
                continue;
            }
            else if (AGENT_E_NOT_FOUND == iRet)
            {
                break;
            }
            uiAgentFlowTableIndex = stWorkerSession.stFlowKey.uiAgentFlowTableIndex;

            // ���FlowTableIndex�Ƿ���Ч
            if (uiAgentFlowTableIndex < uiAgentFlowTableSize)
            {
                sal_memset(&stDetectResultPkt, 0, sizeof(stDetectResultPkt));
                stDetectResultPkt.uiSessionState = stWorkerSession.uiSessionState;
                stDetectResultPkt.uiSequenceNumber = stWorkerSession.uiSequenceNumber;
                stDetectResultPkt.stT1 = stWorkerSession.stT1;
                stDetectResultPkt.stT2 = stWorkerSession.stT2;
                stDetectResultPkt.stT3 = stWorkerSession.stT3;
                stDetectResultPkt.stT4 = stWorkerSession.stT4;

                // У��stFlowKey, ׷�ٱ��Ļ�ʹ�������dscp.
                if ( stWorkerSession.stFlowKey != AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowTableIndex].stFlowKey
                    && (stWorkerSession.stFlowKey.uiDscp != pcAgentCfg->GetDetectTrackingDscp()))
                {
                    FLOW_MANAGER_INFO("Worker Session mismatch with flow table index[%u]. Maybe DoQuery just clear the agent flow table", 
                        uiAgentFlowTableIndex);
                    continue;
                }
                if (0 == FLOW_ENTRY_STATE_CHECK(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowTableIndex].uiFlowState, FLOW_ENTRY_STATE_ENABLE))
                {
                    //FLOW_MANAGER_INFO("Index[%u] already disabled, reply too late", uiAgentFlowTableIndex);
                    continue;
                }
                // ������д��̽����
                AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiAgentFlowTableIndex].vFlowDetectResultPkt.push_back(stDetectResultPkt);

                // ��Ըռ����̽�������д���, ���ж���,Urgent���¼�����.
                iRet = DetectResultProcess(uiAgentFlowTableIndex);
                if (iRet)
                {
                    FLOW_MANAGER_WARNING("DetectResultProcess failed [%d]", iRet);
                    continue;
                }
            }
            else
            {
                FLOW_MANAGER_INFO("FlowTableIndex[%u] is over size[%d]. Maybe DoQuery just clear the agent flow table", 
                    uiAgentFlowTableIndex, uiAgentFlowTableSize);
                continue;
            }
            
        }while( AGENT_E_NOT_FOUND != iRet );
    }
    AGENT_WORKING_FLOW_TABLE_UNLOCK();
    
    return AGENT_OK;
}

// ����ʱ�Ƿ�������ϱ�Collector����.
INT32 FlowManager_C::ReportCheck(UINT32 uiCounter)
{
     UINT32 uiTimeCost = 0 ;

    // uiCounter �������
    uiTimeCost = (uiCounter >= uiLastReportTimeCounter) 
                ? uiCounter - uiLastReportTimeCounter
                : uiCounter + ((UINT32)(-1) - uiLastReportTimeCounter + 1);

    if ( uiTimeCost >= pcAgentCfg->GetReportPeriod())
    {
        uiLastReportTimeCounter = uiCounter;
        return AGENT_ENABLE;
    }
    else
        return AGENT_DISABLE;
}
    
// ������̽�����ϱ�.
INT32 FlowManager_C::DoReport()
{
    INT32 iRet = AGENT_OK;
    UINT32 uiFlowTableIndex = 0;
    //FLOW_MANAGER_INFO("Start Report Now");

    AGENT_WORKING_FLOW_TABLE_LOCK();
    for(uiFlowTableIndex = 0; uiFlowTableIndex < AgentFlowTable[AGENT_WORKING_FLOW_TABLE].size(); uiFlowTableIndex++)
    {
        // ��ǰֻ����udpЭ��, δ�����Էſ�
        if (AGENT_DETECT_PROTOCOL_UDP == AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowKey.eProtocol)
        {
            // ֻ����enable�ķ�Urgent Entry
            if ((FLOW_ENTRY_STATE_CHECK(AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].uiFlowState, FLOW_ENTRY_STATE_ENABLE))
                && (AGENT_TRUE != AgentFlowTable[AGENT_WORKING_FLOW_TABLE][uiFlowTableIndex].stFlowKey.uiUrgentFlow))
            {
                iRet = FlowLatencyReport(uiFlowTableIndex);
                if (iRet)
                {
                    FLOW_MANAGER_ERROR("Flow Latency Report failed[%d], index[%u]", iRet, uiFlowTableIndex);
                }
            }
        }
    }
    AGENT_WORKING_FLOW_TABLE_UNLOCK();

    // ����range������һ���ϱ�����ʹ��AgentFlowTable�е���Щ��.
    iRet = AgentFlowTableEntryAdjust();
    if (iRet)
    {
        FLOW_MANAGER_ERROR("Flow Working Entry Adjust failed[%d]", iRet);
    }
    return iRet;
}

// ��Server��ȡһ���µ�̽������Ϣ, ֻ���ȡ��ͨ��. ����Ϣ�м������, ��׮
INT32 FlowManager_C::GetFlowFromServer(ServerFlowKey_S * pstNewFlow)
{
    INT32 iRet = AGENT_E_NOT_FOUND;
    static INT32 counter = 0;
    sal_memset(pstNewFlow, 0, sizeof(ServerFlowKey_S));

    //return AGENT_E_NOT_FOUND;
    if (uiServerFlowTableIsEmpty)
        counter = 0;
    
    pstNewFlow->eProtocol = AGENT_DETECT_PROTOCOL_UDP;
    pstNewFlow->uiSrcIP  = sal_inet_aton("10.10.10.1");
    pstNewFlow->uiDestIP = sal_inet_aton("10.10.10.2");
    pstNewFlow->uiSrcPortMin = 0;
    pstNewFlow->uiSrcPortMax = 0;
    pstNewFlow->uiSrcPortRange = 3;
    pstNewFlow->uiDscp = 20;
    pstNewFlow->uiUrgentFlow = AGENT_FALSE;
    pstNewFlow->stServerTopo.uiLevel = 1;
    pstNewFlow->stServerTopo.uiSvid  = 1;
    pstNewFlow->stServerTopo.uiDvid  = 2;
    iRet = AGENT_OK;
    
    if (counter)
        iRet = AGENT_E_NOT_FOUND;

    counter ++;

    return iRet;
}
            
// ����ʱ�Ƿ��������ѯServer��������.
INT32 FlowManager_C::QueryCheck(UINT32 uiCounter)
{
    UINT32 uiTimeCost = 0 ;
    UINT32 uiQueryPeriod = pcAgentCfg->GetQueryPeriod();

    // uiCounter �������
    uiTimeCost = (uiCounter >= uiLastQuerytTimeCounter) 
                ? uiCounter - uiLastQuerytTimeCounter
                : uiCounter + ((UINT32)(-1) - uiLastQuerytTimeCounter + 1);

    // ServerFlowTableΪ��ʱ, ���ٲ�ѯServer�ٶ�, �����Դﵽÿ300polling�����ѯ1��.
    if (uiServerFlowTableIsEmpty)
        uiQueryPeriod = uiQueryPeriod / 1000 + 300;
        

    if ( uiTimeCost >= uiQueryPeriod)
    {
        uiLastQuerytTimeCounter = uiCounter;
        // ˢ��̽��������,δ��ɵ�̽�����ᱻ���,�ϱ��������¿�ʼ����.
        uiLastReportTimeCounter = uiCounter;
        return AGENT_ENABLE;
    }
    else
        return AGENT_DISABLE;
}

// ������Serverˢ����������.
INT32 FlowManager_C::DoQuery()
{
    INT32 iRet = AGENT_OK;
    
    FLOW_MANAGER_INFO("Start Query Server Now");

    // ���Server��������
    ServerClearFlowTable(SERVER_CFG_FLOW_TABLE);
    uiServerFlowTableIsEmpty = AGENT_TRUE;

    // ˢ�� Server��������
#if 0
    ServerFlowKey_S stNewServerFlow;
    do
    {
        // ��Server��ȡ����Ϣ
        iRet = GetFlowFromServer(&stNewServerFlow);
        if (AGENT_OK == iRet) 
        {
            // ��������Ϣ��Server��������
            iRet = ServerCfgFlowTableAdd(stNewServerFlow);
            if (AGENT_OK != iRet)
            {
                FLOW_MANAGER_WARNING("Add New Flow to Server Cfg Flow Table failed[%d]", iRet);
            }
            else
                uiServerFlowTableIsEmpty = AGENT_FALSE;
        }
        else if (AGENT_E_NOT_FOUND != iRet)
        {
            FLOW_MANAGER_ERROR("Get Flow From Server failed[%d]", iRet);
            break;
        }
        
    }while(AGENT_E_NOT_FOUND != iRet);
#else
    iRet = RequestProbeListFromServer(this);
    if (AGENT_OK != iRet)
    {
        FLOW_MANAGER_WARNING("RequestProbeListFromServer[%d]", iRet);
    }
    else
    {
        uiServerFlowTableIsEmpty = AGENT_FALSE;
    }
#endif

    // �ύ Server��������
    iRet = ServerCommitCfgFlowTable();
    if (AGENT_OK != iRet)
    {
        FLOW_MANAGER_WARNING("Commit Server Cfg Flow Table failed[%d]", iRet);
    }
    
    FLOW_MANAGER_INFO("Query Server Finished");

    if ( AGENT_E_NOT_FOUND == iRet )
        return AGENT_OK;
    else
        return iRet;
}



// Thread�ص�����.
// PreStopHandler()ִ�к�, ThreadHandler()��Ҫ��GetCurrentInterval() us�������˳�.
INT32 FlowManager_C::ThreadHandler()
{
    INT32 iRet = AGENT_OK;
    UINT32 counter = 0;

    uiLastCheckTimeCounter = counter;
    uiLastReportTimeCounter = counter;
    uiLastQuerytTimeCounter = counter;
    while (GetCurrentInterval())
    {
        // ��ǰ�����Ƿ������̽������.
        if (DetectCheck(counter))
        {
            // ����̽������.
            iRet = DoDetect();
            if (iRet)
            {
                FLOW_MANAGER_WARNING("Do UDP Detect failed[%d]", iRet);
            }
        }
        
        // ��ǰ�����Ƿ���ռ�̽����
        if (DetectResultCheck(counter))
        {
            // �����ռ�̽��������
            iRet = GetDetectResult();
            if (iRet)
            {
                FLOW_MANAGER_WARNING("Get UDP Detect Result failed[%d]", iRet);
            }
        }
        
        // ��ǰ�����Ƿ�������ϱ�Collector����        
        if (ReportCheck(counter))
        {
            // �����ϱ�Collector����
            iRet = DoReport();
            if (iRet)
            {
                FLOW_MANAGER_WARNING("Do UDP Report failed[%d]", iRet);
            }
        }

        // ��ǰ�����Ƿ�ò�ѯServer����
        if (QueryCheck(counter))
        {
            // ������ѯServer��������.
            iRet = DoQuery();
            if (iRet)
            {
                FLOW_MANAGER_WARNING("Do Query failed[%d]", iRet);
            }
        }
        
        sal_sem_take(semTimer, -1);
        counter ++;
    }
    return AGENT_OK;
}

// Thread��������, ֪ͨThreadHandler����׼��.
INT32 FlowManager_C::PreStartHandler()
{
    INT32 iRet = AGENT_OK;
    iRet = SetNewInterval(pcAgentCfg->GetPollingTimerPeriod());
    if (iRet)
    {
        FLOW_MANAGER_ERROR("SetNewInterval failed[%d], Interval[%d]", iRet, pcAgentCfg->GetPollingTimerPeriod());
        return AGENT_E_PARA;
    }
    return iRet;
}

// Thread����ֹͣ, ֪ͨThreadHandler�����˳�.
INT32 FlowManager_C::PreStopHandler()
{
    SetNewInterval(0);
    return AGENT_OK;
}

