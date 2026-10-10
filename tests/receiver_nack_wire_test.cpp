/* SPDX-License-Identifier: GPL-3.0-only */
/* Host tests exercise the production compound-packet encoder with the actual
 * pinned/patched libdatachannel Message and RTCP structures. No network send,
 * SRTCP cryptography, authenticated receive or console behavior is measured. */
#include "rtc_receiver_nack.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <vector>

static uint64_t checks;
static unsigned cases;
static constexpr uint32_t Sender = UINT32_C(0xaabbccdd);
static constexpr uint32_t Epoch = 17;
static constexpr std::array<char, 35> Cname = {
    'x', '4', '-', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b',
    'c', 'd', 'e', 'f', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a',
    'b', 'c', 'd', 'e', 'f'
};

static void fail(const char *expression, const char *file, int line)
{
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    std::exit(1);
}
#define CHECK(expression) do { ++checks; if (!(expression)) fail(#expression, __FILE__, __LINE__); } while (0)
#define CASE(test) do { test(); ++cases; std::printf("PASS %s\n", #test); } while (0)

static uint16_t read16(const uint8_t *bytes)
{ return uint16_t((uint16_t(bytes[0]) << 8) | bytes[1]); }
static uint32_t read32(const uint8_t *bytes)
{ return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
    (uint32_t(bytes[2]) << 8) | bytes[3]; }

static X4ReceiverNackGap gap(uint64_t first, unsigned count)
{
    X4ReceiverNackGap result{};
    result.present = true;
    result.ssrc = UINT32_C(0x01020304);
    result.sequence_epoch = 2;
    result.gap_id = 9;
    result.request_id = 3;
    result.first_extended = first;
    result.last_extended = first + count - 1;
    result.received_extended = first + count;
    result.detected_us = 100;
    result.requested_us = 101;
    result.missing_count = count;
    return result;
}

static std::vector<uint16_t> expectedPositions(const X4ReceiverNackGap &request,
    uint64_t pending)
{
    std::vector<uint16_t> result;
    for (unsigned i = 0; i < request.missing_count; ++i)
        if (pending & (UINT64_C(1) << i)) result.push_back(uint16_t(request.first_extended + i));
    return result;
}

static rtc::message_ptr checkPacket(const X4ReceiverNackGap &request,
    uint64_t pending, unsigned expectedFci)
{
    auto message = rtc::x4ReceiverNackPacket(Sender, Epoch, request, pending,
        300, Cname.data(), Cname.size());
    CHECK(message && message->type == rtc::Message::Control);
    CHECK(message->size() == 8 + 48 + 12 + 4 * expectedFci);
    const auto *bytes = reinterpret_cast<const uint8_t *>(message->data());
    /* Minimal compound RR has no report block. */
    CHECK(bytes[0] == 0x80 && bytes[1] == 201);
    CHECK(read16(bytes + 2) == 1 && read32(bytes + 4) == Sender);
    /* One SDES source chunk, CNAME, END and zero padding to 32-bit boundary. */
    CHECK(bytes[8] == 0x81 && bytes[9] == 202);
    CHECK(read16(bytes + 10) == 11 && read32(bytes + 12) == Sender);
    CHECK(bytes[16] == 1 && bytes[17] == Cname.size());
    for (unsigned i = 0; i < Cname.size(); ++i) CHECK(bytes[18 + i] == uint8_t(Cname[i]));
    CHECK(bytes[53] == 0 && bytes[54] == 0 && bytes[55] == 0);
    /* RTPFB Generic NACK: FMT 1, PT 205, sender and original media SSRC. */
    const uint8_t *nack = bytes + 56;
    CHECK(nack[0] == 0x81 && nack[1] == 205);
    CHECK(read16(nack + 2) == 2 + expectedFci);
    CHECK(read32(nack + 4) == Sender && read32(nack + 8) == request.ssrc);
    std::vector<uint16_t> decoded;
    for (unsigned part = 0; part < expectedFci; ++part) {
        uint16_t pid = read16(nack + 12 + 4 * part);
        uint16_t mask = read16(nack + 14 + 4 * part);
        decoded.push_back(pid);
        for (unsigned i = 0; i < 16; ++i)
            if (mask & (1u << i)) decoded.push_back(uint16_t(pid + i + 1));
    }
    CHECK(decoded == expectedPositions(request, pending));
    CHECK(message->x4NackIdentity == ((uint64_t(Epoch) << 32) | request.request_id));
    CHECK(message->x4NackSource == ((UINT64_C(1) << 63) | (UINT64_C(102) << 32) | request.ssrc));
    CHECK(message->x4NackFirst == request.first_extended && message->x4NackLast == request.last_extended);
    CHECK(message->x4NackDetected == request.detected_us && message->x4NackRequested == request.requested_us);
    CHECK(message->x4NackDispatch == 300);
    return message;
}

