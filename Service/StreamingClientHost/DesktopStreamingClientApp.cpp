#include "DesktopStreamingClientApp.h"

#include <objbase.h>

#include <stdio.h>

// 이 DLL 자신의 모듈 핸들. 창 클래스는 등록한 모듈에 묶이므로
// exe 의 핸들이 아니라 이것으로 등록해야 한다.
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
	constexpr wchar_t TICK_WINDOW_CLASS[] = L"StreamingClientHost.TickWindow";

	HINSTANCE ThisModule()
	{
		return reinterpret_cast<HINSTANCE>(&__ImageBase);
	}
}

DesktopStreamingClientApp::~DesktopStreamingClientApp()
{
	Shutdown();
}

bool DesktopStreamingClientApp::Initialize(const char* serverIp, uint16_t serverPort,
	HWND parentWindow, RECT windowRect)
{
	::strncpy_s(m_serverIp, serverIp, _TRUNCATE);
	m_serverPort = serverPort;

	// 틱 창을 가장 먼저 만든다.
	//
	// 이 창은 주기 작업 타이머이면서 호스트 이벤트의 배달 통로다. 아래에서
	// StartClient 를 부르는 순간부터 IOCP 워커가 접속 통지를 보낼 수 있는데,
	// 그때 창이 없으면 통지가 사라진다 — 호스트는 첫 "연결됨" 을 영영 못
	// 받는다. 그래서 워커를 하나라도 띄우기 전에 창부터 둔다.
	//
	// 먼저 만들어도 틱이 반쯤 만든 객체를 건드리지 않는다. WM_TIMER 는 이
	// 스레드가 메시지를 펌프할 때만 오는데 Initialize 는 펌프하지 않고,
	// 설령 온다 해도 ServiceTick 은 맨 끝에서 m_running 이 켜지기 전까지
	// 아무것도 하지 않는다.
	if (!CreateTickWindow())
	{
		printf_s("[DesktopStreamingClient] Failed to create the tick timer.\n");
		Shutdown();
		return false;
	}

	RenderEngineConfig renderEngineConfig = {};
	renderEngineConfig.initD2D = false;
	renderEngineConfig.initD3D = true;
	renderEngineConfig.initFontManager = false;
#if defined(_DEBUG)
	renderEngineConfig.initDebugLayer = true;
#endif

	m_D3D11Engine = new D3D11RenderEngine();
	if (!m_D3D11Engine)
	{
		Shutdown();
		return false;
	}

	if (!m_D3D11Engine->Initialize(renderEngineConfig))
	{
		printf_s("[DesktopStreamingClient] Failed to initialize D3D11RenderEngine.\n");
		Shutdown();
		return false;
	}

	// 여기에 outputWidth / outputHeight = 2560 / 1440 이 있었다.
	// 선언하고 0인지 검사한 뒤 아무 데도 쓰지 않았다 — 디코더는 비트
	// 스트림의 SPS 에서 해상도를 읽고, ImageView 는 공유 텍스처를
	// 그대로 받는다. 서버가 알려주는 크기는 OnStreamInfo 로 온다.

	m_nvDecoder = new D3D11NvDecoder();
	if (!m_nvDecoder)
	{
		Shutdown();
		return false;
	}

	// 유입 큐는 디코더가 소유한다. 버퍼는 프로토콜의 프레임 상한에 맞춘다 —
	// 큐가 패킷을 자기 버퍼로 복사하므로 가장 큰 프레임이 들어가야 한다.
	NvDecConfig decoderConfig;
	decoderConfig.inputQueueDepth = 8;
	decoderConfig.maxPacketSize = DESKTOP_STREAM_MAX_FRAME_SIZE;
	decoderConfig.sharedOutputTextureMode = true;

	// contextGate = nullptr: 이 엔진의 immediate context 를 쓰는 주체는
	// 디코더 하나뿐이다. ImageView 는 자기 엔진을 따로 만들고(아래 Initialize 의
	// D3D11Engine 인자가 nullptr), 디코딩 결과는 shared handle 로 건네받는다.
	if (!m_nvDecoder->Initialize(m_D3D11Engine->GetD3DDevice(), decoderConfig, nullptr))
	{
		printf_s("[DesktopStreamingClient] Failed to initialize D3D11NvDecoder.\n");
		Shutdown();
		return false;
	}

	// 프레임 유실과 파이프라인 정지를 통지받는다. 등록하지 않고 있었다.
	//
	// NvDecErrorCode::OutputPoolExhausted 와 OutputNotConsumed 는 앱이
	// 프레임을 제때 가져가지 않는다는 뜻이다. 통지가 없으면 그 상황이
	// "가끔 끊기는 화면" 으로만 보인다.
	m_nvDecoder->SetErrorCallback(DecoderErrorCallback, this);

	m_imageView = new D3D11ImageView();
	if (!m_imageView)
	{
		Shutdown();
		return false;
	}

	// 부모가 주어지면 그 안에 자식 창으로 붙는다. WPF 의 HwndHost 가
	// BuildWindowCore 에서 이 경로를 쓴다 — WPF 는 자기 창 위에 네이티브
	// 창을 그릴 수 없으므로(airspace) 뷰어가 자식 창이 되어야 한다.
	//
	// 부모가 없으면 예전처럼 독립 창이다.
	const bool asChild = (parentWindow != nullptr);
	const HWND ownerWindow = asChild ? parentWindow : ::GetDesktopWindow();
	const DWORD windowStyle = asChild
		? (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN)
		: (WS_VISIBLE | WS_OVERLAPPEDWINDOW);

	if (!m_imageView->Initialize(ownerWindow, windowRect, windowStyle, nullptr))
	{
		printf_s("[DesktopStreamingClient] Failed to initialize D3D11ImageView.\n");
		Shutdown();
		return false;
	}

	// 뷰어 창이 이 앱의 유일한 창이다. 닫기 버튼을 누르면 그것이 곧
	// 종료 요청이므로 통지를 받아 메시지 루프를 끝낸다.
	m_imageView->SetCloseHandler(ViewerCloseCallback, this);
	::InterlockedExchange(&m_viewerAlive, TRUE);

	// 뷰어 내장 UI 를 끈다.
	//
	// 툴바의 측정 / LUT / 픽셀 격자는 검사 이미지를 들여다보는 도구라
	// 실시간 영상에는 쓸 일이 없고, 하단 상태바는 아래에 붙일 컨트롤
	// 바와 자리가 겹친다. 둘 다 띄우면 UI 가 두 벌로 보인다.
	m_imageView->SetToolbarVisible(false);
	m_imageView->SetStatusBarVisible(false);

	// 스트리밍 컨트롤 바. 뷰어에 외부 렌더 레이어로 얹힌다.
	//
	// 뷰어는 이 UI 가 무엇인지 모른다 — 재생이라는 개념은 전부
	// StreamingViewerUI.dll 안에 있고, 뷰어는 그리기 자리와 마우스만
	// 넘겨준다.
	m_viewerUI = new StreamingViewerUI();
	if (m_viewerUI)
	{
		m_viewerUI->SetCommandCallback(ViewerUICommandCallback, this);

		if (!m_viewerUI->Attach(m_imageView))
		{
			printf_s("[DesktopStreamingClient] Failed to attach the viewer control bar.\n");
			delete m_viewerUI;
			m_viewerUI = nullptr;
		}
		else
		{
			// 지연 눈금 상한. 이 값을 넘으면 바가 가득 찬 채로 멈춘다.
			m_viewerUI->SetLatencyRange(200.0f);

			// 접속이 되면 StreamingClient 가 스스로 구독한다. 즉 앱은
			// 재생 상태로 뜨는 것이고, 버튼도 그렇게 보여야 한다.
			PushPlaybackStateToUI();
		}
	}

	// 디코드 워커와 유입 큐를 모두 디코더가 소유한다.
	m_nvDecoder->SetFrameCallback(DecodedFrameCallback, this);
	if (!m_nvDecoder->StartDecodeThread())
	{
		printf_s("[DesktopStreamingClient] Failed to start the decode thread.\n");
		Shutdown();
		return false;
	}

	m_streamingClient = new StreamingClient();
	if (!m_streamingClient)
	{
		Shutdown();
		return false;
	}

	m_streamingClient->SetStreamInfoCallback(StreamInfoCallback, this);
	m_streamingClient->SetFrameCallback(FrameCallback, this);

	// 접속 상태 변화를 받는다. 재접속은 이 콜백 안이 아니라 앱 스레드의
	// 틱(ServiceTick)이 한다 — 이 콜백은 엔진 워커 스레드에서 불리므로 그 안에서
	// StopClient 를 부르면 자기 스레드의 종료를 자기가 기다리게 된다.
	m_streamingClient->SetConnectionCallback(ConnectionCallback, this);

	// 서버가 아직 안 떠 있어도 실패가 아니다. 타이머가 계속 다시 붙는다.
	if (!m_streamingClient->StartClient(m_serverIp, m_serverPort))
	{
		printf_s("[DesktopStreamingClient] Initial connect to %s:%u failed. will retry.\n",
			m_serverIp, m_serverPort);
		m_reconnectPending = TRUE;
	}

	// 여기서부터 ServiceTick 이 일을 한다. (틱 창은 맨 앞에서 만들었다)
	::InterlockedExchange(&m_running, TRUE);
	m_nextStatsTick = ::GetTickCount64() + STATS_INTERVAL_MS;
	return true;
}

