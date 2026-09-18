#pragma once

#include "../../../../Module/IOCPNetworkEngine/Core/IOCPServer.h" // for IOCPServer
#include "../StreamingProtocol/StreamSessionContext.h"
#include "../StreamingProtocol/StreamingConfig.h"

#ifdef BUILD_IOCP_STREAMING_SERVER_DLL
#define IOCP_STREAMING_SERVER_API __declspec(dllexport)
#else
#define IOCP_STREAMING_SERVER_API __declspec(dllimport)
#endif

class ISession;

// 브로드캐스트 경로의 누적 지표.
//
// 이게 없으면 "프레임이 안 보인다" 는 관찰에서 갈 수 있는 곳이 없다.
// 인코더가 안 준 것인지, 볼 사람이 없어서 버린 것인지, 큐가 차서 못 민
// 것인지, 풀이 말라서 포기한 것인지가 전부 다른 처방이다.
//
// 부하 테스트가 읽는 값이기도 하다. (tools 의 부하 시나리오)
struct DesktopStreamingServerStats
{
	uint64_t framesOffered = 0;          // BroadcastEncodedFrame 호출 수
	uint64_t framesDelivered = 0;        // 한 명 이상에게 온전히 들어간 프레임
	uint64_t framesSkippedNoViewer = 0;  // 구독자가 없어 그냥 버린 프레임
	uint64_t framesRejected = 0;         // 크기 위반 등으로 아예 거절한 프레임
	uint64_t framesAborted = 0;          // 풀 고갈로 중간에 포기한 프레임
	uint64_t chunksEnqueued = 0;         // 뷰어 송신 큐에 들어간 청크 수
	uint64_t chunksFailed = 0;           // 큐 포화 등으로 못 넣은 청크 수
	uint64_t viewerFrameIncomplete = 0;  // 뷰어가 프레임을 완성하지 못한 횟수
	uint64_t keyframesForced = 0;        // 실제로 IDR 을 강제한 횟수

	// 뷰어가 스스로 보고한 값의 합. 서버의 송신 실패(chunksFailed)와는
	// 다른 신호다 — 이쪽은 "디코더가 못 따라간다" 를 뜻한다.
	uint64_t viewerFramesCompleted = 0;
	uint64_t viewerFramesDiscarded = 0;
	uint64_t viewerDecodeDropped = 0;
	uint64_t feedbackReports = 0;
	uint64_t bytesQueued = 0;            // 큐에 들어간 청크 바이트 합

	uint32_t subscribedViewerCount = 0;  // 지금 목록에 있는 뷰어 수
};

class IOCP_STREAMING_SERVER_API StreamingServer : public IOCPServer
{
public:
	StreamingServer();
	~StreamingServer() override;

public:
	// 엔진 설정은 이 서비스의 프리셋을 쓴다. 예전에는 인자를 하나도 넘기지
	// 않아 범용 기본값으로 돌았고, 그 기본값은 실시간 스트리밍을 가정하지
	// 않는다. (무엇이 어떻게 어긋나 있었는지는 StreamingConfig.h)
	//
	// 프리셋을 바꿔 가며 재는 것이 부하 테스트의 목적이므로 인자로 연다.
	bool StartServer(const char* ipAddress, const uint16_t port, const uint32_t maxConnectionCount,
		const SessionBufferConfig& bufferConfig = DesktopStreamingPreset::ServerSessionBuffer(),
		const EnginePoolConfig& poolConfig = DesktopStreamingPreset::ServerPool(),
		const HeartbeatConfig& heartbeatConfig = DesktopStreamingPreset::ServerHeartbeat());
	void StopServer();

	// 스트림 정보를 바꾼다. 여기서 올린 버전이 구독 응답과 모든 청크에
	// 실려 나간다.
	//
	// 기동 뒤에 불러도 된다 (해상도 변경). 다만 이미 붙어 있는 뷰어는
	// 이 호출만으로는 아무것도 모르므로, 뒤이어 BroadcastStreamInfo() 를
	// 불러야 한다.
	void SetStreamInfo(uint16_t width, uint16_t height, uint16_t fps, DESKTOP_STREAM_CODEC_TYPE codecType);

	// 캡처 스레드가 매 프레임 부른다. 락도 순회도 하지 않는다.
	// (그 사정은 m_subscribedViewerCount 선언부 주석)
	bool HasSubscribedViewer() const;

	// 키프레임을 기다리는 뷰어가 있는가. 사실을 그대로 답한다.
	bool HasViewerWaitingForKeyframe() const;

