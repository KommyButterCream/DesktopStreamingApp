// StreamingServerHostC.h 의 구현. 로직은 없다 — 핸들을 객체로 풀고,
// 인자를 검사하고, 예외를 결과 코드로 바꾸고, C++ 이벤트를 C 콜백으로
// 나눠 보낼 뿐이다.

#include "StreamingServerHostC.h"
#include "DesktopStreamingServerApp.h"

#include <new>
#include <stddef.h>
#include <string.h>

static_assert(static_cast<int32_t>(CaptureState::Idle) == DSS_CAPTURE_IDLE, "capture state mismatch");
static_assert(static_cast<int32_t>(CaptureState::Running) == DSS_CAPTURE_RUNNING, "capture state mismatch");
static_assert(static_cast<int32_t>(CaptureState::Reconnecting) == DSS_CAPTURE_RECONNECTING, "capture state mismatch");
static_assert(static_cast<int32_t>(CaptureState::Faulted) == DSS_CAPTURE_FAULTED, "capture state mismatch");

static_assert(static_cast<int32_t>(CaptureEventCode::None) == DSS_CAPTURE_EVENT_NONE, "capture event mismatch");
static_assert(static_cast<int32_t>(CaptureEventCode::AccessLost) == DSS_CAPTURE_EVENT_ACCESS_LOST, "capture event mismatch");
static_assert(static_cast<int32_t>(CaptureEventCode::Reconnecting) == DSS_CAPTURE_EVENT_RECONNECTING, "capture event mismatch");
static_assert(static_cast<int32_t>(CaptureEventCode::Reconnected) == DSS_CAPTURE_EVENT_RECONNECTED, "capture event mismatch");
static_assert(static_cast<int32_t>(CaptureEventCode::ModeChanged) == DSS_CAPTURE_EVENT_MODE_CHANGED, "capture event mismatch");
static_assert(static_cast<int32_t>(CaptureEventCode::DeviceRemoved) == DSS_CAPTURE_EVENT_DEVICE_REMOVED, "capture event mismatch");
static_assert(static_cast<int32_t>(CaptureEventCode::DeviceRecreated) == DSS_CAPTURE_EVENT_DEVICE_RECREATED, "capture event mismatch");
static_assert(static_cast<int32_t>(CaptureEventCode::Faulted) == DSS_CAPTURE_EVENT_FAULTED, "capture event mismatch");

static_assert(static_cast<int32_t>(DesktopStreamingServerApp::HostStopReason::CaptureFaulted) == DSS_STOP_CAPTURE_FAULTED, "stop reason mismatch");
static_assert(static_cast<int32_t>(DesktopStreamingServerApp::HostStopReason::EncoderRebuildLimit) == DSS_STOP_ENCODER_REBUILD_LIMIT, "stop reason mismatch");
static_assert(static_cast<int32_t>(DesktopStreamingServerApp::HostStopReason::EncoderRebuildFailed) == DSS_STOP_ENCODER_REBUILD_FAILED, "stop reason mismatch");

// C# 이 같은 크기로 선언했는지 확인할 기준. 바뀌면 C# 쪽도 고쳐야 한다.
static_assert(sizeof(DSS_Stats) == 200, "DSS_Stats layout changed - update the C# declaration");
static_assert(offsetof(DSS_Stats, capturedFrames) == 40, "DSS_Stats 64-bit block must start 8-byte aligned");

struct DSS_Server_
{
	DesktopStreamingServerApp app;

	bool initialized = false;
	bool shutDown = false;

	DSS_StoppedCallback stoppedCallback = nullptr;
	void* stoppedUserData = nullptr;

	DSS_ViewerCountCallback viewerCountCallback = nullptr;
	void* viewerCountUserData = nullptr;

	DSS_StreamInfoCallback streamInfoCallback = nullptr;
	void* streamInfoUserData = nullptr;

	DSS_BitrateCallback bitrateCallback = nullptr;
	void* bitrateUserData = nullptr;

	DSS_CaptureEventCallback captureEventCallback = nullptr;
	void* captureEventUserData = nullptr;
};

namespace
{
	constexpr int32_t VERSION_MAJOR = 1;
	constexpr int32_t VERSION_MINOR = 0;
	constexpr int32_t VERSION_PATCH = 0;

	constexpr uint16_t DEFAULT_PORT = 27015;
	constexpr uint32_t DEFAULT_MAX_VIEWERS = 64;

