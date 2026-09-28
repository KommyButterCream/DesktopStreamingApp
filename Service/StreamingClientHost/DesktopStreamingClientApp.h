#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <stdint.h>

#include "../../../../Module/D3D11Engine/Core/D3D11RenderEngine.h"
#include "../../../../Module/D3D11ImageView/D3D11ImageView/D3D11ImageView.h"
#include "../../../../Module/NvCodec/NvDecode/D3D11NvDecoder.h"
#include "../StreamingClient/StreamingClient.h"
#include "../StreamingViewerUI/StreamingViewerUI.h"

#ifdef BUILD_STREAMING_CLIENT_HOST_DLL
#define STREAMING_CLIENT_HOST_API __declspec(dllexport)
#else
#define STREAMING_CLIENT_HOST_API __declspec(dllimport)
#endif

// 스트리밍 클라이언트 한 벌.
//
// 네트워크(StreamingClient) → 디코더(NvDecoder) → 페이싱 → 뷰어(ImageView)
// 를 잇고, 재접속 / 피드백 / 재생 제어 / 컨트롤 바 갱신을 맡는다.
//
// 예전에는 DesktopStreamingClient.exe 안의 헤더 전용 클래스였다. WPF 같은
// 다른 호스트에서도 같은 코드를 쓰려고 StreamingClientHost.dll 로 옮겼다.
// 프레임이 지나가는 경로(디코드 → 페이싱 → UpdateSharedTexture)는 전부
// 이 안에 있고 밖으로 나가지 않는다.
class STREAMING_CLIENT_HOST_API DesktopStreamingClientApp
{
public:
	DesktopStreamingClientApp() = default;
	~DesktopStreamingClientApp();

	// 뷰어 창, 디코더 스레드, 네트워크 세션을 소유한다. 복사하면 둘이
	// 같은 것을 지우게 된다.
	DesktopStreamingClientApp(const DesktopStreamingClientApp&) = delete;
	DesktopStreamingClientApp& operator=(const DesktopStreamingClientApp&) = delete;

public:
	// --- 호스트 이벤트 ---
	//
	// 호스트가 알아야 할 일. 워커 스레드(IOCP, 디코더)에서 일어난 일도
	// 전부 Initialize 를 부른 스레드로 옮겨서 통지한다 — WPF 라면 UI
	// 스레드라 받는 쪽이 Dispatcher 를 거치지 않고 바로 화면을 고칠 수
	// 있다. (PostHostEvent 참고)
	//
	// 값 a/b/c 의 뜻은 이벤트마다 다르다.
	enum class HostEvent : uint32_t
	{
		ConnectionChanged = 0,   // a = DESKTOP_STREAM_CONNECTION_EVENT, b = DisconnectReason, c = WSA 오류
		StreamInfoChanged,       // a = width, b = height, c = fps
		PlaybackStateChanged,    // a = StreamingPlaybackState
		Stopped,                 // a = HostStopReason
	};

	// 호스트가 부탁하지 않았는데 스스로 멈춘 이유. RequestStop 으로
	// 멈춘 것은 부른 쪽이 이미 알므로 통지하지 않는다.
	enum class HostStopReason : int32_t
	{
		ViewerClosed = 0,
		DecoderFault,
	};

	using HostEventCallback = void (*)(HostEvent event, int32_t a, int32_t b, int32_t c, void* userData);

	// Initialize 전에 걸어도 된다. 통지는 메시지 루프에서 배달되므로,
	// 루프를 처음 돌리기 전에만 걸면 첫 접속 통지까지 받는다.
	void SetHostEventCallback(HostEventCallback callback, void* userData);

	// Initialize 와 Shutdown 은 같은 스레드에서 불러야 하고, 그 스레드는
	// 메시지를 펌프해야 한다. 재접속 / 피드백 / 컨트롤 바 갱신 같은 주기
	// 작업이 그 스레드의 타이머로 돈다(ServiceTick 주석 참고).
	//
	// 뷰어 창이 닫히거나 치명적 오류가 나면 IsRunning 이 false 가 된다.
	// 호스트는 그걸 보고 Shutdown 을 부른다.
	//
	// parentWindow 가 nullptr 이면 뷰어가 독립 창으로 뜬다(네이티브 exe).
	// 값이 있으면 그 창의 자식으로 붙는다(WPF HwndHost 등). windowRect 는
	// 전자일 때 화면 좌표, 후자일 때 부모의 클라이언트 좌표다.
	bool Initialize(const char* serverIp = "127.0.0.1", uint16_t serverPort = 27015,
		HWND parentWindow = nullptr, RECT windowRect = { 0, 0, 1920, 900 });
	void RequestStop();
	bool IsRunning() const;
	void Shutdown();
	void PrintStats();

