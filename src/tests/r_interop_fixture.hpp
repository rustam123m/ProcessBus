#pragma once

// Drives the bundled libiec61850 R-session over UDP loopback as the oracle.

#include "common/r_session.hpp"
#include "common/r_session_crypto.hpp"
#include "common/r_frame_builder.hpp"

#include "r_session.h"
// Declared without an extern "C" block of its own, so wrap it here.
extern "C" {
#include "r_session_internal.h"
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

class LibRSession
{
public:
    struct Decoded
    {
        bool     received = false;
        uint16_t appId = 0;
        std::vector< uint8_t > apdu;
    };

    explicit LibRSession(RSess::SecurityMode mode)
    {
        // Distinct port pair per instance: tests run back to back and a stray
        // datagram from a previous case must not be mistaken for this one's.
        static uint16_t s_nextPort = 20102;
        m_libPort = s_nextPort++;
        m_ourPort = s_nextPort++;

        m_session = RSession_create();
        if (m_session == nullptr) {
            throw std::runtime_error("RSession_create failed");
        }

        RSecurityAlgorithm secAlgo = R_SESSION_SEC_ALGO_NONE;
        RSignatureAlgorithm sigAlgo = R_SESSION_SIG_ALGO_NONE;
        int keySize = static_cast< int >(RSess::HMAC_KEY_SIZE);

        switch (mode) {
        case RSess::SEC_NONE:
            /*
             * A key still has to be registered: the version-2 receiver rejects
             * KeyID 0 before it ever looks at the algorithms (r_session.c:661).
             */
            break;
        case RSess::SEC_HMAC:
            sigAlgo = R_SESSION_SIG_ALGO_HMAC_SHA256_128;
            break;
        case RSess::SEC_GCM:
            secAlgo = R_SESSION_SEC_ALGO_AES_128_GCM;
            // The backend picks AES-128 vs AES-256 from the key length.
            keySize = static_cast< int >(RSess::GCM_KEY_SIZE);
            break;
        }

        RSession_addKey(m_session, RSess::KEY_ID,
                        const_cast< uint8_t* >(RSess::LAB_KEY), keySize,
                        secAlgo, sigAlgo);
        RSession_setActiveKey(m_session, RSess::KEY_ID);

        RSession_setLocalAddress(m_session, "127.0.0.1", m_libPort);
        RSession_setRemoteAddress(m_session, "127.0.0.1", m_ourPort);
        if (RSession_start(m_session) != R_SESSION_ERROR_OK) {
            RSession_destroy(m_session);
            throw std::runtime_error("RSession_start failed on port "
                                     + std::to_string(m_libPort));
        }

        m_sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_sock < 0) {
            RSession_destroy(m_session);
            throw std::runtime_error("socket() failed");
        }

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_ourPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(m_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
            ::close(m_sock);
            RSession_destroy(m_session);
            throw std::runtime_error("bind() failed on port " + std::to_string(m_ourPort));
        }

        // Never let a rejected frame turn into a hung test.
        timeval tv = { 2, 0 };
        ::setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ~LibRSession()
    {
        if (m_sock >= 0) {
            ::close(m_sock);
        }
        if (m_session != nullptr) {
            RSession_destroy(m_session);
        }
    }

    LibRSession(const LibRSession&) = delete;
    LibRSession& operator=(const LibRSession&) = delete;

    // Ours -> theirs: hand a UDP payload we built to the library.
    Decoded Receive(const uint8_t* udpPayload, size_t size)
    {
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(m_libPort);
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::sendto(m_sock, udpPayload, size, 0, (sockaddr*)&to, sizeof(to))
            != (ssize_t)size) {
            throw std::runtime_error("sendto() to the library failed");
        }

        m_decoded = Decoded{};
        RSession_receiveMessage(m_session, &LibRSession::handler, this);
        return m_decoded;
    }

    // Theirs -> ours: let the library encode a frame and capture it.
    std::vector< uint8_t > Send(RSessionProtocol_SPDU_ID spduId, uint16_t appId,
                                const uint8_t* apdu, size_t size)
    {
        if (RSession_sendMessage(m_session, spduId, false, appId,
                                 const_cast< uint8_t* >(apdu),
                                 static_cast< int >(size)) != R_SESSION_ERROR_OK) {
            return {};
        }

        std::vector< uint8_t > buf(65535);
        const ssize_t got = ::recv(m_sock, buf.data(), buf.size(), 0);
        if (got <= 0) {
            return {};
        }
        buf.resize(static_cast< size_t >(got));
        return buf;
    }

    // Wrap a bare UDP payload in the Eth/IPv4/UDP prefix our parsers expect.
    static std::vector< uint8_t > WrapInFrame(const std::vector< uint8_t >& udpPayload,
                                              const RFrameConfig& cfg)
    {
        std::vector< uint8_t > frame(RFrame::OFF_UDP_PAYLOAD + udpPayload.size());
        RFrame::build_prefix(frame.data(), cfg, udpPayload.size());
        std::memcpy(frame.data() + RFrame::OFF_UDP_PAYLOAD,
                    udpPayload.data(), udpPayload.size());
        return frame;
    }

private:
    static void handler(void* parameter, uint16_t appId, uint8_t* payload, int payloadSize)
    {
        LibRSession* self = reinterpret_cast< LibRSession* >(parameter);
        self->m_decoded.received = true;
        self->m_decoded.appId = appId;
        self->m_decoded.apdu.assign(payload, payload + payloadSize);
    }

private:
    RSession    m_session = nullptr;
    int         m_sock = -1;
    uint16_t    m_libPort = 0;
    uint16_t    m_ourPort = 0;
    Decoded     m_decoded;
};
