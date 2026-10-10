/* SPDX-License-Identifier: GPL-3.0-only */
#include "screen.h"
#include "canvas.h"
#include "../auth/auth_profile.h"
#include <stdio.h>
#include <string.h>
static const uint32_t BG = 0xff1b1510, PANEL = 0xff30261c;
static const uint32_t WHITE = 0xfff5f1eb, MUTED = 0xffb6a899, GREEN = 0xff72d86e;
void x4_screen_update(X4Screen *s, uint32_t pressed, uint32_t held)
{
    if (pressed & ORBIS_PAD_BUTTON_OPTIONS) s->exit_requested = 1;
    /* Main waits for the session worker to close the remote session. */
    if (s->page == 6) return;
    if ((pressed & ORBIS_PAD_BUTTON_CIRCLE) && (s->page != 1 || (held & ORBIS_PAD_BUTTON_L1))) {
        s->page = s->page == 5 ? 4 : 0; return;
    }
    if (s->page == 0) {
        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_UP)) s->selected = (s->selected + 3) % 4;
        if (pressed & (ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_DOWN)) s->selected = (s->selected + 1) % 4;
        if (pressed & ORBIS_PAD_BUTTON_CROSS) s->page = s->selected + 1;
    }
}
static void header(uint32_t *p, const X4Controller *c)
{
    x4_rect(p, 0, 0, X4_WIDTH, X4_HEIGHT, BG);
    x4_rect(p, 84, 88, 12, 78, GREEN);
    x4_text(p, 122, 92, 9, "XCLOUD4", WHITE);
    x4_text(p, 1310, 110, 3, "VERSION 0.7.29", MUTED);
    x4_rect(p, 84, 205, 1752, 2, PANEL);
    x4_text(p, 84, 963, 3, c->data.connected ? "DUALSHOCK 4 CONECTADO" : "CONECTA TU DUALSHOCK 4", c->data.connected ? GREEN : MUTED);
    if (c->error) {
        char error[50];
        snprintf(error, sizeof(error), "CONTROL: 0X%08X", (unsigned)c->error);
        x4_text(p, 1110, 963, 3, error, MUTED);
    }
}
static void home(uint32_t *p, const X4Screen *s)
{
    x4_text(p, 84, 267, 5, "TU PUNTO DE PARTIDA", WHITE);
    x4_text(p, 84, 332, 3, "EL PROYECTO AVANZA EN TU CONSOLA", MUTED);
    const char *titles[] = {"CONTROL", "PROYECTO", "IMAGEN Y\nSONIDO", "CUENTA"};
    const char *descriptions[] = {"BOTONES, PALANCAS\nY GATILLOS.", "ESTADO Y SIGUIENTES\nETAPAS DEL PROYECTO.", "UN VIDEO LOCAL\nY TONOS EN ESTEREO.", "ACCESO A MICROSOFT\nCON UN CODIGO."};
    for (int i = 0; i < 4; ++i) {
        int x = 84 + i * 446;
        x4_rect(p, x, 441, 414, 326, PANEL);
        x4_rect(p, x, 441, 414, 6, s->selected == i ? GREEN : PANEL);
        x4_text(p, x + 28, 493, 4, titles[i], WHITE);
        x4_text(p, x + 28, 590, 3, descriptions[i], MUTED);
        x4_text(p, x + 28, 699, 3, s->selected == i ? "X  ABRIR" : "", GREEN);
    }
    x4_text(p, 84, 833, 3, "ABRE CUENTA PARA CONECTAR Y CONSULTAR EL CATALOGO.", MUTED);
    x4_text(p, 84, 894, 3, "CRUCETA  ELEGIR     X  ABRIR     OPTIONS  SALIR", WHITE);
}
static void button(uint32_t *p, int x, int y, const char *label, uint32_t mask, uint32_t buttons)
{
    int active = (buttons & mask) != 0;
    x4_rect(p, x, y, 230, 70, active ? GREEN : PANEL);
    x4_text(p, x + 18, y + 23, 3, label, active ? BG : WHITE);
}
static void stick_panel(uint32_t *p, int x, int y, const char *label, stick value)
{
    x4_text(p, x, y, 3, label, WHITE);
    x4_rect(p, x, y + 42, 230, 150, PANEL);
    x4_rect(p, x + 114, y + 50, 2, 134, MUTED);
    x4_rect(p, x + 8, y + 115, 214, 2, MUTED);
    x4_rect(p, x + 8 + (int)value.x * 204 / 255, y + 50 + (int)value.y * 124 / 255, 10, 10, GREEN);
    char text[32];
    snprintf(text, sizeof(text), "X %03u  Y %03u", (unsigned)value.x, (unsigned)value.y);
    x4_text(p, x, y + 215, 3, text, MUTED);
}
static void controller_page(uint32_t *p, const X4Controller *c)
{
    x4_text(p, 84, 267, 5, "TU CONTROL", WHITE);
    x4_text(p, 84, 332, 3, "PULSA UN BOTON O MUEVE LAS PALANCAS.", MUTED);
    const char *labels[] = {"ARRIBA", "DERECHA", "ABAJO", "IZQUIERDA", "TRIANGULO", "CIRCULO", "X", "CUADRADO", "L1", "R1", "L3", "R3", "L2", "R2", "TOUCHPAD"};
    const uint32_t masks[] = {ORBIS_PAD_BUTTON_UP, ORBIS_PAD_BUTTON_RIGHT, ORBIS_PAD_BUTTON_DOWN, ORBIS_PAD_BUTTON_LEFT,
        ORBIS_PAD_BUTTON_TRIANGLE, ORBIS_PAD_BUTTON_CIRCLE, ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_SQUARE,
        ORBIS_PAD_BUTTON_L1, ORBIS_PAD_BUTTON_R1, ORBIS_PAD_BUTTON_L3, ORBIS_PAD_BUTTON_R3,
        ORBIS_PAD_BUTTON_L2, ORBIS_PAD_BUTTON_R2, ORBIS_PAD_BUTTON_TOUCH_PAD};
    for (int i = 0; i < 15; ++i)
        button(p, 84 + (i % 4) * 250, 410 + (i / 4) * 91, labels[i], masks[i], c->data.buttons);
    stick_panel(p, 1160, 410, "IZQUIERDA", c->data.leftStick);
    stick_panel(p, 1500, 410, "DERECHA", c->data.rightStick);
    char triggers[48];
    snprintf(triggers, sizeof(triggers), "L2 %03u     R2 %03u", (unsigned)c->data.analogButtons.l2, (unsigned)c->data.analogButtons.r2);
    x4_text(p, 1160, 713, 3, triggers, WHITE);
    x4_rect(p, 1160, 758, 255, 12, PANEL);
    x4_rect(p, 1160, 758, c->data.analogButtons.l2, 12, GREEN);
    x4_rect(p, 1500, 758, 255, 12, PANEL);
    x4_rect(p, 1500, 758, c->data.analogButtons.r2, 12, GREEN);
    x4_text(p, 84, 894, 3, "L1 + CIRCULO  VOLVER     OPTIONS  SALIR", WHITE);
}
static void project_page(uint32_t *p)
{
    x4_text(p, 84, 267, 5, "UN CLIENTE NATIVO PARA PS4", WHITE);
    x4_text(p, 84, 356, 3, "OBJETIVO: JUGAR DESDE LA CONSOLA, DIRECTO A XBOX CLOUD GAMING.", MUTED);
    x4_rect(p, 84, 445, 1752, 356, PANEL);
    x4_text(p, 126, 490, 4, "AHORA", GREEN);
    x4_text(p, 126, 550, 3, "CONTROL, IMAGEN Y SONIDO. CUENTA MICROSOFT Y CATALOGO.", WHITE);
    x4_text(p, 126, 625, 4, "DESPUES", WHITE);
    x4_text(p, 126, 685, 3, "CONEXION A UN JUEGO. IMAGEN, SONIDO Y CONTROL EN LA NUBE.", MUTED);
    x4_text(p, 84, 894, 3, "CIRCULO  VOLVER     OPTIONS  SALIR", WHITE);
}
void x4_screen_draw(const X4Screen *s, const X4Controller *c, uint32_t *p)
{
    header(p, c);
    if (s->page == 1) controller_page(p, c);
    else if (s->page == 2) project_page(p);
    else if (s->page == 0) home(p, s);
}

