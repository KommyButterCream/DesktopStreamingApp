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
#include "../Service/StreamingServer/StreamingServer.h"

class DesktopStreamingServerApp
{
public:
	// --- 목표 스트림 : QHD(2560x1440) 60fps, 서버 하나에 여러 뷰어 ---
	//
	// 프레임률은 여기 한 곳에서만 정한다. 캡처(SetTargetFps)와 인코더
	// (NvEncConfig::frameRateNumerator)와 스트림 정보가 전부 이 값을 쓴다.
	// 예전에는 셋이 서로 다른 값을 들고 있었다.
	//
	// 60 이 되기까지
	//   캡처와 인코더가 같은 D3D11 디바이스를 쓰던 시절, 60fps 에서
	//   수십 프레임 만에 캡처가 영구히 멈췄다. 스택 덤프로 잡은 교착이다.
	//
	//     캡처   : ProcessCaptureFrame -> D3D11ImmediateContextGate::Enter
	//              -> RtlAcquireSRWLockExclusive              (영구 대기)
	//     인코드 : SubmitFrame -> nvEncodeAPI64 -> d3d11
	//              -> nvwgf2umx -> WaitForSingleObjectEx      (게이트 보유)
	//
	//   immediate context 를 지키는 게이트 하나를 두 스레드가 다퉜고,
	//   인코더가 그걸 쥔 채 드라이버 안에서 무기한 블로킹했다. 캡처는
	//   16ms 마다 그 게이트를 원하므로 통째로 죽었다. 30fps 에서는 두
	//   스레드가 겹칠 창이 좁아 드러나지 않았을 뿐이다.
	//
	//   락 규약을 아무리 다듬어도 풀리지 않는 종류였다. 순환의 한 변이
	//   드라이버 안에 있어서, 우리가 그 대기를 놓아 줄 방법이 없다.
	//   (시도해서 아닌 것으로 확인: 복사 완료 대기 켜기 / 인코드 버퍼
	//    4->8 / nvEncEncodePicture 게이트 제거 / NV12 변환 뒤 Flush.
	//    nvEncMapInputResource 의 게이트를 빼면 프로세스가 죽는다)
	//
	//   그래서 게이트 공유 자체를 없앴다. 인코더에 별도 D3D11 디바이스를
	//   주고, 캡처 풀을 KEYEDMUTEX 공유 텍스처로 만들어 인코더 디바이스가
	//   초기화 때 한 번 열어 둔다. 두 컨텍스트가 독립이므로 인코더가
	//   드라이버 안에서 얼마나 오래 기다리든 캡처는 영향을 받지 않는다.
	//   OBS 가 인코더에 별도 디바이스를 주는 것과 같은 구조다.
	static constexpr uint16_t TARGET_FPS = 60;

	// QHD 데스크톱 화면 기준. 글자가 많아 움직임 예측이 잘 듣지 않으므로
	// 같은 해상도의 동영상보다 더 필요하다.
	//
	// 상한은 뷰어 수가 정한다. TCP 로 뷰어마다 같은 스트림을 보내므로
	// 서버 송신량은 비트레이트 x 뷰어 수다. 20Mbps x 8뷰어 = 160Mbps
	// (20MB/s) 로 기가비트 LAN 안에 들어온다. 뷰어가 더 늘거나 대역이
	// 좁으면 이 값을 내려야 하고, 그 조정을 자동으로 하는 것이 앞으로
	// 할 일이다(비트레이트 적응).
	static constexpr uint32_t TARGET_BITRATE_BPS = 20'000'000;
	static constexpr uint32_t MAX_BITRATE_BPS = 25'000'000;

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

		if (!m_D3D11Engine->SetImmediateContextGateEnabled(true))
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

		// 캡처 풀을 공유 가능하게 만든다. Initialize 전에 켜야 한다.
		//
		// 이게 이 구조의 출발점이다. 풀 텍스처가 KEYEDMUTEX 공유로
		// 만들어지고, 인코더 디바이스가 그걸 열어서 직접 읽는다.
		// (왜 디바이스를 나누는지는 TARGET_FPS 주석)
		//
		// SetWaitForFrameCopyCompletion(false) 는 그대로 둔다. 복사 완료를
		// CPU 이벤트 쿼리로 기다리는 대신 keyed mutex 가 그 순서를 맡는다 —
		// 그쪽이 드라이버가 아는 동기화라 더 정확하고 더 싸다.
		if (!m_duplicateEngine->SetFramePoolSharable(true) ||
			!m_duplicateEngine->SetWaitForFrameCopyCompletion(false) ||
			!m_duplicateEngine->Initialize(m_D3D11Engine, 0))
		{
			printf_s("[DesktopStreamingServer] Failed to initialize the capture engine.\n");
			Shutdown();
			return false;
		}

		// 접근 상실 / 디바이스 상실 / 해상도 변경을 통지받는다.
		//
		// 등록하지 않고 있었다. 그래서 캡처가 조용히 멈춰도(모니터 잠금,
		// 전체 화면 전환, 드라이버 갱신 — 전부 AccessLost 를 낸다) 서버는
		// 아무 일 없다는 듯 돌았고, 남는 증상은 "프레임이 안 온다" 뿐이었다.
		m_duplicateEngine->SetCaptureEventCallback(CaptureEventCallback, this);

		const uint32_t outputWidth = m_duplicateEngine->GetOutputWidth();
		const uint32_t outputHeight = m_duplicateEngine->GetOutputHeight();
		if (outputWidth == 0 || outputHeight == 0)
		{
			Shutdown();
			return false;
		}

