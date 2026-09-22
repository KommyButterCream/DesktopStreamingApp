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
#include "../Service/StreamingClient/StreamingClient.h"

class DesktopStreamingClientApp
{
public:
	DesktopStreamingClientApp() = default;
	~DesktopStreamingClientApp()
	{
		Shutdown();
	}

	bool Initialize(const char* serverIp = "127.0.0.1", uint16_t serverPort = 27015)
	{
		::strncpy_s(m_serverIp, serverIp, _TRUNCATE);
		m_serverPort = serverPort;

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

		// 여기에 outputWidth / outputHeight = 2560 / 1440 이 있었다.
		// 선언하고 0인지 검사한 뒤 아무 데도 쓰지 않았다 — 디코더는 비트
		// 스트림의 SPS 에서 해상도를 읽고, ImageView 는 공유 텍스처를
		// 그대로 받는다. 서버가 알려주는 크기는 OnStreamInfo 로 온다.

		m_nvDecoder = new D3D11NvDecoder();
		if (!m_nvDecoder)
		{
			Shutdown();
			return false;
		}

		// 유입 큐는 디코더가 소유한다. 버퍼는 프로토콜의 프레임 상한에 맞춘다 —
		// 큐가 패킷을 자기 버퍼로 복사하므로 가장 큰 프레임이 들어가야 한다.
		NvDecConfig decoderConfig;
		decoderConfig.inputQueueDepth = 8;
		decoderConfig.maxPacketSize = DESKTOP_STREAM_MAX_FRAME_SIZE;
		decoderConfig.sharedOutputTextureMode = true;

		// contextGate = nullptr: 이 엔진의 immediate context 를 쓰는 주체는
		// 디코더 하나뿐이다. ImageView 는 자기 엔진을 따로 만들고(아래 Initialize 의
		// D3D11Engine 인자가 nullptr), 디코딩 결과는 shared handle 로 건네받는다.
		if (!m_nvDecoder->Initialize(m_D3D11Engine->GetD3DDevice(), decoderConfig, nullptr))
		{
			printf_s("[DesktopStreamingClient] Failed to initialize D3D11NvDecoder.\n");
			Shutdown();
			return false;
		}

		// 프레임 유실과 파이프라인 정지를 통지받는다. 등록하지 않고 있었다.
		//
		// NvDecErrorCode::OutputPoolExhausted 와 OutputNotConsumed 는 앱이
		// 프레임을 제때 가져가지 않는다는 뜻이다. 통지가 없으면 그 상황이
		// "가끔 끊기는 화면" 으로만 보인다.
		m_nvDecoder->SetErrorCallback(DecoderErrorCallback, this);

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

		// 뷰어 창이 이 앱의 유일한 창이다. 닫기 버튼을 누르면 그것이 곧
		// 종료 요청이므로 통지를 받아 메시지 루프를 끝낸다.
		m_imageView->SetCloseHandler(ViewerCloseCallback, this);
		::InterlockedExchange(&m_viewerAlive, TRUE);

		// 디코드 워커와 유입 큐를 모두 디코더가 소유한다.
		m_nvDecoder->SetFrameCallback(DecodedFrameCallback, this);
		if (!m_nvDecoder->StartDecodeThread())
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

		// 접속 상태 변화를 받는다. 재접속은 이 콜백 안이 아니라 Run() 의
		// 루프가 한다 — 이 콜백은 엔진 워커 스레드에서 불리므로 그 안에서
		// StopClient 를 부르면 자기 스레드의 종료를 자기가 기다리게 된다.
		m_streamingClient->SetConnectionCallback(ConnectionCallback, this);

		// 서버가 아직 안 떠 있어도 실패가 아니다. 루프가 계속 다시 붙는다.
		if (!m_streamingClient->StartClient(m_serverIp, m_serverPort))
		{
			printf_s("[DesktopStreamingClient] Initial connect to %s:%u failed. will retry.\n",
				m_serverIp, m_serverPort);
			m_reconnectPending = TRUE;
		}

		::InterlockedExchange(&m_running, TRUE);
		m_nextStatsTick = ::GetTickCount64() + STATS_INTERVAL_MS;
		return true;
	}

