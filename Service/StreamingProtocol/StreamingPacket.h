#pragma once

#include "StreamingPacketID.h"
#include "StreamSessionContext.h"
#include "../../../../Module/IOCPNetworkEngine/Protocol/PacketHeader.h"
#include "../../../../Module/IOCPNetworkEngine/Buffer/PreDefine.h"

#include <limits.h>
#include <stddef.h>

#pragma pack(push, 1)

struct CS_DESKTOP_STREAMING_SUBSCRIBE_PACKET
{
	PACKET_HEADER header = PACKET_HEADER(
		ToPacketID(DESKTOP_STREAMING_PACKET_ID::CS_DESKTOP_STREAMING_SUBSCRIBE),
		sizeof(CS_DESKTOP_STREAMING_SUBSCRIBE_PACKET));

	uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY;
};

struct CS_DESKTOP_STREAMING_UNSUBSCRIBE_PACKET
{
	PACKET_HEADER header = PACKET_HEADER(
		ToPacketID(DESKTOP_STREAMING_PACKET_ID::CS_DESKTOP_STREAMING_UNSUBSCRIBE),
		sizeof(CS_DESKTOP_STREAMING_UNSUBSCRIBE_PACKET));

	uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY;
};

// 이름이 CS_ 였다. 서버가 만들어 클라이언트에게 보내는 패킷인데 접두사는
// 반대를 가리키고 있었고, 패킷 ID 는 제대로 SC_ 였다(StreamingPacketID.h).
// 방향을 읽으려고 붙인 접두사가 방향을 속이면 없느니만 못하다.
struct SC_DESKTOP_STREAMING_INFO_PACKET
{
	PACKET_HEADER header = PACKET_HEADER(
		ToPacketID(DESKTOP_STREAMING_PACKET_ID::SC_DESKTOP_STREAMING_INFO),
		sizeof(SC_DESKTOP_STREAMING_INFO_PACKET));

	uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY;
	uint32_t streamInfoVersion = 1;
	uint32_t codecConfigVersion = 1;
	uint16_t codecType = static_cast<uint16_t>(DESKTOP_STREAM_CODEC_TYPE::NONE);
	uint16_t width = 1280;
	uint16_t height = 720;
	uint16_t fps = 30;
};


// 클라이언트가 주기적으로 보내는 수신 상태.
//
// 왜 필요한가
//   서버는 자기 송신 실패(chunksFailed)만 볼 수 있다. 그건 "네트워크가
//   막혔다" 는 신호이지 "디코더가 못 따라간다" 는 신호가 아니다. 뒤엣것은
//   클라이언트만 안다 — 큐가 넘쳐 프레임을 버렸는지, 재조립이 어긋났는지.
//
//   비트레이트를 내려야 할 이유의 절반이 이쪽에 있다.
//
// 값은 전부 '직전 보고 이후의 증분'이다. 누적을 보내면 서버가 델타를
// 계산해야 하고, 그러면 재접속으로 카운터가 0 으로 돌아갈 때 음수가 된다.
struct CS_DESKTOP_STREAMING_FEEDBACK_PACKET
{
	PACKET_HEADER header = PACKET_HEADER(
		ToPacketID(DESKTOP_STREAMING_PACKET_ID::CS_DESKTOP_STREAMING_FEEDBACK),
		sizeof(CS_DESKTOP_STREAMING_FEEDBACK_PACKET));

	uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY;

	// 보고 순번. 중복/역순 도착을 걸러낸다.
	uint32_t reportSequence = 0;

	uint32_t framesCompleted = 0;   // 온전히 복원한 프레임
	uint32_t framesDiscarded = 0;   // 청크가 어긋나 버린 프레임
	uint32_t decodeDropped = 0;     // 디코더가 밀려 버린 프레임
};
struct SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET
{
	PACKET_HEADER header = PACKET_HEADER(
		ToPacketID(DESKTOP_STREAMING_PACKET_ID::SC_DESKTOP_STREAMING_FRAME_CHUNK),
		sizeof(SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET));

	uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY;
	uint32_t streamInfoVersion = 1;
	uint32_t codecConfigVersion = 1;
	uint64_t frameId = 0;
	uint64_t timestamp = 0;
	uint32_t totalFrameSize = 0;
	uint32_t chunkOffset = 0;
	uint16_t chunkIndex = 0;
	uint16_t chunkCount = 0;
	uint16_t chunkDataSize = 0;
	uint16_t frameType = 0;
	char chunkData[1] = {};
};

#pragma pack(pop)

inline constexpr uint32_t GetDesktopStreamingFrameChunkHeaderSize()
{
	return static_cast<uint32_t>(offsetof(SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET, chunkData));
}

// --- 청크 크기. 이 스트림의 유일한 출처다 ---
//
// 예전에는 서버가 BroadcastEncodedFrame 안에서 MEMORY_SIZE_32K 를 직접
// 쓰고, 클라이언트는 엔진 기본 프리셋의 maxRecvPacketSize(65535)를 그대로
// 썼다. 두 숫자가 서로를 모르는 채로 "어쩌다 맞는" 상태였다.
//
// 그 관계가 깨지면 증상이 고약하다. 보내는 쪽이 상대의 수신 상한을 넘기면
// 받는 쪽은 프로토콜 위반으로 보고 연결을 끊고, 보낸 쪽에는 원인 모를 피어
// 끊김만 남는다. 엔진이 SessionBufferConfig::maxSendPacketSize 주석에서
// 경고하는 바로 그 상황이다.
//
// 그래서 한 곳에서 정하고, 서버의 송신 상한과 클라이언트의 수신 상한을
// 둘 다 여기서 끌어간다. (StreamingConfig.h)
constexpr uint32_t DESKTOP_STREAM_CHUNK_PACKET_SIZE = MEMORY_SIZE_32K;
constexpr uint32_t DESKTOP_STREAM_CHUNK_HEADER_SIZE = GetDesktopStreamingFrameChunkHeaderSize();
constexpr uint32_t DESKTOP_STREAM_MAX_CHUNK_DATA_SIZE = DESKTOP_STREAM_CHUNK_PACKET_SIZE - DESKTOP_STREAM_CHUNK_HEADER_SIZE;

// 프레임 하나가 쪼개질 수 있는 최대 청크 수. 클라이언트가 chunkCount 를
// 검증할 때 쓴다 — 우리가 보낼 수 있는 것보다 큰 값을 주장하는 상대는
// 우리 구현이 아니다.
constexpr uint32_t DESKTOP_STREAM_MAX_CHUNK_COUNT =
	(DESKTOP_STREAM_MAX_FRAME_SIZE + DESKTOP_STREAM_MAX_CHUNK_DATA_SIZE - 1) / DESKTOP_STREAM_MAX_CHUNK_DATA_SIZE;

static_assert(DESKTOP_STREAM_CHUNK_PACKET_SIZE <= PACKET_SIZE_LIMIT,
	"chunk packet must fit in PACKET_HEADER::packetSize (uint16_t)");
static_assert(DESKTOP_STREAM_MAX_CHUNK_DATA_SIZE > 0,
	"chunk header must leave room for payload");
static_assert(DESKTOP_STREAM_MAX_CHUNK_COUNT <= USHRT_MAX,
	"chunkCount is uint16_t");

// 크기를 uint16_t 로 좁힌다.
//
// __debugbreak() 가 여기 있었다. Release 빌드에서도 살아 있는 명령이라,
// 디버거가 붙어 있지 않으면 그대로 프로세스가 죽는다. 서비스 코드가
// 크기 계산을 한 번 잘못했을 때 낼 수 있는 가장 나쁜 결과다.
//
// 지금은 디버그 빌드에서만 멈추고, Release 에서는 0 을 돌려준다.
// 0 을 받은 호출부는 전송을 포기한다 — 프레임 한 장을 잃지만 살아 있다.
inline uint16_t MakeDesktopStreamingPacketSize(uint32_t size)
{
	if (size > USHRT_MAX)
	{
#if defined(_DEBUG)
		__debugbreak();
#endif
		return 0;
	}

	return static_cast<uint16_t>(size);
}

inline uint16_t MakeDesktopStreamingVariablePacketSize(uint32_t baseSize, uint32_t payloadSize)
{
	return MakeDesktopStreamingPacketSize(baseSize + payloadSize);
}