bool DesktopStreamingClientApp::CreateTickWindow()
{
	WNDCLASSEXW windowClass = {};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = &DesktopStreamingClientApp::TickWindowProc;
	windowClass.hInstance = ThisModule();
	windowClass.lpszClassName = TICK_WINDOW_CLASS;

	// 인스턴스를 둘 이상 만들면 두 번째부터는 이미 등록돼 있다. 정상이다.
	if (!::RegisterClassExW(&windowClass) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return false;

	// 메시지 전용 창. 보이지 않고 브로드캐스트도 받지 않는다 —
	// 타이머와 호스트 이벤트를 받을 자리로만 쓴다.
	HWND window = ::CreateWindowExW(0, TICK_WINDOW_CLASS, L"", 0, 0, 0, 0, 0,
		HWND_MESSAGE, nullptr, ThisModule(), this);
	if (!window)
		return false;

	if (::SetTimer(window, TICK_TIMER_ID, TICK_INTERVAL_MS, nullptr) == 0)
	{
		::DestroyWindow(window);
		return false;
	}

	// 타이머까지 걸린 뒤에 공개한다. 워커 스레드는 이 값이 보이는 순간부터
	// 이벤트를 보낸다.
	::InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&m_tickWindow), window);
	return true;
}

void DesktopStreamingClientApp::DestroyTickWindow()
{
	// 먼저 값을 비워 워커 스레드가 더는 이 창으로 보내지 않게 한다.
	// 이미 보낸 것은 창과 함께 버려진다.
	HWND window = static_cast<HWND>(::InterlockedExchangePointer(
		reinterpret_cast<PVOID volatile*>(&m_tickWindow), nullptr));
	if (!window)
		return;

	// 창을 부수기 전에 이 객체와의 연결부터 끊는다. 소멸자가 다른 스레드에서
	// 불려 DestroyWindow 가 실패하더라도, 그 뒤에 배달되는 메시지가
	// 지워진 객체를 부르지 않는다.
	::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
	::KillTimer(window, TICK_TIMER_ID);
	::DestroyWindow(window);

	// 다른 인스턴스가 아직 창을 쓰고 있으면 실패한다. 그러면 그쪽이
	// 자기 Shutdown 에서 지운다.
	::UnregisterClassW(TICK_WINDOW_CLASS, ThisModule());
}

