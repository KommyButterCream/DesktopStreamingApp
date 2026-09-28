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

// 이 exe 는 C ABI 만 쓴다. WPF 서버 UI 가 P/Invoke 로 부를 경로와 같은
// 경로를 C# 없이 먼저 검증하려는 것이다.
#include "../Service/StreamingServerHost/StreamingServerHostC.h"

// 콘솔 핸들러 스레드가 읽는다. DSS_RequestStop 은 어느 스레드에서 불러도 된다.
static DSS_Server* g_server = nullptr;

BOOL WINAPI ConsoleHandler(DWORD ctrlType)
{
	if (ctrlType == CTRL_CLOSE_EVENT ||
		ctrlType == CTRL_C_EVENT)
	{
		if (g_server)
		{
			DSS_RequestStop(g_server);
		}
		return TRUE;
	}
	return FALSE;
}

static const char* CaptureStateName(int32_t state)
{
	switch (state)
	{
	case DSS_CAPTURE_IDLE:         return "idle";
	case DSS_CAPTURE_RUNNING:      return "run";
	case DSS_CAPTURE_RECONNECTING: return "recon";
	case DSS_CAPTURE_FAULTED:      return "fault";
	default:                       return "?";
	}
}

// DSS_GetStats 로 한 줄 요약. 콘솔 "stats" 명령, 뷰어 수 변화, 세션 끝에서 쓴다.
static void PrintAbiStats(DSS_Server* server, const char* label)
{
	DSS_Stats stats = {};
	stats.structSize = sizeof(stats);

	if (DSS_GetStats(server, &stats) != DSS_OK)
	{
		printf_s("[abi] %s : unavailable\n", label);
		return;
	}

	printf_s("[abi] %s : capture %s %ux%u@%u | viewers %u | %.1f / %.1f Mbps | "
		"captured %llu encoded %llu offered %llu delivered %llu | "
		"chunkfail %llu incomplete %llu vwDecDrop %llu | idr %llu\n",
		label,
		CaptureStateName(stats.captureState),
		stats.streamWidth, stats.streamHeight, stats.streamFps,
		stats.viewerCount,
		stats.currentBitrateBps / 1'000'000.0, stats.targetBitrateBps / 1'000'000.0,
		stats.capturedFrames, stats.encodeCompleted, stats.framesOffered, stats.framesDelivered,
		stats.chunksFailed, stats.viewerFrameIncomplete, stats.viewerDecodeDropped,
		stats.keyframesForced);

	::fflush(stdout);
}

// --- 호스트 이벤트 ---
//
// 전부 이 스레드(메인)에서, 아래 메시지 루프를 통해 불린다. 캡처나 인코더
// 스레드에서 일어난 일도 DLL 이 이리로 옮겨 온다.
static void DSS_CALL OnStopped(int32_t reason, void*)
{
	printf_s("[abi] stopped    : %s\n",
		reason == DSS_STOP_CAPTURE_FAULTED ? "capture faulted" :
		reason == DSS_STOP_ENCODER_REBUILD_LIMIT ? "encoder rebuild limit" :
		reason == DSS_STOP_ENCODER_REBUILD_FAILED ? "encoder rebuild failed" : "?");
}

static void DSS_CALL OnViewerCount(uint32_t viewerCount, void* userData)
{
	printf_s("[abi] viewers    : %u\n", viewerCount);

	// 콜백 안에서 다른 DSS_* 를 불러도 된다(Destroy 만 안 된다). 헤더가
	// 그렇게 약속했으니 여기서 실제로 해 본다.
	PrintAbiStats(static_cast<DSS_Server*>(userData), "viewers");
}

static void DSS_CALL OnStreamInfo(uint32_t width, uint32_t height, uint32_t fps, void*)
{
	printf_s("[abi] stream     : %ux%u @%u (rebuilt)\n", width, height, fps);
}

static void DSS_CALL OnBitrate(uint32_t previousBps, uint32_t currentBps, void*)
{
	printf_s("[abi] bitrate    : %.1f -> %.1f Mbps\n",
		previousBps / 1'000'000.0, currentBps / 1'000'000.0);
}

static void DSS_CALL OnCaptureEvent(int32_t event, int32_t hresult, void*)
{
	printf_s("[abi] capture    : event %d (hr=0x%08X)\n", event, static_cast<unsigned int>(hresult));
}

static bool IsRunning(DSS_Server* server)
{
	int32_t running = 0;
	return DSS_IsRunning(server, &running) == DSS_OK && running != 0;
}

// 메시지 루프와 콘솔 명령. 둘 다 이 exe 의 몫이다.
//
// 해상도 변경 재구성 / 비트레이트 적응 / 통계 같은 주기 작업은 DLL 이 이
// 스레드의 타이머로 돌린다. 호스트 이벤트도 같은 창으로 온다. 여기서
// 메시지를 펌프하는 동안 둘 다 배달된다.
static void RunMessageLoop(DSS_Server* server)
{
	std::string cmd;
	MSG message = {};

	while (IsRunning(server))
	{
		while (::PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
		{
			if (message.message == WM_QUIT)
			{
				DSS_RequestStop(server);
				break;
			}

			::TranslateMessage(&message);
			::DispatchMessage(&message);
		}

		if (!IsRunning(server))
			break;

		if (_kbhit())
		{
			std::getline(std::cin, cmd);
			if (cmd == "quit")
			{
				DSS_RequestStop(server);
			}
			else if (cmd == "stats")
			{
				PrintAbiStats(server, "stats  ");
			}
		}

		::Sleep(10);
	}

	// 스스로 멈췄다면 그 이유(DSS_StoppedCallback)가 아직 큐에 있을 수 있다.
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
	DSS_GetVersion(&major, &minor, &patch);
	printf_s("[abi] StreamingServerHost %d.%d.%d\n", major, minor, patch);

	DSS_Server* server = nullptr;
	if (DSS_Create(&server) != DSS_OK)
	{
		::CoUninitialize();
		return -1;
	}

	DSS_SetStoppedCallback(server, OnStopped, nullptr);
	DSS_SetViewerCountCallback(server, OnViewerCount, server);
	DSS_SetStreamInfoCallback(server, OnStreamInfo, nullptr);
	DSS_SetBitrateCallback(server, OnBitrate, nullptr);
	DSS_SetCaptureEventCallback(server, OnCaptureEvent, nullptr);

	g_server = server;
	::SetConsoleCtrlHandler(ConsoleHandler, TRUE);

	// 예전 exe 와 같은 값이다.
	DSS_InitParams params = {};
	params.structSize = sizeof(params);
	params.port = 27015;
	params.maxViewers = 64;

	const DSS_Result initResult = DSS_Initialize(server, &params);
	if (initResult != DSS_OK)
	{
		printf_s("[abi] DSS_Initialize failed (%d)\n", static_cast<int>(initResult));
		g_server = nullptr;
		DSS_Destroy(server);
		::CoUninitialize();
		return -1;
	}

	RunMessageLoop(server);

	PrintAbiStats(server, "session");

	DSS_Shutdown(server);

	printf_s("Shutting down...\n");

	g_server = nullptr;
	DSS_Destroy(server);

	// 호스트가 D3D 객체를 전부 놓은 뒤에 불러야 의미가 있다.
#if defined(_DEBUG)
	Core::DirectX::DxReportLiveObjects();
#endif

	::CoUninitialize();

	return 0;
}
