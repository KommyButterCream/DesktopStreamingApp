#include "StreamingClient.h"

#include <new>

#include "../../../../Module/IOCPNetworkEngine/Job/Job.h"
#include "../../../../Module/IOCPNetworkEngine/HandlerTable/PacketHandlerTable.h"

#include "../../../../Module/IOCPNetworkEngine/Memory/EngineMemoryPoolHelper.h"

#include "../../../../Module/IOCPNetworkEngine/Protocol/PacketID.h"

#include "../../../../Module/IOCPNetworkEngine/Session/SessionJobQueue.h"
#include "../../../../Module/IOCPNetworkEngine/Session/ClientSession.h"

#include "ClientPacketHandler.h"

using namespace Core::Util;

namespace
{
	// 서버 쪽과 같은 사정이다. 이 클라이언트가 세션 컨텍스트에 넣는 것은
	// DesktopStreamClientSessionContext 하나뿐이므로 확인할 것이 없다.
	inline DesktopStreamClientSessionContext* GetStreamContext(ClientSession* session)
	{
		if (!session)
			return nullptr;

		return static_cast<DesktopStreamClientSessionContext*>(session->GetSessionContext());
	}

	inline void CountUp(volatile LONG64& counter, LONG64 amount = 1)
	{
		::InterlockedAdd64(&counter, amount);
	}

	inline uint64_t ReadCount(const volatile LONG64& counter)
	{
		return static_cast<uint64_t>(::ReadAcquire64(&counter));
	}

	inline bool ReadFlag(const volatile LONG& flag)
	{
		return ::ReadAcquire(&flag) != FALSE;
	}
}

StreamingClient::StreamingClient() = default;

StreamingClient::~StreamingClient()
{
	StopClient();
}

bool StreamingClient::StartClient(const char* ipAddress, const uint16_t port,
	const SessionBufferConfig& bufferConfig, const EnginePoolConfig& poolConfig)
{
	if (!bufferConfig.IsValid())
	{
		LOGE("invalid session buffer config");
		return false;
	}

	if (!poolConfig.IsValid())
	{
		LOGE("invalid engine pool config");
		return false;
	}

	// 서버가 보내는 청크를 우리가 받아줄 수 있는지.
	//
	// 이게 어긋나면 엔진은 헤더가 주장하는 크기를 프로토콜 위반으로 보고
	// 연결을 끊는다. 서버 쪽에는 원인 모를 피어 끊김만 남는다 — 양쪽
	// 로그를 모아 놓고 봐도 원인이 드러나지 않는 종류의 실패다.
	if (bufferConfig.maxRecvPacketSize < DESKTOP_STREAM_CHUNK_PACKET_SIZE)
	{
		LOGE("maxRecvPacketSize %u is smaller than the server's chunk packet size %u. every chunk would be rejected",
			bufferConfig.maxRecvPacketSize, DESKTOP_STREAM_CHUNK_PACKET_SIZE);
		return false;
	}

	// 핸들러를 먼저 등록한다.
	//
	// 순서가 반대였다. StartClient 가 접속을 시작한 뒤에 등록했으므로,
	// 접속이 빠르면 핸들러가 없는 사이에 OnSessionEstablished 가 왔다.
	// 그 경우를 m_subscribeWhenReady 로 메우고 있었는데, 그 보정 자체가
	// 두 스레드가 같은 bool 을 보는 경합이었다.
	//
	// 등록을 먼저 하면 그 창이 아예 없다. 보정은 남겨 두지만(엔진이
	// 핸들러 테이블을 StartClient 안에서 만들므로 완전히 없앨 수는 없다)
	// 실제로 쓰일 일은 사라진다.
	if (!IOCPClient::StartClient(ipAddress, port, bufferConfig, 0, poolConfig))
		return false;

	const bool registerResult = PacketHandler::Client::RegisterHandlers(GetPacketHandlerTable());
	::InterlockedExchange(&m_handlersRegistered, registerResult ? TRUE : FALSE);

	if (!registerResult)
	{
		LOGE("failed to register packet handlers");
		IOCPClient::StopClient();
		return false;
	}

	// 등록 전에 세션이 established 가 됐다면 여기서 따라잡는다.
	if (::InterlockedExchange(&m_subscribeWhenReady, FALSE) != FALSE)
	{
		ClientSession* clientSession = GetClientSession();
		if (clientSession && clientSession->IsEstablished())
		{
			SendSubscribe(DESKTOP_STREAM_ID_PRIMARY);
		}
	}

	LOGI("connecting to %s:%u (chunk %u bytes, recv ring %u)",
		ipAddress, port, DESKTOP_STREAM_CHUNK_PACKET_SIZE, bufferConfig.recvRingSize);

	return true;
}