LRESULT CALLBACK DesktopStreamingClientApp::TickWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE)
	{
		const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
	}
	else if (message == WM_TIMER && wParam == TICK_TIMER_ID)
	{
		DesktopStreamingClientApp* self =
			reinterpret_cast<DesktopStreamingClientApp*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (self)
		{
			self->ServiceTick();
		}
		return 0;
	}
	else if (message == HOST_EVENT_MESSAGE)
	{
		DesktopStreamingClientApp* self =
			reinterpret_cast<DesktopStreamingClientApp*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

		// 콜백을 지역 변수로 옮긴 뒤 부르고, 그 뒤로는 self 를 건드리지
		// 않는다. 호스트가 콜백 안에서 이 객체를 지우더라도 여기서
		// 지워진 메모리를 읽지 않게 하려는 것이다.
		if (self && self->m_hostEventCallback)
		{
			const HostEventCallback callback = self->m_hostEventCallback;
			void* userData = self->m_hostEventUserData;

			// PostHostEvent 가 싼 순서 그대로 푼다.
			//   wParam = [63:32] event  [31:0] a
			//   lParam = [63:32] b      [31:0] c
			const HostEvent event = static_cast<HostEvent>(static_cast<uint64_t>(wParam) >> 32);
			const int32_t a = static_cast<int32_t>(static_cast<uint64_t>(wParam) & 0xFFFFFFFFull);
			const int32_t b = static_cast<int32_t>(static_cast<uint64_t>(lParam) >> 32);
			const int32_t c = static_cast<int32_t>(static_cast<uint64_t>(lParam) & 0xFFFFFFFFull);

			callback(event, a, b, c, userData);
		}
		return 0;
	}

	return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

void DesktopStreamingClientApp::PostHostEvent(HostEvent event, int32_t a, int32_t b, int32_t c)
{
	// 값 넷을 32비트씩 WPARAM / LPARAM 두 칸(x64 에서 각 64비트)에 싼다.
	// 힙을 쓰지 않으므로 받는 쪽이 사라져 메시지가 버려져도 새는 것이 없다.
	HWND window = static_cast<HWND>(::ReadPointerAcquire(
		reinterpret_cast<PVOID const volatile*>(&m_tickWindow)));
	if (!window)
		return;

	const WPARAM wParam = static_cast<WPARAM>(
		(static_cast<uint64_t>(event) << 32) | static_cast<uint32_t>(a));
	const LPARAM lParam = static_cast<LPARAM>(
		(static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32) | static_cast<uint32_t>(c));

	// 실패하는 것은 창이 막 사라졌을 때뿐이다(Shutdown 이 먼저 창을 부순다).
	// 그때 통지는 더 이상 받을 사람이 없다.
	::PostMessageW(window, HOST_EVENT_MESSAGE, wParam, lParam);
}

void DesktopStreamingClientApp::SetHostEventCallback(HostEventCallback callback, void* userData)
{
	m_hostEventCallback = callback;
	m_hostEventUserData = userData;
}

// 예전 Run() 루프의 몸통에서 메시지 펌프와 콘솔 입력을 뺀 나머지다.
// 그 둘은 exe 의 몫으로 돌아갔다(main.cpp).
void DesktopStreamingClientApp::ServiceTick()
{
	// 종료가 요청됐으면 아무것도 하지 않는다. 예전 Run() 이 그 순간 루프를
	// 빠져나오던 것과 같다 — 치명적 오류 뒤에 재접속을 시도하면 안 된다.
	if (!IsRunning() || m_inTick)
		return;

	m_inTick = true;

	const ULONGLONG now = ::GetTickCount64();

	ServiceReconnect(now);

	// 수신 상태를 서버에 알린다. 서버의 비트레이트 조정이 이걸 본다.
	ServiceFeedback(now);

	// 컨트롤 바의 지연 표시.
	ServiceViewerUI(now);

	if (now >= m_nextStatsTick)
	{
		m_nextStatsTick = now + STATS_INTERVAL_MS;
		PrintStats();
	}

	m_inTick = false;
}

void DesktopStreamingClientApp::RequestStop()
{
	::InterlockedExchange(&m_running, FALSE);
}

bool DesktopStreamingClientApp::IsRunning() const
{
	return ::ReadAcquire(&m_running) != FALSE;
}

void DesktopStreamingClientApp::Shutdown()
{
	::InterlockedExchange(&m_running, FALSE);

	// 주기 작업부터 멈춘다. 아래에서 네트워크와 디코더를 지우는 동안
	// 틱이 그것들을 부르면 안 된다.
	DestroyTickWindow();

	::InterlockedExchange(&m_viewerAlive, FALSE);

	if (m_streamingClient)
	{
		m_streamingClient->StopClient();
		delete m_streamingClient;
		m_streamingClient = nullptr;
	}

	// 워커가 큐를 참조하므로 큐를 지우기 전에 반드시 멈춘다.
	if (m_nvDecoder)
	{
		m_nvDecoder->StopDecodeThread();
	}

	// 컨트롤 바를 뷰어보다 먼저 뗀다. Detach 가 렌더 락 안에서
	// 레이어를 빼므로, 반환한 뒤에는 렌더 스레드가 그것을 부르지 않는다.
	if (m_viewerUI)
	{
		m_viewerUI->Detach();
		delete m_viewerUI;
		m_viewerUI = nullptr;
	}

	if (m_imageView)
	{
		delete m_imageView;
		m_imageView = nullptr;
	}

	if (m_nvDecoder)
	{
		m_nvDecoder->Destroy();
		delete m_nvDecoder;
		m_nvDecoder = nullptr;
	}

	if (m_D3D11Engine)
	{
		delete m_D3D11Engine;
		m_D3D11Engine = nullptr;
	}
}

