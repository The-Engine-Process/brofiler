#include "Brofiler.h"

#if USE_BROFILER_ETW

#include <windows.h>
#include "Core.h"
#include "Tracer.h"

namespace Brofiler
{

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
const byte SWITCH_CONTEXT_INSTRUCTION_OPCODE = 36;
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
struct CSwitch 
{
	uint32 NewThreadId;
	uint32 OldThreadId;
	int8  NewThreadPriority;
	int8  OldThreadPriority;
	uint8  PreviousCState;
	int8  SpareByte;
	int8  OldThreadWaitReason;
	int8  OldThreadWaitMode;
	int8  OldThreadState;
	int8  OldThreadWaitIdealProcessor;
	uint32 NewThreadWaitTime;
	uint32 Reserved;
};
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void WINAPI OnRecordEvent(PEVENT_RECORD eventRecord)
{
	if (eventRecord->EventHeader.EventDescriptor.Opcode != SWITCH_CONTEXT_INSTRUCTION_OPCODE)
		return;

	if (sizeof(CSwitch) != eventRecord->UserDataLength)
		return;

	CSwitch* pSwitchEvent = (CSwitch*)eventRecord->UserData;

	const std::vector<ThreadEntry*>& threads = Core::Get().GetThreads();

	for (size_t i = 0; i < threads.size(); ++i)
	{
		ThreadEntry* entry = threads[i];

		if (entry->description.threadID.AsUInt64() == (uint64)pSwitchEvent->OldThreadId)
		{
			if (SyncData* time = entry->storage.synchronizationBuffer.Back())
			{
				time->finish = eventRecord->EventHeader.TimeStamp.QuadPart;
				time->reason = pSwitchEvent->OldThreadWaitReason;
			}
		}

		if (entry->description.threadID.AsUInt64() == (uint64)pSwitchEvent->NewThreadId)
		{
			SyncData& time = entry->storage.synchronizationBuffer.Add();
			time.start = eventRecord->EventHeader.TimeStamp.QuadPart;
			time.finish = time.start;
			time.core = eventRecord->BufferContext.ProcessorNumber;
		}
	}
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
const TRACEHANDLE INVALID_TRACEHANDLE = (TRACEHANDLE)-1;
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
DWORD WINAPI ETW::RunProcessTraceThreadFunction( LPVOID parameter )
{
	ETW* etw = (ETW*)parameter;
	ProcessTrace(&etw->openedHandle, 1, 0, 0);
	return 0;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

ETW::ETW() : isActive(false), sessionHandle(INVALID_TRACEHANDLE), openedHandle(INVALID_TRACEHANDLE), processThreadHandle(INVALID_HANDLE_VALUE)
{
	ULONG bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(KERNEL_LOGGER_NAME);
	sessionProperties =(EVENT_TRACE_PROPERTIES*) malloc(bufferSize);
}

EtwStatus ETW::Start()
{
	if (!isActive) 
	{
		ULONG bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(KERNEL_LOGGER_NAME);
		ZeroMemory(sessionProperties, bufferSize);
		sessionProperties->Wnode.BufferSize = bufferSize;
		sessionProperties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

		sessionProperties->EnableFlags = EVENT_TRACE_FLAG_CSWITCH;
		sessionProperties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;

		sessionProperties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
		sessionProperties->Wnode.ClientContext = 1;
		sessionProperties->Wnode.Guid = SystemTraceControlGuid;

		// ERROR_BAD_LENGTH(24): The Wnode.BufferSize member of Properties specifies an incorrect size. Properties does not have sufficient space allocated to hold a copy of SessionName.
		// ERROR_ALREADY_EXISTS(183): A session with the same name or GUID is already running.
		// ERROR_ACCESS_DENIED(5): Only users with administrative privileges, users in the Performance Log Users group, and services running as LocalSystem, LocalService, NetworkService can control event tracing sessions.
		// ERROR_INVALID_PARAMETER(87)
		// ERROR_BAD_PATHNAME(161)
		// ERROR_DISK_FULL(112)
		int retryCount = 2;
		ULONG status = ETW_OK;

		while (--retryCount >= 0)
		{
			status = StartTrace(&sessionHandle, KERNEL_LOGGER_NAME, sessionProperties);

			switch (status)
			{
			case ERROR_ALREADY_EXISTS:
				ControlTrace(0, KERNEL_LOGGER_NAME, sessionProperties, EVENT_TRACE_CONTROL_STOP);
				break;

			case ERROR_ACCESS_DENIED:
				return ETW_ERROR_ACCESS_DENIED;

			case ERROR_SUCCESS:
				retryCount = 0;
				break;

			default:
				return ETW_FAILED;
			}
		}

		if (status != ERROR_SUCCESS)
			return ETW_FAILED;

		ZeroMemory(&logFile, sizeof(EVENT_TRACE_LOGFILE));

		logFile.LoggerName = KERNEL_LOGGER_NAME;
		logFile.ProcessTraceMode = (PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP);
		logFile.EventRecordCallback = OnRecordEvent;

		openedHandle = OpenTrace(&logFile);
		if (openedHandle == INVALID_TRACEHANDLE)
			return ETW_FAILED;

		DWORD threadID;
		processThreadHandle = CreateThread(0, 0, RunProcessTraceThreadFunction, this, 0, &threadID);
		
		isActive = true;
	}

	return ETW_OK;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool ETW::Stop()
{
	if (isActive)
	{
		ULONG controlTraceResult = ControlTrace(openedHandle, KERNEL_LOGGER_NAME, sessionProperties, EVENT_TRACE_CONTROL_STOP);

		// ERROR_CTX_CLOSE_PENDING(7007L): The call was successful. The ProcessTrace function will stop after it has processed all real-time events in its buffers (it will not receive any new events).
		// ERROR_BUSY(170L): Prior to Windows Vista, you cannot close the trace until the ProcessTrace function completes.
		// ERROR_INVALID_HANDLE(6L): One of the following is true: TraceHandle is NULL. TraceHandle is INVALID_HANDLE_VALUE.
		ULONG closeTraceStatus = CloseTrace(openedHandle);

		// Wait for ProcessThread to finish
		WaitForSingleObject(processThreadHandle, INFINITE);
		BOOL wasThreadClosed = CloseHandle(processThreadHandle);

		isActive = false;

		return wasThreadClosed && closeTraceStatus == ERROR_SUCCESS && controlTraceResult == ERROR_SUCCESS;
	}

	return false;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
ETW::~ETW()
{
	Stop();
	delete sessionProperties;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
}

#endif