void StreamingClient::StopClient()
{
	::InterlockedExchange(&m_handlersRegistered, FALSE);
	::InterlockedExchange(&m_subscribeWhenReady, FALSE);
	IOCPClient::StopClient();
}

bool StreamingClient::SendSubscribe(uint32_t streamId)
{
	void* packetMemory = MEMORY_POOL::CreatePacket(*GetPacketMemoryPool(), sizeof(CS_DESKTOP_STREAMING_SUBSCRIBE_PACKET));
	CS_DESKTOP_STREAMING_SUBSCRIBE_PACKET* subscribePacket = reinterpret_cast<CS_DESKTOP_STREAMING_SUBSCRIBE_PACKET*>(packetMemory);
	if (!subscribePacket)
		return false;

	*subscribePacket = CS_DESKTOP_STREAMING_SUBSCRIBE_PACKET();
	subscribePacket->streamId = streamId;

	void* sendData = subscribePacket;
	if (!EnqueueSendPacket(&sendData, sizeof(CS_DESKTOP_STREAMING_SUBSCRIBE_PACKET)))
	{
		MEMORY_POOL::ReleasePacket(*GetPacketMemoryPool(), *GetGeneralMemoryPool(), subscribePacket);
		return false;
	}

	return true;
}

bool StreamingClient::SendUnsubscribe(uint32_t streamId)
{
	void* packetMemory = MEMORY_POOL::CreatePacket(*GetPacketMemoryPool(), sizeof(CS_DESKTOP_STREAMING_UNSUBSCRIBE_PACKET));
	CS_DESKTOP_STREAMING_UNSUBSCRIBE_PACKET* unsubscribePacket = reinterpret_cast<CS_DESKTOP_STREAMING_UNSUBSCRIBE_PACKET*>(packetMemory);
	if (!unsubscribePacket)
		return false;

	*unsubscribePacket = CS_DESKTOP_STREAMING_UNSUBSCRIBE_PACKET();
	unsubscribePacket->streamId = streamId;

	void* sendData = unsubscribePacket;
	if (!EnqueueSendPacket(&sendData, sizeof(CS_DESKTOP_STREAMING_UNSUBSCRIBE_PACKET)))
	{
		MEMORY_POOL::ReleasePacket(*GetPacketMemoryPool(), *GetGeneralMemoryPool(), unsubscribePacket);
		return false;
	}

	return true;
}


