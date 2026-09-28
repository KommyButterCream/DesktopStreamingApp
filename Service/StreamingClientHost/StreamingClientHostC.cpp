// StreamingClientHostC.h 의 구현. 로직은 없다 — 핸들을 객체로 풀고,
// 인자를 검사하고, 예외를 결과 코드로 바꾸고, C++ 이벤트를 C 콜백으로
// 나눠 보낼 뿐이다.

#include "StreamingClientHostC.h"
#include "DesktopStreamingClientApp.h"

#include <new>
#include <stddef.h>
#include <string.h>

// C 쪽 값과 C++ 쪽 값이 같은 수여야 캐스트 한 번으로 넘길 수 있다.
// 어느 한쪽 열거형이 바뀌면 여기서 빌드가 멈춘다.
static_assert(static_cast<int32_t>(StreamingPlaybackState::Stopped) == DSC_PLAYBACK_STOPPED, "playback state mismatch");
static_assert(static_cast<int32_t>(StreamingPlaybackState::Playing) == DSC_PLAYBACK_PLAYING, "playback state mismatch");
static_assert(static_cast<int32_t>(StreamingPlaybackState::Paused) == DSC_PLAYBACK_PAUSED, "playback state mismatch");

static_assert(static_cast<int32_t>(DESKTOP_STREAM_CONNECTION_EVENT::Established) == DSC_CONNECTION_ESTABLISHED, "connection event mismatch");
static_assert(static_cast<int32_t>(DESKTOP_STREAM_CONNECTION_EVENT::ConnectFailed) == DSC_CONNECTION_CONNECT_FAILED, "connection event mismatch");
static_assert(static_cast<int32_t>(DESKTOP_STREAM_CONNECTION_EVENT::Disconnected) == DSC_CONNECTION_DISCONNECTED, "connection event mismatch");

static_assert(static_cast<int32_t>(DesktopStreamingClientApp::HostStopReason::ViewerClosed) == DSC_STOP_VIEWER_CLOSED, "stop reason mismatch");
static_assert(static_cast<int32_t>(DesktopStreamingClientApp::HostStopReason::DecoderFault) == DSC_STOP_DECODER_FAULT, "stop reason mismatch");

// C# 이 같은 크기로 선언했는지 확인할 기준. 바뀌면 C# 쪽도 고쳐야 한다.
static_assert(sizeof(DSC_Stats) == 144, "DSC_Stats layout changed - update the C# declaration");
static_assert(offsetof(DSC_Stats, chunksAccepted) == 48, "DSC_Stats 64-bit block must start 8-byte aligned");

struct DSC_Client_
{
	DesktopStreamingClientApp app;

	// Initialize 는 한 번뿐이다. 두 번째 호출과 Shutdown 뒤 호출을 가른다.
	bool initialized = false;
	bool shutDown = false;

	DSC_ConnectionCallback connectionCallback = nullptr;
	void* connectionUserData = nullptr;

	DSC_StreamInfoCallback streamInfoCallback = nullptr;
	void* streamInfoUserData = nullptr;

	DSC_PlaybackStateCallback playbackStateCallback = nullptr;
	void* playbackStateUserData = nullptr;

	DSC_StoppedCallback stoppedCallback = nullptr;
	void* stoppedUserData = nullptr;
};

namespace
{
	constexpr int32_t VERSION_MAJOR = 1;
	constexpr int32_t VERSION_MINOR = 0;
	constexpr int32_t VERSION_PATCH = 0;

	constexpr uint16_t DEFAULT_PORT = 27015;
	constexpr int32_t DEFAULT_WIDTH = 1920;
	constexpr int32_t DEFAULT_HEIGHT = 900;

	// 호스트 스레드에서, 메시지 루프를 통해 불린다. 이벤트 하나를 알맞은
	// C 콜백 하나로 보낸다.
	void OnHostEvent(DesktopStreamingClientApp::HostEvent event,
		int32_t a, int32_t b, int32_t c, void* userData)
	{
		DSC_Client_* client = static_cast<DSC_Client_*>(userData);
		if (!client)
			return;

		// 콜백 안에서 호스트가 예외를 던질 수는 없지만(C 함수 포인터다),
		// C# 쪽 예외가 네이티브 프레임을 건너 올라오면 여기서 멈춘다.
		try
		{
			switch (event)
			{
			case DesktopStreamingClientApp::HostEvent::ConnectionChanged:
				if (client->connectionCallback)
					client->connectionCallback(a, b, c, client->connectionUserData);
				break;

			case DesktopStreamingClientApp::HostEvent::StreamInfoChanged:
				if (client->streamInfoCallback)
					client->streamInfoCallback(static_cast<uint32_t>(a), static_cast<uint32_t>(b),
						static_cast<uint32_t>(c), client->streamInfoUserData);
				break;

			case DesktopStreamingClientApp::HostEvent::PlaybackStateChanged:
				if (client->playbackStateCallback)
					client->playbackStateCallback(a, client->playbackStateUserData);
				break;

			case DesktopStreamingClientApp::HostEvent::Stopped:
				if (client->stoppedCallback)
					client->stoppedCallback(a, client->stoppedUserData);
				break;

			default:
				break;
			}
		}
		catch (...)
		{
		}
	}