void DesktopStreamingClientApp::PrintStats()
{
	DesktopStreamingClientStats netStats = {};
	if (m_streamingClient)
		m_streamingClient->GetStats(netStats);

	NvDecStats decodeStats = {};
	if (m_nvDecoder)
		m_nvDecoder->GetStats(decodeStats);

	const uint64_t queueDrops = decodeStats.droppedInputQueue;

	printf_s(
		"[stats] net connected=%d chunks=%llu rejected=%llu frames=%llu discarded=%llu bytes=%llu | "
		"decode parsed=%llu decoded=%llu delivered=%llu poolDrop=%llu lagDrop=%llu qdrop=%llu faulted=%d\n",
		netStats.connected ? 1 : 0, netStats.chunksAccepted, netStats.chunksRejected,
		netStats.framesCompleted, netStats.framesDiscarded, netStats.chunkBytesReceived,
		decodeStats.parsedPackets, decodeStats.decodedFrames, decodeStats.deliveredFrames,
		decodeStats.droppedPoolExhausted, decodeStats.droppedNotConsumed, queueDrops,
		decodeStats.faulted ? 1 : 0);

	// 페이싱 상태. 대기가 전혀 없으면 페이싱이 안 걸리는 것이고,
	// resync 가 계속 오르면 기준이 못 잡히는 것이다.
	printf_s("[pace] presented=%llu buffer=%ldms waits=%llu avgWait=%.1fms resync=%llu\n",
		static_cast<unsigned long long>(m_presentedFrames),
		::ReadAcquire(&m_jitterBufferMs),
		static_cast<unsigned long long>(m_paceWaitCount),
		m_paceWaitCount > 0 ? static_cast<double>(m_paceWaitTotalMs) / m_paceWaitCount : 0.0,
		static_cast<unsigned long long>(m_paceResyncCount));

	// 서버 쪽과 같은 사정이다. 리디렉션된 stdout 은 완전 버퍼링이라
	// 흘려보내지 않으면 주기 지표가 제때 보이지 않는다.
	::fflush(stdout);
}

StreamingPlaybackState DesktopStreamingClientApp::GetPlaybackState() const
{
	return m_playbackState;
}

void DesktopStreamingClientApp::SetJitterBufferMs(uint32_t milliseconds)
{
	// 디코드 스레드가 매 프레임 원자적으로 읽는다. 0 이면 페이싱을 끈다.
	::InterlockedExchange(&m_jitterBufferMs, static_cast<LONG>(milliseconds));

	// 깊이가 바뀌면 지금 기준으로 잡힌 목표 시각들이 전부 틀어진다.
	::InterlockedExchange(&m_paceResetRequest, TRUE);
}

void DesktopStreamingClientApp::SetControlBarVisible(bool visible)
{
	if (m_viewerUI)
	{
		m_viewerUI->SetVisible(visible);
	}
}

void DesktopStreamingClientApp::SetControlBarAutoHide(bool enabled)
{
	if (m_viewerUI)
	{
		m_viewerUI->SetAutoHide(enabled);
	}
}

void DesktopStreamingClientApp::SetStatsOverlayVisible(bool visible)
{
	if (m_viewerUI)
	{
		m_viewerUI->SetStatsVisible(visible);
	}
}

HWND DesktopStreamingClientApp::GetViewerWindow() const
{
	return m_imageView ? m_imageView->GetHWND() : nullptr;
}

void DesktopStreamingClientApp::GetStatsSnapshot(StreamingStatsInfo& stats, StreamingQualityInfo& quality) const
{
	stats = m_lastStats;
	quality = m_lastQuality;
}

void DesktopStreamingClientApp::GetNetworkStats(DesktopStreamingClientStats& stats) const
{
	stats = {};
	if (m_streamingClient)
	{
		m_streamingClient->GetStats(stats);
	}
}

void DesktopStreamingClientApp::GetDecoderStats(NvDecStats& stats) const
{
	stats = {};
	if (m_nvDecoder)
	{
		m_nvDecoder->GetStats(stats);
	}
}

uint64_t DesktopStreamingClientApp::GetPresentedFrameCount() const
{
	// 디코드 스레드가 올리는 카운터다. x64 에서 정렬된 8바이트 읽기는
	// 찢어지지 않으므로 PrintStats 와 같은 방식으로 읽는다.
	return m_presentedFrames;
}

// 수신 상태를 서버에 알린다. 앱 스레드에서 부른다.
//
// 디코더 통계는 서비스가 모르므로 여기서 합쳐 넘긴다. 서버는 이 값과
// 자기 송신 실패를 같이 보고 비트레이트를 정한다.
void DesktopStreamingClientApp::ServiceFeedback(ULONGLONG now)
{
	if (!m_streamingClient || !m_streamingClient->IsConnected())
		return;

	if (now < m_nextFeedbackTick)
		return;

	m_nextFeedbackTick = now + FEEDBACK_INTERVAL_MS;

	NvDecStats decodeStats = {};
	if (m_nvDecoder)
		m_nvDecoder->GetStats(decodeStats);

	const uint64_t queueDrops = decodeStats.droppedInputQueue;

	// 디코더가 버린 것과 큐가 버린 것을 합친다. 서버 입장에서는 둘 다
	// "이 뷰어가 못 따라간다" 는 같은 신호다.
	const uint64_t decodeDroppedTotal =
		decodeStats.droppedPoolExhausted +
		decodeStats.droppedNotConsumed +
		decodeStats.droppedDisplayFailed +
		queueDrops;

	m_streamingClient->SendFeedback(decodeDroppedTotal);
}

// 끊긴 뒤 다시 붙는다.
//
// 콜백 안이 아니라 여기서 하는 이유는 수명이다. StopClient 는 엔진의
// 워커 스레드들이 빠져나오기를 기다리는데, 그 대기를 워커 스레드가
// 스스로 하면 영원히 끝나지 않는다. 콜백은 표시만 남기고 실제 작업은
// 앱 스레드의 틱(ServiceTick)이 한다.
void DesktopStreamingClientApp::ServiceReconnect(ULONGLONG now)
{
	if (!m_streamingClient)
		return;

	if (::ReadAcquire(&m_reconnectPending) == FALSE)
		return;

	if (now < m_nextReconnectTick)
		return;

	m_nextReconnectTick = now + RECONNECT_INTERVAL_MS;

	// 이미 붙어 있으면 할 일이 없다. (콜백과 루프 사이에 다시 붙은 경우)
	if (m_streamingClient->IsConnected())
	{
		::InterlockedExchange(&m_reconnectPending, FALSE);
		return;
	}

	printf_s("[net] reconnecting to %s:%u ...\n", m_serverIp, m_serverPort);

	m_streamingClient->StopClient();

	if (m_streamingClient->StartClient(m_serverIp, m_serverPort))
	{
		::InterlockedExchange(&m_reconnectPending, FALSE);
	}
}

void DesktopStreamingClientApp::StreamInfoCallback(const DesktopStreamClientSessionContext& streamContext, void* userData)
{
	DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
	if (self)
	{
		self->OnStreamInfo(streamContext);
	}
}

