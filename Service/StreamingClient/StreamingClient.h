#pragma once

#include "../../../../Module/IOCPNetworkEngine/Core/IOCPClient.h" // for IOCPClient
#include "../StreamingProtocol/StreamingPacket.h"
#include "../StreamingProtocol/StreamingConfig.h"

#ifdef BUILD_IOCP_STREAMING_CLIENT_DLL
#define IOCP_STREAMING_CLIENT_API __declspec(dllexport)
#else
#define IOCP_STREAMING_CLIENT_API __declspec(dllimport)
#endif

class ISession;

// 접속 상태가 바뀌었다.
//
// 예전에는 이 신호가 로그로만 나갔다. 그래서 앱은 서버가 죽었는지,
// 잠깐 끊긴 것인지, 애초에 붙지 못한 것인지 알 수 없었고 재접속 정책을
// 짤 수단이 없었다.
//
// 재접속 자체를 여기서 하지 않는 이유는, 이 콜백이 엔진 워커 스레드에서
// 불리기 때문이다. 그 안에서 StopClient / StartClient 를 부르면 자기
// 스레드의 종료를 자기가 기다리게 된다. 판단과 재시도는 앱의 루프가 한다.
enum class DESKTOP_STREAM_CONNECTION_EVENT : uint32_t
{
	Established,    // 인증까지 끝났다. 구독 요청이 나간 직후다
	ConnectFailed,  // 접속 자체가 성립하지 않았다 (errorCode 유효)
	Disconnected,   // 붙어 있다가 끊겼다 (reason 유효)
};

struct DesktopStreamingClientStats
{
	uint64_t chunksAccepted = 0;      // 재조립에 반영된 청크
	uint64_t chunksRejected = 0;      // 검증에서 걸러낸 청크
	uint64_t chunkBytesReceived = 0;  // 재조립에 반영된 바이트
	uint64_t framesCompleted = 0;     // 온전히 복원한 프레임
	uint64_t framesDiscarded = 0;     // 청크가 어긋나 버린 프레임
	uint64_t streamInfoReceived = 0;  // 스트림 정보 패킷 수

	bool connected = false;
};

class IOCP_STREAMING_CLIENT_API StreamingClient : public IOCPClient
{
public:
	using StreamInfoCallback = void (*)(const DesktopStreamClientSessionContext& streamContext, void* userData);
	using FrameCallback = void (*)(const uint8_t* frameData, uint32_t frameSize, uint64_t frameId, uint64_t timestamp, uint16_t frameType, void* userData);

	// event 가 Disconnected 면 reason 이, ConnectFailed 면 errorCode 가 유효하다.
	// 엔진 워커 스레드에서 불리므로 블로킹 작업을 하면 안 된다.
	using ConnectionCallback = void (*)(DESKTOP_STREAM_CONNECTION_EVENT event, DisconnectReason reason, int errorCode, void* userData);

	StreamingClient();
	~StreamingClient() override;

public:
	// 엔진 설정은 이 서비스의 프리셋을 쓴다. 기본값이 무엇을 바꾸는지는
	// StreamingConfig.h 참고. 부하 테스트에서 바꿔 가며 재려고 인자로 연다.
	bool StartClient(const char* ipAddress, const uint16_t port,
		const SessionBufferConfig& bufferConfig = DesktopStreamingPreset::ClientSessionBuffer(),
		const EnginePoolConfig& poolConfig = DesktopStreamingPreset::ClientPool());
	void StopClient();

	bool SendSubscribe(uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY);
	bool SendUnsubscribe(uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY);

	// 수신 상태를 서버에 알린다. 앱이 주기적으로 부른다(1초 권장).
	//
	// 직전 호출 이후의 증분을 자동으로 계산해서 보낸다. 누적을 보내면
	// 재접속으로 카운터가 0 이 될 때 서버 쪽 델타가 음수가 된다.
	//
	// decodeDropped 는 이 서비스가 모르는 값이라 앱이 넘긴다
	// (디코더 통계는 앱이 들고 있다).
	bool SendFeedback(uint64_t decodeDroppedTotal, uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY);

	void SetStreamInfoCallback(StreamInfoCallback callback, void* userData);
	void SetFrameCallback(FrameCallback callback, void* userData);
	void SetConnectionCallback(ConnectionCallback callback, void* userData);

	// 세션이 인증까지 끝냈는가. 앱의 재접속 루프가 읽는다.
	bool IsConnected() const;

	void GetStats(DesktopStreamingClientStats& stats) const;
	void ResetStats();

public:
	// --- 패킷 핸들러가 부르는 진입점 ---
	bool HandleStreamInfo(const SC_DESKTOP_STREAMING_INFO_PACKET* infoPacket);
	bool HandleFrameComplete(const DesktopStreamClientSessionContext& streamContext);

	void NoteChunkAccepted(uint32_t chunkBytes);
	void NoteChunkRejected();
	void NoteFrameDiscarded();

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
	void NotifyConnectionEvent(DESKTOP_STREAM_CONNECTION_EVENT event, DisconnectReason reason, int errorCode);

private:
	StreamInfoCallback m_streamInfoCallback = nullptr;
	void* m_streamInfoCallbackUserData = nullptr;
	FrameCallback m_frameCallback = nullptr;
	void* m_frameCallbackUserData = nullptr;
	ConnectionCallback m_connectionCallback = nullptr;
	void* m_connectionCallbackUserData = nullptr;

	// 평범한 bool 이었다. 그런데 이 둘은 서로 다른 스레드가 읽고 쓴다.
	//
	//   StartClient   : 앱 스레드. 핸들러 등록을 마치고 m_handlersRegistered 를 세운다
	//   OnSessionEstablished : IOCP 워커. 그 값을 보고 구독을 보낼지 미룰지 정한다
	//
	// 접속이 빠르면 두 일이 실제로 겹친다. 여기서 어긋나면 구독 요청이
	// 나가지 않고, 증상은 "붙었는데 프레임이 안 온다" 다.
	volatile LONG m_handlersRegistered = FALSE;
	volatile LONG m_subscribeWhenReady = FALSE;

	volatile LONG64 m_statChunksAccepted = 0;
	volatile LONG64 m_statChunksRejected = 0;
	volatile LONG64 m_statChunkBytesReceived = 0;
	volatile LONG64 m_statFramesCompleted = 0;
	volatile LONG64 m_statFramesDiscarded = 0;
	volatile LONG64 m_statStreamInfoReceived = 0;

	// 피드백 증분 계산용. 마지막으로 보고한 누적값을 기억한다.
	// 앱 스레드에서만 만진다.
	uint64_t m_reportedFramesCompleted = 0;
	uint64_t m_reportedFramesDiscarded = 0;
	uint64_t m_reportedDecodeDropped = 0;
};
