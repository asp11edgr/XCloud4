/* SPDX-License-Identifier: GPL-3.0-only */
#include "lifecycle.h"
#include "module.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static int32_t (*load_exec)(const char *, const char *[]);

int x4_exit_prepare(void)
{
    if (load_exec) return 0;
    int handle = x4_module_open("libSceSystemService");
    if (handle < 0) return handle;
    void *address = NULL;
    int rc = x4_module_symbol(handle, "sceSystemServiceLoadExec", &address);
    if (rc >= 0) load_exec = (int32_t (*)(const char *, const char *[]))address;
    return rc;
}

int x4_exit_request(void)
{
    if (!load_exec) return -1;
    printf("XCloud4: solicitar salida al menu PS4\n");
    int rc = load_exec("exit", NULL);
    printf("XCloud4: LoadExec exit -> 0x%08x\n", (unsigned)rc);
    return rc;
}
