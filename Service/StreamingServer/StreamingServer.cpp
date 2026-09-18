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

namespace
{
	// 세션에 붙은 스트림 컨텍스트를 꺼낸다.
	//
	// dynamic_cast 였다. 그런데 이 자리는 프레임 하나당 (뷰어 수 x 청크 수)
	// 번 지나간다 — 64뷰어 16청크 30fps 면 초당 3만 번이고, dynamic_cast 는
	// RTTI 를 걸어 타고 다니는 호출이라 그 규모에서는 공짜가 아니다.
	//
	// 그리고 그 비용으로 사는 안전성이 없다. 이 서버의 세션 컨텍스트를
	// 만드는 곳은 OnClientConnect 와 HandleSubscribe 두 곳뿐이고 둘 다
	// DesktopStreamServerSessionContext 를 넣는다. 다른 타입이 들어올
	// 경로가 아예 없으므로 검사할 것이 없다.
	//
	// nullptr 은 여전히 가능하다 — new (nothrow) 가 실패하면 컨텍스트가
	// 붙지 않은 세션이 남는다. 그건 호출부가 계속 확인한다.
	inline DesktopStreamServerSessionContext* GetStreamContext(ClientSession* session)
	{
		if (!session)
			return nullptr;

		return static_cast<DesktopStreamServerSessionContext*>(session->GetSessionContext());
	}

	inline void CountUp(volatile LONG64& counter, LONG64 amount = 1)
	{
		::InterlockedAdd64(&counter, amount);
	}

	inline uint64_t ReadCount(const volatile LONG64& counter)
	{
		return static_cast<uint64_t>(::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&counter), 0, 0));
	}
}

StreamingServer::StreamingServer()
{

}

StreamingServer::~StreamingServer()
{
	StopServer();
}