void DesktopStreamingClientApp::FrameCallback(const uint8_t* frameData, uint32_t frameSize, uint64_t /*frameId*/, uint64_t timestamp, uint16_t /*frameType*/, void* userData)
{
	DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
	if (self)
	{
		self->OnEncodedFrame(frameData, frameSize, timestamp);
	}
}

void DesktopStreamingClientApp::DecodedFrameCallback(const D3D11NvDecoder::Frame& frame, void* userData)
{
	DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
	if (self)
	{
		self->OnDecodedFrame(frame);
	}
}

// 컨트롤 바 버튼이 눌렸다. 창 메시지 스레드에서 불린다.
//
// 일시정지와 정지를 의미가 아니라 비용으로 나눈다. 라이브라 되감기가
// 없으므로 둘 다 재개하면 현재 시점에 붙는다. 차이는 재개가 얼마나
// 싸냐다.
//
//   일시정지 : 화면만 멈춘다. 대역폭 그대로, 재개는 다음 프레임
//   정지     : 구독을 해제한다. 대역폭 0, 재개는 SUBSCRIBE + IDR
void DesktopStreamingClientApp::OnViewerUICommand(StreamingViewerCommand command)
{
	switch (command)
	{
	case StreamingViewerCommand::Play:  StartPlayback(); break;
	case StreamingViewerCommand::Pause: PausePlayback(); break;
	case StreamingViewerCommand::Stop:  StopPlayback();  break;
	default: break;
	}
}

// 재생. 정지 상태였다면 구독부터 다시 한다.
void DesktopStreamingClientApp::StartPlayback()
{
	if (m_playbackState == StreamingPlaybackState::Playing)
		return;

	const bool wasStopped = (m_playbackState == StreamingPlaybackState::Stopped);
	m_playbackState = StreamingPlaybackState::Playing;

	if (wasStopped && m_streamingClient)
	{
		// 자동 구독을 먼저 켠다. 지금 연결이 없어도 재접속이 끝나면
		// 그쪽에서 구독을 보내 준다.
		m_streamingClient->SetAutoSubscribe(true);
		m_streamingClient->SendSubscribe();

		// 멈춘 동안 벽시계는 흘렀지만 서버는 인코딩을 쉬었으므로
		// 프레임 순번은 그 자리에 있다. 기준을 버리지 않으면 재개
		// 첫 프레임들이 전부 "늦음" 으로 보여 resync 가 연달아 난다.
		//
		// 서버가 구독 응답으로 스트림 정보를 다시 보내므로
		// OnStreamInfo 도 같은 일을 하지만, 그 순서에 기대지 않는다.
		::InterlockedExchange(&m_paceResetRequest, TRUE);
	}

	::InterlockedExchange(&m_presentEnabled, TRUE);

	printf_s("[viewer-ui] play%s\n", wasStopped ? " (resubscribed)" : "");
	PushPlaybackStateToUI();
}

// 일시정지. 화면만 멈추고 수신과 디코드는 그대로 둔다.
//
// 디코드나 소비를 멈추면 디코더의 드롭 카운터가 오르고, 그 값은
// SendFeedback 으로 서버에 가서 혼잡 신호가 된다. 서버는 2초마다
// 비트레이트를 0.75 배로 깎으므로, 일시정지했다는 이유만으로 화질이
// 하한까지 내려간다. 그래서 파이프라인은 손대지 않고 표시만 막는다.
void DesktopStreamingClientApp::PausePlayback()
{
	if (m_playbackState == StreamingPlaybackState::Paused)
		return;

	m_playbackState = StreamingPlaybackState::Paused;
	::InterlockedExchange(&m_presentEnabled, FALSE);

	printf_s("[viewer-ui] pause\n");
	PushPlaybackStateToUI();
}

// 정지. 구독을 해제한다.
//
// 서버는 구독자가 없으면 OnFrameCallback 첫 줄에서 돌아가므로,
// 이 뷰어가 마지막이면 캡처와 인코딩까지 함께 멈춘다.
void DesktopStreamingClientApp::StopPlayback()
{
	if (m_playbackState == StreamingPlaybackState::Stopped)
		return;

	m_playbackState = StreamingPlaybackState::Stopped;
	::InterlockedExchange(&m_presentEnabled, FALSE);

	if (m_streamingClient)
	{
		// 순서가 중요하다. 자동 구독을 먼저 끄지 않으면 해제 직후
		// 재접속이 일어났을 때 스스로 다시 구독해 버린다.
		m_streamingClient->SetAutoSubscribe(false);
		m_streamingClient->SendUnsubscribe();
	}

	printf_s("[viewer-ui] stop (unsubscribed)\n");
	PushPlaybackStateToUI();
}

void DesktopStreamingClientApp::PushPlaybackStateToUI()
{
	if (m_viewerUI)
	{
		m_viewerUI->SetPlaybackState(m_playbackState);
	}

	// 컨트롤 바의 버튼으로 바꿨든 호스트가 불러서 바꿨든 같이 알린다.
	// WPF 쪽에 자기 재생 버튼이 있다면 이걸 보고 모양을 맞춘다.
	PostHostEvent(HostEvent::PlaybackStateChanged, static_cast<int32_t>(m_playbackState));
}

