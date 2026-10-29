/* SPDX-License-Identifier: GPL-3.0-only */
#include "lang.h"
#include "../core/build_identity.h"
#include <stdio.h>
#include <string.h>
#ifdef X4_OPENORBIS
#include <orbis/SystemService.h>
#else
enum {
    ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG = 1,
    ORBIS_SYSTEM_PARAM_LANG_JAPANESE = 0,
    ORBIS_SYSTEM_PARAM_LANG_ENGLISH_US = 1,
    ORBIS_SYSTEM_PARAM_LANG_FRENCH = 2,
    ORBIS_SYSTEM_PARAM_LANG_SPANISH = 3,
    ORBIS_SYSTEM_PARAM_LANG_GERMAN = 4,
    ORBIS_SYSTEM_PARAM_LANG_ITALIAN = 5,
    ORBIS_SYSTEM_PARAM_LANG_DUTCH = 6,
    ORBIS_SYSTEM_PARAM_LANG_PORTUGUESE_PT = 7,
    ORBIS_SYSTEM_PARAM_LANG_PORTUGUESE_BR = 17,
    ORBIS_SYSTEM_PARAM_LANG_ENGLISH_GB = 18,
    ORBIS_SYSTEM_PARAM_LANG_SPANISH_LA = 20,
    ORBIS_SYSTEM_PARAM_LANG_FRENCH_CA = 22,
};
#endif

static enum X4Lang current_lang = X4_LANG_ES;
static char current_locale[16] = "es-MX";
static char current_market[8] = "MX";
static char current_store_suffix[128] = "&market=MX&languages=es-MX&fieldsTemplate=Details";

void x4_lang_set(enum X4Lang lang, const char *locale, const char *market)
{
    current_lang = (lang == X4_LANG_ES) ? X4_LANG_ES : X4_LANG_EN;
    snprintf(current_locale, sizeof(current_locale), "%s", (locale && locale[0]) ? locale : "en-US");
    snprintf(current_market, sizeof(current_market), "%s", (market && market[0]) ? market : "US");
    snprintf(current_store_suffix, sizeof(current_store_suffix),
        "&market=%s&languages=%s&fieldsTemplate=Details", current_market, current_locale);
}

void x4_lang_apply_system_id(int32_t param_lang)
{
    switch (param_lang) {
    case ORBIS_SYSTEM_PARAM_LANG_SPANISH:
        x4_lang_set(X4_LANG_ES, "es-ES", "ES");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_SPANISH_LA:
        x4_lang_set(X4_LANG_ES, "es-MX", "MX");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_ENGLISH_US:
        x4_lang_set(X4_LANG_EN, "en-US", "US");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_ENGLISH_GB:
        x4_lang_set(X4_LANG_EN, "en-GB", "GB");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_PORTUGUESE_BR:
        x4_lang_set(X4_LANG_EN, "pt-BR", "BR");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_PORTUGUESE_PT:
        x4_lang_set(X4_LANG_EN, "pt-PT", "PT");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_FRENCH:
        x4_lang_set(X4_LANG_EN, "fr-FR", "FR");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_FRENCH_CA:
        x4_lang_set(X4_LANG_EN, "fr-CA", "CA");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_GERMAN:
        x4_lang_set(X4_LANG_EN, "de-DE", "DE");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_ITALIAN:
        x4_lang_set(X4_LANG_EN, "it-IT", "IT");
        break;
    default:
        x4_lang_set(X4_LANG_EN, "en-US", "US");
        break;
    }
}

int x4_lang_init(void)
{
#ifdef X4_OPENORBIS
    int32_t raw_lang = -1;
    int32_t rc = sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG, &raw_lang);
    if (rc < 0) {
        x4_lang_set(X4_LANG_EN, "en-US", "US");
        printf("XCloud4: sceSystemServiceParamGetInt(LANG) fallo rc=0x%08x; usando en-US/US\n", (unsigned)rc);
        return (int)rc;
    }
    x4_lang_apply_system_id(raw_lang);
    printf("XCloud4: idioma consola=%d ui=%s locale=%s market=%s\n",
        (int)raw_lang, current_lang == X4_LANG_ES ? "es" : "en", current_locale, current_market);
    return 0;
#else
    x4_lang_set(X4_LANG_EN, "en-US", "US");
    return 0;
#endif
}

enum X4Lang x4_lang_current(void)
{
    return current_lang;
}

const char *x4_lang_locale(void)
{
    return current_locale;
}

const char *x4_lang_market(void)
{
    return current_market;
}

const char *x4_lang_store_suffix(void)
{
    return current_store_suffix;
}

