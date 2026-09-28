// StreamingServerHost — flat C ABI
//
// DesktopStreamingServerApp(C++)를 P/Invoke 로 부를 수 있게 하는 같은 DLL
// 안의 얇은 위임 계층이다. WPF 서버 UI 는 이것만 보면 된다. 네이티브
// DesktopStreamingServer.exe 도 이것만 쓴다.
//
// 무엇이 경계를 넘지 않는가
//   프레임. 캡처 → 인코더 → 브로드캐스트 경로는 전부 DLL 안에 있다.
//
// 규칙은 StreamingClientHostC.h 와 같다.
//   - bool 대신 int32_t, enum 은 int32_t 로 주고받는다.
//   - 예외를 경계 밖으로 내보내지 않는다.
//   - 구조체는 첫 멤버가 structSize 다.
//
// 스레드
//   DSS_Initialize 를 부른 스레드를 "호스트 스레드" 라 한다. 그 스레드는
//   메시지를 펌프해야 한다 — 해상도 변경 재구성 / 비트레이트 적응 같은
//   주기 작업이 그 스레드의 타이머로 돈다.
//
//   DSS_GetVersion 과 DSS_RequestStop 을 빼면 전부 호스트 스레드에서 부른다.
//
// 콜백
//   전부 호스트 스레드에서, 메시지 루프를 통해 불린다. 캡처 / 인코더
//   스레드에서 일어난 일도 호스트 스레드로 옮겨 온다.
//
//   C# 에서 콜백 델리게이트는 필드에 붙잡아 두어야 한다(GC 가 수거하면
//   네이티브가 수거된 함수 포인터를 부른다). DSS_Destroy 가 반환할 때까지.
//
//   콜백 안에서 DSS_Destroy 를 부르지 않는다.
//
// 아직 열지 않은 것
//   프레임률과 목표 비트레이트는 C++ 쪽에서 컴파일 타임 상수라 설정할 수
//   없다(DSS_Stats 로 읽을 수만 있다). 모니터 선택도 아직 없다 — 주 모니터를
//   캡처한다.

#pragma once

#ifdef BUILD_STREAMING_SERVER_HOST_DLL
#define DSS_API __declspec(dllexport)
#else
#define DSS_API __declspec(dllimport)
#endif

