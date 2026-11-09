/* SPDX-License-Identifier: GPL-3.0-only */
/* Included in the application-owned receiver override. It does not change
 * X4ReceiverNack's ABI: the pinned CAPI allocated that class itself. */
#include "recovery43_bridge.h"
#include <rtc/rtcpreceivingsession.hpp>
extern "C" {
#include "../core/x4_log.h"
}
#include <memory>
#include <mutex>
#include <unordered_map>
#include <iterator>
#if X4_RECOVERY43_HOST_TEST
#include <thread>
extern "C" void x4_recovery43_test_reader_acquired(uint32_t);
extern "C" void x4_recovery43_test_end_waiting(uint32_t);
extern "C" void x4_recovery43_test_receive_before_gate(uint32_t);
#else
extern "C" {
#include <orbis/libkernel.h>
}
#endif

namespace x4_recovery43 {
struct Engine {
    explicit Engine(const X4VideoRepair43Config &value)
        : config(value), repair(x4_video_repair43_create(&config)) {}
    ~Engine() { x4_video_repair43_free(repair); }
    const X4VideoRepair43Config config;
    X4VideoRepair43 *repair;
};
struct State {
    State(uint32_t e, unsigned m
#if !X4_PUBLIC
        , unsigned l, uint32_t s
#endif
        ) : epoch(e), mode(m),
#if !X4_PUBLIC
        loss(l), seed(s),
#endif
        scratch(new X4Recovery43Snapshot{}) {
        for (auto &tag : delivered) tag.store(0, std::memory_order_relaxed);
        for (auto &value : delivery_late) value.store(0, std::memory_order_relaxed);
        for (auto &value : delivery_duplicate) value.store(0, std::memory_order_relaxed);
    }
    const uint32_t epoch;
    const unsigned mode;
#if !X4_PUBLIC
    const unsigned loss;
    const uint32_t seed;
#endif
    /* Assigned once under the setup mutex; released only with this State.
     * Published raw pointer avoids atomic<shared_ptr>'s hidden receive lock. */
    std::unique_ptr<Engine> engine;
    std::atomic<Engine *> engine_published{nullptr};
    std::atomic<bool> closing{false};
    std::atomic<uint64_t> receives{0}, unauthenticated{0}, contention{0}, errors{0};
    std::atomic<uint64_t> incomplete{0}, discarded{0}, pli{0}, freezes{0};
    std::array<std::atomic<uint64_t>, 2048> delivered{};
    std::array<std::atomic<uint64_t>, 3> delivery_late{}, delivery_duplicate{};
    std::atomic<uint64_t> delivery_unknown{0};
    std::atomic_flag receiving = ATOMIC_FLAG_INIT;
#if !X4_PUBLIC
    /* Only the existing NACK owner writes this; final poll is after join. */
    uint64_t last_log_us = 0;
#endif
    std::unique_ptr<X4Recovery43Snapshot> scratch;
};
static std::mutex registry_mutex;
static std::unordered_map<uint32_t, std::shared_ptr<State>> registry;
/* Metadata-only publication: no shared_ptr runtime or registry lock on the
 * worker path. A reader increments BEFORE loading active, all seq_cst. If it
 * observes the old pointer, its increment precedes end's pointer exchange;
 * end cannot release that State until the reader leaves. A reader starting
 * after end observed zero must load NULL/the newer peer, never the old one.
 * Registry/handler shared_ptrs still own the State; this is only a lease. */
static std::atomic<State *> active{nullptr};
static std::atomic<unsigned> metadata_readers{0};
struct MetadataLease {
    State *state;
    MetadataLease() noexcept {
        metadata_readers.fetch_add(1, std::memory_order_seq_cst);
        state = active.load(std::memory_order_seq_cst);
    }
    ~MetadataLease() noexcept { metadata_readers.fetch_sub(1, std::memory_order_seq_cst); }
};
static std::shared_ptr<State> lookup(uint32_t epoch) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto found = registry.find(epoch);
    return found == registry.end() ? nullptr : found->second;
}
static void snapshot(const std::shared_ptr<State> &state, X4Recovery43Snapshot *out) {
    *out = {};
    out->epoch = state->epoch;
    out->requested_mode = state->mode;
#if !X4_PUBLIC
    out->requested_loss = state->loss;
#endif
    out->closing = state->closing.load(std::memory_order_acquire);
    out->receive_calls = state->receives.load(std::memory_order_relaxed);
    out->unauthenticated = state->unauthenticated.load(std::memory_order_relaxed);
    out->receive_contention = state->contention.load(std::memory_order_relaxed);
    out->engine_errors = state->errors.load(std::memory_order_relaxed);
    out->incomplete_au = state->incomplete.load(std::memory_order_relaxed);
    out->discarded_au = state->discarded.load(std::memory_order_relaxed);
    out->pli_requested = state->pli.load(std::memory_order_relaxed);
    out->freezes = state->freezes.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < 3; ++i) {
        out->delivery_late[i] = state->delivery_late[i].load(std::memory_order_relaxed);
        out->delivery_duplicate[i] = state->delivery_duplicate[i].load(std::memory_order_relaxed);
    }
    out->delivery_unknown = state->delivery_unknown.load(std::memory_order_relaxed);
    const auto *engine = state->engine_published.load(std::memory_order_acquire);
    if (engine) {
        out->configured = true; out->accepted = engine->config;
        x4_video_repair43_snapshot(engine->repair, &out->repair);
    }
}
class Handler final : public rtc::MediaHandler {
public:
    Handler(std::shared_ptr<State> state, const std::shared_ptr<rtc::RtcpReceivingSession> &rr)
        : state_(std::move(state)), rr_(rr) { output_.reserve(9); }
    void incoming(rtc::message_vector &messages, const rtc::message_callback &) override {
        const auto state = state_; // Keeps setup and engine alive through end().
#if X4_PUBLIC
        const bool passive = !state->mode;
#else
        const bool passive = !state->mode && !state->loss;
#endif
        if (state->closing.load(std::memory_order_acquire)) {
            messages.clear();
            return;
        }
        auto *engine = state->engine_published.load(std::memory_order_acquire);
        if (!engine) {
            if (passive) return;
            /* Before answer publication, or after a nonfatal allocation/
             * mapping failure, preserve only the existing H264 path. Raw FEC
             * must never leak into the application as if it were an AU. */
            messages.erase(std::remove_if(messages.begin(), messages.end(), [](const rtc::message_ptr &m) {
                if (!m) return true;
                if (m->type != rtc::Message::Binary) return false;
                return m->size() < 12 || !(m->x4DiagFlags & X4_RD_AUTHENTICATED) ||
                    (uint8_t((*m)[1]) & 127u) != X4_RECEIVER_NACK_VIDEO_PT;
            }), messages.end());
            return;
        }
        const auto rr = rr_.lock();
#if X4_RECOVERY43_HOST_TEST
        x4_recovery43_test_receive_before_gate(state->epoch);
#endif
        if (state->receiving.test_and_set(std::memory_order_acquire)) {
            state->contention.fetch_add(1, std::memory_order_relaxed);
            if (!passive || state->closing.load(std::memory_order_acquire)) messages.clear();
            return; // Never access the single-owner cache concurrently.
        }
        struct Guard { std::atomic_flag &flag;
            ~Guard() { flag.clear(std::memory_order_release); } } guard{state->receiving};
        /* end may have claimed closing between our first check and gate
         * acquisition. Do not create a writer after its final snapshot. */
        if (state->closing.load(std::memory_order_acquire)) {
            messages.clear();
            return;
        }
        // Reuse the single-owner vector: ordinary packets allocate no message
        // or output-vector storage after setup; repairs own their new bytes.
        auto &output = output_;
        output.clear();
        for (const auto &message : messages) {
            if (!message) continue;
            if (message->type != rtc::Message::Binary) {
                output.push_back(message); continue;
            }
            state->receives.fetch_add(1, std::memory_order_relaxed);
            if (!(message->x4DiagFlags & X4_RD_AUTHENTICATED)) {
                state->unauthenticated.fetch_add(1, std::memory_order_relaxed);
                if (passive) output.push_back(message);
                continue;
            }
            struct Emission {
                rtc::message_vector *output;
                const rtc::message_ptr *source;
                uint32_t media_ssrc;
                State *state;
                rtc::RtcpReceivingSession *rr;
                bool passive;
#if X4_PUBLIC
                bool nack_only;
#endif
            } emission{&output, &message, engine->config.media_ssrc, state.get(), rr.get(), passive
#if X4_PUBLIC
                , !engine->config.mode
#endif
            };
            const auto observe = [](void *opaque) {
                auto &context = *static_cast<Emission *>(opaque);
                // Count media on the wire even if already supplied by repair.
                if (context.rr) context.rr->x4ObserveEdgeVideoRr(**context.source);
#if X4_PUBLIC
                // Preserve native admission for NACK-only; suppress raw RTX/FEC.
                if (context.nack_only && !context.passive) context.output->push_back(*context.source);
#endif
            };
            const auto emit = [](void *opaque, const uint8_t *packet, size_t bytes,
                unsigned method, uint64_t) {
                auto &context = *static_cast<Emission *>(opaque);
                if (bytes >= 12 && method <= X4_REPAIR43_FEC) {
                    const uint16_t sequence = uint16_t(uint16_t(packet[2]) << 8 | packet[3]);
                    const uint32_t timestamp = uint32_t(packet[4]) << 24 |
                        uint32_t(packet[5]) << 16 | uint32_t(packet[6]) << 8 | packet[7];
                    const uint64_t tag = uint64_t(sequence) << 48 | uint64_t(timestamp) << 16 | (method + 1u);
                    context.state->delivered[sequence & 2047u].store(tag, std::memory_order_release);
                }
                if (context.passive) return; // Observations cannot alter baseline delivery.
                if (method == X4_REPAIR43_NORMAL) {
#if X4_PUBLIC
                    if (context.nack_only) return;
#endif
                    context.output->push_back(*context.source);
                    return;
                }
                /* Copy all authenticated boundary/provenance metadata, then
                 * replace wire bytes. The NACK owner produces its own ticket.
                 * The engine counts RTX/FEC; downstream sees ordinary RTP. */
                auto result = std::make_shared<rtc::Message>(**context.source);
                result->resize(bytes);
                std::memcpy(result->data(), packet, bytes);
                result->stream = context.media_ssrc;
                result->x4DiagFlags &= ~X4_RD_RETRANSMISSION;
                result->x4DiagFlags |= X4_RD_AUTHENTICATED | X4_RD_RECOVERY43;
                if (method == X4_REPAIR43_RTX) result->x4DiagFlags |= X4_RD_RTX_NORMALIZED;
                if (method == X4_REPAIR43_FEC) result->x4DiagFlags |= X4_RD_FEC_RECOVERED;
                context.output->push_back(std::move(result));
            };
            const int rc = x4_video_repair43_process_observed(engine->repair,
                reinterpret_cast<const uint8_t *>(message->data()), message->size(),
                x4_rtc_receive_clock(), emit, observe, &emission);
            if (rc < 0) state->errors.fetch_add(1, std::memory_order_relaxed);
            if (passive) output.push_back(message);
        }
        messages.assign(std::make_move_iterator(output.begin()), std::make_move_iterator(output.end()));
        output.clear();
    }
private:
    const std::shared_ptr<State> state_;
    const std::weak_ptr<rtc::RtcpReceivingSession> rr_;
    rtc::message_vector output_;
};
static void install(uint32_t epoch, const std::shared_ptr<rtc::MediaHandler> &receiving) {
    const auto state = lookup(epoch);
    if (!state || !receiving) return;
    /* The pinned public API calls media() before recursing into next().
     * Chain: generator -> ReceivingSession -> Recovery; incoming is last-first. */
    auto tail = receiving;
    while (tail->next()) {
        if (std::dynamic_pointer_cast<Handler>(tail->next())) return;
        tail = tail->next();
    }
    tail->setNext(std::make_shared<Handler>(state,
        std::dynamic_pointer_cast<rtc::RtcpReceivingSession>(receiving)));
}
} // namespace x4_recovery43

