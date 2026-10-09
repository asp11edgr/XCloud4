/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* Public view of one Xbox cloud session preparation. This milestone asks
 * Xbox to provision a cloud session, waits until the server reports it ready
 * to negotiate, then authorizes the connection: the Microsoft access is
 * renewed, a console-transfer token is obtained from Microsoft and sent to
 * the session's /connect. No WebRTC, SDP or ICE is sent and no game is
 * rendered. Every prepared session is deleted again before the worker
 * releases its private credentials.
 *
 *   IDLE -> STARTING -> WAITING -> READY -> AUTHORIZING -> AUTHORIZED
 *        -> STOPPING -> CLOSED
 *   any active state -> STOPPING -> CANCELLED (user cancel, cleanup confirmed)
 *   any state -> ERROR (service/transport failure, deadline or failed cleanup)
 *
 * READY is set only for the server states ReadyToConnect or Provisioned and
 * means "ready to negotiate", not streaming. ready_seen stays 1 afterwards.
 * AUTHORIZING is published before the Microsoft exchanges start. AUTHORIZED
 * only means Xbox accepted the /connect request (2xx without errorDetails);
 * it does not mean a media connection exists. connection_authorized stays 1
 * afterwards, also once the session was closed. cleanup_failed is 1 whenever
 * the remote session could not be confirmed closed; it is never cleared by
 * a cancellation. */
enum X4SessionState {
    X4_SESSION_IDLE,
    X4_SESSION_STARTING,
    X4_SESSION_WAITING,
    X4_SESSION_READY,
    X4_SESSION_STOPPING,
    X4_SESSION_CLOSED,
    X4_SESSION_CANCELLED,
    X4_SESSION_ERROR,
    X4_SESSION_AUTHORIZING,
    X4_SESSION_AUTHORIZED,
};

/* Holds no token, session path, session ID or server body. stage is always
 * copied from a constant literal. passport_http_status is the status of the
 * last Microsoft exchange of the authorization step (access renewal or
 * console-transfer token); connect_http_status that of /connect; 0 when the
 * exchange did not return a status. */
typedef struct {
    enum X4SessionState state;
    int error, http_status;
    int cleanup_error, cleanup_http_status;
    int ready_seen, cleanup_failed;
    int connection_authorized, passport_http_status, connect_http_status;
    unsigned elapsed_seconds, seconds_left;
    char stage[80], region[80], offering[24], title_name[128];
} X4SessionSnapshot;