bool StreamingServer::StartServer(const char* ipAddress, const uint16_t port, const uint32_t maxConnectionCount,
	const SessionBufferConfig& bufferConfig, const EnginePoolConfig& poolConfig, const HeartbeatConfig& heartbeatConfig)
{
	// 엔진은 잘못된 설정을 받으면 기동을 거절한다. 그 거절이 "포트를 못
	// 열었다" 와 같은 false 로 도착하면 원인을 찾는 데 시간이 든다.
	// 우리 설정이 원인일 때는 여기서 먼저 말한다.
	if (!bufferConfig.IsValid())
	{
		Logger::Log(LogLevel::LOG_ERROR, "[%s] invalid session buffer config", __FUNCTION__);
		return false;
	}

	if (!poolConfig.IsValid())
	{
		Logger::Log(LogLevel::LOG_ERROR, "[%s] invalid engine pool config", __FUNCTION__);
		return false;
	}

	if (!heartbeatConfig.IsValid())
	{
		Logger::Log(LogLevel::LOG_ERROR, "[%s] invalid heartbeat config", __FUNCTION__);
		return false;
	}

	// 우리가 만드는 가장 큰 패킷을 우리 송신 상한이 받아주는지.
	// 여기서 틀리면 모든 청크가 EnqueueSharedSendPacket 에서 거절당해
	// "연결은 되는데 화면만 안 나오는" 상태가 된다.
	if (bufferConfig.maxSendPacketSize < DESKTOP_STREAM_CHUNK_PACKET_SIZE)
	{
		Logger::Log(LogLevel::LOG_ERROR, "[%s] maxSendPacketSize %u is smaller than the chunk packet size %u",
			__FUNCTION__, bufferConfig.maxSendPacketSize, DESKTOP_STREAM_CHUNK_PACKET_SIZE);
		return false;
	}

	if (!IOCPServer::StartServer(ipAddress, port, maxConnectionCount,
		bufferConfig, ConnectionPolicyConfig(), poolConfig, heartbeatConfig))
	{
		return false;
	}

	if (!InitializeViewerList(maxConnectionCount))
	{
		IOCPServer::StopServer();
		return false;
	}

	if (!PacketHandler::Server::RegisterHandlers(GetPacketHandlerTable()))
	{
		Logger::Log(LogLevel::LOG_ERROR, "[%s] failed to register packet handlers", __FUNCTION__);
		FinalizeViewerList();
		IOCPServer::StopServer();
		return false;
	}

	Logger::Log(LogLevel::LOG_INFO,
		"[%s] listening on %s:%u (max %u viewers, chunk %u bytes, send queue depth %u)",
		__FUNCTION__, ipAddress, port, maxConnectionCount,
		DESKTOP_STREAM_CHUNK_PACKET_SIZE, bufferConfig.sendQueueDepth);

	return true;
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

void StreamingServer::GetStats(DesktopStreamingServerStats& stats) const
{
	stats.framesOffered = ReadCount(m_statFramesOffered);
	stats.framesDelivered = ReadCount(m_statFramesDelivered);
	stats.framesSkippedNoViewer = ReadCount(m_statFramesSkippedNoViewer);
	stats.framesRejected = ReadCount(m_statFramesRejected);
	stats.framesAborted = ReadCount(m_statFramesAborted);
	stats.chunksEnqueued = ReadCount(m_statChunksEnqueued);
	stats.chunksFailed = ReadCount(m_statChunksFailed);
	stats.viewerFrameIncomplete = ReadCount(m_statViewerFrameIncomplete);
	stats.keyframesForced = ReadCount(m_statKeyframesForced);
	stats.viewerFramesCompleted = ReadCount(m_statViewerFramesCompleted);
	stats.viewerFramesDiscarded = ReadCount(m_statViewerFramesDiscarded);
	stats.viewerDecodeDropped = ReadCount(m_statViewerDecodeDropped);
	stats.feedbackReports = ReadCount(m_statFeedbackReports);
	stats.bytesQueued = ReadCount(m_statBytesQueued);

	stats.subscribedViewerCount =
		static_cast<uint32_t>(::InterlockedCompareExchange(const_cast<volatile LONG*>(&m_subscribedViewerCount), 0, 0));
}

void StreamingServer::ResetStats()
{
	::InterlockedExchange64(&m_statFramesOffered, 0);
	::InterlockedExchange64(&m_statFramesDelivered, 0);
	::InterlockedExchange64(&m_statFramesSkippedNoViewer, 0);
	::InterlockedExchange64(&m_statFramesRejected, 0);
	::InterlockedExchange64(&m_statFramesAborted, 0);
	::InterlockedExchange64(&m_statChunksEnqueued, 0);
	::InterlockedExchange64(&m_statChunksFailed, 0);
	::InterlockedExchange64(&m_statViewerFrameIncomplete, 0);
	::InterlockedExchange64(&m_statKeyframesForced, 0);
	::InterlockedExchange64(&m_statViewerFramesCompleted, 0);
	::InterlockedExchange64(&m_statViewerFramesDiscarded, 0);
	::InterlockedExchange64(&m_statViewerDecodeDropped, 0);
	::InterlockedExchange64(&m_statFeedbackReports, 0);
	::InterlockedExchange64(&m_lastForcedKeyFrameTick, 0);
	::InterlockedExchange64(&m_statBytesQueued, 0);
}

// 캡처 스레드가 매 프레임 부른다.
//
// 예전에는 여기서 GetSubscribedViewerSnapshot() 을 불렀다. 그 함수는
// 필요하면 스냅샷을 다시 만드는데, 같은 배열을 엔코더 완료 스레드가
// BroadcastEncodedFrame 안에서 락 없이 순회하고 있었다.
// (사정은 헤더의 GetSubscribedViewerSnapshot 주석)
//
// 이 질문에 답하는 데 스냅샷이 필요하지 않다. 목록을 바꾸는 쪽이 유지하는
// 카운터를 읽으면 된다.
bool StreamingServer::HasSubscribedViewer() const
{
	return ::InterlockedCompareExchange(const_cast<volatile LONG*>(&m_subscribedViewerCount), 0, 0) > 0;
}

// 이쪽도 캡처 스레드가 매 프레임 부른다. 이 답이 인코더의 IDR 강제를
// 좌우하므로 스냅샷(엔코더 스레드 전용)이 아니라 원본 목록을 본다.
//
// 공유 락 안에서 최대 maxConnectionCount 번 도는 순회다. 뷰어 수가
// 세 자리로 가면 카운터로 바꿔야 하지만, 그때는 대기 표시가 켜지고
// 꺼지는 모든 경로에서 카운터를 맞춰야 한다 — 지금 규모(수십)에서는
// 그 복잡도를 살 이유가 없다.
bool StreamingServer::HasViewerWaitingForKeyframe() const
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

		DesktopStreamServerSessionContext* streamContext = GetStreamContext(viewer);
		if (!streamContext || !streamContext->IsSubscribed() || streamContext->GetStreamId() != DESKTOP_STREAM_ID_PRIMARY)
			continue;

		if (streamContext->IsWaitingForKeyframe())
		{
			hasWaitingViewer = true;
			break;
		}
	}
	::ReleaseSRWLockShared(&m_viewerLock);

	return hasWaitingViewer;
}

