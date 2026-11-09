/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/ui/catalog_presentation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned cases,checks;
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#define RUN(f) do{++cases;f();}while(0)
static X4CatalogPresentation presentation;
static X4CatalogSnapshot catalog;
static X4CatalogBrowser browser;
static X4RecentGames recent;
static X4ProductMetadata metadata[24];
static char demand[24][32];
static void setup(unsigned count)
{
    x4_catalog_presentation_init(&presentation);x4_catalog_presentation_set_generation(&presentation,1);
    memset(&catalog,0,sizeof(catalog));catalog.state=X4_CATALOG_READY;catalog.count=count;
    for(unsigned i=0;i<count;++i){snprintf(catalog.titles[i].id,96,"TITLE_%u",i);snprintf(catalog.titles[i].product_id,32,"PRODUCT%u",i);catalog.titles[i].entitled=i%2==0;}
    x4_catalog_browser_init(&browser);x4_catalog_browser_rebuild(&browser,&catalog);x4_recent_init(&recent);
    memset(metadata,0,sizeof(metadata));
}
static void metadata_for(unsigned slot,unsigned index,const char *name)
{
    memset(&metadata[slot],0,sizeof(metadata[slot]));snprintf(metadata[slot].product_id,32,"PRODUCT%u",index);
    snprintf(metadata[slot].name,128,"%s",name);
}
static void stale_generation_and_revision(void)
{
    setup(6);metadata_for(0,0,"\xc3\x81rbol");CHECK(x4_catalog_presentation_ingest(&presentation,1,1,metadata,1)==1);
    CHECK(x4_catalog_presentation_apply_names(&presentation,&catalog));CHECK(!strcmp(catalog.titles[0].name,"\xc3\x81rbol"));
    metadata_for(0,0,"Wrong stale result");x4_catalog_presentation_set_generation(&presentation,2);
    CHECK(x4_catalog_presentation_ingest(&presentation,1,2,metadata,1)==0);CHECK(presentation.stale_results==1);
    catalog.titles[0].name[0]=0;CHECK(x4_catalog_presentation_apply_names(&presentation,&catalog));
    CHECK(!strcmp(catalog.titles[0].name,"\xc3\x81rbol"));
    metadata_for(0,1,"Current name");CHECK(x4_catalog_presentation_ingest(&presentation,2,3,metadata,1)==1);
    metadata_for(0,1,"Wrong duplicate revision");CHECK(x4_catalog_presentation_ingest(&presentation,2,3,metadata,1)==0);
    CHECK(x4_catalog_presentation_ingest(&presentation,2,2,metadata,1)==0);
    CHECK(x4_catalog_presentation_apply_names(&presentation,&catalog));CHECK(!strcmp(catalog.titles[1].name,"Current name"));
    x4_catalog_presentation_set_generation(&presentation,0);CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==0);
}
static void full_catalog_search_and_selection(void)
{
    setup(4096);x4_catalog_browser_move(&browser,&catalog,4095,true);CHECK(!strcmp(browser.selected_id,"TITLE_4095"));
    metadata_for(0,4095,"Forza: m\xc3\xa1s all\xc3\xa1");CHECK(x4_catalog_presentation_ingest(&presentation,1,1,metadata,1)==1);
    int entitled=catalog.titles[4095].entitled;CHECK(x4_catalog_presentation_apply_names(&presentation,&catalog));
    CHECK(catalog.titles[4095].entitled==entitled);CHECK(!strcmp(catalog.titles[4095].id,"TITLE_4095"));
    x4_catalog_browser_rebuild(&browser,&catalog);CHECK(browser.selected==4095);CHECK(!strcmp(browser.selected_id,"TITLE_4095"));
    const char *query="FORZA";while(*query)CHECK(x4_catalog_browser_query_append(&browser,*query++));
    x4_catalog_browser_rebuild(&browser,&catalog);CHECK(browser.filtered_count==1);unsigned index;
    CHECK(x4_catalog_browser_selected_index(&browser,&catalog,&index));CHECK(index==4095);
    X4CatalogTitle t=catalog.titles[4095];catalog.titles[4095]=catalog.titles[0];catalog.titles[0]=t;
    catalog.titles[0].name[0]=0;x4_catalog_presentation_set_generation(&presentation,2);
    CHECK(x4_catalog_presentation_apply_names(&presentation,&catalog));x4_catalog_browser_rebuild(&browser,&catalog);
    CHECK(x4_catalog_browser_selected_index(&browser,&catalog,&index));CHECK(index==0);CHECK(!strcmp(browser.selected_id,"TITLE_4095"));
}
static void rejected_metadata_and_empty_catalog(void)
{
    setup(3);metadata_for(0,0,"Fine");strcpy(metadata[0].product_id,"../bad");
    metadata_for(1,1,"Broken\xc0\xaf");CHECK(x4_catalog_presentation_ingest(&presentation,1,1,metadata,2)==0);
    CHECK(presentation.rejected_metadata==2);CHECK(presentation.count==0);
    metadata_for(0,0,"Fine");CHECK(x4_catalog_presentation_ingest(&presentation,1,2,metadata,25)==0);
    CHECK(x4_catalog_presentation_ingest(&presentation,1,2,NULL,1)==0);
    CHECK(x4_catalog_presentation_ingest(&presentation,1,2,metadata,1)==1);
    catalog.state=X4_CATALOG_LOADING;CHECK(!x4_catalog_presentation_apply_names(&presentation,&catalog));
    CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==0);
    catalog.state=X4_CATALOG_READY;catalog.count=0;x4_catalog_browser_rebuild(&browser,&catalog);
    CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==0);
}
static void demand_priority_and_capacity(void)
{
    setup(48);char ids[25][96]={{0}};for(unsigned i=0;i<25;++i)snprintf(ids[i],96,"TITLE_%u",39-i);
    x4_recent_begin(&recent,1,1);CHECK(x4_recent_publish(&recent,1,1,ids,25,0,200,&catalog));x4_catalog_browser_sync_recents(&browser,&recent);
    browser.section=X4_CATALOG_SECTION_CATALOG;CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==24);
    CHECK(!strcmp(demand[0],"PRODUCT0"));CHECK(!strcmp(demand[5],"PRODUCT5"));
    CHECK(!strcmp(demand[6],"PRODUCT39"));CHECK(!strcmp(demand[11],"PRODUCT34"));
    CHECK(!strcmp(demand[12],"PRODUCT6"));CHECK(!strcmp(demand[17],"PRODUCT11"));CHECK(!strcmp(demand[23],"PRODUCT28"));
    browser.section=X4_CATALOG_SECTION_RECENT;CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==24);
    CHECK(!strcmp(demand[0],"PRODUCT39"));CHECK(!strcmp(demand[5],"PRODUCT34"));CHECK(!strcmp(demand[6],"PRODUCT0"));
    CHECK(!strcmp(demand[12],"PRODUCT33"));CHECK(!strcmp(demand[18],"PRODUCT6"));
    for(unsigned i=0;i<24;++i)for(unsigned j=0;j<i;++j)CHECK(strcmp(demand[i],demand[j])!=0);
    x4_recent_begin(&recent,2,2);CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==12);
}
static void selected_priority_and_unfocused_visible_recent_page(void)
{
    setup(48);char ids[25][96]={{0}};for(unsigned i=0;i<25;++i)snprintf(ids[i],96,"TITLE_%u",39-i);
    x4_recent_begin(&recent,1,1);CHECK(x4_recent_publish(&recent,1,1,ids,25,0,200,&catalog));
    x4_catalog_browser_sync_recents(&browser,&recent);
    CHECK(x4_catalog_browser_page_move(&browser,&catalog,&recent,1));
    CHECK(x4_catalog_browser_nav(&browser,&catalog,&recent,1,0));
    CHECK(x4_catalog_browser_nav(&browser,&catalog,&recent,1,0));CHECK(browser.recent_selected==8);
    CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==24);
    CHECK(!strcmp(demand[0],"PRODUCT31"));CHECK(!strcmp(demand[1],"PRODUCT33"));
    CHECK(!strcmp(demand[5],"PRODUCT28"));CHECK(!strcmp(demand[6],"PRODUCT0"));
    CHECK(!strcmp(demand[12],"PRODUCT27"));
    /* Recents is still displaying page 2 after focus moves to the grid. */
    browser.section=X4_CATALOG_SECTION_CATALOG;x4_catalog_browser_move(&browser,&catalog,2,false);
    CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==24);
    CHECK(!strcmp(demand[0],"PRODUCT2"));CHECK(!strcmp(demand[1],"PRODUCT0"));
    CHECK(!strcmp(demand[5],"PRODUCT5"));CHECK(!strcmp(demand[6],"PRODUCT33"));
    CHECK(!strcmp(demand[11],"PRODUCT28"));CHECK(!strcmp(demand[18],"PRODUCT27"));
    for(unsigned i=0;i<24;++i)CHECK(strcmp(demand[i],"PRODUCT39"));
    CHECK(browser.recent_selected==8);CHECK(!strcmp(browser.selected_id,"TITLE_2"));
}
static void rapid_navigation_replaces_priorities_without_mutating_selection(void)
{
    setup(72);
    for(unsigned page=0;page<10;++page){
        if(page)CHECK(x4_catalog_browser_page_move(&browser,&catalog,&recent,1));
        CHECK(x4_catalog_browser_grid_move(&browser,&catalog,1,0));
        CHECK(x4_catalog_browser_grid_move(&browser,&catalog,0,1));
        unsigned selected;CHECK(x4_catalog_browser_selected_index(&browser,&catalog,&selected));
        char anchor[96];memcpy(anchor,browser.selected_id,sizeof(anchor));
        CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==12);
        CHECK(!strcmp(demand[0],catalog.titles[selected].product_id));
        CHECK(!strcmp(anchor,browser.selected_id));
        unsigned start=x4_catalog_browser_page_start(&browser);
        for(unsigned i=start;i<start+6;++i){
            bool visible=false;for(unsigned j=0;j<6;++j)if(!strcmp(demand[j],catalog.titles[i].product_id))visible=true;
            CHECK(visible);
        }
        /* Every call replaces the bounded demand, rather than appending old
         * page work; the next page has the selected-column anchor intact. */
        CHECK(x4_catalog_browser_grid_move(&browser,&catalog,-1,0));
        CHECK(x4_catalog_browser_grid_move(&browser,&catalog,0,-1));
    }
}
static void invalid_selection_never_overrides_visible_priority(void)
{
    setup(24);x4_catalog_browser_move(&browser,&catalog,4,false);
    strcpy(browser.selected_id,"STALE_ID");
    CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==12);
    CHECK(!strcmp(demand[0],"PRODUCT0"));
    browser.criteria_dirty=true;CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==0);
}
static void empty_recent_and_invalid_page_bounds(void)
{
    setup(48);x4_recent_begin(&recent,1,1);
    CHECK(x4_recent_publish(&recent,1,1,NULL,0,0,200,&catalog));x4_catalog_browser_sync_recents(&browser,&recent);
    CHECK(x4_recent_count(&recent)==0);browser.section=X4_CATALOG_SECTION_RECENT;
    CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==12);
    CHECK(!strcmp(demand[0],"PRODUCT0"));CHECK(!strcmp(demand[11],"PRODUCT11"));
    browser.section=X4_CATALOG_SECTION_CATALOG;x4_catalog_browser_move(&browser,&catalog,5,false);
    CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==12);
    CHECK(!strcmp(demand[0],"PRODUCT5"));CHECK(!strcmp(demand[1],"PRODUCT0"));
    /* A corrupt page anchor cannot wrap an unsigned adjacent offset back
     * into an unrelated valid page. Normal browser APIs clamp these. */
    browser.selected=UINT32_MAX;browser.recent_selected=UINT32_MAX;
    CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==0);
    setup(48);char ids[1][96]={"TITLE_39"};x4_recent_begin(&recent,2,2);
    CHECK(x4_recent_publish(&recent,2,2,ids,1,0,200,&catalog));
    browser.recent_account_generation=1;browser.section=X4_CATALOG_SECTION_RECENT;
    CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==12);
    for(unsigned i=0;i<12;++i)CHECK(strcmp(demand[i],"PRODUCT39"));
}
static void final_page_and_product_dedup(void)
{
    setup(15);x4_catalog_browser_page_move(&browser,&catalog,&recent,1);x4_catalog_browser_page_move(&browser,&catalog,&recent,1);
    CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==9);
    CHECK(!strcmp(demand[0],"PRODUCT12"));CHECK(!strcmp(demand[2],"PRODUCT14"));CHECK(!strcmp(demand[3],"PRODUCT6"));
    strcpy(catalog.titles[13].product_id,"product12");CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==8);
    strcpy(catalog.titles[14].product_id,"not/a/product");CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==7);
    browser.criteria_dirty=true;CHECK(x4_catalog_presentation_demand(&presentation,&browser,&catalog,&recent,demand)==0);
}
static void assert_row_only(const uint32_t *masks, size_t count)
{
    for (size_t i = 0; i < count; ++i) CHECK(masks[i] == (1u << X4_COVER_ROW));
}