		// --- 인코더 전용 D3D11 디바이스 ---
		//
		// 캡처와 같은 디바이스를 쓰면 immediate context 가 하나뿐이고,
		// 그걸 지키는 게이트를 두 스레드가 다툰다. 인코더가 그 게이트를
		// 쥔 채 드라이버 안에서 블로킹하면 캡처가 죽는다.
		//
		// 디바이스를 나누면 컨텍스트가 서로 독립이라 그 순환이 성립하지
		// 않는다. 인코더가 nvEncMapInputResource 안에서 얼마나 오래
		// 기다리든 캡처 스레드는 자기 컨텍스트로 계속 돈다.
		//
		// 게이트는 이 디바이스에도 여전히 필요하다 — EncodeThread 와
		// EncodeCompletionThread 가 이 컨텍스트를 같이 쓰기 때문이다.
		// 다만 그 둘은 캡처와 무관하다.
		m_encodeEngine = new D3D11RenderEngine();
		if (!m_encodeEngine)
		{
			Shutdown();
			return false;
		}

		if (!m_encodeEngine->SetImmediateContextGateEnabled(true))
		{
			Shutdown();
			return false;
		}

		RenderEngineConfig encodeEngineConfig = {};
		encodeEngineConfig.initD2D = false;
		encodeEngineConfig.initD3D = true;
		encodeEngineConfig.initFontManager = false;
#if defined(_DEBUG)
		encodeEngineConfig.initDebugLayer = true;
#endif

		if (!m_encodeEngine->Initialize(encodeEngineConfig))
		{
			printf_s("[DesktopStreamingServer] Failed to initialize the encoder D3D11 device.\n");
			Shutdown();
			return false;
		}

		m_nvEncoder = new D3D11NvEncoder();
		if (!m_nvEncoder)
		{
			Shutdown();
			return false;
		}

		// 인코더 생성과 공유 풀 등록. 해상도 변경 때 같은 경로를 다시 탄다.
		if (!InitializeEncoder(outputWidth, outputHeight))
		{
			Shutdown();
			return false;
		}

		// 한 프레임 예산이 프로토콜의 프레임 상한 안에 들어오는지.
		//
		// CBR + 단일 프레임 VBV + lowDelayKeyFrameScale=1 이므로 프레임
		// 크기는 이 예산 근처에 묶인다. 상한을 넘기면 브로드캐스트가
		// 프레임을 통째로 거절하므로(화면이 아예 안 나온다) 기동 시점에
		// 여유를 확인해 둔다.
		const uint32_t bytesPerFrameBudget = MAX_BITRATE_BPS / TARGET_FPS / 8;
		printf_s("[DesktopStreamingServer] %ux%u @%u fps, %.1f Mbps (max %.1f) "
			"-> ~%u bytes/frame, frame cap %u bytes (x%.1f headroom)\n",
			outputWidth, outputHeight, TARGET_FPS,
			TARGET_BITRATE_BPS / 1'000'000.0, MAX_BITRATE_BPS / 1'000'000.0,
			bytesPerFrameBudget, DESKTOP_STREAM_MAX_FRAME_SIZE,
			static_cast<double>(DESKTOP_STREAM_MAX_FRAME_SIZE) / bytesPerFrameBudget);

		const uint16_t fps = TARGET_FPS;
		m_streamingServer = new StreamingServer();
		if (!m_streamingServer)
		{
			Shutdown();
			return false;
		}

		// StartServer 보다 먼저 불러야 한다. 여기서 올린 버전이 구독 응답에
		// 실려 나간다.
		m_streamWidth = static_cast<uint16_t>(outputWidth);
		m_streamHeight = static_cast<uint16_t>(outputHeight);
		m_streamingServer->SetStreamInfo(
			static_cast<uint16_t>(outputWidth), static_cast<uint16_t>(outputHeight), fps, DESKTOP_STREAM_CODEC_TYPE::H264);

		// 큐 깊이를 이 스트림의 실제 모양에서 계산한다.
		//
		// 예전에는 프로토콜 상한(512KB 프레임 = 17청크)에서 잡았다. 실제
		// 프레임은 42KB = 2청크라, "4프레임" 이라고 적어 둔 68엔트리가
		// 실제로는 34프레임 약 570ms 였다. 그 8배는 전부 지연이다.
		// (계산은 DesktopStreamingPreset::StreamProfile)
		DesktopStreamingPreset::StreamProfile streamProfile;
		streamProfile.fps = TARGET_FPS;
		streamProfile.bitrateBps = TARGET_BITRATE_BPS;

		printf_s("[DesktopStreamingServer] send queue depth %u chunks (~%u ms of backlog)\n",
			DesktopStreamingPreset::SendQueueDepth(streamProfile),
			streamProfile.backlogMs);

		if (!m_streamingServer->StartServer("0.0.0.0", 27015, 64,
			DesktopStreamingPreset::ServerSessionBuffer(streamProfile),
			DesktopStreamingPreset::ServerPool(),
			DesktopStreamingPreset::ServerHeartbeat()))
		{
			printf_s("[DesktopStreamingServer] Failed to start the streaming server on 0.0.0.0:27015.\n");
			Shutdown();
			return false;
		}

		SyncStreamSelfHealing();

		// 엔코드 워커는 엔코더가 소유한다. 콜백은 InitializeEncoder 가 건다.
		if (!m_nvEncoder->StartEncodeThread())
		{
			printf_s("[DesktopStreamingServer] Failed to start the encode thread.\n");
			Shutdown();
			return false;
		}

		m_duplicateEngine->SetTargetFps(fps);
		m_duplicateEngine->SetFrameCaptureCallback(FrameCallback, this);

		if (!m_duplicateEngine->StartThread())
		{
			Shutdown();
			return false;
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

			// 주기 지표. 이게 없으면 "화면이 안 나온다" 에서 캡처 / 인코딩 /
			// 전송 중 어디가 막혔는지 알 방법이 없다.
			const ULONGLONG now = ::GetTickCount64();

			// 해상도가 바뀌었으면 인코드 파이프라인을 다시 만든다.
			// 캡처 스레드가 세운 표시를 여기서 처리한다.
			ServiceStreamRebuild();

			// 혼잡 신호를 보고 비트레이트를 조정한다.
			ServiceBitrateControl(now);

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

		// 큐를 먼저 닫아 워커를 깨우고, 그 다음 캡처를 멈춘다.
		// (Destroy 도 워커를 멈추지만 순서를 명시해 둔다)
		if (m_nvEncoder)
		{
			m_nvEncoder->StopEncodeThread();
		}

		if (m_duplicateEngine)
		{
			m_duplicateEngine->StopThread();
		}

		// 인코더를 캡처 엔진보다 먼저 정리한다.
		// Destroy 가 유입 큐를 닫으면서 대기 중이던 프레임을 돌려주는데,
		// 그 반납이 캡처 엔진을 거치므로 엔진이 아직 살아 있어야 한다.
		if (m_nvEncoder)
		{
			m_nvEncoder->Destroy();
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
			delete m_nvEncoder;
			m_nvEncoder = nullptr;
		}

		if (m_encodeEngine)
		{
			delete m_encodeEngine;
			m_encodeEngine = nullptr;
		}

		if (m_D3D11Engine)
		{
			delete m_D3D11Engine;
			m_D3D11Engine = nullptr;
		}

	}