	// 네트워크 엔진은 char* 주소를 받는다. IPv4 점 표기만 쓰므로 ASCII 가
	// 아닌 글자가 섞였다면 잘못된 입력이다 — 조용히 바꾸지 않고 거절한다.
	bool ToAsciiAddress(const wchar_t* source, char* destination, size_t destinationSize)
	{
		if (!source || !destination || destinationSize == 0)
			return false;

		size_t index = 0;
		for (; source[index] != L'\0'; ++index)
		{
			if (index + 1 >= destinationSize)
				return false;

			const wchar_t ch = source[index];
			if (ch < 0x20 || ch > 0x7E)
				return false;

			destination[index] = static_cast<char>(ch);
		}

		destination[index] = '\0';
		return index > 0;
	}

	DSC_Result RequireInitialized(const DSC_Client_* client)
	{
		if (!client)
			return DSC_ERR_INVALID_HANDLE;

		if (!client->initialized || client->shutDown)
			return DSC_ERR_NOT_INITIALIZED;

		return DSC_OK;
	}
}

extern "C"
{
	DSC_API void DSC_CALL DSC_GetVersion(int32_t* outMajor, int32_t* outMinor, int32_t* outPatch)
	{
		if (outMajor) *outMajor = VERSION_MAJOR;
		if (outMinor) *outMinor = VERSION_MINOR;
		if (outPatch) *outPatch = VERSION_PATCH;
	}

	DSC_API DSC_Result DSC_CALL DSC_Create(DSC_Client** outClient)
	{
		if (!outClient)
			return DSC_ERR_INVALID_ARG;

		*outClient = nullptr;

		try
		{
			DSC_Client_* client = new (std::nothrow) DSC_Client_();
			if (!client)
				return DSC_ERR_FAILED;

			client->app.SetHostEventCallback(&OnHostEvent, client);
			*outClient = client;
			return DSC_OK;
		}
		catch (...)
		{
			return DSC_ERR_FAILED;
		}
	}

	DSC_API DSC_Result DSC_CALL DSC_Destroy(DSC_Client* client)
	{
		if (!client)
			return DSC_ERR_INVALID_HANDLE;

		try
		{
			// 소멸자가 Shutdown 을 부른다.
			delete client;
			return DSC_OK;
		}
		catch (...)
		{
			return DSC_ERR_FAILED;
		}
	}

	DSC_API DSC_Result DSC_CALL DSC_SetConnectionCallback(DSC_Client* client,
		DSC_ConnectionCallback callback, void* userData)
	{
		if (!client)
			return DSC_ERR_INVALID_HANDLE;

		client->connectionCallback = callback;
		client->connectionUserData = userData;
		return DSC_OK;
	}

	DSC_API DSC_Result DSC_CALL DSC_SetStreamInfoCallback(DSC_Client* client,
		DSC_StreamInfoCallback callback, void* userData)
	{
		if (!client)
			return DSC_ERR_INVALID_HANDLE;

		client->streamInfoCallback = callback;
		client->streamInfoUserData = userData;
		return DSC_OK;
	}

	DSC_API DSC_Result DSC_CALL DSC_SetPlaybackStateCallback(DSC_Client* client,
		DSC_PlaybackStateCallback callback, void* userData)
	{
		if (!client)
			return DSC_ERR_INVALID_HANDLE;

		client->playbackStateCallback = callback;
		client->playbackStateUserData = userData;
		return DSC_OK;
	}

	DSC_API DSC_Result DSC_CALL DSC_SetStoppedCallback(DSC_Client* client,
		DSC_StoppedCallback callback, void* userData)
	{
		if (!client)
			return DSC_ERR_INVALID_HANDLE;

		client->stoppedCallback = callback;
		client->stoppedUserData = userData;
		return DSC_OK;
	}

	DSC_API DSC_Result DSC_CALL DSC_Initialize(DSC_Client* client, const DSC_InitParams* params)
	{
		if (!client)
			return DSC_ERR_INVALID_HANDLE;

		if (!params || params->structSize < sizeof(DSC_InitParams))
			return DSC_ERR_INVALID_ARG;

		// Shutdown 뒤 다시 Initialize 하는 것은 지원하지 않는다. C++ 클래스의
		// 카운터와 재생 상태가 이전 세션 값을 들고 있어서, 새로 만든 것처럼
		// 보이지만 실제로는 섞인 상태가 된다. 핸들을 새로 만드는 편이 정직하다.
		if (client->initialized || client->shutDown)
			return DSC_ERR_INVALID_STATE;

		if (params->serverPort > 0xFFFF || params->width < 0 || params->height < 0)
			return DSC_ERR_INVALID_ARG;

		char address[64] = "127.0.0.1";
		if (params->serverAddress && !ToAsciiAddress(params->serverAddress, address, sizeof(address)))
			return DSC_ERR_INVALID_ARG;

		const uint16_t port = (params->serverPort != 0)
			? static_cast<uint16_t>(params->serverPort) : DEFAULT_PORT;

		const int32_t width = (params->width != 0) ? params->width : DEFAULT_WIDTH;
		const int32_t height = (params->height != 0) ? params->height : DEFAULT_HEIGHT;

		RECT rect = {};
		rect.left = params->x;
		rect.top = params->y;
		rect.right = params->x + width;
		rect.bottom = params->y + height;

		try
		{
			if (!client->app.Initialize(address, port, static_cast<HWND>(params->parentHwnd), rect))
				return DSC_ERR_INIT_FAILED;

			client->initialized = true;
			return DSC_OK;
		}
		catch (...)
		{
			return DSC_ERR_FAILED;
		}
	}

	DSC_API DSC_Result DSC_CALL DSC_Shutdown(DSC_Client* client)
	{
		if (!client)
			return DSC_ERR_INVALID_HANDLE;

		if (!client->initialized || client->shutDown)
			return DSC_ERR_NOT_INITIALIZED;

		try
		{
			client->app.Shutdown();
			client->shutDown = true;
			return DSC_OK;
		}
		catch (...)
		{
			client->shutDown = true;
			return DSC_ERR_FAILED;
		}
	}

	DSC_API DSC_Result DSC_CALL DSC_IsRunning(DSC_Client* client, int32_t* outRunning)
	{
		if (!client)
			return DSC_ERR_INVALID_HANDLE;

		if (!outRunning)
			return DSC_ERR_INVALID_ARG;

		// 초기화 전이나 정리 뒤에도 에러 대신 0 을 준다. 메시지 루프가
		// 이 값 하나로 끝낼지 정하므로 "돌고 있지 않다" 가 정답이다.
		*outRunning = (client->initialized && !client->shutDown && client->app.IsRunning()) ? 1 : 0;
		return DSC_OK;
	}

	DSC_API DSC_Result DSC_CALL DSC_RequestStop(DSC_Client* client)
	{
		if (!client)
			return DSC_ERR_INVALID_HANDLE;

		// 원자적 깃발 하나라 어느 스레드에서든 안전하다.
		client->app.RequestStop();
		return DSC_OK;
	}

	DSC_API DSC_Result DSC_CALL DSC_GetHwnd(DSC_Client* client, void** outHwnd)
	{
		const DSC_Result state = RequireInitialized(client);
		if (state != DSC_OK)
			return state;

		if (!outHwnd)
			return DSC_ERR_INVALID_ARG;

		*outHwnd = client->app.GetViewerWindow();
		return DSC_OK;
	}

	DSC_API DSC_Result DSC_CALL DSC_Play(DSC_Client* client)
	{
		const DSC_Result state = RequireInitialized(client);
		if (state != DSC_OK)
			return state;

		try { client->app.StartPlayback(); return DSC_OK; }
		catch (...) { return DSC_ERR_FAILED; }
	}

	DSC_API DSC_Result DSC_CALL DSC_Pause(DSC_Client* client)
	{
		const DSC_Result state = RequireInitialized(client);
		if (state != DSC_OK)
			return state;

		try { client->app.PausePlayback(); return DSC_OK; }
		catch (...) { return DSC_ERR_FAILED; }
	}

	DSC_API DSC_Result DSC_CALL DSC_Stop(DSC_Client* client)
	{
		const DSC_Result state = RequireInitialized(client);
		if (state != DSC_OK)
			return state;

		try { client->app.StopPlayback(); return DSC_OK; }
		catch (...) { return DSC_ERR_FAILED; }
	}

	DSC_API DSC_Result DSC_CALL DSC_GetPlaybackState(DSC_Client* client, int32_t* outState)
	{
		const DSC_Result state = RequireInitialized(client);
		if (state != DSC_OK)
			return state;

		if (!outState)
			return DSC_ERR_INVALID_ARG;

		*outState = static_cast<int32_t>(client->app.GetPlaybackState());
		return DSC_OK;
	}

	DSC_API DSC_Result DSC_CALL DSC_GetStats(DSC_Client* client, DSC_Stats* inOutStats)
	{
		const DSC_Result state = RequireInitialized(client);
		if (state != DSC_OK)
			return state;

		if (!inOutStats || inOutStats->structSize < sizeof(uint32_t))
			return DSC_ERR_INVALID_ARG;

		try
		{
			StreamingStatsInfo stats = {};
			StreamingQualityInfo quality = {};
			client->app.GetStatsSnapshot(stats, quality);

			DesktopStreamingClientStats network = {};
			client->app.GetNetworkStats(network);

			NvDecStats decoder = {};
			client->app.GetDecoderStats(decoder);

			DSC_Stats full = {};
			full.structSize = sizeof(DSC_Stats);
			full.connected = network.connected ? 1 : 0;
			full.playbackState = static_cast<int32_t>(client->app.GetPlaybackState());

			full.streamWidth = quality.width;
			full.streamHeight = quality.height;
			full.streamFps = quality.fps;

			full.receivedMbps = quality.bitrateMbps;
			full.latencyMs = stats.latencyMs;
			full.presentedFps = stats.presentedFps;
			full.jitterBufferMs = stats.jitterBufferMs;
			full.averagePaceWaitMs = stats.avgPaceWaitMs;

			full.chunksAccepted = network.chunksAccepted;
			full.chunksRejected = network.chunksRejected;
			full.bytesReceived = network.chunkBytesReceived;
			full.framesCompleted = network.framesCompleted;
			full.framesDiscarded = network.framesDiscarded;
			full.framesDecoded = decoder.decodedFrames;
			full.framesPresented = client->app.GetPresentedFrameCount();
			full.decodeQueueDrops = decoder.droppedInputQueue;
			full.decodePoolExhausted = decoder.droppedPoolExhausted;
			full.decodeNotConsumed = decoder.droppedNotConsumed;
			full.decodeDisplayFailed = decoder.droppedDisplayFailed;
			full.paceResyncCount = stats.resyncCount;

			// 호출자가 아는 크기까지만 쓴다. 구버전 호출자의 작은 구조체를
			// 넘쳐 쓰지 않게 하려는 것이 structSize 의 존재 이유다.
			const uint32_t writable = (inOutStats->structSize < sizeof(DSC_Stats))
				? inOutStats->structSize : static_cast<uint32_t>(sizeof(DSC_Stats));

			::memcpy(inOutStats, &full, writable);
			inOutStats->structSize = writable;
			return DSC_OK;
		}
		catch (...)
		{
			return DSC_ERR_FAILED;
		}
	}

	DSC_API DSC_Result DSC_CALL DSC_SetJitterBufferMs(DSC_Client* client, uint32_t milliseconds)
	{
		const DSC_Result state = RequireInitialized(client);
		if (state != DSC_OK)
			return state;

		// 대기 상한(100ms)보다 깊으면 페이싱이 매번 기준을 버린다. 그 위는
		// 의미가 없으니 받지 않는다.
		if (milliseconds > 100)
			return DSC_ERR_INVALID_ARG;

		client->app.SetJitterBufferMs(milliseconds);
		return DSC_OK;
	}

	DSC_API DSC_Result DSC_CALL DSC_SetControlBarVisible(DSC_Client* client, int32_t visible)
	{
		const DSC_Result state = RequireInitialized(client);
		if (state != DSC_OK)
			return state;

		client->app.SetControlBarVisible(visible != 0);
		return DSC_OK;
	}

	DSC_API DSC_Result DSC_CALL DSC_SetControlBarAutoHide(DSC_Client* client, int32_t enabled)
	{
		const DSC_Result state = RequireInitialized(client);
		if (state != DSC_OK)
			return state;

		client->app.SetControlBarAutoHide(enabled != 0);
		return DSC_OK;
	}

	DSC_API DSC_Result DSC_CALL DSC_SetStatsOverlayVisible(DSC_Client* client, int32_t visible)
	{
		const DSC_Result state = RequireInitialized(client);
		if (state != DSC_OK)
			return state;

		client->app.SetStatsOverlayVisible(visible != 0);
		return DSC_OK;
	}

	DSC_API const char* DSC_CALL DSC_GetDisconnectReasonName(int32_t reason)
	{
		const char* name = ToString(static_cast<DisconnectReason>(reason));
		return name ? name : "unknown";
	}
}
