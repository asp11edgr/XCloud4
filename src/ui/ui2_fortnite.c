/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui2_fortnite.h"
#include "ui2_modal.h"
#include "canvas.h"
#include "lang.h"
#include "screen.h"

#include <stdio.h>
#include <strings.h>

bool x4_ui2_fortnite_is_product(const char *product)
{
    return product && !strcasecmp(product, X4_FORTNITE_PRODUCT_ID);
}

void x4_ui2_fortnite_begin(X4Ui2Fortnite *view)
{
    *view = (X4Ui2Fortnite){.phase = X4_UI2_FORTNITE_QUEUED};
}

void x4_ui2_fortnite_started(X4Ui2Fortnite *view, int result)
{
    if (view->phase != X4_UI2_FORTNITE_QUEUED) return;
    view->phase = result ? X4_UI2_FORTNITE_ERROR : X4_UI2_FORTNITE_QUERYING;
    view->error = result;
}

void x4_ui2_fortnite_complete(X4Ui2Fortnite *view, const X4CatalogSnapshot *catalog, bool busy)
{
    if (view->phase != X4_UI2_FORTNITE_QUERYING || busy ||
        catalog->state == X4_CATALOG_LOADING) return;
    if (catalog->state == X4_CATALOG_READY && catalog->count > 0 &&
        x4_ui2_fortnite_is_product(catalog->titles[0].product_id)) {
        view->phase = X4_UI2_FORTNITE_READY;
        return;
    }

    view->error = catalog->state == X4_CATALOG_ERROR && catalog->error ?
        catalog->error : X4_AUTH_E_FORTNITE_NOT_FOUND;
    const bool regional = catalog->state == X4_CATALOG_ERROR &&
        x4_lang_is_cloud_unavailable(catalog->error, catalog->http_status,
            catalog->xerr, catalog->offering, catalog->stage);
    view->phase = regional ? X4_UI2_FORTNITE_REGION : X4_UI2_FORTNITE_ERROR;
}

void x4_ui2_fortnite_back(X4Ui2Fortnite *view)
{
    *view = (X4Ui2Fortnite){0};
}

bool x4_ui2_fortnite_waiting(const X4Ui2Fortnite *view)
{
    return view->phase == X4_UI2_FORTNITE_QUEUED || view->phase == X4_UI2_FORTNITE_QUERYING;
}

void x4_ui2_fortnite_draw(uint32_t *pixels, const X4Ui2Fortnite *view,
    const X4CatalogSnapshot *catalog, const X4CatalogAssetView *artwork)
{
    enum { LEFT = X4_UI2_CONTENT_X, RIGHT = X4_UI_SAFE_RIGHT, TITLE_Y = 188,
           ART_X = 1280, ART_Y = 169, ART_W = 508, ART_H = 710, STATUS_Y = 385 };
    x4_rect(pixels, 0, 0, X4_UI_WIDTH, X4_UI_HEIGHT, X4_UI2_BACKGROUND);
    x4_ui2_card(pixels, ART_X, ART_Y, ART_W, ART_H, X4_UI2_RADIUS_CARD);
    if (artwork && artwork->surface[X4_COVER_FORTNITE_POSTER].rgba)
        x4_apple_blit_image_rounded(pixels, ART_X, ART_Y, ART_W, ART_H, X4_UI2_RADIUS_CARD,
            &artwork->surface[X4_COVER_FORTNITE_POSTER], X4_GRAD_NONE);
    else if (artwork && artwork->surface[X4_COVER_ROW].rgba)
        x4_apple_blit_image_rounded(pixels, ART_X + 6, ART_Y + 215, 496, 279, X4_UI2_RADIUS_CARD,
            &artwork->surface[X4_COVER_ROW], X4_GRAD_NONE);
    else if (artwork && artwork->surface[X4_COVER_HOME_FORTNITE].rgba)
        x4_apple_blit_image_exact(pixels, ART_X + (ART_W - 420) / 2, ART_Y + (ART_H - 448) / 2,
            420, 448, X4_UI2_RADIUS_CARD, X4_UI_WIDTH,
            &artwork->surface[X4_COVER_HOME_FORTNITE]);
    else
        x4_ui2_cloud(pixels, ART_X + (ART_W - x4_ui2_cloud_width(80)) / 2,
            ART_Y + (ART_H - 80) / 2, 80, X4_UI2_ACCENT);

    x4_ui2_heading(pixels, LEFT, TITLE_Y, x4_tr(X4_TR_U49_FORTNITE_TITLE));
    (void)x4_ui2_text_bounded(pixels, LEFT, TITLE_Y + 77, X4_UI2_BODY, X4_FONT_REGULAR,
        x4_tr(X4_TR_U49_FORTNITE_SUBTITLE), ART_X - LEFT - 34, 30, 2, X4_UI2_MUTED);
    const bool waiting = x4_ui2_fortnite_waiting(view);
    if (waiting) {
        x4_apple_spinner(pixels, LEFT, STATUS_Y - 5, x4_apple_get_animation_phase());
        (void)x4_ui2_text_bounded(pixels, LEFT + 55, STATUS_Y, X4_UI2_LABEL, X4_FONT_SEMIBOLD,
            x4_tr(X4_TR_U49_FORTNITE_WAIT), ART_X - LEFT - 89, 34, 2, X4_UI2_TEXT);
    } else {
        char message[160], code[48];
        const char *body = x4_tr(X4_TR_U49_FORTNITE_UNAVAILABLE);
        if (catalog && catalog->state == X4_CATALOG_ERROR && catalog->error == view->error) {
            const char *detail = x4_lang_catalog_error_msg(catalog->error, catalog->http_status,
                catalog->xerr, catalog->offering, catalog->stage, message, sizeof(message));
            if (detail) body = detail;
        }
        const int lines = x4_ui2_text_bounded(pixels, LEFT, STATUS_Y, X4_UI2_LABEL,
            X4_FONT_SEMIBOLD, body, ART_X - LEFT - 34, 34, 4, X4_UI2_TEXT);
        snprintf(code, sizeof(code), x4_tr(X4_TR_UI2_MODAL_EXIT_CODE_FMT), (unsigned)view->error);
        if (view->error)
            (void)x4_ui2_text_bounded(pixels, LEFT, STATUS_Y + lines * 34 + 12, X4_UI2_CAPTION,
                X4_FONT_REGULAR, code, RIGHT - LEFT, 26, 1, X4_UI2_MUTED);
    }
    const X4Ui2Hint back = {X4_GLYPH_CIRCLE, x4_tr(X4_TR_APPLE_HINT_BACK)};
    x4_ui2_footer(pixels, &back, 1);
}