	void PrintStats()
	{
		CaptureStats captureStats = {};
		if (m_duplicateEngine)
			captureStats = m_duplicateEngine->GetStats();

		NvEncStats encodeStats = {};
		if (m_nvEncoder)
			m_nvEncoder->GetStats(encodeStats);

		DesktopStreamingServerStats serverStats = {};
		if (m_streamingServer)
			m_streamingServer->GetStats(serverStats);

		const uint64_t queueDrops = encodeStats.droppedInputQueue;

		// 캡처 상태와 타임아웃을 같이 찍는다.
		//
		// 이게 없으면 cap=0 이 세 가지 중 무엇인지 구분되지 않는다 —
		// 화면이 안 변해서 건너뛴 것인지(skip), duplication 이 아무것도
		// 주지 않는 것인지(timeout), 접근을 잃은 것인지(accessLost).
		// 처방이 셋 다 다르다.
		const char* captureState = "?";
		if (m_duplicateEngine)
		{
			switch (m_duplicateEngine->GetCaptureState())
			{
			case CaptureState::Idle:         captureState = "idle"; break;
			case CaptureState::Running:      captureState = "run"; break;
			case CaptureState::Reconnecting: captureState = "recon"; break;
			case CaptureState::Faulted:      captureState = "fault"; break;
			default: break;
			}
		}

		printf_s(
			"[stats] capture %s cap=%llu skip=%llu drop=%llu timeout=%llu lost=%llu | "
			"encode sub=%llu done=%llu lost=%llu pend=%u qdrop=%llu faulted=%d noslot=%llu prep=%llu subfail=%llu | "
			"net viewers=%u offer=%llu deliver=%llu novw=%llu abort=%llu chunkfail=%llu incomplete=%llu idr=%llu | "
			"fb reports=%llu vwDiscard=%llu vwDecDrop=%llu rate=%.1fMbps\n",
			captureState,
			captureStats.capturedFrames, captureStats.skippedFrames, captureStats.droppedFrames,
			captureStats.timeoutCount, captureStats.accessLostCount,
			encodeStats.submittedFrames, encodeStats.completedFrames, encodeStats.lostFrames,
			encodeStats.pendingFrames, queueDrops, encodeStats.faulted ? 1 : 0,
			encodeStats.droppedNoEncoderSlot, encodeStats.droppedPrepareFailed, encodeStats.droppedSubmitFailed,
			serverStats.subscribedViewerCount, serverStats.framesOffered, serverStats.framesDelivered,
			serverStats.framesSkippedNoViewer, serverStats.framesAborted, serverStats.chunksFailed,
			serverStats.viewerFrameIncomplete, serverStats.keyframesForced,
			serverStats.feedbackReports, serverStats.viewerFramesDiscarded,
			serverStats.viewerDecodeDropped, m_currentBitrateBps / 1'000'000.0);

		// --- 캡처 정지 감지 ---
		//
		// 캡처 스레드가 게이트에서 영구히 막히는 교착이 있다(TARGET_FPS
		// 주석 참고). 그때 겉으로 드러나는 것이 아무것도 없다 — 상태는
		// Running 이고, 로그도 오류도 없고, 그저 숫자가 멈출 뿐이다.
		// 그걸 사람이 눈으로 비교해서 알아채야 했다.
		//
		// 여기서는 직전 주기와 비교해서 말로 한다. 조건은 세 가지가
		// 모두 참일 때다.
		//   - 캡처 상태가 Running (faulted 나 reconnecting 이 아니다)
		//   - 구독자가 있다 (없으면 캡처를 건너뛰는 것이 정상이다)
		//   - capturedFrames 가 한 주기 동안 한 장도 늘지 않았다
		//
		// 화면이 완전히 정지한 경우와 구분하기 위해 timeout 도 같이 본다.
		// 진짜 정지 화면이면 AcquireNextFrame 이 타임아웃을 쌓으므로
		// timeout 은 늘어난다. 교착이면 그것마저 멈춘다.
		const bool captureRunning = m_duplicateEngine &&
			m_duplicateEngine->GetCaptureState() == CaptureState::Running;

		if (captureRunning && serverStats.subscribedViewerCount > 0 &&
			captureStats.capturedFrames == m_lastCapturedFrames &&
			captureStats.timeoutCount == m_lastCaptureTimeouts &&
			m_statsPrintCount > 0)
		{
			++m_captureStallTicks;

			printf_s("[capture] *** STALLED *** no captured frame and no timeout for %llu stats interval(s). "
				"the capture thread is most likely blocked on the immediate context gate "
				"(see TARGET_FPS comment). lower TARGET_FPS or give the encoder its own device.\n",
				static_cast<unsigned long long>(m_captureStallTicks));
		}
		else
		{
			m_captureStallTicks = 0;
		}

		m_lastCapturedFrames = captureStats.capturedFrames;
		m_lastCaptureTimeouts = captureStats.timeoutCount;
		++m_statsPrintCount;

		// 콘솔이 파이프로 리디렉션되면 stdout 이 완전 버퍼링으로 바뀐다.
		// 그러면 이 줄들이 4KB 가 찰 때까지 파일에 나타나지 않고, 프로세스를
		// 강제 종료하면 통째로 사라진다. 주기 지표는 제때 보여야 쓸모가 있다.
		::fflush(stdout);
	}

private:
	static constexpr ULONGLONG STATS_INTERVAL_MS = 5'000;