static const char *const table_es[X4_TR_COUNT] = {
    [X4_TR_DS4_CONNECTED] = "DUALSHOCK 4 CONECTADO",
    [X4_TR_DS4_CONNECT] = "CONECTA TU DUALSHOCK 4",
    [X4_TR_CONTROLLER_ERR_FMT] = "CONTROL: 0X%08X",

    [X4_TR_HOME_TITLE] = "TU PUNTO DE PARTIDA",
    [X4_TR_HOME_SUBTITLE_PUBLIC] = "XBOX CLOUD GAMING EN TU CONSOLA PS4",
    [X4_TR_HOME_SUBTITLE_LAB] = "EL PROYECTO AVANZA EN TU CONSOLA",
    [X4_TR_CARD_ACCOUNT_TITLE] = "CUENTA",
    [X4_TR_CARD_ACCOUNT_DESC] = "INICIA SESION CON\nMICROSOFT Y JUEGA.",
    [X4_TR_CARD_ABOUT_TITLE] = "ACERCA DE",
    [X4_TR_CARD_ABOUT_DESC] = "INFORMACION, CREDITOS\nY CONTROLES.",
    [X4_TR_CARD_SUPPORT_TITLE] = "APOYA EL\nPROYECTO",
    [X4_TR_CARD_SUPPORT_DESC] = "DONACION VOLUNTARIA\nPARA EL DESARROLLO.",
    [X4_TR_X_OPEN] = "X  ABRIR",
    [X4_TR_HOME_HINT] = "ABRE CUENTA PARA CONECTAR Y CONSULTAR EL CATALOGO.",
    [X4_TR_HOME_FOOTER_PUBLIC] = "CRUCETA  ELEGIR     X  ABRIR     OPTIONS  AJUSTES     PARA SALIR: BOTON PS > CERRAR APLICACION",
    [X4_TR_HOME_FOOTER_LAB] = "CRUCETA  ELEGIR     X  ABRIR     OPTIONS  AJUSTES     PARA SALIR: BOTON PS > CERRAR APLICACION",

    [X4_TR_ABOUT_TITLE] = "ACERCA DE",
    [X4_TR_ABOUT_LINE1] = "XCLOUD4 " X4_PRODUCT_VERSION " - CLIENTE NO OFICIAL DE XBOX CLOUD GAMING PARA PS4",
    [X4_TR_ABOUT_LINE2] = "REQUIERE GAME PASS CON JUEGO EN LA NUBE, O UNA CUENTA GRATIS CON PERFIL DE XBOX PARA JUEGOS GRATUITOS COMO FORTNITE.",
    [X4_TR_ABOUT_LINE3] = "Proyecto independiente, sin afiliación con Microsoft, Xbox ni Sony. Úsalo bajo tu propia responsabilidad.",
    [X4_TR_ABOUT_LINE4] = "SOFTWARE LIBRE GPL-3.0 - GITHUB.COM/ASP11EDGR/XCLOUD4",
    [X4_TR_ABOUT_LINE5] = "DESARROLLADO POR ASAP_EDGR",
    [X4_TR_ABOUT_LINE6] = "CREDITOS: OPENORBIS, GOLDHEN, LIBDATACHANNEL, OPUS, MBED TLS. REFERENCIAS: GREENVITA Y PSBOX.",
    [X4_TR_ABOUT_LINE7] = "EN EL JUEGO: MANTÉN DOS DEDOS EN EL TOUCHPAD 1 S PARA ABRIR EL MENÚ",
    [X4_TR_ABOUT_LINE8] = "PARA SALIR: BOTON PS > CERRAR APLICACION",
    [X4_TR_CIRCLE_BACK] = "CIRCULO  VOLVER",

    [X4_TR_SUPPORT_TITLE] = "APOYA EL PROYECTO",
    [X4_TR_SUPPORT_LINE1] = "NUESTRA MISION: QUE MAS PERSONAS PUEDAN JUGAR TITULOS DE NUEVA GENERACION SIN COMPRAR UNA CONSOLA NUEVA.",
    [X4_TR_SUPPORT_LINE2] = "XCLOUD4 ES GRATUITO Y DE CODIGO ABIERTO. SI TE ES UTIL, PUEDES APOYAR SU DESARROLLO CON UNA DONACION VOLUNTARIA.",
    [X4_TR_SUPPORT_LINE3] = "LAS DONACIONES NO DAN ACCESO A JUEGOS NI A SERVICIOS DE XBOX.",
    [X4_TR_SUPPORT_LINE4] = "ESCANEA EL CODIGO CON TU TELEFONO O VISITA: KO-FI.COM/ASAPEDGR",
    [X4_TR_SUPPORT_LINE5] = "GRACIAS POR TU APOYO.",

    [X4_TR_EXIT_ERR_PUBLIC_FMT] = "NO SE PUDO SALIR: 0X%08X. PARA SALIR: BOTON PS > CERRAR APLICACION.",
    [X4_TR_EXIT_ERR_LAB_FMT] = "NO SE PUDO SALIR: 0X%08X. BOTON PS > CERRAR APLICACION.",

    [X4_TR_AUTH_TITLE] = "TU CUENTA MICROSOFT",
    [X4_TR_AUTH_PROMPT_PUBLIC] = "INICIA SESION CON TU CUENTA DE MICROSOFT DESDE TU TELEFONO O PC.",
    [X4_TR_AUTH_NOTE_PUBLIC] = "INICIA SESION CON TU CUENTA DE MICROSOFT DESDE TU TELEFONO O PC.",
    [X4_TR_AUTH_CLOSING_TITLE] = "CERRANDO LA CONEXION...",
    [X4_TR_AUTH_CLOSING_SUB] = "ESPERA UNOS SEGUNDOS PARA VOLVER AL INICIO DE PS4.",
    [X4_TR_AUTH_CODE_PROMPT] = "INTRODUCE ESTE CODIGO EN MICROSOFT:",
    [X4_TR_AUTH_EXPIRES_FMT] = "CADUCA EN %u:%02u",
    [X4_TR_AUTH_CANCEL_HINT] = "CIRCULO CANCELA EL ACCESO Y VUELVE AL MENU.",
    [X4_TR_AUTH_READY] = "LISTO PARA INICIAR SESION",
    [X4_TR_AUTH_CONNECTING] = "CONECTANDO CON MICROSOFT...",
    [X4_TR_AUTH_CONNECTED] = "CONEXION A MICROSOFT CORRECTA",
    [X4_TR_AUTH_AUTHORIZED] = "CUENTA MICROSOFT AUTORIZADA",
    [X4_TR_AUTH_CANCELLED] = "ACCESO CANCELADO",
    [X4_TR_AUTH_EXPIRED] = "EL ACCESO HA CADUCADO",
    [X4_TR_AUTH_DENIED] = "ACCESO RECHAZADO",
    [X4_TR_AUTH_ERROR] = "NO SE PUDO COMPLETAR EL ACCESO",
    [X4_TR_AUTH_AUTHORIZED_HINT] = "PULSA R1 PARA CONSULTAR EL CATALOGO DE XBOX.\nLA CUENTA SE CONSERVA HASTA CERRAR ESTA APP.",
    [X4_TR_AUTH_IDLE_HINT_PUBLIC] = "X SOLICITA UN CODIGO PARA INICIAR SESION.\nTU CONTRASENA SE INTRODUCE EN LA PAGINA DE MICROSOFT.",
    [X4_TR_AUTH_IDLE_HINT_LAB] = "X SOLICITA UN CODIGO. CUADRADO REVISA LA CONEXION.\nTU CONTRASENA SE INTRODUCE EN LA PAGINA DE MICROSOFT.",
    [X4_TR_DETAIL_HTTP_FMT] = "DETALLE 0X%08X    HTTP %d",
    [X4_TR_AUTH_FOOTER_PUBLIC] = "X INICIAR SESION   TRIANGULO CERRAR SESION   OPTIONS AJUSTES   CIRCULO VOLVER   R1 CATALOGO",
    [X4_TR_AUTH_FOOTER_LAB] = "X INICIAR   CUADRADO CONEXION   TRIANGULO BORRAR SESION   OPTIONS AJUSTES   CIRCULO VOLVER",
    [X4_TR_AUTH_R1_HINT] = "R1 CATALOGO DE XBOX. ELIGE UN TITULO CON ACCESO PARA JUGAR.",

    [X4_TR_SESS_TITLE] = "SESION DE JUEGO",
    [X4_TR_SESS_SELECTED_TITLE] = "TITULO SELECCIONADO",
    [X4_TR_SESS_PREPARING] = "PREPARANDO LA SOLICITUD...",
    [X4_TR_SESS_WAITING] = "XBOX ESTA PREPARANDO EL JUEGO...",
    [X4_TR_SESS_READY] = "XBOX PREPARO LA SESION",
    [X4_TR_SESS_AUTHORIZING] = "AUTORIZANDO LA CONEXION...",
    [X4_TR_SESS_AUTHORIZED] = "XBOX ACEPTO LA CONEXION",
    [X4_TR_SESS_NEGOTIATING] = "NEGOCIANDO IMAGEN Y SONIDO...",
    [X4_TR_SESS_CONNECTING] = "CONECTANDO EL JUEGO POR WEBRTC...",
    [X4_TR_SESS_STREAMING] = "RECIBIENDO PAQUETES DEL JUEGO",
    [X4_TR_SESS_STOPPING] = "CERRANDO LA SESION EN XBOX...",
    [X4_TR_SESS_CLOSED] = "SESION CERRADA",
    [X4_TR_SESS_CANCELLED] = "SOLICITUD CANCELADA",
    [X4_TR_SESS_ERROR] = "NO SE PUDO COMPLETAR LA SESION",
    [X4_TR_SESS_CLOSING_APP] = "CERRANDO LA SESION Y LA APLICACION...",
    [X4_TR_SESS_PREP_AV] = "PREPARANDO LA CONEXION DE IMAGEN Y SONIDO.",
    [X4_TR_SESS_PREP_AUTH] = "PREPARANDO LA AUTORIZACION DE ESTA SESION.",
    [X4_TR_SESS_PERM_ACCEPTED] = "XBOX ACEPTO EL PERMISO PARA CONECTAR ESTA SESION.",
    [X4_TR_SESS_CONFIRMED_READY] = "XBOX CONFIRMO QUE LA SESION ESTABA LISTA.",
    [X4_TR_SESS_ELAPSED_FMT] = "TIEMPO TRANSCURRIDO: %u SEGUNDOS.",
    [X4_TR_SESS_WEBRTC_FMT] = "SDP HTTP %d   ICE HTTP %d   WEBRTC %s",
    [X4_TR_SESS_RTC_CONNECTED] = "CONECTADO",
    [X4_TR_SESS_RTC_WAITING] = "EN ESPERA",
    [X4_TR_SESS_CLEANUP_FAIL_FMT] = "CIERRE SIN CONFIRMAR: 0X%08X   HTTP %d",
    [X4_TR_SESS_DETAIL_FMT] = "DETALLE 0X%08X   HTTP %d",
    [X4_TR_SESS_FOOTER_PUBLIC] = "CIRCULO  CERRAR Y VOLVER AL CATALOGO     PARA SALIR: BOTON PS > CERRAR APLICACION",
    [X4_TR_SESS_FOOTER_LAB] = "CIRCULO  CERRAR Y VOLVER AL CATALOGO     PARA SALIR: BOTON PS > CERRAR APLICACION",
    [X4_TR_GAME_TOUCH_HINT] = "EN EL JUEGO: MANTÉN DOS DEDOS EN EL TOUCHPAD 1 S PARA ABRIR EL MENÚ",

    [X4_TR_LIVE_PACKETS_FMT] = "H264: %llu PAQUETES, %llu IMAGENES   OPUS: %llu PAQUETES, %llu BLOQUES",
    [X4_TR_LIVE_VIDEO_ERR_PUBLIC_FMT] = "Código %d · audio %s · PARA SALIR: BOTON PS > CERRAR APLICACION",
    [X4_TR_LIVE_VIDEO_ERR_LAB_FMT] = "Código %d · audio %s · PARA SALIR: BOTON PS > CERRAR APLICACION",
    [X4_TR_LIVE_AUDIO_UNAVAIL] = "no disponible",
    [X4_TR_LIVE_AUDIO_ACTIVE] = "activo",
    [X4_TR_LIVE_AUDIO_WAITING] = "en espera",
    [X4_TR_LIVE_MEDIA_ERR_FMT] = "MEDIOS: 0X%08X  VIDEO: 0X%08X  AUDIO: 0X%08X",
    [X4_TR_TOUCH_MENU_TITLE] = "MENÚ DEL JUEGO",
    [X4_TR_TOUCH_MENU_BACK] = "Volver al catálogo",
    [X4_TR_TOUCH_MENU_MUTE] = "Silenciar sonido",
    [X4_TR_TOUCH_MENU_UNMUTE] = "Activar sonido",
    [X4_TR_TOUCH_MENU_XBOX] = "Botón Xbox",
    [X4_TR_TOUCH_MENU_CANCEL] = "Cancelar",
    [X4_TR_TOUCH_MENU_MARK_TEST] = "Marcar prueba",
    [X4_TR_TOUCH_MENU_EXIT_APP] = "Salir de la app",
    [X4_TR_TOUCH_MENU_FOOTER] = "CRUCETA Elegir   X Aceptar   CÍRCULO Cerrar",
    [X4_TR_OVERLAY_AUDIO_UNAVAIL] = "SONIDO NO DISPONIBLE",
    [X4_TR_OVERLAY_AUDIO_MUTED] = "SONIDO SILENCIADO",
    [X4_TR_OVERLAY_INPUT_UNAVAIL] = "CONTROL NO DISPONIBLE",
    [X4_TR_VIDEO_REJECTED_DIM_PUBLIC_FMT] = "Xbox envió %u×%u; esta PS4 solo admite hasta 1080p. Cierra con botón PS.",
    [X4_TR_VIDEO_REJECTED_DIM_LAB_FMT] = "Xbox envió %u×%u; esta PS4 solo admite hasta 1080p. Sal con OPTIONS.",
    [X4_TR_VIDEO_STOPPED_PUBLIC_FMT] = "Vídeo detenido (código %d). Cierra con botón PS.",
    [X4_TR_VIDEO_STOPPED_LAB_FMT] = "Vídeo detenido (código %d). Sal con OPTIONS.",
    [X4_TR_VIDEO_NO_FRAMES] = "Xbox no envió imagen. Puede ser un problema temporal de Xbox con este juego: vuelve a intentarlo o prueba otro servidor en OPTIONS.",
    [X4_TR_CANCEL_HINT] = "CÍRCULO Cancelar",

    [X4_TR_CAT_DS4_CONNECTED] = "DualShock 4 conectado",
    [X4_TR_CAT_DS4_CONNECT] = "Conecta tu DualShock 4",
    [X4_TR_CAT_TITLE] = "Catálogo",
    [X4_TR_CAT_SEARCH_BTN] = "TRIÁNGULO Buscar juegos",
    [X4_TR_CAT_RECENT_TITLE] = "Jugados recientemente",
    [X4_TR_CAT_RECENT_EMPTY] = "Tus juegos recientes aparecerán aquí",
    [X4_TR_CAT_RECENT_LOADING] = "Cargando tus recientes…",
    [X4_TR_CAT_RECENT_ERROR] = "No se pudieron cargar tus recientes",
    [X4_TR_CAT_RECENT_RETRY] = "CUADRADO Reintentar recientes",
    [X4_TR_CAT_NEXT_GAME] = "Tu próximo juego",
    [X4_TR_CAT_FILTER_ALL] = "Todos",
    [X4_TR_CAT_FILTER_ENTITLED] = "Con acceso",
    [X4_TR_CAT_FILTER_UNCONFIRMED] = "Por confirmar",
    [X4_TR_CAT_LOADING] = "Cargando el catálogo…",
    [X4_TR_CAT_NO_MATCH] = "No hay juegos en esta búsqueda",
    [X4_TR_CAT_OPEN_ACCOUNT] = "Abre tu cuenta para cargar el catálogo",
    [X4_TR_CAT_ERROR] = "No se pudo cargar el catálogo",
    [X4_TR_CAT_PERMISSION_UNKNOWN] = "Permiso todavía desconocido",
    [X4_TR_CAT_NOT_AVAILABLE] = "No disponible ahora",
    [X4_TR_CAT_REFRESH_PROMPT] = "Actualiza el catálogo",
    [X4_TR_CAT_PREPARING] = "Preparando…",
    [X4_TR_CAT_QUERYING] = "Consultando xCloud…",
    [X4_TR_CAT_PLAY_BTN] = "X  Jugar",
    [X4_TR_CAT_CHOOSE_GAME] = "Elige un juego",
    [X4_TR_CAT_FOOTER_PUBLIC] = "Cruceta Elegir   X Jugar   OPTIONS Ajustes   L1/R1 Página   L2/R2 Grupos   CUADRADO Actualizar   CÍRCULO Cuenta",
    [X4_TR_CAT_FOOTER_LAB] = "Cruceta Elegir   X Jugar   OPTIONS Ajustes   L1/R1 Página   L2/R2 Grupos   CUADRADO Actualizar   CÍRCULO Cuenta",
    [X4_TR_CAT_FINISHING_COVERS] = "Terminando la carga de portadas…",
    [X4_TR_CAT_RESULTS_FMT] = "%u resultados · Página %u/%u · %s",
    [X4_TR_KB_TITLE] = "Buscar juegos",
    [X4_TR_KB_PLACEHOLDER] = "Escribe un título o ID",
    [X4_TR_KB_HELP1] = "X Añadir   CUADRADO Borrar   TRIÁNGULO Limpiar",
    [X4_TR_KB_HELP2_PUBLIC] = "R1 Espacio   R2 Aplicar   CÍRCULO Volver",
    [X4_TR_KB_HELP2_LAB] = "R1 Espacio   R2 Aplicar   CÍRCULO Volver",
    [X4_TR_ASSET_DOWNLOADING] = "Descargando…",
    [X4_TR_ASSET_READY] = "Lista",
    [X4_TR_ASSET_RETRY_PENDING] = "Reintento pendiente",
    [X4_TR_ASSET_MISSING] = "Sin imagen",
    [X4_TR_ASSET_PENDING] = "Pendiente",
    [X4_TR_ASSET_SMALL_LOADING] = "Cargando",
    [X4_TR_ASSET_SMALL_RETRY] = "Reintento",
    [X4_TR_NOTICE_COVER_CLEANUP] = "No se pudo cerrar la conexión de portadas. Sal y abre XCloud4 otra vez.",
    [X4_TR_NOTICE_QUERY_ERR_FMT] = "No se pudo iniciar la consulta (%d)",
    [X4_TR_HOME_UPDATE_AVAILABLE_FMT] = "NUEVA VERSION DISPONIBLE: %s · GITHUB.COM/ASP11EDGR/XCLOUD4",
    [X4_TR_UPDATE_CHECKING] = "COMPROBANDO ACTUALIZACIONES...",
    [X4_TR_UPDATE_LATEST] = "ESTAS USANDO LA ULTIMA VERSION",
    [X4_TR_UPDATE_AVAILABLE_FMT] = "HAY UNA VERSION NUEVA: %s",
    [X4_TR_UPDATE_FAILED] = "NO SE PUDO COMPROBAR SI HAY ACTUALIZACIONES",
    [X4_TR_AUTH_SCAN_PHONE] = "ESCANEA CON TU TELEFONO",

    [X4_TR_XSTS_NO_PROFILE] = "Tu cuenta aún no tiene perfil de Xbox. Entra una vez en xbox.com con esta cuenta para crearlo y vuelve a iniciar sesión.",
    [X4_TR_XSTS_UNAVAILABLE_COUNTRY] = "Xbox Live no está disponible en el país de tu cuenta.",
    [X4_TR_XSTS_AGE_VERIFICATION] = "Tu cuenta necesita verificación de edad en xbox.com.",
    [X4_TR_XSTS_CHILD_ACCOUNT] = "Esta cuenta es de un menor: un adulto de la familia Microsoft debe darle permiso.",
    [X4_TR_DETAIL_XBOX_HTTP_FMT] = "DETALLE XBOX %u    HTTP %d",

    [X4_TR_NET_HINT_DNS] = "La consola no puede resolver direcciones de internet (DNS). Revisa Ajustes > Red y usa un DNS manual que funcione, por ejemplo 1.1.1.1 / 8.8.8.8.",
    [X4_TR_NET_HINT_TIMEOUT] = "La conexión tardó demasiado. Revisa tu internet y vuelve a intentarlo.",
    [X4_TR_NET_HINT_SSL_CERT] = "La consola no reconoce el certificado del servidor. Actualiza XCloud4 a la última versión.",

    [X4_TR_PIC_SETTINGS_TITLE] = "Ajustes de imagen",
    [X4_TR_PIC_SETTINGS_SUBTITLE] = "Se guardan en la consola y se aplican al iniciar el siguiente juego.",
    [X4_TR_PIC_SETTINGS_SECTION] = "Calidad de imagen",
    [X4_TR_PIC_SETTINGS_OPT_1080P_ALTA] = "1080p Alta (recomendada)",
    [X4_TR_PIC_SETTINGS_OPT_1080P] = "1080p",
    [X4_TR_PIC_SETTINGS_OPT_720P] = "720p",
    [X4_TR_PIC_SETTINGS_TAG_1080P_ALTA] = "~10-13 Mbps",
    [X4_TR_PIC_SETTINGS_TAG_1080P] = "~5-8 Mbps",
    [X4_TR_PIC_SETTINGS_TAG_720P] = "~7 Mbps",
    [X4_TR_PIC_SETTINGS_DESC_1080P_ALTA] = "1920x1080 a 60 fps, unos 10-13 Mbps. La mejor nitidez. Necesita una conexión estable de 15 Mbps o más (mejor por cable). Es la opción recomendada.",
    [X4_TR_PIC_SETTINGS_DESC_1080P] = "1920x1080 a 60 fps, hasta 8 Mbps. Usa menos internet a cambio de algo más de compresión. Buena si notas cortes con la opción Alta.",
    [X4_TR_PIC_SETTINGS_DESC_720P] = "1280x720 a 60 fps, unos 7 Mbps. Menos retraso, porque la consola decodifica cada imagen más rápido, pero la imagen se ve menos nítida.",
    [X4_TR_PIC_SETTINGS_FOOTER] = "ARRIBA/ABAJO Elegir     X Guardar     CÍRCULO Cancelar",
    [X4_TR_PIC_SETTINGS_BANNER_1080P_ALTA] = "1080p · Alta",
    [X4_TR_PIC_SETTINGS_BANNER_1080P] = "1080p",
    [X4_TR_PIC_SETTINGS_BANNER_720P] = "720p",

    [X4_TR_OPTIONS_TAB_PICTURE] = "Imagen",
    [X4_TR_OPTIONS_TAB_SERVER] = "Servidor",
    [X4_TR_OPTIONS_FOOTER] = "L1/R1 Pestaña     ARRIBA/ABAJO Elegir     X Guardar     CÍRCULO Cancelar",
    [X4_TR_SERVER_SETTINGS_SUBTITLE] = "Los ms se miden en tu última partida en cada servidor. Se aplica al cargar el catálogo y al iniciar el siguiente juego.",
    [X4_TR_SERVER_OPT_AUTO] = "Automático (recomendado)",
    [X4_TR_SERVER_TAG_AUTO_FMT] = "Xbox elige: %s",
    [X4_TR_SERVER_LAST_GAME] = "última partida",
    [X4_TR_SERVER_NOT_MEASURED] = "sin medir",
    [X4_TR_SERVER_MEASURING] = "midiendo…",
    [X4_TR_SERVER_NO_RESPONSE] = "sin respuesta",
    [X4_TR_SERVER_NO_LIST] = "Inicia sesión para ver los servidores que Xbox ofrece a tu cuenta.",
    [X4_TR_SERVER_NOT_AVAILABLE] = "Tu servidor elegido ya no está disponible; se usa Automático.",
    [X4_TR_SERVER_FIRST_TITLE] = "Elige tu servidor",
    [X4_TR_SERVER_FIRST_SUBTITLE] = "Elige el que tenga menos ms. Puedes cambiarlo cuando quieras en OPTIONS.",
    [X4_TR_ACCOUNT_HINT_FIRST_SERVER] = "AL INICIAR SESION PODRAS ELEGIR TU SERVIDOR.",
    [X4_TR_CAT_CHOOSE_SERVER_WAIT] = "Elige tu servidor para continuar",

    [X4_TR_CARD_FORTNITE_TITLE] = "FORTNITE",
    [X4_TR_CARD_FORTNITE_DESC] = "¿SIN GAME PASS?\nJUEGA FORTNITE GRATIS\nCON TU CUENTA\nDE MICROSOFT.",
    [X4_TR_X_PLAY] = "X  JUGAR",
    [X4_TR_FORTNITE_TITLE] = "FORTNITE",
    [X4_TR_FORTNITE_PIC_QUALITY] = "CALIDAD DE IMAGEN",
    [X4_TR_FORTNITE_HINT] = "Si notas cortes o tu internet es lento, elige 1080p o 720p.",
    [X4_TR_FORTNITE_FOOTER] = "ARRIBA/ABAJO CALIDAD     X JUGAR     OPTIONS SERVIDOR     CIRCULO VOLVER",
    [X4_TR_FORTNITE_UNAVAILABLE] = "Fortnite no está disponible para tu cuenta en este servidor. Prueba otro servidor en OPTIONS.",
    [X4_TR_FORTNITE_SEARCHING] = "Buscando Fortnite en el servidor…",
    [X4_TR_FORTNITE_ERROR_FOOTER] = "OPTIONS SERVIDOR     CIRCULO VOLVER",
    [X4_TR_CLOUD_UNAVAILABLE_FMT] = "Xbox Cloud Gaming no está disponible para tu ubicación o tu cuenta (HTTP %d). XCloud4 solo puede usar los servidores que Xbox te ofrece.",

    [X4_TR_APPLE_YOUR_ACCOUNT] = "Tu cuenta",
    [X4_TR_APPLE_HOME_TITLE] = "Tu próxima partida.",
    [X4_TR_APPLE_HOME_SUBTITLE] = "Elige cómo quieres jugar.",
    [X4_TR_APPLE_CARD_EXPLORE_TITLE] = "Explorar catálogo",
    [X4_TR_APPLE_CARD_EXPLORE_SUB] = "Encuentra tu próxima aventura",
    [X4_TR_APPLE_BTN_ENTER] = "Entrar",
    [X4_TR_APPLE_CARD_FORTNITE_TITLE] = "Fortnite",
    [X4_TR_APPLE_CARD_FORTNITE_SUB] = "Acceso directo",
    [X4_TR_APPLE_CARD_ACCOUNT_TITLE] = "Cuenta",
    [X4_TR_APPLE_CARD_ACCOUNT_SUB_OUT] = "Inicia sesión",
    [X4_TR_APPLE_CARD_ACCOUNT_SUB_IN] = "Sesión activa",
    [X4_TR_APPLE_PILL_SETTINGS] = "Ajustes",
    [X4_TR_APPLE_PILL_ABOUT] = "Acerca de",
    [X4_TR_APPLE_PILL_SUPPORT] = "Apoyar",
    [X4_TR_APPLE_HINT_SELECT] = "Seleccionar",
    [X4_TR_APPLE_HINT_SETTINGS] = "Ajustes",
    [X4_TR_APPLE_HINT_SEARCH] = "Buscar",
    [X4_TR_APPLE_HINT_BACK] = "Volver",
    [X4_TR_APPLE_HINT_SERVER_FIRST] = "Al iniciar sesión podrás elegir tu servidor",
    [X4_TR_APPLE_TAB_DISCOVER] = "Descubrir",
    [X4_TR_APPLE_TAB_CATALOG] = "Catálogo",
    [X4_TR_APPLE_TAB_RECENTS] = "Recientes",
    [X4_TR_APPLE_TAB_FORTNITE] = "Fortnite",
    [X4_TR_APPLE_HERO_FEATURED] = "DESTACADO",
    [X4_TR_APPLE_HERO_PLAY_NOW] = "Jugar ahora",
    [X4_TR_APPLE_HERO_DEFAULT_SUB] = "Juega al instante en la nube desde tu consola PS4.",
    [X4_TR_APPLE_ROW_CONTINUE] = "Continúa donde lo dejaste",
    [X4_TR_APPLE_ROW_POPULAR] = "Populares",
    [X4_TR_APPLE_PS_EXIT_HINT] = "Botón PS: Salir",
    [X4_TR_APPLE_CARD_FORTNITE_SUB1] = "¿Sin Game Pass? Juega gratis",
    [X4_TR_APPLE_CARD_FORTNITE_SUB2] = "con tu cuenta de Microsoft.",
    [X4_TR_APPLE_BADGE_FREE] = "Gratis",
    [X4_TR_FORTNITE_REQ_LINE] = "Requiere una cuenta de Microsoft con perfil de Xbox. No necesitas Game Pass.",
    [X4_TR_APPLE_HINT_RIGHT_STICK_READ_MORE] = "Stick derecho: leer más",
    [X4_TR_ACCESS_TITLE] = "Posible restricción regional",
    [X4_TR_ACCESS_COUNTRY] = "País de acceso",
    [X4_TR_ACCESS_AUTO] = "Automático",
    [X4_TR_ACCESS_US] = "Estados Unidos",
    [X4_TR_ACCESS_BR] = "Brasil",
    [X4_TR_ACCESS_JP] = "Japón",
    [X4_TR_ACCESS_KR] = "Corea",
    [X4_TR_ACCESS_PL] = "Polonia",
    [X4_TR_ACCESS_DESCRIPTION] = "Si el servicio no está disponible en tu país, prueba otro país de acceso. No cambia el país de tu cuenta ni el servidor de juego.",
    [X4_TR_ACCESS_WARNING] = "Usar un país de acceso distinto puede ir contra los términos de Xbox y Microsoft podría restringir tu cuenta. Úsalo bajo tu propia responsabilidad.",
    [X4_TR_ACCESS_TRY] = "Probar acceso",
    [X4_TR_ACCESS_HELP] = "Más ayuda",
    [X4_TR_ACCESS_CONTINUE] = "Entiendo, continuar",
    [X4_TR_ACCESS_CONFIRM_TITLE] = "Antes de probar otro país",
    [X4_TR_ACCESS_TRYING_FMT] = "Probando acceso desde %s…",
    [X4_TR_ACCESS_SUCCESS_FMT] = "Acceso configurado: %s",
    [X4_TR_ACCESS_SAVE_ERROR] = "No se pudo guardar. No se cambió el país de acceso. Prueba de nuevo.",
    [X4_TR_ACCESS_TAB] = "Acceso",
    [X4_TR_ACCESS_NEXT_QUERY] = "Se aplicará en la siguiente consulta o partida.",
    [X4_TR_ACCESS_BADGE_FMT] = " · Acceso: %s",
    [X4_TR_ACCESS_CANCEL] = "Cancelar",
    [X4_TR_ACCESS_BACK] = "Volver",
    [X4_TR_ACCESS_SAVE] = "Guardar",
    [X4_TR_UPDATE_NOTICE_TITLE] = "Nueva versión disponible",
    [X4_TR_UPDATE_NOTICE_VERSION_FMT] = "XCloud4 %s ya está disponible. Tienes la %s.",
    [X4_TR_UPDATE_NOTICE_DESCRIPTION] = "Escanéalo para descargarla desde GitHub. Instálala con GoldHEN > Package Installer.",
    [X4_TR_UPDATE_NOTICE_CLOSE] = "Cerrar",
    [X4_TR_UPDATE_NOTICE_TEST] = "Probar aviso de actualización",
    [X4_TR_UPDATE_NOTICE_TEST_QUEUED] = "La prueba del aviso se mostrará al volver a Inicio.",
};

