/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../src/ui/lang.c"

static void check_system_language(void)
{
    static const struct {
        int system;
        enum X4Lang ui;
        const char *locale;
        const char *market;
    } mapping[] = {
        {ORBIS_SYSTEM_PARAM_LANG_SPANISH, X4_LANG_ES, "es-ES", "ES"},
        {ORBIS_SYSTEM_PARAM_LANG_SPANISH_LA, X4_LANG_ES, "es-MX", "MX"},
        {ORBIS_SYSTEM_PARAM_LANG_ENGLISH_US, X4_LANG_EN, "en-US", "US"},
        {ORBIS_SYSTEM_PARAM_LANG_ENGLISH_GB, X4_LANG_EN, "en-GB", "GB"},
        {ORBIS_SYSTEM_PARAM_LANG_PORTUGUESE_BR, X4_LANG_PT_BR, "pt-BR", "BR"},
        {ORBIS_SYSTEM_PARAM_LANG_PORTUGUESE_PT, X4_LANG_PT_BR, "pt-PT", "PT"},
        {ORBIS_SYSTEM_PARAM_LANG_FRENCH, X4_LANG_FR, "fr-FR", "FR"},
        {ORBIS_SYSTEM_PARAM_LANG_FRENCH_CA, X4_LANG_FR, "fr-CA", "CA"},
        {ORBIS_SYSTEM_PARAM_LANG_ITALIAN, X4_LANG_IT, "it-IT", "IT"},
        {ORBIS_SYSTEM_PARAM_LANG_GERMAN, X4_LANG_DE, "de-DE", "DE"},
        {ORBIS_SYSTEM_PARAM_LANG_JAPANESE, X4_LANG_EN, "en-US", "US"},
    };
    for (unsigned i = 0; i < sizeof(mapping) / sizeof(mapping[0]); ++i) {
        x4_lang_apply_system_id(mapping[i].system);
        assert(x4_lang_current() == mapping[i].ui);
        assert(x4_lang_system_ui() == mapping[i].ui);
        char suffix[128];
        snprintf(suffix, sizeof(suffix), "&market=%s&languages=%s&fieldsTemplate=Details",
            mapping[i].market, mapping[i].locale);
        for (enum X4Lang lang = X4_LANG_ES; lang < X4_LANG_COUNT; ++lang) {
            uint64_t revision = x4_lang_ui_revision();
            const bool changed = x4_lang_current() != lang;
            x4_lang_set_ui(lang);
            assert(x4_lang_ui_revision() == revision + changed);
            assert(x4_lang_current() == lang);
            assert(x4_lang_system_ui() == mapping[i].ui);
            assert(!strcmp(x4_lang_locale(), mapping[i].locale));
            assert(!strcmp(x4_lang_market(), mapping[i].market));
            assert(!strcmp(x4_lang_store_suffix(), suffix));
        }
        printf("LANG50_SYSTEM id=%d ui=%d locale=%s market=%s PASS\n", mapping[i].system,
            mapping[i].ui, mapping[i].locale, mapping[i].market);
    }
}