// 수신 상태를 서버에 알린다. 앱 스레드에서 주기적으로 부른다.
//
// 서버는 자기 송신 실패만 볼 수 있고, 그건 "네트워크가 막혔다" 는 신호다.
// "디코더가 못 따라간다" 는 여기서만 나온다. 비트레이트를 내려야 할 이유의
// 절반이 이쪽에 있다.
bool StreamingClient::SendFeedback(uint64_t decodeDroppedTotal, uint32_t streamId)
{
	ClientSession* clientSession = GetClientSession();
	if (!clientSession || !clientSession->IsEstablished())
		return false;

	DesktopStreamClientSessionContext* streamContext = GetStreamContext(clientSession);
	if (!streamContext || !streamContext->hasStreamInfo)
		return false;

	const uint64_t completedTotal = ReadCount(m_statFramesCompleted);
	const uint64_t discardedTotal = ReadCount(m_statFramesDiscarded);

	// 증분으로 보낸다. 재접속으로 누적이 뒤로 가면 기준을 다시 잡는다.
	const uint64_t completedDelta = (completedTotal >= m_reportedFramesCompleted)
		? (completedTotal - m_reportedFramesCompleted) : 0;
	const uint64_t discardedDelta = (discardedTotal >= m_reportedFramesDiscarded)
		? (discardedTotal - m_reportedFramesDiscarded) : 0;
	const uint64_t decodeDroppedDelta = (decodeDroppedTotal >= m_reportedDecodeDropped)
		? (decodeDroppedTotal - m_reportedDecodeDropped) : 0;

	void* packetMemory = MEMORY_POOL::CreatePacket(*GetPacketMemoryPool(), sizeof(CS_DESKTOP_STREAMING_FEEDBACK_PACKET));
	CS_DESKTOP_STREAMING_FEEDBACK_PACKET* feedbackPacket =
		reinterpret_cast<CS_DESKTOP_STREAMING_FEEDBACK_PACKET*>(packetMemory);
	if (!feedbackPacket)
		return false;

	*feedbackPacket = CS_DESKTOP_STREAMING_FEEDBACK_PACKET();
	feedbackPacket->streamId = streamId;
	feedbackPacket->reportSequence = ++streamContext->feedbackSequence;
	feedbackPacket->framesCompleted = static_cast<uint32_t>(completedDelta);
	feedbackPacket->framesDiscarded = static_cast<uint32_t>(discardedDelta);
	feedbackPacket->decodeDropped = static_cast<uint32_t>(decodeDroppedDelta);

	void* sendData = feedbackPacket;
	if (!EnqueueSendPacket(&sendData, sizeof(CS_DESKTOP_STREAMING_FEEDBACK_PACKET)))
	{
		MEMORY_POOL::ReleasePacket(*GetPacketMemoryPool(), *GetGeneralMemoryPool(), feedbackPacket);
		--streamContext->feedbackSequence;
		return false;
	}

	// 보낸 뒤에만 기준을 옮긴다. 실패한 증분은 다음 보고에 합쳐진다.
	m_reportedFramesCompleted = completedTotal;
	m_reportedFramesDiscarded = discardedTotal;
	m_reportedDecodeDropped = decodeDroppedTotal;

	return true;
}
void StreamingClient::SetStreamInfoCallback(StreamInfoCallback callback, void* userData)
{
	m_streamInfoCallback = callback;
	m_streamInfoCallbackUserData = userData;
}

void StreamingClient::SetFrameCallback(FrameCallback callback, void* userData)
{
	m_frameCallback = callback;
	m_frameCallbackUserData = userData;
}

void StreamingClient::SetConnectionCallback(ConnectionCallback callback, void* userData)
{
	m_connectionCallback = callback;
	m_connectionCallbackUserData = userData;
}

bool StreamingClient::IsConnected() const
{
	ClientSession* clientSession = GetClientSession();
	return clientSession && clientSession->IsEstablished();
}

void StreamingClient::GetStats(DesktopStreamingClientStats& stats) const
{
	stats.chunksAccepted = ReadCount(m_statChunksAccepted);
	stats.chunksRejected = ReadCount(m_statChunksRejected);
	stats.chunkBytesReceived = ReadCount(m_statChunkBytesReceived);
	stats.framesCompleted = ReadCount(m_statFramesCompleted);
	stats.framesDiscarded = ReadCount(m_statFramesDiscarded);
	stats.streamInfoReceived = ReadCount(m_statStreamInfoReceived);
	stats.connected = IsConnected();
}

