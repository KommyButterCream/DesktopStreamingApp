#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11_4.h>

#include <stdint.h>

#include "../../../../Module/D3D11Engine/Core/D3D11RenderEngine.h"
#include "../../../../Module/D3D11DuplicateEngine/D3D11DuplicateEngine/D3D11DuplicateEngine.h"
#include "../../../../Module/D3D11DuplicateEngine/D3D11DuplicateEngine/CommonTypes.h"
#include "../../../../Module/NvCodec/NvEncode/D3D11NvEncoder.h"
#include "../StreamingServer/StreamingServer.h"

#ifdef BUILD_STREAMING_SERVER_HOST_DLL
#define STREAMING_SERVER_HOST_API __declspec(dllexport)
#else
#define STREAMING_SERVER_HOST_API __declspec(dllimport)
#endif

// 스트리밍 서버 한 벌.
//
// 캡처(DuplicateEngine) → 인코더(NvEncoder) → 브로드캐스트(StreamingServer)
// 를 잇고, 해상도 변경 재구성 / 인코더 fault 복구 / 비트레이트 적응 /
// 캡처 정지 감지를 맡는다.
//
// 예전에는 DesktopStreamingServer.exe 안의 헤더 전용 클래스였다. WPF 같은
// 다른 호스트에서도 같은 코드를 쓰려고 StreamingServerHost.dll 로 옮겼다.
class STREAMING_SERVER_HOST_API DesktopStreamingServerApp
{
public:
	// --- 목표 스트림 : QHD(2560x1440) 60fps, 서버 하나에 여러 뷰어 ---
	//
	// 프레임률은 여기 한 곳에서만 정한다. 캡처(SetTargetFps)와 인코더
	// (NvEncConfig::frameRateNumerator)와 스트림 정보가 전부 이 값을 쓴다.
	// 예전에는 셋이 서로 다른 값을 들고 있었다.
	//
	// 60 이 되기까지
	//   캡처와 인코더가 같은 D3D11 디바이스를 쓰던 시절, 60fps 에서
	//   수십 프레임 만에 캡처가 영구히 멈췄다. 스택 덤프로 잡은 교착이다.
	//
	//     캡처   : ProcessCaptureFrame -> D3D11ImmediateContextGate::Enter
	//              -> RtlAcquireSRWLockExclusive              (영구 대기)
	//     인코드 : SubmitFrame -> nvEncodeAPI64 -> d3d11
	//              -> nvwgf2umx -> WaitForSingleObjectEx      (게이트 보유)
	//
	//   immediate context 를 지키는 게이트 하나를 두 스레드가 다퉜고,
	//   인코더가 그걸 쥔 채 드라이버 안에서 무기한 블로킹했다. 캡처는
	//   16ms 마다 그 게이트를 원하므로 통째로 죽었다. 30fps 에서는 두
	//   스레드가 겹칠 창이 좁아 드러나지 않았을 뿐이다.
	//
	//   락 규약을 아무리 다듬어도 풀리지 않는 종류였다. 순환의 한 변이
	//   드라이버 안에 있어서, 우리가 그 대기를 놓아 줄 방법이 없다.
	//   (시도해서 아닌 것으로 확인: 복사 완료 대기 켜기 / 인코드 버퍼
	//    4->8 / nvEncEncodePicture 게이트 제거 / NV12 변환 뒤 Flush.
	//    nvEncMapInputResource 의 게이트를 빼면 프로세스가 죽는다)
	//
	//   그래서 게이트 공유 자체를 없앴다. 인코더에 별도 D3D11 디바이스를
	//   주고, 캡처 풀을 KEYEDMUTEX 공유 텍스처로 만들어 인코더 디바이스가
	//   초기화 때 한 번 열어 둔다. 두 컨텍스트가 독립이므로 인코더가
	//   드라이버 안에서 얼마나 오래 기다리든 캡처는 영향을 받지 않는다.
	//   OBS 가 인코더에 별도 디바이스를 주는 것과 같은 구조다.
	static constexpr uint16_t TARGET_FPS = 60;