void x4_media_draw(const X4DemoVideo *v, const X4DemoAudio *a, uint32_t *p)
{
    x4_text(p, 84, 260, 5, "IMAGEN Y SONIDO", WHITE);
    x4_rect(p, 82, 328, 964, 556, PANEL);
    if (v->pixels && v->frames) {
        for (unsigned y = 0; y < 552; ++y)
            for (unsigned x = 0; x < 960; ++x)
                p[(330 + y) * X4_WIDTH + 84 + x] = v->pixels[(y * 2 / 3) * X4_SAMPLE_WIDTH + x * 2 / 3];
    } else {
        x4_text(p, 126, 529, 3, v->error ? "VIDEO NO DISPONIBLE" : "INICIANDO VIDEO LOCAL...", MUTED);
    }
    x4_text(p, 1110, 355, 4, "VIDEO H264", v->error ? MUTED : GREEN);
    x4_text(p, 1110, 417, 2, v->stage[0] ? v->stage : "INICIANDO...", WHITE);
    char line[64];
    if (v->error) snprintf(line, sizeof(line), "ERROR 0X%08X", (unsigned)v->error);
    else snprintf(line, sizeof(line), "IMAGENES %u / %u", v->frames, v->total);
    x4_text(p, 1110, 460, 3, line, MUTED);
    x4_text(p, 1110, 536, 4, "SONIDO ESTEREO", atomic_load(&a->error) ? MUTED : GREEN);
    int error = atomic_load(&a->error);
    if (error) snprintf(line, sizeof(line), "ERROR 0X%08X", (unsigned)error);
    else snprintf(line, sizeof(line), "%s", atomic_load(&a->muted) ? "SILENCIADO" : atomic_load(&a->finished) ? "TONOS TERMINADOS" : "TONOS IZQUIERDA / DERECHA");
    x4_text(p, 1110, 598, 2, line, WHITE);
    x4_text(p, 1110, 710, 2, "MUESTRA LOCAL DE 8 SEGUNDOS.\n640 X 368. 48 KHZ.\nXBOX AUN NO ESTA CONECTADO.", MUTED);
    x4_text(p, 84, 915, 3, "X  REPETIR     CUADRADO  SONIDO     CIRCULO  VOLVER     OPTIONS  SALIR", WHITE);
}

