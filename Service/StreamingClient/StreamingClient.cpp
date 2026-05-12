#include "StreamingClient.h"

#include <new>

#include "../../../../Module/IOCPNetworkEngine/Job/Job.h"
#include "../../../../Module/IOCPNetworkEngine/HandlerTable/PacketHandlerTable.h"

#include "../../../../Module/IOCPNetworkEngine/Memory/SlabMemoryPoolHelper.h"

#include "../../../../Module/IOCPNetworkEngine/Protocol/PacketID.h"

#include "../../../../Module/IOCPNetworkEngine/Session/SessionJobQueue.h"
#include "../../../../Module/IOCPNetworkEngine/Session/ClientSession.h"

#include "ClientPacketHandler.h"

StreamingClient::StreamingClient()
	: m_requestSequence(0)
{
}

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

void StreamingClient::OnClientDisconnect(ISession* session)
{
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
	ClientSession* clientSession = dynamic_cast<ClientSession*>(session);
	if (!clientSession)
	{
		__debugbreak();
		return;
	}

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

	Job* job = MEMORY_POOL::CreateJob(*GetJobMemoryPool());
	if (!job)
	{
		__debugbreak();
		return;
	}

	job->SetPacketJob(JobType::PACKET, packetHandler, session, packetId, packetData, packetSize, GetHandlerContext());

	bool wasEmpty = false;
	clientSession->GetJobQueue().EnqueueJob(job, wasEmpty);
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
