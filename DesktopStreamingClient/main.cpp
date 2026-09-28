#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <objbase.h>

#include <stdio.h>
#include <string>
#include <iostream>
#include <conio.h>

#include "../../../Module/Core/DirectX/DxDebugUtils.h"

// 이 exe 는 C ABI 만 쓴다. C++ 클래스를 쓸 수도 있지만 일부러 그렇게
// 하지 않는다 — WPF 가 P/Invoke 로 부를 경로와 똑같은 경로를 C# 없이
// 네이티브 디버거로 먼저 검증하려는 것이다. 여기서 되면 WPF 쪽 문제는
// 마샬링 선언만 남는다.
#include "../Service/StreamingClientHost/StreamingClientHostC.h"

// 콘솔 핸들러 스레드가 읽는다. DSC_RequestStop 은 어느 스레드에서 불러도 된다.
static DSC_Client* g_client = nullptr;

BOOL WINAPI ConsoleHandler(DWORD ctrlType)
{
	if (ctrlType == CTRL_CLOSE_EVENT ||
		ctrlType == CTRL_C_EVENT)
	{
		if (g_client)
		{
			DSC_RequestStop(g_client);
		}
		return TRUE;
	}
	return FALSE;
}

// --- 호스트 이벤트 ---
//
// 전부 이 스레드(메인)에서, 아래 메시지 루프를 통해 불린다. 네트워크나
// 디코더 스레드에서 일어난 일도 DLL 이 이리로 옮겨 온다.
//
// DLL 도 같은 사건을 자기 로그로 찍는다. 여기서 [abi] 로 한 번 더 찍는
// 것은 통지가 C ABI 를 넘어 호스트 스레드까지 제대로 오는지를 로그만
// 보고 확인하기 위해서다.
static const char* PlaybackName(int32_t state)
{
	switch (state)
	{
	case DSC_PLAYBACK_STOPPED: return "stopped";
	case DSC_PLAYBACK_PLAYING: return "playing";
	case DSC_PLAYBACK_PAUSED:  return "paused";
	default:                   return "?";
	}
}

static void DSC_CALL OnConnection(int32_t event, int32_t reason, int32_t errorCode, void*)
{
	switch (event)
	{
	case DSC_CONNECTION_ESTABLISHED:
		printf_s("[abi] connection : established\n");
		break;
	case DSC_CONNECTION_CONNECT_FAILED:
		printf_s("[abi] connection : connect failed (WSA %d)\n", errorCode);
		break;
	case DSC_CONNECTION_DISCONNECTED:
		printf_s("[abi] connection : disconnected (%s)\n", DSC_GetDisconnectReasonName(reason));
		break;
	default:
		break;
	}
}

static void DSC_CALL OnStreamInfo(uint32_t width, uint32_t height, uint32_t fps, void*)
{
	printf_s("[abi] stream     : %ux%u @%u\n", width, height, fps);
}

static void DSC_CALL OnPlaybackState(int32_t state, void*)
{
	printf_s("[abi] playback   : %s\n", PlaybackName(state));
}

static void DSC_CALL OnStopped(int32_t reason, void*)
{
	printf_s("[abi] stopped    : %s\n",
		reason == DSC_STOP_VIEWER_CLOSED ? "viewer closed" :
		reason == DSC_STOP_DECODER_FAULT ? "decoder fault" : "?");
}

// DSC_GetStats 로 한 줄 요약. 콘솔 "stats" 명령과 세션 끝에서 쓴다.
static void PrintAbiStats(DSC_Client* client, const char* label)
{
	DSC_Stats stats = {};
	stats.structSize = sizeof(stats);

	if (DSC_GetStats(client, &stats) != DSC_OK)
	{
		printf_s("[abi] %s : unavailable\n", label);
		return;
	}

	printf_s("[abi] %s : %s %s %ux%u@%u %.1f Mbps latency %.0f ms, %.0f fps | "
		"frames recv %llu decoded %llu presented %llu | "
		"loss rejected %llu discarded %llu qdrop %llu pool %llu unconsumed %llu | resync %llu\n",
		label,
		stats.connected ? "connected" : "disconnected",
		PlaybackName(stats.playbackState),
		stats.streamWidth, stats.streamHeight, stats.streamFps,
		stats.receivedMbps, stats.latencyMs, stats.presentedFps,
		stats.framesCompleted, stats.framesDecoded, stats.framesPresented,
		stats.chunksRejected, stats.framesDiscarded, stats.decodeQueueDrops,
		stats.decodePoolExhausted, stats.decodeNotConsumed,
		stats.paceResyncCount);

	::fflush(stdout);
}