void StreamingClient::ResetStats()
{
	::InterlockedExchange64(&m_statChunksAccepted, 0);
	::InterlockedExchange64(&m_statChunksRejected, 0);
	::InterlockedExchange64(&m_statChunkBytesReceived, 0);
	::InterlockedExchange64(&m_statFramesCompleted, 0);
	::InterlockedExchange64(&m_statFramesDiscarded, 0);
	::InterlockedExchange64(&m_statStreamInfoReceived, 0);
}

void StreamingClient::NoteChunkAccepted(uint32_t chunkBytes)
{
	CountUp(m_statChunksAccepted);
	CountUp(m_statChunkBytesReceived, static_cast<LONG64>(chunkBytes));
}

void StreamingClient::NoteChunkRejected()
{
	CountUp(m_statChunksRejected);
}

void StreamingClient::NoteFrameDiscarded()
{
	CountUp(m_statFramesDiscarded);
}

bool StreamingClient::HandleStreamInfo(const SC_DESKTOP_STREAMING_INFO_PACKET* infoPacket)
{
	if (!infoPacket)
		return false;

	DesktopStreamClientSessionContext* streamContext = GetStreamContext(GetClientSession());
	if (!streamContext)
		return false;

	// 버전이 바뀌었으면 스트림 자체가 달라진 것이다 (해상도 변경 등).
	// 진행 중이던 재조립은 옛 스트림의 것이므로 버린다 — 이어 붙이면
	// 깨진 NAL 이 디코더로 간다.
	const bool streamChanged =
		streamContext->hasStreamInfo &&
		(streamContext->streamInfoVersion != infoPacket->streamInfoVersion ||
		 streamContext->width != infoPacket->width ||
		 streamContext->height != infoPacket->height);

	if (streamChanged)
	{
		LOGI("stream changed : %ux%u -> %ux%u (version %u -> %u). resetting reassembly",
			streamContext->width, streamContext->height,
			infoPacket->width, infoPacket->height,
			streamContext->streamInfoVersion, infoPacket->streamInfoVersion);

		streamContext->SetReassembling(false);
		streamContext->receivedFrameSize = 0;
		streamContext->nextChunkIndex = 0;
		streamContext->expectedFrameSize = 0;
		streamContext->expectedChunkCount = 0;
	}

	streamContext->SetSubscribed(true);
	streamContext->hasStreamInfo = true;
	streamContext->streamId = infoPacket->streamId;
	streamContext->streamInfoVersion = infoPacket->streamInfoVersion;
	streamContext->codecConfigVersion = infoPacket->codecConfigVersion;
	streamContext->codecType = infoPacket->codecType;
	streamContext->width = infoPacket->width;
	streamContext->height = infoPacket->height;

	CountUp(m_statStreamInfoReceived);

	if (m_streamInfoCallback)
	{
		m_streamInfoCallback(*streamContext, m_streamInfoCallbackUserData);
	}

	return true;
}

bool StreamingClient::HandleFrameComplete(const DesktopStreamClientSessionContext& streamContext)
{
	CountUp(m_statFramesCompleted);

	if (m_frameCallback)
	{
		m_frameCallback(
			reinterpret_cast<const uint8_t*>(streamContext.frameBuffer),
			streamContext.receivedFrameSize,
			streamContext.currentFrameId,
			streamContext.currentTimestamp,
			streamContext.frameType,
			m_frameCallbackUserData);
	}

	return true;
}

void* StreamingClient::GetServiceContext()
{
	return this;
}

void StreamingClient::OnClientConnect(ISession* session)
{
	ClientSession* clientSession = static_cast<ClientSession*>(session);
	if (!clientSession)
		return;

	// 이 컨텍스트는 512KB 짜리 재조립 버퍼를 안고 있다. 할당 실패를
	// 조용히 넘기면 모든 청크가 컨텍스트 없음으로 버려진다.
	DesktopStreamClientSessionContext* streamContext = new (std::nothrow) DesktopStreamClientSessionContext();
	if (!streamContext)
	{
		LOGE("could not allocate the stream context (%u bytes)",
			static_cast<uint32_t>(sizeof(DesktopStreamClientSessionContext)));
		return;
	}

	clientSession->SetSessionContext(streamContext);
}