// 컨트롤 바의 지연 표시와 화질 표시를 갱신한다.
//
// 끝에서 끝까지의 지연은 서버와 시계를 맞춰야 알 수 있고 지금 그런
// 수단이 없다. 대신 클라이언트가 실제로 아는 것을 보여준다 —
// 지터 버퍼 깊이와 최근 평균 대기 시간의 합이다. 즉 "디코딩이 끝난
// 프레임이 화면에 나오기까지 이쪽에서 붙든 시간" 이다.
void DesktopStreamingClientApp::ServiceViewerUI(ULONGLONG now)
{
	// 컨트롤 바가 없어도 계산은 한다. GetStatsSnapshot 이 이 값을 돌려주므로
	// 바를 붙이지 못한 경우에도 호스트는 통계를 받아야 한다. 바에 넣는
	// 줄만 m_viewerUI 를 확인한다.
	if (now < m_nextViewerUITick)
		return;

	const ULONGLONG previousTick = m_lastViewerUITick;
	m_lastViewerUITick = now;
	m_nextViewerUITick = now + VIEWER_UI_INTERVAL_MS;

	// 정지 중에는 화면이 멈춰 있으므로 지연이라는 값 자체가 없다.
	// 지터 버퍼 깊이는 그대로 남아 있지만 그건 화면 지연이 아니다.
	// 0 ms 로 두면 지연이 없는 것처럼 보이므로 "모름" 으로 보낸다.
	const bool stopped = (m_playbackState == StreamingPlaybackState::Stopped);

	float latencyMs = -1.0f;
	if (!stopped)
	{
		const float bufferMs = static_cast<float>(::ReadAcquire(&m_jitterBufferMs));
		const float avgWaitMs = (m_paceWaitCount > 0)
			? static_cast<float>(m_paceWaitTotalMs) / static_cast<float>(m_paceWaitCount)
			: 0.0f;

		latencyMs = bufferMs + avgWaitMs;
	}

	if (m_viewerUI)
	{
		m_viewerUI->SetLatency(latencyMs);
	}

	// --- 화질 표시 ---
	//
	// 해상도와 fps 는 서버가 SC_INFO 로 알려준 값이고, 비트레이트는
	// 서버가 설정한 목표치가 아니라 이 구간에 실제로 도착한 양이다.
	// 서버의 목표치를 보여주면 혼잡으로 실제 수신이 무너진 순간에도
	// 숫자는 멀쩡해 보인다 — 그러면 표시할 이유가 없다.
	DesktopStreamingClientStats netStats = {};
	if (m_streamingClient)
		m_streamingClient->GetStats(netStats);

	const LONG64 geometry = ::ReadAcquire64(&m_streamGeometry);

	// 정지 중이면 전부 0 으로 둔다. 받는 것이 없으니 해상도도 지금
	// 스트림의 것이 아니고, 0.0 Mbps 를 띄우느니 "--" 가 정확하다.
	StreamingQualityInfo qualityInfo = {};

	if (!stopped)
	{
		qualityInfo.width = static_cast<uint32_t>((geometry >> 32) & 0xFFFF);
		qualityInfo.height = static_cast<uint32_t>((geometry >> 16) & 0xFFFF);
		qualityInfo.fps = static_cast<uint32_t>(geometry & 0xFFFF);

		const ULONGLONG elapsedMs = (previousTick > 0 && now > previousTick)
			? (now - previousTick) : 0;

		if (elapsedMs > 0 && netStats.chunkBytesReceived >= m_lastQualityBytes)
		{
			const uint64_t deltaBytes = netStats.chunkBytesReceived - m_lastQualityBytes;

			// bytes/ms -> Mbps : *8 로 비트, /1000 으로 초, /1e6 으로 메가.
			qualityInfo.bitrateMbps =
				static_cast<float>(deltaBytes) * 8.0f / static_cast<float>(elapsedMs) / 1000.0f;
		}
	}

	// 기준은 정지 중에도 옮긴다. 그래야 재개 직후 첫 구간이 멈춰
	// 있던 시간까지 포함한 엉뚱한 평균이 되지 않는다.
	//
	// 재접속으로 카운터가 0 으로 돌아가면 위 비교가 막아 주고,
	// 기준만 새 값으로 옮겨 다음 구간부터 다시 잰다.
	m_lastQualityBytes = netStats.chunkBytesReceived;

	if (m_viewerUI)
	{
		m_viewerUI->SetQualityInfo(qualityInfo);
	}

	ServiceStatsOverlay(now, previousTick, netStats, qualityInfo, latencyMs);
}

// 진단 오버레이에 넣을 값을 모은다.
//
// 손실 카운터를 하나로 합치지 않고 다섯을 따로 넘기는 것이 요점이다.
// 서버로 가는 피드백은 합계여도 되지만(서버는 "이 뷰어가 못 따라간다"
// 하나만 알면 된다), 화면 앞에 앉은 사람은 어느 단계가 막혔는지를
// 알아야 한다. 합쳐 버리면 그 정보가 사라진다.
//
// 호스트가 GetStatsSnapshot 으로 가져갈 값도 여기서 남긴다. 오버레이와
// 호스트가 같은 숫자를 봐야 하므로 계산을 한 곳에서 한다.
void DesktopStreamingClientApp::ServiceStatsOverlay(ULONGLONG now, ULONGLONG previousTick,
	const DesktopStreamingClientStats& netStats,
	const StreamingQualityInfo& qualityInfo,
	float latencyMs)
{
	NvDecStats decodeStats = {};
	if (m_nvDecoder)
		m_nvDecoder->GetStats(decodeStats);

	StreamingStatsInfo stats = {};
	stats.connected = netStats.connected;

	stats.bitrateMbps = qualityInfo.bitrateMbps;
	stats.latencyMs = latencyMs;

	// 실제로 화면에 올라간 프레임 수로 잰다. 디코드된 수가 아니다 —
	// 둘이 벌어지는 것 자체가 표시 쪽이 밀린다는 신호이고, 그건
	// 아래 손실 줄이 따로 말해 준다.
	const ULONGLONG elapsedMs = (previousTick > 0 && now > previousTick)
		? (now - previousTick) : 0;

	if (elapsedMs > 0 && m_presentedFrames >= m_lastPresentedFrames)
	{
		const uint64_t delta = m_presentedFrames - m_lastPresentedFrames;
		stats.presentedFps =
			static_cast<float>(delta) * 1000.0f / static_cast<float>(elapsedMs);
	}

	m_lastPresentedFrames = m_presentedFrames;

	stats.chunksRejected = netStats.chunksRejected;
	stats.framesDiscarded = netStats.framesDiscarded;
	stats.decodeQueueDrops = decodeStats.droppedInputQueue;
	stats.poolExhausted = decodeStats.droppedPoolExhausted;
	stats.notConsumed = decodeStats.droppedNotConsumed;

	stats.jitterBufferMs = static_cast<uint32_t>(::ReadAcquire(&m_jitterBufferMs));
	stats.avgPaceWaitMs = (m_paceWaitCount > 0)
		? static_cast<float>(m_paceWaitTotalMs) / static_cast<float>(m_paceWaitCount)
		: 0.0f;
	stats.resyncCount = m_paceResyncCount;

	m_lastStats = stats;
	m_lastQuality = qualityInfo;

	if (m_viewerUI)
	{
		m_viewerUI->SetStatsInfo(stats);
	}
}