static void check_counts(void)
{
    static const struct {
        const char *game[3], *result[3], *local[3], *cloud[3];
        const char *multiplayer;
    } expected[X4_LANG_COUNT] = {
        [X4_LANG_ES] = {
            {"0 juegos", "1 juego", "2 juegos"}, {"0 resultados", "1 resultado", "2 resultados"},
            {"0 juegos · 43 min jugados", "1 juego · 43 min jugados", "2 juegos · 43 min jugados"},
            {"Más de 0 juegos en la nube", "1 juego en la nube", "Más de 2 juegos en la nube"},
            "Multijugador",
        },
        [X4_LANG_EN] = {
            {"0 games", "1 game", "2 games"}, {"0 results", "1 result", "2 results"},
            {"0 games · 43 min played", "1 game · 43 min played", "2 games · 43 min played"},
            {"More than 0 cloud games", "1 cloud game", "More than 2 cloud games"},
            "Multiplayer",
        },
        [X4_LANG_PT_BR] = {
            {"0 jogos", "1 jogo", "2 jogos"}, {"0 resultados", "1 resultado", "2 resultados"},
            {"0 jogos · 43 min de jogo", "1 jogo · 43 min de jogo", "2 jogos · 43 min de jogo"},
            {"Mais de 0 jogos na nuvem", "1 jogo na nuvem", "Mais de 2 jogos na nuvem"},
            "Multijogador",
        },
        [X4_LANG_FR] = {
            {"0 jeux", "1 jeu", "2 jeux"}, {"0 résultats", "1 résultat", "2 résultats"},
            {"0 jeux · 43 min de jeu", "1 jeu · 43 min de jeu", "2 jeux · 43 min de jeu"},
            {"Plus de 0 jeux dans le cloud", "1 jeu dans le cloud", "Plus de 2 jeux dans le cloud"},
            "Multijoueur",
        },
        [X4_LANG_IT] = {
            {"0 giochi", "1 gioco", "2 giochi"}, {"0 risultati", "1 risultato", "2 risultati"},
            {"0 giochi · 43 min di gioco", "1 gioco · 43 min di gioco", "2 giochi · 43 min di gioco"},
            {"Più di 0 giochi nel cloud", "1 gioco nel cloud", "Più di 2 giochi nel cloud"},
            "Multigiocatore",
        },
        [X4_LANG_DE] = {
            {"0 Spiele", "1 Spiel", "2 Spiele"}, {"0 Ergebnisse", "1 Ergebnis", "2 Ergebnisse"},
            {"0 Spiele · 43 min gespielt", "1 Spiel · 43 min gespielt", "2 Spiele · 43 min gespielt"},
            {"Über 0 Spiele in der Cloud", "1 Spiel in der Cloud", "Über 2 Spiele in der Cloud"},
            "Mehrspieler",
        },
    };
    char actual[128], filter[128];
    for (enum X4Lang lang = X4_LANG_ES; lang < X4_LANG_COUNT; ++lang) {
        x4_lang_set_ui(lang);
        assert(!strcmp(x4_tr(X4_TR_U49_COLLECTION_MULTIPLAYER_NAME), expected[lang].multiplayer));
        for (unsigned n = 0; n <= 2; ++n) {
            snprintf(actual, sizeof(actual), "%u %s", n, x4_tr_count(n, X4_TR_U49_GAME, X4_TR_U49_GAMES));
            assert(!strcmp(actual, expected[lang].game[n]));
            snprintf(actual, sizeof(actual), "%u %s", n, x4_tr_count(n, X4_TR_U49_RESULT, X4_TR_U49_RESULTS));
            assert(!strcmp(actual, expected[lang].result[n]));
            snprintf(actual, sizeof(actual), x4_tr_count(n, X4_TR_50_ACCOUNT_LOCAL_ONE_FMT,
                X4_TR_U49_ACCOUNT_LOCAL_FMT), n, "43 min");
            assert(!strcmp(actual, expected[lang].local[n]));
            snprintf(actual, sizeof(actual), x4_tr_count(n, X4_TR_50_HOME_CLOUD_COUNT_ONE_FMT,
                X4_TR_U49_HOME_CLOUD_COUNT_FMT), n);
            assert(!strcmp(actual, expected[lang].cloud[n]));
            snprintf(actual, sizeof(actual), x4_tr(X4_TR_50_CATALOG_FILTER_COUNT_FMT),
                x4_tr(X4_TR_U49_COLLECTION_MULTIPLAYER_NAME), n);
            snprintf(filter, sizeof(filter), "%s · %u", expected[lang].multiplayer, n);
            assert(!strcmp(actual, filter));
            snprintf(actual, sizeof(actual), x4_tr(X4_TR_50_LIBRARY_TAB_COUNT_FMT),
                x4_tr(X4_TR_U49_COLLECTION_MULTIPLAYER_NAME), n);
            snprintf(filter, sizeof(filter), "%s (%u)", expected[lang].multiplayer, n);
            assert(!strcmp(actual, filter));
        }
    }
    puts("LANG50_COUNTS_PASS counts=0,1,2 locales=6");
}

