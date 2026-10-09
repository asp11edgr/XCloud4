/* SPDX-License-Identifier: GPL-3.0-only */
#include "module.h"
#include <stdio.h>
#include <string.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>

#define X4_MODULE_AMBIGUOUS (-2)

static const char *module_base(OrbisKernelModuleInfo *info)
{
    info->name[sizeof(info->name) - 1] = 0;
    const char *base = strrchr(info->name, '/');
    return base ? base + 1 : info->name;
}

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
            const char *base = module_base(&info);
            char with_extension[128];
            snprintf(with_extension, sizeof(with_extension), "%s.sprx", name);
            if (!strcmp(base, name) || !strcmp(base, with_extension)) return (int)handles[i];
        }
    }
    return -1;
}

/* Some firmware modules do not report their library name through GetModuleInfo.
 * Probe the reviewed network/random exports; media lookup is unchanged.
 * A Sysmodule success code is never a kernel module handle. */
static int network_handle(const char *name, int dump)
{
    const char *init, *term;
    if (!strcmp(name, "libSceSsl")) { init = "sceSslInit"; term = "sceSslTerm"; }
    else if (!strcmp(name, "libSceHttp")) { init = "sceHttpInit"; term = "sceHttpTerm"; }
    else if (!strcmp(name, "libSceRandom")) { init = "sceRandomGetRandomNumber"; term = NULL; }
    else if (!strcmp(name, "libSceNetCtl")) { init = "sceNetCtlGetInfo"; term = "sceNetCtlGetState"; }
    else return -1;
    OrbisKernelModule handles[256];
    size_t count = 0;
    int rc = sceKernelGetModuleList(handles, sizeof(handles) / sizeof(handles[0]), &count);
    if (rc < 0) return rc;
    if (count > 256) { printf("XCloud4: lista de modulos truncada\n"); count = 256; }
    int found = -1;
    unsigned hits = 0;
    void *first_init = NULL, *first_term = NULL;
    for (size_t i = 0; i < count; ++i) {
        OrbisKernelModuleInfo info = {.size = sizeof(info)};
        int info_rc = sceKernelGetModuleInfo(handles[i], &info);
        const char *base = info_rc >= 0 ? module_base(&info) : "nombre no disponible";
        if (dump) printf("XCloud4: modulo cargado %s h=0x%08x info=0x%08x\n",
                         base, (unsigned)handles[i], (unsigned)info_rc);
        void *init_address = NULL, *term_address = NULL;
        /* Raw probes deliberately avoid an error log for every missing export. */
        if (sceKernelDlsym(handles[i], init, &init_address) >= 0 && init_address &&
            (!term || (sceKernelDlsym(handles[i], term, &term_address) >= 0 && term_address))) {
            printf("XCloud4: API %s en %s h=0x%08x\n", init, base, (unsigned)handles[i]);
            if (!hits) {
                found = (int)handles[i];
                first_init = init_address;
                first_term = term_address;
                hits = 1;
            } else if (init_address != first_init || term_address != first_term) {
                ++hits;
            }
        }
    }
    if (hits > 1) {
        printf("XCloud4: API %s ambigua (%u modulos)\n", init, hits);
        return X4_MODULE_AMBIGUOUS;
    }
    return found;
}

static int load_path(const char *path, const char *name, const char *location)
{
    int handle = (int32_t)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
    /* Do not log the sandbox word or full path. */
    printf("XCloud4: cargar %s (%s) -> 0x%08x\n", name, location, (unsigned)handle);
    if (handle >= 0) return handle;
    int existing = loaded_handle(name);
    if (existing < 0) existing = network_handle(name, 0);
    return existing >= 0 || existing == X4_MODULE_AMBIGUOUS ? existing : handle;
}

int x4_module_open(const char *name)
{
    int handle = loaded_handle(name);
    if (handle >= 0) return handle;
    uint32_t id = 0;
    int internal = 1;
    if (!strcmp(name, "libSceVideodec2")) { id = ORBIS_SYSMODULE_VIDEODEC2; internal = 0; }
    else if (!strcmp(name, "libSceRandom")) { id = ORBIS_SYSMODULE_RANDOM; internal = 0; }
    else if (!strcmp(name, "libSceAudioOut")) id = ORBIS_SYSMODULE_INTERNAL_AUDIOOUT;
    else if (!strcmp(name, "libSceSystemService")) id = ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE;
    else if (!strcmp(name, "libSceNet")) id = ORBIS_SYSMODULE_INTERNAL_NET;
    else if (!strcmp(name, "libSceNetCtl")) id = ORBIS_SYSMODULE_INTERNAL_NETCTL;
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
    static unsigned network_dumped;
    unsigned bit = !strcmp(name, "libSceSsl") ? 1u : !strcmp(name, "libSceHttp") ? 2u : 0u;
    handle = network_handle(name, bit && !(network_dumped & bit));
    network_dumped |= bit;
    if (handle >= 0 || handle == X4_MODULE_AMBIGUOUS) return handle;

    char path[512];
    const char *word = bit ? sceKernelGetFsSandboxRandomWord() : NULL;
    if (word) {
        size_t length = 0;
        while (length < 128 && word[length]) ++length;
        int valid = length > 0 && length < 128;
        for (size_t i = 0; valid && i < length; ++i) {
            unsigned char c = (unsigned char)word[i];
            valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
        }
        if (valid) {
            int size = snprintf(path, sizeof(path), "/%s/common/lib/%s.sprx", word, name);
            if (size > 0 && (size_t)size < sizeof(path)) {
                handle = load_path(path, name, "sandbox common");
                if (handle >= 0 || handle == X4_MODULE_AMBIGUOUS) return handle;
            }
        }
    }
    snprintf(path, sizeof(path), "/system/common/lib/%s.sprx", name);
    handle = load_path(path, name, "system common");
    if (handle < 0 && handle != X4_MODULE_AMBIGUOUS && bit) {
        snprintf(path, sizeof(path), "/system/priv/lib/%s.sprx", name);
        handle = load_path(path, name, "system priv");
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