// 컨트롤 바에서 버튼을 눌렀다. 창 메시지 스레드에서 불린다.
void DesktopStreamingClientApp::ViewerUICommandCallback(StreamingViewerCommand command, void* userData)
{
	DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
	if (self)
	{
		self->OnViewerUICommand(command);
	}
}

void DesktopStreamingClientApp::ViewerCloseCallback(void* userData)
{
	DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
	if (self)
	{
		self->OnViewerClosed();
	}
}

// 뷰어 창이 파괴되기 직전에 UI 스레드에서 불린다.
//
// 깃발만 내린다. 네트워크/디코더 정리는 호스트가 IsRunning 이 false 가 된 것을
// 보고 Shutdown 을 부를 때 하던 대로 한다 — 창 메시지 처리 중에 스레드 조인까지 하면 닫기 반응이
// 그만큼 늦어진다.
//
// 이 함수가 돌아가는 즉시 창이 파괴되고 뷰어가 정리되므로, 디코드
// 스레드가 그 뒤로 뷰어를 건드리지 않게 m_viewerAlive 를 먼저 내린다.
void DesktopStreamingClientApp::OnViewerClosed()
{
	::InterlockedExchange(&m_viewerAlive, FALSE);
	RequestStop();

	// 호스트가 Shutdown 으로 지운 경우에는 틱 창이 먼저 사라졌으므로
	// 이 통지는 나가지 않는다. 사용자가 창을 닫은 경우에만 간다.
	PostHostEvent(HostEvent::Stopped, static_cast<int32_t>(HostStopReason::ViewerClosed));
}

void DesktopStreamingClientApp::DecoderErrorCallback(NvDecErrorCode errorCode, void* userData)
{
	DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
	if (self)
	{
		self->OnDecoderError(errorCode);
	}
}

void DesktopStreamingClientApp::ConnectionCallback(DESKTOP_STREAM_CONNECTION_EVENT event, DisconnectReason reason, int errorCode, void* userData)
{
	DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
	if (self)
	{
		self->OnConnectionEvent(event, reason, errorCode);
	}
}

// 엔진 워커 스레드에서 불린다. 표시만 남긴다.
void DesktopStreamingClientApp::OnConnectionEvent(DESKTOP_STREAM_CONNECTION_EVENT event, DisconnectReason reason, int errorCode)
{
	PostHostEvent(HostEvent::ConnectionChanged,
		static_cast<int32_t>(event), static_cast<int32_t>(reason), errorCode);

	switch (event)
	{
	case DESKTOP_STREAM_CONNECTION_EVENT::Established:
		printf_s("[net] connected to %s:%u\n", m_serverIp, m_serverPort);
		::InterlockedExchange(&m_reconnectPending, FALSE);
		break;

	case DESKTOP_STREAM_CONNECTION_EVENT::ConnectFailed:
		printf_s("[net] connect failed (WSA %d)\n", errorCode);
		::InterlockedExchange(&m_reconnectPending, TRUE);
		break;

	case DESKTOP_STREAM_CONNECTION_EVENT::Disconnected:
		printf_s("[net] disconnected : %s\n", ToString(reason));

		// 우리가 내린 종료(StopClient)라면 다시 붙지 않는다.
		// 엔진이 그 판단을 대신 해 준다.
		if (IsRetryableDisconnect(reason))
		{
			::InterlockedExchange(&m_reconnectPending, TRUE);
		}
		break;

	default:
		break;
	}
}

// 디코드 스레드에서 불린다.
void DesktopStreamingClientApp::OnDecoderError(NvDecErrorCode errorCode)
{
	const char* name = "unknown";
	bool fatal = false;

	switch (errorCode)
	{
	case NvDecErrorCode::OutputPoolExhausted: name = "output pool exhausted (frame lost)"; break;
	case NvDecErrorCode::OutputNotConsumed:   name = "output not consumed (frame skipped)"; break;
	case NvDecErrorCode::ParseFailed:         name = "parse failed"; break;
	case NvDecErrorCode::DecodeFailed:        name = "decode failed"; break;
	case NvDecErrorCode::DisplayFailed:       name = "display failed (frame lost)"; break;
	case NvDecErrorCode::ReconfigureFailed:   name = "reconfigure failed"; fatal = true; break;
	case NvDecErrorCode::DecoderFaulted:      name = "decoder faulted"; fatal = true; break;
	default: break;
	}

	printf_s("[decode] %s\n", name);

	if (fatal)
	{
		RequestStop();
		PostHostEvent(HostEvent::Stopped, static_cast<int32_t>(HostStopReason::DecoderFault));
	}
}

void DesktopStreamingClientApp::OnStreamInfo(const DesktopStreamClientSessionContext& streamContext)
{
	printf_s(
		"Stream info: streamId=%u, %ux%u, codec=%u, infoVersion=%u, codecConfigVersion=%u\n",
		streamContext.streamId,
		streamContext.width,
		streamContext.height,
		streamContext.codecType,
		streamContext.streamInfoVersion,
		streamContext.codecConfigVersion);

	// 컨트롤 바의 화질 표시가 읽는다. 다음 ServiceViewerUI 틱에
	// 비트레이트와 함께 올라간다. 읽는 쪽은 앱 스레드다.
	::InterlockedExchange64(&m_streamGeometry,
		PackStreamGeometry(streamContext.width, streamContext.height, streamContext.fps));

	PostHostEvent(HostEvent::StreamInfoChanged,
		streamContext.width, streamContext.height, streamContext.fps);

	// 스트림이 바뀌면 페이싱 기준도 다시 잡아야 한다. 다음 프레임이
	// 잡도록 무효로 표시한다.
	::InterlockedExchange(&m_paceResetRequest, TRUE);
}