void StreamingServer::SetMinKeyFrameIntervalMs(uint32_t intervalMs)
{
	::InterlockedExchange(&m_minKeyFrameIntervalMs, static_cast<LONG>(intervalMs));
}

uint32_t StreamingServer::GetMinKeyFrameIntervalMs() const
{
	return static_cast<uint32_t>(
		::InterlockedCompareExchange(const_cast<volatile LONG*>(&m_minKeyFrameIntervalMs), 0, 0));
}

// 인코드 스레드가 매 프레임 부른다. (사정은 헤더 주석)
//
// 시간 게이트를 뷰어 순회보다 먼저 본다. 대부분의 프레임은 여기서
// 걸러지고, 그때는 최대 64개를 도는 순회를 할 이유가 없다.
bool StreamingServer::ShouldForceKeyFrame()
{
	const LONG64 now = static_cast<LONG64>(::GetTickCount64());
	const LONG64 interval = static_cast<LONG64>(GetMinKeyFrameIntervalMs());
	const LONG64 last = ::InterlockedCompareExchange64(&m_lastForcedKeyFrameTick, 0, 0);

	if (last != 0 && interval > 0 && (now - last) < interval)
		return false;

	if (!HasViewerWaitingForKeyframe())
		return false;

	// 토큰을 실제로 가져간 스레드만 true 를 받는다. 지금은 인코드 스레드
	// 하나만 부르지만, 두 곳에서 부르면 프레임 하나에 IDR 요청이 두 번
	// 나가고 그중 하나는 낭비다.
	if (::InterlockedCompareExchange64(&m_lastForcedKeyFrameTick, now, last) != last)
		return false;

	CountUp(m_statKeyframesForced);
	return true;
}

void StreamingServer::SetStreamSelfHealing(bool enabled)
{
	::InterlockedExchange(&m_streamSelfHealing, enabled ? TRUE : FALSE);
}

bool StreamingServer::IsStreamSelfHealing() const
{
	return ::InterlockedCompareExchange(const_cast<volatile LONG*>(&m_streamSelfHealing), 0, 0) != FALSE;
}

// 프레임을 완성하지 못한 뷰어를 처리한다.
void StreamingServer::RearmKeyframeWait(DesktopStreamServerSessionContext* streamContext)
{
	if (!streamContext)
		return;

	// 완성하지 못했다는 사실 자체는 항상 센다. 비트레이트 적응이 이 값을
	// 혼잡 신호로 읽으므로, 아래에서 무엇을 하든 세는 것은 멈추지 않는다.
	CountUp(m_statViewerFrameIncomplete);

	// intra refresh 가 돌고 있으면 여기서 할 일이 없다.
	//
	// 이 뷰어는 이미 화면을 갖고 있고 일부 프레임만 놓쳤다. 뒤이어 오는
	// P 프레임을 그냥 받기만 해도 refresh 파도가 손상 영역을 덮어쓴다.
	// 실측으로 확인했다 — 90프레임(1.5초) 공백을 반복해서 넣어도 NVDEC 는
	// 오류 없이 계속 디코딩했고 IDR 없이 화면이 돌아왔다.
	//
	// 반대로 여기서 IDR 을 요구하면 대가가 크다. 그쪽 사정은
	// SetStreamSelfHealing 선언부에 적어 두었다.
	if (IsStreamSelfHealing())
		return;

	streamContext->SetWaitingForKeyframe(true);
}

