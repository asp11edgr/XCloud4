/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/ui_draw_metrics.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    x4_ui_draw_metrics_reset();X4UiDrawMetricView v;
    assert(x4_ui_draw_metrics_snapshot(X4_UI_MENU,false,&v)&&v.draw.n==0);
    for(unsigned i=1;i<=50;++i)x4_ui_draw_metrics_record(X4_UI_MENU,false,true,
        (uint64_t)i*1000,300,17000,8294400,1000000+i*20000,0);
    assert(x4_ui_draw_metrics_snapshot(X4_UI_MENU,false,&v));
    assert(v.draw.n==50&&v.draw.p50_us==25100&&v.draw.p95_us==48100&&v.draw.max_us==50000);
    assert(!v.draw.p50_overflow&&!v.draw.p95_overflow&&v.draw.overflow==0);
    assert(v.copy.p50_us==400&&v.present.p95_us==17100);
    x4_ui_draw_metrics_record(X4_UI_MENU,false,false,0,200,16000,8294400,2100000,12);
    assert(x4_ui_draw_metrics_snapshot(X4_UI_MENU,false,&v));
    assert(v.draw.n==50&&v.presentations==51&&v.copy_only==1);
    FILE *f=tmpfile();assert(f);x4_ui_draw_metrics_write(f,X4_UI_MENU,false,&v,12);
    assert(!fseek(f,0,SEEK_SET));char line[2048];assert(fgets(line,sizeof(line),f));assert(!fclose(f));
    assert(strstr(line,"XCloud4 UI draw: screen=menu phase=full"));
    assert(strstr(line,"draw_p50_us=25100")&&strstr(line,"draw_p95_us=48100")&&strstr(line,"draw_max_us=50000"));
    assert(strstr(line,"copy_p50_us=400")&&strstr(line,"present_p95_us=17100")&&strstr(line,"bucket_us=100"));
    for(unsigned i=0;i<3;++i)x4_ui_draw_metrics_record(X4_UI_OPTIONS_PICTURE,true,true,
        300000,1000,17000,100,2200000+i*20000,12);
    assert(x4_ui_draw_metrics_snapshot(X4_UI_OPTIONS_PICTURE,true,&v));
    assert(v.draw.n==3&&v.draw.max_us==300000&&v.draw.p50_overflow&&v.draw.p95_overflow&&v.draw.overflow==3);
    assert(v.draw.p50_us==102300&&v.draw.p50_us!=v.draw.max_us);
    assert(!x4_ui_draw_metrics_snapshot((enum X4UiScreen)-1,false,&v));
    x4_ui_draw_metrics_flush(9000000);
    puts("PASS TAREA20c UI metrics: 100us bins distinguish 1..50ms, overflow explicit, redraws separate from copy-only and flip wait");
    return 0;
}
