/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/ui2_account.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static X4History history;

static void counts(void)
{
    X4Ui2AccountSummary s;
    x4_ui2_account_summary(&history, &s);
    assert(s.games == 0 && s.count == 0 && s.total_seconds == 0);

    strcpy(history.entries[0].product_id, "FICTICIO001");
    history.entries[0].played_s = 3600;
    history.count = 1;
    x4_ui2_account_summary(&history, &s);
    assert(s.games == 1 && s.count == 1 && s.total_seconds == 3600);
    assert(!strcmp(s.most_played[0].product_id, "FICTICIO001"));

    for (unsigned i = 0; i < X4_HISTORY_MAX; ++i) {
        X4HistoryEntry *e = &history.entries[i];
        snprintf(e->product_id, sizeof(e->product_id), "FICTICIO%03u", i);
        e->played_s = i / 2;
        e->last_session_s = i;
    }
    history.count = X4_HISTORY_MAX;
    x4_ui2_account_summary(&history, &s);
    assert(s.games == X4_HISTORY_MAX && s.count == X4_UI2_ACCOUNT_TOP_GAMES);
    assert(s.total_seconds == UINT64_C(62250));
    for (unsigned i = 0; i < s.count; ++i)
        assert(s.most_played[i].last_session_s == X4_HISTORY_MAX - 1 - i);

    history.entries[0].played_s = UINT64_MAX;
    x4_ui2_account_summary(&history, &s);
    assert(s.total_seconds == UINT64_MAX);
    assert(!strcmp(s.most_played[0].product_id, "FICTICIO000"));
}

static void navigation(void)
{
    X4AuthSnapshot auth = {.state = X4_AUTH_AUTHORIZED};
    X4Ui2Account account = {0};
    X4Ui2AccountSummary s = {0};
    X4Ui2AccountModel model = {.auth = &auth, .history = &s};
    const char *id = NULL;
    x4_ui2_account_summary(&history, &s);

    assert(x4_ui2_account_select(&account, &model, &id) == X4_UI2_ACCOUNT_NONE);
    model.remember.available = true;
    assert(x4_ui2_account_select(&account, &model, &id) == X4_UI2_ACCOUNT_REMEMBER);
    assert(x4_ui2_account_move(&account, &model, 0, 1));
    assert(x4_ui2_account_select(&account, &model, &id) == X4_UI2_ACCOUNT_SERVER);
    assert(x4_ui2_account_move(&account, &model, 1, 0));
    assert(x4_ui2_account_select(&account, &model, &id) == X4_UI2_ACCOUNT_OPEN_GAME);
    assert(!strcmp(id, "FICTICIO000"));
    assert(x4_ui2_account_move(&account, &model, 0, 1));
    assert(x4_ui2_account_select(&account, &model, &id) == X4_UI2_ACCOUNT_OPEN_GAME);
    assert(!strcmp(id, "FICTICIO499"));
    assert(x4_ui2_account_move(&account, &model, -1, 0));
    assert(x4_ui2_account_select(&account, &model, &id) == X4_UI2_ACCOUNT_SERVER && !id);
    assert(x4_ui2_account_move(&account, &model, 0, 1));
    assert(x4_ui2_account_select(&account, &model, &id) == X4_UI2_ACCOUNT_COUNTRY);
    assert(x4_ui2_account_move(&account, &model, 0, 1));
    assert(x4_ui2_account_select(&account, &model, &id) == X4_UI2_ACCOUNT_SIGN_OUT);
    assert(!x4_ui2_account_move(&account, &model, 0, 1));
    model.busy = true;
    assert(!x4_ui2_account_move(&account, &model, 0, -1));
    assert(x4_ui2_account_select(&account, &model, &id) == X4_UI2_ACCOUNT_NONE);
    model.busy = false;
    auth.state = X4_AUTH_IDLE;
    assert(x4_ui2_account_select(&account, &model, &id) == X4_UI2_ACCOUNT_SIGN_IN);
    auth.state = X4_AUTH_WAITING;
    assert(x4_ui2_account_select(&account, &model, &id) == X4_UI2_ACCOUNT_NONE);
}

int main(void)
{
    counts();
    navigation();
    puts("T49 account PASS: history 0/1/500, stable top five, saturation and focus");
    return 0;
}