	// 지금 IDR 을 강제할 것인가. 위 질문에 최소 간격을 씌운 것이다.
	//
	// 왜 따로 두는가
	//   인코더는 이 답을 매 프레임 묻는다(EncodeThread::QueryKeyFrameRequest).
	//   HasViewerWaitingForKeyframe() 을 그대로 물려 두면 뷰어 하나가 대기
	//   상태로 남아 있는 동안 '모든 프레임이 IDR' 이 된다.
	//
	//   그게 왜 나쁜가. 인코더는 CBR 이고 UltraLow 모드에서
	//   lowDelayKeyFrameScale=1 이라 IDR 도 한 프레임 예산을 넘지 못한다.
	//   즉 비트레이트가 튀는 대신 '화질이 무너진다' — 전부 intra 로 채운
	//   프레임을 P 프레임과 같은 비트로 만들어야 하기 때문이다.
	//   밀린 뷰어 하나가 모든 뷰어의 화면을 깎는다.
	//
	//   그리고 그 자체가 되먹임이다. 프레임이 커지고 화질이 나빠지는 동안
	//   밀린 뷰어는 더 밀리고, 더 밀리면 대기 표시가 다시 서고, 그래서 또
	//   모든 프레임이 IDR 이 된다.
	//
	// 왜 그냥 막지 않는가
	//   intra refresh 를 켜면 인코더가 idrPeriod 를 무한으로 둔다
	//   (D3D11NvEncoder_Impl 의 SDK 제약 주석). 주기적 IDR 이 아예 없으므로
	//   강제가 유일한 IDR 원천이고, 막으면 새로 붙은 뷰어가 영영 첫 화면을
	//   보지 못한다.
	//
	// 기본 간격 1초는 intraRefreshPeriodFrames(60프레임 @60fps)와 같다.
	// 최악의 경우 뷰어가 1초 기다리고 첫 화면을 받는다.
	bool ShouldForceKeyFrame();
	void SetMinKeyFrameIntervalMs(uint32_t intervalMs);
	uint32_t GetMinKeyFrameIntervalMs() const;

	// 스트림이 스스로 회복하는가 — 인코더의 intra refresh 가 켜져 있는가.
	//
	// 켜져 있으면 프레임을 놓친 뷰어에게 IDR 을 요구하지 않는다. 그 뷰어는
	// P 프레임을 계속 받기만 해도 refresh 파도가 한 바퀴 도는 동안 화면이
	// 맞춰진다. 그래서 여기서 할 일이 없다.
	//
	// 요구하면 오히려 나쁘다. UltraLow 는 lowDelayKeyFrameScale=1 이라
	// IDR 도 한 프레임 예산을 넘지 못하므로, 화면 전체를 intra 로 채운
	// 프레임이 P 프레임과 같은 비트로 만들어진다. 비트레이트가 튀는 대신
	// 화질이 무너지고, 스트림이 하나라 그 손해를 모든 뷰어가 나눠 진다.
	//
	// 꺼져 있으면 주기적 IDR 구성이므로 예전처럼 다음 키프레임을 기다린다.
	//
	// 첫 화면이 없는 뷰어는 이 설정과 무관하게 항상 IDR 이 필요하다.
	// 참조 프레임도 SPS/PPS 도 없어 P 프레임으로는 아무것도 못 한다.
	// 그쪽은 Subscribe / BroadcastStreamInfo 가 따로 표시를 세운다.
	void SetStreamSelfHealing(bool enabled);
	bool IsStreamSelfHealing() const;

	// 브로드캐스트 스레드 전용. 엔코더 완료 스레드 하나만 부른다.
	bool BroadcastEncodedFrame(const uint8_t* encodedData, uint32_t encodedSize, uint64_t frameId, uint64_t timestamp, uint16_t frameType, bool isKeyFrame);

	// 스트림 정보가 바뀌었음을 이미 구독 중인 뷰어들에게 알린다.
	//
	// SetStreamInfo 는 버전만 올릴 뿐 이미 붙어 있는 뷰어에게는 아무것도
	// 보내지 않았다. 그래서 해상도가 바뀌면 뷰어는 옛 크기를 믿은 채
	// 새 비트스트림을 받았다. 이 함수가 그 구멍을 메운다.
	//
	// 정보를 다시 보내면서 모든 뷰어를 키프레임 대기로 되돌린다 —
	// 해상도가 바뀐 스트림은 이전 참조 프레임이 쓸모없다.
	uint32_t BroadcastStreamInfo();

