#pragma once

// 이 서비스가 엔진에 넘기는 설정.
//
// 엔진은 역할별 기본 프리셋(SessionBufferPreset::Server / Client,
// EnginePoolPreset::...)을 주고, 그 주석에서 "서비스가 자기 값으로 조정하면
// 된다" 고 말한다. 여기가 그 자리다. 예전에는 아무도 조정하지 않아서 양쪽
// 모두 범용 기본값으로 돌고 있었고, 그 기본값은 실시간 스트리밍을 가정하지
// 않는다.
//
// 가장 크게 어긋나 있던 것은 세 가지다.
//
//   1) 송신 큐 깊이 4096
//      청크 하나가 32KB 이므로 세션 하나가 최대 128MB 를 붙들 수 있다는
//      뜻이다. 그런데 그 128MB 는 전부 '오래된 화면' 이다. 느린 뷰어는
//      메모리를 쌓으면서 점점 더 과거를 보게 된다.
//
//      실시간 스트리밍에서 밀린 프레임은 값어치가 없다. 쌓지 말고 버리고,
//      다음 키프레임부터 다시 맞추는 편이 낫다. 그 동작은 이미
//      BroadcastEncodedFrame 에 있다(전부-아니면-전무 + 대기 표시 복원).
//      큐를 얕게 잡아야 그 경로가 제때 작동한다.
//
//   2) 클라이언트 수신 상한 65535 vs 서버 청크 32768
//      둘이 서로를 모르는 채로 어쩌다 맞아 있었다. 이제 한 숫자에서
//      나온다. (StreamingPacket.h 의 DESKTOP_STREAM_CHUNK_PACKET_SIZE)
//
//   3) 읽지 않는 뷰어를 60초까지 봐준다
//      엔진 기본 stalledPeerTimeout 이다. 그 60초 동안 서버는 그 뷰어
//      몫의 청크를 계속 만들고 계속 실패한다. 스트리밍에서 20초를 내리
//      읽지 않는 상대는 이미 뷰어가 아니다.

#include "StreamingPacket.h"

#include "../../../../Module/IOCPNetworkEngine/Buffer/SessionBufferConfig.h"
#include "../../../../Module/IOCPNetworkEngine/Memory/EnginePoolConfig.h"
#include "../../../../Module/IOCPNetworkEngine/Core/HeartbeatConfig.h"

namespace DesktopStreamingPreset
{
	// --- 스트림의 실제 모양 ---
	//
	// 큐 깊이를 정하려면 "프레임 하나가 몇 청크인가" 를 알아야 한다.
	//
	// 예전에는 그걸 DESKTOP_STREAM_MAX_CHUNK_COUNT(17) 로 잡았다. 그건
	// 프로토콜 상한(512KB)에서 나온 수이고, 실제 프레임과는 관계가 없다.
	// QHD 60fps 20Mbps 면 한 프레임이 약 42KB = 2청크다. 그래서 "4프레임
	// 분량" 이라고 적어 둔 68엔트리가 실제로는 34프레임, 약 570ms 였다.
	//
	// 의도한 것의 여덟 배를 붙들고 있었던 셈이고, 그 8배는 전부 지연이다.
	// 실시간 스트리밍에서 붙들린 프레임은 값어치가 없다.
	//
	// 그래서 상한이 아니라 실제 비트레이트에서 계산한다.
	struct StreamProfile
	{
		uint16_t fps = 60;
		uint32_t bitrateBps = 20'000'000;

		// 한 프레임이 평균의 몇 배까지 커질 수 있는가.
		//
		// CBR + 단일 프레임 VBV + UltraLow(lowDelayKeyFrameScale=1) 이면
		// IDR 도 평균 예산을 크게 넘지 않는다. 그래도 VBV 는 목표이지
		// 절대 상한이 아니므로 여유를 둔다.
		//
		// 이 값이 작으면 큰 프레임이 큐에 다 들어가지 못해 중간에 포기하고
		// 키프레임을 다시 요구한다. 그 상황은 chunksFailed / keyframeRearms
		// 가 정상 운영에서 오르는 것으로 드러난다 — 그러면 이 값을 올린다.
		uint32_t peakFrameFactor = 4;

		// 밀린 뷰어를 위해 붙들어 줄 시간. 이 시간을 넘긴 화면은 버린다.
		uint32_t backlogMs = 100;
	};

	// 평균 프레임 크기(바이트).
	constexpr uint32_t AverageFrameBytes(const StreamProfile& profile)
	{
		const uint32_t fps = (profile.fps > 0) ? profile.fps : 60;
		return profile.bitrateBps / fps / 8;
	}