void DesktopStreamingClientApp::OnEncodedFrame(const uint8_t* frameData, uint32_t frameSize, uint64_t timestamp)
{
	if (!m_nvDecoder || !frameData || frameSize == 0)
		return;

	NvDecPacket packet = {};
	packet.data = frameData;
	packet.size = frameSize;
	packet.timestamp = timestamp;

	// 실패는 큐가 가득 찬 것이고 그때 가장 오래된 패킷이 버려진다.
	// 그 수는 GetStats 의 droppedInputQueue 로 나오므로 여기서 따로
	// 세지 않는다.
	m_nvDecoder->EnqueuePacket(packet);
}

// --- 지터 버퍼 ---
//
// 디코드 스레드에서 불린다.
//
// 예전에는 도착 즉시 표시했다. 네트워크 지터가 그대로 화면 떨림이 된다 —
// 60fps 라면 프레임 간격이 16ms 인데, 도착이 5ms 빨랐다 20ms 늦었다
// 하면 사람 눈에 끊김으로 보인다.
//
// timestamp 를 쓴다. 이 값은 인코더가 넣은 프레임 순번이 NVDEC 를
// 그대로 통과해 돌아온 것이다(picParams.inputTimeStamp). 서버의
// 프레임 간격이 1/fps 이므로, 순번 차이에 프레임 간격을 곱하면 이
// 프레임이 '표시되어야 할 시각' 이 나온다.
//
//   목표시각 = 기준시각 + (순번 - 기준순번) * (1000/fps) + 버퍼깊이
//
// 목표보다 이르면 그만큼 기다렸다 표시한다. 늦었으면 그냥 표시한다 —
// 이미 늦은 것을 더 늦출 이유가 없다.
//
// 왜 프레임을 쌓아 두지 않고 여기서 기다리는가
//   D3D11NvDecoder::FrameCallback 의 계약이 "frame 은 콜백이 반환할
//   때까지만 유효하다" 다. 텍스처를 들고 나가려면 AcquireFrame /
//   ReleaseFrame 을 직접 돌려야 하는데, 그건 디코더의 슬롯 관리를
//   앱으로 가져오는 일이라 훨씬 크다.
//
//   여기서 기다리면 디코드 스레드가 그만큼 멈추고, 그게 그대로
//   디코더 유입 큐의 백프레셔가 된다. 버퍼 깊이가 얕으면 그 대기도
//   짧으므로 실질적으로는 '소스 박자에 맞춰 푸는' 효과만 남는다.
//
// 대기 상한을 두는 이유
//   기준시각이 한 번 틀어지면(재접속, 스트림 변경) 목표시각이 한참
//   뒤로 갈 수 있다. 그때 그대로 기다리면 화면이 멎는다. 상한을 넘으면
//   기준을 다시 잡는다.
void DesktopStreamingClientApp::OnDecodedFrame(const D3D11NvDecoder::Frame& frame)
{
	if (!m_imageView)
		return;

	// 창이 닫히는 중이면 뷰어를 건드리지 않는다.
	if (::ReadAcquire(&m_viewerAlive) == FALSE)
		return;

	if (!frame.sharedHandle)
		return;

	// 페이싱은 멈춰 있어도 그대로 돈다. 프레임을 계속 실시간으로
	// 소비해야 기준 시각이 유효한 채로 남고, 그래야 재개가 다음
	// 프레임 한 장으로 끝난다.
	PaceFramePresentation(frame.timestamp);

	// 일시정지 / 정지. 화면 갱신만 건너뛴다.
	//
	// 게이트가 파이프라인 맨 끝에 있는 이유는, 여기까지 오면 프레임이
	// 이미 디코더에서 "전달됨" 으로 처리된 뒤라 어떤 드롭 카운터도
	// 오르지 않기 때문이다. 앞쪽에서 막으면 그 드롭이 피드백을 타고
	// 서버로 가서 비트레이트를 깎는다. (PausePlayback 주석 참고)
	if (::ReadAcquire(&m_presentEnabled) == FALSE)
		return;

	m_imageView->UpdateSharedTexture(frame.sharedHandle);
	++m_presentedFrames;
}

void DesktopStreamingClientApp::PaceFramePresentation(uint64_t frameTimestamp)
{
	const uint32_t bufferDepthMs = ::ReadAcquire(&m_jitterBufferMs);

	if (bufferDepthMs == 0)
		return;   // 페이싱을 끄면 예전처럼 즉시 표시한다

	const uint16_t fps = static_cast<uint16_t>(::ReadAcquire(&m_streamFpsAtomic));
	const double frameIntervalMs = 1000.0 / fps;
	const ULONGLONG now = ::GetTickCount64();

	// 기준이 없거나 순번이 뒤로 갔으면(재접속/스트림 변경) 다시 잡는다.
	// 스트림이 바뀌었으면 기준을 버린다. (OnStreamInfo 가 세운다)
	const bool resetRequested = ::InterlockedExchange(&m_paceResetRequest, FALSE) != FALSE;

	if (!m_paceBaseValid || resetRequested || frameTimestamp < m_paceBaseTimestamp)
	{
		m_paceBaseValid = true;
		m_paceBaseTimestamp = frameTimestamp;
		m_paceBaseTick = now;
		return;
	}

	const uint64_t elapsedFrames = frameTimestamp - m_paceBaseTimestamp;
	const ULONGLONG targetTick = m_paceBaseTick +
		static_cast<ULONGLONG>(elapsedFrames * frameIntervalMs) + bufferDepthMs;

	if (targetTick <= now)
	{
		// 늦었다. 누적 지연이 상한을 넘으면 기준을 다시 잡는다 —
		// 그러지 않으면 한 번 밀린 만큼 영원히 밀린 채로 간다.
		if ((now - targetTick) > MAX_PACE_DRIFT_MS)
		{
			m_paceBaseTimestamp = frameTimestamp;
			m_paceBaseTick = now;
			++m_paceResyncCount;
		}
		return;
	}

	ULONGLONG waitMs = targetTick - now;
	if (waitMs > MAX_PACE_WAIT_MS)
	{
		// 여기까지 오면 기준이 틀어진 것이다. 기다리지 않고 다시 잡는다.
		m_paceBaseTimestamp = frameTimestamp;
		m_paceBaseTick = now;
		++m_paceResyncCount;
		return;
	}

	::Sleep(static_cast<DWORD>(waitMs));
	m_paceWaitTotalMs += waitMs;
	++m_paceWaitCount;
}
