/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* Microsoft public-client profile, fixed at compile time for the whole
 * process. Device-code clients have no secret. Switching profile requires a
 * complete new device sign-in: tokens live only in RAM and are never carried
 * from one client ID to another. */

/* XCloud4 app registered by the project owner for personal accounts. */
#define X4_AUTH_OWN_CLIENT_ID "f9ac8684-1032-4131-bb47-d2f58da9bb93"
/* Public client ID used by GreenVita (Day-OS green-vita,
 * src/api_xbox/auth.rs), used temporarily with the owner's explicit
 * authorization until the own registration works. */
#define X4_AUTH_REFERENCE_CLIENT_ID "1f907974-e22b-4810-a9de-d9647380c97e"

#ifndef X4_AUTH_USE_REFERENCE
#define X4_AUTH_USE_REFERENCE 1 /* temporary for 0.6.2 */
#endif

#if X4_AUTH_USE_REFERENCE != 0 && X4_AUTH_USE_REFERENCE != 1
#error "X4_AUTH_USE_REFERENCE must be 0 or 1"
#endif

#if X4_AUTH_USE_REFERENCE
#define X4_AUTH_CLIENT_ID X4_AUTH_REFERENCE_CLIENT_ID
#define X4_AUTH_PROFILE_NOTE "ACCESO TEMPORAL: CLIENTE DE REFERENCIA USADO POR GREENVITA."
#define X4_AUTH_PROFILE_PROMPT "AUTORIZA EN MICROSOFT. CLIENTE TEMPORAL USADO POR GREENVITA."
#else
#define X4_AUTH_CLIENT_ID X4_AUTH_OWN_CLIENT_ID
#define X4_AUTH_PROFILE_NOTE "ACCESO CON EL REGISTRO PROPIO DE XCLOUD4."
#define X4_AUTH_PROFILE_PROMPT "AUTORIZA XCLOUD4 DESDE TU TELEFONO O PC."
#endif