	// 한 프레임이 최대 몇 청크까지 될 수 있는가.
	//
	// 송신 큐가 이보다 얕으면 그 프레임은 절대 다 들어가지 못한다.
	// 전부-아니면-전무 규약 때문에 그런 프레임은 통째로 버려지고
	// 키프레임 재요구가 따라온다 — 깊이의 바닥은 여기서 나온다.
	constexpr uint32_t PeakFrameChunks(const StreamProfile& profile)
	{
		const uint32_t peakBytes = AverageFrameBytes(profile) * profile.peakFrameFactor;
		const uint32_t chunks = (peakBytes + DESKTOP_STREAM_MAX_CHUNK_DATA_SIZE - 1) / DESKTOP_STREAM_MAX_CHUNK_DATA_SIZE;
		return (chunks > 0) ? chunks : 1;
	}

	// backlogMs 동안 흘러가는 바이트가 몇 청크인가.
	constexpr uint32_t BacklogChunks(const StreamProfile& profile)
	{
		const uint64_t bytes = static_cast<uint64_t>(profile.bitrateBps) / 8ull * profile.backlogMs / 1000ull;
		const uint32_t chunks = static_cast<uint32_t>(
			(bytes + DESKTOP_STREAM_MAX_CHUNK_DATA_SIZE - 1) / DESKTOP_STREAM_MAX_CHUNK_DATA_SIZE);
		return (chunks > 0) ? chunks : 1;
	}

	// 송신 큐 깊이.
	//
	// 시간 예산과 "가장 큰 프레임 두 장" 중 큰 쪽을 쓴다. 뒤엣것이 바닥인
	// 이유는 위의 PeakFrameChunks 주석에 있다 — 한 장도 못 담는 큐는
	// 그 프레임을 영원히 버린다. 두 장인 것은 한 장이 빠져나가는 동안
	// 다음 장을 담을 수 있어야 하기 때문이다.
	constexpr uint32_t SendQueueDepth(const StreamProfile& profile)
	{
		const uint32_t byTime = BacklogChunks(profile);
		const uint32_t floor = PeakFrameChunks(profile) * 2;
		return (byTime > floor) ? byTime : floor;
	}

	// 기본 프로파일. 앱이 자기 값을 넘기지 않으면 이걸 쓴다.
	constexpr StreamProfile DefaultProfile() { return StreamProfile(); }

	// 서버 : 제어 메시지(구독/해지, 8바이트)를 받고 청크를 민다.
	inline SessionBufferConfig ServerSessionBuffer(const StreamProfile& profile = DefaultProfile())
	{
		SessionBufferConfig config = SessionBufferPreset::Server();

		// 받는 것은 구독/해지와 엔진의 시스템 패킷뿐이다. 그래도 4K 를
		// 그대로 두는 이유는, 여기를 좁혀서 버는 것이 세션당 몇 KB 인 반면
		// 엔진이 시스템 패킷을 키우면 인증이 조용히 깨지기 때문이다.
		// 아끼는 쪽보다 안 깨지는 쪽을 고른다.
		config.maxRecvPacketSize = MEMORY_SIZE_4K;
		config.recvRingSize = MEMORY_SIZE_16K;

		// 우리가 보내는 가장 큰 것이 청크 패킷이다. 이 값은 동시에
		// "클라이언트가 받아줄 수 있다고 우리가 믿는 크기" 이기도 하다.
		config.maxSendPacketSize = DESKTOP_STREAM_CHUNK_PACKET_SIZE;

		// 시간으로 잡는다. (위 StreamProfile 주석)
		//
		// QHD 60fps 20Mbps 기본 프로파일이면 backlogMs 100 -> 8청크,
		// 바닥(가장 큰 프레임 두 장) -> 12청크, 결과 12다.
		// 예전 68 은 같은 스트림에서 570ms 였다.
		config.sendQueueDepth = SendQueueDepth(profile);

		// 서버는 제어 메시지만 받으므로 잡이 쌓일 일이 없다. 백프레셔가
		// 걸린다면 그건 유입이 많은 것이 아니라 스케줄러가 멈춘 것이다.
		config.recvPauseJobDepth = 64;
		config.recvResumeJobDepth = 16;

		return config;
	}

