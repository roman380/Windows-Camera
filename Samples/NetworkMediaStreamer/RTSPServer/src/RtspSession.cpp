// Copyright (C) Microsoft Corporation. All rights reserved.
#include <stdio.h>
#include <time.h>

#include<iostream>
#include <sstream>
#include <ppltasks.h>
#include <thread>
#include "RtspSession.h"
#include "RTSPServer.h"

using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Networking::Sockets;
RTSPSession::RTSPSession(
      CSocketWrapper* rtspClientSocket
    , streamerMapType streamers
    , IRTSPAuthProvider* pAuthProvider
    , winrt::event<winrt::delegate<winrt::hresult, winrt::hstring>>* m_pLoggers)
    : m_pRtspClient(rtspClientSocket)
    , m_callBackHandle(nullptr)
    , m_RtspReadEvent(nullptr)
    , m_pTcpTxBuff(nullptr)
    , m_pTcpRxBuff(nullptr)
    , m_bStreamingStarted(false)
    , m_streamers(streamers)
    , m_spCurrentStreamer(nullptr)
    , m_pLoggerEvents(m_pLoggers)
    , m_bTerminate(false)
    , m_bAuthorizationReceived(false)
{
    m_RtspSessionID = rand() << 16;         // create a session ID
    m_RtspSessionID |= rand();
    m_RtspSessionID |= 0x80000000;
    m_ssrc = 0;
    m_ClientRTPPort = RTP_DEFAULT_PORT;
    m_ClientRTCPPort = RTP_DEFAULT_PORT;
    m_LocalRTPPort = RTP_DEFAULT_PORT;
    m_LocalRTCPPort = RTP_DEFAULT_PORT;
    m_TcpTransport = false;

    sockaddr_in recvAddr;
    int         recvLen = sizeof(recvAddr);
    getpeername(m_pRtspClient->GetSocket(), (struct sockaddr*) & recvAddr, &recvLen);
    char addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(recvAddr.sin_addr), addr, INET_ADDRSTRLEN);
    m_RtspClientAddr = addr;
    m_RtspPort = ntohs(recvAddr.sin_port);
    m_dest.clear();
    Init();
    m_pTcpRxBuff = std::make_unique<BYTE[]>(RTSP_BUFFER_SIZE);
    if (m_pRtspClient->IsClientCertAuthenticated())
    {
        m_pLoggerEvents[(int)LoggerType::OTHER](S_OK, L"\nClient Authenticated over TLS as user: " + winrt::hstring(m_pRtspClient->GetClientCertUserName()));
    }
    else
    {
        m_pLoggerEvents[(int)LoggerType::OTHER](S_OK, winrt::hstring(L"\nClient  not Authenticated over TLS "));
    }
    m_spAuthProvider.copy_from(pAuthProvider);
}

RTSPSession::~RTSPSession()
{
    StopIfStreaming();
    if (m_callBackHandle)
    {
        UnregisterWait(m_callBackHandle);
        m_callBackHandle = nullptr;
    }
    if (m_RtspReadEvent)
    {
        WSACloseEvent(m_RtspReadEvent);
        m_RtspReadEvent = nullptr;
    }
}
void RTSPSession::InitTCPTransport()
{

    m_pTcpTxBuff = std::make_unique<BYTE[]>(USHRT_MAX);
    m_packetHandler = [this](BYTE* buf, size_t size)
    {
        BYTE strmIdx = ((buf[1] >= 192 && buf[1] <= 195) || buf[1] >= 200 && buf[1] <= 210);
        m_pTcpTxBuff[0] = '$';
        m_pTcpTxBuff[1] = strmIdx;
        m_pTcpTxBuff[2] = (size & 0x0000FF00) >> 8;
        m_pTcpTxBuff[3] = (size & 0x000000FF);
        memcpy_s(&m_pTcpTxBuff[4], USHRT_MAX - 4, buf, size);

        m_pRtspClient->Send(m_pTcpTxBuff.get(), (int)size + 4);
    };
}
void RTSPSession::InitUDPTransport()
{
    sockaddr_in Server;
    // allocate port pairs for RTP/RTCP ports in UDP transport mode
    Server.sin_family = AF_INET;
    Server.sin_addr.s_addr = INADDR_ANY;
    for (u_short P = 6970; P < 0xFFFE; P += 2)
    {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
        Server.sin_port = htons(P);
        if (bind(s, (sockaddr*)&Server, sizeof(Server)) == 0)
        {   // Rtp socket was bound successfully. Lets try to bind the consecutive Rtsp socket
            SOCKET s1 = socket(AF_INET, SOCK_DGRAM, 0);
            Server.sin_port = htons(P + 1);
            if (bind(s1, (sockaddr*)&Server, sizeof(Server)) == 0)
            {
                m_LocalRTPPort = P;
                m_LocalRTCPPort = P + 1;
                closesocket(s);
                closesocket(s1);
                break;
            }
            else
            {
                closesocket(s);
                closesocket(s1);
            };
        }
        else closesocket(s);
    };

}
void RTSPSession::Init()
{
    m_CSeq.clear();
    m_URLHostPort.clear();
    m_URLProto.clear();
    //m_ContentLength = 0;

}