	// --- 재생 제어 (Initialize 를 부른 스레드에서) ---
	//
	// 컨트롤 바 버튼과 같은 경로다. 어느 쪽으로 바꾸든 PlaybackStateChanged
	// 가 한 번 온다.
	void StartPlayback();
	void PausePlayback();
	void StopPlayback();
	StreamingPlaybackState GetPlaybackState() const;

	// --- 설정 ---
	//
	// SetJitterBufferMs 는 어느 스레드에서 불러도 된다(디코드 스레드가
	// 원자적으로 읽는다). 나머지는 Initialize 를 부른 스레드에서.
	void SetJitterBufferMs(uint32_t milliseconds);
	void SetControlBarVisible(bool visible);
	void SetControlBarAutoHide(bool enabled);
	void SetStatsOverlayVisible(bool visible);

	// --- 조회 (Initialize 를 부른 스레드에서) ---
	HWND GetViewerWindow() const;

	// 마지막 틱(500ms 주기)에 계산한 값. 수신량 / 표시 fps 는 두 틱
	// 사이의 증분이라 호출할 때마다 새로 잴 수 없다.
	void GetStatsSnapshot(StreamingStatsInfo& stats, StreamingQualityInfo& quality) const;
	void GetNetworkStats(DesktopStreamingClientStats& stats) const;
	void GetDecoderStats(NvDecStats& stats) const;
	uint64_t GetPresentedFrameCount() const;

private:
	static constexpr ULONGLONG STATS_INTERVAL_MS = 5'000;
	static constexpr ULONGLONG RECONNECT_INTERVAL_MS = 2'000;

	// 서버에 수신 상태를 알리는 주기. 서버의 비트레이트 조정이 이걸 본다.
	static constexpr ULONGLONG FEEDBACK_INTERVAL_MS = 1'000;

	// 지연 표시 갱신 주기. 더 자주 바꿔 봐야 숫자가 떨리기만 한다.
	static constexpr ULONGLONG VIEWER_UI_INTERVAL_MS = 500;

	// 지터 버퍼 기본 깊이. 한 프레임 남짓이다.
	//
	// 원격 화면 공유는 지연이 화질보다 중요하므로 깊게 잡지 않는다.
	// 16ms 면 60fps 한 프레임이고, 도착 흔들림의 대부분을 흡수하면서
	// 사람이 느낄 만한 지연은 더하지 않는다. 0 으로 두면 페이싱을 끈다.
	static constexpr LONG DEFAULT_JITTER_BUFFER_MS = 16;

	// 이보다 오래 기다려야 하면 기준이 틀어진 것이다. 다시 잡는다.
	static constexpr ULONGLONG MAX_PACE_WAIT_MS = 100;

	// 이보다 많이 밀렸으면 따라잡기를 포기하고 기준을 다시 잡는다.
	static constexpr ULONGLONG MAX_PACE_DRIFT_MS = 250;

	// --- 주기 작업의 구동 ---
	//
	// 예전에는 exe 의 Run() 루프가 10ms 마다 아래 Service* 를 불렀다. 그
	// 루프는 메시지 펌프와 콘솔 입력까지 같이 하고 있었는데, WPF 에서는
	// 메시지 루프를 Dispatcher 가 소유하고 콘솔이 없다. 그래서 주기 작업만
	// 떼어 이 DLL 안의 타이머로 옮겼다.
	//
	// 타이머는 Initialize 를 부른 스레드에 만든 메시지 전용 창에 건다.
	// WM_TIMER 는 그 스레드의 메시지를 펌프하는 쪽이 누구든 배달되므로 —
	// 네이티브 exe 의 루프든 WPF Dispatcher 든 — 이 클래스는 루프가 누구
	// 것인지 몰라도 된다. 그리고 예전처럼 그 스레드에서 돌기 때문에
	// ServiceViewerUI 가 뷰어를 만질 때 새로운 스레드 경합이 생기지 않는다.
	static constexpr UINT_PTR TICK_TIMER_ID = 1;
	static constexpr UINT TICK_INTERVAL_MS = 10;

