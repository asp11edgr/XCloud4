#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Small host parser fixtures, NOT valid compressed video or console data.

Files contain deterministic minimal Annex-B prefixes/AUD bytes plus filler
bytes and a real X4AU1 index with Python SHA-256 identities. They must never
be decoded or presented. The test links the real source reader and MbedTLS
hash binding; no parser/hash callbacks are replaced.
"""
import argparse
import hashlib
import json
import pathlib
import struct


def need(ok, message):
    if not ok: raise ValueError(message)


def au_size(n): return 5+(n%9)*67


def au_bytes(n):
    count=au_size(n)
    return b'\0\0\0\1\x09'+bytes(1+(n*17+i*29)%251 for i in range(count-5))


def base(count):
    data=bytearray(); records=[]
    for n in range(count):
        payload=au_bytes(n); records.append((len(data),len(payload),n*1_000_000//60)); data.extend(payload)
    header=struct.pack('<8sIIQQQIIIIII32s',b'X4AU1\0\0\0',1,96,len(data),count,
                       count*1_000_000//60,1920,1080,1920,1088,60,1,
                       hashlib.sha256(data).digest())
    return bytes(data),header+b''.join(struct.pack('<QQQ',*row) for row in records),records


def array(data): return '{'+','.join(f'0x{n:02x}' for n in data)+'}'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',required=True,type=pathlib.Path)
    a=p.parse_args(); root=a.output.resolve()
    need(not root.exists(),'Preserve existing parser fixture directory')
    root.mkdir(parents=True); samples=root/'xcloud4-probe'/'t26'; samples.mkdir(parents=True)
    good,good_index,records=base(1200)
    rows=[]
    cases=[('good',good,good_index,None,None),
           # Damage outside the781 cached prefix: full identity must still fail.
           ('clip-damaged',good[:-1]+bytes([good[-1]^1]),good_index,hashlib.sha256(good).digest(),None),
           ('index-damaged',good,good_index[:-1]+bytes([good_index[-1]^1]),None,hashlib.sha256(good_index).digest())]
    for name,count in (('records780',780),('records1201',1201)):
        data,index,_=base(count);cases.append((name,data,index,None,None))
    for name,offset,value,fmt in (
        ('bad-offset',96+24*300,1,'<Q'),
        ('bad-size',96+24*300+8,2*1024*1024+1,'<Q'),
        ('bad-pts',96+24*1199+16,0,'<Q'),
        ('bad-header-count',24,1199,'<Q')):
        index=bytearray(good_index);struct.pack_into(fmt,index,offset,value)
        cases.append((name,good,bytes(index),None,None))
    for name,data,index,clip_expected,index_expected in cases:
        clip=samples/(name+'.h264'); idx=samples/(name+'.x4au')
        with clip.open('xb') as f:f.write(data)
        with idx.open('xb') as f:f.write(index)
        rows.append({'id':name,'clip_path':str(clip),'index_path':str(idx),
                     'clip_bytes':len(data),'index_bytes':len(index),
                     'clip_sha256':clip_expected or hashlib.sha256(data).digest(),
                     'index_sha256':index_expected or hashlib.sha256(index).digest()})
    lines=['/* Generated parser-only fixture catalog; NOT decodable video. */',
           '#pragma once',f'#define T26_CACHE_FIXTURE_ROOT {json.dumps(str(root))}',
           'static const X4T26GeneratedSample t26_cache_fixtures[] = {']
    for row in rows:
        lines += ['    {',f'        .id={json.dumps("parser-fixture-not-video-"+row["id"])},',
                  f'        .clip_path={json.dumps(row["clip_path"])}, .index_path={json.dumps(row["index_path"])},',
                  '        .visible_width=1920,.visible_height=1080,.coded_width=1920,.coded_height=1088,',
                  '        .fps_num=60,.fps_den=1,.frame_count=1200,.nominal_mbps=0,',
                  f'        .clip_bytes={row["clip_bytes"]}ull,.index_bytes={row["index_bytes"]}ull,',
                  '        .profile_idc=77,.level_idc=42,.max_num_ref_frames=1,.max_dec_frame_buffering=1,',
                  '        .pic_order_cnt_type=2,.chroma_format_idc=1,.bit_depth_luma=8,.bit_depth_chroma=8,',
                  '        .matrix_coefficients=1,.full_range=0,.cabac=1,.idr_count=1,',
                  '        .clip_sha256='+array(row['clip_sha256'])+',',
                  '        .index_sha256='+array(row['index_sha256'])+',','    },']
    lines += ['};','#define T26_CACHE_FIXTURE_COUNT (sizeof(t26_cache_fixtures)/sizeof(t26_cache_fixtures[0]))',
              'enum { T26_CACHE_GOOD=0,T26_CACHE_CLIP_DAMAGED,T26_CACHE_INDEX_DAMAGED,',
              '       T26_CACHE_RECORDS780,T26_CACHE_RECORDS1201,T26_CACHE_BAD_OFFSET,',
              '       T26_CACHE_BAD_SIZE,T26_CACHE_BAD_PTS,T26_CACHE_BAD_HEADER_COUNT };']
    for name,values in (('offset', [r[0] for r in records[:781]]),
                        ('size', [r[1] for r in records[:781]]),
                        ('pts', [r[2] for r in records[:781]])):
        lines += [f'static const uint64_t t26_cache_expected_{name}[781] = {{',
                  ','.join(str(v)+'ull' for v in values),'};']
    prefix=sum(r[1] for r in records[:781]); arena=sum((r[1]+255)&~255 for r in records[:781])
    lines += [f'#define T26_CACHE_EXPECTED_PREFIX {prefix}u',f'#define T26_CACHE_EXPECTED_V1_ARENA {arena}u','']
    with (root/'fixture_catalog.h').open('x') as f:f.write('\n'.join(lines))
    manifest={'scope':'Host parser fixtures only; no valid H264 or console measurements',
              'good_records':1200,'cached_prefix_records':781,'prefix_bytes':prefix,
              'v1_input_arena_bytes':arena,'files':[]}
    for file in sorted(samples.iterdir()):
        manifest['files'].append({'file':file.name,'bytes':file.stat().st_size,
                                 'sha256':hashlib.sha256(file.read_bytes()).hexdigest()})
    with (root/'fixture_manifest.json').open('x') as f:json.dump(manifest,f,indent=2);f.write('\n')
    print(json.dumps({'parser_fixtures':len(rows),'path':str(root),'console_or_benchmark_data':False}))


if __name__=='__main__':main()
