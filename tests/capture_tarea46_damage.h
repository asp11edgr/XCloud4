/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* This checks the renderer on the same persistent canvas and assets as the
 * captures. None of these full redraws run inside the timing loops. */
static void assert_focus_frame46(uint32_t *pixels, uint32_t *incremental,
    bool expect_two, unsigned maximum_area, const char *step)
{
    const size_t bytes = pixel_count * sizeof(*pixels);
    memcpy(incremental, pixels, bytes);
    draw_tab(pixels);
    X4UiRect rects[5];
    const size_t count = x4_catalog_gui_home_damage(rects, NULL);
    assert(count <= 5);
    if (expect_two) assert(count == 2);
    unsigned area = 0;
    for (size_t i = 0; i < count; ++i) {
        const X4UiRect rect = rects[i];
        assert(rect.x >= 0 && rect.y >= 0 && rect.w > 0 && rect.h > 0);
        assert(rect.x + rect.w <= WIDTH && rect.y + rect.h <= HEIGHT);
        area += (unsigned)(rect.w * rect.h);
        for (int row = 0; row < rect.h; ++row) {
            const size_t offset = (size_t)(rect.y + row) * WIDTH + rect.x;
            memcpy(incremental + offset, pixels + offset, (size_t)rect.w * sizeof(*pixels));
        }
    }
    if (maximum_area) assert(area && area <= maximum_area);
    /* Reproduce the copy to the presentation canvas using damage only. */
    assert(memcmp(incremental, pixels, bytes) == 0);
    x4_catalog_gui_home_test_force_full();
    draw_tab(pixels);
    if(memcmp(incremental,pixels,pixel_count*sizeof(*pixels))){
        for(size_t i=0;i<pixel_count;++i)if(incremental[i]!=pixels[i]){
            fprintf(stderr,"T46 damage mismatch %s x=%zu y=%zu %08x != %08x\n",
                step,i%WIDTH,i/WIDTH,incremental[i],pixels[i]);
            break;
        }
        assert(0);
    }
    check_canvas(pixels);
}

static void assert_discover_focus_damage46(uint32_t *pixels)
{
    assert(browser.home_rows.count>=2&&assets.background_available);
    X4CatalogBrowser *saved=malloc(sizeof(*saved));
    uint32_t *incremental=malloc(pixel_count*sizeof(*pixels));
    assert(saved&&incremental);
    *saved=browser;

    for(unsigned background=0;background<2;++background){
        x4_catalog_gui_home_invalidate();
        assets.background_available=background!=0;
        browser=*saved;
        browser.home_first_row=0;
        browser.home_row=0;
        browser.hero_focused=true;
        browser.top_bar_focused=false;
        draw_tab(pixels);

        browser.hero_focused=false;
        assert_focus_frame46(pixels,incremental,true,110000,"hero-to-first-row");
        browser.home_row=1;
        assert_focus_frame46(pixels,incremental,true,145000,"first-to-second-row");
        browser.top_bar_focused=true;
        assert_focus_frame46(pixels,incremental,true,290000,"row-to-header");
        browser.top_bar_focused=false;
        assert_focus_frame46(pixels,incremental,true,290000,"header-to-row");

        const X4HomeRow *second=&browser.home_rows.rows[1];
        assert(second->count>1);
        browser.home_columns[second->kind]=(browser.home_columns[second->kind]+1u)%second->count;
        assert_focus_frame46(pixels,incremental,false,0,"horizontal-column-change");
        browser.hero_focused=true;
        assert_focus_frame46(pixels,incremental,true,110000,"row-to-hero");

        /* The active slot can arrive after a neutral first frame or change
         * while we retain its borrowed pixels. In both cases redraw in full. */
        assets.background_available=true;
        ++assets.backgrounds[assets.active].generation;
        ++assets.revision;
        draw_tab(pixels);
        assert(x4_catalog_gui_home_damage(NULL,NULL)==SIZE_MAX);
        browser.hero_focused=false;
        assert_focus_frame46(pixels,incremental,true,110000,"focus-after-background");
    }
    x4_catalog_gui_home_invalidate();
    assets.background_available=true;
    browser=*saved;
    free(incremental);
    free(saved);
    draw_tab(pixels);
    fputs("PASS T46 Discover: identical full/partial pixels with neutral and real background, focus-only dirty bounds\n",stderr);
}