void StreamingClient::OnSessionEstablished(ISession* session)
{
	(void)session;

	NotifyConnectionEvent(DESKTOP_STREAM_CONNECTION_EVENT::Established, DisconnectReason::Unknown, 0);

	if (!ReadFlag(m_handlersRegistered))
	{
		// 핸들러가 아직 없다. 지금 구독을 보내면 응답을 처리할 곳이 없으므로
		// StartClient 가 등록을 마친 뒤 대신 보내게 한다.
		::InterlockedExchange(&m_subscribeWhenReady, TRUE);
		return;
	}

	SendSubscribe(DESKTOP_STREAM_ID_PRIMARY);
}

void StreamingClient::OnConnectFailed(int errorCode)
{
	LOGW("could not reach the server : WSA error %d", errorCode);

	NotifyConnectionEvent(DESKTOP_STREAM_CONNECTION_EVENT::ConnectFailed, DisconnectReason::ConnectFailed, errorCode);
}

void StreamingClient::OnClientDisconnect(ISession* session, DisconnectReason reason)
{
	// 사유를 남긴다. 재접속 정책을 붙이기 전이라도 "왜 끊겼는지" 가
	// 로그에 남아야 현장에서 판단할 수 있다.
	LOGI("disconnected : %s (retryable=%d)", ToString(reason), IsRetryableDisconnect(reason) ? 1 : 0);

	DesktopStreamClientSessionContext* streamContext = GetStreamContext(static_cast<ClientSession*>(session));
	if (streamContext)
	{
		streamContext->SetSubscribed(false);

		// 재조립을 중단한다. 다시 붙으면 새 프레임부터 시작해야 한다 —
		// 끊기기 전의 절반짜리 프레임에 새 청크를 이어 붙이면 깨진 NAL 이
		// 디코더로 간다.
		streamContext->SetReassembling(false);
	}

	NotifyConnectionEvent(DESKTOP_STREAM_CONNECTION_EVENT::Disconnected, reason, 0);
}

void StreamingClient::OnReceive(ISession* session, uint16_t packetId, const char* packetData, uint32_t packetSize)
{
	// 잡 생성과 큐 적재를 엔진이 처리한다. packetData 의 소유권도 넘어간다.
	//
	// 예전에는 이 자리에서 직접 했는데, 네 개의 실패 경로가 전부
	// __debugbreak 후 그냥 return 해서 packetData 를 흘리고 있었다.
	// (서버 쪽과 같은 규약이라 같은 함수 이름을 쓴다)
	if (!SubmitPacketJob(session, packetId, packetData, packetSize))
	{
		LOGW("packet id %u was not queued", packetId);
	}
}

void StreamingClient::OnSend(ISession* session, uint32_t bytesTransferred)
{
	// 서버 쪽과 같다. dynamic_cast 와 __debugbreak 가 있었지만 확인할 것도
	// 할 일도 없는 자리다.
	(void)session;
	(void)bytesTransferred;
}

void StreamingClient::NotifyConnectionEvent(DESKTOP_STREAM_CONNECTION_EVENT event, DisconnectReason reason, int errorCode)
{
	if (!m_connectionCallback)
		return;

	m_connectionCallback(event, reason, errorCode, m_connectionCallbackUserData);
}

bool StreamingClient::EnqueueSendPacket(void** packetData, uint32_t packetSize)
{
	if (!packetData || packetSize == 0)
		return false;

	ClientSession* clientSession = GetClientSession();
	if (!clientSession || !clientSession->IsEstablished())
		return false;

	if (!clientSession->EnqueueSendPacket(packetData, packetSize))
		return false;

	return true;
}
