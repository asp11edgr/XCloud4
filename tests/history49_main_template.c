/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <orbis/libkernel.h>

#include "auth/xbox_catalog.h"
#include "ui/game_page.h"
#include "ui/lang.h"
#include "ui/play_history.h"

static const char *history_path;
#undef X4_HISTORY_PATH
#define X4_HISTORY_PATH history_path

#if !X4_PUBLIC
uint64_t sceKernelGetProcessTime(void) { return 1000000; }
#endif

/* @HISTORY@ */

typedef struct {
    bool open;
    unsigned source;
    char id[96], product[X4_PRODUCT_ID_CAP];
} GamePageUi;

static GamePageUi game_page_ui;
static X4CatalogSnapshot catalog;
static X4History play_history, restored;
static UiLocalSession local_session;
static uint64_t input_at;
static unsigned catalog_selected;
static struct { int page; } screen;
static int previous_page;
static struct { bool open; } picture_settings_ui;
static struct { uint64_t video_present_calls; } live_status;
static bool auth_running, pending_launch;
static void *auth, *catalog_assets;
static int cloud_launch, cloud_time;

static bool x4_auth_busy(void *state) { (void)state; return auth_running; }
static bool cloud_launch_allowed(const int *launch, int time, int page, const char *title)
{
    (void)launch;
    (void)time;
    return (page == 0 || page == 5 || page == 8) && title[0];
}

enum { X4_ASSET_FAULT_CLEANUP = 1 };
static void x4_catalog_assets_suspend(void *assets) { (void)assets; }
static int x4_catalog_assets_fault(void *assets) { (void)assets; return 0; }
static bool x4_catalog_assets_idle(void *assets) { (void)assets; return true; }

/* @SELECTED@ */

static void launch_step(bool game_play)
{
    const bool game_page_pressed = game_play;
    const bool catalog_copied = false, recent_copied = false, metadata_changed = false;
    const int pending_auth = -1;
    const unsigned ui_generation = 1;
    static unsigned pending_generation;
    char ui_notice[128];
    /* @LAUNCH@ */
    /* @LAUNCH_GATE@ */
        /* @BEGIN@ */
        auth_running = true;
    }
}

static void sample_step(void)
{
    /* @OBSERVE@ */
    /* @FINISH@ */
}

static void set_title(X4CatalogTitle *title, const char *product)
{
    *title = (X4CatalogTitle){.id = "FIXTURE", .entitled = 1};
    snprintf(title->product_id, sizeof(title->product_id), "%s", product);
}