#define DSS_CALL __stdcall

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

	// ─────────────────────────────────────────────────────────────────────
	// 결과 코드
	// ─────────────────────────────────────────────────────────────────────
	typedef enum DSS_Result
	{
		DSS_OK = 0,
		DSS_ERR_INVALID_ARG,
		DSS_ERR_INVALID_HANDLE,
		DSS_ERR_NOT_INITIALIZED,
		DSS_ERR_INVALID_STATE,      // Initialize 를 두 번 불렀다
		DSS_ERR_INIT_FAILED,        // 디바이스 / 캡처 / 인코더 / 리스닝 실패
		DSS_ERR_FAILED
	} DSS_Result;

	// ─────────────────────────────────────────────────────────────────────
	// 값 목록
	// ─────────────────────────────────────────────────────────────────────
	typedef enum DSS_CaptureState
	{
		DSS_CAPTURE_IDLE = 0,
		DSS_CAPTURE_RUNNING,
		DSS_CAPTURE_RECONNECTING,
		DSS_CAPTURE_FAULTED
	} DSS_CaptureState;

	typedef enum DSS_CaptureEvent
	{
		DSS_CAPTURE_EVENT_NONE = 0,
		DSS_CAPTURE_EVENT_ACCESS_LOST,       // 잠금 화면 / 전체 화면 전환 등. 자동 재연결에 들어간다
		DSS_CAPTURE_EVENT_RECONNECTING,
		DSS_CAPTURE_EVENT_RECONNECTED,
		DSS_CAPTURE_EVENT_MODE_CHANGED,      // 해상도 변경. 인코드 파이프라인이 다시 만들어진다
		DSS_CAPTURE_EVENT_DEVICE_REMOVED,
		DSS_CAPTURE_EVENT_DEVICE_RECREATED,
		DSS_CAPTURE_EVENT_FAULTED            // 복구 불가. DSS_StoppedCallback 이 뒤따른다
	} DSS_CaptureEvent;

	// 호스트가 부탁하지 않았는데 스스로 멈춘 이유. 받으면 DSS_Shutdown 을 부른다.
	typedef enum DSS_StopReason
	{
		DSS_STOP_CAPTURE_FAULTED = 0,
		DSS_STOP_ENCODER_REBUILD_LIMIT,      // 인코더 fault 가 1분 안에 3번을 넘었다
		DSS_STOP_ENCODER_REBUILD_FAILED
	} DSS_StopReason;

	// ─────────────────────────────────────────────────────────────────────
	// 핸들 / 버전
	// ─────────────────────────────────────────────────────────────────────
	typedef struct DSS_Server_ DSS_Server;

	DSS_API void DSS_CALL DSS_GetVersion(int32_t* outMajor,
	                                     int32_t* outMinor,
	                                     int32_t* outPatch);

	// ─────────────────────────────────────────────────────────────────────
	// 구조체
	// ─────────────────────────────────────────────────────────────────────
	typedef struct DSS_InitParams
	{
		uint32_t structSize;         // sizeof(DSS_InitParams)
		uint32_t port;               // 0 이면 27015
		uint32_t maxViewers;         // 0 이면 64
	} DSS_InitParams;

	typedef struct DSS_Stats
	{
		uint32_t structSize;         // 호출자가 sizeof(DSS_Stats) 로 채운다
		int32_t  captureState;       // DSS_CaptureState

		uint32_t streamWidth;
		uint32_t streamHeight;
		uint32_t streamFps;

		uint32_t viewerCount;        // 구독 중인 뷰어
		uint32_t currentBitrateBps;  // 적응형 조정이 지금 쓰는 값
		uint32_t targetBitrateBps;   // 혼잡이 없을 때 돌아가는 상한
		uint32_t encodePendingFrames;
		int32_t  encoderFaulted;

		// 누적 카운터
		uint64_t capturedFrames;
		uint64_t captureSkippedFrames;   // 화면 변화가 없어 건너뜀 (정상)
		uint64_t captureDroppedFrames;
		uint64_t captureTimeouts;
		uint64_t captureAccessLost;

		uint64_t encodeSubmitted;
		uint64_t encodeCompleted;
		uint64_t encodeLost;
		uint64_t encodeQueueDrops;

		uint64_t framesOffered;
		uint64_t framesDelivered;
		uint64_t framesSkippedNoViewer;
		uint64_t framesAborted;
		uint64_t chunksFailed;           // 뷰어 송신 큐가 찼다 → 비트레이트를 내리는 신호
		uint64_t viewerFrameIncomplete;
		uint64_t keyframesForced;

		uint64_t feedbackReports;
		uint64_t viewerFramesDiscarded;  // 뷰어가 보고한 값의 합
		uint64_t viewerDecodeDropped;    // 뷰어가 보고한 값의 합
		uint64_t bytesQueued;
	} DSS_Stats;

	// ─────────────────────────────────────────────────────────────────────
	// 콜백 (전부 호스트 스레드)
	// ─────────────────────────────────────────────────────────────────────
	typedef void (DSS_CALL* DSS_StoppedCallback)(int32_t reason,              // DSS_StopReason
	                                             void* userData);

	typedef void (DSS_CALL* DSS_ViewerCountCallback)(uint32_t viewerCount,
	                                                 void* userData);

	// 해상도 변경 재구성이 끝난 뒤. 뷰어들에게도 이미 알렸다.
	typedef void (DSS_CALL* DSS_StreamInfoCallback)(uint32_t width,
	                                                uint32_t height,
	                                                uint32_t fps,
	                                                void* userData);

	// 적응형 비트레이트가 값을 바꿨다. 내릴 때 x0.75, 올릴 때 x1.15.
	typedef void (DSS_CALL* DSS_BitrateCallback)(uint32_t previousBps,
	                                             uint32_t currentBps,
	                                             void* userData);

	typedef void (DSS_CALL* DSS_CaptureEventCallback)(int32_t event,       // DSS_CaptureEvent
	                                                  int32_t hresult,
	                                                  void* userData);

	// ─────────────────────────────────────────────────────────────────────
	// 생성 / 초기화
	// ─────────────────────────────────────────────────────────────────────
	DSS_API DSS_Result DSS_CALL DSS_Create(DSS_Server** outServer);
	DSS_API DSS_Result DSS_CALL DSS_Destroy(DSS_Server* server);

	// Initialize 전에 건다. null 이면 해제한다.
	DSS_API DSS_Result DSS_CALL DSS_SetStoppedCallback(DSS_Server* server,
	                                                   DSS_StoppedCallback callback,
	                                                   void* userData);
	DSS_API DSS_Result DSS_CALL DSS_SetViewerCountCallback(DSS_Server* server,
	                                                       DSS_ViewerCountCallback callback,
	                                                       void* userData);
	DSS_API DSS_Result DSS_CALL DSS_SetStreamInfoCallback(DSS_Server* server,
	                                                      DSS_StreamInfoCallback callback,
	                                                      void* userData);
	DSS_API DSS_Result DSS_CALL DSS_SetBitrateCallback(DSS_Server* server,
	                                                   DSS_BitrateCallback callback,
	                                                   void* userData);
	DSS_API DSS_Result DSS_CALL DSS_SetCaptureEventCallback(DSS_Server* server,
	                                                        DSS_CaptureEventCallback callback,
	                                                        void* userData);

	// 캡처 / 인코더를 만들고 리스닝을 시작한다. 호출하는 스레드는 COM 이
	// 초기화돼 있어야 한다.
	DSS_API DSS_Result DSS_CALL DSS_Initialize(DSS_Server* server,
	                                           const DSS_InitParams* params);

	DSS_API DSS_Result DSS_CALL DSS_Shutdown(DSS_Server* server);

	DSS_API DSS_Result DSS_CALL DSS_IsRunning(DSS_Server* server, int32_t* outRunning);

	// 어느 스레드에서 불러도 된다. 정리는 호스트 스레드에서 DSS_Shutdown 으로.
	DSS_API DSS_Result DSS_CALL DSS_RequestStop(DSS_Server* server);

	// inOutStats->structSize 를 채워서 넘긴다. 그 크기까지만 쓴다.
	DSS_API DSS_Result DSS_CALL DSS_GetStats(DSS_Server* server, DSS_Stats* inOutStats);

#ifdef __cplusplus
}
#endif