	bool CreateTickWindow();
	void DestroyTickWindow();
	void ServiceTick();
	static LRESULT CALLBACK TickWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

	// 호스트 이벤트를 틱 창으로 보낸다. 어느 스레드에서 불러도 된다.
	//
	// 같은 스레드에서 부를 때도 곧바로 콜백하지 않고 큐를 거친다. 재생
	// 상태를 바꾸는 도중에 호스트 코드가 끼어들면(콜백 안에서 다시
	// StopPlayback 을 부르는 식) 이 클래스의 상태가 반쯤 바뀐 채로
	// 재진입한다. 큐를 거치면 늘 한 동작이 끝난 뒤에 통지된다.
	static constexpr UINT HOST_EVENT_MESSAGE = WM_APP + 1;
	void PostHostEvent(HostEvent event, int32_t a = 0, int32_t b = 0, int32_t c = 0);

	// --- 앱 스레드에서 주기적으로 도는 일 ---
	void ServiceFeedback(ULONGLONG now);
	void ServiceReconnect(ULONGLONG now);
	void ServiceViewerUI(ULONGLONG now);
	void ServiceStatsOverlay(ULONGLONG now, ULONGLONG previousTick,
		const DesktopStreamingClientStats& netStats,
		const StreamingQualityInfo& qualityInfo,
		float latencyMs);

	// --- 재생 제어 (창 메시지 스레드) ---
	void OnViewerUICommand(StreamingViewerCommand command);
	void PushPlaybackStateToUI();

	// --- 콜백 진입점 ---
	static void StreamInfoCallback(const DesktopStreamClientSessionContext& streamContext, void* userData);
	static void FrameCallback(const uint8_t* frameData, uint32_t frameSize, uint64_t frameId, uint64_t timestamp, uint16_t frameType, void* userData);
	static void DecodedFrameCallback(const D3D11NvDecoder::Frame& frame, void* userData);
	static void ViewerUICommandCallback(StreamingViewerCommand command, void* userData);
	static void ViewerCloseCallback(void* userData);
	static void DecoderErrorCallback(NvDecErrorCode errorCode, void* userData);
	static void ConnectionCallback(DESKTOP_STREAM_CONNECTION_EVENT event, DisconnectReason reason, int errorCode, void* userData);

	void OnViewerClosed();
	void OnConnectionEvent(DESKTOP_STREAM_CONNECTION_EVENT event, DisconnectReason reason, int errorCode);
	void OnDecoderError(NvDecErrorCode errorCode);
	void OnStreamInfo(const DesktopStreamClientSessionContext& streamContext);
	void OnEncodedFrame(const uint8_t* frameData, uint32_t frameSize, uint64_t timestamp);

	// --- 디코드 스레드 ---
	void OnDecodedFrame(const D3D11NvDecoder::Frame& frame);
	void PaceFramePresentation(uint64_t frameTimestamp);

private:
	volatile LONG m_running = FALSE;

	// 주기 작업 타이머가 걸린 메시지 전용 창. Initialize 를 부른 스레드 소유다.
	//
	// 워커 스레드도 PostHostEvent 에서 이 값을 읽는다. 그래서 쓰기는
	// InterlockedExchangePointer, 읽기는 ReadPointerAcquire 로 한다.
	HWND m_tickWindow = nullptr;

	HostEventCallback m_hostEventCallback = nullptr;
	void* m_hostEventUserData = nullptr;

	// 마지막 틱에 계산해 둔 값. GetStatsSnapshot 이 돌려준다.
	// 틱과 조회가 같은 스레드라 보호가 필요 없다.
	StreamingStatsInfo m_lastStats = {};
	StreamingQualityInfo m_lastQuality = {};

	// ServiceTick 이 도는 중인가. 같은 스레드에서만 만진다.
	//
	// 예전 Run() 루프에서는 한 틱이 끝나야 다음 틱이 왔다. 타이머로 바꾸면
	// 틱 안에서 누군가 메시지를 펌프할 때(모달 루프 등) WM_TIMER 가 다시
	// 들어올 수 있다. 그 재진입을 막아 예전과 같은 "한 번에 한 틱" 을 지킨다.
	bool m_inTick = false;

	// 뷰어 창이 살아 있는가. UI 스레드가 내리고 디코드 스레드가 읽는다.
	volatile LONG m_viewerAlive = FALSE;
	volatile LONG m_reconnectPending = FALSE;

