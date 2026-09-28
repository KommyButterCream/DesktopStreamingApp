// StreamingClientHost — flat C ABI
//
// DesktopStreamingClientApp(C++)는 맹글링된 심볼과 C++ 타입을 쓰므로
// P/Invoke 로 부를 수 없다. 이 헤더는 같은 DLL 안의 얇은 위임 계층이다.
// WPF 같은 호스트는 이것만 보면 된다. 네이티브 DesktopStreamingClient.exe
// 도 이것만 쓴다 — WPF 가 부를 경로를 C# 없이 먼저 검증하려는 것이다.
//
// 무엇이 경계를 넘지 않는가
//   프레임. 네트워크 → 디코더 → 페이싱 → 뷰어 경로는 전부 DLL 안에 있다.
//   호스트가 받는 것은 제어와 저빈도 통지와 통계뿐이다. C# 이 프레임 경로에
//   끼면 GC 일시정지가 그대로 화면 끊김이 된다.
//
// 규칙 (D3D11ImageViewC.h 와 같다)
//   - bool 대신 int32_t. C++ bool 크기는 처리계 정의이고 C# 마샬링과 어긋난다.
//   - enum 은 int32_t 로 주고받는다.
//   - 예외를 경계 밖으로 내보내지 않는다. 모든 함수가 결과 코드를 반환한다.
//   - 구조체는 첫 멤버가 structSize 다. 필드 추가 시 구버전 호출자가 깨지지 않는다.
//   - 문자열은 UTF-16(wchar_t). C# string 과 그대로 맞는다.
//
// 스레드
//   DSC_Initialize 를 부른 스레드를 "호스트 스레드" 라 한다. WPF 라면 UI
//   스레드다. 그 스레드는 메시지를 펌프해야 한다 — 재접속 / 피드백 같은
//   주기 작업이 그 스레드의 타이머로 돈다. WPF 의 Dispatcher 는 이미 그렇게
//   한다.
//
//   DSC_GetVersion 과 DSC_RequestStop 을 빼면 전부 호스트 스레드에서 부른다.
//   DSC_Create 와 DSC_Set*Callback 은 Initialize 전이라면 어디서 불러도 된다.
//
// 콜백
//   전부 호스트 스레드에서, 메시지 루프를 통해 불린다. 네트워크나 디코더
//   스레드에서 일어난 일도 호스트 스레드로 옮겨 온다. WPF 에서는 콜백 안에서
//   Dispatcher 없이 바로 UI 를 만져도 된다.
//
//   C# 에서 콜백 델리게이트는 필드에 붙잡아 두어야 한다. 지역 변수로 넘기면
//   GC 가 수거하고, 네이티브가 수거된 함수 포인터를 부른다(P/Invoke 의 고전적
//   크래시). DSC_Destroy 가 반환할 때까지 살아 있어야 한다.
//
//   콜백 안에서 DSC_Destroy 를 부르지 않는다. 다른 DSC_* 는 불러도 된다.
//
// 수명
//   DSC_Create → (DSC_Set*Callback) → DSC_Initialize → ... → DSC_Shutdown
//   → DSC_Destroy. 한 핸들에서 Initialize 는 한 번뿐이다. 다른 서버에
//   붙으려면 핸들을 새로 만든다.

#pragma once

#ifdef BUILD_STREAMING_CLIENT_HOST_DLL
#define DSC_API __declspec(dllexport)
#else
#define DSC_API __declspec(dllimport)
#endif