static bool IsRunning(DSC_Client* client)
{
	int32_t running = 0;
	return DSC_IsRunning(client, &running) == DSC_OK && running != 0;
}

// 메시지 루프와 콘솔 명령. 둘 다 이 exe 의 몫이다.
//
// 재접속 / 피드백 / 컨트롤 바 갱신 / 통계 같은 주기 작업은 DLL 이 이
// 스레드의 타이머로 돌린다. 호스트 이벤트도 같은 창으로 온다. 여기서
// 메시지를 펌프하는 동안 둘 다 배달된다 — 이 루프가 멈추면 둘 다 멈춘다.
static void RunMessageLoop(DSC_Client* client)
{
	std::string cmd;
	MSG message = {};

	while (IsRunning(client))
	{
		while (::PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
		{
			if (message.message == WM_QUIT)
			{
				DSC_RequestStop(client);
				break;
			}

			::TranslateMessage(&message);
			::DispatchMessage(&message);
		}

		if (!IsRunning(client))
			break;

		if (_kbhit())
		{
			std::getline(std::cin, cmd);
			if (cmd == "quit")
			{
				DSC_RequestStop(client);
			}
			else if (cmd == "stats")
			{
				PrintAbiStats(client, "stats  ");
			}
		}

		::Sleep(10);
	}

	// 루프를 끝낸 이유가 창 닫기였다면 그 통지(DSC_StoppedCallback)가 아직
	// 큐에 있을 수 있다. 정리하기 전에 한 번 비워서 호스트가 이유를 보게 한다.
	while (::PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
	{
		::TranslateMessage(&message);
		::DispatchMessage(&message);
	}
}

int main()
{
	HRESULT hr = ::CoInitializeEx(0, COINIT_MULTITHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
	{
		return hr;
	}

	int32_t major = 0;
	int32_t minor = 0;
	int32_t patch = 0;
	DSC_GetVersion(&major, &minor, &patch);
	printf_s("[abi] StreamingClientHost %d.%d.%d\n", major, minor, patch);

	DSC_Client* client = nullptr;
	if (DSC_Create(&client) != DSC_OK)
	{
		::CoUninitialize();
		return -1;
	}

	// 콜백은 Initialize 전에 건다. 그래야 첫 접속 통지까지 받는다.
	DSC_SetConnectionCallback(client, OnConnection, nullptr);
	DSC_SetStreamInfoCallback(client, OnStreamInfo, nullptr);
	DSC_SetPlaybackStateCallback(client, OnPlaybackState, nullptr);
	DSC_SetStoppedCallback(client, OnStopped, nullptr);

	g_client = client;
	::SetConsoleCtrlHandler(ConsoleHandler, TRUE);

	// 부모 창 없이 독립 창으로 띄운다. 크기와 위치는 기본값(0,0 에서
	// 1920 x 900)을 쓴다 — 예전 exe 와 같다.
	DSC_InitParams params = {};
	params.structSize = sizeof(params);
	params.serverAddress = L"127.0.0.1";
	params.serverPort = 27015;

	const DSC_Result initResult = DSC_Initialize(client, &params);
	if (initResult != DSC_OK)
	{
		printf_s("[abi] DSC_Initialize failed (%d)\n", static_cast<int>(initResult));
		g_client = nullptr;
		DSC_Destroy(client);
		::CoUninitialize();
		return -1;
	}

	RunMessageLoop(client);

	PrintAbiStats(client, "session");

	DSC_Shutdown(client);

	printf_s("Shutting down...\n");

	g_client = nullptr;
	DSC_Destroy(client);

	// 호스트가 D3D 객체를 전부 놓은 뒤에 불러야 의미가 있다. 이 보고는
	// 프로세스 전체가 대상이라 프로세스 주인인 exe 가 맨 끝에서 한다.
#if defined(_DEBUG)
	Core::DirectX::DxReportLiveObjects();
#endif

	::CoUninitialize();

	return 0;
}
