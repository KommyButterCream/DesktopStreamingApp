#include "StreamingClient.h"

#include <new>

#include "../../../../Module/IOCPNetworkEngine/Job/Job.h"
#include "../../../../Module/IOCPNetworkEngine/HandlerTable/PacketHandlerTable.h"

#include "../../../../Module/IOCPNetworkEngine/Memory/EngineMemoryPoolHelper.h"

#include "../../../../Module/IOCPNetworkEngine/Protocol/PacketID.h"

#include "../../../../Module/IOCPNetworkEngine/Session/SessionJobQueue.h"
#include "../../../../Module/IOCPNetworkEngine/Session/ClientSession.h"

#include "ClientPacketHandler.h"

StreamingClient::StreamingClient() = default;

StreamingClient::~StreamingClient()
{
	StopClient();
}

bool StreamingClient::StartClient(const char* ipAddress, const uint16_t port)
{
	if (!IOCPClient::StartClient(ipAddress, port))
		return false;

	bool registerResult = true;
	registerResult &= PacketHandler::Client::RegisterHandlers(GetPacketHandlerTable());
	m_handlersRegistered = registerResult;

	ClientSession* clientSession = GetClientSession();
	if (m_handlersRegistered && m_subscribeWhenReady && clientSession && clientSession->IsEstablished())
	{
		m_subscribeWhenReady = false;
		SendSubscribe(DESKTOP_STREAM_ID_PRIMARY);
	}

	return registerResult;
}

void StreamingClient::StopClient()
{
	m_handlersRegistered = false;
	m_subscribeWhenReady = false;
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

bool StreamingClient::HandleStreamInfo(const CS_DESKTOP_STREAMING_INFO_PACKET* infoPacket)
{
	if (!infoPacket)
		return false;

	ClientSession* clientSession = GetClientSession();
	DesktopStreamClientSessionContext* streamContext = clientSession ? dynamic_cast<DesktopStreamClientSessionContext*>(clientSession->GetSessionContext()) : nullptr;
	if (!streamContext)
		return false;

	streamContext->subscribed = true;
	streamContext->hasStreamInfo = true;
	streamContext->streamId = infoPacket->streamId;
	streamContext->streamInfoVersion = infoPacket->streamInfoVersion;
	streamContext->codecConfigVersion = infoPacket->codecConfigVersion;
	streamContext->codecType = infoPacket->codecType;
	streamContext->width = infoPacket->width;
	streamContext->height = infoPacket->height;

	if (m_streamInfoCallback)
	{
		m_streamInfoCallback(*streamContext, m_streamInfoCallbackUserData);
	}

	return true;
}

bool StreamingClient::HandleFrameComplete(const DesktopStreamClientSessionContext& streamContext)
{
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
	ClientSession* clientSession = dynamic_cast<ClientSession*>(session);
	if (!clientSession)
		return;

	clientSession->SetSessionContext(new (std::nothrow) DesktopStreamClientSessionContext());
}

void StreamingClient::OnSessionEstablished(ISession* session)
{
	if (!m_handlersRegistered)
	{
		m_subscribeWhenReady = true;
		return;
	}

	SendSubscribe(DESKTOP_STREAM_ID_PRIMARY);
}

void StreamingClient::OnConnectFailed(int errorCode)
{
	Logger::Log(LogLevel::LOG_WARNING, "[%s] could not reach the server : WSA error %d",
		__FUNCTION__, errorCode);
}

void StreamingClient::OnClientDisconnect(ISession* session, DisconnectReason reason)
{
	// 사유를 남긴다. 재접속 정책을 붙이기 전이라도 "왜 끊겼는지" 가
	// 로그에 남아야 현장에서 판단할 수 있다.
	Logger::Log(LogLevel::LOG_INFO, "[%s] disconnected : %s (retryable=%d)",
		__FUNCTION__, ToString(reason), IsRetryableDisconnect(reason) ? 1 : 0);

	ClientSession* clientSession = dynamic_cast<ClientSession*>(session);
	if (!clientSession)
		return;

	DesktopStreamClientSessionContext* streamContext = dynamic_cast<DesktopStreamClientSessionContext*>(clientSession->GetSessionContext());
	if (!streamContext)
		return;

	streamContext->subscribed = false;
	streamContext->reassembling = false;
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
		Logger::Log(LogLevel::LOG_WARNING, "[%s] packet id %u was not queued", __FUNCTION__, packetId);
	}
}

void StreamingClient::OnSend(ISession* session, uint32_t bytesTransferred)
{
	ClientSession* clientSession = dynamic_cast<ClientSession*>(session);
	if (!clientSession)
	{
		__debugbreak();
		return;
	}
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