	void GetStats(DesktopStreamingServerStats& stats) const;
	void ResetStats();

private:
	void* GetServiceContext() override;
	void OnClientConnect(ISession* session) override;
	void OnClientDisconnect(ISession* session, DisconnectReason reason) override;
	void OnReceive(ISession* session, uint16_t packetId, const char* packetData, uint32_t packetSize) override;
	void OnSend(ISession* session, uint32_t bytesTransferred) override;


public:
	bool HandleSubscribe(ClientSession* session, uint32_t streamId, const HandlerContext& context);
	bool HandleUnsubscribe(ClientSession* session, uint32_t streamId);
	bool HandleFeedback(ClientSession* session, const CS_DESKTOP_STREAMING_FEEDBACK_PACKET& feedback);

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

	// --- 브로드캐스트 스레드 전용 ---
	//
	// 이 둘은 엔코더 완료 스레드 하나만 부른다. 그 전제 위에서 스냅샷
	// 배열을 락 없이 읽는다.
	//
	// 예전에는 HasSubscribedViewer() 도 이 경로로 들어왔고, 그쪽은 캡처
	// 스레드가 매 프레임 불렀다. 그래서 캡처 스레드가 스냅샷을 다시 만드는
	// 동안 엔코더 스레드가 같은 배열을 순회했다. 두 배열의 인덱스가
	// m_viewerTakingFrame 과 짝지어져 있으므로, 재빌드 한 번이 "3번 뷰어가
	// 이 프레임을 받고 있다" 는 표시를 엉뚱한 뷰어에게 옮긴다.
	//
	// 지금은 캡처 스레드가 이 경로에 들어오지 않는다. 구독자 수는 원자
	// 카운터로 따로 들고, 대기 여부는 뷰어 목록을 공유 락으로 훑는다.
	uint32_t GetSubscribedViewerSnapshot(ClientSession*** viewers);
	void RebuildSubscribedViewerSnapshot();

	void MarkViewerSnapshotDirty();

	// 대기 표시를 세우면서 그 사실을 센다. 되살린 횟수가 곧 "그 뷰어가
	// 프레임을 완성하지 못한 횟수" 이고, 부하 테스트에서 가장 먼저 보는 값이다.
	void RearmKeyframeWait(DesktopStreamServerSessionContext* streamContext);

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

	// m_viewerCount 의 원자 사본.
	//
	// 캡처 스레드는 매 프레임 "볼 사람이 있는가" 를 묻는다. 그 답 하나
	// 때문에 락을 잡거나 스냅샷을 다시 만들 이유가 없다. 목록을 바꾸는
	// 쪽(AddViewer / RemoveViewer)이 락 안에서 이 값을 같이 갱신한다.
	//
	// 이 카운터는 "목록에 있는 뷰어 수" 이고, 스냅샷 쪽은 거기서
	// established / subscribed / streamId 까지 확인한 수다. 둘이 잠깐
	// 어긋날 수 있다 — 그래도 되는 값이다. 여기서 틀리면 캡처 한 장을
	// 헛돌리거나 한 장 건너뛸 뿐이고, 다음 프레임이 바로잡는다.
	volatile LONG m_subscribedViewerCount = 0;

	// 지표. 브로드캐스트 스레드가 대부분 올리지만 구독/해지 경로도 만지므로
	// 전부 원자 연산을 쓴다.
	volatile LONG64 m_statFramesOffered = 0;
	volatile LONG64 m_statFramesDelivered = 0;
	volatile LONG64 m_statFramesSkippedNoViewer = 0;
	volatile LONG64 m_statFramesRejected = 0;
	volatile LONG64 m_statFramesAborted = 0;
	volatile LONG64 m_statChunksEnqueued = 0;
	volatile LONG64 m_statChunksFailed = 0;
	volatile LONG64 m_statViewerFrameIncomplete = 0;
	volatile LONG64 m_statKeyframesForced = 0;
	volatile LONG64 m_statViewerFramesCompleted = 0;
	volatile LONG64 m_statViewerFramesDiscarded = 0;
	volatile LONG64 m_statViewerDecodeDropped = 0;
	volatile LONG64 m_statFeedbackReports = 0;
	volatile LONG64 m_statBytesQueued = 0;

	// IDR 강제의 최소 간격과 마지막으로 강제한 시각(GetTickCount64).
	// 0 은 "아직 한 번도 강제하지 않음" 이므로 첫 요청은 즉시 통과한다.
	volatile LONG m_streamSelfHealing = FALSE;
	volatile LONG m_minKeyFrameIntervalMs = 1'000;
	volatile LONG64 m_lastForcedKeyFrameTick = 0;

	uint32_t m_streamInfoVersion = 1;
	uint32_t m_codecConfigVersion = 1;
	uint16_t m_streamWidth = 1280;
	uint16_t m_streamHeight = 720;
	uint16_t m_streamFps = 30;
	DESKTOP_STREAM_CODEC_TYPE m_codecType = DESKTOP_STREAM_CODEC_TYPE::H264;
};
