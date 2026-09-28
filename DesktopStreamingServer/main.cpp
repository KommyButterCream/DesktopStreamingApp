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

#include "../Service/StreamingServerHost/DesktopStreamingServerApp.h"

static DesktopStreamingServerApp* g_appInstance = nullptr;

BOOL WINAPI ConsoleHandler(DWORD ctrlType)
{
	if (ctrlType == CTRL_CLOSE_EVENT ||
		ctrlType == CTRL_C_EVENT)
	{
		if (g_appInstance)
		{
			g_appInstance->RequestStop();
		}
		return TRUE;
	}
	return FALSE;
}

// 메시지 루프와 콘솔 명령. 둘 다 이 exe 의 몫이다.
//
// 해상도 변경 재구성 / 비트레이트 적응 / 통계 같은 주기 작업은 DLL 이 이 스레드의 타이머로
// 돌린다. 여기서 메시지를 펌프하는 동안 그 WM_TIMER 가 배달된다 — 이
// 루프가 멈추면 주기 작업도 멈춘다.
//
// 예전에는 이 루프가 DLL 쪽 클래스의 Run() 안에 주기 작업과 한 덩어리로
// 있었다. WPF 에서는 메시지 루프를 Dispatcher 가 갖고 콘솔이 없으므로
// 그 둘을 호스트 쪽으로 돌려놓았다.
static void RunMessageLoop(DesktopStreamingServerApp& app)
{
	std::string cmd;
	MSG message = {};

	while (app.IsRunning())
	{
		while (::PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
		{
			if (message.message == WM_QUIT)
			{
				app.RequestStop();
				break;
			}

			::TranslateMessage(&message);
			::DispatchMessage(&message);
		}

		if (!app.IsRunning())
			break;

		if (_kbhit())
		{
			std::getline(std::cin, cmd);
			if (cmd == "quit")
			{
				app.RequestStop();
			}
			else if (cmd == "stats")
			{
				app.PrintStats();
			}
		}

		::Sleep(10);
	}
}

int main()
{
	HRESULT hr = ::CoInitializeEx(0, COINIT_MULTITHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
	{
		return hr;
	}

	DesktopStreamingServerApp app;
	g_appInstance = &app;

	::SetConsoleCtrlHandler(ConsoleHandler, TRUE);

	if (!app.Initialize())
	{
		g_appInstance = nullptr;
		::CoUninitialize();
		return -1;
	}

	RunMessageLoop(app);
	app.Shutdown();

	printf_s("Shutting down...\n");

	g_appInstance = nullptr;


#if defined(_DEBUG)
	Core::DirectX::DxReportLiveObjects();
#endif

	::CoUninitialize();

	return 0;
}