#include "StreamingServer.h"

#include "../../../../Module/IOCPNetworkEngine/Job/Job.h" 
#include "../../../../Module/IOCPNetworkEngine/HandlerTable/PacketHandlerTable.h" 
#include "../../../../Module/IOCPNetworkEngine/Scheduler/ReadySessionQueue.h" 

#include "../../../../Module/IOCPNetworkEngine/Buffer/PreDefine.h"
#include "../../../../Module/IOCPNetworkEngine/Buffer/SharedSendPacket.h"
#include "../../../../Module/IOCPNetworkEngine/Memory/EngineMemoryPoolHelper.h" 

#include "../../../../Module/IOCPNetworkEngine/Protocol/PacketID.h" 

#include "../../../../Module/IOCPNetworkEngine/Session/SessionJobQueue.h" 
#include "../../../../Module/IOCPNetworkEngine/Session/ClientSession.h" // for ClientSession

#include "ServerPacketHandler.h"
#include "../StreamingProtocol/StreamingPacket.h"

#include <cstring>
#include <new>

using namespace Core::Util;

StreamingServer::StreamingServer()
{

}

StreamingServer::~StreamingServer()
{
	StopServer();
}

bool StreamingServer::StartServer(const char* ipAddress, const uint16_t port, const uint32_t maxConnectionCount)
{
	if (!IOCPServer::StartServer(ipAddress, port, maxConnectionCount))
		return false;

	if (!InitializeViewerList(maxConnectionCount))
	{
		IOCPServer::StopServer();
		return false;
	}

	bool registerResult = true;

	registerResult &= PacketHandler::Server::RegisterHandlers(GetPacketHandlerTable());

	return registerResult;
}

void StreamingServer::StopServer()
{
	FinalizeViewerList();

	IOCPServer::StopServer();
}

void StreamingServer::SetStreamInfo(uint16_t width, uint16_t height, uint16_t fps, DESKTOP_STREAM_CODEC_TYPE codecType)
{
	m_streamWidth = width;
	m_streamHeight = height;
	m_streamFps = fps;
	m_codecType = codecType;
	++m_streamInfoVersion;
	++m_codecConfigVersion;
}

bool StreamingServer::HasSubscribedViewer()
{
	ClientSession** viewers = nullptr;
	return GetSubscribedViewerSnapshot(&viewers) > 0;
}

bool StreamingServer::HasViewerWaitingForKeyframe()
{
	if (!m_viewers || m_viewerCapacity == 0)
		return false;

	bool hasWaitingViewer = false;

	::AcquireSRWLockShared(&m_viewerLock);
	for (uint32_t index = 0; index < m_viewerCount; ++index)
	{
		ClientSession* viewer = m_viewers[index];
		if (!viewer || !viewer->IsEstablished())
			continue;

		DesktopStreamServerSessionContext* streamContext = dynamic_cast<DesktopStreamServerSessionContext*>(viewer->GetSessionContext());
		if (!streamContext || !streamContext->subscribed || streamContext->streamId != DESKTOP_STREAM_ID_PRIMARY)
			continue;

		if (streamContext->waitingForKeyframe)
		{
			hasWaitingViewer = true;
			break;
		}
	}
	::ReleaseSRWLockShared(&m_viewerLock);

	return hasWaitingViewer;
}