static const char *const table_en[X4_TR_COUNT] = {
    [X4_TR_DS4_CONNECTED] = "DUALSHOCK 4 CONNECTED",
    [X4_TR_DS4_CONNECT] = "CONNECT YOUR DUALSHOCK 4",
    [X4_TR_CONTROLLER_ERR_FMT] = "CONTROLLER: 0X%08X",

    [X4_TR_HOME_TITLE] = "YOUR STARTING POINT",
    [X4_TR_HOME_SUBTITLE_PUBLIC] = "XBOX CLOUD GAMING ON YOUR PS4 CONSOLE",
    [X4_TR_HOME_SUBTITLE_LAB] = "THE PROJECT ADVANCES ON YOUR CONSOLE",
    [X4_TR_CARD_ACCOUNT_TITLE] = "ACCOUNT",
    [X4_TR_CARD_ACCOUNT_DESC] = "SIGN IN WITH\nMICROSOFT AND PLAY.",
    [X4_TR_CARD_ABOUT_TITLE] = "ABOUT",
    [X4_TR_CARD_ABOUT_DESC] = "INFORMATION, CREDITS\nAND CONTROLS.",
    [X4_TR_CARD_SUPPORT_TITLE] = "SUPPORT THE\nPROJECT",
    [X4_TR_CARD_SUPPORT_DESC] = "VOLUNTARY DONATION\nFOR DEVELOPMENT.",
    [X4_TR_X_OPEN] = "X  OPEN",
    [X4_TR_HOME_HINT] = "OPEN ACCOUNT TO SIGN IN AND BROWSE THE CATALOG.",
    [X4_TR_HOME_FOOTER_PUBLIC] = "D-PAD  SELECT     X  OPEN     OPTIONS  SETTINGS     TO EXIT: PS BUTTON > CLOSE APPLICATION",
    [X4_TR_HOME_FOOTER_LAB] = "D-PAD  SELECT     X  OPEN     OPTIONS  SETTINGS     TO EXIT: PS BUTTON > CLOSE APPLICATION",

    [X4_TR_ABOUT_TITLE] = "ABOUT",
    [X4_TR_ABOUT_LINE1] = "XCLOUD4 " X4_PRODUCT_VERSION " - UNOFFICIAL XBOX CLOUD GAMING CLIENT FOR PS4",
    [X4_TR_ABOUT_LINE2] = "REQUIRES GAME PASS WITH CLOUD GAMING, OR A FREE ACCOUNT WITH AN XBOX PROFILE FOR FREE GAMES LIKE FORTNITE.",
    [X4_TR_ABOUT_LINE3] = "Independent project, not affiliated with Microsoft, Xbox or Sony. Use at your own risk.",
    [X4_TR_ABOUT_LINE4] = "FREE SOFTWARE GPL-3.0 - GITHUB.COM/ASP11EDGR/XCLOUD4",
    [X4_TR_ABOUT_LINE5] = "DEVELOPED BY ASAP_EDGR",
    [X4_TR_ABOUT_LINE6] = "CREDITS: OPENORBIS, GOLDHEN, LIBDATACHANNEL, OPUS, MBED TLS. REFERENCES: GREENVITA AND PSBOX.",
    [X4_TR_ABOUT_LINE7] = "IN GAME: HOLD TWO FINGERS ON THE TOUCHPAD FOR 1 S TO OPEN THE MENU",
    [X4_TR_ABOUT_LINE8] = "TO EXIT: PS BUTTON > CLOSE APPLICATION",
    [X4_TR_CIRCLE_BACK] = "CIRCLE  BACK",

    [X4_TR_SUPPORT_TITLE] = "SUPPORT THE PROJECT",
    [X4_TR_SUPPORT_LINE1] = "OUR MISSION: HELPING MORE PEOPLE PLAY NEXT-GEN GAMES WITHOUT BUYING A NEW CONSOLE.",
    [X4_TR_SUPPORT_LINE2] = "XCLOUD4 IS FREE AND OPEN SOURCE. IF YOU FIND IT USEFUL, YOU CAN SUPPORT ITS DEVELOPMENT WITH A VOLUNTARY DONATION.",
    [X4_TR_SUPPORT_LINE3] = "DONATIONS DO NOT GRANT ACCESS TO GAMES OR XBOX SERVICES.",
    [X4_TR_SUPPORT_LINE4] = "SCAN THE CODE WITH YOUR PHONE OR VISIT: KO-FI.COM/ASAPEDGR",
    [X4_TR_SUPPORT_LINE5] = "THANK YOU FOR YOUR SUPPORT.",

    [X4_TR_EXIT_ERR_PUBLIC_FMT] = "COULD NOT EXIT: 0X%08X. TO EXIT: PS BUTTON > CLOSE APPLICATION.",
    [X4_TR_EXIT_ERR_LAB_FMT] = "COULD NOT EXIT: 0X%08X. PS BUTTON > CLOSE APPLICATION.",

    [X4_TR_AUTH_TITLE] = "YOUR MICROSOFT ACCOUNT",
    [X4_TR_AUTH_PROMPT_PUBLIC] = "SIGN IN WITH YOUR MICROSOFT ACCOUNT FROM YOUR PHONE OR PC.",
    [X4_TR_AUTH_NOTE_PUBLIC] = "SIGN IN WITH YOUR MICROSOFT ACCOUNT FROM YOUR PHONE OR PC.",
    [X4_TR_AUTH_CLOSING_TITLE] = "CLOSING CONNECTION...",
    [X4_TR_AUTH_CLOSING_SUB] = "WAIT A FEW SECONDS TO RETURN TO THE PS4 HOME SCREEN.",
    [X4_TR_AUTH_CODE_PROMPT] = "ENTER THIS CODE AT MICROSOFT:",
    [X4_TR_AUTH_EXPIRES_FMT] = "EXPIRES IN %u:%02u",
    [X4_TR_AUTH_CANCEL_HINT] = "CIRCLE CANCELS SIGN-IN AND RETURNS TO THE MENU.",
    [X4_TR_AUTH_READY] = "READY TO SIGN IN",
    [X4_TR_AUTH_CONNECTING] = "CONNECTING TO MICROSOFT...",
    [X4_TR_AUTH_CONNECTED] = "MICROSOFT CONNECTION VERIFIED",
    [X4_TR_AUTH_AUTHORIZED] = "MICROSOFT ACCOUNT AUTHORIZED",
    [X4_TR_AUTH_CANCELLED] = "SIGN-IN CANCELLED",
    [X4_TR_AUTH_EXPIRED] = "SIGN-IN CODE EXPIRED",
    [X4_TR_AUTH_DENIED] = "SIGN-IN DENIED",
    [X4_TR_AUTH_ERROR] = "COULD NOT COMPLETE SIGN-IN",
    [X4_TR_AUTH_AUTHORIZED_HINT] = "PRESS R1 TO BROWSE THE XBOX CATALOG.\nYOUR ACCOUNT STAYS SIGNED IN UNTIL YOU CLOSE THIS APP.",
    [X4_TR_AUTH_IDLE_HINT_PUBLIC] = "PRESS X TO REQUEST A SIGN-IN CODE.\nYOUR PASSWORD IS ENTERED ON THE MICROSOFT PAGE.",
    [X4_TR_AUTH_IDLE_HINT_LAB] = "PRESS X FOR A CODE. SQUARE CHECKS CONNECTION.\nYOUR PASSWORD IS ENTERED ON THE MICROSOFT PAGE.",
    [X4_TR_DETAIL_HTTP_FMT] = "DETAIL 0X%08X    HTTP %d",
    [X4_TR_AUTH_FOOTER_PUBLIC] = "X SIGN IN   TRIANGLE SIGN OUT   OPTIONS SETTINGS   CIRCLE BACK   R1 CATALOG",
    [X4_TR_AUTH_FOOTER_LAB] = "X SIGN IN   SQUARE CONNECTION   TRIANGLE SIGN OUT   OPTIONS SETTINGS   CIRCLE BACK",
    [X4_TR_AUTH_R1_HINT] = "R1 XBOX CATALOG. SELECT AN AVAILABLE TITLE TO PLAY.",

    [X4_TR_SESS_TITLE] = "GAME SESSION",
    [X4_TR_SESS_SELECTED_TITLE] = "SELECTED TITLE",
    [X4_TR_SESS_PREPARING] = "PREPARING REQUEST...",
    [X4_TR_SESS_WAITING] = "XBOX IS PREPARING THE GAME...",
    [X4_TR_SESS_READY] = "XBOX PREPARED THE SESSION",
    [X4_TR_SESS_AUTHORIZING] = "AUTHORIZING CONNECTION...",
    [X4_TR_SESS_AUTHORIZED] = "XBOX ACCEPTED THE CONNECTION",
    [X4_TR_SESS_NEGOTIATING] = "NEGOTIATING AUDIO AND VIDEO...",
    [X4_TR_SESS_CONNECTING] = "CONNECTING GAME VIA WEBRTC...",
    [X4_TR_SESS_STREAMING] = "RECEIVING GAME STREAM",
    [X4_TR_SESS_STOPPING] = "CLOSING SESSION ON XBOX...",
    [X4_TR_SESS_CLOSED] = "SESSION CLOSED",
    [X4_TR_SESS_CANCELLED] = "REQUEST CANCELLED",
    [X4_TR_SESS_ERROR] = "COULD NOT COMPLETE THE SESSION",
    [X4_TR_SESS_CLOSING_APP] = "CLOSING SESSION AND APPLICATION...",
    [X4_TR_SESS_PREP_AV] = "PREPARING AUDIO AND VIDEO CONNECTION.",
    [X4_TR_SESS_PREP_AUTH] = "PREPARING AUTHORIZATION FOR THIS SESSION.",
    [X4_TR_SESS_PERM_ACCEPTED] = "XBOX ACCEPTED PERMISSION TO CONNECT THIS SESSION.",
    [X4_TR_SESS_CONFIRMED_READY] = "XBOX CONFIRMED THAT THE SESSION WAS READY.",
    [X4_TR_SESS_ELAPSED_FMT] = "ELAPSED TIME: %u SECONDS.",
    [X4_TR_SESS_WEBRTC_FMT] = "SDP HTTP %d   ICE HTTP %d   WEBRTC %s",
    [X4_TR_SESS_RTC_CONNECTED] = "CONNECTED",
    [X4_TR_SESS_RTC_WAITING] = "WAITING",
    [X4_TR_SESS_CLEANUP_FAIL_FMT] = "UNCONFIRMED CLOSE: 0X%08X   HTTP %d",
    [X4_TR_SESS_DETAIL_FMT] = "DETAIL 0X%08X   HTTP %d",
    [X4_TR_SESS_FOOTER_PUBLIC] = "CIRCLE  CLOSE AND RETURN TO CATALOG     TO EXIT: PS BUTTON > CLOSE APPLICATION",
    [X4_TR_SESS_FOOTER_LAB] = "CIRCLE  CLOSE AND RETURN TO CATALOG     TO EXIT: PS BUTTON > CLOSE APPLICATION",
    [X4_TR_GAME_TOUCH_HINT] = "IN GAME: HOLD TWO FINGERS ON THE TOUCHPAD FOR 1 S TO OPEN THE MENU",

    [X4_TR_LIVE_PACKETS_FMT] = "H264: %llu PACKETS, %llu FRAMES   OPUS: %llu PACKETS, %llu BLOCKS",
    [X4_TR_LIVE_VIDEO_ERR_PUBLIC_FMT] = "Code %d · audio %s · TO EXIT: PS BUTTON > CLOSE APPLICATION",
    [X4_TR_LIVE_VIDEO_ERR_LAB_FMT] = "Code %d · audio %s · TO EXIT: PS BUTTON > CLOSE APPLICATION",
    [X4_TR_LIVE_AUDIO_UNAVAIL] = "unavailable",
    [X4_TR_LIVE_AUDIO_ACTIVE] = "active",
    [X4_TR_LIVE_AUDIO_WAITING] = "waiting",
    [X4_TR_LIVE_MEDIA_ERR_FMT] = "MEDIA: 0X%08X  VIDEO: 0X%08X  AUDIO: 0X%08X",
    [X4_TR_TOUCH_MENU_TITLE] = "GAME MENU",
    [X4_TR_TOUCH_MENU_BACK] = "Return to catalog",
    [X4_TR_TOUCH_MENU_MUTE] = "Mute audio",
    [X4_TR_TOUCH_MENU_UNMUTE] = "Unmute audio",
    [X4_TR_TOUCH_MENU_XBOX] = "Xbox button",
    [X4_TR_TOUCH_MENU_CANCEL] = "Cancel",
    [X4_TR_TOUCH_MENU_MARK_TEST] = "Mark test",
    [X4_TR_TOUCH_MENU_EXIT_APP] = "Exit app",
    [X4_TR_TOUCH_MENU_FOOTER] = "D-PAD Choose   X Confirm   CIRCLE Close",
    [X4_TR_OVERLAY_AUDIO_UNAVAIL] = "AUDIO UNAVAILABLE",
    [X4_TR_OVERLAY_AUDIO_MUTED] = "AUDIO MUTED",
    [X4_TR_OVERLAY_INPUT_UNAVAIL] = "CONTROLLER UNAVAILABLE",
    [X4_TR_VIDEO_REJECTED_DIM_PUBLIC_FMT] = "Xbox sent %u×%u; this PS4 supports up to 1080p. Close via PS button.",
    [X4_TR_VIDEO_REJECTED_DIM_LAB_FMT] = "Xbox sent %u×%u; this PS4 supports up to 1080p. Exit with OPTIONS.",
    [X4_TR_VIDEO_STOPPED_PUBLIC_FMT] = "Video stopped (code %d). Close via PS button.",
    [X4_TR_VIDEO_STOPPED_LAB_FMT] = "Video stopped (code %d). Exit with OPTIONS.",
    [X4_TR_VIDEO_NO_FRAMES] = "Xbox didn't send any video. It may be a temporary Xbox problem with this game: try again or pick another server in OPTIONS.",
    [X4_TR_CANCEL_HINT] = "CIRCLE Cancel",

    [X4_TR_CAT_DS4_CONNECTED] = "DualShock 4 connected",
    [X4_TR_CAT_DS4_CONNECT] = "Connect your DualShock 4",
    [X4_TR_CAT_TITLE] = "Catalog",
    [X4_TR_CAT_SEARCH_BTN] = "TRIANGLE Search games",
    [X4_TR_CAT_RECENT_TITLE] = "Recently played",
    [X4_TR_CAT_RECENT_EMPTY] = "Your recently played games will appear here",
    [X4_TR_CAT_RECENT_LOADING] = "Loading your recent games…",
    [X4_TR_CAT_RECENT_ERROR] = "Could not load your recent games",
    [X4_TR_CAT_RECENT_RETRY] = "SQUARE Retry recent games",
    [X4_TR_CAT_NEXT_GAME] = "Your next game",
    [X4_TR_CAT_FILTER_ALL] = "All",
    [X4_TR_CAT_FILTER_ENTITLED] = "Available",
    [X4_TR_CAT_FILTER_UNCONFIRMED] = "Unconfirmed",
    [X4_TR_CAT_LOADING] = "Loading catalog…",
    [X4_TR_CAT_NO_MATCH] = "No games match this search",
    [X4_TR_CAT_OPEN_ACCOUNT] = "Open your account to load the catalog",
    [X4_TR_CAT_ERROR] = "Could not load the catalog",
    [X4_TR_CAT_PERMISSION_UNKNOWN] = "Access not yet confirmed",
    [X4_TR_CAT_NOT_AVAILABLE] = "Unavailable right now",
    [X4_TR_CAT_REFRESH_PROMPT] = "Refresh catalog",
    [X4_TR_CAT_PREPARING] = "Preparing…",
    [X4_TR_CAT_QUERYING] = "Contacting xCloud…",
    [X4_TR_CAT_PLAY_BTN] = "X  Play",
    [X4_TR_CAT_CHOOSE_GAME] = "Select a game",
    [X4_TR_CAT_FOOTER_PUBLIC] = "D-Pad Select   X Play   OPTIONS Settings   L1/R1 Page   L2/R2 Filter   SQUARE Refresh   CIRCLE Account",
    [X4_TR_CAT_FOOTER_LAB] = "D-Pad Select   X Play   OPTIONS Settings   L1/R1 Page   L2/R2 Filter   SQUARE Refresh   CIRCLE Account",
    [X4_TR_CAT_FINISHING_COVERS] = "Finishing cover loading…",
    [X4_TR_CAT_RESULTS_FMT] = "%u results · Page %u/%u · %s",
    [X4_TR_KB_TITLE] = "Search games",
    [X4_TR_KB_PLACEHOLDER] = "Type a title or ID",
    [X4_TR_KB_HELP1] = "X Add   SQUARE Delete   TRIANGLE Clear",
    [X4_TR_KB_HELP2_PUBLIC] = "R1 Space   R2 Apply   CIRCLE Back",
    [X4_TR_KB_HELP2_LAB] = "R1 Space   R2 Apply   CIRCLE Back",
    [X4_TR_ASSET_DOWNLOADING] = "Downloading…",
    [X4_TR_ASSET_READY] = "Ready",
    [X4_TR_ASSET_RETRY_PENDING] = "Retry pending",
    [X4_TR_ASSET_MISSING] = "No image",
    [X4_TR_ASSET_PENDING] = "Pending",
    [X4_TR_ASSET_SMALL_LOADING] = "Loading",
    [X4_TR_ASSET_SMALL_RETRY] = "Retry",
    [X4_TR_NOTICE_COVER_CLEANUP] = "Could not close cover connection. Close and reopen XCloud4.",
    [X4_TR_NOTICE_QUERY_ERR_FMT] = "Could not start request (%d)",
    [X4_TR_HOME_UPDATE_AVAILABLE_FMT] = "NEW VERSION AVAILABLE: %s · GITHUB.COM/ASP11EDGR/XCLOUD4",
    [X4_TR_UPDATE_CHECKING] = "CHECKING FOR UPDATES...",
    [X4_TR_UPDATE_LATEST] = "YOU ARE USING THE LATEST VERSION",
    [X4_TR_UPDATE_AVAILABLE_FMT] = "NEW VERSION AVAILABLE: %s",
    [X4_TR_UPDATE_FAILED] = "COULD NOT CHECK FOR UPDATES",
    [X4_TR_AUTH_SCAN_PHONE] = "SCAN WITH YOUR PHONE",

    [X4_TR_XSTS_NO_PROFILE] = "Your account doesn't have an Xbox profile yet. Sign in once at xbox.com to create it, then sign in again.",
    [X4_TR_XSTS_UNAVAILABLE_COUNTRY] = "Xbox Live isn't available in your account's country.",
    [X4_TR_XSTS_AGE_VERIFICATION] = "Your account needs age verification at xbox.com.",
    [X4_TR_XSTS_CHILD_ACCOUNT] = "This is a child account: an adult in the Microsoft family must give permission.",
    [X4_TR_DETAIL_XBOX_HTTP_FMT] = "DETAIL XBOX %u    HTTP %d",

    [X4_TR_NET_HINT_DNS] = "The console can't resolve internet addresses (DNS). Check Settings > Network and use a working manual DNS, e.g. 1.1.1.1 / 8.8.8.8.",
    [X4_TR_NET_HINT_TIMEOUT] = "The connection timed out. Check your internet and try again.",
    [X4_TR_NET_HINT_SSL_CERT] = "The console doesn't recognize the server certificate. Update XCloud4 to the latest version.",

    [X4_TR_PIC_SETTINGS_TITLE] = "Picture settings",
    [X4_TR_PIC_SETTINGS_SUBTITLE] = "Saved on the console and applied when you start your next game.",
    [X4_TR_PIC_SETTINGS_SECTION] = "Picture quality",
    [X4_TR_PIC_SETTINGS_OPT_1080P_ALTA] = "1080p High (recommended)",
    [X4_TR_PIC_SETTINGS_OPT_1080P] = "1080p",
    [X4_TR_PIC_SETTINGS_OPT_720P] = "720p",
    [X4_TR_PIC_SETTINGS_TAG_1080P_ALTA] = "~10-13 Mbps",
    [X4_TR_PIC_SETTINGS_TAG_1080P] = "~5-8 Mbps",
    [X4_TR_PIC_SETTINGS_TAG_720P] = "~7 Mbps",
    [X4_TR_PIC_SETTINGS_DESC_1080P_ALTA] = "1920x1080 at 60 fps, about 10-13 Mbps. The sharpest picture. Needs a stable connection of 15 Mbps or more (wired is best). Recommended.",
    [X4_TR_PIC_SETTINGS_DESC_1080P] = "1920x1080 at 60 fps, up to 8 Mbps. Uses less bandwidth with a bit more compression. Good if the High option stutters.",
    [X4_TR_PIC_SETTINGS_DESC_720P] = "1280x720 at 60 fps, about 7 Mbps. Less delay, because the console decodes each frame faster, but the picture is less sharp.",
    [X4_TR_PIC_SETTINGS_FOOTER] = "UP/DOWN Choose     X Save     CIRCLE Cancel",
    [X4_TR_PIC_SETTINGS_BANNER_1080P_ALTA] = "1080p · High",
    [X4_TR_PIC_SETTINGS_BANNER_1080P] = "1080p",
    [X4_TR_PIC_SETTINGS_BANNER_720P] = "720p",

    [X4_TR_OPTIONS_TAB_PICTURE] = "Picture",
    [X4_TR_OPTIONS_TAB_SERVER] = "Server",
    [X4_TR_OPTIONS_FOOTER] = "L1/R1 Tab     UP/DOWN Choose     X Save     CIRCLE Cancel",
    [X4_TR_SERVER_SETTINGS_SUBTITLE] = "ms are measured in your last game on each server. Applied when the catalog loads and when you start your next game.",
    [X4_TR_SERVER_OPT_AUTO] = "Automatic (recommended)",
    [X4_TR_SERVER_TAG_AUTO_FMT] = "Xbox picks: %s",
    [X4_TR_SERVER_LAST_GAME] = "last game",
    [X4_TR_SERVER_NOT_MEASURED] = "not measured",
    [X4_TR_SERVER_MEASURING] = "measuring…",
    [X4_TR_SERVER_NO_RESPONSE] = "no response",
    [X4_TR_SERVER_NO_LIST] = "Sign in to see the servers Xbox offers to your account.",
    [X4_TR_SERVER_NOT_AVAILABLE] = "Your chosen server is no longer available; Automatic is used.",
    [X4_TR_SERVER_FIRST_TITLE] = "Choose your server",
    [X4_TR_SERVER_FIRST_SUBTITLE] = "Pick the one with the lowest ms. You can change it anytime in OPTIONS.",
    [X4_TR_ACCOUNT_HINT_FIRST_SERVER] = "AFTER SIGNING IN YOU CAN CHOOSE YOUR SERVER.",
    [X4_TR_CAT_CHOOSE_SERVER_WAIT] = "Choose your server to continue",

    [X4_TR_CARD_FORTNITE_TITLE] = "FORTNITE",
    [X4_TR_CARD_FORTNITE_DESC] = "NO GAME PASS?\nPLAY FORTNITE FREE\nWITH YOUR\nMICROSOFT ACCOUNT.",
    [X4_TR_X_PLAY] = "X  PLAY",
    [X4_TR_FORTNITE_TITLE] = "FORTNITE",
    [X4_TR_FORTNITE_PIC_QUALITY] = "PICTURE QUALITY",
    [X4_TR_FORTNITE_HINT] = "If you notice stutters or your internet is slow, choose 1080p or 720p.",
    [X4_TR_FORTNITE_FOOTER] = "UP/DOWN QUALITY     X PLAY     OPTIONS SERVER     CIRCLE BACK",
    [X4_TR_FORTNITE_UNAVAILABLE] = "Fortnite isn't available for your account on this server. Try another server in OPTIONS.",
    [X4_TR_FORTNITE_SEARCHING] = "Finding Fortnite on server…",
    [X4_TR_FORTNITE_ERROR_FOOTER] = "OPTIONS SERVER     CIRCLE BACK",
    [X4_TR_CLOUD_UNAVAILABLE_FMT] = "Xbox Cloud Gaming isn't available for your location or account (HTTP %d). XCloud4 can only use the servers Xbox offers you.",

    [X4_TR_APPLE_YOUR_ACCOUNT] = "Your account",
    [X4_TR_APPLE_HOME_TITLE] = "Your next game.",
    [X4_TR_APPLE_HOME_SUBTITLE] = "Choose how you want to play.",
    [X4_TR_APPLE_CARD_EXPLORE_TITLE] = "Explore catalog",
    [X4_TR_APPLE_CARD_EXPLORE_SUB] = "Find your next adventure",
    [X4_TR_APPLE_BTN_ENTER] = "Enter",
    [X4_TR_APPLE_CARD_FORTNITE_TITLE] = "Fortnite",
    [X4_TR_APPLE_CARD_FORTNITE_SUB] = "Direct access",
    [X4_TR_APPLE_CARD_ACCOUNT_TITLE] = "Account",
    [X4_TR_APPLE_CARD_ACCOUNT_SUB_OUT] = "Sign in",
    [X4_TR_APPLE_CARD_ACCOUNT_SUB_IN] = "Signed in",
    [X4_TR_APPLE_PILL_SETTINGS] = "Settings",
    [X4_TR_APPLE_PILL_ABOUT] = "About",
    [X4_TR_APPLE_PILL_SUPPORT] = "Support",
    [X4_TR_APPLE_HINT_SELECT] = "Select",
    [X4_TR_APPLE_HINT_SETTINGS] = "Settings",
    [X4_TR_APPLE_HINT_SEARCH] = "Search",
    [X4_TR_APPLE_HINT_BACK] = "Back",
    [X4_TR_APPLE_HINT_SERVER_FIRST] = "Once signed in, you can choose your server",
    [X4_TR_APPLE_TAB_DISCOVER] = "Discover",
    [X4_TR_APPLE_TAB_CATALOG] = "Catalog",
    [X4_TR_APPLE_TAB_RECENTS] = "Recent",
    [X4_TR_APPLE_TAB_FORTNITE] = "Fortnite",
    [X4_TR_APPLE_HERO_FEATURED] = "FEATURED",
    [X4_TR_APPLE_HERO_PLAY_NOW] = "Play now",
    [X4_TR_APPLE_HERO_DEFAULT_SUB] = "Stream instantly on the cloud from your PS4 console.",
    [X4_TR_APPLE_ROW_CONTINUE] = "Continue where you left off",
    [X4_TR_APPLE_ROW_POPULAR] = "Popular",
    [X4_TR_APPLE_PS_EXIT_HINT] = "PS button: Exit",
    [X4_TR_APPLE_CARD_FORTNITE_SUB1] = "No Game Pass? Play free",
    [X4_TR_APPLE_CARD_FORTNITE_SUB2] = "with your Microsoft account.",
    [X4_TR_APPLE_BADGE_FREE] = "Free",
    [X4_TR_FORTNITE_REQ_LINE] = "Requires a Microsoft account with an Xbox profile. No Game Pass needed.",
    [X4_TR_APPLE_HINT_RIGHT_STICK_READ_MORE] = "Right stick: read more",
    [X4_TR_ACCESS_TITLE] = "Possible regional restriction",
    [X4_TR_ACCESS_COUNTRY] = "Access country",
    [X4_TR_ACCESS_AUTO] = "Automatic",
    [X4_TR_ACCESS_US] = "United States",
    [X4_TR_ACCESS_BR] = "Brazil",
    [X4_TR_ACCESS_JP] = "Japan",
    [X4_TR_ACCESS_KR] = "Korea",
    [X4_TR_ACCESS_PL] = "Poland",
    [X4_TR_ACCESS_DESCRIPTION] = "If the service isn't available in your country, try another access country. It doesn't change your account's country or the game server.",
    [X4_TR_ACCESS_WARNING] = "Using a different access country may go against Xbox's terms, and Microsoft could restrict your account. Use it at your own risk.",
    [X4_TR_ACCESS_TRY] = "Try access",
    [X4_TR_ACCESS_HELP] = "More help",
    [X4_TR_ACCESS_CONTINUE] = "I understand, continue",
    [X4_TR_ACCESS_CONFIRM_TITLE] = "Before trying another country",
    [X4_TR_ACCESS_TRYING_FMT] = "Trying access from %s…",
    [X4_TR_ACCESS_SUCCESS_FMT] = "Access configured: %s",
    [X4_TR_ACCESS_SAVE_ERROR] = "Couldn't save. The access country wasn't changed. Try again.",
    [X4_TR_ACCESS_TAB] = "Access",
    [X4_TR_ACCESS_NEXT_QUERY] = "Applies to the next query or game.",
    [X4_TR_ACCESS_BADGE_FMT] = " · Access: %s",
    [X4_TR_ACCESS_CANCEL] = "Cancel",
    [X4_TR_ACCESS_BACK] = "Back",
    [X4_TR_ACCESS_SAVE] = "Save",
    [X4_TR_UPDATE_NOTICE_TITLE] = "New version available",
    [X4_TR_UPDATE_NOTICE_VERSION_FMT] = "XCloud4 %s is available. You have %s.",
    [X4_TR_UPDATE_NOTICE_DESCRIPTION] = "Scan it to download it from GitHub. Install it with GoldHEN > Package Installer.",
    [X4_TR_UPDATE_NOTICE_CLOSE] = "Close",
    [X4_TR_UPDATE_NOTICE_TEST] = "Test update notice",
    [X4_TR_UPDATE_NOTICE_TEST_QUEUED] = "The test notice will appear when you return Home.",
};

