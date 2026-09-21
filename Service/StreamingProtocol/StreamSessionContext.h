#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <stdint.h>

#include "../../../../Module/IOCPNetworkEngine/Session/SessionContext.h"

constexpr uint32_t DESKTOP_STREAM_ID_PRIMARY = 1;
constexpr uint32_t DESKTOP_STREAM_MAX_FRAME_SIZE = 512 * 1024;

enum class DESKTOP_STREAM_CODEC_TYPE : uint16_t
{
	NONE,
	H264
};

// 서버가 뷰어 하나에 대해 들고 있는 상태.
//
// 이 안의 두 표시(subscribed, waitingForKeyframe)는 서로 다른 스레드가
// 읽고 쓴다. 평범한 bool 로 두면 안 된다.
//
//   쓰는 쪽 : 브로드캐스트 스레드(엔코더 완료), 잡 스케줄러(구독/해지),
//             IOCP 워커(접속 종료)
//   읽는 쪽 : 캡처 스레드(매 프레임 forceKeyFrame 판정)
//
// 특히 waitingForKeyframe 은 인코더의 IDR 강제를 좌우한다. 이 값이 한 번
// 잘못 읽히면 그 뷰어는 참조 프레임 없이 남는다 — 화면이 영영 검은 채로
// 머무는 결함이 전부 이 한 바이트에 걸려 있다. 그래서 원자 연산으로 읽고
// 쓰고, 그 규약을 지키도록 필드를 private 으로 내린다.
class DesktopStreamServerSessionContext final : public SessionContext
{
public:
	bool IsSubscribed() const
	{
		return ::ReadAcquire(&m_subscribed) != FALSE;
	}

	void SetSubscribed(bool subscribed)
	{
		::InterlockedExchange(&m_subscribed, subscribed ? TRUE : FALSE);
	}

	bool IsWaitingForKeyframe() const
	{
		return ::ReadAcquire(&m_waitingForKeyframe) != FALSE;
	}

	void SetWaitingForKeyframe(bool waiting)
	{
		::InterlockedExchange(&m_waitingForKeyframe, waiting ? TRUE : FALSE);
	}

	uint32_t GetStreamId() const
	{
		return static_cast<uint32_t>(::ReadAcquire(&m_streamId));
	}

	void SetStreamId(uint32_t streamId)
	{
		::InterlockedExchange(&m_streamId, static_cast<LONG>(streamId));
	}

public:
	// 구독 시점의 버전. 잡 스케줄러 스레드에서만 쓰고 읽으므로 평범한 정수다.
	uint32_t streamInfoVersion = 0;
	uint32_t codecConfigVersion = 0;

	// 이 뷰어가 마지막으로 보고한 수신 상태. 잡 스케줄러 스레드가 쓰고
	// 같은 스레드가 읽는다(피드백 핸들러도 거기서 돈다).
	uint32_t lastFeedbackSequence = 0;

private:
	volatile LONG m_subscribed = FALSE;
	volatile LONG m_waitingForKeyframe = TRUE;
	volatile LONG m_streamId = static_cast<LONG>(DESKTOP_STREAM_ID_PRIMARY);
};

// 클라이언트가 스트림 하나에 대해 들고 있는 상태.
//
// 재조립 필드들은 잡 스케줄러 스레드 하나만 만진다(패킷 핸들러가 거기서
// 돈다). 예외는 subscribed / reassembling 으로, 접속이 끊기면 IOCP 워커가
// 그 자리에서 내린다. 그 둘만 원자 연산을 쓴다.
class DesktopStreamClientSessionContext final : public SessionContext
{
public:
	bool IsSubscribed() const
	{
		return ::ReadAcquire(&m_subscribed) != FALSE;
	}

	void SetSubscribed(bool subscribed)
	{
		::InterlockedExchange(&m_subscribed, subscribed ? TRUE : FALSE);
	}

	bool IsReassembling() const
	{
		return ::ReadAcquire(&m_reassembling) != FALSE;
	}

	void SetReassembling(bool reassembling)
	{
		::InterlockedExchange(&m_reassembling, reassembling ? TRUE : FALSE);
	}

public:
	bool hasStreamInfo = false;

	// 서버에 보낸 피드백 순번. 보낼 때마다 올린다.
	uint32_t feedbackSequence = 0;

	uint32_t streamId = 0;
	uint32_t streamInfoVersion = 0;
	uint32_t codecConfigVersion = 0;

	uint16_t codecType = 0;
	uint16_t frameType = 0;
	uint16_t width = 0;
	uint16_t height = 0;

	uint64_t currentFrameId = 0;
	uint64_t currentTimestamp = 0;

	uint32_t expectedFrameSize = 0;
	uint32_t receivedFrameSize = 0;
	uint16_t expectedChunkCount = 0;
	uint16_t nextChunkIndex = 0;

	char frameBuffer[DESKTOP_STREAM_MAX_FRAME_SIZE] = {};

private:
	volatile LONG m_subscribed = FALSE;
	volatile LONG m_reassembling = FALSE;
};