	void Run()
	{
		std::string cmd;
		MSG message = {};

		while (IsRunning())
		{
			while (::PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
			{
				if (message.message == WM_QUIT)
				{
					RequestStop();
					break;
				}

				::TranslateMessage(&message);
				::DispatchMessage(&message);
			}

			if (!IsRunning())
				break;

			if (_kbhit())
			{
				std::getline(std::cin, cmd);
				if (cmd == "quit")
				{
					RequestStop();
				}
				else if (cmd == "stats")
				{
					PrintStats();
				}
			}

			const ULONGLONG now = ::GetTickCount64();

			ServiceReconnect(now);

			// 수신 상태를 서버에 알린다. 서버의 비트레이트 조정이 이걸 본다.
			ServiceFeedback(now);

			if (now >= m_nextStatsTick)
			{
				m_nextStatsTick = now + STATS_INTERVAL_MS;
				PrintStats();
			}

			::Sleep(10);
		}
	}

	void RequestStop()
	{
		::InterlockedExchange(&m_running, FALSE);
	}

	bool IsRunning() const
	{
		return ::ReadAcquire(&m_running) != FALSE;
	}

	void Shutdown()
	{
		::InterlockedExchange(&m_running, FALSE);
		::InterlockedExchange(&m_viewerAlive, FALSE);

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

	void PrintStats()
	{
		DesktopStreamingClientStats netStats = {};
		if (m_streamingClient)
			m_streamingClient->GetStats(netStats);

		NvDecStats decodeStats = {};
		if (m_nvDecoder)
			m_nvDecoder->GetStats(decodeStats);

		const uint64_t queueDrops = decodeStats.droppedInputQueue;

		printf_s(
			"[stats] net connected=%d chunks=%llu rejected=%llu frames=%llu discarded=%llu bytes=%llu | "
			"decode parsed=%llu decoded=%llu delivered=%llu poolDrop=%llu lagDrop=%llu qdrop=%llu faulted=%d\n",
			netStats.connected ? 1 : 0, netStats.chunksAccepted, netStats.chunksRejected,
			netStats.framesCompleted, netStats.framesDiscarded, netStats.chunkBytesReceived,
			decodeStats.parsedPackets, decodeStats.decodedFrames, decodeStats.deliveredFrames,
			decodeStats.droppedPoolExhausted, decodeStats.droppedNotConsumed, queueDrops,
			decodeStats.faulted ? 1 : 0);

		// 페이싱 상태. 대기가 전혀 없으면 페이싱이 안 걸리는 것이고,
		// resync 가 계속 오르면 기준이 못 잡히는 것이다.
		printf_s("[pace] presented=%llu buffer=%ldms waits=%llu avgWait=%.1fms resync=%llu\n",
			static_cast<unsigned long long>(m_presentedFrames),
			::ReadAcquire(&m_jitterBufferMs),
			static_cast<unsigned long long>(m_paceWaitCount),
			m_paceWaitCount > 0 ? static_cast<double>(m_paceWaitTotalMs) / m_paceWaitCount : 0.0,
			static_cast<unsigned long long>(m_paceResyncCount));

		// 서버 쪽과 같은 사정이다. 리디렉션된 stdout 은 완전 버퍼링이라
		// 흘려보내지 않으면 주기 지표가 제때 보이지 않는다.
		::fflush(stdout);
	}

private:
	static constexpr ULONGLONG STATS_INTERVAL_MS = 5'000;
	static constexpr ULONGLONG RECONNECT_INTERVAL_MS = 2'000;

	// 서버에 수신 상태를 알리는 주기. 서버의 비트레이트 조정이 이걸 본다.
	static constexpr ULONGLONG FEEDBACK_INTERVAL_MS = 1'000;

	// 지터 버퍼 기본 깊이. 한 프레임 남짓이다.
	//
	// 원격 화면 공유는 지연이 화질보다 중요하므로 깊게 잡지 않는다.
	// 16ms 면 60fps 한 프레임이고, 도착 흔들림의 대부분을 흡수하면서
	// 사람이 느낄 만한 지연은 더하지 않는다. 0 으로 두면 페이싱을 끈다.
	static constexpr LONG DEFAULT_JITTER_BUFFER_MS = 16;

	// 이보다 오래 기다려야 하면 기준이 틀어진 것이다. 다시 잡는다.
	static constexpr ULONGLONG MAX_PACE_WAIT_MS = 100;

	// 이보다 많이 밀렸으면 따라잡기를 포기하고 기준을 다시 잡는다.
	static constexpr ULONGLONG MAX_PACE_DRIFT_MS = 250;

	// 끊긴 뒤 다시 붙는다.
	//
	// 콜백 안이 아니라 여기서 하는 이유는 수명이다. StopClient 는 엔진의
	// 워커 스레드들이 빠져나오기를 기다리는데, 그 대기를 워커 스레드가
	// 스스로 하면 영원히 끝나지 않는다. 콜백은 표시만 남기고 실제 작업은
	// 앱 스레드인 이 루프가 한다.

	// 수신 상태를 서버에 알린다. 앱 스레드에서 부른다.
	//
	// 디코더 통계는 서비스가 모르므로 여기서 합쳐 넘긴다. 서버는 이 값과
	// 자기 송신 실패를 같이 보고 비트레이트를 정한다.
	void ServiceFeedback(ULONGLONG now)
	{
		if (!m_streamingClient || !m_streamingClient->IsConnected())
			return;

		if (now < m_nextFeedbackTick)
			return;

		m_nextFeedbackTick = now + FEEDBACK_INTERVAL_MS;

		NvDecStats decodeStats = {};
		if (m_nvDecoder)
			m_nvDecoder->GetStats(decodeStats);

		const uint64_t queueDrops = decodeStats.droppedInputQueue;

		// 디코더가 버린 것과 큐가 버린 것을 합친다. 서버 입장에서는 둘 다
		// "이 뷰어가 못 따라간다" 는 같은 신호다.
		const uint64_t decodeDroppedTotal =
			decodeStats.droppedPoolExhausted +
			decodeStats.droppedNotConsumed +
			decodeStats.droppedDisplayFailed +
			queueDrops;

		m_streamingClient->SendFeedback(decodeDroppedTotal);
	}

	void ServiceReconnect(ULONGLONG now)
	{
		if (!m_streamingClient)
			return;

		if (::ReadAcquire(&m_reconnectPending) == FALSE)
			return;

		if (now < m_nextReconnectTick)
			return;

		m_nextReconnectTick = now + RECONNECT_INTERVAL_MS;

		// 이미 붙어 있으면 할 일이 없다. (콜백과 루프 사이에 다시 붙은 경우)
		if (m_streamingClient->IsConnected())
		{
			::InterlockedExchange(&m_reconnectPending, FALSE);
			return;
		}

		printf_s("[net] reconnecting to %s:%u ...\n", m_serverIp, m_serverPort);

		m_streamingClient->StopClient();

		if (m_streamingClient->StartClient(m_serverIp, m_serverPort))
		{
			::InterlockedExchange(&m_reconnectPending, FALSE);
		}
	}

	static void StreamInfoCallback(const DesktopStreamClientSessionContext& streamContext, void* userData)
	{
		DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
		if (self)
		{
			self->OnStreamInfo(streamContext);
		}
	}

	static void FrameCallback(const uint8_t* frameData, uint32_t frameSize, uint64_t /*frameId*/, uint64_t timestamp, uint16_t /*frameType*/, void* userData)
	{
		DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
		if (self)
		{
			self->OnEncodedFrame(frameData, frameSize, timestamp);
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

	static void ViewerCloseCallback(void* userData)
	{
		DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
		if (self)
		{
			self->OnViewerClosed();
		}
	}

	// 뷰어 창이 파괴되기 직전에 UI 스레드에서 불린다.
	//
	// 깃발만 내린다. 네트워크/디코더 정리는 Run 이 빠져나온 뒤 Shutdown 이
	// 하던 대로 한다 — 창 메시지 처리 중에 스레드 조인까지 하면 닫기 반응이
	// 그만큼 늦어진다.
	//
	// 이 함수가 돌아가는 즉시 창이 파괴되고 뷰어가 정리되므로, 디코드
	// 스레드가 그 뒤로 뷰어를 건드리지 않게 m_viewerAlive 를 먼저 내린다.
	void OnViewerClosed()
	{
		::InterlockedExchange(&m_viewerAlive, FALSE);
		RequestStop();
	}

	static void DecoderErrorCallback(NvDecErrorCode errorCode, void* userData)
	{
		DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
		if (self)
		{
			self->OnDecoderError(errorCode);
		}
	}

	static void ConnectionCallback(DESKTOP_STREAM_CONNECTION_EVENT event, DisconnectReason reason, int errorCode, void* userData)
	{
		DesktopStreamingClientApp* self = static_cast<DesktopStreamingClientApp*>(userData);
		if (self)
		{
			self->OnConnectionEvent(event, reason, errorCode);
		}
	}

	// 엔진 워커 스레드에서 불린다. 표시만 남긴다.
	void OnConnectionEvent(DESKTOP_STREAM_CONNECTION_EVENT event, DisconnectReason reason, int errorCode)
	{
		switch (event)
		{
		case DESKTOP_STREAM_CONNECTION_EVENT::Established:
			printf_s("[net] connected to %s:%u\n", m_serverIp, m_serverPort);
			::InterlockedExchange(&m_reconnectPending, FALSE);
			break;

		case DESKTOP_STREAM_CONNECTION_EVENT::ConnectFailed:
			printf_s("[net] connect failed (WSA %d)\n", errorCode);
			::InterlockedExchange(&m_reconnectPending, TRUE);
			break;

		case DESKTOP_STREAM_CONNECTION_EVENT::Disconnected:
			printf_s("[net] disconnected : %s\n", ToString(reason));

			// 우리가 내린 종료(StopClient)라면 다시 붙지 않는다.
			// 엔진이 그 판단을 대신 해 준다.
			if (IsRetryableDisconnect(reason))
			{
				::InterlockedExchange(&m_reconnectPending, TRUE);
			}
			break;

		default:
			break;
		}
	}

	// 디코드 스레드에서 불린다.
	void OnDecoderError(NvDecErrorCode errorCode)
	{
		const char* name = "unknown";
		bool fatal = false;

		switch (errorCode)
		{
		case NvDecErrorCode::OutputPoolExhausted: name = "output pool exhausted (frame lost)"; break;
		case NvDecErrorCode::OutputNotConsumed:   name = "output not consumed (frame skipped)"; break;
		case NvDecErrorCode::ParseFailed:         name = "parse failed"; break;
		case NvDecErrorCode::DecodeFailed:        name = "decode failed"; break;
		case NvDecErrorCode::DisplayFailed:       name = "display failed (frame lost)"; break;
		case NvDecErrorCode::ReconfigureFailed:   name = "reconfigure failed"; fatal = true; break;
		case NvDecErrorCode::DecoderFaulted:      name = "decoder faulted"; fatal = true; break;
		default: break;
		}

		printf_s("[decode] %s\n", name);

		if (fatal)
		{
			RequestStop();
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

		// 스트림이 바뀌면 페이싱 기준도 다시 잡아야 한다. 다음 프레임이
		// 잡도록 무효로 표시한다.
		::InterlockedExchange(&m_paceResetRequest, TRUE);
	}

	void OnEncodedFrame(const uint8_t* frameData, uint32_t frameSize, uint64_t timestamp)
	{
		if (!m_nvDecoder || !frameData || frameSize == 0)
			return;

		NvDecPacket packet = {};
		packet.data = frameData;
		packet.size = frameSize;
		packet.timestamp = timestamp;

		// 실패는 큐가 가득 찬 것이고 그때 가장 오래된 패킷이 버려진다.
		// 그 수는 GetStats 의 droppedInputQueue 로 나오므로 여기서 따로
		// 세지 않는다.
		m_nvDecoder->EnqueuePacket(packet);
	}

	// --- 지터 버퍼 ---
	//
	// 디코드 스레드에서 불린다.
	//
	// 예전에는 도착 즉시 표시했다. 네트워크 지터가 그대로 화면 떨림이 된다 —
	// 60fps 라면 프레임 간격이 16ms 인데, 도착이 5ms 빨랐다 20ms 늦었다
	// 하면 사람 눈에 끊김으로 보인다.
	//
	// timestamp 를 쓴다. 이 값은 인코더가 넣은 프레임 순번이 NVDEC 를
	// 그대로 통과해 돌아온 것이다(picParams.inputTimeStamp). 서버의
	// 프레임 간격이 1/fps 이므로, 순번 차이에 프레임 간격을 곱하면 이
	// 프레임이 '표시되어야 할 시각' 이 나온다.
	//
	//   목표시각 = 기준시각 + (순번 - 기준순번) * (1000/fps) + 버퍼깊이
	//
	// 목표보다 이르면 그만큼 기다렸다 표시한다. 늦었으면 그냥 표시한다 —
	// 이미 늦은 것을 더 늦출 이유가 없다.
	//
	// 왜 프레임을 쌓아 두지 않고 여기서 기다리는가
	//   D3D11NvDecoder::FrameCallback 의 계약이 "frame 은 콜백이 반환할
	//   때까지만 유효하다" 다. 텍스처를 들고 나가려면 AcquireFrame /
	//   ReleaseFrame 을 직접 돌려야 하는데, 그건 디코더의 슬롯 관리를
	//   앱으로 가져오는 일이라 훨씬 크다.
	//
	//   여기서 기다리면 디코드 스레드가 그만큼 멈추고, 그게 그대로
	//   디코더 유입 큐의 백프레셔가 된다. 버퍼 깊이가 얕으면 그 대기도
	//   짧으므로 실질적으로는 '소스 박자에 맞춰 푸는' 효과만 남는다.
	//
	// 대기 상한을 두는 이유
	//   기준시각이 한 번 틀어지면(재접속, 스트림 변경) 목표시각이 한참
	//   뒤로 갈 수 있다. 그때 그대로 기다리면 화면이 멎는다. 상한을 넘으면
	//   기준을 다시 잡는다.
	void OnDecodedFrame(const D3D11NvDecoder::Frame& frame)
	{
		if (!m_imageView)
			return;

		// 창이 닫히는 중이면 뷰어를 건드리지 않는다.
		if (::ReadAcquire(&m_viewerAlive) == FALSE)
			return;

		if (!frame.sharedHandle)
			return;

		PaceFramePresentation(frame.timestamp);

		m_imageView->UpdateSharedTexture(frame.sharedHandle);
		++m_presentedFrames;
	}

	void PaceFramePresentation(uint64_t frameTimestamp)
	{
		const uint32_t bufferDepthMs = ::ReadAcquire(&m_jitterBufferMs);

		if (bufferDepthMs == 0)
			return;   // 페이싱을 끄면 예전처럼 즉시 표시한다

		const uint16_t fps = static_cast<uint16_t>(::ReadAcquire(&m_streamFpsAtomic));
		const double frameIntervalMs = 1000.0 / fps;
		const ULONGLONG now = ::GetTickCount64();

		// 기준이 없거나 순번이 뒤로 갔으면(재접속/스트림 변경) 다시 잡는다.
		// 스트림이 바뀌었으면 기준을 버린다. (OnStreamInfo 가 세운다)
		const bool resetRequested = ::InterlockedExchange(&m_paceResetRequest, FALSE) != FALSE;

		if (!m_paceBaseValid || resetRequested || frameTimestamp < m_paceBaseTimestamp)
		{
			m_paceBaseValid = true;
			m_paceBaseTimestamp = frameTimestamp;
			m_paceBaseTick = now;
			return;
		}

		const uint64_t elapsedFrames = frameTimestamp - m_paceBaseTimestamp;
		const ULONGLONG targetTick = m_paceBaseTick +
			static_cast<ULONGLONG>(elapsedFrames * frameIntervalMs) + bufferDepthMs;

		if (targetTick <= now)
		{
			// 늦었다. 누적 지연이 상한을 넘으면 기준을 다시 잡는다 —
			// 그러지 않으면 한 번 밀린 만큼 영원히 밀린 채로 간다.
			if ((now - targetTick) > MAX_PACE_DRIFT_MS)
			{
				m_paceBaseTimestamp = frameTimestamp;
				m_paceBaseTick = now;
				++m_paceResyncCount;
			}
			return;
		}

		ULONGLONG waitMs = targetTick - now;
		if (waitMs > MAX_PACE_WAIT_MS)
		{
			// 여기까지 오면 기준이 틀어진 것이다. 기다리지 않고 다시 잡는다.
			m_paceBaseTimestamp = frameTimestamp;
			m_paceBaseTick = now;
			++m_paceResyncCount;
			return;
		}

		::Sleep(static_cast<DWORD>(waitMs));
		m_paceWaitTotalMs += waitMs;
		++m_paceWaitCount;
	}

private:
	volatile LONG m_running = FALSE;

	// 뷰어 창이 살아 있는가. UI 스레드가 내리고 디코드 스레드가 읽는다.
	volatile LONG m_viewerAlive = FALSE;
	volatile LONG m_reconnectPending = FALSE;

	ULONGLONG m_nextStatsTick = 0;
	ULONGLONG m_nextReconnectTick = 0;
	ULONGLONG m_nextFeedbackTick = 0;

	// 지터 버퍼. 디코드 스레드가 읽고 앱 스레드가 바꿀 수 있어 원자적이다.
	volatile LONG m_jitterBufferMs = DEFAULT_JITTER_BUFFER_MS;

	// 스트림이 바뀌었으니 페이싱 기준을 다시 잡으라는 표시.
	// 앱/워커 스레드가 세우고 디코드 스레드가 내린다.
	volatile LONG m_paceResetRequest = FALSE;

	// 페이싱 상태. 디코드 스레드 전용.
	bool m_paceBaseValid = false;
	uint64_t m_paceBaseTimestamp = 0;
	ULONGLONG m_paceBaseTick = 0;
	uint64_t m_paceWaitTotalMs = 0;
	uint64_t m_paceWaitCount = 0;
	uint64_t m_paceResyncCount = 0;
	uint64_t m_presentedFrames = 0;

	// 스트림 정보에서 받은 프레임률. 페이싱 간격의 기준이다.
	volatile LONG m_streamFpsAtomic = 60;

	char m_serverIp[64] = "127.0.0.1";
	uint16_t m_serverPort = 27015;

	D3D11RenderEngine* m_D3D11Engine = nullptr;
	D3D11NvDecoder* m_nvDecoder = nullptr;
	D3D11ImageView* m_imageView = nullptr;
	StreamingClient* m_streamingClient = nullptr;
};
