/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/streaming/recovery43_sdp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char before[]="v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 102\r\na=mid:video\r\na=rtpmap:102 H264/90000\r\na=rtcp-fb:102 nack\r\na=rtcp-fb:102 nack pli\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n";
static const char base_video[]="a=mid:video\r\na=rtpmap:102 H264/90000\r\na=rtcp-fb:102 nack\r\na=rtcp-fb:102 nack pli\r\n";
static void offers(void)
{
    char text[2048],expected[2048];X4Recovery43Offer p;
    strcpy(text,before);assert(!x4_recovery43_sdp_offer(text,sizeof(text),0,&p));assert(!memcmp(text,before,sizeof(before)));
    assert(!x4_recovery43_sdp_offer(NULL,0,0,NULL));
    for(unsigned mode=1;mode<=2;++mode){
        strcpy(text,before);assert(!x4_recovery43_sdp_offer(text,sizeof(text),mode,&p));
        snprintf(expected,sizeof(expected),"v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 102 103%s\r\n%s"
            "a=rtpmap:103 rtx/90000\r\na=fmtp:103 apt=102\r\n%s"
            "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n",
            mode==2?" 49":"",base_video,mode==2?"a=rtpmap:49 flexfec-03/90000\r\na=fmtp:49 repair-window=10000000\r\n":"");
        assert(!strcmp(text,expected));assert(p.media_pt==102&&p.rtx_pt==103&&p.fec_pt==(mode==2?49:0));
        assert(!strstr(text,"transport-cc"));
        printf("SDP_DIFF mode=%u m=video +103%s; +rtpmap103 rtx/90000; +fmtp103 apt=102%s\n",mode,
            mode==2?" +49":"",mode==2?"; +rtpmap49 flexfec-03/90000; +fmtp49 repair-window=10000000":"");
    }
    strcpy(text,"m=video 9 UDP/TLS/RTP/SAVPF 102 103\na=rtpmap:102 H264/90000\na=rtpmap:103 VP8/90000\n");
    assert(!x4_recovery43_sdp_offer(text,sizeof(text),1,&p)&&p.rtx_pt==96);
    assert(strstr(text,"a=rtpmap:103 VP8/90000\n")&&strstr(text,"a=fmtp:96 apt=102\n"));
    strcpy(text,before);assert(x4_recovery43_sdp_offer(text,sizeof(before),2,&p)<0&&!memcmp(text,before,sizeof(before)));
    strcpy(text,"m=video 9 UDP/TLS/RTP/SAVPF 102\na=rtpmap:102 H264/90000");
    assert(!x4_recovery43_sdp_offer(text,sizeof(text),1,&p));assert(strstr(text,"H264/90000\na=rtpmap:103"));
}
static void answers(void)
{
    const char *accepted="m=video 9 UDP/TLS/RTP/SAVPF 102 105 52\r\na=rtpmap:102 H264/90000\r\na=rtpmap:105 rtx/90000\r\na=fmtp:105 apt=102\r\na=rtpmap:52 flexfec-03/90000\r\na=ssrc-group:FID 100 200\r\na=ssrc-group:FEC-FR 100 300\r\na=ssrc:100 cname:test\r\na=ssrc:200 cname:test\r\na=ssrc:300 cname:test\r\n";
    X4Recovery43Negotiation a;assert(!x4_recovery43_sdp_answer(accepted,strlen(accepted),2,&a));
    assert(a.video_found&&a.rtx_accepted&&a.fec_accepted&&a.media_ssrc==100&&a.rtx_ssrc==200&&a.fec_ssrc==300&&a.rtx_pt==105&&a.fec_pt==52);
    assert(!x4_recovery43_sdp_answer(accepted,strlen(accepted),1,&a)&&a.rtx_accepted&&!a.fec_accepted);
    assert(!x4_recovery43_sdp_answer(accepted,strlen(accepted),0,&a)&&!a.rtx_accepted&&!a.fec_accepted&&a.media_ssrc==100);
    char text[4096];const char *attributes=strstr(accepted,"\r\n");assert(attributes);
    snprintf(text,sizeof(text),"m=video 9 UDP/TLS/RTP/SAVPF 102%s",attributes);
    assert(!x4_recovery43_sdp_answer(text,strlen(text),2,&a)&&a.video_found&&!a.rtx_accepted&&!a.fec_accepted);
    snprintf(text,sizeof(text),"m=video 9 UDP/TLS/RTP/SAVPF 102 105%s",attributes);
    assert(!x4_recovery43_sdp_answer(text,strlen(text),2,&a)&&a.rtx_accepted&&!a.fec_accepted);
    snprintf(text,sizeof(text),"m=video 9 UDP/TLS/RTP/SAVPF 105 52%s",attributes);
    assert(!x4_recovery43_sdp_answer(text,strlen(text),2,&a)&&!a.media_ssrc&&!a.rtx_accepted&&!a.fec_accepted);
    const char *plain="m=video 9 UDP/TLS/RTP/SAVPF 102\na=rtpmap:102 H264/90000\na=ssrc:100 cname:test\n";
    assert(!x4_recovery43_sdp_answer(plain,strlen(plain),2,&a)&&a.video_found&&!a.rtx_accepted&&!a.fec_accepted&&a.media_ssrc==100);
    strcpy(text,accepted);strcat(text,"a=extmap:3 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n");
    assert(!x4_recovery43_sdp_answer(text,strlen(text),2,&a)&&a.mutable_extensions&&a.rtx_accepted&&!a.fec_accepted);
    strcpy(text,accepted);char *end=strstr(text,"a=ssrc:300");assert(end);*end=0;
    assert(!x4_recovery43_sdp_answer(text,strlen(text),2,&a)&&a.rtx_accepted&&!a.fec_accepted);
    strcpy(text,accepted);strcat(text,"a=fmtp:105 apt=111\r\n");
    assert(!x4_recovery43_sdp_answer(text,strlen(text),2,&a)&&!a.rtx_accepted&&!a.fec_accepted);
    strcpy(text,accepted);strcat(text,"a=ssrc-group:FID 100 400\r\n");
    assert(!x4_recovery43_sdp_answer(text,strlen(text),2,&a)&&!a.rtx_accepted);
}
static void edge_reference(const char *path)
{
    FILE *f=fopen(path,"rb");assert(f);char text[4096];size_t n=fread(text,1,sizeof(text)-1,f);assert(!ferror(f)&&feof(f)&&!fclose(f));text[n]=0;
    X4Recovery43Negotiation a;assert(!x4_recovery43_sdp_answer(text,n,2,&a));
    assert(a.rtx_accepted&&a.fec_accepted&&a.rtx_pt==103&&a.fec_pt==49&&a.media_ssrc==100&&a.rtx_ssrc==200&&a.fec_ssrc==300);
    puts("PASS reference Edge negotiation, synthetic SSRC declarations and unchanged codec mappings");
}
int main(int argc,char **argv)
{assert(argc==2);offers();answers();edge_reference(argv[1]);puts("PASS recovery43 SDP: T42 no-op bytes, exact additions, PT collision, accepted/declined/alternate mappings, bounds");return 0;}
