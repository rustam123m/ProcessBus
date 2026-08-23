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
// The configured SV profile carries INT32/Quality pairs. Any nonzero Quality
// marks the complete frame broken and is counted as an SV parser error.
constexpr int SV_PARSE_ERR_QUALITY = -11;

/*
 * validate_r_envelope() outcomes. All are broken-frame conditions folded into
 * the parser-error total; a distinct code per cause keeps the tests specific.
 * The destination-IP check is deliberately absent: a wrong group address is
 * carried in the passport and resolves to an Unknown-APPID lookup miss.
 */
enum R_ENV_ERR : int
{
    R_ENV_OK              = 0,
    R_ENV_ERR_NOT_IPV4    = -20,   // EtherType is not IPv4
    R_ENV_ERR_IHL         = -21,   // version != 4 or IHL != 5 (options present)
    R_ENV_ERR_IP_LEN      = -22,   // total length < 28 or exceeds received bytes
    R_ENV_ERR_FRAGMENT    = -23,   // More-Fragments set or nonzero fragment offset
    R_ENV_ERR_PROTO       = -24,   // IP protocol is not UDP
    R_ENV_ERR_UDP_PORT    = -25,   // UDP destination port is not 102
    R_ENV_ERR_UDP_LEN     = -26,   // UDP length < 8 or != IP total length - 20
    R_ENV_ERR_IP_CKSUM    = -27,   // IPv4 header checksum rejected
};

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
     * Validate the fixed Ethernet/IPv4/UDP envelope of a routable frame before
     * any session, crypto, or APDU work. Every field read is bounded by size.
     *
     * olFlags mbuf ol_flags carrying the RX IPv4-checksum result; tests pass
     *               the RTE_MBUF_F_RX_IP_CKSUM_* state directly. UNKNOWN falls
     *               back to a software checksum.
     * dstIP   set to the destination group (host order) on success
     *
     * Returns R_ENV_OK, or a negative R_ENV_ERR describing the first failure.
     */
    static int
    validate_r_envelope(const uint8_t *buffer, unsigned size,
                        uint64_t olFlags, uint32_t &dstIP);

    /*
     * Returns 0 on success, -100 when a mandatory field is missing,
     *         -101 for a malformed allData entry, -102 when the number of
     *         entries found differs from numDatSetEntries.
     */
    static int
    parse_goose_packet(const uint8_t *buffer, int size,
                       GoosePassport &passport, GooseState &state);

    /*
     * Parse and validate every ASDU in an SV APDU. The supported data profile
     * is a sequence of 4-byte INT32 values followed by 4-byte Quality fields;
     * every Quality byte must be zero.
     *
     * Returns 0 on success, SV_PARSE_ERR_QUALITY for a nonzero Quality field,
     * or another negative value for malformed ASN.1/required fields.
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
