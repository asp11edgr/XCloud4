/* Deliberately small Opus double API; the product still includes real Opus. */
#pragma once
#include <stdint.h>
typedef int32_t opus_int32;
typedef uint32_t opus_uint32;
typedef struct OpusDecoder { unsigned resets, calls, plc_calls; } OpusDecoder;
enum { OPUS_OK = 0, OPUS_RESET_STATE = 4028, OPUS_GET_FINAL_RANGE_REQUEST = 4031 };
#define OPUS_GET_FINAL_RANGE(out) OPUS_GET_FINAL_RANGE_REQUEST, (out)
OpusDecoder *opus_decoder_create(int, int, int *);
void opus_decoder_destroy(OpusDecoder *);
int opus_decoder_ctl(OpusDecoder *, int, ...);
int opus_decode(OpusDecoder *, const unsigned char *, opus_int32, int16_t *, int, int);
int opus_packet_get_nb_samples(const unsigned char *, opus_int32, int);
int opus_packet_get_samples_per_frame(const unsigned char *, opus_int32);
int opus_packet_has_lbrr(const unsigned char *, opus_int32);