	// fault 재생성 한도. 창 안에서 이만큼 넘게 다시 만들어야 했다면
	// 일시적 오류가 아니라고 보고 멈춘다.
	static constexpr uint32_t MAX_FAULT_REBUILDS = 3;
	static constexpr ULONGLONG FAULT_REBUILD_WINDOW_MS = 60'000;

	// 비트레이트 조정 주기. 너무 짧으면 순간적인 지터에 반응하고,
	// 너무 길면 혼잡이 길게 이어진다.
	static constexpr ULONGLONG BITRATE_INTERVAL_MS = 2'000;

	// 이만큼 연속으로 깨끗해야 비트레이트를 올린다.
	static constexpr uint32_t CLEAN_WINDOWS_TO_RAISE = 3;

	// 더 내려가면 화면을 알아볼 수 없다. QHD 기준 하한.
	static constexpr uint32_t MIN_BITRATE_BPS = 3'000'000;

	static void FrameCallback(void* userData)
	{
		DesktopStreamingServerApp* self = static_cast<DesktopStreamingServerApp*>(userData);
		if (self)
		{
			self->OnFrameCallback();
		}
	}

	static void ReleaseCapturedFrameCallback(NvEncInputFrame& inputFrame, void* userData)
	{
		DesktopStreamingServerApp* self = static_cast<DesktopStreamingServerApp*>(userData);
		if (self)
		{
			self->ReleaseCapturedFrame(inputFrame);
		}
	}

	// 인코드 스레드가 매 프레임 묻는다 (EncodeThread::QueryKeyFrameRequest).
	//
	// IDR 강제를 소비하는 자리는 여기 하나뿐이다. 예전에는 OnFrameCallback
	// 도 같은 질문을 해서 EnqueueFrame 의 forceKeyFrame 으로 넘겼는데,
	// 두 가지가 나빴다.
	//
	//   큐가 latest-only 라 그 플래그를 단 프레임이 다음 프레임에 밀려
	//   버려지면 요청도 같이 사라졌다. 이쪽 경로는 인코더의
	//   RequestKeyFrame() 이 받아 두므로 제출될 때까지 남는다.
	//
	//   그리고 최소 간격을 두는 지금은 토큰이 하나다. 두 곳에서 물으면
	//   한쪽이 토큰을 가져가고 다른 쪽은 못 받는데, 못 받은 쪽이 실제로
	//   인코딩되는 경로일 수 있다.
	static bool KeyFrameRequestCallback(void* userData)
	{
		DesktopStreamingServerApp* self = static_cast<DesktopStreamingServerApp*>(userData);
		return self ? self->ShouldForceKeyFrame() : false;
	}

	static void EncodedPacketCallback(const NvEncPacket& packet, void* userData)
	{
		DesktopStreamingServerApp* self = static_cast<DesktopStreamingServerApp*>(userData);
		if (self)
		{
			self->OnEncodedFrame(packet);
		}
	}

	static void CaptureEventCallback(CaptureEventCode code, HRESULT hr, void* userData)
	{
		DesktopStreamingServerApp* self = static_cast<DesktopStreamingServerApp*>(userData);
		if (self)
		{
			self->OnCaptureEvent(code, hr);
		}
	}

	static void EncoderErrorCallback(NvEncErrorCode errorCode, void* userData)
	{
		DesktopStreamingServerApp* self = static_cast<DesktopStreamingServerApp*>(userData);
		if (self)
		{
			self->OnEncoderError(errorCode);
		}
	}

	// 캡처 스레드에서 불린다. 블로킹 작업을 하면 안 되므로 기록만 한다.
	void OnCaptureEvent(CaptureEventCode code, HRESULT hr)
	{
		const char* name = "unknown";
		switch (code)
		{
		case CaptureEventCode::AccessLost:      name = "access lost"; break;
		case CaptureEventCode::Reconnecting:    name = "reconnecting"; break;
		case CaptureEventCode::Reconnected:     name = "reconnected"; break;
		case CaptureEventCode::ModeChanged:     name = "mode changed"; break;
		case CaptureEventCode::DeviceRemoved:   name = "device removed"; break;
		case CaptureEventCode::DeviceRecreated: name = "device recreated"; break;
		case CaptureEventCode::Faulted:         name = "faulted"; break;
		default: break;
		}

		printf_s("[capture] %s (hr=0x%08X)\n", name, static_cast<unsigned int>(hr));

		// 해상도가 바뀌거나 디바이스가 다시 만들어지면 인코더가 들고 있는
		// 크기와 공유 풀 핸들이 전부 무효다. 여기서 다시 만드는 것이
		// 맞지만, 이 콜백은 캡처 스레드에서 불린다 — 그 안에서 인코더를
		// Destroy 하면 인코드 스레드의 종료를 캡처 스레드가 기다리게 된다.
		//
		// 그래서 표시만 남기고 실제 재구성은 앱 루프가 한다.
		// (ServiceStreamRebuild)
		if (code == CaptureEventCode::ModeChanged ||
			code == CaptureEventCode::DeviceRecreated)
		{
			printf_s("[capture] output changed. the encode pipeline will be rebuilt.\n");
			::InterlockedExchange(&m_streamRebuildPending, TRUE);
			return;
		}

		// 복구 불가. 캡처 스레드가 유휴로 떨어지므로 더 할 수 있는 것이 없다.
		if (code == CaptureEventCode::Faulted)
		{
			printf_s("[capture] unrecoverable. stopping.\n");
			RequestStop();
		}
	}