// x64 에는 호출 규약이 하나뿐이라 무의미하지만, D3D11ImageViewC.h 와 맞춘다.
// C# 쪽은 CallingConvention.StdCall 로 선언한다.
#define DSC_CALL __stdcall

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

	// ─────────────────────────────────────────────────────────────────────
	// 결과 코드
	// ─────────────────────────────────────────────────────────────────────
	typedef enum DSC_Result
	{
		DSC_OK = 0,
		DSC_ERR_INVALID_ARG,        // 인자가 null 이거나 범위 밖, structSize 부족
		DSC_ERR_INVALID_HANDLE,     // client 핸들이 null
		DSC_ERR_NOT_INITIALIZED,    // Initialize 전이거나 Shutdown 뒤
		DSC_ERR_INVALID_STATE,      // Initialize 를 두 번 불렀다
		DSC_ERR_INIT_FAILED,        // 디바이스 / 디코더 / 뷰어 생성 실패
		DSC_ERR_FAILED              // 그 외 (내부 예외 포함)
	} DSC_Result;

	// ─────────────────────────────────────────────────────────────────────
	// 값 목록 (콜백과 조회에서 int32_t 로 온다)
	// ─────────────────────────────────────────────────────────────────────
	typedef enum DSC_PlaybackState
	{
		DSC_PLAYBACK_STOPPED = 0,   // 구독 해제. 대역폭 0
		DSC_PLAYBACK_PLAYING,
		DSC_PLAYBACK_PAUSED         // 화면만 정지. 수신과 디코드는 계속
	} DSC_PlaybackState;

	typedef enum DSC_ConnectionEvent
	{
		DSC_CONNECTION_ESTABLISHED = 0,
		DSC_CONNECTION_CONNECT_FAILED,   // errorCode = WSA 오류
		DSC_CONNECTION_DISCONNECTED      // reason = 끊긴 이유 (DSC_GetDisconnectReasonName)
	} DSC_ConnectionEvent;

	// 호스트가 부탁하지 않았는데 스스로 멈춘 이유. 이 통지를 받으면
	// DSC_Shutdown 을 부른다. DSC_RequestStop 으로 멈춘 경우는 통지하지 않는다.
	typedef enum DSC_StopReason
	{
		DSC_STOP_VIEWER_CLOSED = 0,  // 독립 창 모드에서 사용자가 창을 닫았다
		DSC_STOP_DECODER_FAULT       // 디코더가 복구 불가 상태가 됐다
	} DSC_StopReason;

	// ─────────────────────────────────────────────────────────────────────
	// 핸들 / 버전
	// ─────────────────────────────────────────────────────────────────────
	typedef struct DSC_Client_ DSC_Client;

	DSC_API void DSC_CALL DSC_GetVersion(int32_t* outMajor,
	                                     int32_t* outMinor,
	                                     int32_t* outPatch);

	// ─────────────────────────────────────────────────────────────────────
	// 구조체
	//
	// C# 은 [StructLayout(LayoutKind.Sequential)] 로 같은 순서로 선언한다.
	// 64비트 필드 앞의 reserved 는 정렬 패딩을 눈에 보이게 둔 것이다 —
	// 암묵적 패딩에 기대면 C# 쪽 선언이 한 칸 어긋나도 알아채기 어렵다.
	// ─────────────────────────────────────────────────────────────────────
	typedef struct DSC_InitParams
	{
		uint32_t structSize;          // sizeof(DSC_InitParams)
		uint32_t serverPort;          // 0 이면 27015

		// IPv4 점 표기(예: L"192.168.0.10"). 호스트 이름은 받지 않는다 —
		// 네트워크 엔진이 inet_pton(AF_INET) 으로 해석한다. null 이면 127.0.0.1.
		const wchar_t* serverAddress;

		// null 이면 뷰어가 독립 창으로 뜬다(네이티브 exe).
		// 값이 있으면 그 창의 자식 창이 된다(WPF HwndHost.BuildWindowCore 의 hwndParent).
		void* parentHwnd;

		// 픽셀 단위(DIP 아님). 독립 창이면 화면 좌표, 자식 창이면 부모의
		// 클라이언트 좌표다. width / height 가 0 이면 1920 x 900.
		// WPF 에서는 DIP 에 DPI 배율을 곱해서 넘긴다.
		int32_t x;
		int32_t y;
		int32_t width;
		int32_t height;
	} DSC_InitParams;

	// 통계. 수신량 / 표시 fps / 지연은 500ms 주기로 계산해 둔 값이다.
	// 정지(DSC_PLAYBACK_STOPPED) 중에는 스트림 크기와 수신량이 0, 지연이 음수다.
	typedef struct DSC_Stats
	{
		uint32_t structSize;          // 호출자가 sizeof(DSC_Stats) 로 채운다
		int32_t  connected;
		int32_t  playbackState;       // DSC_PlaybackState

		uint32_t streamWidth;
		uint32_t streamHeight;
		uint32_t streamFps;

		float    receivedMbps;        // 서버 목표치가 아니라 실제로 도착한 양
		float    latencyMs;           // 지터 버퍼 + 평균 대기. 음수 = 모름
		float    presentedFps;        // 실제로 화면에 올라간 프레임 기준
		uint32_t jitterBufferMs;
		float    averagePaceWaitMs;
		uint32_t reserved0;

		// 누적 카운터
		uint64_t chunksAccepted;
		uint64_t chunksRejected;      // 검증에서 걸러냄        → 프로토콜 / 재조립
		uint64_t bytesReceived;
		uint64_t framesCompleted;
		uint64_t framesDiscarded;     // 청크가 어긋남          → 네트워크
		uint64_t framesDecoded;
		uint64_t framesPresented;
		uint64_t decodeQueueDrops;    // 유입 큐 넘침           → 디코더가 못 따라감
		uint64_t decodePoolExhausted; // 출력 풀 고갈           → 표시 쪽이 못 따라감
		uint64_t decodeNotConsumed;   // 아무도 안 가져감       → 표시 쪽이 못 따라감
		uint64_t decodeDisplayFailed;
		uint64_t paceResyncCount;     // 재개 / 스트림 변경마다 오르는 것이 정상
	} DSC_Stats;

	// ─────────────────────────────────────────────────────────────────────
	// 콜백 (전부 호스트 스레드)
	// ─────────────────────────────────────────────────────────────────────
	typedef void (DSC_CALL* DSC_ConnectionCallback)(int32_t event,      // DSC_ConnectionEvent
	                                                int32_t reason,     // DISCONNECTED 일 때만 의미
	                                                int32_t errorCode,  // CONNECT_FAILED 일 때 WSA 오류
	                                                void* userData);

	typedef void (DSC_CALL* DSC_StreamInfoCallback)(uint32_t width,
	                                                uint32_t height,
	                                                uint32_t fps,
	                                                void* userData);

	// 컨트롤 바의 버튼으로 바뀌어도, DSC_Play 등으로 바뀌어도 온다.
	// 호스트에 자기 재생 버튼이 있다면 이걸 보고 모양을 맞춘다.
	typedef void (DSC_CALL* DSC_PlaybackStateCallback)(int32_t state,   // DSC_PlaybackState
	                                                   void* userData);

	typedef void (DSC_CALL* DSC_StoppedCallback)(int32_t reason,        // DSC_StopReason
	                                             void* userData);

	// ─────────────────────────────────────────────────────────────────────
	// 생성 / 초기화
	// ─────────────────────────────────────────────────────────────────────
	DSC_API DSC_Result DSC_CALL DSC_Create(DSC_Client** outClient);

	// Shutdown 을 부르지 않았다면 여기서 부른다. 호스트 스레드에서 부른다.
	DSC_API DSC_Result DSC_CALL DSC_Destroy(DSC_Client* client);

	// 콜백은 Initialize 전에 건다. 통지는 메시지 루프에서 배달되므로 루프를
	// 처음 돌리기 전에만 걸면 첫 접속 통지까지 받는다. null 이면 해제한다.
	DSC_API DSC_Result DSC_CALL DSC_SetConnectionCallback(DSC_Client* client,
	                                                      DSC_ConnectionCallback callback,
	                                                      void* userData);
	DSC_API DSC_Result DSC_CALL DSC_SetStreamInfoCallback(DSC_Client* client,
	                                                      DSC_StreamInfoCallback callback,
	                                                      void* userData);
	DSC_API DSC_Result DSC_CALL DSC_SetPlaybackStateCallback(DSC_Client* client,
	                                                         DSC_PlaybackStateCallback callback,
	                                                         void* userData);
	DSC_API DSC_Result DSC_CALL DSC_SetStoppedCallback(DSC_Client* client,
	                                                   DSC_StoppedCallback callback,
	                                                   void* userData);

	// 디바이스 / 디코더 / 뷰어를 만들고 서버에 접속을 시작한다.
	// 서버가 아직 없어도 실패가 아니다 — 계속 다시 붙는다.
	// 호출하는 스레드는 COM 이 초기화돼 있어야 한다(WPF UI 스레드는 이미 STA).
	DSC_API DSC_Result DSC_CALL DSC_Initialize(DSC_Client* client,
	                                           const DSC_InitParams* params);

	DSC_API DSC_Result DSC_CALL DSC_Shutdown(DSC_Client* client);

	// 뷰어 창이 닫히거나 치명적 오류가 나면 0 이 된다. 네이티브 exe 는
	// 이걸 보고 메시지 루프를 끝낸다. WPF 는 DSC_StoppedCallback 을 쓰면 된다.
	DSC_API DSC_Result DSC_CALL DSC_IsRunning(DSC_Client* client, int32_t* outRunning);

	// 어느 스레드에서 불러도 된다(콘솔 Ctrl+C 핸들러 등). IsRunning 을 0 으로
	// 만들 뿐이고 정리는 하지 않는다 — 호스트 스레드에서 DSC_Shutdown 을 부른다.
	DSC_API DSC_Result DSC_CALL DSC_RequestStop(DSC_Client* client);

	// 뷰어 창. WPF HwndHost.BuildWindowCore 가 돌려줄 HandleRef 의 값이다.
	DSC_API DSC_Result DSC_CALL DSC_GetHwnd(DSC_Client* client, void** outHwnd);

	// ─────────────────────────────────────────────────────────────────────
	// 재생 제어
	//
	// 일시정지와 정지는 비용이 다르다. 일시정지는 화면만 멈추고 수신과
	// 디코드를 계속해서 재개가 다음 프레임이다. 정지는 구독을 해제해서
	// 대역폭이 0 이 되고, 재개할 때 IDR 을 기다린다.
	// ─────────────────────────────────────────────────────────────────────
	DSC_API DSC_Result DSC_CALL DSC_Play(DSC_Client* client);
	DSC_API DSC_Result DSC_CALL DSC_Pause(DSC_Client* client);
	DSC_API DSC_Result DSC_CALL DSC_Stop(DSC_Client* client);
	DSC_API DSC_Result DSC_CALL DSC_GetPlaybackState(DSC_Client* client, int32_t* outState);

	// ─────────────────────────────────────────────────────────────────────
	// 조회 / 설정
	// ─────────────────────────────────────────────────────────────────────

	// inOutStats->structSize 를 채워서 넘긴다. 그 크기까지만 쓴다.
	DSC_API DSC_Result DSC_CALL DSC_GetStats(DSC_Client* client, DSC_Stats* inOutStats);

	// 0 이면 페이싱을 끄고 도착하는 대로 표시한다. 기본 16ms.
	DSC_API DSC_Result DSC_CALL DSC_SetJitterBufferMs(DSC_Client* client, uint32_t milliseconds);

	// 뷰어 위에 그려지는 네이티브 컨트롤 바 / 진단 오버레이.
	DSC_API DSC_Result DSC_CALL DSC_SetControlBarVisible(DSC_Client* client, int32_t visible);
	DSC_API DSC_Result DSC_CALL DSC_SetControlBarAutoHide(DSC_Client* client, int32_t enabled);
	DSC_API DSC_Result DSC_CALL DSC_SetStatsOverlayVisible(DSC_Client* client, int32_t visible);

	// DSC_ConnectionCallback 의 reason 을 사람이 읽을 이름으로. 정적 문자열이라
	// 해제하지 않는다. 모르는 값이면 "unrecognised".
	DSC_API const char* DSC_CALL DSC_GetDisconnectReasonName(int32_t reason);

#ifdef __cplusplus
}
#endif