bool StreamingServer::BroadcastEncodedFrame(const uint8_t* encodedData, uint32_t encodedSize, uint64_t frameId, uint64_t timestamp, uint16_t frameType, bool isKeyFrame)
{
	if (!encodedData || encodedSize == 0 || !m_viewers || m_viewerCapacity == 0)
		return false;

	if (encodedSize > DESKTOP_STREAM_MAX_FRAME_SIZE)
	{
		Logger::Log(LogLevel::LOG_WARNING, "[%s] encoded frame too large (%u)", __FUNCTION__, encodedSize);
		return false;
	}

	ClientSession** viewers = nullptr;
	const uint32_t viewerCount = GetSubscribedViewerSnapshot(&viewers);
	if (viewerCount == 0)
		return true;

	if (!m_viewerTakingFrame)
		return false;

	// 프레임은 뷰어별로 전부-아니면-전무다.
	//
	// 예전에는 청크 하나가 큐 포화로 실패해도 그냥 넘어가고 다음 청크를
	// 계속 밀었다. 클라이언트는 그 구멍을 잡아내지만(재조립이 chunkIndex 를
	// 순서대로 요구한다) 그래서 그 프레임을 통째로 버리므로, 뒤이어 보낸
	// 청크들은 이미 차 있는 큐를 더 채우는 순수한 낭비였다.
	//
	// 그보다 나빴던 것은 대기 표시다. 키프레임의 0번 청크가 큐에 들어간
	// 것만 보고 waitingForKeyframe 을 내렸는데, 7번에서 막히면 그 뷰어는
	// 키프레임을 완성하지 못한다. 완성하지 못했는데 표시는 내려갔고,
	// 그 표시를 보는 것이 인코더의 forceKeyFrame 이다
	// (DesktopStreamingServerApp.h). 서버는 동기화됐다고 믿고 IDR 강제를
	// 멈추므로 그 뷰어는 참조 프레임 없이 영구히 남는다.
	//
	// 실측: 한 바이트도 읽지 않는 뷰어에 243프레임을 밀면
	// HasViewerWaitingForKeyframe() 이 false 로 뒤집혔다. (tools/framedrop)
	for (uint32_t viewerIndex = 0; viewerIndex < viewerCount; ++viewerIndex)
		m_viewerTakingFrame[viewerIndex] = true;

	constexpr uint32_t chunkHeaderSize = GetDesktopStreamingFrameChunkHeaderSize();
	const uint32_t maxChunkDataSize = MEMORY_SIZE_32K - chunkHeaderSize;
	if (maxChunkDataSize == 0)
		return false;

	const uint32_t chunkCount32 = (encodedSize + maxChunkDataSize - 1) / maxChunkDataSize;
	if (chunkCount32 > USHRT_MAX)
		return false;

	const uint16_t chunkCount = static_cast<uint16_t>(chunkCount32);

	for (uint16_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
	{
		// 아무도 이 프레임을 끝까지 받고 있지 않으면 남은 청크는 낭비다.
		bool anyViewerLeft = false;
		for (uint32_t viewerIndex = 0; viewerIndex < viewerCount; ++viewerIndex)
		{
			if (m_viewerTakingFrame[viewerIndex])
			{
				anyViewerLeft = true;
				break;
			}
		}

		if (!anyViewerLeft)
			break;

		const uint32_t chunkOffset = static_cast<uint32_t>(chunkIndex) * maxChunkDataSize;
		const uint32_t remainingSize = encodedSize - chunkOffset;
		const uint32_t chunkDataSize = (remainingSize < maxChunkDataSize) ? remainingSize : maxChunkDataSize;
		const uint16_t packetSize = MakeDesktopStreamingVariablePacketSize(chunkHeaderSize, chunkDataSize);
		if (packetSize == 0)
			return false;

		void* packetMemory = MEMORY_POOL::CreatePacket(*GetPacketMemoryPool(), packetSize);
		SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET* framePacket = reinterpret_cast<SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET*>(packetMemory);
		if (!framePacket)
		{
			// 여기서 그냥 돌아가면 앞선 청크를 이미 받은 뷰어들이 미완성
			// 프레임을 안고 남는다. 키프레임이었다면 대기 표시를 되살려야
			// 다음 IDR 을 받는다.
			AbortFrameForTakingViewers(viewers, viewerCount, isKeyFrame);
			return false;
		}

		*framePacket = SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET();
		framePacket->header.packetSize = packetSize;
		framePacket->streamId = DESKTOP_STREAM_ID_PRIMARY;
		framePacket->streamInfoVersion = m_streamInfoVersion;
		framePacket->codecConfigVersion = m_codecConfigVersion;
		framePacket->frameId = frameId;
		framePacket->timestamp = timestamp;
		framePacket->totalFrameSize = encodedSize;
		framePacket->chunkOffset = chunkOffset;
		framePacket->chunkIndex = chunkIndex;
		framePacket->chunkCount = chunkCount;
		framePacket->chunkDataSize = static_cast<uint16_t>(chunkDataSize);
		framePacket->frameType = frameType;
		memcpy(framePacket->chunkData, encodedData + chunkOffset, chunkDataSize);

		// 참조 계수 규약은 SharedSendPacket.h 의 예시 그대로다 —
		// Create 가 1 로 시작하고 그 1 이 배포자 몫이다.
		SharedSendPacket* sharedPacket = SHARED_SEND_PACKET::Create(
			*GetSendQueueMemoryPool(), *GetPacketMemoryPool(), framePacket);

		if (!sharedPacket)
		{
			MEMORY_POOL::ReleasePacket(*GetPacketMemoryPool(), *GetGeneralMemoryPool(), framePacket);
			AbortFrameForTakingViewers(viewers, viewerCount, isKeyFrame);
			return false;
		}

		for (uint32_t viewerIndex = 0; viewerIndex < viewerCount; ++viewerIndex)
		{
			// 이 프레임을 이미 놓친 뷰어에게는 나머지 청크를 보내지 않는다.
			// 어차피 재조립에서 버려질 바이트로 포화된 큐를 더 채울 뿐이다.
			if (!m_viewerTakingFrame[viewerIndex])
				continue;

			ClientSession* viewer = viewers[viewerIndex];
			if (!viewer)
			{
				m_viewerTakingFrame[viewerIndex] = false;
				continue;
			}

			DesktopStreamServerSessionContext* streamContext = dynamic_cast<DesktopStreamServerSessionContext*>(viewer->GetSessionContext());
			if (!streamContext || streamContext->streamId != DESKTOP_STREAM_ID_PRIMARY)
			{
				m_viewerTakingFrame[viewerIndex] = false;
				continue;
			}

			// 키프레임을 기다리는 뷰어에게 P 프레임은 의미가 없다. 실패가
			// 아니라 정상적인 건너뛰기이므로 대기 표시는 그대로 둔다.
			if (streamContext->waitingForKeyframe && !isKeyFrame)
			{
				m_viewerTakingFrame[viewerIndex] = false;
				continue;
			}

			SHARED_SEND_PACKET::AddRef(sharedPacket);
			if (!viewer->EnqueueSharedSendPacket(framePacket, packetSize, SHARED_SEND_PACKET::ReleaseCallback, sharedPacket))
			{
				SHARED_SEND_PACKET::Release(sharedPacket);

				// 이 뷰어는 이 프레임을 완성할 수 없다. 앞서 보낸 청크는
				// 클라이언트가 버리고, 남은 청크는 보내지 않는다.
				//
				// 그리고 다음 키프레임부터 다시 시작해야 한다. 이 표시가
				// 인코더의 IDR 강제를 되살린다.
				m_viewerTakingFrame[viewerIndex] = false;
				streamContext->waitingForKeyframe = true;
			}
		}

		SHARED_SEND_PACKET::Release(sharedPacket);
	}

	// 마지막 청크까지 실패 없이 온 뷰어만 프레임을 온전히 받았다.
	// 키프레임의 대기 표시를 내리는 자리는 여기 하나뿐이다.
	uint32_t deliveredCount = 0;

	for (uint32_t viewerIndex = 0; viewerIndex < viewerCount; ++viewerIndex)
	{
		if (!m_viewerTakingFrame[viewerIndex])
			continue;

		++deliveredCount;

		if (!isKeyFrame)
			continue;

		ClientSession* viewer = viewers[viewerIndex];
		if (!viewer)
			continue;

		DesktopStreamServerSessionContext* streamContext = dynamic_cast<DesktopStreamServerSessionContext*>(viewer->GetSessionContext());
		if (streamContext)
		{
			streamContext->waitingForKeyframe = false;
		}
	}

	return deliveredCount > 0;
}

// 프레임 중간에 포기한다. 이미 앞선 청크를 받은 뷰어들은 미완성 프레임을
// 안게 되므로, 키프레임이었다면 대기 표시를 되살려 다음 IDR 을 받게 한다.
void StreamingServer::AbortFrameForTakingViewers(ClientSession** viewers, uint32_t viewerCount, bool isKeyFrame)
{
	if (!viewers || !m_viewerTakingFrame)
		return;

	for (uint32_t viewerIndex = 0; viewerIndex < viewerCount; ++viewerIndex)
	{
		if (!m_viewerTakingFrame[viewerIndex])
			continue;

		m_viewerTakingFrame[viewerIndex] = false;

		if (!isKeyFrame)
			continue;

		ClientSession* viewer = viewers[viewerIndex];
		if (!viewer)
			continue;

		DesktopStreamServerSessionContext* streamContext = dynamic_cast<DesktopStreamServerSessionContext*>(viewer->GetSessionContext());
		if (streamContext)
		{
			streamContext->waitingForKeyframe = true;
		}
	}
}

void* StreamingServer::GetServiceContext()
{
	return this;
}

void StreamingServer::OnClientConnect(ISession* session)
{
	ClientSession* clientSession = dynamic_cast<ClientSession*>(session);
	if (!clientSession)
		return;

	clientSession->SetSessionContext(new (std::nothrow) DesktopStreamServerSessionContext());
}

void StreamingServer::OnClientDisconnect(ISession* session, DisconnectReason reason)
{
	ClientSession* clientSession = dynamic_cast<ClientSession*>(session);
	if (!clientSession)
		return;

	RemoveViewer(clientSession);
}

void StreamingServer::OnReceive(ISession* session, uint16_t packetId, const char* packetData, uint32_t packetSize)
{
	const char* payload = packetData + sizeof(PACKET_HEADER);
	uint32_t payloadSize = packetSize - sizeof(PACKET_HEADER);

	ClientSession* clientSession = dynamic_cast<ClientSession*>(session);

	if (!clientSession)
	{
		__debugbreak();

		return;
	}

	Logger::Log(LogLevel::LOG_INFO, "[%s] RECV PACKET - ID: %u, Size: %d, %d, %s", __FUNCTION__, session->GetSessionID(), packetId, packetSize, payload);

	// 잡 생성 + 큐 적재 + 세션 스케줄을 엔진이 한 번에 처리한다.
	//
	// 예전에는 이 자리에서 다섯 단계를 직접 했다 — CreateJob, EnqueueJob,
	// wasEmpty 검사, IsProcessingReady, Push, 그리고 Push 실패 시 플래그
	// 복구. 어느 하나를 빠뜨리면 그 세션이 조용히 영구 정지하는 규약이었다.
	//
	// packetData 의 소유권도 이 호출이 가져간다. 실패해도 엔진이 회수하므로
	// 여기서 해제할 일이 없다. (예전 실패 경로들은 그냥 return 해서 패킷을
	// 흘리고 있었다)
	if (!SubmitPacketJob(session, packetId, packetData, packetSize))
	{
		Logger::Log(LogLevel::LOG_WARNING, "[%s] packet id %u was not queued for session %u",
			__FUNCTION__, packetId, session->GetSessionID());
	}
}

void StreamingServer::OnSend(ISession* session, uint32_t bytesTransferred)
{
	ClientSession* clientSession = dynamic_cast<ClientSession*>(session);

	if (!clientSession)
	{
		__debugbreak();

		return;
	}
}

bool StreamingServer::HandleSubscribe(ClientSession* session, uint32_t streamId, const HandlerContext& context)
{
	if (!session || streamId != DESKTOP_STREAM_ID_PRIMARY)
		return false;

	DesktopStreamServerSessionContext* streamContext = dynamic_cast<DesktopStreamServerSessionContext*>(session->GetSessionContext());
	if (!streamContext)
	{
		streamContext = new (std::nothrow) DesktopStreamServerSessionContext();
		if (!streamContext)
			return false;

		session->SetSessionContext(streamContext);
	}

	streamContext->subscribed = true;
	streamContext->waitingForKeyframe = true;
	streamContext->streamId = streamId;
	streamContext->streamInfoVersion = m_streamInfoVersion;
	streamContext->codecConfigVersion = m_codecConfigVersion;

	if (!SendStreamInfoPacket(session, context))
	{
		streamContext->subscribed = false;
		return false;
	}

	if (!AddViewer(session))
	{
		streamContext->subscribed = false;
		return false;
	}

	return true;
}

uint32_t StreamingServer::GetSubscribedViewerSnapshot(ClientSession*** viewers)
{
	if (!viewers || !m_subscribedViewerSnapshot)
		return 0;

	if (::InterlockedCompareExchange(&m_viewerSnapshotDirty, FALSE, FALSE) != FALSE)
	{
		RebuildSubscribedViewerSnapshot();
	}

	*viewers = m_subscribedViewerSnapshot;
	return m_subscribedViewerSnapshotCount;
}

void StreamingServer::MarkViewerSnapshotDirty()
{
	::InterlockedExchange(&m_viewerSnapshotDirty, TRUE);
}

void StreamingServer::RebuildSubscribedViewerSnapshot()
{
	if (!m_subscribedViewerSnapshot || !m_viewers)
		return;

	::AcquireSRWLockExclusive(&m_viewerLock);

	if (::InterlockedCompareExchange(&m_viewerSnapshotDirty, FALSE, FALSE) == FALSE)
	{
		::ReleaseSRWLockExclusive(&m_viewerLock);
		return;
	}

	uint32_t snapshotCount = 0;

	for (uint32_t index = 0; index < m_viewerCount && snapshotCount < m_viewerCapacity; ++index)
	{
		ClientSession* viewer = m_viewers[index];
		if (!viewer || !viewer->IsEstablished())
			continue;

		DesktopStreamServerSessionContext* streamContext = dynamic_cast<DesktopStreamServerSessionContext*>(viewer->GetSessionContext());
		if (!streamContext || !streamContext->subscribed || streamContext->streamId != DESKTOP_STREAM_ID_PRIMARY)
			continue;

		m_subscribedViewerSnapshot[snapshotCount++] = viewer;
	}

	for (uint32_t index = snapshotCount; index < m_subscribedViewerSnapshotCount; ++index)
	{
		m_subscribedViewerSnapshot[index] = nullptr;
	}

	m_subscribedViewerSnapshotCount = snapshotCount;
	::InterlockedExchange(&m_viewerSnapshotDirty, FALSE);

	::ReleaseSRWLockExclusive(&m_viewerLock);
}

bool StreamingServer::HandleUnsubscribe(ClientSession* session, uint32_t streamId)
{
	if (!session || streamId != DESKTOP_STREAM_ID_PRIMARY)
		return false;

	DesktopStreamServerSessionContext* streamContext = dynamic_cast<DesktopStreamServerSessionContext*>(session->GetSessionContext());
	if (streamContext)
	{
		streamContext->subscribed = false;
		streamContext->waitingForKeyframe = true;
	}

	RemoveViewer(session);

	return true;
}

bool StreamingServer::InitializeViewerList(uint32_t maxConnectionCount)
{
	FinalizeViewerList();

	m_viewers = new ClientSession * [maxConnectionCount] {};
	if (!m_viewers)
		return false;

	m_subscribedViewerSnapshot = new ClientSession * [maxConnectionCount] {};
	if (!m_subscribedViewerSnapshot)
	{
		delete[] m_viewers;
		m_viewers = nullptr;
		return false;
	}

	m_viewerTakingFrame = new (std::nothrow) bool[maxConnectionCount] {};
	if (!m_viewerTakingFrame)
	{
		delete[] m_subscribedViewerSnapshot;
		m_subscribedViewerSnapshot = nullptr;
		delete[] m_viewers;
		m_viewers = nullptr;
		return false;
	}

	m_viewerCapacity = maxConnectionCount;
	m_viewerCount = 0;
	m_subscribedViewerSnapshotCount = 0;
	::InterlockedExchange(&m_viewerSnapshotDirty, TRUE);

	return true;
}

void StreamingServer::FinalizeViewerList()
{
	if (m_viewers)
	{
		delete[] m_viewers;
		m_viewers = nullptr;
	}

	if (m_subscribedViewerSnapshot)
	{
		delete[] m_subscribedViewerSnapshot;
		m_subscribedViewerSnapshot = nullptr;
	}

	if (m_viewerTakingFrame)
	{
		delete[] m_viewerTakingFrame;
		m_viewerTakingFrame = nullptr;
	}

	m_viewerCapacity = 0;
	m_viewerCount = 0;
	m_subscribedViewerSnapshotCount = 0;
	::InterlockedExchange(&m_viewerSnapshotDirty, TRUE);
}

bool StreamingServer::AddViewer(ClientSession* session)
{
	if (!session || !m_viewers)
		return false;

	::AcquireSRWLockExclusive(&m_viewerLock);

	for (uint32_t index = 0; index < m_viewerCount; ++index)
	{
		if (m_viewers[index] == session)
		{
			::ReleaseSRWLockExclusive(&m_viewerLock);
			return true;
		}
	}

	if (m_viewerCount >= m_viewerCapacity)
	{
		::ReleaseSRWLockExclusive(&m_viewerLock);
		return false;
	}

	m_viewers[m_viewerCount++] = session;
	MarkViewerSnapshotDirty();
	::ReleaseSRWLockExclusive(&m_viewerLock);
	return true;
}

void StreamingServer::RemoveViewer(ClientSession* session)
{
	if (!session || !m_viewers)
		return;

	::AcquireSRWLockExclusive(&m_viewerLock);

	for (uint32_t index = 0; index < m_viewerCount; ++index)
	{
		if (m_viewers[index] != session)
			continue;

		for (uint32_t moveIndex = index + 1; moveIndex < m_viewerCount; ++moveIndex)
		{
			m_viewers[moveIndex - 1] = m_viewers[moveIndex];
		}

		m_viewers[m_viewerCount - 1] = nullptr;
		--m_viewerCount;
		MarkViewerSnapshotDirty();
		break;
	}

	::ReleaseSRWLockExclusive(&m_viewerLock);
}

bool StreamingServer::SendStreamInfoPacket(ClientSession* session, const HandlerContext& context)
{
	if (!session)
		return false;

	void* packetMemory = MEMORY_POOL::CreatePacket(*context.packetMemoryPool, sizeof(CS_DESKTOP_STREAMING_INFO_PACKET));
	CS_DESKTOP_STREAMING_INFO_PACKET* infoPacket = reinterpret_cast<CS_DESKTOP_STREAMING_INFO_PACKET*>(packetMemory);
	if (!infoPacket)
		return false;

	*infoPacket = CS_DESKTOP_STREAMING_INFO_PACKET();
	infoPacket->streamId = DESKTOP_STREAM_ID_PRIMARY;
	infoPacket->streamInfoVersion = m_streamInfoVersion;
	infoPacket->codecConfigVersion = m_codecConfigVersion;
	infoPacket->codecType = static_cast<uint16_t>(m_codecType);
	infoPacket->width = m_streamWidth;
	infoPacket->height = m_streamHeight;
	infoPacket->fps = m_streamFps;

	void* sendData = infoPacket;
	if (!session->EnqueueSendPacket(&sendData, sizeof(CS_DESKTOP_STREAMING_INFO_PACKET)))
	{
		MEMORY_POOL::ReleasePacket(*context.packetMemoryPool, *context.generalMemoryPool, infoPacket);
		return false;
	}

	return true;
}