	// --- 해상도 변경 대응 ---
	//
	// 앱 스레드에서만 부른다. 캡처 스레드가 표시를 세우고 여기서 실제
	// 작업을 한다.
	//
	// 순서가 중요하다.
	//   1. 인코드 워커를 멈춘다. 이게 살아 있으면 무효가 된 공유 슬롯을
	//      계속 읽는다.
	//   2. 인코더를 부순다. 등록된 공유 텍스처도 같이 놓는다.
	//   3. 새 크기로 인코더를 다시 만들고 공유 풀을 다시 연다.
	//      캡처 엔진이 풀을 새로 만들었으므로 핸들도 전부 새것이다.
	//   4. 서버의 스트림 정보를 갱신하고 뷰어들에게 알린다.
	//      이걸 빼먹으면 뷰어는 옛 크기를 믿은 채 새 비트스트림을 받는다.

	// 인코더를 만들고 공유 입력 풀을 연다.
	//
	// Initialize 와 해상도 변경 재구성(ServiceStreamRebuild)이 같이 쓴다.
	// 예전에는 이 코드가 Initialize 안에만 있어서, 해상도가 바뀌면
	// 재구성할 방법이 없었다.
	//
	// 실패해도 여기서 Shutdown 하지 않는다. 호출부가 자기 맥락에 맞게
	// 정리한다 — 기동 실패면 전체 종료, 재구성 실패면 스트림만 멈춘다.

