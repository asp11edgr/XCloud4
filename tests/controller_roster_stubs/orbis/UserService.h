/* SPDX-License-Identifier: GPL-3.0-only */
/* Host-only user service fixture, with four login slots. */
#pragma once
#include <stddef.h>
#include <stdint.h>
enum {
    ORBIS_USER_SERVICE_MAX_LOGIN_USERS = 4,
    ORBIS_USER_SERVICE_USER_ID_SYSTEM = 0xff,
    ORBIS_USER_SERVICE_USER_ID_INVALID = -1,
    ORBIS_KERNEL_PRIO_FIFO_LOWEST = 0x2ff,
    SCE_USER_SERVICE_EVENT_TYPE_LOGIN = 0,
    SCE_USER_SERVICE_EVENT_TYPE_LOGOUT = 1
};
typedef struct { unsigned priority; } OrbisUserServiceInitializeParams;
typedef struct { int32_t userId[ORBIS_USER_SERVICE_MAX_LOGIN_USERS]; } OrbisUserServiceLoginUserIdList;
typedef struct { int event; int32_t userId; } OrbisUserServiceEvent;
int sceUserServiceInitialize(void *);
int sceUserServiceGetInitialUser(int32_t *);
int sceUserServiceGetUserName(int32_t, char *, size_t);
int sceUserServiceGetLoginUserIdList(OrbisUserServiceLoginUserIdList *);
int sceUserServiceGetEvent(OrbisUserServiceEvent *);