static void start(const char *product, unsigned source, int page)
{
    screen.page = previous_page = page;
    catalog.state = X4_CATALOG_READY;
    catalog.count = source + 1;
    set_title(&catalog.titles[source], product);
    game_page_ui = (GamePageUi){.open = true, .source = source, .id = "FIXTURE"};
    snprintf(game_page_ui.product, sizeof(game_page_ui.product), "%s", product);
    X4GamePage detail;
    const X4GamePageModel model = {.product_id = product, .available = true, .entitled = true};
    x4_game_page_init(&detail);
    assert(x4_game_page_update(&detail, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_PLAY);
    launch_step(true);
    assert(auth_running && catalog_selected == source && local_session.active);
    assert(!strcmp(local_session.product, product));
    screen.page = 6;
    input_at = 10000000;
    live_status.video_present_calls = 0;
}

static void test_origins(void)
{
    const struct { const char *origin, *product; unsigned index; int page; } games[] = {
        {"Inicio", "GAMEHOME", 6, 0}, {"Descubrir", "GAMEDISC", 5, 5},
        {"Catálogo", "GAMECATALOG", 4, 5}, {"Mis juegos", "GAMEOWNED", 3, 5},
        {"Buscar", "GAMESEARCH", 2, 5}, {"Similares", "GAMESIMILAR", 1, 5},
        {"Fortnite", "BT5P2X999VH2", 0, 8},
    };
    for (size_t i = 0; i < sizeof(games) / sizeof(games[0]); ++i) {
        start(games[i].product, games[i].index, games[i].page);
        sample_step();
        assert(!x4_history_find(&play_history, games[i].product));
        catalog_selected = 0;
        catalog.count = 1;
        set_title(&catalog.titles[0], "DIFFERENT");
        live_status.video_present_calls = 1;
        sample_step();
        const X4HistoryEntry *entry = x4_history_find(&play_history, games[i].product);
        assert(entry && entry->played_s == 0);
        assert(x4_history_load(&restored, history_path));
        assert(x4_history_find(&restored, games[i].product)->played_s == 0);
        input_at += 7000000;
        live_status.video_present_calls = 250;
        sample_step();
        assert(x4_history_find(&play_history, games[i].product)->played_s == 0);
        auth_running = false;
        sample_step();
        assert(!local_session.active && !local_session.product[0]);
        assert(x4_history_load(&restored, history_path));
        assert(x4_history_find(&restored, games[i].product)->played_s == 7);
        assert(!x4_history_find(&restored, "DIFFERENT"));
        printf("PASS historial: %s, 1.ª imagen y 7 s tras cambiar catálogo e índice\n", games[i].origin);
    }
}

enum Failure { NO_FAILURE, FAIL_FOPEN, FAIL_FWRITE, FAIL_FCLOSE, FAIL_RENAME };
static enum Failure fault;
FILE *__real_fopen(const char *, const char *);
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
int __real_fclose(FILE *);
int __real_rename(const char *, const char *);
int __real_remove(const char *);

FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (fault == FAIL_FOPEN && !strcmp(mode, "wb")) { errno = EACCES; return NULL; }
    return __real_fopen(path, mode);
}
size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *file)
{
    if (fault == FAIL_FWRITE) { errno = ENOSPC; return 0; }
    return __real_fwrite(data, size, count, file);
}
int __wrap_fclose(FILE *file)
{
    int result = __real_fclose(file);
    if (fault == FAIL_FCLOSE) { errno = EIO; return EOF; }
    return result;
}
int __wrap_rename(const char *from, const char *to)
{
    if (fault == FAIL_RENAME) { errno = EXDEV; return -1; }
    return __real_rename(from, to);
}
int __wrap_remove(const char *path)
{
    int result = __real_remove(path);
    if (fault) errno = EBUSY;
    return result;
}

static void test_skips_and_errno(void)
{
    start("SAMECYCLE", 0, 5);
    live_status.video_present_calls = 1;
    auth_running = false;
    sample_step();
    assert(!local_session.active && x4_history_find(&play_history, "SAMECYCLE"));
    assert(x4_history_load(&restored, history_path));
    assert(x4_history_find(&restored, "SAMECYCLE")->played_s == 0);

    start("NOIMAGE", 0, 5);
    auth_running = false;
    sample_step();
    assert(!x4_history_find(&play_history, "NOIMAGE"));

    start("", 0, 5);
    live_status.video_present_calls = 1;
    auth_running = false;
    sample_step();
    assert(!local_session.active);

    const int expected[] = {0, EACCES, ENOSPC, EIO, EXDEV};
    const unsigned count = play_history.count;
    const uint64_t revision = play_history.revision;
    for (fault = FAIL_FOPEN; fault <= FAIL_RENAME; ++fault) {
        errno = 0;
        assert(!x4_history_record(&play_history, "UNSAVED", 1, 3, history_path));
        assert(errno == expected[fault]);
        assert(play_history.count == count && play_history.revision == revision);
        ui_history_record(&play_history, "UNSAVED", 3);
    }
    fault = NO_FAILURE;
    assert(x4_history_load(&restored, history_path) && restored.count == count);
    assert(!x4_history_find(&restored, "UNSAVED"));
    puts("PASS historial: omisiones, errno por primitiva y rollback");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    history_path = argv[1];
    test_origins();
    test_skips_and_errno();
    return 0;
}