	// 클라이언트 : 청크를 받고 제어 메시지를 민다.
	inline SessionBufferConfig ClientSessionBuffer(const StreamProfile& profile = DefaultProfile())
	{
		SessionBufferConfig config = SessionBufferPreset::Client();

		// 서버가 보내는 최대치와 같은 숫자다.
		config.maxRecvPacketSize = DESKTOP_STREAM_CHUNK_PACKET_SIZE;

		// 조각 + 최대 패킷이 동시에 들어가야 하므로 최소 2배, 권장 4배다.
		config.recvRingSize = DESKTOP_STREAM_CHUNK_PACKET_SIZE * 4;

		// 보내는 것은 구독/해지뿐이다.
		config.maxSendPacketSize = MEMORY_SIZE_4K;
		config.sendQueueDepth = 64;

		// 수신 백프레셔도 시간으로 잡는다.
		//
		// 잡 큐에 쌓인 청크는 아직 재조립조차 되지 않은 화면이다. 그게
		// 깊으면 그대로 지연이고, 뒤늦게 풀어 봐야 이미 지난 화면이다.
		//
		// 다만 송신 큐보다는 여유를 둔다. 재조립 자체는 memcpy 라 빠르고,
		// 여기서 백프레셔가 걸리면 TCP 수신 윈도가 닫혀 서버가 프레임을
		// 버리기 시작한다 — 정상 운영에서 그게 일어나면 안 된다.
		// 그래서 송신 큐 깊이의 두 배를 멈춤 수위로, 그 1/4 을 재개
		// 수위로 둔다. 기본 프로파일이면 24 / 6 이다.
		//
		// 예전에는 68 / 17 이었고, 그건 "4프레임" 이라고 적혀 있었지만
		// 실제로는 34프레임(약 570ms)이었다.
		const uint32_t pauseDepth = SendQueueDepth(profile) * 2;

		config.recvPauseJobDepth = pauseDepth;
		config.recvResumeJobDepth = (pauseDepth / 4 > 0) ? (pauseDepth / 4) : 1;

		return config;
	}

	// 메모리 풀.
	//
	// 엔진 프리셋에서 출발해 근거가 있는 항목만 바꾼다. 처음에는 빈 목록을
	// 직접 짜서 "우리가 쓰는 두 크기(32KB 청크와 수십 바이트 제어 패킷)만
	// 남긴다" 고 했는데, 그건 이 풀의 구조를 잘못 읽은 것이었다.
	//
	// 빈 사다리는 고정이다(64B ~ 32KB). 설정이 정하는 것은 어떤 빈이
	// 존재하는가가 아니라 각 빈에 블록을 몇 개 미리 잡아 둘 것인가다.
	// 목록에서 뺀 크기는 사라지지 않고 '선할당 0' 이 되어, 처음 그 크기를
	// 요구받는 순간 세그먼트를 새로 잡는다. 실제로 그렇게 됐다 —
	// 1KB 빈이 growth 1 로 기동 직후 자랐다.
	//
	// 무엇이 그 크기를 쓰는지 우리는 모른다. 엔진의 수신 경로가 패킷
	// 크기에 맞춰 블록을 꺼내므로, 서비스가 보내지 않는 크기도 엔진
	// 자신이 쓴다. 모르는 것을 지우는 것보다 남겨 두는 편이 싸다.
	inline EnginePoolConfig ServerPool()
	{
		EnginePoolConfig config = EnginePoolPreset::Server();

		// 잡 하나가 수신 패킷 하나다. 서버가 받는 것은 구독/해지와 시스템
		// 패킷뿐이라 1024 개는 쓰이지 않는다. 다만 이건 상한이 아니라
		// 출발점이므로 줄여도 부족하면 알아서 자란다.
		config.job.blockCount = 256;

		// 송신 큐 엔트리와 SharedSendPacket 이 여기서 나온다.
		// 최대 = 뷰어 수 x 큐 깊이 = 64 x 12 ≈ 800 이지만, 실제 고수위는
		// 뷰어 수가 아니라 스레드 캐시 총량이 정한다(실측 6528). tools/README.md
		// 의 풀 고수위 표 참고.
		//
		// 엔진 기본값은 65536 개다. 그건 예전에 이 값이 진짜 상한이던
		// 시절의 숫자이고(PreDefine.h 의 SEND_QUEUE_ENTRY_COUNT 주석),
		// 지금은 출발점일 뿐이라 이만큼 미리 잡을 이유가 없다.
		config.sendQueue.blockCount = 8192;

		return config;
	}

	inline EnginePoolConfig ClientPool()
	{
		EnginePoolConfig config = EnginePoolPreset::Client();

		// 보내는 것은 구독/해지뿐이다.
		config.sendQueue.blockCount = 256;

		return config;
	}

	// 하트비트 / 정체 판정.
	//
	// stalledPeerTimeout 을 60초에서 20초로 줄인다. 그 사이 서버는 그
	// 뷰어 몫의 청크를 만들고 실패하기를 반복한다 — 낭비이면서, 그 뷰어가
	// 붙들고 있는 송신 큐 엔트리가 풀에서 빠져 있는 시간이기도 하다.
	inline HeartbeatConfig ServerHeartbeat()
	{
		HeartbeatConfig config;
		config.checkInterval_ms = 5'000;
		config.timeout_ms = 15'000;
		config.stalledPeerTimeout_ms = 20'000;
		return config;
	}
}