static void check_catalog_and_stage_labels(void)
{
    x4_lang_set(X4_LANG_ES, "es-MX", "MX");
    x4_lang_set_ui(X4_LANG_PT_BR);
    assert(!strcmp(x4_tr(X4_TR_U49_GENRE), "Gênero"));
    assert(!strcmp(x4_tr(X4_TR_U49_ACCESS), "Acesso"));
    assert(!strcmp(x4_tr(X4_TR_U49_SORT_BY), "Ordenar por"));
    assert(!strcmp(x4_lang_store_category("Disparos"), x4_lang_store_category("Shooter")));
    assert(!strcmp(x4_lang_locale(), "es-MX"));
    assert(!strcmp(x4_lang_market(), "MX"));
    x4_lang_set_ui(X4_LANG_IT);
    assert(!strcmp(x4_tr_stage("solicitud de token Xbox"), "richiesta del token Xbox"));
    assert(!strcmp(x4_tr_stage("negociacion TLS"), "negoziazione TLS"));
    x4_lang_set_ui(X4_LANG_DE);
    assert(!strcmp(x4_tr_stage("solicitud de token Xbox"), "Xbox-Token-Anfrage"));
    assert(!strcmp(x4_tr_stage("cabeceras publicas"), "öffentliche Header"));
    puts("LANG50_LABELS_PASS");
}

int main(void)
{
    check_system_language();
    check_counts();
    check_catalog_and_stage_labels();
    static const char *const *const translated[] = {table_pt_br, table_fr, table_it, table_de};
    unsigned visible = 0;
    for (unsigned id = 0; id < X4_TR_ALL_COUNT; ++id) {
        assert((table_es[id] == NULL) == (table_en[id] == NULL));
        if (!table_en[id]) continue;
        ++visible;
        for (unsigned i = 0; i < sizeof(translated) / sizeof(translated[0]); ++i) {
#if X4_PUBLIC
            assert(translated[i][id] && *translated[i][id]);
#else
            if (!translated[i][id]) {
                assert(x4_tr_for_lang((enum X4Lang)(X4_LANG_PT_BR + i), (enum X4TextId)id) == table_en[id]);
                continue;
            }
#endif
            assert(translated[i][id] == x4_tr_for_lang((enum X4Lang)(X4_LANG_PT_BR + i), (enum X4TextId)id));
        }
    }
    assert(visible >= 600);
    for (enum X4Lang lang = X4_LANG_PT_BR; lang < X4_LANG_COUNT; ++lang) {
        x4_lang_set_ui(lang);
        for (size_t i = 0; i < sizeof(stage_map) / sizeof(stage_map[0]); ++i) {
            const char *expected = stage_translation(i, lang);
            assert(expected && *expected);
            assert(!strcmp(x4_tr_stage(stage_map[i].es), expected));
            char input[160], output[160];
            snprintf(input, sizeof(input), "HTTPS: %s", stage_map[i].es);
            snprintf(output, sizeof(output), "HTTPS: %s", expected);
            assert(!strcmp(x4_tr_stage(input), output));
        }
        const char *stage = x4_tr_stage("Xbox está preparando el juego… 17 s");
        assert(strstr(stage, "17 s"));
        assert(strstr(x4_tr_stage("Xbox cerró la sesión: SessionNotActive"), "SessionNotActive"));
        const char *category = "Racing & flying";
        assert(strcmp(x4_lang_store_category(category), category));
    }
    puts("LANG50_TABLES_STAGES_CATEGORIES_PASS");
}
