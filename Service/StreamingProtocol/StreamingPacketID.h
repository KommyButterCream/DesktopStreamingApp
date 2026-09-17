#pragma once

#include <stdint.h>

#include "../../../../Module/IOCPNetworkEngine/Protocol/PacketID.h"

enum class DESKTOP_STREAMING_PACKET_ID : uint16_t
{
	CS_DESKTOP_STREAMING_SUBSCRIBE = ToPacketID(PACKET_ID::SERVICE_BEGIN),
	CS_DESKTOP_STREAMING_UNSUBSCRIBE,
	SC_DESKTOP_STREAMING_INFO,
	SC_DESKTOP_STREAMING_FRAME_CHUNK,

	// 클라이언트가 자기 수신 상태를 알린다. 서버의 비트레이트 조정이
	// 이 신호를 본다 — 서버의 송신 실패만으로는 "디코더가 못 따라간다" 를
	// 알 수 없기 때문이다.
	CS_DESKTOP_STREAMING_FEEDBACK,
};

constexpr PACKET_ID_TYPE ToPacketID(DESKTOP_STREAMING_PACKET_ID packetID)
{
	return static_cast<PACKET_ID_TYPE>(packetID);
};