RTSP_CMD RTSPSession::ParseRequest(char const* aRequest, unsigned aRequestSize)
{
    std::string cmdName;
    std::string curRequest;
    unsigned curRequestSize;
    RTSP_CMD rtspCmdType = RTSP_CMD::UNKNOWN;
    Init();
    curRequestSize = aRequestSize;
    curRequest = std::string(aRequest, aRequestSize);
    curRequest[aRequestSize - 1] = '\0';

    m_pLoggerEvents[(int)LoggerType::RTSPMSGS](S_OK, L"\nRequest:: " + winrt::to_hstring(curRequest));


    // look for client port
    size_t clientportPos;
    if ((clientportPos = curRequest.find("client_port")) != std::string::npos)
    {
        if ((clientportPos = curRequest.find('=', clientportPos)) != std::string::npos)
        {
            clientportPos++;
            m_ClientRTPPort = std::stoi(curRequest.substr(clientportPos));
            if ((clientportPos = curRequest.find("-", clientportPos)) != std::string::npos)
            {
                clientportPos++;
                m_ClientRTCPPort = std::stoi(curRequest.substr(clientportPos));
            }
            else
            {
                m_ClientRTCPPort = m_ClientRTPPort + 1;
            }
        }
    }

    // look for ssrc
    size_t ssrcPos;
    if ((ssrcPos = curRequest.find("ssrc")) != std::string::npos)
    {
        if ((ssrcPos = curRequest.find("=")) != std::string::npos)
        {
            ssrcPos++;
            m_ssrc = std::stoi(curRequest.substr(ssrcPos), nullptr, 16);
        }
    }

    // Read everything up to the first space as the command name
    bool parseSucceeded = false;
    size_t cmdPos = curRequest.find_first_of(" \t"); // check for space or tab, both tokens need to be there in the token string
    if (cmdPos == std::string::npos)
    {
        return RTSP_CMD::UNKNOWN;
    }
    cmdName = curRequest.substr(0, cmdPos);
    std::map<std::string, RTSP_CMD> commandMap =
    {
        {"OPTIONS",RTSP_CMD::OPTIONS },
        {"DESCRIBE",RTSP_CMD::DESCRIBE },
        {"SETUP",RTSP_CMD::SETUP },
        {"PLAY",RTSP_CMD::PLAY },
        {"TEARDOWN",RTSP_CMD::TEARDOWN }
    };

    // find out the command type
    auto cmdIt = commandMap.find(cmdName);
    if (cmdIt != commandMap.end())
    {
        rtspCmdType = cmdIt->second;
    }
    else
    {
        rtspCmdType = RTSP_CMD::UNKNOWN;
    }
    // check whether the request contains transport information (UDP or TCP)
    if (rtspCmdType == RTSP_CMD::SETUP)
    {
        if (curRequest.find("RTP/AVP/TCP") != std::string::npos)
        {
            m_TcpTransport = true;
        }
        else
        {
            m_TcpTransport = false;
        }
    };
    bool bURLFound = false;
    auto urlPos = curRequest.find("://", cmdPos);

    bURLFound = (urlPos != std::string::npos);
    if (bURLFound)
    {
        if ((curRequest[urlPos - 1] == 's') || (curRequest[urlPos - 1] == 'S'))
        {
            m_URLProto = "rtsps";
        }
        else
        {
            m_URLProto = "rtsp";
        }
        urlPos += 3;
        m_URLHostPort = curRequest.substr(urlPos, curRequest.find_first_of("/ \t", urlPos) - urlPos);
        if ((!m_spCurrentStreamer) && ((rtspCmdType == RTSP_CMD::DESCRIBE) || (rtspCmdType == RTSP_CMD::SETUP)))
        {

            // look for url suffix only if streaming not started
            urlPos += m_URLHostPort.length();
            if (curRequest[urlPos] == '/')
            {
                m_urlSuffix = curRequest.substr(urlPos, curRequest.find_first_of(" \t\r\n", urlPos) - urlPos);
            }

            if (m_streamers.find(m_urlSuffix) != m_streamers.end())
            {
                
                m_streamers[m_urlSuffix]->GetStreamSinkByIndex(0, m_spCurrentStreamer.put());
            }
            else
            {
                //if no url suffix matched videostreamer list
                m_streamers.begin()->second->GetStreamSinkByIndex(0, m_spCurrentStreamer.put());
            }
        }
    }

    auto CSeqPos = curRequest.find("CSeq:", bURLFound ? urlPos : cmdPos);
    if (CSeqPos == std::string::npos)
    {
        //if no CSeq found in request then increment
        m_CSeq = std::to_string(std::stoi(m_CSeq) + 1);
    }
    else
    {
        CSeqPos += 5;
        CSeqPos = curRequest.find_first_not_of(" \t", CSeqPos);
        m_CSeq = curRequest.substr(CSeqPos, curRequest.find_first_of("\r\n", CSeqPos) - CSeqPos);
    }

    auto authpos = curRequest.find("Authorization:");
    m_bAuthorizationReceived =(authpos != std::string::npos);
    if (m_bAuthorizationReceived)
    {
        auto auth = curRequest.substr(authpos, curRequest.find_first_of("\r\n", authpos));
        m_bAuthorizationReceived = m_spAuthProvider? m_spAuthProvider->Authorize(auth, m_curAuthSessionMsg, cmdName) : true;
    }
    return rtspCmdType;
}

