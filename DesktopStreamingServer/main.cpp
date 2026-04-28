#include <stdio.h>

#include "../../../Module/Core/DirectX/DxDebugUtils.h"

#include "DesktopDuplicateApp.h"

static DesktopDuplicateApp* g_appInstance = nullptr;

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

	DesktopDuplicateApp app;
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