const char *x4_tr(enum X4TextId id)
{
    if (id < 0 || id >= X4_TR_COUNT) return "";
    const char *const *table = (current_lang == X4_LANG_ES) ? table_es : table_en;
    return table[id] ? table[id] : "";
}

int x4_lang_is_cloud_unavailable(int error, int http_status, uint32_t xerr,
    const char *offering, const char *stage)
{
    if (xerr != 0) return 0;
    int is_4xx = (http_status >= 400 && http_status < 500);
    int is_xbox_offer_err = (error == -2010 /* X4_AUTH_E_XBOX */ || error == 0);
    int not_other_stage = (!stage || !stage[0] || !strcmp(stage, "Xbox Cloud Gaming rechazo la cuenta"));
    int is_second_offer = (!offering || !offering[0] || !strcmp(offering, "xgpuwebf2p"));
    return (is_4xx && is_xbox_offer_err && not_other_stage && is_second_offer) ? 1 : 0;
}

const char *x4_lang_catalog_error_msg(int error, int http_status, uint32_t xerr,
    const char *offering, const char *stage, char *buf, unsigned cap)
{
    if (xerr != 0) {
        if (xerr == 2148916233u) return x4_tr(X4_TR_XSTS_NO_PROFILE);
        if (xerr == 2148916235u) return x4_tr(X4_TR_XSTS_UNAVAILABLE_COUNTRY);
        if (xerr == 2148916236u || xerr == 2148916237u) return x4_tr(X4_TR_XSTS_AGE_VERIFICATION);
        if (xerr == 2148916238u) return x4_tr(X4_TR_XSTS_CHILD_ACCOUNT);
        if (buf && cap > 0) {
            snprintf(buf, cap, "%s (Xbox 0x%08X)", x4_tr(X4_TR_CAT_ERROR), xerr);
            return buf;
        }
        return x4_tr(X4_TR_CAT_ERROR);
    }
    /* Both offerings (xgpuweb and xgpuwebf2p) rejected with 4xx and xerr == 0 */
    if (x4_lang_is_cloud_unavailable(error, http_status, xerr, offering, stage) && buf && cap > 0) {
        snprintf(buf, cap, x4_tr(X4_TR_CLOUD_UNAVAILABLE_FMT), http_status);
        return buf;
    }
    return NULL;
}

