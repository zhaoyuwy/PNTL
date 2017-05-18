
#ifndef __SRC_AgentJsonAPI_H__
#define __SRC_AgentJsonAPI_H__

#include <sstream>
#include "FlowManager.h"

// ����Agent���������ļ�, ��ɳ�ʼ������.
extern INT32 ParserLocalCfg(const CHAR * pcJsonData, ServerAntAgentCfg_C * pcCfg);

// ����json��ʽ���ַ���, ���·���FlowManager, ������Server�����·���Urgent̽����.
extern INT32 ProcessUrgentFlowFromServer(const CHAR * pcJsonData, FlowManager_C* pcFlowManager);

// ����json��ʽ���ַ���, ���·���FlowManager, ��������Server����ʱServer�ظ�����ͨ̽����.
extern INT32 ProcessNormalFlowFromServer(const CHAR * pcJsonData, FlowManager_C* pcFlowManager);

// ����json��ʽ���ַ���, ���ڷ�����Server����Probe-listʱ�ύ��post data.
extern INT32 CreatProbeListRequestPostData(ServerAntAgentCfg_C * pcCfg, stringstream * pssPostData);

// ����json��ʽ���ַ���, ������Analyzer�ϱ���ʱ��Ϣ.
extern INT32 CreatLatencyReportData(AgentFlowTableEntry_S * pstAgentFlowEntry, stringstream * pssReportData);

// ����json��ʽ���ַ���, ������Analyzer�ϱ�������Ϣ.
extern INT32 CreatDropReportData(AgentFlowTableEntry_S * pstAgentFlowEntry, stringstream * pssReportData);

#endif