	// 호스트 스레드에서, 메시지 루프를 통해 불린다.
	void OnHostEvent(DesktopStreamingServerApp::HostEvent event,
		int32_t a, int32_t b, int32_t c, void* userData)
	{
		DSS_Server_* server = static_cast<DSS_Server_*>(userData);
		if (!server)
			return;

		try
		{
			switch (event)
			{
			case DesktopStreamingServerApp::HostEvent::Stopped:
				if (server->stoppedCallback)
					server->stoppedCallback(a, server->stoppedUserData);
				break;

			case DesktopStreamingServerApp::HostEvent::ViewerCountChanged:
				if (server->viewerCountCallback)
					server->viewerCountCallback(static_cast<uint32_t>(a), server->viewerCountUserData);
				break;

			case DesktopStreamingServerApp::HostEvent::StreamInfoChanged:
				if (server->streamInfoCallback)
					server->streamInfoCallback(static_cast<uint32_t>(a), static_cast<uint32_t>(b),
						static_cast<uint32_t>(c), server->streamInfoUserData);
				break;

			case DesktopStreamingServerApp::HostEvent::BitrateChanged:
				if (server->bitrateCallback)
					server->bitrateCallback(static_cast<uint32_t>(a), static_cast<uint32_t>(b),
						server->bitrateUserData);
				break;

			case DesktopStreamingServerApp::HostEvent::CaptureEvent:
				if (server->captureEventCallback)
					server->captureEventCallback(a, b, server->captureEventUserData);
				break;

			default:
				break;
			}
		}
		catch (...)
		{
		}
	}

	DSS_Result RequireInitialized(const DSS_Server_* server)
	{
		if (!server)
			return DSS_ERR_INVALID_HANDLE;

		if (!server->initialized || server->shutDown)
			return DSS_ERR_NOT_INITIALIZED;

		return DSS_OK;
	}
}

