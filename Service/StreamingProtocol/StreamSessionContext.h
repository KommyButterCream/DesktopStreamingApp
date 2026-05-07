#pragma once

#include <stdint.h>

#include "../../../../Module/IOCPNetworkEngine/Session/ISessionContext.h"

constexpr uint32_t DESKTOP_STREAM_ID_PRIMARY = 1;
constexpr uint32_t DESKTOP_STREAM_MAX_FRAME_SIZE = 256 * 1024;
constexpr uint32_t DESKTOP_STREAM_FAKE_FRAME_SIZE = 70 * 1024;

enum class DESKTOP_STREAM_CODEC_TYPE : uint16_t
{
	NONE,
	H264
};

class DesktopStreamServerSessionContext final : public SessionContext
{
public:
	bool subscribed = false;
	bool waitingForKeyframe = true;
	uint32_t streamId = DESKTOP_STREAM_ID_PRIMARY;
	uint32_t streamInfoVersion = 0;
	uint32_t codecConfigVersion = 0;
	uint64_t lastFrameId = 0;
};

class DesktopStreamClientSessionContext final : public SessionContext
{
public:
	bool subscribed = false;
	bool hasStreamInfo = false;
	bool hasCodecConfig = false;
	bool reassembling = false;

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

	char codecConfig[256] = {};
	uint32_t codecConfigSize = 0;

	char frameBuffer[DESKTOP_STREAM_MAX_FRAME_SIZE] = {};
};