bool StreamingServer::BroadcastEncodedFrame(const uint8_t* encodedData, uint32_t encodedSize, uint64_t frameId, uint64_t timestamp, uint16_t frameType, bool isKeyFrame)
{
	CountUp(m_statFramesOffered);

	if (!encodedData || encodedSize == 0 || !m_viewers || m_viewerCapacity == 0)
	{
		CountUp(m_statFramesRejected);
		return false;
	}

	if (encodedSize > DESKTOP_STREAM_MAX_FRAME_SIZE)
	{
		// 인코더가 우리 프레임 상한보다 큰 것을 냈다. 비트레이트나 해상도
		// 설정이 프로토콜과 어긋난 것이므로 한 장 버리고 끝날 일이 아니다.
		Logger::Log(LogLevel::LOG_ERROR, "[%s] encoded frame too large (%u > %u). raise DESKTOP_STREAM_MAX_FRAME_SIZE or lower the bitrate",
			__FUNCTION__, encodedSize, DESKTOP_STREAM_MAX_FRAME_SIZE);
		CountUp(m_statFramesRejected);
		return false;
	}

	ClientSession** viewers = nullptr;
	const uint32_t viewerCount = GetSubscribedViewerSnapshot(&viewers);
	if (viewerCount == 0)
	{
		CountUp(m_statFramesSkippedNoViewer);
		return true;
	}

	if (!m_viewerTakingFrame)
	{
		CountUp(m_statFramesRejected);
		return false;
	}

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

	// 청크 크기는 프로토콜이 정한다. 예전에는 이 자리에서 MEMORY_SIZE_32K 를
	// 직접 읽었고, 클라이언트의 수신 상한과는 아무 연결이 없었다.
	constexpr uint32_t chunkHeaderSize = DESKTOP_STREAM_CHUNK_HEADER_SIZE;
	constexpr uint32_t maxChunkDataSize = DESKTOP_STREAM_MAX_CHUNK_DATA_SIZE;

	const uint32_t chunkCount32 = (encodedSize + maxChunkDataSize - 1) / maxChunkDataSize;
	if (chunkCount32 > DESKTOP_STREAM_MAX_CHUNK_COUNT)
	{
		CountUp(m_statFramesRejected);
		return false;
	}

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
		{
			AbortFrameForTakingViewers(viewers, viewerCount, isKeyFrame);
			CountUp(m_statFramesAborted);
			return false;
		}

		void* packetMemory = MEMORY_POOL::CreatePacket(*GetPacketMemoryPool(), packetSize);
		SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET* framePacket = reinterpret_cast<SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET*>(packetMemory);
		if (!framePacket)
		{
			// 여기서 그냥 돌아가면 앞선 청크를 이미 받은 뷰어들이 미완성
			// 프레임을 안고 남는다. 키프레임이었다면 대기 표시를 되살려야
			// 다음 IDR 을 받는다.
			Logger::Log(LogLevel::LOG_WARNING, "[%s] packet pool exhausted at chunk %u/%u",
				__FUNCTION__, chunkIndex, chunkCount);
			AbortFrameForTakingViewers(viewers, viewerCount, isKeyFrame);
			CountUp(m_statFramesAborted);
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
			Logger::Log(LogLevel::LOG_WARNING, "[%s] send queue pool exhausted at chunk %u/%u",
				__FUNCTION__, chunkIndex, chunkCount);
			MEMORY_POOL::ReleasePacket(*GetPacketMemoryPool(), *GetGeneralMemoryPool(), framePacket);
			AbortFrameForTakingViewers(viewers, viewerCount, isKeyFrame);
			CountUp(m_statFramesAborted);
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

			// 스냅샷은 프레임 시작 시점의 사진이다. 그 사이에 이 뷰어가
			// 끊기고 같은 슬롯이 새 접속에 임대됐을 수 있다.
			//
			// 세션 객체 자체는 풀이 통째로 들고 있어 사라지지 않지만,
			// 새 접속은 OnClientConnect 에서 새 컨텍스트를 받으므로
			// subscribed 가 내려가 있다. 매 청크 그걸 확인한다 —
			// 구독한 적 없는 상대에게 프레임 조각을 흘리지 않기 위해서다.
			DesktopStreamServerSessionContext* streamContext = GetStreamContext(viewer);
			if (!streamContext || !streamContext->IsSubscribed() ||
				streamContext->GetStreamId() != DESKTOP_STREAM_ID_PRIMARY)
			{
				m_viewerTakingFrame[viewerIndex] = false;
				continue;
			}

			// 키프레임을 기다리는 뷰어에게 P 프레임은 의미가 없다. 실패가
			// 아니라 정상적인 건너뛰기이므로 대기 표시는 그대로 둔다.
			//
			// intra refresh 를 켜도 이 줄은 그대로다. 여기 걸리는 것은 아직
			// 첫 화면이 없는 뷰어뿐이고(신규 구독 / 스트림 정보 변경),
			// 그 뷰어에게는 참조 프레임도 SPS/PPS 도 없어서 refresh 파도로는
			// 아무것도 복원되지 않는다. 프레임을 놓쳤을 뿐인 뷰어는
			// RearmKeyframeWait 가 표시를 세우지 않으므로 여기 걸리지 않는다.
			if (streamContext->IsWaitingForKeyframe() && !isKeyFrame)
			{
				m_viewerTakingFrame[viewerIndex] = false;
				continue;
			}

			SHARED_SEND_PACKET::AddRef(sharedPacket);
			if (viewer->EnqueueSharedSendPacket(framePacket, packetSize, SHARED_SEND_PACKET::ReleaseCallback, sharedPacket))
			{
				CountUp(m_statChunksEnqueued);
				CountUp(m_statBytesQueued, static_cast<LONG64>(packetSize));
			}
			else
			{
				SHARED_SEND_PACKET::Release(sharedPacket);

				// 이 뷰어는 이 프레임을 완성할 수 없다. 앞서 보낸 청크는
				// 클라이언트가 버리고, 남은 청크는 보내지 않는다.
				//
				// 그리고 다음 키프레임부터 다시 시작해야 한다. 이 표시가
				// 인코더의 IDR 강제를 되살린다.
				m_viewerTakingFrame[viewerIndex] = false;
				CountUp(m_statChunksFailed);
				RearmKeyframeWait(streamContext);
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

		DesktopStreamServerSessionContext* streamContext = GetStreamContext(viewers[viewerIndex]);
		if (streamContext)
		{
			streamContext->SetWaitingForKeyframe(false);
		}
	}

	if (deliveredCount > 0)
	{
		CountUp(m_statFramesDelivered);
		return true;
	}

	return false;
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

		RearmKeyframeWait(GetStreamContext(viewers[viewerIndex]));
	}
}

