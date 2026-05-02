#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <objbase.h>

#include <stdio.h>

#include "../../../Module/Core/DirectX/DxDebugUtils.h"

#include "DesktopStreamingClientApp.h"

static DesktopStreamingClientApp* g_appInstance = nullptr;

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

int main()
{
	HRESULT hr = ::CoInitializeEx(0, COINIT_MULTITHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
	{
		return hr;
	}

	DesktopStreamingClientApp app;
	g_appInstance = &app;

	::SetConsoleCtrlHandler(ConsoleHandler, TRUE);

	if (!app.Initialize())
	{
		g_appInstance = nullptr;
		::CoUninitialize();
		return -1;
	}

	app.Run();
	app.Shutdown();

	printf_s("Shutting down...\n");

	g_appInstance = nullptr;


#if defined(_DEBUG)
	Core::DirectX::DxReportLiveObjects();
#endif

	::CoUninitialize();

	return 0;
}
