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
#include "../../../Module/D3D11DuplicateEngine/D3D11DuplicateEngine/D3D11DuplicateEngine.h"
#include "../../../Module/D3D11DuplicateEngine/D3D11DuplicateEngine/CommonTypes.h"
#include "../../../Module/D3D11ImageView/D3D11ImageView/D3D11ImageView.h"
#include "../../../Module/NvCodec/NvEncode/D3D11NvEncoder.h"
#include "../../../Module/NvCodec/NvDecode/D3D11NvDecoder.h"

class DesktopStreamingServerApp
{
public:
	DesktopStreamingServerApp() = default;
	~DesktopStreamingServerApp()
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

		m_duplicateEngine = new D3D11DuplicateEngine();
		if (!m_duplicateEngine)
		{
			Shutdown();
			return false;
		}

		m_callbackContext.sharedData = &m_sharedData;
		m_callbackContext.ownerData = this;
		m_callbackContext.imageView = m_imageView;
		m_callbackContext.D3D11Device = m_D3D11Engine->GetD3DDevice();

		if (!m_duplicateEngine->Initialize(m_D3D11Engine, 0))
		{
			Shutdown();
			return false;
		}

		const uint32_t outputWidth = m_duplicateEngine->GetOutputWidth();
		const uint32_t outputHeight = m_duplicateEngine->GetOutputHeight();
		if (outputWidth == 0 || outputHeight == 0)
		{
			Shutdown();
			return false;
		}

		m_nvEncoder = new D3D11NvEncoder();
		if (!m_nvEncoder)
		{
			Shutdown();
			return false;
		}

		if (!m_nvEncoder->Initialize(m_D3D11Engine->GetD3DDevice(), outputWidth, outputHeight, 4))
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

		m_callbackContext.imageView = m_imageView;

		m_duplicateEngine->SetTargetFps(60);
		m_duplicateEngine->SetFrameCaptureCallback(FrameCallbackThunk, &m_callbackContext);

		if (!m_duplicateEngine->StartThread())
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

		if (m_duplicateEngine)
		{
			m_duplicateEngine->Shutdown();
			delete m_duplicateEngine;
			m_duplicateEngine = nullptr;
		}

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

		if (m_nvEncoder)
		{
			m_nvEncoder->Destroy();
			delete m_nvEncoder;
			m_nvEncoder = nullptr;
		}

		if (m_callbackContext.opendTexture)
		{
			m_callbackContext.opendTexture->Release();
			m_callbackContext.opendTexture = nullptr;
		}

		if (m_callbackContext.stagingTex)
		{
			m_callbackContext.stagingTex->Release();
			m_callbackContext.stagingTex = nullptr;
		}

		if (m_D3D11Engine)
		{
			delete m_D3D11Engine;
			m_D3D11Engine = nullptr;
		}

		ZeroMemory(&m_sharedData, sizeof(m_sharedData));
		ZeroMemory(&m_callbackContext, sizeof(m_callbackContext));
	}

private:
	static void FrameCallbackThunk(void* userData)
	{
		CaptureCallbackContext* context = static_cast<CaptureCallbackContext*>(userData);
		if (!context)
			return;

		DesktopStreamingServerApp* self = static_cast<DesktopStreamingServerApp*>(context->ownerData);
		if (self)
		{
			self->OnFrameCallback();
		}
	}

	void OnFrameCallback()
	{
		if (!m_callbackContext.sharedData)
			return;

		CapturedFrameHandle frameHandle = m_duplicateEngine->GetLatestFrameHandle();

		printf_s("Slot ID : %d, Frame ID : %llu\n", frameHandle.slotId, frameHandle.frameId);

		if (!frameHandle.texture)
		{
			return;
		}

		NvEncPacket encodeResultPacket = {};
		if (!m_nvEncoder->PrepareFrameForEncode(frameHandle.texture))
		{
			__debugbreak();

			m_duplicateEngine->ReleaseLatestFrameHandle(frameHandle);
			
			return;
		}

		if (!m_nvEncoder->DoEncode(encodeResultPacket))
		{
			__debugbreak();
			
			m_duplicateEngine->ReleaseLatestFrameHandle(frameHandle);
			
			return;
		}

		if (!encodeResultPacket.data || encodeResultPacket.size == 0)
		{
			m_duplicateEngine->ReleaseLatestFrameHandle(frameHandle);
			return;
		}

		if (!m_nvDecoder->Parse(encodeResultPacket.data, encodeResultPacket.size, true, false, false))
		{
			__debugbreak();

			m_duplicateEngine->ReleaseLatestFrameHandle(frameHandle);

			return;
		}

		if (D3D11NvDecoder::Frame* frame = m_nvDecoder->GetFrame())
		{
			if (m_imageView && frame->sharedHandle)
			{
				m_imageView->UpdateSharedTexture(frame->sharedHandle);
			}
			else if (m_imageView && frame->texture)
			{
				m_imageView->UpdateTexture(frame->texture);
			}
		}
		
		m_duplicateEngine->ReleaseLatestFrameHandle(frameHandle);
	}

private:
	bool m_running = false;
	SharedCaptureData m_sharedData = {};
	CaptureCallbackContext m_callbackContext = {};
	D3D11RenderEngine* m_D3D11Engine = nullptr;
	D3D11DuplicateEngine* m_duplicateEngine = nullptr;
	D3D11NvEncoder* m_nvEncoder = nullptr;
	D3D11NvDecoder* m_nvDecoder = nullptr;
	D3D11ImageView* m_imageView = nullptr;
};
