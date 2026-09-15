#pragma once

#include "../../../../Module/IOCPNetworkEngine/Core/IOCPClient.h" // for IOCPClient
#include "../StreamingProtocol/StreamingPacket.h"

#ifdef BUILD_IOCP_STREAMING_CLIENT_DLL
#define IOCP_STREAMING_CLIENT_API __declspec(dllexport)
#else
#define IOCP_STREAMING_CLIENT_API __declspec(dllimport)
#endif

class ISession;

class IOCP_STREAMING_CLIENT_API StreamingClient : public IOCPClient
{
public:
	using StreamInfoCallback = void (*)(const DesktopStreamClientSessionContext& streamContext, void* userData);
	using FrameCallback = void (*)(const uint8_t* frameData, uint32_t frameSize, uint64_t frameId, uint64_t timestamp, uint16_t frameType, void* userData);

	StreamingClient();
	~StreamingClient() override;

public:
	bool StartClient(const char* ipAddress, const uint16_t port);
	void StopClient();
	bool SendSubscribe(uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY);
	bool SendUnsubscribe(uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY);
	void SetStreamInfoCallback(StreamInfoCallback callback, void* userData);
	void SetFrameCallback(FrameCallback callback, void* userData);
	bool HandleStreamInfo(const CS_DESKTOP_STREAMING_INFO_PACKET* infoPacket);
	bool HandleFrameComplete(const DesktopStreamClientSessionContext& streamContext);

private:
	void* GetServiceContext() override;
	void OnClientConnect(ISession* session) override;
	void OnSessionEstablished(ISession* session) override;
	void OnClientDisconnect(ISession* session, DisconnectReason reason) override;

	// 접속 자체가 실패했다. 서버가 안 떠 있으면 이쪽으로만 온다 —
	// OnClientConnect 를 부른 적이 없으므로 OnClientDisconnect 는 오지 않는다.
	void OnConnectFailed(int errorCode) override;

	void OnReceive(ISession* session, uint16_t packetId, const char* packetData, uint32_t packetSize) override;
	void OnSend(ISession* session, uint32_t bytesTransferred) override;

private:
	bool EnqueueSendPacket(void** packetData, uint32_t packetSize);

private:
	StreamInfoCallback m_streamInfoCallback = nullptr;
	void* m_streamInfoCallbackUserData = nullptr;
	FrameCallback m_frameCallback = nullptr;
	void* m_frameCallbackUserData = nullptr;
	bool m_handlersRegistered = false;
	bool m_subscribeWhenReady = false;
};