	// QHD 데스크톱 화면 기준. 글자가 많아 움직임 예측이 잘 듣지 않으므로
	// 같은 해상도의 동영상보다 더 필요하다.
	//
	// 상한은 뷰어 수가 정한다. TCP 로 뷰어마다 같은 스트림을 보내므로
	// 서버 송신량은 비트레이트 x 뷰어 수다. 20Mbps x 8뷰어 = 160Mbps
	// (20MB/s) 로 기가비트 LAN 안에 들어온다. 뷰어가 더 늘거나 대역이
	// 좁으면 이 값을 내려야 하고, 그 조정을 자동으로 하는 것이 앞으로
	// 할 일이다(비트레이트 적응).
	static constexpr uint32_t TARGET_BITRATE_BPS = 20'000'000;
	static constexpr uint32_t MAX_BITRATE_BPS = 25'000'000;

	DesktopStreamingServerApp() = default;
	~DesktopStreamingServerApp();

	// 캡처 / 인코드 스레드와 D3D11 디바이스를 소유한다. 복사하면 둘이
	// 같은 것을 지우게 된다.
	DesktopStreamingServerApp(const DesktopStreamingServerApp&) = delete;
	DesktopStreamingServerApp& operator=(const DesktopStreamingServerApp&) = delete;

public:
	// --- 호스트 이벤트 ---
	//
	// 호스트가 알아야 할 일. 캡처 / 인코더 스레드에서 일어난 일도 전부
	// Initialize 를 부른 스레드로 옮겨서 통지한다. (PostHostEvent 참고)
	//
	// 값 a/b/c 의 뜻은 이벤트마다 다르다.
	enum class HostEvent : uint32_t
	{
		Stopped = 0,           // a = HostStopReason
		ViewerCountChanged,    // a = 구독 중인 뷰어 수
		StreamInfoChanged,     // a = width, b = height, c = fps (해상도 변경 재구성 뒤)
		BitrateChanged,        // a = 이전 bps, b = 새 bps
		CaptureEvent,          // a = CaptureEventCode, b = HRESULT
	};

	// 호스트가 부탁하지 않았는데 스스로 멈춘 이유.
	enum class HostStopReason : int32_t
	{
		CaptureFaulted = 0,     // 캡처 엔진이 복구 불가
		EncoderRebuildLimit,    // 인코더 fault 가 창 안에서 한도를 넘었다
		EncoderRebuildFailed,   // 재구성 자체가 실패했다
	};

	using HostEventCallback = void (*)(HostEvent event, int32_t a, int32_t b, int32_t c, void* userData);

	// Initialize 전에 걸어도 된다. 통지는 메시지 루프에서 배달된다.
	void SetHostEventCallback(HostEventCallback callback, void* userData);

	// Initialize 와 Shutdown 은 같은 스레드에서 불러야 하고, 그 스레드는
	// 메시지를 펌프해야 한다. 해상도 변경 재구성 / 비트레이트 적응 같은
	// 주기 작업이 그 스레드의 타이머로 돈다(ServiceTick 주석 참고).
	//
	// 캡처가 복구 불가로 떨어지거나 인코더 재생성 한도를 넘으면 IsRunning
	// 이 false 가 된다. 호스트는 그걸 보고 Shutdown 을 부른다.
	bool Initialize(uint16_t port = 27015, uint32_t maxViewers = 64);
	void RequestStop();
	bool IsRunning() const;
	void Shutdown();
	void PrintStats();

	// --- 조회 (Initialize 를 부른 스레드에서) ---
	void GetCaptureStats(CaptureStats& stats) const;
	CaptureState GetCaptureState() const;
	void GetEncoderStats(NvEncStats& stats) const;
	void GetServerStats(DesktopStreamingServerStats& stats) const;
	uint32_t GetCurrentBitrate() const;
	void GetStreamSize(uint32_t& width, uint32_t& height) const;

private:
	static constexpr ULONGLONG STATS_INTERVAL_MS = 5'000;