extern "C"
{
	DSS_API void DSS_CALL DSS_GetVersion(int32_t* outMajor, int32_t* outMinor, int32_t* outPatch)
	{
		if (outMajor) *outMajor = VERSION_MAJOR;
		if (outMinor) *outMinor = VERSION_MINOR;
		if (outPatch) *outPatch = VERSION_PATCH;
	}

	DSS_API DSS_Result DSS_CALL DSS_Create(DSS_Server** outServer)
	{
		if (!outServer)
			return DSS_ERR_INVALID_ARG;

		*outServer = nullptr;

		try
		{
			DSS_Server_* server = new (std::nothrow) DSS_Server_();
			if (!server)
				return DSS_ERR_FAILED;

			server->app.SetHostEventCallback(&OnHostEvent, server);
			*outServer = server;
			return DSS_OK;
		}
		catch (...)
		{
			return DSS_ERR_FAILED;
		}
	}

	DSS_API DSS_Result DSS_CALL DSS_Destroy(DSS_Server* server)
	{
		if (!server)
			return DSS_ERR_INVALID_HANDLE;

		try
		{
			// 소멸자가 Shutdown 을 부른다.
			delete server;
			return DSS_OK;
		}
		catch (...)
		{
			return DSS_ERR_FAILED;
		}
	}

	DSS_API DSS_Result DSS_CALL DSS_SetStoppedCallback(DSS_Server* server,
		DSS_StoppedCallback callback, void* userData)
	{
		if (!server)
			return DSS_ERR_INVALID_HANDLE;

		server->stoppedCallback = callback;
		server->stoppedUserData = userData;
		return DSS_OK;
	}

	DSS_API DSS_Result DSS_CALL DSS_SetViewerCountCallback(DSS_Server* server,
		DSS_ViewerCountCallback callback, void* userData)
	{
		if (!server)
			return DSS_ERR_INVALID_HANDLE;

		server->viewerCountCallback = callback;
		server->viewerCountUserData = userData;
		return DSS_OK;
	}

	DSS_API DSS_Result DSS_CALL DSS_SetStreamInfoCallback(DSS_Server* server,
		DSS_StreamInfoCallback callback, void* userData)
	{
		if (!server)
			return DSS_ERR_INVALID_HANDLE;

		server->streamInfoCallback = callback;
		server->streamInfoUserData = userData;
		return DSS_OK;
	}

	DSS_API DSS_Result DSS_CALL DSS_SetBitrateCallback(DSS_Server* server,
		DSS_BitrateCallback callback, void* userData)
	{
		if (!server)
			return DSS_ERR_INVALID_HANDLE;

		server->bitrateCallback = callback;
		server->bitrateUserData = userData;
		return DSS_OK;
	}

	DSS_API DSS_Result DSS_CALL DSS_SetCaptureEventCallback(DSS_Server* server,
		DSS_CaptureEventCallback callback, void* userData)
	{
		if (!server)
			return DSS_ERR_INVALID_HANDLE;

		server->captureEventCallback = callback;
		server->captureEventUserData = userData;
		return DSS_OK;
	}

	DSS_API DSS_Result DSS_CALL DSS_Initialize(DSS_Server* server, const DSS_InitParams* params)
	{
		if (!server)
			return DSS_ERR_INVALID_HANDLE;

		if (!params || params->structSize < sizeof(DSS_InitParams))
			return DSS_ERR_INVALID_ARG;

		// Shutdown 뒤 재초기화는 지원하지 않는다. (StreamingClientHostC.cpp 와 같은 이유)
		if (server->initialized || server->shutDown)
			return DSS_ERR_INVALID_STATE;

		if (params->port > 0xFFFF)
			return DSS_ERR_INVALID_ARG;

		const uint16_t port = (params->port != 0)
			? static_cast<uint16_t>(params->port) : DEFAULT_PORT;
		const uint32_t maxViewers = (params->maxViewers != 0)
			? params->maxViewers : DEFAULT_MAX_VIEWERS;

		try
		{
			if (!server->app.Initialize(port, maxViewers))
				return DSS_ERR_INIT_FAILED;

			server->initialized = true;
			return DSS_OK;
		}
		catch (...)
		{
			return DSS_ERR_FAILED;
		}
	}

	DSS_API DSS_Result DSS_CALL DSS_Shutdown(DSS_Server* server)
	{
		if (!server)
			return DSS_ERR_INVALID_HANDLE;

		if (!server->initialized || server->shutDown)
			return DSS_ERR_NOT_INITIALIZED;

		try
		{
			server->app.Shutdown();
			server->shutDown = true;
			return DSS_OK;
		}
		catch (...)
		{
			server->shutDown = true;
			return DSS_ERR_FAILED;
		}
	}

	DSS_API DSS_Result DSS_CALL DSS_IsRunning(DSS_Server* server, int32_t* outRunning)
	{
		if (!server)
			return DSS_ERR_INVALID_HANDLE;

		if (!outRunning)
			return DSS_ERR_INVALID_ARG;

		*outRunning = (server->initialized && !server->shutDown && server->app.IsRunning()) ? 1 : 0;
		return DSS_OK;
	}

	DSS_API DSS_Result DSS_CALL DSS_RequestStop(DSS_Server* server)
	{
		if (!server)
			return DSS_ERR_INVALID_HANDLE;

		server->app.RequestStop();
		return DSS_OK;
	}

	DSS_API DSS_Result DSS_CALL DSS_GetStats(DSS_Server* server, DSS_Stats* inOutStats)
	{
		const DSS_Result state = RequireInitialized(server);
		if (state != DSS_OK)
			return state;

		if (!inOutStats || inOutStats->structSize < sizeof(uint32_t))
			return DSS_ERR_INVALID_ARG;

		try
		{
			CaptureStats capture = {};
			server->app.GetCaptureStats(capture);

			NvEncStats encoder = {};
			server->app.GetEncoderStats(encoder);

			DesktopStreamingServerStats network = {};
			server->app.GetServerStats(network);

			uint32_t width = 0;
			uint32_t height = 0;
			server->app.GetStreamSize(width, height);

			DSS_Stats full = {};
			full.structSize = sizeof(DSS_Stats);
			full.captureState = static_cast<int32_t>(server->app.GetCaptureState());
			full.streamWidth = width;
			full.streamHeight = height;
			full.streamFps = DesktopStreamingServerApp::TARGET_FPS;
			full.viewerCount = network.subscribedViewerCount;
			full.currentBitrateBps = server->app.GetCurrentBitrate();
			full.targetBitrateBps = DesktopStreamingServerApp::TARGET_BITRATE_BPS;
			full.encodePendingFrames = encoder.pendingFrames;
			full.encoderFaulted = encoder.faulted ? 1 : 0;

			full.capturedFrames = capture.capturedFrames;
			full.captureSkippedFrames = capture.skippedFrames;
			full.captureDroppedFrames = capture.droppedFrames;
			full.captureTimeouts = capture.timeoutCount;
			full.captureAccessLost = capture.accessLostCount;

			full.encodeSubmitted = encoder.submittedFrames;
			full.encodeCompleted = encoder.completedFrames;
			full.encodeLost = encoder.lostFrames;
			full.encodeQueueDrops = encoder.droppedInputQueue;

			full.framesOffered = network.framesOffered;
			full.framesDelivered = network.framesDelivered;
			full.framesSkippedNoViewer = network.framesSkippedNoViewer;
			full.framesAborted = network.framesAborted;
			full.chunksFailed = network.chunksFailed;
			full.viewerFrameIncomplete = network.viewerFrameIncomplete;
			full.keyframesForced = network.keyframesForced;

			full.feedbackReports = network.feedbackReports;
			full.viewerFramesDiscarded = network.viewerFramesDiscarded;
			full.viewerDecodeDropped = network.viewerDecodeDropped;
			full.bytesQueued = network.bytesQueued;

			const uint32_t writable = (inOutStats->structSize < sizeof(DSS_Stats))
				? inOutStats->structSize : static_cast<uint32_t>(sizeof(DSS_Stats));

			::memcpy(inOutStats, &full, writable);
			inOutStats->structSize = writable;
			return DSS_OK;
		}
		catch (...)
		{
			return DSS_ERR_FAILED;
		}
	}
}