void x4_exit_error_draw(int error, uint32_t *p)
{
    if (!error) return;
    char message[96];
    snprintf(message, sizeof(message), "NO SE PUDO SALIR: 0X%08X. OPTIONS PARA REINTENTAR.", (unsigned)error);
    x4_rect(p, 84, 999, 1752, 54, PANEL);
    x4_text(p, 100, 1015, 3, message, WHITE);
}

void x4_auth_draw(const X4AuthSnapshot *a, int busy, int closing, uint32_t *p)
{
    x4_text(p, 84, 267, 5, "TU CUENTA MICROSOFT", WHITE);
    x4_text(p, 84, 332, 3, X4_AUTH_PROFILE_PROMPT, MUTED);
    x4_rect(p, 84, 400, 1752, 424, PANEL);
    if (closing) {
        x4_text(p, 126, 452, 4, "CERRANDO LA CONEXION...", WHITE);
        x4_text(p, 126, 530, 3, "ESPERA UNOS SEGUNDOS PARA VOLVER AL INICIO DE PS4.", MUTED);
    } else if (a->state == X4_AUTH_WAITING) {
        x4_text(p, 126, 443, 3, "INTRODUCE ESTE CODIGO EN MICROSOFT:", WHITE);
        x4_text(p, 126, 493, strlen(a->user_code) <= 16 ? 8 : 4, a->user_code, GREEN);
        x4_text_literal(p, 126, 593, 3, a->verification_uri, WHITE);
        char left[64];
        snprintf(left, sizeof(left), "CADUCA EN %u:%02u", a->seconds_left / 60, a->seconds_left % 60);
        x4_text(p, 126, 661, 3, left, MUTED);
        x4_text(p, 126, 734, 3, "CIRCULO CANCELA EL ACCESO Y VUELVE AL MENU.", MUTED);
    } else {
        const char *title = "LISTO PARA INICIAR SESION";
        switch (a->state) {
        case X4_AUTH_CONNECTING: title = "CONECTANDO CON MICROSOFT..."; break;
        case X4_AUTH_CONNECTED: title = "CONEXION A MICROSOFT CORRECTA"; break;
        case X4_AUTH_AUTHORIZED: title = "CUENTA MICROSOFT AUTORIZADA"; break;
        case X4_AUTH_CANCELLED: title = "ACCESO CANCELADO"; break;
        case X4_AUTH_EXPIRED: title = "EL ACCESO HA CADUCADO"; break;
        case X4_AUTH_DENIED: title = "ACCESO RECHAZADO"; break;
        case X4_AUTH_ERROR: title = "NO SE PUDO COMPLETAR EL ACCESO"; break;
        default: break;
        }
        x4_text(p, 126, 452, 4, title, a->state == X4_AUTH_ERROR ? WHITE : GREEN);
        x4_text(p, 126, 531, 3, a->stage, MUTED);
        if (a->state == X4_AUTH_AUTHORIZED)
            x4_text(p, 126, 601, 3, "PULSA R1 PARA CONSULTAR EL CATALOGO DE XBOX.\nLA CUENTA SE CONSERVA HASTA CERRAR ESTA APP.", WHITE);
        else if (!busy)
            x4_text(p, 126, 601, 3, "X SOLICITA UN CODIGO. CUADRADO REVISA LA CONEXION.\nTU CONTRASENA SE INTRODUCE EN LA PAGINA DE MICROSOFT.", WHITE);
        if (a->error || a->http_status >= 400) {
            char error[72];
            snprintf(error, sizeof(error), "DETALLE 0X%08X    HTTP %d", (unsigned)a->error, a->http_status);
            x4_text(p, 126, 742, 3, error, MUTED);
        }
    }
    x4_text(p, 84, 853, 2, "X INICIAR   CUADRADO CONEXION   TRIANGULO BORRAR SESION   CIRCULO VOLVER   OPTIONS SALIR", WHITE);
    x4_text(p, 84, 899, 2, "R1 CATALOGO DE XBOX. ELIGE UN TITULO CON ACCESO PARA JUGAR.", MUTED);
    x4_text(p, 84, 1004, 2, X4_AUTH_PROFILE_NOTE, MUTED);
}

