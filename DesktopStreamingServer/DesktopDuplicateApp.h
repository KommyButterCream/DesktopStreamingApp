#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <iostream>
#include <conio.h>

#include "../../../Module/D3D11DuplicateEngine/D3D11DuplicateEngine/D3D11DuplicateEngine.h"
#include "../../../Module/D3D11DuplicateEngine/D3D11DuplicateEngine/CommonTypes.h"
#include "../../../Module/D3D11ImageView/D3D11ImageView/D3D11ImageView.h"

class DesktopDuplicateApp
{
public:
	DesktopDuplicateApp() = default;
	~DesktopDuplicateApp()
	{
		Shutdown();
	}

	bool Initialize()
	{
		DWORD windowStyle = WS_VISIBLE | WS_OVERLAPPEDWINDOW;

		m_imageView = new D3D11ImageView();
		if (!m_imageView)
			return false;

		if (!m_imageView->Initialize(GetDesktopWindow(), RECT(0, 0, 1920, 900), windowStyle))
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
		m_callbackContext.D3D11Device = m_imageView->GetDevice();

		if (!m_duplicateEngine->Initialize(0))
		{
			Shutdown();
			return false;
		}

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

		if (m_imageView)
		{
			delete m_imageView;
			m_imageView = nullptr;
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

		DesktopDuplicateApp* self = static_cast<DesktopDuplicateApp*>(context->ownerData);
		if (self)
		{
			self->OnFrameCallback();
		}
	}

	void OnFrameCallback()
	{
		if (!m_callbackContext.sharedData || !m_callbackContext.imageView)
			return;

		HANDLE currentHandle = nullptr;
		bool hasNewFrame = false;

		if (m_callbackContext.captureFrame)
		{
			ID3D11Texture2D* bgraTexture = m_callbackContext.captureFrame->frame;
			(void)bgraTexture;

			// NVENC Encode (h264)
		}

		::AcquireSRWLockExclusive(&m_callbackContext.sharedData->lock);
		currentHandle = m_callbackContext.sharedData->sharedHandle;
		hasNewFrame = m_callbackContext.sharedData->newFrame;
		m_callbackContext.sharedData->newFrame = false;
		::ReleaseSRWLockExclusive(&m_callbackContext.sharedData->lock);

		if (hasNewFrame && currentHandle)
		{
			m_callbackContext.imageView->UpdateSharedTexture(currentHandle);
		}
	}

private:
	bool m_running = false;
	SharedCaptureData m_sharedData = {};
	CaptureCallbackContext m_callbackContext = {};
	D3D11DuplicateEngine* m_duplicateEngine = nullptr;
	D3D11ImageView* m_imageView = nullptr;
};