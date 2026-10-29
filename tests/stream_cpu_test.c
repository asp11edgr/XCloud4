/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual policy/UI/CPU functions; only native scheduler/attribute APIs doubled. */
#include "../src/video/stream_cpu.h"
#include "../src/ui/stream_settings.h"
#include <orbis/libkernel.h>
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
static unsigned self_calls,priority_calls,affinity_calls,query_calls,attr_init_calls,attr_get_calls,attr_affinity_calls,attr_destroy_calls;
static int priority_failure,affinity_failure,restore_priority_failure,restore_affinity_failure;
static int current_priority,last_priority;static uint64_t current_affinity,last_affinity;
static bool affinity_error_changes_value;
static unsigned fail_query_round,fail_query_step;static int query_failure;
static int injected(unsigned step){return query_calls==fail_query_round&&step==fail_query_step?query_failure:0;}
OrbisPthread scePthreadSelf(void){++self_calls;return (OrbisPthread)27;}
int32_t scePthreadGetprio(OrbisPthread self,int32_t *value)
{assert(self==(OrbisPthread)27);++query_calls;int rc=injected(1);if(!rc)*value=current_priority;return rc;}
int32_t scePthreadAttrInit(OrbisPthreadAttr *attr)
{++attr_init_calls;memset(attr,0,sizeof(*attr));return injected(2);}
int32_t scePthreadAttrGet(OrbisPthread self,OrbisPthreadAttr *attr)
{assert(self==(OrbisPthread)27&&attr);++attr_get_calls;return injected(3);}
int32_t scePthreadAttrGetaffinity(const OrbisPthreadAttr *attr,void *value)
{assert(attr&&value);++attr_affinity_calls;int rc=injected(4);if(!rc)*(uint64_t*)value=current_affinity;return rc;}
int32_t scePthreadAttrDestroy(OrbisPthreadAttr *attr)
{assert(attr);++attr_destroy_calls;return injected(5);}
int32_t scePthreadSetprio(OrbisPthread self,int value)
{assert(self==(OrbisPthread)27);++priority_calls;last_priority=value;
 int rc=priority_calls==1?priority_failure:restore_priority_failure;if(!rc)current_priority=value;return rc;}
int32_t scePthreadSetaffinity(OrbisPthread self,uint64_t value)
{assert(self==(OrbisPthread)27);++affinity_calls;last_affinity=value;
 int rc=affinity_calls==1?affinity_failure:restore_affinity_failure;
 if(!rc||(affinity_calls==1&&affinity_error_changes_value))current_affinity=value;
 return rc;}
