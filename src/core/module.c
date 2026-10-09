/* SPDX-License-Identifier: GPL-3.0-only */
#include "module.h"
#include <stdio.h>
#include <string.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>

static int loaded_handle(const char *name)
{
    OrbisKernelModule handles[256];
    size_t count = 0;
    int rc = sceKernelGetModuleList(handles, sizeof(handles) / sizeof(handles[0]), &count);
    if (rc < 0) return rc;
    if (count > 256) count = 256;
    for (size_t i = 0; i < count; ++i) {
        OrbisKernelModuleInfo info = {.size = sizeof(info)};
        if (sceKernelGetModuleInfo(handles[i], &info) >= 0) {
            info.name[sizeof(info.name) - 1] = 0;
            const char *base = strrchr(info.name, '/');
            base = base ? base + 1 : info.name;
            char with_extension[128];
            snprintf(with_extension, sizeof(with_extension), "%s.sprx", name);
            if (!strcmp(base, name) || !strcmp(base, with_extension)) return (int)handles[i];
        }
    }
    return -1;
}

int x4_module_open(const char *name)
{
    int handle = loaded_handle(name);
    if (handle >= 0) return handle;
    uint32_t id = 0;
    int internal = 1;
    if (!strcmp(name, "libSceVideodec2")) { id = ORBIS_SYSMODULE_VIDEODEC2; internal = 0; }
    else if (!strcmp(name, "libSceAudioOut")) id = ORBIS_SYSMODULE_INTERNAL_AUDIOOUT;
    else if (!strcmp(name, "libSceSystemService")) id = ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE;
    else if (!strcmp(name, "libSceNet")) id = ORBIS_SYSMODULE_INTERNAL_NET;
    else if (!strcmp(name, "libSceHttp")) id = ORBIS_SYSMODULE_INTERNAL_HTTP;
    else if (!strcmp(name, "libSceSsl")) id = ORBIS_SYSMODULE_INTERNAL_SSL;
    if (id) {
        int sysmodule = x4_module_open("libSceSysmodule");
        if (sysmodule >= 0) {
            void *address = NULL;
            const char *symbol = internal ? "sceSysmoduleLoadModuleInternal" : "sceSysmoduleLoadModule";
            if (x4_module_symbol(sysmodule, symbol, &address) >= 0) {
                int32_t (*load)(uint32_t) = (int32_t (*)(uint32_t))address;
                int rc = load(id);
                printf("XCloud4: Sysmodule %s -> 0x%08x\n", name, (unsigned)rc);
                handle = loaded_handle(name);
                if (handle >= 0) return handle;
            }
        }
    }
    char path[128];
    snprintf(path, sizeof(path), "/system/common/lib/%s.sprx", name);
    handle = (int32_t)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
    if (handle < 0) {
        int existing = loaded_handle(name);
        if (existing >= 0) handle = existing;
    }
    printf("XCloud4: modulo %s -> 0x%08x\n", name, (unsigned)handle);
    return handle;
}

int x4_module_symbol(int handle, const char *name, void **address)
{
    *address = NULL;
    int rc = sceKernelDlsym(handle, name, address);
    if (rc >= 0 && !*address) rc = -1;
    if (rc < 0) printf("XCloud4: simbolo %s -> 0x%08x\n", name, (unsigned)rc);
    return rc;
}
