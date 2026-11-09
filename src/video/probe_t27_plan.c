/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t27_plan.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26 && X4_PROBE_T27
#include <string.h>
bool x4_t27_plan_cell(unsigned index,X4T27Cell *out)
{
 if(!out||index>=X4_T27_CELL_COUNT)return false;
 X4T27Cell c={.index=index,.reference=index<X4_T27_BASE_COUNT};
 x4_t26_config_base(&c.config);
 if(c.reference)c.clip=index?X4_T26_CLIP_1080_20:X4_T26_CLIP_1080_15;
 else{
  unsigned n=index-X4_T27_BASE_COUNT;
  c.clip=n<6u?(n<3u?X4_T26_CLIP_1080_15:X4_T26_CLIP_1080_20):X4_T26_CLIP_1080_15;
  c.config.realistic=true;c.presentation.source_hz=n<6u?60u:30u;
  c.presentation.vsync_policy=n%3u?1u:2u;c.presentation.mailbox=n%3u==2u;
 }
 *out=c;return true;
}
uint64_t x4_t27_plan_remaining_us(unsigned next)
{
 uint64_t us=0;for(unsigned i=next;i<X4_T27_CELL_COUNT;++i){X4T27Cell c;
  if(!x4_t27_plan_cell(i,&c))break;us+=c.reference?UINT64_C(13000000):UINT64_C(780000000)/c.presentation.source_hz;}
 return us;
}
const char *x4_t27_plan_mode(const X4T27Cell *c)
{ return !c?"no disponible":c->reference?"Base · Solo Decode · máxima velocidad":
 c->presentation.vsync_policy==2u?"VSync Nunca · Cola":c->presentation.mailbox?"VSync Siempre · Mailbox":"VSync Siempre · Cola"; }
#endif