/* UI-only clipping: external names/IDs cannot create another rendered row.
 * The validated catalog and its naming policy remain unchanged. */
static void catalog_label(char *out, size_t capacity, const char *text, size_t source_capacity)
{
    size_t length = 0;
    while (length + 1 < capacity && length < source_capacity && text[length]) {
        unsigned char character = (unsigned char)text[length];
        out[length] = character < ' ' || character == 127 ? ' ' : (char)character;
        ++length;
    }
    out[length] = 0;
}

void x4_catalog_draw(const X4CatalogSnapshot *c, const X4CatalogBrowser *b, int busy, uint32_t *p)
{
    x4_text(p, 84, 260, 5, "CATALOGO DE XBOX", WHITE);
    char line[160];
    if (!c || c->state != X4_CATALOG_READY || c->count > X4_CATALOG_MAX || !b) {
        x4_rect(p, 84, 388, 1752, 452, PANEL);
        const char *title = c && c->state == X4_CATALOG_LOADING ? "CONSULTANDO TU CATALOGO..." :
            c && c->state == X4_CATALOG_CANCELLED ? "CONSULTA CANCELADA" :
            c && c->state == X4_CATALOG_IDLE ? "AUTORIZA TU CUENTA EN CUENTA" :
            "NO SE PUDO CARGAR EL CATALOGO";
        x4_text(p, 126, 442, 4, title, busy ? GREEN : WHITE);
        if (c) x4_text(p, 126, 525, 3, c->stage, MUTED);
        if (c && (c->error || c->http_status >= 400 || c->xerr)) {
            snprintf(line, sizeof(line), "DETALLE 0X%08X   HTTP %d   XERR %u",
                (unsigned)c->error, c->http_status, (unsigned)c->xerr);
            x4_text(p, 126, 604, 3, line, WHITE);
        }
        x4_text(p, 126, 727, 3, "CUADRADO REINTENTA. CIRCULO VUELVE A TU CUENTA.", MUTED);
        x4_text(p, 84, 929, 2, "CUADRADO ACTUALIZAR   CIRCULO CUENTA   OPTIONS SALIR", WHITE);
    } else {
        static const char *groups[X4_CATALOG_FILTER_COUNT] = {"TODOS", "CON ACCESO", "POR CONFIRMAR"};
        bool valid = !b->criteria_dirty && b->filtered_count <= c->count &&
            (unsigned)b->filter < X4_CATALOG_FILTER_COUNT;
        snprintf(line, sizeof(line), "BUSCAR: %.48s", b->query[0] ? b->query : "SIN BUSQUEDA");
        x4_text(p, 84, 322, 3, line, b->editing ? GREEN : WHITE);
        snprintf(line, sizeof(line), "TODOS %u   CON ACCESO %u   POR CONFIRMAR %u   COINCIDEN %u",
            b->group_counts[X4_CATALOG_FILTER_ALL], b->group_counts[X4_CATALOG_FILTER_CONFIRMED_ACCESS],
            b->group_counts[X4_CATALOG_FILTER_UNCONFIRMED_ACCESS], valid ? b->filtered_count : 0);
        x4_text(p, 84, 365, 2, line, MUTED);
        snprintf(line, sizeof(line), "GRUPO: %s   %u TITULOS RECIBIDOS%s",
            (unsigned)b->filter < X4_CATALOG_FILTER_COUNT ? groups[b->filter] : "NO DISPONIBLE",
            c->total, c->truncated ? "   LIMITE LOCAL" : "");
        x4_text(p, 84, 397, 2, line, GREEN);
        if (b->editing) {
            x4_rect(p, 84, 427, 708, 410, PANEL);
            for (unsigned key = 0; key < X4_CATALOG_KEYBOARD_KEYS; ++key) {
                int x = 126 + (int)(key % X4_CATALOG_KEYBOARD_COLUMNS) * 96;
                int y = 445 + (int)(key / X4_CATALOG_KEYBOARD_COLUMNS) * 65;
                int active = key == b->keyboard_cursor;
                char character[2] = {key < 26 ? (char)('A' + key) : (char)('0' + key - 26), 0};
                x4_rect(p, x, y, 76, 53, active ? GREEN : BG);
                x4_text(p, x + 27, y + 13, 4, character, active ? BG : WHITE);
            }
            x4_text(p, 850, 451, 3, "CRUCETA ELIGE LA LETRA", WHITE);
            x4_text(p, 850, 507, 3, "X ESCRIBIR", WHITE);
            x4_text(p, 850, 563, 3, "CUADRADO BORRAR LA ULTIMA", WHITE);
            x4_text(p, 850, 619, 3, "TRIANGULO LIMPIAR BUSQUEDA", WHITE);
            x4_text(p, 850, 675, 3, "R1 ESPACIO", WHITE);
            x4_text(p, 850, 731, 3, "R2 APLICAR Y VER RESULTADOS", GREEN);
            x4_text(p, 850, 787, 3, "CIRCULO VOLVER A RESULTADOS", MUTED);
            x4_text(p, 84, 862, 2, "HASTA 48 LETRAS, NUMEROS O ESPACIOS. BUSCA EN NOMBRES E IDENTIFICADORES.", MUTED);
            x4_text(p, 84, 929, 2, "R2 APLICAR   CIRCULO RESULTADOS   OPTIONS SALIR", WHITE);
        } else {
            unsigned source_index = 0;
            bool selected_valid = x4_catalog_browser_selected_index(b, c, &source_index);
            unsigned start = selected_valid ? b->selected / 8 * 8 : 0;
            for (unsigned row = 0; valid && row < 8 && start + row < b->filtered_count; ++row) {
                unsigned visible = start + row;
                unsigned index = b->indices[visible];
                if (index >= c->count) continue;
                const X4CatalogTitle *title = &c->titles[index];
                int y = 440 + (int)row * 50;
                int active = selected_valid && visible == b->selected;
                x4_rect(p, 84, y, 1752, 42, active ? GREEN : PANEL);
                char name[73];
                catalog_label(name, sizeof(name), title->name[0] ? title->name : title->id,
                    title->name[0] ? sizeof(title->name) : sizeof(title->id));
                x4_text_literal(p, 110, y + 11, 3, name, active ? BG : WHITE);
                x4_text(p, 1490, y + 14, 2, title->entitled ? "CON ACCESO" : "POR CONFIRMAR", active ? BG : MUTED);
            }
            if (!c->count) x4_text(p, 126, 500, 4, "XBOX DEVOLVIO UNA LISTA VACIA", WHITE);
            else if (valid && !b->filtered_count) {
                x4_text(p, 126, 500, 4, "NO HAY TITULOS QUE COINCIDAN", WHITE);
                x4_text(p, 126, 567, 3, "CAMBIA LA BUSQUEDA O EL GRUPO DE ACCESO.", MUTED);
            }
            if (selected_valid) {
                char id[71];
                catalog_label(id, sizeof(id), c->titles[source_index].id, sizeof(c->titles[source_index].id));
                snprintf(line, sizeof(line), "%u/%u   ID: %s", b->selected + 1, b->filtered_count, id);
                x4_text_literal(p, 84, 850, 2, line, MUTED);
            }
            x4_text(p, 84, 898, 2, "TRIANGULO BUSCAR   L2/R2 GRUPOS   CRUCETA ELEGIR   L1/R1 PAGINAS", WHITE);
            x4_text(p, 84, 929, 2, "X SESION   CUADRADO ACTUALIZAR   CIRCULO CUENTA   OPTIONS SALIR", WHITE);
        }
    }
    x4_text(p, 84, 1004, 2, "CON ACCESO: PERMISO CONFIRMADO POR XBOX. POR CONFIRMAR: ACCESO SIN VERIFICAR.", MUTED);
}