void* StreamingServer::GetServiceContext()
{
	return this;
}

void StreamingServer::OnClientConnect(ISession* session)
{
	// 엔진이 이 콜백에 넘기는 것은 항상 ClientSession 이다. RECV / SEND
	// 완료 키에 등록되는 것이 그것뿐이기 때문이다(IOCPServer.h 주석).
	// dynamic_cast 로 매번 확인할 계약이 아니다.
	ClientSession* clientSession = static_cast<ClientSession*>(session);
	if (!clientSession)
		return;

	DesktopStreamServerSessionContext* streamContext = new (std::nothrow) DesktopStreamServerSessionContext();
	if (!streamContext)
	{
		// 컨텍스트 없이 붙은 세션은 구독할 수 없다. 조용히 넘어가면
		// "접속은 되는데 구독만 안 되는" 세션이 남으므로 남겨 둔다.
		Logger::Log(LogLevel::LOG_ERROR, "[%s] session %u could not allocate a stream context",
			__FUNCTION__, clientSession->GetSessionID());
		return;
	}

	clientSession->SetSessionContext(streamContext);
}

void StreamingServer::OnClientDisconnect(ISession* session, DisconnectReason reason)
{
	ClientSession* clientSession = static_cast<ClientSession*>(session);
	if (!clientSession)
		return;

	// 사유를 남긴다. 엔진이 넷을 구분해서 주는데(만석 거절 / 프로토콜 위반 /
	// 하트비트 타임아웃 / 정체) 서비스가 버리면 부하 테스트에서 "왜 뷰어가
	// 줄었는지" 를 알 수 없다.
	Logger::Log(LogLevel::LOG_INFO, "[%s] session %u disconnected : %s",
		__FUNCTION__, clientSession->GetSessionID(), ToString(reason));

	RemoveViewer(clientSession);
}

