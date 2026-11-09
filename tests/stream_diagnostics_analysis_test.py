#!/usr/bin/env python3
"""Read JSON produced by the real C diagnostic exporter; no console claims."""
import json, sys
from pathlib import Path
d=json.loads(Path(sys.argv[1]).read_text())
assert len(d['stages'])==18
for name in ('decoder_slot_acquisition_wait','decoder_ring_gate_wait'):
 assert d['stages'][name]['count']==0 and d['stages'][name]['p50_upper_us'] is None
s=d['stages']['decode']
assert s['count']==100 and s['p50_upper_us']==6000 and s['p95_upper_us']==10500 and s['p99_upper_us']==10900
assert d['stages']['decode_IDR']['count']==d['stages']['decode_non_IDR']['count']==50
assert d['stages']['decode_IDR']['sum_us']+d['stages']['decode_non_IDR']['sum_us']==s['sum_us']
assert d['stages']['conversion']['overflow_count']==1 and d['stages']['conversion']['p50_upper_us'] is None
assert d['stages']['stripe0']['count']==0 and d['stages']['stripe0']['p50_upper_us'] is None
assert d['intervals_over_100ms']==1 and d['longest_interval_us']==100001
b=d['received_video_rtp']
assert b['packets']==4001 and b['bytes']==400007 and b['complete_seconds']==2
assert b['mean_bps']==1600000 and b['p95_bps']==3200000
assert b['omitted_packets']==1 and b['omitted_bytes']==7
assert d['presentation_interval_us']==200001
assert abs(d['images_per_second']-2e6/200001)<1e-6
print('PASS real JSON percentiles, overflow/null, strict pauses, bitrate coverage and denominator')
new=json.loads(Path(sys.argv[1]+'.new').read_text())
c=new['cadence']['received_complete_AU']
assert c['counts_by_complete_second']==[30,60,40,0]
assert (c['near_30_seconds'],c['near_60_seconds'],c['other_seconds'],c['zero_seconds'])==(1,1,1,1)
assert c['total']==131 and c['trailing_partial_count']==1 and c['trailing_partial_duration_us']==500000
assert new['received_idr']['counts_by_complete_second']==[0,0,2,0]
assert new['received_idr']['total']==2 and new['idr_per_minute']['count']==2
assert new['idr_per_minute']['rate']==30.0 and new['idr_per_minute']['complete_minute_counts']==[]
k=new['keyframe_to_idr']
assert k['requests']==135 and k['n']==3 and k['p50_upper_us']==1701000 and k['maximum_us']==2000000
assert k['requests_ambiguous_publication']==1
assert k['requests_pending']==128 and k['requests_omitted']==3 and k['oldest_pending_age_us']==1500000
p=new['pause_evidence'];assert p['counts']==dict(unknown=1,rtp_gap=1,waiting_idr=1,decoder=1,presentation=1,mixed=1)
assert p['omitted_records']==0 and len(p['records'])==6
assert p['terminal_open_interval']['over_100ms'] is True
retained=json.loads(Path(sys.argv[1]+'.retention').read_text())['pause_evidence']
assert retained['record_count']==259 and retained['omitted_records']==3 and len(retained['records'])==256
assert retained['records'][0]['ordinal']==4 and retained['records'][-1]['ordinal']==259
capacity=json.loads(Path(sys.argv[1]+'.capacity').read_text())['cadence']['received_complete_AU']
assert capacity['total']==65537 and capacity['omitted_events']==2
assert capacity['counts_by_complete_second'][0]==65535 and capacity['complete_seconds']==3600
assert capacity['seconds_beyond_capacity']==1 and capacity['trailing_partial_count'] is None
minute=json.loads(Path(sys.argv[1]+'.idr-minute').read_text())
assert minute['received_idr']['counts_by_complete_second']==[1+s%3 for s in range(125)]
assert minute['received_idr']['total']==250 and minute['received_idr']['trailing_partial_count']==1
assert minute['keyframe_to_idr']['observed_complete_idrs']==250
assert minute['idr_per_minute']['complete_seconds']==125 and minute['idr_per_minute']['count']==249
assert minute['idr_per_minute']['complete_minute_counts']==[120,120]
assert abs(minute['idr_per_minute']['rate']-119.52)<1e-6
print('PASS AU/flip cadence, 1ms keyframe-IDR histogram, unpaired/omitted requests and pause evidence retention')
