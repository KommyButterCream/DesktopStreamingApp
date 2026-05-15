#include "StreamingServer.h"

#include "../../../../Module/IOCPNetworkEngine/Job/Job.h" 
#include "../../../../Module/IOCPNetworkEngine/HandlerTable/PacketHandlerTable.h" 
#include "../../../../Module/IOCPNetworkEngine/Scheduler/ReadySessionQueue.h" 

#include "../../../../Module/IOCPNetworkEngine/Buffer/PreDefine.h"
#include "../../../../Module/IOCPNetworkEngine/Memory/SlabMemoryPoolHelper.h" 

#include "../../../../Module/IOCPNetworkEngine/Protocol/PacketID.h" 

#include "../../../../Module/IOCPNetworkEngine/Session/SessionJobQueue.h" 
#include "../../../../Module/IOCPNetworkEngine/Session/ClientSession.h" // for ClientSession

#include "ServerPacketHandler.h"
#include "../StreamingProtocol/StreamingPacket.h"

#include <cstring>
#include <new>

using namespace Core::Util;

struct StreamingServer::SharedStreamPacket
{
	volatile LONG refCount = 1;
	StreamingServer* owner = nullptr;
	void* packet = nullptr;
};

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

	constexpr uint32_t chunkHeaderSize = GetDesktopStreamingFrameChunkHeaderSize();
	const uint32_t maxChunkDataSize = MEMORY_SIZE_32K - chunkHeaderSize;
	if (maxChunkDataSize == 0)
		return false;

	const uint32_t chunkCount32 = (encodedSize + maxChunkDataSize - 1) / maxChunkDataSize;
	if (chunkCount32 > USHRT_MAX)
		return false;

	const uint16_t chunkCount = static_cast<uint16_t>(chunkCount32);
	uint32_t enqueuedCount = 0;

	for (uint16_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
	{
		const uint32_t chunkOffset = static_cast<uint32_t>(chunkIndex) * maxChunkDataSize;
		const uint32_t remainingSize = encodedSize - chunkOffset;
		const uint32_t chunkDataSize = (remainingSize < maxChunkDataSize) ? remainingSize : maxChunkDataSize;
		const uint16_t packetSize = MakeDesktopStreamingVariablePacketSize(chunkHeaderSize, chunkDataSize);
		if (packetSize == 0)
			return false;

		void* packetMemory = MEMORY_POOL::CreatePacket(*GetPacketMemoryPool(), packetSize);
		SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET* framePacket = reinterpret_cast<SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET*>(packetMemory);
		if (!framePacket)
			return false;

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

		SharedStreamPacket* sharedPacket = new (std::nothrow) SharedStreamPacket();
		if (!sharedPacket)
		{
			MEMORY_POOL::ReleasePacket(*GetPacketMemoryPool(), *GetGeneralMemoryPool(), framePacket);
			return false;
		}

		sharedPacket->owner = this;
		sharedPacket->packet = framePacket;

		for (uint32_t viewerIndex = 0; viewerIndex < viewerCount; ++viewerIndex)
		{
			ClientSession* viewer = viewers[viewerIndex];
			if (!viewer)
				continue;

			AddRefSharedStreamPacket(sharedPacket);
			if (viewer->EnqueueSharedSendPacket(framePacket, packetSize, ReleaseSharedStreamPacketCallback, sharedPacket))
			{
				++enqueuedCount;
				if (isKeyFrame && chunkIndex == 0)
				{
					DesktopStreamServerSessionContext* streamContext = dynamic_cast<DesktopStreamServerSessionContext*>(viewer->GetSessionContext());
					if (streamContext && streamContext->streamId == DESKTOP_STREAM_ID_PRIMARY)
					{
						streamContext->waitingForKeyframe = false;
					}
				}
			}
			else
			{
				ReleaseSharedStreamPacket(sharedPacket);
			}
		}

		ReleaseSharedStreamPacket(sharedPacket);
	}

	return enqueuedCount > 0;
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

void StreamingServer::OnClientDisconnect(ISession* session)
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

	PacketHandlerTable* packetHandlerTable = GetPacketHandlerTable();
	if (!packetHandlerTable)
	{
		__debugbreak();

		return;
	}

	PacketHandlerFunc packetHandler = packetHandlerTable->GetHandler(packetId);
	if (!packetHandler)
	{
		__debugbreak();

		return;
	}

	// Job 생성
	Job* job = MEMORY_POOL::CreateJob(*GetJobMemoryPool());
	if (!job)
	{
		__debugbreak();

		return;
	}

	job->SetPacketJob(JobType::PACKET, packetHandler, session, packetId, packetData, packetSize, GetHandlerContext());

	bool wasEmpty = false;
	bool enqueueSucceeded = clientSession->GetJobQueue().EnqueueJob(job, wasEmpty);

	if (wasEmpty && enqueueSucceeded)
	{
		// JobQueue 가 비어있었다면 ReadySessionQueue 에 Session 을 스케줄링 하기 위해 Push 해주어야 한다.
		// 중복으로 Session 을 Push 하는 것을 방지 하기 위해 Processing 플래그를 Atomic 하게 검사.
		if (clientSession->IsProcessingReady())
		{
			// Session 이 Idle 상태인 경우
			// ReadySessionQueue 에 Push 하자!
			if (!GetReadySessionQueue()->Push(clientSession))
			{
				// Push 실패한 경우 플래그를 원상 복구하고 로그 처리 하자.
				clientSession->UpdateProcessingFlag(0);
			}
		}
		else
		{
			// 이미 Session 이 Processing 중 일 때
		}
	}
	else
	{
		if (!enqueueSucceeded)
		{
			// EnqueueJob 에 실패한 경우
			// log...
		}

		if (enqueueSucceeded && !wasEmpty)
		{
			// 이미 Session 이 Processing 중 일 때
		}
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

void StreamingServer::ReleaseSharedStreamPacketCallback(const void* packetData, void* context)
{
	SharedStreamPacket* sharedPacket = static_cast<SharedStreamPacket*>(context);
	if (!sharedPacket || !sharedPacket->owner)
		return;

	sharedPacket->owner->ReleaseSharedStreamPacket(sharedPacket);
}

void StreamingServer::AddRefSharedStreamPacket(SharedStreamPacket* sharedPacket)
{
	if (!sharedPacket)
		return;

	::InterlockedIncrement(&sharedPacket->refCount);
}

void StreamingServer::ReleaseSharedStreamPacket(SharedStreamPacket* sharedPacket)
{
	if (!sharedPacket)
		return;

	const LONG refCount = ::InterlockedDecrement(&sharedPacket->refCount);
	if (refCount != 0)
		return;

	MEMORY_POOL::ReleasePacket(*GetPacketMemoryPool(), *GetGeneralMemoryPool(), sharedPacket->packet);
	sharedPacket->packet = nullptr;
	delete sharedPacket;
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
