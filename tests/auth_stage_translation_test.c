/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/ui/lang.c"

int main(void)
{
    /* Keep the existing language invariants: every ID has both languages,
     * invalid IDs are harmless, locale/market selection remains unchanged. */
    x4_lang_set(X4_LANG_ES,"es-MX","MX");
    assert(x4_lang_current()==X4_LANG_ES);
    assert(!strcmp(x4_lang_locale(),"es-MX"));
    assert(!strcmp(x4_lang_market(),"MX"));
    assert(!strcmp(x4_lang_store_category("Racing & flying"),"Carreras y vuelo"));
    assert(!strcmp(x4_lang_store_category("Shooter"),"Disparos"));
    assert(!strcmp(x4_lang_store_category("Action & adventure"),"Acción y aventura"));
    const char *unknown_category = "New Store category";
    assert(x4_lang_store_category(unknown_category) == unknown_category);
    assert(!strcmp(x4_lang_store_category(NULL),""));
    assert(!strcmp(x4_tr(X4_TR_CAT_HOME_LEAVING),"Se van pronto"));
    for (int i=0;i<X4_TR_ALL_COUNT;++i) {
        if (i==X4_TR_COUNT) continue;
#if X4_PUBLIC
        /* Lab-only legacy IDs stay absent in both public tables. */
        if (!table_es[i] && !table_en[i]) {
            assert(!*x4_tr((enum X4TextId)i));
            continue;
        }
#endif
        assert(table_es[i] && table_es[i][0]);
        assert(table_en[i] && table_en[i][0]);
        assert(!strcmp(x4_tr((enum X4TextId)i),table_es[i]));
    }
    assert(!strcmp(x4_tr((enum X4TextId)-1),""));
    assert(!strcmp(x4_tr(X4_TR_COUNT),""));
    const char *warning="Se guardará un acceso a tu cuenta de Microsoft en esta PS4 para no tener que iniciar sesión cada vez. Cualquiera con acceso a esta consola podrá usar tu cuenta en XCloud4. Se borra al cerrar sesión";
    assert(!strcmp(x4_tr(X4_TR_AUTH_REMEMBER_WARNING_BODY),warning));
    for (size_t i=0;i<sizeof(stage_map)/sizeof(stage_map[0]);++i) {
        const char *expected = stage_translation(i, X4_LANG_ES);
        assert(!strcmp(x4_tr_stage(stage_map[i].es), expected));
    }
    assert(!strcmp(x4_tr_stage("autorizacion de conexion no valida"),
        "autorización de conexión no válida"));
    assert(!strcmp(x4_tr_stage("HTTPS: resolucion DNS"), "HTTPS: resolución DNS"));
    assert(!strcmp(x4_tr_stage("preparando sesion Xbox"), "preparando sesión Xbox"));
    assert(!strcmp(x4_tr_stage("Passport: aplicacion no aceptada (invalid_client)"),
        "Passport: aplicación no aceptada (invalid_client)"));
    assert(!strcmp(x4_tr_stage("Xbox rechazo la autorizacion XSTS"),
        "Xbox rechazó la autorización (XSTS)"));

    x4_lang_set(X4_LANG_EN,"en-US","US");
    assert(!strcmp(x4_lang_store_category("Racing & flying"),"Racing & flying"));
    assert(x4_lang_store_category(unknown_category) == unknown_category);
    assert(!strcmp(x4_tr(X4_TR_CAT_HOME_LEAVING),"Leaving soon"));
    for (int i=0;i<X4_TR_ALL_COUNT;++i) {
        if (i==X4_TR_COUNT) continue;
#if X4_PUBLIC
        /* Lab-only legacy IDs stay absent in both public tables. */
        if (!table_es[i] && !table_en[i]) {
            assert(!*x4_tr((enum X4TextId)i));
            continue;
        }
#endif
        assert(!strcmp(x4_tr((enum X4TextId)i),table_en[i]));
    }
    for (size_t i=0;i<sizeof(stage_map)/sizeof(stage_map[0]);++i) {
        assert(strcmp(stage_map[i].es,stage_map[i].en));
        assert(!strcmp(x4_tr_stage(stage_map[i].es),stage_map[i].en));
        /* The original regression came from a wrapped HTTP stage. */
        char original[256],expected[256];
        snprintf(original,sizeof(original),"HTTPS: %s",stage_map[i].es);
        snprintf(expected,sizeof(expected),"HTTPS: %s",stage_map[i].en);
        assert(!strcmp(x4_tr_stage(original),expected));
    }
    assert(!strcmp(x4_tr_stage("HTTPS: envio HTTPS/TLS"),"HTTPS: HTTPS/TLS send"));
    assert(!strcmp(x4_tr_stage("negociacion TLS"),"TLS handshake"));
    assert(!strcmp(x4_tr_stage("certificado TLS"),"TLS certificate"));
    assert(!strcmp(x4_tr_stage("certificado TLS: revisa fecha y hora"),"TLS certificate: check date and time"));
    assert(!strcmp(x4_tr_stage("HTTPS: negociacion TLS"),"HTTPS: TLS handshake"));
    assert(!strcmp(x4_tr_stage("HTTPS: certificado TLS"),"HTTPS: TLS certificate"));
    assert(!strcmp(x4_tr_stage("HTTPS: certificado TLS: revisa fecha y hora"),"HTTPS: TLS certificate: check date and time"));
    assert(!strcmp(x4_tr_stage("Xbox está preparando el juego… 17 s"),
        "Xbox is getting your game ready… 17 s"));
    assert(!strcmp(x4_tr_stage("Esperando 1080p de Xbox…"),"Waiting for 1080p from Xbox…"));
    assert(!strcmp(x4_tr_stage("Xbox cerró la sesión: SessionNotActive"),
        "Xbox closed the session: SessionNotActive"));
    assert(!strcmp(x4_tr_stage("Se perdió la conexión con el servidor de Xbox: 0x00000003"),
        "The connection to the Xbox server was lost: 0x00000003"));
    assert(!strcmp(x4_tr_stage("Xbox cerró la sesión: el juego se cerró (0x80000001)"),
        "Xbox closed the session: the game closed (0x80000001)"));
    assert(!strcmp(x4_tr(X4_TR_RECONNECTING_FMT),"Reconnecting… (attempt %u of 20)"));
    assert(!strcmp(x4_tr(X4_TR_CONNECTION_RESTORED),"Connection restored"));
    assert(!strcmp(x4_tr(X4_TR_TOUCH_MENU_RELOAD),"Reload stream"));
    assert(!strcmp(x4_tr_stage(NULL),""));
    assert(!strcmp(x4_tr_stage(""),""));
    assert(!strcmp(x4_tr_stage("NetInit"),"NetInit"));
    x4_lang_apply_system_id(ORBIS_SYSTEM_PARAM_LANG_SPANISH_LA);
    assert(x4_lang_current()==X4_LANG_ES && !strcmp(x4_lang_locale(),"es-MX"));
    x4_lang_apply_system_id(ORBIS_SYSTEM_PARAM_LANG_ENGLISH_GB);
    assert(x4_lang_current()==X4_LANG_EN && !strcmp(x4_lang_locale(),"en-GB"));
    x4_lang_apply_system_id(98765);
    assert(x4_lang_current()==X4_LANG_EN && !strcmp(x4_lang_locale(),"en-US"));
    printf("PASS: actual language tables, account warning and %zu stage translations (plain and HTTPS-wrapped).\n",
        sizeof(stage_map)/sizeof(stage_map[0]));
    return 0;
}
