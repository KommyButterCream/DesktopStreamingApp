#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11_4.h>
#include <objbase.h>

#include <string>
#include <iostream>
#include <conio.h>

#include "../../../Module/D3D11Engine/Core/D3D11RenderEngine.h"
#include "../../../Module/D3D11DuplicateEngine/D3D11DuplicateEngine/D3D11DuplicateEngine.h"
#include "../../../Module/D3D11DuplicateEngine/D3D11DuplicateEngine/CommonTypes.h"
#include "../../../Module/NvCodec/NvEncode/D3D11NvEncoder.h"
#include "../../../Module/NvCodec/NvEncode/EncodeFrameQueue.h"
#include "../../../Module/NvCodec/NvEncode/EncodeThread.h"
#include "../Service/StreamingServer/StreamingServer.h"

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

		EnableD3D11MultithreadProtection();

		m_duplicateEngine = new D3D11DuplicateEngine();
		if (!m_duplicateEngine)
		{
			Shutdown();
			return false;
		}

		m_callbackContext.sharedData = &m_sharedData;
		m_callbackContext.ownerData = this;
		m_callbackContext.imageView = nullptr;
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

		uint16_t fps = 30;
		m_streamingServer = new StreamingServer();
		if (!m_streamingServer)
		{
			Shutdown();
			return false;
		}

		m_streamingServer->SetStreamInfo(static_cast<uint16_t>(outputWidth), static_cast<uint16_t>(outputHeight), fps, DESKTOP_STREAM_CODEC_TYPE::H264);
		if (!m_streamingServer->StartServer("0.0.0.0", 27015, 64))
		{
			Shutdown();
			return false;
		}

		m_encodeFrameQueue = new EncodeFrameQueue();
		if (!m_encodeFrameQueue)
		{
			Shutdown();
			return false;
		}

		if (!m_encodeFrameQueue->Initialize(4, ReleaseCapturedFrameHandleCallback, this))
		{
			Shutdown();
			return false;
		}

		m_encodeThread = new EncodeThread();
		if (!m_encodeThread)
		{
			Shutdown();
			return false;
		}

		m_encodeThread->SetKeyFrameRequestCallback(KeyFrameRequestCallback, this);
		m_encodeThread->SetEncodedFrameCallback(EncodedFrameCallback, this);
		if (!m_encodeThread->Initialize(m_encodeFrameQueue, m_nvEncoder))
		{
			Shutdown();
			return false;
		}

		m_duplicateEngine->SetTargetFps(fps);
		m_duplicateEngine->SetFrameCaptureCallback(FrameCallback, &m_callbackContext);

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
			m_duplicateEngine->StopThread();
		}

		if (m_encodeThread)
		{
			m_encodeThread->Shutdown();
			delete m_encodeThread;
			m_encodeThread = nullptr;
		}

		if (m_encodeFrameQueue)
		{
			delete m_encodeFrameQueue;
			m_encodeFrameQueue = nullptr;
		}

		if (m_streamingServer)
		{
			m_streamingServer->StopServer();
			delete m_streamingServer;
			m_streamingServer = nullptr;
		}

		if (m_duplicateEngine)
		{
			m_duplicateEngine->Shutdown();
			delete m_duplicateEngine;
			m_duplicateEngine = nullptr;
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
	static void FrameCallback(void* userData)
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

	static void ReleaseCapturedFrameHandleCallback(EncodeFrameQueue::InputFrameHandle& frameHandle, void* userData)
	{
		DesktopStreamingServerApp* self = static_cast<DesktopStreamingServerApp*>(userData);
		if (self)
		{
			self->ReleaseCapturedFrameHandle(frameHandle);
		}
	}

	static bool KeyFrameRequestCallback(void* userData)
	{
		DesktopStreamingServerApp* self = static_cast<DesktopStreamingServerApp*>(userData);
		return self ? self->HasViewerWaitingForKeyframe() : false;
	}

	static void EncodedFrameCallback(const EncodeThread::EncodedFrame& frame, void* userData)
	{
		DesktopStreamingServerApp* self = static_cast<DesktopStreamingServerApp*>(userData);
		if (self)
		{
			self->OnEncodedFrame(frame);
		}
	}

	void OnFrameCallback()
	{
		if (!m_callbackContext.sharedData || !m_duplicateEngine || !m_encodeFrameQueue || !m_streamingServer)
			return;

		if (!m_streamingServer->HasSubscribedViewer())
			return;

		CapturedFrameHandle frameHandle = m_duplicateEngine->GetLatestFrameHandle();

		if (!frameHandle.texture)
		{
			return;
		}

		EncodeFrameQueue::InputFrameHandle encodeFrameHandle = {};
		encodeFrameHandle.texture = frameHandle.texture;
		encodeFrameHandle.sourceSlotId = frameHandle.slotId;
		encodeFrameHandle.frameId = frameHandle.frameId;

		const bool forceKeyFrame = m_streamingServer->HasViewerWaitingForKeyframe();
		if (!m_encodeFrameQueue->EnqueueLatest(encodeFrameHandle, forceKeyFrame))
		{
			m_duplicateEngine->ReleaseLatestFrameHandle(frameHandle);
			return;
		}
	}

	void ReleaseCapturedFrameHandle(EncodeFrameQueue::InputFrameHandle& frameHandle)
	{
		if (m_duplicateEngine)
		{
			CapturedFrameHandle capturedFrameHandle = {};
			capturedFrameHandle.texture = frameHandle.texture;
			capturedFrameHandle.slotId = static_cast<LONG>(frameHandle.sourceSlotId);
			capturedFrameHandle.frameId = frameHandle.frameId;

			m_duplicateEngine->ReleaseLatestFrameHandle(capturedFrameHandle);

			frameHandle.texture = nullptr;
			frameHandle.sourceSlotId = -1;
			frameHandle.frameId = 0ULL;
		}
	}

	bool HasViewerWaitingForKeyframe()
	{
		return m_streamingServer && m_streamingServer->HasViewerWaitingForKeyframe();
	}

	void OnEncodedFrame(const EncodeThread::EncodedFrame& frame)
	{
		if (!m_streamingServer || !frame.data || frame.size == 0)
			return;

		m_streamingServer->BroadcastEncodedFrame(
			frame.data,
			frame.size,
			frame.frameId,
			frame.timestamp,
			frame.frameType,
			frame.isKeyFrame);
	}

	void EnableD3D11MultithreadProtection()
	{
		if (!m_D3D11Engine || !m_D3D11Engine->GetD3DDeviceContext())
			return;

		ID3D11Multithread* multithread = nullptr;
		if (SUCCEEDED(m_D3D11Engine->GetD3DDeviceContext()->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread))) && multithread)
		{
			multithread->SetMultithreadProtected(TRUE);
			multithread->Release();
		}
	}

private:
	bool m_running = false;
	SharedCaptureData m_sharedData = {};
	CaptureCallbackContext m_callbackContext = {};
	D3D11RenderEngine* m_D3D11Engine = nullptr;
	D3D11DuplicateEngine* m_duplicateEngine = nullptr;
	D3D11NvEncoder* m_nvEncoder = nullptr;
	StreamingServer* m_streamingServer = nullptr;
	EncodeFrameQueue* m_encodeFrameQueue = nullptr;
	EncodeThread* m_encodeThread = nullptr;
};