RTSP_CMD RTSPSession::HandleRequest(char const* aRequest, unsigned aRequestSize)
{
    auto rtspCmdType = ParseRequest(aRequest, aRequestSize);

    switch (rtspCmdType)
    {
    case RTSP_CMD::OPTIONS: { HandleCmdOPTIONS();   break; };
    case RTSP_CMD::DESCRIBE: { HandleCmdDESCRIBE(); break; };
    case RTSP_CMD::SETUP: { HandleCmdSETUP();    break; };
    case RTSP_CMD::PLAY: { HandleCmdPLAY();     break; };
    case RTSP_CMD::TEARDOWN: {HandleCmdTEARDOWN(); break; };
    default: {};
    }
    return rtspCmdType;
}

void RTSPSession::HandleCmdOPTIONS()
{
    std::string Response = "RTSP/1.0 200 OK\r\nCSeq: " + m_CSeq + "\r\n"
        + "Public: DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE\r\n\r\n";

    m_pLoggerEvents[(int)LoggerType::RTSPMSGS](S_OK, winrt::to_hstring(__FUNCTION__) + L":Response:" + winrt::to_hstring(Response));

    SendToClient(Response);
}

void RTSPSession::HandleCmdDESCRIBE()
{
    std::string   Response;
    char   SDPBuf[1024];
    if (m_pRtspClient.get()->IsClientCertAuthenticated() || m_bAuthorizationReceived)
    {
        std::string dest = m_RtspClientAddr + std::string(":") + std::to_string(m_LocalRTPPort);
        m_spCurrentStreamer.as<INetworkMediaStreamSink>()->GenerateSDP(SDPBuf, 1024, dest);

        Response = "RTSP/1.0 200 OK\r\nCSeq: " + m_CSeq + "\r\n"
            + DateHeader() + "\r\n"
            + "Content-Base: " + m_URLProto + "://" + m_URLHostPort + "\r\n"
            + "Content-Length: " + std::to_string(strlen(SDPBuf)) + "\r\n\r\n"
            + SDPBuf;
    }
    else
    {
        Response = "RTSP/1.0 401 Unauthorized\r\n"
            + std::string("CSeq: ") + m_CSeq + "\r\n";
        if (m_spAuthProvider)
        {
            m_curAuthSessionMsg = m_spAuthProvider->GetNewAuthSessionMessage();
            Response += m_curAuthSessionMsg;
        }
         Response += std::string("Server: NightKing\r\n")
        + DateHeader() + "\r\n\r\n";

    }
    SendToClient(Response);

#if (DBGLEVEL == 1)
    m_pLoggerEvents[(int)LoggerType::RTSPMSGS](S_OK, winrt::to_hstring(__FUNCTION__) + L":Response:" + winrt::to_hstring(Response));
#endif
}

