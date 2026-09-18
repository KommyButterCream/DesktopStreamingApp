#pragma once

#include "StreamingPacketID.h"
#include "StreamSessionContext.h"
#include "../../../../Module/IOCPNetworkEngine/Protocol/PacketHeader.h"
#include "../../../../Module/IOCPNetworkEngine/Buffer/PreDefine.h"

#include <limits.h>
#include <stddef.h>

#pragma pack(push, 1)

// 뷰어가 이 스트림을 받겠다고 알린다.
//
// 왜 접속만으로 시작하지 않는가
//   엔진 인증이 끝난 세션이라는 것과 프레임을 받을 준비가 됐다는 것은
//   다르다. 서버는 구독한 세션에만 청크를 보내고, 프레임 중간에 구독이
//   풀릴 수도 있으므로 BroadcastEncodedFrame 은 청크마다 다시 확인한다.
//
//   이 패킷이 그 뷰어를 키프레임 대기로 세우는 자리이기도 하다. 참조
//   프레임도 SPS/PPS 도 없으니 IDR 이 와야 첫 화면이 선다.
//
// streamId 는 지금 PRIMARY 하나뿐이고 서버도 그 값만 받는다. 그래도 필드로
// 두는 것은, 모니터를 여러 개 내보내게 될 때 프로토콜을 바꾸지 않기 위해서다.
struct CS_DESKTOP_STREAMING_SUBSCRIBE_PACKET
{
	PACKET_HEADER header = PACKET_HEADER(
		ToPacketID(DESKTOP_STREAMING_PACKET_ID::CS_DESKTOP_STREAMING_SUBSCRIBE),
		sizeof(CS_DESKTOP_STREAMING_SUBSCRIBE_PACKET));

	uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY;
};


// 연결은 유지한 채 수신만 멈춘다.
//
// 그냥 끊으면 안 되는가
//   된다. 다만 서버는 그 사실을 송신이 실패할 때에야 알게 되고, 그 시점은
//   이미 그 뷰어의 송신 큐를 채우고 프레임을 버린 뒤다. 미리 알려주면
//   목록에서 바로 빠지므로 남은 뷰어가 그 비용을 나눠 물지 않는다.
//
// 다시 구독하면 키프레임 대기부터 새로 시작한다.
struct CS_DESKTOP_STREAMING_UNSUBSCRIBE_PACKET
{
	PACKET_HEADER header = PACKET_HEADER(
		ToPacketID(DESKTOP_STREAMING_PACKET_ID::CS_DESKTOP_STREAMING_UNSUBSCRIBE),
		sizeof(CS_DESKTOP_STREAMING_UNSUBSCRIBE_PACKET));

	uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY;
};


// 첫 청크가 오기 전에 뷰어가 알아야 하는 스트림의 모양.
//
// 두 버전 필드가 이 패킷의 핵심이다
//   해상도가 바뀌면 서버가 인코더를 다시 만들고 버전을 올린 뒤 이것을 다시
//   뿌린다. 뷰어는 버전이 달라진 것을 보고 재조립 상태를 버린다. 이걸
//   빼먹으면 뷰어가 옛 크기를 믿은 채 새 비트스트림을 받고, 그건 디코더
//   쪽에서 조용히 깨진다.
//
//   지금은 SetStreamInfo 가 둘을 함께 올린다. 나눠 둔 것은 화면 크기는
//   그대로인데 코덱 파라메터만 바뀌는 경우를 구분할 자리를 남긴 것이다.
//
// width / height / fps 가 uint16_t 인 것은 화면 치수라 그 범위로 충분하기
// 때문이다. 스트림 식별자와 버전은 오래 도는 값이라 uint32_t 로 둔다.
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
// 인코딩된 프레임 한 장을 나르는 패킷. 이 프로토콜에서 유일한 가변 길이다.
//
// 왜 쪼개는가
//   PACKET_HEADER::packetSize 가 uint16_t 라 패킷 하나가 64KB 를 넘을 수
//   없다. QHD IDR 은 그보다 훨씬 크므로 32KB 단위로 나눠 보낸다.
//   chunkData[1] 은 그 꼬리를 가리키는 자리 표시이고, 실제 할당은
//   헤더 + chunkDataSize 만큼만 한다.
//
// 수신 측 규칙은 전부 아니면 전무다. 청크가 하나라도 어긋나면 그 프레임을
// 통째로 버린다 — 구멍 난 NAL 을 디코더에 넘기지 않기 위해서다.
//
// 필드가 서로를 검증한다
//   totalFrameSize   재조립 버퍼 상한. 이 값을 넘는 청크는 거절한다.
//   chunkIndex       순서 강제. 정확히 다음 번호가 아니면 프레임을 버린다.
//   chunkCount       프레임의 청크 수. MAX_CHUNK_COUNT 보다 크면 우리
//                    구현이 보낸 것이 아니므로 거절한다.
//   chunkOffset      memcpy 목적지. chunkIndex 로 계산할 수 있는 값을 굳이
//                    싣는 이유는 검증이다. 수신 측이
//                    chunkIndex * MAX_CHUNK_DATA_SIZE 와 대조하고, 어긋나면
//                    거절한다. 이 값이 그대로 재조립 버퍼의 오프셋이 되므로
//                    한 번 더 본다.
//   frameId          새 프레임이 시작됐는지 판정한다.
//   timestamp        디코더를 그대로 통과해 원본 프레임과 짝을 맞춘다.
//   frameType        진단용(키프레임 여부 등).
//
// 두 버전 필드는 서버가 청크마다 싣지만 현재 수신 측은 읽지 않는다.
// 스트림이 바뀐 것은 INFO 패킷으로 알고, 그 뒤에 도착한 옛 청크는 어차피
// chunkIndex 가 0 이 아니라 순서 검사에서 걸린다.
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
// 왜 반환 타입이 uint16_t 인가
//   이 값이 갈 곳은 PACKET_HEADER::packetSize 하나뿐이고 그게 uint16_t 다.
//   엔진의 와이어 포맷이 길이를 2 바이트로 싣기 때문에 패킷 하나는
//   65535 바이트를 넘을 수 없다(PACKET_SIZE_LIMIT).
//
//   uint32_t 로 돌려주면 좁히는 일이 호출부로 옮겨갈 뿐이고, 거기서는
//   암묵 변환으로 조용히 일어난다. 65540 바이트 패킷이 와이어에서 4 가
//   되는 식이다. 반환 타입을 좁혀 두면 범위 검사와 변환이 이 함수 한
//   곳에 묶이고, 호출부는 이미 검증된 값만 받는다.
//
//   입력이 uint32_t 인 것은 그 반대 이유다. 호출부는 헤더 + 페이로드를
//   더한 값을 넘기는데 그 합은 65535 를 넘을 수 있어야 검사가 의미를
//   갖는다. 넓게 받아서 좁게 돌려주는 것이 이 함수가 하는 일의 전부다.
//
// 넘치면 어떻게 하는가
//   __debugbreak() 가 여기 있었다. Release 빌드에서도 살아 있는 명령이라,
//   디버거가 붙어 있지 않으면 그대로 프로세스가 죽는다. 서비스 코드가
//   크기 계산을 한 번 잘못했을 때 낼 수 있는 가장 나쁜 결과다.
//
//   지금은 디버그 빌드에서만 멈추고, Release 에서는 0 을 돌려준다.
//   0 을 받은 호출부는 전송을 포기한다 — 서버는 그 프레임을 접고
//   framesAborted 로 세고, 클라이언트는 그 청크를 거절한다.
//   프레임 한 장을 잃지만 살아 있다.
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