	ULONGLONG m_nextStatsTick = 0;
	ULONGLONG m_nextReconnectTick = 0;
	ULONGLONG m_nextFeedbackTick = 0;
	ULONGLONG m_nextViewerUITick = 0;

	// --- 컨트롤 바의 화질 표시 ---
	//
	// 비트레이트는 두 틱 사이의 수신 바이트 증분으로 잰다. 앱 스레드만
	// 만지므로 평범한 멤버다.
	ULONGLONG m_lastViewerUITick = 0;
	uint64_t m_lastQualityBytes = 0;

	// 해상도와 fps 는 OnStreamInfo 가 쓰고 ServiceViewerUI 가 읽는다.
	// 전자는 IOCP 워커 스레드, 후자는 앱 스레드다.
	//
	// 셋을 각각 원자 변수로 두면 해상도가 바뀌는 순간 width 만 새 값이고
	// height 는 옛 값인 조합을 읽을 수 있다. 화면에 잠깐 "1440p" 대신
	// 엉뚱한 수가 뜨는 정도지만, 셋 다 uint16 이라 한 LONG64 에 들어간다.
	// 묶어 두면 그 조합 자체가 생기지 않는다.
	//
	//   [47:32] width   [31:16] height   [15:0] fps
	volatile LONG64 m_streamGeometry = 0;

	// --- 재생 제어 ---
	//
	// 상태는 앱 스레드만 만진다. 버튼 콜백도 창 메시지 스레드에서 오므로
	// 같은 스레드다.
	//
	// 시작 상태가 Playing 인 것은 이것이 "사용자가 원하는 것" 이기 때문이다.
	// 접속이 성립하면 StreamingClient 가 스스로 구독하므로, 앱이 뜬 순간부터
	// 사용자는 재생을 원하는 상태다.
	StreamingPlaybackState m_playbackState = StreamingPlaybackState::Playing;

	// 디코드 스레드가 읽고 앱 스레드가 쓴다. 이 값이 FALSE 면
	// OnDecodedFrame 이 화면 갱신만 건너뛴다.
	volatile LONG m_presentEnabled = TRUE;

	static LONG64 PackStreamGeometry(uint32_t width, uint32_t height, uint32_t fps)
	{
		return (static_cast<LONG64>(width & 0xFFFF) << 32)
			| (static_cast<LONG64>(height & 0xFFFF) << 16)
			| static_cast<LONG64>(fps & 0xFFFF);
	}

	// 지터 버퍼. 디코드 스레드가 읽고 앱 스레드가 바꿀 수 있어 원자적이다.
	volatile LONG m_jitterBufferMs = DEFAULT_JITTER_BUFFER_MS;

	// 스트림이 바뀌었으니 페이싱 기준을 다시 잡으라는 표시.
	// 앱/워커 스레드가 세우고 디코드 스레드가 내린다.
	volatile LONG m_paceResetRequest = FALSE;

	// 페이싱 상태. 디코드 스레드 전용.
	bool m_paceBaseValid = false;
	uint64_t m_paceBaseTimestamp = 0;
	ULONGLONG m_paceBaseTick = 0;
	uint64_t m_paceWaitTotalMs = 0;
	uint64_t m_paceWaitCount = 0;
	uint64_t m_paceResyncCount = 0;
	uint64_t m_presentedFrames = 0;

	// 표시 fps 를 재려고 앱 스레드가 들고 있는 직전 값.
	//
	// m_presentedFrames 를 앱 스레드에서 읽는 것은 원자적이지 않지만,
	// x64 에서 정렬된 8바이트 읽기는 찢어지지 않고 한 틱 늦은 값이
	// 나와도 fps 표시가 0.x 흔들릴 뿐이다. 기존 PrintStats 도 같은
	// 카운터를 같은 방식으로 읽는다.
	uint64_t m_lastPresentedFrames = 0;

	// 스트림 정보에서 받은 프레임률. 페이싱 간격의 기준이다.
	volatile LONG m_streamFpsAtomic = 60;

	char m_serverIp[64] = "127.0.0.1";
	uint16_t m_serverPort = 27015;

	D3D11RenderEngine* m_D3D11Engine = nullptr;
	D3D11NvDecoder* m_nvDecoder = nullptr;
	D3D11ImageView* m_imageView = nullptr;

	// 뷰어 위에 얹는 스트리밍 컨트롤 바. 뷰어보다 먼저 정리한다.
	StreamingViewerUI* m_viewerUI = nullptr;
	StreamingClient* m_streamingClient = nullptr;
};