extern "C" int x4_recovery43_begin(uint32_t epoch, unsigned mode, unsigned loss, uint32_t seed) {
    if (!epoch || mode > 2) return -1;
#if X4_PUBLIC
    if (loss || seed) return -1;
#else
    if ((loss != 0 && loss != 1 && loss != 3) || (loss && !seed)) return -1;
#endif
    try {
        auto state = std::make_shared<x4_recovery43::State>(epoch, mode
#if !X4_PUBLIC
            , loss, seed
#endif
        );
        std::lock_guard<std::mutex> lock(x4_recovery43::registry_mutex);
        if (x4_recovery43::registry.count(epoch)) return -1;
        x4_recovery43::registry.emplace(epoch, state);
        x4_recovery43::active.store(state.get(), std::memory_order_seq_cst);
        return 0;
    } catch (const std::exception &) { return -1; }
}
extern "C" int x4_recovery43_answer(uint32_t epoch, const X4VideoRepair43Config *accepted) {
    if (!accepted) return -1;
    auto state = x4_recovery43::lookup(epoch);
    if (!state || state->closing.load(std::memory_order_acquire)) return -1;
    auto config = *accepted;
#if X4_PUBLIC
    if ((config.mode && config.mode != state->mode) || config.loss_percent || config.seed) return -1;
#else
    config.mode = state->mode; config.loss_percent = state->loss; config.seed = state->seed;
#endif
    try {
        auto engine = std::make_unique<x4_recovery43::Engine>(config);
        if (!engine->repair) return -1;
        /* Answer is setup-only: replacing a live cache is not permitted. */
        std::lock_guard<std::mutex> lock(x4_recovery43::registry_mutex);
        if (state->closing.load(std::memory_order_acquire) ||
            state->engine_published.load(std::memory_order_acquire)) return -1;
        state->engine = std::move(engine);
        state->engine_published.store(state->engine.get(), std::memory_order_release);
        return 0;
    } catch (const std::exception &) { return -1; }
}
extern "C" bool x4_recovery43_snapshot(uint32_t epoch, X4Recovery43Snapshot *out) {
    if (!out) return false;
    auto state = x4_recovery43::lookup(epoch);
    if (!state) { *out = {}; return false; }
    x4_recovery43::snapshot(state, out); return true;
}
extern "C" void x4_recovery43_note_media(uint32_t epoch, uint64_t incomplete,
    uint64_t discarded, uint64_t pli, uint64_t freezes) {
    x4_recovery43::MetadataLease lease;
    auto *state = lease.state;
#if X4_RECOVERY43_HOST_TEST
    if (state) x4_recovery43_test_reader_acquired(epoch);
#endif
    if (!state || state->epoch != epoch || state->closing.load(std::memory_order_acquire)) return;
    state->incomplete.fetch_add(incomplete, std::memory_order_relaxed);
    state->discarded.fetch_add(discarded, std::memory_order_relaxed);
    state->pli.fetch_add(pli, std::memory_order_relaxed);
    state->freezes.fetch_add(freezes, std::memory_order_relaxed);
}
extern "C" void x4_recovery43_delivery(uint32_t epoch, uint16_t sequence,
    uint32_t timestamp, unsigned reason) {
    x4_recovery43::MetadataLease lease;
    auto *state = lease.state;
#if X4_RECOVERY43_HOST_TEST
    if (state) x4_recovery43_test_reader_acquired(epoch);
#endif
    if (!state || state->epoch != epoch || state->closing.load(std::memory_order_acquire) ||
        (reason != X4_RECOVERY43_DELIVERY_LATE && reason != X4_RECOVERY43_DELIVERY_DUPLICATE)) return;
    const uint64_t tag = state->delivered[sequence & 2047u].load(std::memory_order_acquire);
    const unsigned method_tag = unsigned(tag & UINT64_C(0xffff));
    if (uint16_t(tag >> 48) != sequence || uint32_t(tag >> 16) != timestamp ||
        method_tag < 1 || method_tag > 3) {
        state->delivery_unknown.fetch_add(1, std::memory_order_relaxed); return;
    }
    auto &counter = reason == X4_RECOVERY43_DELIVERY_LATE ? state->delivery_late[method_tag - 1] :
        state->delivery_duplicate[method_tag - 1];
    counter.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void x4_recovery43_poll(uint32_t epoch, uint64_t now_us, bool final) {
    const auto state = x4_recovery43::lookup(epoch);
#if X4_PUBLIC
    (void)now_us;
    if (!state || !final) return;
#else
    if (!state || (!final && state->last_log_us && now_us - state->last_log_us < 10000000)) return;
    state->last_log_us = now_us;
#endif
    auto &out = *state->scratch;
    x4_recovery43::snapshot(state, &out);
    const auto &s = out.repair;
#if X4_PUBLIC
    const uint64_t errors = out.engine_errors + s.format_errors[0] + s.format_errors[1] + s.format_errors[2];
    x4_log_printf("XCloud4: recuperacion43 resumen detectados=%llu cerrados_normal=%llu cerrados_rtx=%llu "
        "cerrados_fec=%llu vencidos=%llu fec=%llu pli=%llu rx_contencion=%llu errores=%llu\n",
        (unsigned long long)s.gaps_detected, (unsigned long long)s.gaps_closed[0],
        (unsigned long long)s.gaps_closed[1], (unsigned long long)s.gaps_closed[2],
        (unsigned long long)s.gaps_expired, (unsigned long long)s.packets[2],
        (unsigned long long)out.pli_requested, (unsigned long long)out.receive_contention,
        (unsigned long long)errors);
#else
    const uint64_t closed = s.gaps_closed[0] + s.gaps_closed[1] + s.gaps_closed[2];
    const uint64_t unclosed = s.gaps_detected > closed ? s.gaps_detected - closed : 0;
    x4_log_printf("XCloud4: recuperacion43 final=%u modo=%u perdida=%u semilla=%u pasivo=%u configurado=%u "
        "detectados=%llu cerrados=%llu/%llu/%llu expirados=%llu sin_cerrar=%llu "
        "rtx=%llu fec=%llu formato=%llu/%llu/%llu duplicados=%llu/%llu/%llu "
        "tardios=%llu/%llu/%llu incompletas=%llu descartadas=%llu pli=%llu congelamientos_observados=%llu "
        "rx_contencion=%llu errores=%llu memoria=%llu\n", final ? 1u : 0u, state->mode,
        state->loss, state->seed, (!state->mode&&!state->loss)?1u:0u, out.configured?1u:0u,
        (unsigned long long)s.gaps_detected,
        (unsigned long long)s.gaps_closed[0], (unsigned long long)s.gaps_closed[1],
        (unsigned long long)s.gaps_closed[2], (unsigned long long)s.gaps_expired,
        (unsigned long long)unclosed,
        (unsigned long long)s.packets[1], (unsigned long long)s.packets[2],
        (unsigned long long)s.format_errors[0], (unsigned long long)s.format_errors[1],
        (unsigned long long)s.format_errors[2], (unsigned long long)s.duplicate[0],
        (unsigned long long)s.duplicate[1], (unsigned long long)s.duplicate[2],
        (unsigned long long)s.late[0], (unsigned long long)s.late[1], (unsigned long long)s.late[2],
        (unsigned long long)out.incomplete_au, (unsigned long long)out.discarded_au,
        (unsigned long long)out.pli_requested, (unsigned long long)out.freezes,
        (unsigned long long)out.receive_contention, (unsigned long long)out.engine_errors,
        (unsigned long long)s.allocated_bytes);
    x4_log_printf("XCloud4: recuperacion43 formatos no_soportado=%llu ajenos=%llu rtx_vacios=%llu "
        "fec_sin_falta=%llu fec_varias_faltas=%llu fec_expirados=%llu\n",
        (unsigned long long)s.unsupported, (unsigned long long)s.foreign, (unsigned long long)s.rtx_empty,
        (unsigned long long)s.fec_no_missing, (unsigned long long)s.fec_multiple_missing,
        (unsigned long long)s.fec_cache_expired);
    for (unsigned method = 0; method < 3; ++method)
        x4_log_printf("XCloud4: recuperacion43 tiempo metodo=%u muestras=%llu p50_us=%llu p95_us=%llu "
            "p50_finito=%u p95_finito=%u max_us=%llu simulados=%llu\n", method,
            (unsigned long long)s.closure_samples[method], (unsigned long long)s.closure_p50_us[method],
            (unsigned long long)s.closure_p95_us[method], s.closure_p50_finite[method] ? 1u : 0u,
            s.closure_p95_finite[method] ? 1u : 0u, (unsigned long long)s.closure_max_us[method],
            (unsigned long long)s.lost_simulated[method]);
    x4_log_printf("XCloud4: recuperacion43 admision tarde=%llu/%llu/%llu duplicado=%llu/%llu/%llu "
        "sin_correspondencia=%llu memoria_mapa=%u\n", (unsigned long long)out.delivery_late[0],
        (unsigned long long)out.delivery_late[1], (unsigned long long)out.delivery_late[2],
        (unsigned long long)out.delivery_duplicate[0], (unsigned long long)out.delivery_duplicate[1],
        (unsigned long long)out.delivery_duplicate[2], (unsigned long long)out.delivery_unknown,
        unsigned(sizeof(state->delivered)));
    if (final) for (unsigned h = 0; h < s.header_count && h < X4_REPAIR43_MAX_HEADERS; ++h) {
        char hex[X4_REPAIR43_HEADER_CAP * 2 + 1]{};
        constexpr char digits[] = "0123456789abcdef";
        const unsigned bytes = std::min(s.header_bytes[h], unsigned(X4_REPAIR43_HEADER_CAP));
        for (unsigned i = 0; i < bytes; ++i) {
            hex[2*i] = digits[s.headers[h][i] >> 4]; hex[2*i+1] = digits[s.headers[h][i] & 15];
        }
        x4_log_printf("XCloud4: recuperacion43 fec_cabecera=%u bytes=%u hex=%s\n", h + 1, bytes, hex);
    }
#endif
}
extern "C" void x4_recovery43_end(uint32_t epoch) {
    std::shared_ptr<x4_recovery43::State> state;
    {
        std::lock_guard<std::mutex> lock(x4_recovery43::registry_mutex);
        const auto found = x4_recovery43::registry.find(epoch);
        if (found == x4_recovery43::registry.end()) return;
        state = found->second;
        if (state->closing.load(std::memory_order_acquire)) return;
        auto *expected = state.get();
        (void)x4_recovery43::active.compare_exchange_strong(expected, nullptr,
            std::memory_order_seq_cst, std::memory_order_seq_cst);
        state->closing.store(true, std::memory_order_release);
    }
    /* Cold shutdown only, outside the setup mutex. The local shared owner
     * protects raw readers. No wait, allocation or lock enters their APIs. */
#if X4_RECOVERY43_HOST_TEST
    x4_recovery43_test_end_waiting(epoch);
#endif
    while (x4_recovery43::metadata_readers.load(std::memory_order_seq_cst)) {
#if X4_RECOVERY43_HOST_TEST
        std::this_thread::yield();
#else
        (void)sceKernelUsleep(50);
#endif
    }
    /* closing prevents new cache owners; an already-entered handler keeps
     * its State and finishes before the final atomic snapshot is recorded. */
    while (state->receiving.test_and_set(std::memory_order_acquire)) {
#if X4_RECOVERY43_HOST_TEST
        std::this_thread::yield();
#else
        (void)sceKernelUsleep(50);
#endif
    }
    state->receiving.clear(std::memory_order_release);
    x4_recovery43_poll(epoch, x4_rtc_receive_clock(), true);
    {
        std::lock_guard<std::mutex> lock(x4_recovery43::registry_mutex);
        const auto found = x4_recovery43::registry.find(epoch);
        if (found != x4_recovery43::registry.end() && found->second == state)
            x4_recovery43::registry.erase(found);
    }
    /* Handler callbacks and snapshots retain shared_ptrs. Destruction occurs
     * only after the final owner/observer has returned, never during process. */
}