void StreamingServer::OnReceive(ISession* session, uint16_t packetId, const char* packetData, uint32_t packetSize)
{
	// 여기 이런 로그가 있었다.
	//
	//   const char* payload = packetData + sizeof(PACKET_HEADER);
	//   Logger::Log(LOG_INFO, "... Size: %d, %d, %s", ..., packetSize, payload);
	//
	// 두 가지가 잘못돼 있었다.
	//
	//   payload 를 %s 로 찍었다. 패킷 본문은 바이너리이고 NUL 로 끝나지
	//   않는다. printf 계열은 0 바이트를 만날 때까지 읽으므로 수신 링
	//   버퍼 밖으로 나간다 — 운이 좋아 안 죽었을 뿐인 경계 밖 읽기다.
	//
	//   그리고 LOG_INFO 다. 로거의 기본 레벨이 INFO 라 패킷 하나마다
	//   콘솔에 한 줄씩 나갔다. 부하 상황에서는 이 한 줄이 처리 자체보다
	//   비싸다.
	//
	// 남길 가치가 있는 것은 "무엇이 왔는가" 뿐이고, 그건 디버그 레벨이면
	// 충분하다.
	//
	// Logger::Log 가 아니라 LOGD 를 쓴다. 레벨 검사가 함수 안에 있으면
	// 로그가 꺼져 있어도 GetSessionID() 가 먼저 평가되는데, 그건 가상
	// 함수라 제거되지 않는다. 매크로는 꺼진 로그를 비교 한 번으로 줄인다.
	// (Logger.h 의 매크로 주석)
	LOGD("session %u recv packet id %u (%u bytes)", session->GetSessionID(), packetId, packetSize);

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
	// 여기에 dynamic_cast 와 __debugbreak 가 있었다. 송신 완료마다 지나가는
	// 자리에서 RTTI 를 걸었고, 캐스트가 실패하면 Release 빌드에서도 살아
	// 있는 __debugbreak 로 프로세스를 죽였다. 확인할 것도, 할 일도 없다.
	(void)session;
	(void)bytesTransferred;
}

bool StreamingServer::HandleSubscribe(ClientSession* session, uint32_t streamId, const HandlerContext& context)
{
	if (!session || streamId != DESKTOP_STREAM_ID_PRIMARY)
		return false;

	DesktopStreamServerSessionContext* streamContext = GetStreamContext(session);
	if (!streamContext)
	{
		streamContext = new (std::nothrow) DesktopStreamServerSessionContext();
		if (!streamContext)
			return false;

		session->SetSessionContext(streamContext);
	}

	// 목록에 먼저 넣고 그 다음 정보 패킷을 보낸다.
	//
	// 순서가 반대였다. 정보 패킷을 보낸 뒤 AddViewer 가 실패하면
	// (목록이 꽉 찼을 때) 클라이언트는 구독이 받아들여졌다고 믿고 영영
	// 오지 않는 프레임을 기다린다. 실패를 알 방법이 그쪽에 없다.
	//
	// 지금은 목록에 자리가 있다는 것을 확인한 뒤에만 승낙을 보낸다.
	streamContext->SetSubscribed(true);
	streamContext->SetWaitingForKeyframe(true);
	streamContext->SetStreamId(streamId);
	streamContext->streamInfoVersion = m_streamInfoVersion;
	streamContext->codecConfigVersion = m_codecConfigVersion;

	if (!AddViewer(session))
	{
		Logger::Log(LogLevel::LOG_WARNING, "[%s] session %u could not be added to the viewer list (full)",
			__FUNCTION__, session->GetSessionID());
		streamContext->SetSubscribed(false);
		return false;
	}

	if (!SendStreamInfoPacket(session, context))
	{
		RemoveViewer(session);
		streamContext->SetSubscribed(false);
		return false;
	}

	Logger::Log(LogLevel::LOG_INFO, "[%s] session %u subscribed to stream %u",
		__FUNCTION__, session->GetSessionID(), streamId);

	return true;
}

// 브로드캐스트 스레드 전용이다. 이 배열은 엔코더 완료 스레드 하나만
// 읽고 쓴다 — 다른 스레드가 이 함수에 들어오면 m_viewerTakingFrame 과의
// 인덱스 대응이 깨진다. (헤더 주석 참고)
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