	// fault 재생성 한도. 창 안에서 이만큼 넘게 다시 만들어야 했다면
	// 일시적 오류가 아니라고 보고 멈춘다.
	static constexpr uint32_t MAX_FAULT_REBUILDS = 3;
	static constexpr ULONGLONG FAULT_REBUILD_WINDOW_MS = 60'000;

	// 비트레이트 조정 주기. 너무 짧으면 순간적인 지터에 반응하고,
	// 너무 길면 혼잡이 길게 이어진다.
	static constexpr ULONGLONG BITRATE_INTERVAL_MS = 2'000;

	// 이만큼 연속으로 깨끗해야 비트레이트를 올린다.
	static constexpr uint32_t CLEAN_WINDOWS_TO_RAISE = 3;

	// 더 내려가면 화면을 알아볼 수 없다. QHD 기준 하한.
	static constexpr uint32_t MIN_BITRATE_BPS = 3'000'000;

	// --- 콜백 진입점 ---
	static void FrameCallback(void* userData);
	static void ReleaseCapturedFrameCallback(NvEncInputFrame& inputFrame, void* userData);
	static bool KeyFrameRequestCallback(void* userData);
	static void EncodedPacketCallback(const NvEncPacket& packet, void* userData);
	static void CaptureEventCallback(CaptureEventCode code, HRESULT hr, void* userData);
	static void EncoderErrorCallback(NvEncErrorCode errorCode, void* userData);

	void OnCaptureEvent(CaptureEventCode code, HRESULT hr);
	void OnEncoderError(NvEncErrorCode errorCode);
	void OnFrameCallback();
	void ReleaseCapturedFrame(NvEncInputFrame& inputFrame);
	bool ShouldForceKeyFrame();
	void OnEncodedFrame(const NvEncPacket& frame);

	// --- 주기 작업의 구동 ---
	//
	// 예전에는 exe 의 Run() 루프가 10ms 마다 아래 Service* 를 불렀다. 그
	// 루프는 메시지 펌프와 콘솔 입력까지 같이 하고 있었는데, WPF 에서는
	// 메시지 루프를 Dispatcher 가 소유하고 콘솔이 없다. 그래서 주기 작업만
	// 떼어 이 DLL 안의 타이머로 옮겼다.
	//
	// 서버는 창이 없으므로 메시지 전용 창을 하나 만들어 타이머를 건다.
	// 별도 스레드로 돌리지 않은 이유는 재구성(ServiceStreamRebuild)이
	// 인코더를 부수고 다시 만드는 일이라, 예전과 같은 스레드(Initialize 를
	// 부른 스레드)에서 도는 편이 새로운 경합을 만들지 않기 때문이다.
	static constexpr UINT_PTR TICK_TIMER_ID = 1;
	static constexpr UINT TICK_INTERVAL_MS = 10;

	bool CreateTickWindow();
	void DestroyTickWindow();
	void ServiceTick();
	static LRESULT CALLBACK TickWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

	// 호스트 이벤트를 틱 창으로 보낸다. 어느 스레드에서 불러도 된다.
	// 같은 스레드에서도 곧바로 콜백하지 않고 큐를 거친다 — 재구성 도중에
	// 호스트 코드가 끼어들지 않게 한다.
	static constexpr UINT HOST_EVENT_MESSAGE = WM_APP + 1;
	void PostHostEvent(HostEvent event, int32_t a = 0, int32_t b = 0, int32_t c = 0);

	// --- 앱 스레드에서 주기적으로 도는 일 ---
	void ServiceViewerCount();
	void ServiceBitrateControl(ULONGLONG now);
	bool ServiceStreamRebuild();

	bool InitializeEncoder(uint32_t width, uint32_t height);
	void SyncStreamSelfHealing();

private:
	// 콘솔 핸들러 스레드나 워커 콜백이 RequestStop 으로 내리고, 호스트의
	// 루프와 ServiceTick 이 읽는다. 평범한 bool 이었다 — 여러 스레드가
	// 보는 값이라 원자 연산을 쓴다.
	volatile LONG m_running = FALSE;