static void single_position_compound(void)
{
    auto message = checkPacket(gap(1001, 1), 1, 1);
    const auto *nack = reinterpret_cast<const uint8_t *>(message->data()) + 56;
    CHECK(read16(nack + 12) == 1001 && read16(nack + 14) == 0);
}

static void full_seventeen_positions_one_fci(void)
{
    auto message = checkPacket(gap(1001, 17), (UINT64_C(1) << 17) - 1, 1);
    const auto *nack = reinterpret_cast<const uint8_t *>(message->data()) + 56;
    CHECK(read16(nack + 12) == 1001 && read16(nack + 14) == 65535);
}

static void eighteenth_position_starts_second_fci(void)
{
    auto message = checkPacket(gap(1001, 18), (UINT64_C(1) << 18) - 1, 2);
    const auto *nack = reinterpret_cast<const uint8_t *>(message->data()) + 56;
    CHECK(read16(nack + 16) == 1018 && read16(nack + 18) == 0);
}

static void sixty_four_positions_four_fci(void)
{
    auto message = checkPacket(gap(1001, 64), UINT64_MAX, 4);
    const auto *nack = reinterpret_cast<const uint8_t *>(message->data()) + 56;
    CHECK(read16(nack + 12) == 1001 && read16(nack + 14) == 65535);
    CHECK(read16(nack + 16) == 1018 && read16(nack + 18) == 65535);
    CHECK(read16(nack + 20) == 1035 && read16(nack + 22) == 65535);
    CHECK(read16(nack + 24) == 1052 && read16(nack + 26) == 4095);
}

static void wrap_pid_and_bitmask(void)
{
    auto message = checkPacket(gap(65530, 20), (UINT64_C(1) << 20) - 1, 2);
    const auto *nack = reinterpret_cast<const uint8_t *>(message->data()) + 56;
    CHECK(read16(nack + 12) == 65530 && read16(nack + 14) == 65535);
    CHECK(read16(nack + 16) == 11 && read16(nack + 18) == 3);
}

static void sparse_cancellation_changes_fci(void)
{
    uint64_t pending = UINT64_C(1) | (UINT64_C(1) << 16) |
        (UINT64_C(1) << 17) | (UINT64_C(1) << 63);
    auto message = checkPacket(gap(1001, 64), pending, 3);
    const auto *nack = reinterpret_cast<const uint8_t *>(message->data()) + 56;
    CHECK(read16(nack + 12) == 1001 && read16(nack + 14) == 32768);
    CHECK(read16(nack + 16) == 1018 && read16(nack + 18) == 0);
    CHECK(read16(nack + 20) == 1064 && read16(nack + 22) == 0);
}

static void canceled_prefix_does_not_send_original_pid(void)
{
    auto message = checkPacket(gap(65530, 64), UINT64_C(1) << 63, 1);
    const auto *nack = reinterpret_cast<const uint8_t *>(message->data()) + 56;
    CHECK(read16(nack + 12) == 57 && read16(nack + 14) == 0);
}