void RTSPSession::HandleCmdSETUP()
{
    std::string Response;// [1024] ;
    std::string Transport;// [255] ;
    if (m_pRtspClient.get()->IsClientCertAuthenticated() || m_bAuthorizationReceived)
    {
        // simulate SETUP server response
        if (m_TcpTransport)
        {
            InitTCPTransport();
            Transport = "RTP/AVP/TCP;unicast;interleaved=0-1";
        }
        else
        {
            InitUDPTransport();
            Transport = "RTP/AVP;unicast;destination=" + m_RtspClientAddr + ";source=127.0.0.1;client_port="
                + std::to_string(m_ClientRTPPort) + "-" + std::to_string(m_ClientRTCPPort)
                + ";server_port=" + std::to_string(m_LocalRTPPort) + "-" + std::to_string(m_LocalRTCPPort);
        }
        Response = "RTSP/1.0 200 OK\r\nCSeq: " + m_CSeq + "\r\n"
            + DateHeader() + "\r\n"
            + "Transport: " + Transport + "\r\n"
            + "Session: " + std::to_string(m_RtspSessionID) + "\r\n\r\n";
    }
    else
    {
        Response = "RTSP/1.0 401 Unauthorized\r\n"
            + std::string("CSeq: ") + m_CSeq + "\r\n";
        if (m_spAuthProvider)
        {
            m_curAuthSessionMsg = m_spAuthProvider->GetNewAuthSessionMessage();
            Response += m_curAuthSessionMsg;
        }
        Response += std::string("Server: NightKing\r\n")
            + DateHeader() + "\r\n\r\n";
    }
    SendToClient(Response);

#if (DBGLEVEL == 1)
    m_pLoggerEvents[(int)LoggerType::RTSPMSGS](S_OK, winrt::to_hstring(__FUNCTION__) + L":Response:" + winrt::to_hstring(Response));
#endif
}
void RTSPSession::StopIfStreaming()
{
    if (m_bStreamingStarted)
    {
        if (!m_dest.empty())
        {
            m_spCurrentStreamer.as<INetworkMediaStreamSink>()->RemoveNetworkClient(m_dest);
            m_dest.clear();
        }
        else if (m_packetHandler)
        {
            m_spCurrentStreamer.as<INetworkMediaStreamSink>()->RemoveTransportHandler(m_packetHandler);
            m_packetHandler = nullptr;
        }
    }
    m_bStreamingStarted = false;
}
void RTSPSession::HandleCmdPLAY()
{
    std::string   Response;
    if (m_pRtspClient.get()->IsClientCertAuthenticated() || m_bAuthorizationReceived)
    {
        Response = "RTSP/1.0 200 OK\r\nCSeq: " + m_CSeq + "\r\n"
            + DateHeader() + "\r\n"
            + "Range: npt=0.000-\r\n"
            + "Session: " + std::to_string(m_RtspSessionID) + "\r\n"
            + "RTP-Info: url=" + m_URLProto + "://" + m_URLHostPort + "\r\n\r\n";

        SendToClient(Response);
        StopIfStreaming();
        if (m_TcpTransport)
        {
            m_spCurrentStreamer.as<INetworkMediaStreamSink>()->AddTransportHandler(m_packetHandler, "rtp", m_ssrc);
        }
        else
        {
            std::string dest = m_RtspClientAddr + std::string(":") + std::to_string(m_ClientRTPPort) + std::string("?localrtpport=") + std::to_string(m_LocalRTPPort);
            m_dest = dest;
            m_spCurrentStreamer.as<INetworkMediaStreamSink>()->AddNetworkClient(dest, "rtp", m_ssrc);
        }
        m_bStreamingStarted = true;

        std::string logstring = "\nAdding destination : ";
        if (!m_dest.empty())
            logstring += m_dest;
        else
            logstring += "tcp://" + m_RtspClientAddr;

        m_pLoggerEvents[(int)LoggerType::OTHER](S_OK, winrt::to_hstring(logstring));
    }
    else
    {
        Response = "RTSP/1.0 401 Unauthorized\r\n"
        + std::string("CSeq: ") + m_CSeq + "\r\n";

        if (m_spAuthProvider)
        {
            m_curAuthSessionMsg = m_spAuthProvider->GetNewAuthSessionMessage();
            Response += m_curAuthSessionMsg;
        }
        Response += std::string("Server: NightKing\r\n")
            + DateHeader() + "\r\n\r\n";

        SendToClient(Response);
    }

    m_pLoggerEvents[(int)LoggerType::RTSPMSGS](S_OK, winrt::to_hstring(__FUNCTION__) + L":Response:" + winrt::to_hstring(Response));
}