	// --- 비트레이트 적응 ---
	//
	// 앱 스레드에서 주기적으로 부른다.
	//
	// 무엇을 보는가
	//   서버가 아는 신호와 뷰어가 알려 준 신호, 둘 다 본다.
	//
	//     chunksFailed    : 뷰어 송신 큐가 찼다 (네트워크/피어가 못 받는다)
	//     frameIncomplete : 프레임을 완성하지 못한 뷰어가 있다
	//     viewerDiscarded : 청크가 어긋나 버린 프레임 (뷰어 보고)
	//     viewerDecodeDrop: 디코더가 밀려 버린 프레임 (뷰어 보고)
	//
	//   앞의 둘만으로는 부족하다. 네트워크가 멀쩡해도 디코더가 못 따라가면
	//   화면은 똑같이 끊기는데, 그건 뷰어만 안다. 그래서 피드백 채널을
	//   만들었다.
	//
	// 어떻게 움직이는가
	//   혼잡 신호가 하나라도 있으면 즉시 내린다(x0.75). 올릴 때는 깨끗한
	//   구간이 연속 3번 이어져야 하고 한 번에 조금만 올린다(x1.15).
	//
	//   내릴 때 빠르고 올릴 때 느린 것이 정석이다. 반대로 하면 경계에서
	//   오르내리기를 반복하면서 화질이 계속 출렁인다.
	//
	//   forceIdr 은 false 다. 비트레이트만 바꾸는데 IDR 을 강제하면 그
	//   프레임이 P 프레임의 몇 배가 되어 방금 줄이려던 혼잡을 악화시킨다.
	//   (D3D11NvEncoder::Reconfigure 주석의 조언 그대로다)
	void ServiceBitrateControl(ULONGLONG now)
	{
		if (!m_nvEncoder || !m_streamingServer)
			return;

		if (now < m_nextBitrateTick)
			return;

		m_nextBitrateTick = now + BITRATE_INTERVAL_MS;

		DesktopStreamingServerStats stats = {};
		m_streamingServer->GetStats(stats);

		// 구독자가 없으면 조정할 근거가 없다. 기준만 맞춰 두고 넘어간다.
		if (stats.subscribedViewerCount == 0)
		{
			m_lastChunksFailed = stats.chunksFailed;
			m_lastViewerFrameIncomplete = stats.viewerFrameIncomplete;
			m_lastViewerDiscarded = stats.viewerFramesDiscarded;
			m_lastViewerDecodeDropped = stats.viewerDecodeDropped;
			return;
		}

		const uint64_t chunkFailDelta = stats.chunksFailed - m_lastChunksFailed;
		const uint64_t incompleteDelta = stats.viewerFrameIncomplete - m_lastViewerFrameIncomplete;
		const uint64_t discardDelta = stats.viewerFramesDiscarded - m_lastViewerDiscarded;
		const uint64_t decodeDropDelta = stats.viewerDecodeDropped - m_lastViewerDecodeDropped;

		m_lastChunksFailed = stats.chunksFailed;
		m_lastViewerFrameIncomplete = stats.viewerFrameIncomplete;
		m_lastViewerDiscarded = stats.viewerFramesDiscarded;
		m_lastViewerDecodeDropped = stats.viewerDecodeDropped;

		const bool congested =
			chunkFailDelta > 0 || incompleteDelta > 0 || discardDelta > 0 || decodeDropDelta > 0;

		uint32_t nextBitrate = m_currentBitrateBps;

		if (congested)
		{
			m_cleanWindows = 0;
			nextBitrate = static_cast<uint32_t>(m_currentBitrateBps * 3ull / 4ull);
			if (nextBitrate < MIN_BITRATE_BPS)
				nextBitrate = MIN_BITRATE_BPS;
		}
		else
		{
			++m_cleanWindows;
			if (m_cleanWindows < CLEAN_WINDOWS_TO_RAISE)
				return;

			m_cleanWindows = 0;

			if (m_currentBitrateBps >= TARGET_BITRATE_BPS)
				return;

			nextBitrate = static_cast<uint32_t>(m_currentBitrateBps * 115ull / 100ull);
			if (nextBitrate > TARGET_BITRATE_BPS)
				nextBitrate = TARGET_BITRATE_BPS;
		}

		if (nextBitrate == m_currentBitrateBps)
			return;

		// [runtime] 필드만 바꾼다. 현재 설정을 받아 와서 비트레이트만 고치고
		// 되돌려주는 것이 안전하다 — [init] 이 하나라도 다르면 거절당한다.
		NvEncConfig config = {};
		m_nvEncoder->GetConfig(config);
		config.averageBitrateBps = nextBitrate;
		config.maxBitrateBps = nextBitrate + (nextBitrate / 4);

		// VBV 는 0 으로 두면 인코더가 새 비트레이트 기준으로 다시 계산한다.
		// 옛 값을 그대로 두면 비트레이트와 어긋난 채로 남는다.
		config.vbvBufferSizeBits = 0;

		const NvEncReconfigureResult result = m_nvEncoder->Reconfigure(config, false);
		if (result != NvEncReconfigureResult::Applied && result != NvEncReconfigureResult::NoChange)
		{
			printf_s("[bitrate] reconfigure rejected (result %u). keeping %.1f Mbps\n",
				static_cast<unsigned int>(result), m_currentBitrateBps / 1'000'000.0);
			return;
		}

		printf_s("[bitrate] %.1f -> %.1f Mbps (%s : chunkFail %llu, incomplete %llu, discard %llu, decodeDrop %llu)\n",
			m_currentBitrateBps / 1'000'000.0, nextBitrate / 1'000'000.0,
			congested ? "congested" : "recovering",
			static_cast<unsigned long long>(chunkFailDelta),
			static_cast<unsigned long long>(incompleteDelta),
			static_cast<unsigned long long>(discardDelta),
			static_cast<unsigned long long>(decodeDropDelta));

		m_currentBitrateBps = nextBitrate;
	}
	bool InitializeEncoder(uint32_t width, uint32_t height)
	{
		if (!m_nvEncoder || !m_encodeEngine || !m_duplicateEngine)
			return false;

		// 설정을 직접 채워서 넘긴다.
		//
		// 예전에는 짧은 형태를 썼다.
		//
		//   m_nvEncoder->Initialize(device, w, h, 4, gate);
		//
		// 그 오버로드는 width / height / encodeSlotCount / async 네 개만
		// 채우고 나머지는 NvEncConfig 의 기본값을 쓴다. 그래서
		// frameRateNumerator 가 60 으로 남았는데, 캡처는 SetTargetFps(30)
		// 으로 돌고 있었다.
		//
		// 레이트 컨트롤러에게 프레임률은 장식이 아니다. CBR 의 한 프레임
		// 예산은 averageBitrate / frameRate 이고, VBV 버퍼도 그 값으로
		// 잡힌다(D3D11NvEncoder_Impl 의 ComputeSingleFrameVbvBits).
		// 60 이라고 말해 놓고 30 을 넣으면 인코더는 매 프레임 예산의 절반만
		// 쓰므로, 설정한 5Mbps 가 실제로는 2.5Mbps 로 동작한다.
		//
		// 이제 fps 는 TARGET_FPS 한 곳에서 나온다 — 캡처, 인코더,
		// 스트림 정보가 전부 같은 값을 쓴다.
		NvEncConfig encodeConfig;
		encodeConfig.width = width;
		encodeConfig.height = height;
		encodeConfig.encodeSlotCount = 4;
		encodeConfig.enableAsyncPipeline = true;

		// 원격 화면 공유다. 지연이 화질보다 우선한다.
		// UltraLow 는 룩어헤드와 B프레임을 끄고, 덤으로
		// lowDelayKeyFrameScale=1 이 되어 IDR 도 한 프레임 예산을 넘지 않는다.
		encodeConfig.latencyMode = NvEncLatencyMode::UltraLow;
		encodeConfig.profile = NvEncH264Profile::High;

		encodeConfig.frameRateNumerator = TARGET_FPS;
		encodeConfig.frameRateDenominator = 1;

		encodeConfig.rateControl = NvEncRateControl::ConstantBitrate;
		encodeConfig.averageBitrateBps = TARGET_BITRATE_BPS;
		encodeConfig.maxBitrateBps = MAX_BITRATE_BPS;

		// intra refresh 를 켜면 인코더가 idrPeriod 를 무한으로 둔다(SDK 제약).
		// 주기적 IDR 이 없는 대신 매 주기마다 화면을 조금씩 갱신하므로,
		// 손실 후 회복이 IDR 스파이크 없이 일어난다.
		//
		// 주기를 TARGET_FPS 로 두면 1초다. StreamingServer 의 IDR 강제
		// 최소 간격과 같은 값이며, 그건 우연이 아니라 맞춘 것이다.
		encodeConfig.enableIntraRefresh = true;
		encodeConfig.intraRefreshPeriodFrames = TARGET_FPS;

		// 인코더 전용 디바이스와 그 디바이스의 게이트를 넘긴다.
		// 캡처 디바이스(m_D3D11Engine)가 아니다.
		if (!m_nvEncoder->Initialize(
			m_encodeEngine->GetD3DDevice(),
			encodeConfig,
			m_encodeEngine->GetImmediateContextGate()))
		{
			printf_s("[DesktopStreamingServer] Failed to initialize the encoder (%ux%u @%u fps, %u bps).\n",
				width, height, TARGET_FPS, TARGET_BITRATE_BPS);
			return false;
		}

		// 캡처 풀을 인코더 디바이스에서 한 번만 열어 둔다.
		//
		// 매 프레임 OpenSharedResource 를 하면 그 자체가 드라이버 왕복이라
		// 디바이스를 나눈 이득을 도로 까먹는다. 여기서 끝내고, 이후에는
		// 슬롯 번호만 큐로 넘긴다.
		{
			const uint32_t poolCount = m_duplicateEngine->GetFramePoolCount();
			HANDLE sharedHandles[16] = {};

			if (poolCount == 0 || poolCount > _countof(sharedHandles))
			{
				printf_s("[DesktopStreamingServer] unexpected capture pool count %u.\n", poolCount);
				// 호출부가 정리한다.
				return false;
			}

			for (uint32_t i = 0; i < poolCount; ++i)
			{
				sharedHandles[i] = m_duplicateEngine->GetFramePoolSharedHandle(i);
				if (!sharedHandles[i])
				{
					printf_s("[DesktopStreamingServer] capture pool slot %u has no shared handle.\n", i);
					// 호출부가 정리한다.
					return false;
				}
			}

			if (!m_nvEncoder->RegisterSharedInputPool(sharedHandles, poolCount))
			{
				printf_s("[DesktopStreamingServer] Failed to register the shared capture pool with the encoder.\n");
				// 호출부가 정리한다.
				return false;
			}
		}

		// 인코더가 새로 만들어졌으므로 콜백도 다시 건다.
		// 유입 큐도 이때 새로 만들어지므로 반납 콜백이 특히 중요하다 —
		// 빠뜨리면 버려진 프레임의 캡처 슬롯이 영영 반납되지 않는다.
		m_nvEncoder->SetFrameReleaseCallback(ReleaseCapturedFrameCallback, this);
		m_nvEncoder->SetKeyFrameRequestCallback(KeyFrameRequestCallback, this);
		m_nvEncoder->SetEncodedPacketCallback(EncodedPacketCallback, this);
		m_nvEncoder->SetErrorCallback(EncoderErrorCallback, this);

		return true;
	}
	// 인코더가 intra refresh 로 도는지 서버에 알린다.
	//
	// 이 값 하나로 "프레임을 놓친 뷰어에게 IDR 을 요구할 것인가" 가 갈린다.
	// 하드코딩하지 않고 인코더에게 묻는 이유는, 설정을 바꿨을 때 두 곳이
	// 따로 놀지 않게 하기 위해서다. 인코더를 다시 만들 때도 같이 부른다.
	void SyncStreamSelfHealing()
	{
		if (!m_nvEncoder || !m_streamingServer)
			return;

		NvEncConfig activeConfig;
		m_nvEncoder->GetConfig(activeConfig);
		m_streamingServer->SetStreamSelfHealing(activeConfig.enableIntraRefresh);

		printf_s("[stream] self-healing %s (encoder intra refresh)\n",
			activeConfig.enableIntraRefresh ? "on" : "off");
	}