// 원본 목록을 읽어 스냅샷을 다시 만든다.
//
// 공유 락이면 충분하다. 읽는 것은 m_viewers 뿐이고, 쓰는 곳
// (m_subscribedViewerSnapshot)은 이 스레드 말고 아무도 만지지 않는다.
// 예전에는 배타 락이었는데, 그건 이 함수가 여러 스레드에서 불릴 때를
// 가정한 것이었다. 그 전제가 없어졌으므로 뷰어 목록 변경을 불필요하게
// 막을 이유도 없다.
//
// 더티 플래그를 먼저 내린다. 락을 잡고 있는 사이에 들어온 변경은 다음
// 브로드캐스트가 다시 가져간다 — 나중에 내리면 그 변경을 통째로 놓친다.
void StreamingServer::RebuildSubscribedViewerSnapshot()
{
	if (!m_subscribedViewerSnapshot || !m_viewers)
		return;

	::InterlockedExchange(&m_viewerSnapshotDirty, FALSE);

	::AcquireSRWLockShared(&m_viewerLock);

	uint32_t snapshotCount = 0;

	for (uint32_t index = 0; index < m_viewerCount && snapshotCount < m_viewerCapacity; ++index)
	{
		ClientSession* viewer = m_viewers[index];
		if (!viewer || !viewer->IsEstablished())
			continue;

		DesktopStreamServerSessionContext* streamContext = GetStreamContext(viewer);
		if (!streamContext || !streamContext->IsSubscribed() || streamContext->GetStreamId() != DESKTOP_STREAM_ID_PRIMARY)
			continue;

		m_subscribedViewerSnapshot[snapshotCount++] = viewer;
	}

	::ReleaseSRWLockShared(&m_viewerLock);

	for (uint32_t index = snapshotCount; index < m_subscribedViewerSnapshotCount; ++index)
	{
		m_subscribedViewerSnapshot[index] = nullptr;
	}

	m_subscribedViewerSnapshotCount = snapshotCount;
}


// 뷰어가 보고한 수신 상태를 받는다. 잡 스케줄러 스레드에서 불린다.
//
// 값은 증분이므로 그대로 더하면 된다. 순번이 뒤로 가거나 같으면 중복
// 도착이므로 버린다 — TCP 라 순서는 보장되지만, 재접속으로 순번이 0 부터
// 다시 시작하는 경우가 있다.
bool StreamingServer::HandleFeedback(ClientSession* session, const CS_DESKTOP_STREAMING_FEEDBACK_PACKET& feedback)
{
	if (!session || feedback.streamId != DESKTOP_STREAM_ID_PRIMARY)
		return false;

	DesktopStreamServerSessionContext* streamContext = GetStreamContext(session);
	if (!streamContext)
		return false;

	// 재접속이면 순번이 작아진다. 그때는 받아들이고 기준을 다시 잡는다.
	const bool isStale =
		feedback.reportSequence <= streamContext->lastFeedbackSequence &&
		feedback.reportSequence != 0;

	if (isStale)
		return true;

	streamContext->lastFeedbackSequence = feedback.reportSequence;

	CountUp(m_statViewerFramesCompleted, static_cast<LONG64>(feedback.framesCompleted));
	CountUp(m_statViewerFramesDiscarded, static_cast<LONG64>(feedback.framesDiscarded));
	CountUp(m_statViewerDecodeDropped, static_cast<LONG64>(feedback.decodeDropped));
	CountUp(m_statFeedbackReports);

	LOGD("session %u feedback #%u : completed %u, discarded %u, decodeDropped %u",
		session->GetSessionID(), feedback.reportSequence,
		feedback.framesCompleted, feedback.framesDiscarded, feedback.decodeDropped);

	return true;
}

