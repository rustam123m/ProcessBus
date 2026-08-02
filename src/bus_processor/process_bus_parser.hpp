#pragma once

#include "common/goose_container.hpp"
#include "common/sv_container.hpp"
#include "common/r_session.hpp"
#include "common/r_session_crypto.hpp"

#include <rte_byteorder.h>

enum BUS_PROTO
{
    NON_BUS_PROTO = 0,
    BUS_PROTO_SV,
    BUS_PROTO_GOOSE,
    BUS_PROTO_R_GOOSE,
    BUS_PROTO_R_SV,
    /* BUS_PROTO_PTP */
};

// Returned by the R parsers when the signature or GCM tag does not verify.
constexpr int R_PARSE_ERR_AUTH = -10;

class ProcessBusParser
{
public:
    static inline
    unsigned get_appid(const uint8_t* buffer)
    {
        return RTE_STATIC_BSWAP16(*(uint16_t *)(buffer + 18));
    }

    /*
     * Is used to get APPID and dispatch GOOSE/SV mbuf to a particular CPU
     *
     * size frame length; the routable branch reads deeper than the L2 ones
     *
     * The APPID here is only the RSS key; every stage re-reads it from the frame.
     */
    static inline
    BUS_PROTO get_proto_type(const uint8_t* buffer, unsigned *appid, unsigned size)
    {
        if (buffer[12] == 0x81 && buffer[13] == 0x00) {
            // VLAN
            if (buffer[16] == 0x88 && buffer[17] == 0xBA) {
                // SV
                *appid = get_appid(buffer);
                return BUS_PROTO_SV;
            }
            if (buffer[16] == 0x88 && buffer[17] == 0xB8) {
                // GOOSE
                *appid = get_appid(buffer);
                return BUS_PROTO_GOOSE;
            }
        }
        if (buffer[12] == 0x88 && buffer[13] == 0xBA) {
            // SV without VLAN
            *appid = RTE_STATIC_BSWAP16(*(uint16_t *)(buffer + 14));
            return BUS_PROTO_SV;
        }
        if (buffer[12] == 0x88 && buffer[13] == 0xB8) {
            // GOOSE without VLAN
            *appid = RTE_STATIC_BSWAP16(*(uint16_t *)(buffer + 14));
            return BUS_PROTO_GOOSE;
        }
        if (buffer[12] == 0x08 && buffer[13] == 0x00 && size >= R_MIN_FRAME_SIZE) {
            // Routable GOOSE/SV (IEC 61850-90-5) over IPv4/UDP.
            // Assumes no VLAN tag, IHL = 5 and no fragmentation — the same
            // assumptions RFrame::build_prefix() encodes on the TX side.
            if (buffer[14] == 0x45 && buffer[23] == 0x11
                && buffer[36] == 0x00 && buffer[37] == 0x66) {
                const uint8_t si = buffer[R_SI_OFFSET];
                if (si == RSess::SI_R_GOOSE || si == RSess::SI_R_SV) {
                    /*
                     * RSS key is the UDP source port: under GCM the payload-header
                     * APPID is ciphertext, so the generator publishes it there.
                     */
                    *appid = RTE_STATIC_BSWAP16(*(uint16_t *)(buffer + 34));

                    return (si == RSess::SI_R_GOOSE) ? BUS_PROTO_R_GOOSE
                                                     : BUS_PROTO_R_SV;
                }
            }
        }
        return NON_BUS_PROTO;
    }

    /*
     * Returns 0 on success, -100 when a mandatory field is missing,
     *         -101 for a malformed allData entry, -102 when the number of
     *         entries found differs from numDatSetEntries.
     */
    static int
    parse_goose_packet(const uint8_t *buffer, int size,
                       GoosePassport &passport, GooseState &state);

    /**
     * @function parse_sv_packet
     */
    static int
    parse_sv_packet(const uint8_t *buffer, int size,
                    SVStreamPassport &passport, SVStreamState &state);

    /*
     * Unwrap a routable GOOSE frame and walk its APDU.
     *
     * buffer mutable: AES-GCM decrypts the payload in place
     * mode   security mode the receiver is configured for; a frame that
     *               does not carry it is rejected
     *
     * Returns 0 on success, R_PARSE_ERR_AUTH when the frame does not verify,
     *         an RSess::ParseError for a malformed envelope, or the same
     *         negative codes parse_goose_packet uses for a malformed APDU.
     */
    static int
    parse_r_goose_packet(uint8_t *buffer, int size,
                         RSess::SecurityMode mode, RSess::Crypto &crypto,
                         RSess::SessionHeader &session,
                         GoosePassport &passport, GooseState &state);

    // Routable Sampled Values counterpart of parse_r_goose_packet.
    static int
    parse_r_sv_packet(uint8_t *buffer, int size,
                      RSess::SecurityMode mode, RSess::Crypto &crypto,
                      RSess::SessionHeader &session,
                      SVStreamPassport &passport, SVStreamState &state);

private:
    static constexpr unsigned R_SI_OFFSET =
        RSess::ETH_IP_UDP_SIZE + RSess::OFF_SI;

    // Enough for the whole fixed session header plus the payload-length field.
    static constexpr unsigned R_MIN_FRAME_SIZE =
        RSess::ETH_IP_UDP_SIZE + RSess::SESSION_FIXED_SIZE + 4;
};