	bool ServiceStreamRebuild()
	{
		if (::InterlockedExchange(&m_streamRebuildPending, FALSE) == FALSE)
			return true;

		if (!m_duplicateEngine || !m_nvEncoder || !m_streamingServer || !m_encodeEngine)
			return false;

		const bool fromFault = (::InterlockedExchange(&m_rebuildFromFault, FALSE) == TRUE);

		const uint32_t newWidth = m_duplicateEngine->GetOutputWidth();
		const uint32_t newHeight = m_duplicateEngine->GetOutputHeight();
		if (newWidth == 0 || newHeight == 0)
		{
			printf_s("[rebuild] capture output size is not available yet. retrying next tick.\n");
			if (fromFault)
				::InterlockedExchange(&m_rebuildFromFault, TRUE);
			::InterlockedExchange(&m_streamRebuildPending, TRUE);
			return true;
		}

		// fault 로 온 재생성은 한없이 반복하지 않는다. 창 안에서 한도를 넘으면
		// 다시 만들어도 같은 이유로 죽는다는 뜻이므로 거기서 멈춘다.
		// 창보다 오래 멀쩡히 돌았다면 일시적인 오류로 보고 처음부터 센다.
		if (fromFault)
		{
			const ULONGLONG now = ::GetTickCount64();
			if (m_lastFaultRebuildTick != 0 && (now - m_lastFaultRebuildTick) > FAULT_REBUILD_WINDOW_MS)
				m_faultRebuildCount = 0;

			m_lastFaultRebuildTick = now;
			++m_faultRebuildCount;

			if (m_faultRebuildCount > MAX_FAULT_REBUILDS)
			{
				printf_s("[rebuild] the encoder faulted %u times within %llu s. giving up.\n",
					m_faultRebuildCount, FAULT_REBUILD_WINDOW_MS / 1000ULL);
				RequestStop();
				return false;
			}

			printf_s("[rebuild] encoder fault recovery (%u/%u). %ux%u\n",
				m_faultRebuildCount, MAX_FAULT_REBUILDS, newWidth, newHeight);
		}
		else
		{
			printf_s("[rebuild] %ux%u -> %ux%u\n", m_streamWidth, m_streamHeight, newWidth, newHeight);
		}

		m_nvEncoder->StopEncodeThread();
		m_nvEncoder->Destroy();

		if (!InitializeEncoder(newWidth, newHeight))
		{
			printf_s("[rebuild] failed to re-create the encoder. stopping.\n");
			RequestStop();
			return false;
		}

		if (!m_nvEncoder->StartEncodeThread())
		{
			printf_s("[rebuild] failed to restart the encode thread. stopping.\n");
			RequestStop();
			return false;
		}

		SyncStreamSelfHealing();

		m_streamWidth = static_cast<uint16_t>(newWidth);
		m_streamHeight = static_cast<uint16_t>(newHeight);

		m_streamingServer->SetStreamInfo(m_streamWidth, m_streamHeight, TARGET_FPS, DESKTOP_STREAM_CODEC_TYPE::H264);
		const uint32_t notified = m_streamingServer->BroadcastStreamInfo();

		printf_s("[rebuild] done. %u viewer(s) notified.\n", notified);
		return true;
	}

	// 엔코더 완료 스레드에서 불린다.
	void OnEncoderError(NvEncErrorCode errorCode)
	{
		const char* name = "unknown";
		bool fatal = true;

		switch (errorCode)
		{
		case NvEncErrorCode::OutputReadFailed:   name = "output read failed"; fatal = false; break;
		case NvEncErrorCode::OutputTimeout:      name = "output timeout"; break;
		case NvEncErrorCode::OutputUnmapFailed:  name = "output unmap failed"; break;
		case NvEncErrorCode::SlotRingCorrupted:  name = "pending ring corrupted"; break;
		case NvEncErrorCode::EncoderFaulted:     name = "encoder faulted"; break;
		default: break;
		}

		printf_s("[encode] %s%s\n", name, fatal ? " (rebuilding the encoder)" : " (frame lost, continuing)");

		if (!fatal)
			return;

		// 여기서 인코더를 직접 손대면 안 된다. 이 콜백은 완료 스레드에서
		// 불리는데, 재생성은 Destroy 로 그 스레드를 join 하므로 자기 자신을
		// 기다리게 된다. 표시만 세우고 앱 루프가 실제 작업을 한다.
		//
		// 그 사이에 들어오는 프레임은 새지 않는다. faulted 인코더는
		// CanSubmitFrame 이 false 라 엔코드 스레드가 큐에서 꺼내 버리고,
		// 버릴 때 반납 콜백으로 캡처 슬롯을 돌려준다.
		::InterlockedExchange(&m_rebuildFromFault, TRUE);
		::InterlockedExchange(&m_streamRebuildPending, TRUE);
	}

