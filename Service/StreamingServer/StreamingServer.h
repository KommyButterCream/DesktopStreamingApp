#pragma once

#include "../../../../Module/IOCPNetworkEngine/Core/IOCPServer.h" // for IOCPServer
#include "../StreamingProtocol/StreamSessionContext.h"

#ifdef BUILD_IOCP_STREAMING_SERVER_DLL
#define IOCP_STREAMING_SERVER_API __declspec(dllexport)
#else
#define IOCP_STREAMING_SERVER_API __declspec(dllimport)
#endif

class ISession;

class IOCP_STREAMING_SERVER_API StreamingServer : public IOCPServer
{
public:
	StreamingServer();
	~StreamingServer() override;

public:
	bool StartServer(const char* ipAddress, const uint16_t port, const uint32_t maxConnectionCount);
	void StopServer();
	void SetStreamInfo(uint16_t width, uint16_t height, uint16_t fps, DESKTOP_STREAM_CODEC_TYPE codecType);
	bool HasViewerWaitingForKeyframe();
	bool BroadcastEncodedFrame(const uint8_t* encodedData, uint32_t encodedSize, uint64_t frameId, uint64_t timestamp, uint16_t frameType, bool isKeyFrame);

private:
	void* GetServiceContext() override;
	void OnClientConnect(ISession* session) override;
	void OnClientDisconnect(ISession* session) override;
	void OnReceive(ISession* session, uint16_t packetId, const char* packetData, uint32_t packetSize) override;
	void OnSend(ISession* session, uint32_t bytesTransferred) override;


public:
	bool HandleSubscribe(ClientSession* session, uint32_t streamId, const HandlerContext& context);
	bool HandleUnsubscribe(ClientSession* session, uint32_t streamId);

private:
	bool InitializeViewerList(uint32_t maxConnectionCount);
	void FinalizeViewerList();

	bool AddViewer(ClientSession* session);
	void RemoveViewer(ClientSession* session);


	bool SendStreamInfoPacket(ClientSession* session, const HandlerContext& context);

private:
	struct SharedStreamPacket;

	static void ReleaseSharedStreamPacketCallback(const void* packetData, void* context);
	void AddRefSharedStreamPacket(SharedStreamPacket* sharedPacket);
	void ReleaseSharedStreamPacket(SharedStreamPacket* sharedPacket);
	uint32_t GetSubscribedViewerSnapshot(ClientSession*** viewers);
	void MarkViewerSnapshotDirty();
	void RebuildSubscribedViewerSnapshot();

private:
	ClientSession** m_viewers = nullptr;
	ClientSession** m_subscribedViewerSnapshot = nullptr;
	uint32_t m_viewerCapacity = 0;
	uint32_t m_viewerCount = 0;
	uint32_t m_subscribedViewerSnapshotCount = 0;
	volatile LONG m_viewerSnapshotDirty = TRUE;
	mutable SRWLOCK m_viewerLock = SRWLOCK_INIT;

	uint32_t m_streamInfoVersion = 1;
	uint32_t m_codecConfigVersion = 1;
	uint16_t m_streamWidth = 1280;
	uint16_t m_streamHeight = 720;
	uint16_t m_streamFps = 30;
	DESKTOP_STREAM_CODEC_TYPE m_codecType = DESKTOP_STREAM_CODEC_TYPE::H264;
};
