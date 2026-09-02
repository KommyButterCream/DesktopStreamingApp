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
#include "../../../Module/NvCodec/NvDecode/DecodeFrameQueue.h"
#include "../../../Module/NvCodec/NvDecode/D3D11NvDecoder.h"
#include "../Service/StreamingClient/StreamingClient.h"

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
		RenderEngineConfig renderEngineConfig = {};
		renderEngineConfig.initD2D = false;
		renderEngineConfig.initD3D = true;
		renderEngineConfig.initFontManager = false;
#if defined(_DEBUG)
		renderEngineConfig.initDebugLayer = true;
#endif
		
		m_D3D11Engine = new D3D11RenderEngine();
		if (!m_D3D11Engine)
		{
			Shutdown();
			return false;
		}

		if (!m_D3D11Engine->Initialize(renderEngineConfig))
		{
			printf_s("[DesktopStreamingClient] Failed to initialize D3D11RenderEngine.\n");
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

		// contextGate = nullptr: 이 엔진의 immediate context 를 쓰는 주체는
		// 디코더 하나뿐이다. ImageView 는 자기 엔진을 따로 만들고(아래 Initialize 의
		// D3D11Engine 인자가 nullptr), 디코딩 결과는 shared handle 로 건네받는다.
		// 세 번째 인자는 그 shared output texture 모드를 켜는 것이다.
		if (!m_nvDecoder->Initialize(m_D3D11Engine->GetD3DDevice(), nullptr, true))
		{
			printf_s("[DesktopStreamingClient] Failed to initialize D3D11NvDecoder.\n");
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

		if (!m_imageView->Initialize(GetDesktopWindow(), RECT(0, 0, 1920, 900), windowStyle, nullptr))
		{
			printf_s("[DesktopStreamingClient] Failed to initialize D3D11ImageView.\n");
			Shutdown();
			return false;
		}

		m_decodeFrameQueue = new DecodeFrameQueue(DESKTOP_STREAM_MAX_FRAME_SIZE, 8);
		if (!m_decodeFrameQueue || m_decodeFrameQueue->GetBufferSize() == 0)
		{
			printf_s("[DesktopStreamingClient] Failed to initialize DecodeFrameQueue.\n");
			Shutdown();
			return false;
		}

		// 디코드 워커는 이제 디코더가 소유한다. 앱이 DecodeThread 를 직접
		// 만들지 않고 StartDecodeThread 로 맡긴다.
		m_nvDecoder->SetFrameCallback(DecodedFrameCallback, this);
		if (!m_nvDecoder->StartDecodeThread(m_decodeFrameQueue))
		{
			printf_s("[DesktopStreamingClient] Failed to start the decode thread.\n");
			Shutdown();
			return false;
		}

		m_streamingClient = new StreamingClient();
		if (!m_streamingClient)
		{
			Shutdown();
			return false;
		}

		m_streamingClient->SetStreamInfoCallback(StreamInfoCallback, this);
		m_streamingClient->SetFrameCallback(FrameCallback, this);

		if (!m_streamingClient->StartClient("127.0.0.1", 27015))
		{
			printf_s("[DesktopStreamingClient] Failed to connect to 127.0.0.1:27015.\n");
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

		if (m_streamingClient)
		{
			m_streamingClient->StopClient();
			delete m_streamingClient;
			m_streamingClient = nullptr;
		}

		// 워커가 큐를 참조하므로 큐를 지우기 전에 반드시 멈춘다.
		if (m_nvDecoder)
		{
			m_nvDecoder->StopDecodeThread();
		}

		if (m_decodeFrameQueue)
		{
			delete m_decodeFrameQueue;
			m_decodeFrameQueue = nullptr;
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

		if (m_D3D11Engine)
		{
			delete m_D3D11Engine;
			m_D3D11Engine = nullptr;
		}
	}

private:
	static void StreamInfoCallback(const DesktopStreamClientSessionContext& streamContext, void* userData)
	{
		DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
		if (self)
		{
			self->OnStreamInfo(streamContext);
		}
	}

	static void FrameCallback(const uint8_t* frameData, uint32_t frameSize, uint64_t frameId, uint64_t timestamp, uint16_t frameType, void* userData)
	{
		DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
		if (self)
		{
			self->OnEncodedFrame(frameData, frameSize, frameId, timestamp, frameType);
		}
	}

	static void DecodedFrameCallback(const D3D11NvDecoder::Frame& frame, void* userData)
	{
		DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
		if (self)
		{
			self->OnDecodedFrame(frame);
		}
	}

	void OnStreamInfo(const DesktopStreamClientSessionContext& streamContext)
	{
		printf_s(
			"Stream info: streamId=%u, %ux%u, codec=%u, infoVersion=%u, codecConfigVersion=%u\n",
			streamContext.streamId,
			streamContext.width,
			streamContext.height,
			streamContext.codecType,
			streamContext.streamInfoVersion,
			streamContext.codecConfigVersion);
	}

	void OnEncodedFrame(const uint8_t* frameData, uint32_t frameSize, uint64_t frameId, uint64_t timestamp, uint16_t frameType)
	{
		if (!m_decodeFrameQueue || !frameData || frameSize == 0)
			return;

		DecodeFrameQueue::InputFrameHandle frameHandle = {};
		frameHandle.data = frameData;
		frameHandle.size = frameSize;
		frameHandle.frameId = frameId;
		frameHandle.timestamp = timestamp;
		frameHandle.frameType = frameType;

		if (!m_decodeFrameQueue->EnqueueFrame(frameHandle))
		{
			return;
		}
	}

	void OnDecodedFrame(const D3D11NvDecoder::Frame& frame)
	{
		if (!m_imageView)
			return;

		if (frame.sharedHandle)
		{
			m_imageView->UpdateSharedTexture(frame.sharedHandle);
		}
	}

private:
	bool m_running = false;
	D3D11RenderEngine* m_D3D11Engine = nullptr;
	D3D11NvDecoder* m_nvDecoder = nullptr;
	D3D11ImageView* m_imageView = nullptr;
	DecodeFrameQueue* m_decodeFrameQueue = nullptr;
	StreamingClient* m_streamingClient = nullptr;
};