static void zero_and_outside_masks_rejected(void)
{
    for (uint64_t pending : {UINT64_C(0), UINT64_C(1) << 1}) {
        bool rejected = false;
        try { (void)rtc::x4ReceiverNackPacket(Sender, Epoch, gap(1001, 1), pending,
            300, Cname.data(), Cname.size()); }
        catch (const std::exception &) { rejected = true; }
        CHECK(rejected);
    }
}

static void invalid_range_count_rejected(void)
{
    for (unsigned count : {0u, 65u, 1000u}) {
        auto request = gap(1001, count);
        bool rejected = false;
        try { (void)rtc::x4ReceiverNackPacket(Sender, Epoch, request, 1,
            300, Cname.data(), Cname.size()); }
        catch (const std::exception &) { rejected = true; }
        CHECK(rejected);
    }
}

static void invalid_cname_length_rejected(void)
{
    for (size_t length : {size_t(0), size_t(34), size_t(36), size_t(255)}) {
        bool rejected = false;
        try { (void)rtc::x4ReceiverNackPacket(Sender, Epoch, gap(1001, 1), 1,
            300, Cname.data(), length); }
        catch (const std::exception &) { rejected = true; }
        CHECK(rejected);
    }
}

static void malformed_descriptor_rejected(void)
{
    for (unsigned variant = 0; variant < 8; ++variant) {
        auto request = gap(1001, 3);
        uint32_t sender = Sender, epoch = Epoch;
        const char *cname = Cname.data();
        if (variant == 0) sender = 0;
        else if (variant == 1) epoch = 0;
        else if (variant == 2) request.request_id = 0;
        else if (variant == 3) request.request_id = 65;
        else if (variant == 4) request.last_extended = request.first_extended - 1;
        else if (variant == 5) ++request.last_extended;
        else if (variant == 6) cname = nullptr;
        else request.missing_count = UINT32_MAX;
        bool rejected = false;
        try { (void)rtc::x4ReceiverNackPacket(sender, epoch, request, 1,
            300, cname, Cname.size()); }
        catch (const std::exception &) { rejected = true; }
        CHECK(rejected);
    }
}

static void bounded_sparse_masks_preserve_exact_positions(void)
{
    /* Deterministic masks cover offsets/cancellation with actual production
       wire bytes. The count64 case never shifts a value by64. */
    uint64_t random = UINT64_C(0x982bb778ccd42150);
    for (unsigned count = 1; count <= 64; ++count) {
        uint64_t allowed = count == 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1;
        for (unsigned sample = 0; sample < 16; ++sample) {
            random ^= random << 13; random ^= random >> 7; random ^= random << 17;
            uint64_t pending = random & allowed;
            if (!pending) pending = 1;
            unsigned fci = 0;
            for (unsigned bit = 0; bit < count;) {
                if (pending & (UINT64_C(1) << bit)) { ++fci; bit += 17; }
                else ++bit;
            }
            CHECK(fci >= 1 && fci <= 4);
            checkPacket(gap(65500 + sample, count), pending, fci);
        }
    }
}

int main()
{
    CASE(single_position_compound);
    CASE(full_seventeen_positions_one_fci);
    CASE(eighteenth_position_starts_second_fci);
    CASE(sixty_four_positions_four_fci);
    CASE(wrap_pid_and_bitmask);
    CASE(sparse_cancellation_changes_fci);
    CASE(canceled_prefix_does_not_send_original_pid);
    CASE(zero_and_outside_masks_rejected);
    CASE(invalid_range_count_rejected);
    CASE(invalid_cname_length_rejected);
    CASE(malformed_descriptor_rejected);
    CASE(bounded_sparse_masks_preserve_exact_positions);
    std::printf("PASS receiver NACK production wire: %u cases, %llu checks\n", cases,
        static_cast<unsigned long long>(checks));
    return 0;
}
