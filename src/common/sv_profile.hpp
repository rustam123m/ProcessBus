#pragma once

#include <cstddef>
#include <cstdint>

/*
 * The one SV data profile both sides agree on: IEC 61850-9-2LE, eight channels
 * of INT32 value plus 32-bit Quality. The generator builds it through
 * libiec61850, the processor validates it without linking libiec61850, so the
 * numbers live here rather than in either of them.
 */
namespace SVProfile
{
    constexpr size_t   CHANNELS       = 8;   // four currents, four voltages
    constexpr size_t   VALUE_SIZE     = 4;
    constexpr size_t   QUALITY_SIZE   = 4;
    constexpr size_t   CHANNEL_SIZE   = VALUE_SIZE + QUALITY_SIZE;
    constexpr size_t   ASDU_DATA_SIZE = CHANNELS * CHANNEL_SIZE;

    // libiec61850's QUALITY_VALIDITY_GOOD; see the static_assert in
    // sv_traffic_gen.cpp, which sees both definitions.
    constexpr uint32_t QUALITY_GOOD   = 0;
}