// 이미 구독 중인 뷰어들에게 스트림 정보를 다시 보낸다.
//
// 해상도가 바뀌면 인코더를 다시 만들고 SetStreamInfo 로 버전을 올리는데,
// 그것만으로는 이미 붙어 있는 뷰어가 아무것도 모른다. 옛 크기를 믿은 채
// 새 비트스트림을 받게 되고, 그건 디코더 쪽에서 조용히 깨진다.
//
// 정보를 보내면서 모두를 키프레임 대기로 되돌린다. 해상도가 바뀐 스트림은
// 이전 참조 프레임이 쓸모없으므로 IDR 부터 다시 시작해야 한다.
//
// 앱 스레드에서 부른다. 브로드캐스트 스레드와 겹칠 수 있으므로 뷰어 목록은
// 공유 락으로 훑는다.
uint32_t StreamingServer::BroadcastStreamInfo()
{
	if (!m_viewers || m_viewerCapacity == 0)
		return 0;

	const HandlerContext& context = GetHandlerContext();
	uint32_t notified = 0;

	::AcquireSRWLockShared(&m_viewerLock);

	for (uint32_t index = 0; index < m_viewerCount; ++index)
	{
		ClientSession* viewer = m_viewers[index];
		if (!viewer || !viewer->IsEstablished())
			continue;

		DesktopStreamServerSessionContext* streamContext = GetStreamContext(viewer);
		if (!streamContext || !streamContext->IsSubscribed())
			continue;

		streamContext->streamInfoVersion = m_streamInfoVersion;
		streamContext->codecConfigVersion = m_codecConfigVersion;
		RearmKeyframeWait(streamContext);

		if (SendStreamInfoPacket(viewer, context))
			++notified;
	}

	::ReleaseSRWLockShared(&m_viewerLock);

	Logger::Log(LogLevel::LOG_INFO, "[%s] stream info %ux%u@%u broadcast to %u viewer(s), version %u",
		__FUNCTION__, m_streamWidth, m_streamHeight, m_streamFps, notified, m_streamInfoVersion);

	return notified;
}
bool StreamingServer::HandleUnsubscribe(ClientSession* session, uint32_t streamId)
{
	if (!session || streamId != DESKTOP_STREAM_ID_PRIMARY)
		return false;

	DesktopStreamServerSessionContext* streamContext = GetStreamContext(session);
	if (streamContext)
	{
		streamContext->SetSubscribed(false);
		streamContext->SetWaitingForKeyframe(true);
	}

	RemoveViewer(session);

	Logger::Log(LogLevel::LOG_INFO, "[%s] session %u unsubscribed from stream %u",
		__FUNCTION__, session->GetSessionID(), streamId);

	return true;
}

bool StreamingServer::InitializeViewerList(uint32_t maxConnectionCount)
{
	FinalizeViewerList();

	if (maxConnectionCount == 0)
		return false;

	m_viewers = new (std::nothrow) ClientSession * [maxConnectionCount] {};
	if (!m_viewers)
		return false;

	m_subscribedViewerSnapshot = new (std::nothrow) ClientSession * [maxConnectionCount] {};
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
	::InterlockedExchange(&m_subscribedViewerCount, 0);
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
	::InterlockedExchange(&m_subscribedViewerCount, 0);
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

	// 캡처 스레드가 읽는 사본. 락 안에서 같이 움직여야 목록과 어긋나지 않는다.
	::InterlockedExchange(&m_subscribedViewerCount, static_cast<LONG>(m_viewerCount));

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

		::InterlockedExchange(&m_subscribedViewerCount, static_cast<LONG>(m_viewerCount));

		MarkViewerSnapshotDirty();
		break;
	}

	::ReleaseSRWLockExclusive(&m_viewerLock);
}

bool StreamingServer::SendStreamInfoPacket(ClientSession* session, const HandlerContext& context)
{
	if (!session || !context.packetMemoryPool || !context.generalMemoryPool)
		return false;

	void* packetMemory = MEMORY_POOL::CreatePacket(*context.packetMemoryPool, sizeof(SC_DESKTOP_STREAMING_INFO_PACKET));
	SC_DESKTOP_STREAMING_INFO_PACKET* infoPacket = reinterpret_cast<SC_DESKTOP_STREAMING_INFO_PACKET*>(packetMemory);
	if (!infoPacket)
		return false;

	*infoPacket = SC_DESKTOP_STREAMING_INFO_PACKET();
	infoPacket->streamId = DESKTOP_STREAM_ID_PRIMARY;
	infoPacket->streamInfoVersion = m_streamInfoVersion;
	infoPacket->codecConfigVersion = m_codecConfigVersion;
	infoPacket->codecType = static_cast<uint16_t>(m_codecType);
	infoPacket->width = m_streamWidth;
	infoPacket->height = m_streamHeight;
	infoPacket->fps = m_streamFps;

	void* sendData = infoPacket;
	if (!session->EnqueueSendPacket(&sendData, sizeof(SC_DESKTOP_STREAMING_INFO_PACKET)))
	{
		MEMORY_POOL::ReleasePacket(*context.packetMemoryPool, *context.generalMemoryPool, infoPacket);
		return false;
	}

	return true;
}