	void OnFrameCallback()
	{
		if (!m_duplicateEngine || !m_nvEncoder || !m_streamingServer)
			return;

		if (!m_streamingServer->HasSubscribedViewer())
			return;

		CapturedFrameHandle frameHandle = m_duplicateEngine->GetLatestFrameHandle();

		if (!frameHandle.texture)
		{
			return;
		}

		NvEncInputFrame encodeInputFrame = {};

		// texture 를 넘기지 않는다.
		//
		// 이 포인터는 캡처 디바이스의 텍스처이고, 인코더는 다른 디바이스에
		// 있다. 남의 디바이스 텍스처를 그대로 쓰면 동작하지 않는다.
		//
		// 인코더는 slotId 로 자기가 초기화 때 열어 둔 공유 텍스처를 찾는다.
		// (EncodeThread::Run 이 texture 가 null 이면 그 경로를 탄다)
		encodeInputFrame.texture = nullptr;
		encodeInputFrame.sourceSlotId = frameHandle.slotId;
		encodeInputFrame.frameId = frameHandle.frameId;

		// 반납할 때는 원래 핸들이 필요하므로 따로 기억해 둔다.
		// (ReleaseCapturedFrame 이 texture 로 슬롯을 검증한다)
		m_captureTextureBySlot[frameHandle.slotId & (MAX_CAPTURE_SLOTS - 1)] = frameHandle.texture;

		// forceKeyFrame 은 false 다. IDR 강제를 묻는 자리는
		// KeyFrameRequestCallback 하나뿐이다. (그쪽 주석 참고)
		if (!m_nvEncoder->EnqueueFrame(encodeInputFrame, false))
		{
			m_duplicateEngine->ReleaseLatestFrameHandle(frameHandle);
			return;
		}
	}

	void ReleaseCapturedFrame(NvEncInputFrame& inputFrame)
	{
		if (m_duplicateEngine)
		{
			CapturedFrameHandle capturedFrameHandle = {};

			// 큐에는 texture 를 싣지 않았으므로(다른 디바이스 것이라) 여기서
			// 슬롯 번호로 되찾는다. 캡처 엔진은 반납을 검증할 때 texture 와
			// slotId 가 짝인지 본다.
			const int64_t slotId = inputFrame.sourceSlotId;
			capturedFrameHandle.texture = (slotId >= 0)
				? m_captureTextureBySlot[slotId & (MAX_CAPTURE_SLOTS - 1)]
				: nullptr;
			capturedFrameHandle.slotId = static_cast<LONG>(slotId);
			capturedFrameHandle.frameId = inputFrame.frameId;

			m_duplicateEngine->ReleaseLatestFrameHandle(capturedFrameHandle);

			inputFrame.texture = nullptr;
			inputFrame.sourceSlotId = -1;
			inputFrame.frameId = 0ULL;
		}
	}

	bool ShouldForceKeyFrame()
	{
		return m_streamingServer && m_streamingServer->ShouldForceKeyFrame();
	}

	void OnEncodedFrame(const NvEncPacket& frame)
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

private:
	// 콘솔 핸들러 스레드가 RequestStop 으로 내리고 메인 루프가 읽는다.
	// 평범한 bool 이었다 — 두 스레드가 보는 값이라 원자 연산을 쓴다.
	volatile LONG m_running = FALSE;

	ULONGLONG m_nextStatsTick = 0;

	// 비트레이트 적응 상태. 앱 스레드에서만 만진다.
	ULONGLONG m_nextBitrateTick = 0;
	uint32_t m_currentBitrateBps = TARGET_BITRATE_BPS;
	uint32_t m_cleanWindows = 0;
	uint64_t m_lastChunksFailed = 0;
	uint64_t m_lastViewerFrameIncomplete = 0;
	uint64_t m_lastViewerDiscarded = 0;
	uint64_t m_lastViewerDecodeDropped = 0;

	// 인코더 재생성 표시. 워커 스레드가 세우고 앱 루프가 내린다.
	// 해상도 변경(캡처 스레드)과 인코더 fault(완료 스레드)가 같이 쓴다.
	volatile LONG m_streamRebuildPending = FALSE;
	volatile LONG m_rebuildFromFault = FALSE;

	// fault 로 인한 재생성 횟수. 앱 루프만 만진다.
	// 창 안에서 한도를 넘으면 계속 되살려도 소용없다고 보고 포기한다.
	uint32_t m_faultRebuildCount = 0;
	uint64_t m_lastFaultRebuildTick = 0;

	// 지금 스트림 정보의 크기. 재구성 때 비교용이다.
	uint16_t m_streamWidth = 0;
	uint16_t m_streamHeight = 0;

	// 캡처 정지 감지용. 메인 루프(PrintStats)만 만진다.
	uint64_t m_lastCapturedFrames = 0;
	uint64_t m_lastCaptureTimeouts = 0;
	uint64_t m_captureStallTicks = 0;
	uint64_t m_statsPrintCount = 0;

	D3D11RenderEngine* m_D3D11Engine = nullptr;

	// 인코더 전용 디바이스. 캡처와 컨텍스트를 나누기 위한 것이다.
	D3D11RenderEngine* m_encodeEngine = nullptr;

	// 슬롯 번호 -> 캡처 텍스처. 큐에는 슬롯만 싣고 반납할 때 여기서 되찾는다.
	// 캡처 스레드가 쓰고 인코드 스레드가 읽지만, 슬롯당 한 번 쓰고 그 슬롯이
	// 반납될 때까지 바뀌지 않으므로 경합이 없다.
	static constexpr int64_t MAX_CAPTURE_SLOTS = 16;
	ID3D11Texture2D* m_captureTextureBySlot[MAX_CAPTURE_SLOTS] = {};
	D3D11DuplicateEngine* m_duplicateEngine = nullptr;
	D3D11NvEncoder* m_nvEncoder = nullptr;
	StreamingServer* m_streamingServer = nullptr;
};