void x4_session_draw(const X4SessionSnapshot *s, int busy, int closing, uint32_t *p)
{
    x4_text(p, 84, 260, 5, "SESION DE JUEGO", WHITE);
    char line[160];
    snprintf(line, sizeof(line), "%.85s", s->title_name[0] ? s->title_name : "TITULO SELECCIONADO");
    x4_text(p, 84, 328, 3, line, MUTED);
    x4_rect(p, 84, 400, 1752, 424, PANEL);
    const char *title = "PREPARANDO LA SOLICITUD...";
    switch (s->state) {
    case X4_SESSION_WAITING: title = "XBOX ESTA PREPARANDO EL JUEGO..."; break;
    case X4_SESSION_READY: title = "XBOX PREPARO LA SESION"; break;
    case X4_SESSION_AUTHORIZING: title = "AUTORIZANDO LA CONEXION..."; break;
    case X4_SESSION_AUTHORIZED: title = "XBOX ACEPTO LA CONEXION"; break;
    case X4_SESSION_NEGOTIATING: title = "NEGOCIANDO IMAGEN Y SONIDO..."; break;
    case X4_SESSION_CONNECTING: title = "CONECTANDO EL JUEGO POR WEBRTC..."; break;
    case X4_SESSION_STREAMING: title = "RECIBIENDO PAQUETES DEL JUEGO"; break;
    case X4_SESSION_STOPPING: title = "CERRANDO LA SESION EN XBOX..."; break;
    case X4_SESSION_CLOSED: title = "SESION CERRADA"; break;
    case X4_SESSION_CANCELLED: title = "SOLICITUD CANCELADA"; break;
    case X4_SESSION_ERROR: title = "NO SE PUDO COMPLETAR LA SESION"; break;
    default: break;
    }
    if (closing) title = "CERRANDO LA SESION Y LA APLICACION...";
    x4_text(p, 126, 448, 4, title, s->ready_seen && !s->cleanup_failed && s->state != X4_SESSION_ERROR ? GREEN : WHITE);
    x4_text(p, 126, 523, 3, s->stage, MUTED);
    if (s->state == X4_SESSION_AUTHORIZED) {
        snprintf(line, sizeof(line), "PREPARANDO LA CONEXION DE IMAGEN Y SONIDO.");
        x4_text(p, 126, 587, 3, line, WHITE);
    } else if (s->state == X4_SESSION_READY) {
        snprintf(line, sizeof(line), "PREPARANDO LA AUTORIZACION DE ESTA SESION.");
        x4_text(p, 126, 587, 3, line, WHITE);
    } else if (s->connection_authorized && !busy)
        x4_text(p, 126, 587, 3, "XBOX ACEPTO EL PERMISO PARA CONECTAR ESTA SESION.", GREEN);
    else if (s->ready_seen && !busy)
        x4_text(p, 126, 587, 3, "XBOX CONFIRMO QUE LA SESION ESTABA LISTA.", GREEN);
    else if (busy) {
        snprintf(line, sizeof(line), "TIEMPO TRANSCURRIDO: %u SEGUNDOS.", s->elapsed_seconds);
        x4_text(p, 126, 587, 3, line, MUTED);
    }
    snprintf(line, sizeof(line), "SDP HTTP %d   ICE HTTP %d   WEBRTC %s", s->sdp_http_status,
        s->ice_http_status, s->rtc_connected ? "CONECTADO" : "EN ESPERA");
    x4_text(p, 126, 650, 3, line, MUTED);
    if (s->cleanup_failed) {
        snprintf(line, sizeof(line), "CIERRE SIN CONFIRMAR: 0X%08X   HTTP %d",
            (unsigned)s->cleanup_error, s->cleanup_http_status);
        x4_text(p, 126, 722, 3, line, WHITE);
    } else if (s->error || s->http_status >= 400) {
        snprintf(line, sizeof(line), "DETALLE 0X%08X   HTTP %d", (unsigned)s->error, s->http_status);
        x4_text(p, 126, 722, 3, line, WHITE);
    }
    x4_text(p, 84, 867, 3, "CIRCULO  CERRAR Y VOLVER AL CATALOGO     OPTIONS  SALIR", WHITE);
    x4_text(p, 84, 940, 2, "EN EL JUEGO, MANTEN L1+R1 PARA MOSTRAR LOS ATAJOS.", MUTED);
    x4_text(p, 84, 1004, 2, "CON L1+R1: CIRCULO CATALOGO / OPTIONS SALIR / CUADRADO AUDIO / PANEL GUIA.", MUTED);
}

