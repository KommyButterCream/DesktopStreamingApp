#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <objbase.h>

#include <string>
#include <iostream>
#include <conio.h>

#include "../../../Module/D3D11Engine/Core/D3D11RenderEngine.h"
#include "../../../Module/D3D11ImageView/D3D11ImageView/D3D11ImageView.h"
#include "../../../Module/NvCodec/NvDecode/D3D11NvDecoder.h"

class DesktopStreamingClientApp
{
public:
	DesktopStreamingClientApp() = default;
	~DesktopStreamingClientApp()
	{
		Shutdown();
	}

	bool Initialize()
	{
		// Rendering Engine
		RenderEngineConfig renderEngineConfig = {};
		renderEngineConfig.initD2D = false;
		renderEngineConfig.initD3D = true;
#if defined(_DEBUG)
		renderEngineConfig.initDebugLayer = true;
#endif
		renderEngineConfig.initFontManager = false;

		m_D3D11Engine = new D3D11RenderEngine();
		if (!m_D3D11Engine)
		{
			Shutdown();
			return false;
		}

		if (!m_D3D11Engine->Initialize(renderEngineConfig))
		{
			Shutdown();
			return false;
		}

		const uint32_t outputWidth = 2560;
		const uint32_t outputHeight = 1440;
		if (outputWidth == 0 || outputHeight == 0)
		{
			Shutdown();
			return false;
		}

		m_nvDecoder = new D3D11NvDecoder();
		if (!m_nvDecoder)
		{
			Shutdown();
			return false;
		}

		if (!m_nvDecoder->Initialize(m_D3D11Engine->GetD3DDevice(), true))
		{
			Shutdown();
			return false;
		}

		DWORD windowStyle = WS_VISIBLE | WS_OVERLAPPEDWINDOW;
		m_imageView = new D3D11ImageView();
		if (!m_imageView)
		{
			Shutdown();
			return false;
		}

		if (!m_imageView->Initialize(GetDesktopWindow(), RECT(0, 0, 1920, 900), windowStyle))
		{
			Shutdown();
			return false;
		}

		m_running = true;
		return true;
	}

	void Run()
	{
		std::string cmd;
		MSG message = {};

		while (m_running)
		{
			while (::PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
			{
				if (message.message == WM_QUIT)
				{
					m_running = false;
					break;
				}

				::TranslateMessage(&message);
				::DispatchMessage(&message);
			}

			if (!m_running)
				break;

			if (_kbhit())
			{
				std::getline(std::cin, cmd);
				if (cmd == "quit")
				{
					m_running = false;
				}
			}

			::Sleep(10);
		}
	}

	void RequestStop()
	{
		m_running = false;
	}

	void Shutdown()
	{
		m_running = false;

		if (m_imageView)
		{
			delete m_imageView;
			m_imageView = nullptr;
		}

		if (m_nvDecoder)
		{
			m_nvDecoder->Destroy();
			delete m_nvDecoder;
			m_nvDecoder = nullptr;
		}

		if (m_D3D11Engine)
		{
			delete m_D3D11Engine;
			m_D3D11Engine = nullptr;
		}
	}

private:
	static void FrameCallbackThunk(void* userData)
	{
		//CaptureCallbackContext* context = static_cast<CaptureCallbackContext*>(userData);
		//if (!context)
		//	return;

		//DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(context->ownerData);
		//if (self)
		//{
		//	self->OnFrameCallback();
		//}
	}

	void OnFrameCallback()
	{
		//NvEncPacket encodeResultPacket = {};
		//if (!encodeResultPacket.data || encodeResultPacket.size == 0)
		//{
		//	return;
		//}

		//if (!m_nvDecoder->Parse(encodeResultPacket.data, encodeResultPacket.size, true, false, false))
		//{
		//	__debugbreak();

		//	return;
		//}

		//if (D3D11NvDecoder::Frame* frame = m_nvDecoder->GetFrame())
		//{
		//	if (m_imageView && frame->sharedHandle)
		//	{
		//		m_imageView->UpdateSharedTexture(frame->sharedHandle);
		//	}
		//	else if (m_imageView && frame->texture)
		//	{
		//		m_imageView->UpdateTexture(frame->texture);
		//	}
		//}
	}

private:
	bool m_running = false;
	D3D11RenderEngine* m_D3D11Engine = nullptr;
	D3D11NvDecoder* m_nvDecoder = nullptr;
	D3D11ImageView* m_imageView = nullptr;
};
