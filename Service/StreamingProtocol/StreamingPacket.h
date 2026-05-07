#pragma once

#include "StreamingPacketID.h"
#include "StreamSessionContext.h"
#include "../../../../Module/IOCPNetworkEngine/Protocol/PacketHeader.h"

#pragma pack(push)

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

struct CS_DESKTOP_STREAMING_INFO_PACKET
{
	PACKET_HEADER header = PACKET_HEADER(
		ToPacketID(DESKTOP_STREAMING_PACKET_ID::SC_DESKTOP_STREAMING_INFO),
		sizeof(CS_DESKTOP_STREAMING_INFO_PACKET));

	uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY;
	uint32_t sttreamInfoVersion = 1;
	uint32_t codecConfigVersion = 1;
	uint16_t codecType = static_cast<uint16_t>(DESKTOP_STREAM_CODEC_TYPE::NONE);
	uint16_t width = 1280;
	uint16_t height = 720;
	uint16_t fps = 30;
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

inline uint16_t MakeDesktopStreamingPacketSize(uint32_t size)
{
	if (size > USHRT_MAX)
	{
		__debugbreak();
		return 0;
	}

	return static_cast<uint16_t>(size);
}

inline uint16_t MakeDesktopStreamingVariablePacketSize(uint32_t baseSize, uint32_t payloadSize)
{
	return MakeDesktopStreamingPacketSize(baseSize + payloadSize);
}