static const struct {
    const char *es;
    const char *en;
} stage_map[] = {
    {"sin cuenta Microsoft", "no Microsoft account signed in"},
    {"solicitando codigo a Microsoft", "requesting code from Microsoft"},
    {"esperando autorizacion", "waiting for authorization"},
    {"cuenta Microsoft autorizada", "Microsoft account authorized"},
    {"acceso cancelado", "sign-in cancelled"},
    {"el codigo caduco", "the code expired"},
    {"token Microsoft caducado", "Microsoft token expired"},
    {"la sesion de Microsoft caduco", "Microsoft session expired"},
    {"verificando token", "verifying token"},
    {"TLS con Microsoft verificado", "Microsoft TLS verified"},
    {"acceso rechazado en la cuenta", "sign-in denied on account"},
    {"Microsoft no reconoce el codigo", "Microsoft did not recognize the code"},
    {"Microsoft no acepta esta aplicacion", "Microsoft rejected this application"},
    {"Microsoft rechazo los permisos pedidos", "Microsoft rejected the requested permissions"},
    {"Microsoft rechazo la solicitud", "Microsoft rejected the request"},
    {"estado HTTP inesperado", "unexpected HTTP status"},
    {"respuesta de error no valida", "invalid error response"},
    {"respuesta de token no valida", "invalid token response"},
    {"respuesta de codigo no valida", "invalid code response"},
    {"pagina de verificacion desconocida", "unknown verification page"},
    {"metadatos de Microsoft no validos", "invalid Microsoft metadata"},
    {"inicio HTTPS (red/TLS)", "HTTPS startup (network/TLS)"},
    {"sin memoria para el acceso", "out of memory for sign-in"},
    {"no se pudo iniciar el acceso", "could not start sign-in"},
    {"sin catalogo", "no catalog loaded"},
    {"iniciando HTTPS", "starting HTTPS"},
    {"token de usuario Xbox", "Xbox user token"},
    {"autorizacion XSTS", "XSTS authorization"},
    {"credenciales de Xbox Cloud Gaming", "Xbox Cloud Gaming credentials"},
    {"credenciales de juegos gratuitos", "free-to-play credentials"},
    {"lista de titulos de la region", "regional title list"},
    {"nombres de Microsoft Store", "Microsoft Store titles"},
    {"catalogo listo", "catalog ready"},
    {"consulta cancelada", "request cancelled"},
    {"solicitando sesion en la nube", "requesting cloud session"},
    {"esperando respuesta de Xbox", "waiting for Xbox response"},
    {"Xbox prepara la sesion", "Xbox is preparing the session"},
    {"esperando recursos de Xbox", "waiting for Xbox resources"},
    {"Xbox lista para negociar la conexion", "Xbox ready to negotiate connection"},
    {"sesion aprovisionada por Xbox", "session provisioned by Xbox"},
    {"Xbox reporto un fallo de la sesion", "Xbox reported a session failure"},
    {"autorizando conexion con Microsoft", "authorizing connection with Microsoft"},
    {"enviando autorizacion a Xbox", "sending authorization to Xbox"},
    {"preparando oferta WebRTC", "preparing WebRTC offer"},
    {"esperando respuesta SDP de Xbox", "waiting for Xbox SDP answer"},
    {"negociando ruta ICE con Xbox", "negotiating ICE route with Xbox"},
    {"recibiendo imagen y sonido de Xbox", "receiving audio and video from Xbox"},
    {"recibiendo video de Xbox", "receiving video from Xbox"},
    {"recibiendo audio de Xbox", "receiving audio from Xbox"},
    {"conectado con Xbox, esperando medios", "connected to Xbox, waiting for media"},
    {"esperando cierre seguro del control", "waiting for safe controller shutdown"},
    {"cerrando sesion en Xbox", "closing session on Xbox"},
    {"sesion cerrada en Xbox", "session closed on Xbox"},
    {"sesion cancelada", "session cancelled"},
    {"Xbox no inicio la sesion", "Xbox did not start the session"},
    {"Xbox no preparo la sesion a tiempo", "Xbox did not prepare the session in time"},
    {"Xbox rechazo la autorizacion de conexion", "Xbox rejected connection authorization"},
    {"WebRTC no negocio a tiempo", "WebRTC did not negotiate in time"},
    {"WebRTC no conecto a tiempo", "WebRTC did not connect in time"},
    {"se perdio la conexion WebRTC", "WebRTC connection lost"},
    {"fallo la conexion WebRTC", "WebRTC connection failed"},
    {"WebRTC conectado sin recibir imagen o sonido", "WebRTC connected without receiving audio or video"},
    {"no se confirmo el cierre de la sesion", "session close was not confirmed"},
    {"Esperando 1080p de Xbox…", "Waiting for 1080p from Xbox…"},
    {"no se pudieron preparar o cerrar los medios nativos", "could not prepare or close native media"},
};

const char *x4_tr_stage(const char *stage)
{
    if (!stage || !stage[0]) return "";
    if (current_lang == X4_LANG_ES) return stage;
    const char es_prefix[] = "Xbox está preparando el juego… ";
    size_t es_pfx_len = strlen(es_prefix);
    if (!strncmp(stage, es_prefix, es_pfx_len)) {
        static char dynamic_stage[128];
        snprintf(dynamic_stage, sizeof(dynamic_stage), "Xbox is getting your game ready… %s", stage + es_pfx_len);
        return dynamic_stage;
    }
    for (size_t i = 0; i < sizeof(stage_map) / sizeof(stage_map[0]); ++i) {
        if (!strcmp(stage, stage_map[i].es)) return stage_map[i].en;
    }
    return stage;
}