void RTSPSession::HandleCmdTEARDOWN()
{
    StopIfStreaming();
}
void RTSPSession::Handle_RtspPAUSE()
{
    std::string   Response;// [1024] ;
    Response = "RTSP/1.0 200 OK\r\nCSeq:" + m_CSeq + "\r\n"
        + DateHeader() + "\r\n"
        + "Range: npt=0.000-\r\n"
        + "Session: " + std::to_string(m_RtspSessionID) + "\r\n"
        + "RTP-Info: url=" + m_URLProto + "://" + m_URLHostPort + "\r\n\r\n",

        SendToClient(Response);
    StopIfStreaming();

    m_pLoggerEvents[(int)LoggerType::RTSPMSGS](S_OK, winrt::to_hstring(__FUNCTION__) + L":Response:" + winrt::to_hstring(Response));

}

void RTSPSession::SendToClient(std::string Response)
{
    m_pRtspClient->Send((BYTE*)Response.c_str(), (int)Response.length());
}

char const* RTSPSession::DateHeader()
{
    static char buf[RTSP_PARAM_STRING_MAX];
    time_t tt = time(NULL);
    tm t;
    gmtime_s(&t, &tt);
    strftime(buf, sizeof buf, "Date: %a, %b %d %Y %H:%M:%S GMT", &t);
    return buf;
}

int RTSPSession::GetStreamID()
{
    return 0;//m_StreamID;
}

void RTSPSession::BeginSession(winrt::delegate<RTSPSession*> completed)
{
    m_Completed = completed;
    m_RtspReadEvent = WSACreateEvent();      // create READ wait event for our RTSP client socket
    WSAEventSelect(m_pRtspClient->GetSocket(), m_RtspReadEvent, FD_READ | FD_CLOSE);   // select socket read event
    RegisterWaitForSingleObject(&m_callBackHandle, m_RtspReadEvent, [](PVOID arg, BOOLEAN flag)
        {
            auto pSession = (RTSPSession*)arg;
            WSAResetEvent(pSession->m_RtspReadEvent);
            {
                char* pRecvBuf = (char*)pSession->m_pTcpRxBuff.get();

                RTSP_CMD C = RTSP_CMD::UNKNOWN;
                int res = pSession->m_pRtspClient->Recv((BYTE*)pRecvBuf, RTSP_BUFFER_SIZE);
                if (res > 0)
                {
                    // we filter away everything which seems not to be an RTSP command: O-ption, D-escribe, S-etup, P-lay, T-eardown
                    if ((pRecvBuf[0] == 'O') || (pRecvBuf[0] == 'D') || (pRecvBuf[0] == 'S') || (pRecvBuf[0] == 'P') || (pRecvBuf[0] == 'T'))
                    {
                        C = pSession->HandleRequest(pRecvBuf, res);
                    }
                    else
                    {
                        std::ostringstream logstring;
                        logstring << "\nUnhandled Request ignored : size =" << res << "\nDump:";
                        for (int i = 0; i < res; i++)
                        {
                            logstring << std::hex << pRecvBuf[i];
                        }
                        pSession->m_pLoggerEvents[(int)LoggerType::WARNINGS](S_OK, winrt::to_hstring(logstring.str()));
                    }
                }

                if ((C == RTSP_CMD::TEARDOWN) || (res <= 0) || pSession->m_bTerminate)
                {
                    UnregisterWait(pSession->m_callBackHandle);
                    pSession->m_callBackHandle = nullptr;
                    pSession->m_Completed(pSession);
                }

            }
        }, this, INFINITE, WT_EXECUTEINWAITTHREAD);

}