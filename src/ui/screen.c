/* SPDX-License-Identifier: GPL-3.0-only */
#include "screen.h"
#include "canvas.h"
#include <stdio.h>
static const uint32_t BG = 0xff1b1510, PANEL = 0xff30261c;
static const uint32_t WHITE = 0xfff5f1eb, MUTED = 0xffb6a899, GREEN = 0xff72d86e;
void x4_screen_update(X4Screen *s, uint32_t pressed, uint32_t held)
{
    if (pressed & ORBIS_PAD_BUTTON_OPTIONS) s->exit_requested = 1;
    if ((pressed & ORBIS_PAD_BUTTON_CIRCLE) && (s->page != 1 || (held & ORBIS_PAD_BUTTON_L1))) {
        s->page = 0; return;
    }
    if (s->page == 0) {
        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_UP)) s->selected = (s->selected + 2) % 3;
        if (pressed & (ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_DOWN)) s->selected = (s->selected + 1) % 3;
        if (pressed & ORBIS_PAD_BUTTON_CROSS) s->page = s->selected + 1;
    }
}
static void header(uint32_t *p, const X4Controller *c)
{
    x4_rect(p, 0, 0, X4_WIDTH, X4_HEIGHT, BG);
    x4_rect(p, 84, 88, 12, 78, GREEN);
    x4_text(p, 122, 92, 9, "XCLOUD4", WHITE);
    x4_text(p, 1310, 110, 3, "VERSION 0.2.1", MUTED);
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
    const char *titles[] = {"CONTROL", "PROYECTO", "IMAGEN Y SONIDO"};
    const char *descriptions[] = {"BOTONES, PALANCAS\nY GATILLOS.", "ESTADO Y SIGUIENTES\nETAPAS DEL PROYECTO.", "UN VIDEO LOCAL\nY TONOS EN ESTEREO."};
    for (int i = 0; i < 3; ++i) {
        int x = 84 + i * 596;
        x4_rect(p, x, 441, 560, 326, PANEL);
        x4_rect(p, x, 441, 560, 6, s->selected == i ? GREEN : PANEL);
        x4_text(p, x + 28, 493, 4, titles[i], WHITE);
        x4_text(p, x + 28, 569, 3, descriptions[i], MUTED);
        x4_text(p, x + 28, 699, 3, s->selected == i ? "X  ABRIR" : "", GREEN);
    }
    x4_text(p, 84, 833, 3, "XBOX CLOUD GAMING AUN NO ESTA CONECTADO.", MUTED);
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
    x4_text(p, 126, 550, 3, "INTERFAZ, CONTROL Y MUESTRA LOCAL DE IMAGEN Y SONIDO.", WHITE);
    x4_text(p, 126, 625, 4, "DESPUES", WHITE);
    x4_text(p, 126, 685, 3, "CUENTA MICROSOFT. CATALOGO. CONEXION Y SESION DE JUEGO.", MUTED);
    x4_text(p, 84, 894, 3, "CIRCULO  VOLVER     OPTIONS  SALIR", WHITE);
}
void x4_screen_draw(const X4Screen *s, const X4Controller *c, uint32_t *p)
{
    header(p, c);
    if (s->page == 1) controller_page(p, c);
    else if (s->page == 2) project_page(p);
    else if (s->page != 3) home(p, s);
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