static void tab_surface_masks_and_background(void)
{
    setup(24);
    browser.apple_mode = true;
    browser.active_tab = X4_CAT_TAB_CATALOG;
    browser.selected = 11;
    uint32_t masks[X4_PRESENTATION_DEMAND_MAX];
    size_t count = x4_catalog_presentation_demand_surfaces(&presentation, &browser,
        &catalog, &recent, demand, masks);
    CHECK(count == 16);
    CHECK(!strcmp(demand[0], "PRODUCT11"));
    CHECK(!strcmp(demand[1], "PRODUCT8"));
    CHECK(!strcmp(demand[8], "PRODUCT16"));
    assert_row_only(masks, count);
    browser.criteria_dirty = true;
    CHECK(x4_catalog_presentation_demand_surfaces(&presentation, &browser,
        &catalog, &recent, demand, masks) == 0);
    browser.criteria_dirty = false;

    char titles[][96] = {"TITLE_9", "TITLE_7", "HISTORICAL"};
    x4_recent_begin(&recent, 1, 1);
    CHECK(x4_recent_publish(&recent, 1, 1, titles, 3, 0, 200, &catalog));
    strcpy(recent.entries[2].product_id, "HISTPRODUCT");
    x4_catalog_browser_sync_recents(&browser, &recent);
    browser.active_tab = X4_CAT_TAB_RECENTS;
    browser.section = X4_CATALOG_SECTION_RECENT;
    count = x4_catalog_presentation_demand_surfaces(&presentation, &browser,
        &catalog, &recent, demand, masks);
    CHECK(count == 3);
    CHECK(!strcmp(demand[0], "PRODUCT9"));
    CHECK(!strcmp(demand[1], "PRODUCT7"));
    CHECK(!strcmp(demand[2], "HISTPRODUCT"));
    assert_row_only(masks, count);
    CHECK(!x4_catalog_browser_tab_background(&browser, &catalog, &recent, 1000));
    CHECK(!x4_catalog_browser_tab_background(&browser, &catalog, &recent, 300999));
    const char *background = x4_catalog_browser_tab_background(&browser, &catalog, &recent, 301000);
    CHECK(background && !strcmp(background, demand[0]));
    CHECK(x4_catalog_presentation_demand_surfaces(&presentation, &browser,
        &catalog, &recent, demand, masks) == 3);
    assert_row_only(masks, 3);

    catalog.state = X4_CATALOG_ERROR;
    CHECK(x4_catalog_presentation_demand_surfaces(&presentation, &browser,
        &catalog, &recent, demand, masks) == 3);
    assert_row_only(masks, 3);
    ++browser.recent_account_generation;
    CHECK(x4_catalog_presentation_demand_surfaces(&presentation, &browser,
        &catalog, &recent, demand, masks) == 0);
    CHECK(!x4_catalog_browser_tab_background(&browser, &catalog, &recent, 301001));
    CHECK(x4_catalog_presentation_demand_surfaces(&presentation, NULL,
        &catalog, &recent, demand, masks) == 0);
    for (size_t i = 0; i < X4_PRESENTATION_DEMAND_MAX; ++i)
        CHECK(!demand[i][0] && !masks[i]);
}
static void discover_five_visible_surfaces(void)
{
    setup(14);
    browser.apple_mode=true;
    browser.active_tab=X4_CAT_TAB_DISCOVER;
    X4HomeRows rows={.count=2,.rows={
        {.kind=X4_HOME_CONTINUE,.count=7,.indices={0,1,2,3,4,5,6}},
        {.kind=X4_HOME_MINE,.count=7,.indices={7,8,9,10,11,12,13}},
    }};
    x4_catalog_browser_home_apply(&browser,&catalog,&rows);
    uint32_t masks[X4_PRESENTATION_DEMAND_MAX];
    size_t count=x4_catalog_presentation_demand_surfaces(&presentation,&browser,
        &catalog,&recent,demand,masks);
    CHECK(count==12&&X4_CATALOG_DISCOVER_ROW_VISIBLE==5);
    CHECK(!strcmp(demand[4],"PRODUCT4")&&masks[4]==(1u<<X4_COVER_ROW));
    CHECK(!strcmp(demand[9],"PRODUCT11")&&masks[9]==(1u<<X4_COVER_ROW));
    CHECK(!strcmp(demand[10],"PRODUCT5")&&!strcmp(demand[11],"PRODUCT12"));
    CHECK(x4_catalog_browser_apple_nav(&browser,&catalog,&recent,0,1,0));
    for(unsigned i=0;i<5;++i)
        CHECK(x4_catalog_browser_apple_nav(&browser,&catalog,&recent,1,0,0));
    CHECK(x4_catalog_browser_home_row_start(&browser,0)==1);
    count=x4_catalog_presentation_demand_surfaces(&presentation,&browser,
        &catalog,&recent,demand,masks);
    CHECK(count==13&&!strcmp(demand[0],"PRODUCT5")&&!strcmp(demand[1],"PRODUCT1"));
    CHECK(!strcmp(demand[10],"PRODUCT0")&&!strcmp(demand[11],"PRODUCT6"));
}

