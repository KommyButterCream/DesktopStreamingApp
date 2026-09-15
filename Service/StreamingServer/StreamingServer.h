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
	bool HasSubscribedViewer();
	bool HasViewerWaitingForKeyframe();
	bool BroadcastEncodedFrame(const uint8_t* encodedData, uint32_t encodedSize, uint64_t frameId, uint64_t timestamp, uint16_t frameType, bool isKeyFrame);

private:
	void* GetServiceContext() override;
	void OnClientConnect(ISession* session) override;
	void OnClientDisconnect(ISession* session, DisconnectReason reason) override;
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
	// 여기에 SharedStreamPacket 구조체와 AddRef / Release / 정적 콜백이
	// 있었다. 엔진의 SharedSendPacket(Buffer/SharedSendPacket.h)이 같은 일을
	// 하므로 그쪽을 쓴다.
	//
	// 옮긴 이유는 재사용이 아니라 두 가지 결함이었다.
	//   - 청크마다 new / delete 를 했다. 엔진이 패킷 경로에서 걷어낸 힙이
	//     브로드캐스트 팬아웃 지점에서 그대로 돌아와 있었다.
	//   - 서버 객체(this)를 들고 있다가 반납 시점에 GetPacketMemoryPool() 을
	//     불렀다. 늦게 도착한 반납이 StopServer 뒤면 죽은 풀을 역참조한다.
	// 엔진판은 서버가 아니라 풀 자체를 들고, 자기도 풀에서 나온다.
	// 프레임 전송을 중간에 포기할 때, 앞선 청크를 이미 받은 뷰어들의
	// 대기 표시를 되살린다. (사정은 구현부 주석)
	void AbortFrameForTakingViewers(ClientSession** viewers, uint32_t viewerCount, bool isKeyFrame);

	uint32_t GetSubscribedViewerSnapshot(ClientSession*** viewers);
	void MarkViewerSnapshotDirty();
	void RebuildSubscribedViewerSnapshot();

private:
	ClientSession** m_viewers = nullptr;
	ClientSession** m_subscribedViewerSnapshot = nullptr;

	// 지금 브로드캐스트 중인 프레임을 아직 끝까지 받고 있는 뷰어 표시.
	// 스냅샷과 같은 순서, 같은 길이다.
	//
	// 프레임을 청크로 쪼개 보내는데 루프가 청크 바깥 / 뷰어 안쪽이라,
	// "이 뷰어는 3번 청크에서 막혔으니 나머지도 보내지 말자" 를 기억할
	// 곳이 필요하다. BroadcastEncodedFrame 은 인코더 스레드 하나에서만
	// 불리므로 멤버 배열로 충분하다.
	bool* m_viewerTakingFrame = nullptr;

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