	// 주기 작업 타이머가 걸린 메시지 전용 창. Initialize 를 부른 스레드 소유다.
	//
	// 캡처 / 인코더 스레드도 PostHostEvent 에서 이 값을 읽는다. 그래서
	// 쓰기는 InterlockedExchangePointer, 읽기는 ReadPointerAcquire 로 한다.
	HWND m_tickWindow = nullptr;

	HostEventCallback m_hostEventCallback = nullptr;
	void* m_hostEventUserData = nullptr;

	// 직전 틱의 구독 뷰어 수. 바뀌면 ViewerCountChanged 를 보낸다.
	// StreamingServer 에는 구독 변화 콜백이 없어서 틱에서 비교한다.
	uint32_t m_lastViewerCount = 0;

	// ServiceTick 이 도는 중인가. 같은 스레드에서만 만진다.
	//
	// 예전 Run() 루프에서는 한 틱이 끝나야 다음 틱이 왔다. 타이머로 바꾸면
	// 틱 안에서 누군가 메시지를 펌프할 때(모달 루프 등) WM_TIMER 가 다시
	// 들어올 수 있다. 재구성 도중에 재구성이 겹치면 안 되므로 막는다.
	bool m_inTick = false;

	ULONGLONG m_nextStatsTick = 0;

	// 비트레이트 적응 상태. 앱 스레드에서만 만진다.
	ULONGLONG m_nextBitrateTick = 0;
	uint32_t m_currentBitrateBps = TARGET_BITRATE_BPS;
	uint32_t m_cleanWindows = 0;
	uint64_t m_lastChunksFailed = 0;
	uint64_t m_lastViewerFrameIncomplete = 0;
	uint64_t m_lastViewerDiscarded = 0;
	uint64_t m_lastViewerDecodeDropped = 0;

	// 인코더 재생성 표시. 워커 스레드가 세우고 ServiceTick 이 내린다.
	// 해상도 변경(캡처 스레드)과 인코더 fault(완료 스레드)가 같이 쓴다.
	volatile LONG m_streamRebuildPending = FALSE;
	volatile LONG m_rebuildFromFault = FALSE;

	// fault 로 인한 재생성 횟수. ServiceTick 만 만진다.
	// 창 안에서 한도를 넘으면 계속 되살려도 소용없다고 보고 포기한다.
	uint32_t m_faultRebuildCount = 0;
	uint64_t m_lastFaultRebuildTick = 0;

	// 지금 스트림 정보의 크기. 재구성 때 비교용이다.
	uint16_t m_streamWidth = 0;
	uint16_t m_streamHeight = 0;

	// 캡처 정지 감지용. PrintStats 만 만진다(주기 호출은 ServiceTick).
	uint64_t m_lastCapturedFrames = 0;
	uint64_t m_lastCaptureTimeouts = 0;
	uint64_t m_captureStallTicks = 0;
	uint64_t m_statsPrintCount = 0;

	D3D11RenderEngine* m_D3D11Engine = nullptr;

	// 인코더 전용 디바이스. 캡처와 컨텍스트를 나누기 위한 것이다.
	D3D11RenderEngine* m_encodeEngine = nullptr;

	// 슬롯 번호 -> 캡처 텍스처. 큐에는 슬롯만 싣고 반납할 때 여기서 되찾는다.
	// 캡처 스레드가 쓰고 인코드 스레드가 읽지만, 슬롯당 한 번 쓰고 그 슬롯이
	// 반납될 때까지 바뀌지 않으므로 경합이 없다.
	static constexpr int64_t MAX_CAPTURE_SLOTS = 16;
	ID3D11Texture2D* m_captureTextureBySlot[MAX_CAPTURE_SLOTS] = {};
	D3D11DuplicateEngine* m_duplicateEngine = nullptr;
	D3D11NvEncoder* m_nvEncoder = nullptr;
	StreamingServer* m_streamingServer = nullptr;
};