static void reset(void)
{
    self_calls=priority_calls=affinity_calls=query_calls=attr_init_calls=attr_get_calls=attr_affinity_calls=attr_destroy_calls=0;
    priority_failure=affinity_failure=restore_priority_failure=restore_affinity_failure=0;
    fail_query_round=fail_query_step=0;query_failure=0;current_priority=612;current_affinity=0x2d;
    last_priority=0;last_affinity=0;
    affinity_error_changes_value=false;
}
static void write_check(const X4StreamCpuReport *report,const char *outcome)
{
    FILE *f=tmpfile();assert(f);x4_stream_cpu_write(f,report);assert(!fflush(f));rewind(f);
    char json[2048]={0};assert(fread(json,1,sizeof(json)-1,f)>0);assert(!fclose(f));
    assert(strstr(json,"\"nonfatal\":true")&&strstr(json,outcome));
    assert(strstr(json,"\"before\":")&&strstr(json,"\"after\":")&&strstr(json,"\"restore_priority_result\":"));
}
static void all_variants(void)
{
    /* Published f18bbeb already includes smoothing/sharpness/request mode.
     * Pin its complete x86_64 layout; this task does not change the struct. */
    _Static_assert(sizeof(X4StreamRequestConfig)==72&&offsetof(X4StreamRequestConfig,cpu_variant)==49&&
        offsetof(X4StreamRequestConfig,decoder_depth)==50&&offsetof(X4StreamRequestConfig,interaction_modes)==51&&
        offsetof(X4StreamRequestConfig,convert_mode)==52&&offsetof(X4StreamRequestConfig,sharpness)==53&&
        offsetof(X4StreamRequestConfig,resolution_request_mode)==54&&offsetof(X4StreamRequestConfig,h264_profile)==56&&
        offsetof(X4StreamRequestConfig,max_fs)==60&&offsetof(X4StreamRequestConfig,max_mbps)==64,
        "published x86_64 captured config layout and stable CPU/depth offsets");
    for(unsigned v=0;v<=X4_CPU_3;++v){
        X4StreamCpuConfig c=x4_stream_cpu_config(v);
        assert(c.variant==v&&c.decoder_priority==(v==0?700:v==3?256:480)&&c.decoder_affinity==(v>=2?15u:63u));
        for(unsigned role=0;role<3;++role)for(unsigned i=0;i<(role==X4_CPU_ROLE_CONVERTER?2u:1u);++i){
            reset();X4StreamCpuReport report;assert(!x4_stream_cpu_apply_current_thread(v,(X4StreamCpuRole)role,i,&report));
            bool applied=v>=2,priority=applied&&role!=X4_CPU_ROLE_DECODE;
            assert(self_calls==applied&&priority_calls==priority&&affinity_calls==applied);
            assert(report.priority_attempted==priority&&report.affinity_attempted==applied&&report.result==0&&!report.partial_applied);
            assert(query_calls==(applied?2u:0u));
            if(applied){
                assert(report.outcome==X4_CPU_APPLIED&&report.before.valid&&report.after.valid);
                assert(report.before.priority==612&&report.before.affinity==0x2d);
                if(priority)assert(last_priority==(role==X4_CPU_ROLE_ASSEMBLY?500:700));
                else assert(report.priority_result==X4_CPU_NOT_RUN&&current_priority==612);
                assert(last_affinity==(role==X4_CPU_ROLE_DECODE?15u:role==X4_CPU_ROLE_ASSEMBLY?32u:(16u<<i)));
                assert(attr_init_calls==2&&attr_get_calls==2&&attr_affinity_calls==2&&attr_destroy_calls==2);
                write_check(&report,"\"outcome\":\"applied\"");
            }else{assert(report.outcome==X4_CPU_UNCHANGED&&!report.before.attempted&&!report.after.attempted);
                assert(report.priority_result==X4_CPU_NOT_RUN&&report.affinity_result==X4_CPU_NOT_RUN);}
            printf("CPU-%u role=%u index=%u native_priority=%d native_mask=0x%llx setter_priority=%d prio_rc=%d mask=0x%llx affinity_rc=%d outcome=%s result=%d\n",
                v,role,i,c.decoder_priority,(unsigned long long)c.decoder_affinity,report.requested_priority,
                report.priority_result,(unsigned long long)report.requested_affinity,report.affinity_result,
                x4_stream_cpu_outcome_name(report.outcome),report.result);
        }
    }
}
static void partial_and_restore(void)
{
    X4StreamCpuReport report;
    for(unsigned variant=2;variant<=3;++variant){
        reset();priority_failure=-17;
        assert(x4_stream_cpu_apply_current_thread(variant,X4_CPU_ROLE_CONVERTER,0,&report)==-17);
        assert(priority_calls==2&&!affinity_calls&&report.restore_priority_attempted&&!report.restore_affinity_attempted);
        assert(!report.restore_priority_result&&report.outcome==X4_CPU_RESTORED&&report.partial_applied);
        assert(report.after.valid&&report.after.priority==612&&report.after.affinity==0x2d);
        reset();affinity_failure=-19;
        assert(x4_stream_cpu_apply_current_thread(variant,X4_CPU_ROLE_CONVERTER,1,&report)==-19);
        assert(priority_calls==2&&affinity_calls==2&&report.restore_priority_attempted&&report.restore_affinity_attempted);
        assert(!report.restore_priority_result&&!report.restore_affinity_result&&report.outcome==X4_CPU_RESTORED);
        assert(report.after.priority==612&&report.after.affinity==0x2d);write_check(&report,"\"outcome\":\"restored\"");
        reset();affinity_failure=-19;restore_priority_failure=-29;
        assert(x4_stream_cpu_apply_current_thread(variant,X4_CPU_ROLE_CONVERTER,1,&report)==-19);
        assert(report.restore_priority_result==-29&&!report.restore_affinity_result);
        assert(report.after.valid&&report.after.priority==700&&report.after.affinity==0x2d&&report.outcome==X4_CPU_PARTIAL);
        write_check(&report,"\"partial_applied\":true");
        reset();affinity_failure=-23;restore_affinity_failure=-33;
        assert(x4_stream_cpu_apply_current_thread(variant,X4_CPU_ROLE_DECODE,0,&report)==-23);
        assert(!priority_calls&&affinity_calls==2&&!report.priority_attempted&&!report.restore_priority_attempted);
        assert(report.restore_affinity_result==-33&&report.after.valid&&report.after.affinity==0x2d);
        /* A failed restore is not assumed successful: the getter independently
         * proves the unchanged original value in this particular mock. */
        assert(report.outcome==X4_CPU_RESTORED&&report.partial_applied);
        reset();affinity_failure=-23;restore_affinity_failure=-33;affinity_error_changes_value=true;
        assert(x4_stream_cpu_apply_current_thread(variant,X4_CPU_ROLE_CONVERTER,1,&report)==-23);
        assert(report.restore_affinity_result==-33&&report.after.valid&&report.after.priority==612);
        assert(report.after.affinity==0x20&&report.outcome==X4_CPU_PARTIAL&&report.partial_applied);
        /* Setter error and restore error can both leave changed state. The
         * report must retain the observed mask, never invent rollback. */
    }
    for(unsigned step=1;step<=5;++step){
        reset();fail_query_round=1;fail_query_step=step;query_failure=-100-(int)step;
        assert(x4_stream_cpu_apply_current_thread(2,X4_CPU_ROLE_CONVERTER,0,&report)==query_failure);
        assert(!priority_calls&&!affinity_calls&&report.outcome==X4_CPU_SKIPPED_CAPTURE&&report.partial_applied);
        assert(!report.before.valid&&!report.after.attempted&&current_priority==612&&current_affinity==0x2d);
        assert(attr_destroy_calls==(step==2?0u:1u));write_check(&report,"\"outcome\":\"skipped_capture\"");
    }
    reset();fail_query_round=2;fail_query_step=4;query_failure=-104;
    assert(x4_stream_cpu_apply_current_thread(2,X4_CPU_ROLE_CONVERTER,0,&report)==-104);
    assert(report.before.valid&&!report.after.valid&&report.outcome==X4_CPU_PARTIAL&&report.partial_applied);
    assert(current_priority==700&&current_affinity==0x10); /* final state unknown in report */
    reset();assert(x4_stream_cpu_apply_current_thread(4,X4_CPU_ROLE_CONVERTER,0,&report)==X4_CPU_INVALID);
    assert(x4_stream_cpu_apply_current_thread(2,X4_CPU_ROLE_CONVERTER,2,&report)==X4_CPU_INVALID);
    assert(x4_stream_cpu_apply_current_thread(2,(X4StreamCpuRole)99,0,&report)==X4_CPU_INVALID);
    assert(!self_calls&&!priority_calls&&!affinity_calls);assert(x4_stream_cpu_config(99).decoder_priority==700);
    puts("PASS CPU best-effort failures: real-before capture, skipped setters, observed restore, partial/unknown state and every native return exported");
}
static void selection(void)
{
    assert(x4_stream_settings_get().cpu_variant==X4_CPU_0);X4StreamSettingsUi ui={0};
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_DOWN,true)&&ui.row==1);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_DOWN,true)&&ui.row==2);
    for(unsigned cpu=1;cpu<=3;++cpu)assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true)&&ui.cpu==(X4StreamCpuVariant)cpu);
    assert(x4_stream_settings_get().cpu_variant==X4_CPU_0);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CANCEL,true));assert(x4_stream_settings_get().cpu_variant==X4_CPU_0);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));assert(ui.cpu==X4_CPU_0);
    ui.cpu=X4_CPU_3;ui.profile=X4_REQUEST_1080P_G8;assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    X4StreamRequestConfig captured=x4_stream_settings_get();assert(captured.cpu_variant==X4_CPU_3&&captured.profile==X4_REQUEST_1080P_G8);
    assert(x4_stream_settings_set_cpu(X4_REQUEST_720P_HQ,X4_COLOR_FORCE_601,X4_CPU_1));
    X4StreamRequestConfig next=x4_stream_request_capture(&captured);assert(next.cpu_variant==X4_CPU_3&&next.profile==X4_REQUEST_1080P_G8);
    next=x4_stream_settings_get();assert(next.cpu_variant==X4_CPU_1&&next.profile==X4_REQUEST_720P_HQ);
    assert(x4_stream_settings_set(X4_REQUEST_1080P_G,X4_COLOR_AUTO_709)&&x4_stream_settings_get().cpu_variant==X4_CPU_1);
    assert(!x4_stream_settings_set_cpu(X4_REQUEST_1080P_G,X4_COLOR_AUTO_709,(X4StreamCpuVariant)4));
    captured.cpu_variant=255;assert(x4_stream_request_capture(&captured).cpu_variant==X4_CPU_0);
    puts("PASS CPU-0/1/2/3 policy, draft cancel and immutable next-session selection");
}
int main(void){all_variants();partial_and_restore();selection();return 0;}
