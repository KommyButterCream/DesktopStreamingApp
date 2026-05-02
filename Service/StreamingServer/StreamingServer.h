#pragma once

#include "../../../../Module/IOCPNetworkEngine/Core/IOCPServer.h" // for IOCPServer

#ifdef BUILD_IOCP_STREAMING_SERVER_DLL
#define IOCP_STREAMING_SERVER_API __declspec(dllexport)
#else
#define IOCP_STREAMING_SERVER_API __declspec(dllimport)
#endif

class ISession;

class IOCP_STREAMING_SERVER_API StreamingServer : public IOCPServer
{
public:
	StreamingServer();
	~StreamingServer() override;

public:
	bool StartServer(const char* ipAddress, const uint16_t port, const uint32_t maxConnectionCount);
	void StopServer();

private:
	void OnClientConnect(ISession* session) override;
	void OnClientDisconnect(ISession* session) override;
	void OnReceive(ISession* session, uint16_t packetId, const char* packetData, uint32_t packetSize) override;
	void OnSend(ISession* session, uint32_t bytesTransferred) override;
};