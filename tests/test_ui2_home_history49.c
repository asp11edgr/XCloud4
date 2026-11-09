/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/ui2_home.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static X4CatalogSnapshot catalog = {.state=X4_CATALOG_READY,.count=3};
    const char *const products[]={"FIRST","SECOND","THIRD"};
    const char *const titles[]={"title-a","title-b","title-c"};
    for (unsigned i=0; i<3; ++i) {
        strcpy(catalog.titles[i].product_id,products[i]);
        strcpy(catalog.titles[i].id,titles[i]);
    }
    static X4History history;
    X4RecentGames remote={.state=X4_RECENT_READY,.count=2};
    strcpy(remote.entries[0].id,titles[0]);
    strcpy(remote.entries[1].id,titles[1]);
    X4Ui2HomeModel model;
    x4_ui2_home_history(&history);
    x4_ui2_home_model(&model,true,&catalog,&remote,NULL);
    assert(model.continuation==0 && model.recent_count==1 && model.recent[0]==1);
    assert(x4_history_record(&history,"missing",104,0,NULL));
    assert(x4_history_record(&history,"third",102,60,NULL));
    assert(x4_history_record(&history,"second",101,30,NULL));
    x4_ui2_home_model(&model,true,&catalog,&remote,NULL);
    assert(model.continuation==2 && model.recent_count==2 && model.recent[0]==1 && model.recent[1]==0);
    X4CatalogTitle old=catalog.titles[0];
    catalog.titles[0]=catalog.titles[2];
    catalog.titles[2]=old;
    x4_ui2_home_catalog_changed();
    x4_ui2_home_model(&model,true,&catalog,&remote,NULL);
    assert(model.continuation==0 && model.recent_count==2 && model.recent[0]==1 && model.recent[1]==2);
    remote.state=X4_RECENT_ERROR;
    x4_ui2_home_model(&model,true,&catalog,&remote,NULL);
    assert(model.continuation==0 && model.mode==X4_UI2_HOME_WITH_RECENT && model.recent_count==1);
    assert(x4_history_record(&history,"second",105,1,NULL));
    x4_ui2_home_model(&model,true,&catalog,&remote,NULL);
    assert(model.continuation==1 && model.recent_count==1 && model.recent[0]==0);
    x4_ui2_home_model(&model,false,&catalog,&remote,NULL);
    assert(model.mode==X4_UI2_HOME_SIGNED_OUT && model.recent_count==0);
    puts("PASS T49 Inicio: historial por producto, orden local, refresco/reorden, respaldo de Recientes y deduplicacion");
    return 0;
}
