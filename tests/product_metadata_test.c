/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/product_metadata.h"
#include "../src/auth/http_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_surface_image_priorities(void)
{
    static const char *const purposes[]={"Poster","BoxArt","SuperHeroArt","TitledHeroArt"};
    static const char *const expected_query[]={
        [X4_COVER_GRID]="?w=356&h=200&q=85&format=jpg",
        [X4_COVER_DETAIL]="?w=524&h=295&q=85&format=jpg",
        [X4_COVER_RECENT]="?w=96&h=96&q=85&format=jpg",
        [X4_COVER_ROW]="?w=384&h=216&q=85&format=jpg",
        [X4_COVER_HOME_FEATURE]="?w=852&h=480&q=85&format=jpg",
        [X4_COVER_HOME_FORTNITE]="?w=796&h=448&q=85&format=jpg",
        [X4_COVER_FORTNITE_POSTER]="?w=550&h=825&q=85&format=jpg",
        [X4_COVER_BACKGROUND]="?w=1920&h=1080&q=85&format=jpg"
    };
    char fixture[1400];
    const char start[]="{\"Products\":[{\"ProductId\":\"PLAY\",\"LocalizedProperties\":[{\"Images\":[";
    const char end[]= "] } ] } ] }";
    memcpy(fixture,start,sizeof(start));
    size_t length=sizeof(start)-1;
    for(unsigned i=0;i<4;++i){
        int n=snprintf(fixture+length,sizeof(fixture)-length,
            "%s{\"ImagePurpose\":\"%s\",\"Uri\":\"//store-images.s-microsoft.com/image/apps.%s\",\"Width\":800,\"Height\":450}",
            i?",":"",purposes[i],purposes[i]);
        assert(n>0&&(size_t)n+sizeof(end)<sizeof(fixture)-length);
        length+=(size_t)n;
        memcpy(fixture+length,end,sizeof(end));
        X4ProductMetadata metadata;
        assert(!x4_product_metadata_parse(fixture,length+sizeof(end)-1,"PLAY",&metadata));
        for(unsigned surface=0;surface<=X4_COVER_BACKGROUND;++surface){
            const char *purpose=purposes[i];
            if(surface==X4_COVER_RECENT)purpose=NULL;
            if(surface==X4_COVER_BACKGROUND || surface==X4_COVER_HOME_FORTNITE)
                purpose=i>=2?"SuperHeroArt":NULL;
            if(surface==X4_COVER_HOME_FEATURE && i>=2)purpose="SuperHeroArt";
            if(surface==X4_COVER_FORTNITE_POSTER)purpose="Poster";
            char url[X4_PUBLIC_COVER_URL_CAP],expected[X4_PUBLIC_COVER_URL_CAP];
            int available=x4_product_surface_url(&metadata,surface,url);
            assert(!!available==!!purpose);
            if(purpose){
                n=snprintf(expected,sizeof(expected),"https://store-images.s-microsoft.com/image/apps.%s%s",
                    purpose,expected_query[surface]);
                assert(n>0&&(size_t)n<sizeof(expected));
                assert(x4_http_public_image_url(url)&&!strcmp(url,expected));
            }
        }
    }
}
static void test_fortnite_fallbacks(void)
{
    static const char fixture[] =
        "{\"Products\":[{\"ProductId\":\"PLAY\",\"LocalizedProperties\":[{\"Images\":["
        "{\"ImagePurpose\":\"BoxArt\",\"Uri\":\"//store-images.s-microsoft.com/image/apps.box\","
        "\"Width\":770,\"Height\":770},"
        "{\"ImagePurpose\":\"TitledHeroArt\",\"Uri\":\"//store-images.s-microsoft.com/image/apps.titled\","
        "\"Width\":1600,\"Height\":900}]}]}]}";
    static const char no_hero[] =
        "{\"Products\":[{\"ProductId\":\"PLAY\",\"LocalizedProperties\":[{\"Images\":["
        "{\"ImagePurpose\":\"BoxArt\",\"Uri\":\"//store-images.s-microsoft.com/image/apps.box\","
        "\"Width\":770,\"Height\":770},"
        "{\"ImagePurpose\":\"Poster\",\"Uri\":\"//store-images.s-microsoft.com/image/apps.poster\","
        "\"Width\":550,\"Height\":825}]}]}]}";
    X4ProductMetadata metadata;
    char url[X4_PUBLIC_COVER_URL_CAP];
    assert(!x4_product_metadata_parse(fixture, sizeof(fixture) - 1, "PLAY", &metadata));
    assert(x4_product_surface_url(&metadata, X4_COVER_HOME_FORTNITE, url));
    assert(!strcmp(url, "https://store-images.s-microsoft.com/image/apps.titled?w=796&h=448&q=85&format=jpg"));
    assert(x4_product_surface_url(&metadata, X4_COVER_HOME_FEATURE, url));
    assert(!strcmp(url, "https://store-images.s-microsoft.com/image/apps.titled?w=852&h=480&q=85&format=jpg"));
    assert(x4_product_surface_url(&metadata, X4_COVER_FORTNITE_POSTER, url));
    assert(!strcmp(url, "https://store-images.s-microsoft.com/image/apps.box?w=770&h=770&q=85&format=jpg"));
    assert(x4_http_public_image_url(url));

    /* A cached T35 BoxArt source can only turn into the new square URL. */
    strcpy(metadata.recent_url, "https://store-images.s-microsoft.com/image/apps.old?w=96&h=96&q=85&format=jpg");
    assert(x4_product_surface_url(&metadata, X4_COVER_FORTNITE_POSTER, url));
    assert(!strcmp(url, "https://store-images.s-microsoft.com/image/apps.old?w=770&h=770&q=85&format=jpg"));

    assert(!x4_product_metadata_parse(no_hero, sizeof(no_hero) - 1, "PLAY", &metadata));
    assert(!x4_product_surface_url(&metadata, X4_COVER_HOME_FORTNITE, url));
    assert(x4_product_surface_url(&metadata, X4_COVER_HOME_FEATURE, url));
    assert(!strcmp(url, "https://store-images.s-microsoft.com/image/apps.box?w=852&h=480&q=85&format=jpg"));
    assert(x4_product_surface_url(&metadata, X4_COVER_FORTNITE_POSTER, url));
    assert(!strcmp(url, "https://store-images.s-microsoft.com/image/apps.poster?w=550&h=825&q=85&format=jpg"));
}

int main(void)
{
    test_surface_image_priorities();
    test_fortnite_fallbacks();
    assert(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?w=384&h=216&q=80&format=jpg"));
    assert(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?w=385&h=216&q=85&format=jpg"));
    assert(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?h=216&w=384&q=85&format=jpg"));
    assert(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?w=384&h=216&q=85&format=png"));
    assert(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?w=852&h=479&q=85&format=jpg"));
    assert(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?w=852&h=448&q=85&format=jpg"));
    assert(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?w=420&h=448&q=85&format=jpg"));
    assert(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?w=550&h=770&q=85&format=jpg"));
    puts("product metadata PASS: ranked Store images, exact menu/Fortnite URLs, cache compatibility and closed query grammar");
}