void x4_live_status_draw(const X4LiveMediaSnapshot *m, int error, uint32_t *p)
{
    char line[160];
    snprintf(line, sizeof(line), "H264: %llu PAQUETES, %llu IMAGENES   OPUS: %llu PAQUETES, %llu BLOQUES",
        (unsigned long long)m->video_packets, (unsigned long long)m->video_frames,
        (unsigned long long)m->audio_packets, (unsigned long long)m->audio_frames);
    x4_text(p, 126, 790, 2, line, m->video_ready ? GREEN : MUTED);
    if (error || m->video_error || m->audio_error) {
        snprintf(line, sizeof(line), "MEDIOS: 0X%08X  VIDEO: 0X%08X  AUDIO: 0X%08X", (unsigned)error,
            (unsigned)m->video_error, (unsigned)m->audio_error);
        x4_text(p, 84, 922, 2, line, WHITE);
    }
}
void x4_live_overlay(const X4LiveMediaSnapshot *m, const X4SessionSnapshot *s, int muted, int show_controls, uint32_t *p)
{
    /* The game already fills the framebuffer. Do not reserve/crop any rows
     * for normal playback; a fresh complete media draw also erases hints. */
    if (show_controls) {
        x4_rect(p, 0, X4_HEIGHT - 36, X4_WIDTH, 36, BG);
        x4_text(p, 36, X4_HEIGHT - 27, 2,
            "L1+R1: CUADRADO AUDIO / CIRCULO CATALOGO / OPTIONS SALIR / PANEL GUIA", MUTED);
    }
    if (m->audio_error || s->input_error || muted) {
        char line[96];
        const char *audio = m->audio_error ? "SONIDO NO DISPONIBLE" : muted ? "SONIDO SILENCIADO" : "";
        snprintf(line, sizeof(line), "%s%s%s", audio,
            audio[0] && s->input_error ? " / " : "",
            s->input_error ? "CONTROL NO DISPONIBLE" : "");
        x4_rect(p, 20, 20, 620, 36, BG);
        x4_text(p, 32, 29, 2, line, WHITE);
    }
}