static void bounded_overlay_and_refresh_eviction(void)
{
    setup(4096);uint64_t revision=1;
    for(unsigned start=0;start<4096;start+=24){unsigned count=4096-start;if(count>24)count=24;
        for(unsigned i=0;i<count;++i)metadata_for(i,start+i,"Known public name");
        CHECK(x4_catalog_presentation_ingest(&presentation,1,revision++,metadata,count)==count);
    }
    CHECK(presentation.count==4096);CHECK(x4_catalog_presentation_apply_names(&presentation,&catalog));
    metadata_for(0,9999,"New product");CHECK(x4_catalog_presentation_ingest(&presentation,1,revision++,metadata,1)==0);
    CHECK(presentation.capacity_rejected==1);CHECK(presentation.count==4096);
    x4_catalog_presentation_set_generation(&presentation,2);CHECK(!x4_catalog_presentation_apply_names(&presentation,&catalog));
    CHECK(x4_catalog_presentation_ingest(&presentation,2,1,metadata,1)==0);CHECK(presentation.capacity_rejected==2);
    x4_catalog_presentation_set_generation(&presentation,3);CHECK(x4_catalog_presentation_ingest(&presentation,3,1,metadata,1)==1);
    CHECK(presentation.count==4096);strcpy(catalog.titles[0].product_id,"PRODUCT9999");catalog.titles[0].name[0]=0;
    CHECK(x4_catalog_presentation_apply_names(&presentation,&catalog));CHECK(!strcmp(catalog.titles[0].name,"New product"));
    CHECK(sizeof(presentation)<=704u*1024);
}
int main(void)
{
    RUN(stale_generation_and_revision);RUN(full_catalog_search_and_selection);RUN(rejected_metadata_and_empty_catalog);
    RUN(demand_priority_and_capacity);RUN(selected_priority_and_unfocused_visible_recent_page);
    RUN(rapid_navigation_replaces_priorities_without_mutating_selection);RUN(invalid_selection_never_overrides_visible_priority);
    RUN(empty_recent_and_invalid_page_bounds);
    RUN(final_page_and_product_dedup);RUN(tab_surface_masks_and_background);
    RUN(discover_five_visible_surfaces);RUN(bounded_overlay_and_refresh_eviction);
    printf("PASS catalog presentation: %u cases, %u checks; overlay=%zu bytes\n",cases,checks,sizeof(presentation));return 0;
}
