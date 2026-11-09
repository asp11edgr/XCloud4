/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded HTTP/1.1 framing; no sockets, clock, allocation, or logging. */
#pragma once
#include "http_client.h"

enum { X4_HTTP_WIRE_HEADERS_MAX=32768, X4_HTTP_WIRE_LINES_MAX=100 };
enum { X4_HTTP_WIRE_MORE=0, X4_HTTP_WIRE_DONE=1 };

typedef int (*X4HttpWireSink)(void *context,const void *bytes,size_t size);
typedef struct {
    const char *method; /* GET / POST / DELETE; policy validation stays above. */
    const char *host,*target; /* exact DNS name and origin-form request target */
    const char *accept,*content_type,*user_agent;
    const X4HttpHeader *headers;
    size_t header_count,payload_size;
    bool keepalive;
} X4HttpWireRequest;
/* Writes only the request line and headers, ending in CRLF CRLF. The caller
 * sends its separate payload afterwards. sink returns 0 or a negative error.
 * Reserved/framing headers and Cookie cannot be injected through headers[]. */
int x4_http_wire_write_request(const X4HttpWireRequest *,X4HttpWireSink,void *);

typedef struct {
    int status;
    size_t body_bytes;
    bool reusable,response_started,complete;
    uint64_t retry_after_ms;
} X4HttpWireResult;

/* Small caller-owned state: safe on the X4Http heap, not a 32KiB worker stack
 * array. Unknown header values stream past without being retained. */
typedef struct {
    unsigned state,line_kind,header_bytes,header_lines,line_length,name_length;
    unsigned value_length,value_stored,status_pos,interim_count;
    int status,error;
    bool cr,colon,http10,cl_seen,te_seen,ce_seen,connection_close,connection_keepalive;
    bool response_started,complete,retry_seen,retry_invalid,body_forbidden;
    bool cl_digit,cl_after_digit,cl_list_value,cl_header_value;
    uint64_t content_length,remaining,chunk_size,cl_number;
    unsigned chunk_digits,chunk_ext_state;
    char name[32],value[96],status_prefix[13];
    unsigned char *body;
    size_t capacity,body_bytes;
    int64_t now_seconds;
    uint64_t retry_after_ms;
} X4HttpWire;

/* capacity includes the trailing NUL, including for binary responses. */
void x4_http_wire_init(X4HttpWire *,void *body,size_t capacity,int64_t utc_now_seconds);
/* Accept arbitrary fragments. Stops at the exact message boundary. A caller
 * must reject unconsumed bytes (no pipelining). DONE proves entity EOF for
 * CL and chunked, including the zero chunk and complete trailers. Close-
 * delimited responses require eof(). Errors use existing X4_HTTP_* codes.
 * A full body buffer is successful only after that entity EOF is proven. */
int x4_http_wire_feed(X4HttpWire *,const void *,size_t,size_t *consumed);
int x4_http_wire_eof(X4HttpWire *);
X4HttpWireResult x4_http_wire_result(const X4HttpWire *);
/* Pure existing Retry-After semantics: delta seconds / IMF-fixdate, saturated
 * milliseconds; no locale/timegm dependency. Invalid/past values return 0. */
uint64_t x4_http_wire_retry_after(const char *,size_t,int64_t utc_now_seconds);
