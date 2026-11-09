#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Offline T27 identity and cross-edition guards, not native package proof."""
import importlib.util,pathlib,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('t27_package',ROOT/'scripts/package_identity.py')
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)
def flags(probe=False,public=False):
 return dict(X4_RECEIVER_NACK=1,X4_NACK_GRACE_MS=45,X4_RUMBLE=int(not probe),
  X4_INGRESS_MODE_BASELINE=0,X4_VIDEO_PROBE=int(probe),X4_VIDEO_CAPACITY_1080=1,
  X4_STREAM_PROFILE=1,X4_PUBLIC=int(public),X4_PROBE_T26=int(probe),X4_PROBE_T27=int(probe))
class T27Identity(unittest.TestCase):
 def test_three_exact_identities(self):
  rows=[('normal','',False,False,'XCLD00001','01.31','0.7.63-lab','XCloud4-0.7.63-lab-27.pkg'),
   ('public','',False,True,'XCLD00003','01.08','1.3.0-beta','XCloud4-1.3.0-beta-publico-27.pkg'),
   ('probe','t27',True,False,'XCLD00002','02.03','0.7.63-probe.t27','XCloud4-probe-t27.pkg')]
  header=(ROOT/'src/core/build_identity.h').read_text()
  builds=set()
  for mode,variant,probe,public,title,version,product,name in rows:
   m=p.metadata(mode,variant);f=flags(probe,public);p.check_flags(m,f)
   self.assertEqual((m['title_id'],m['version'],m['product_version'],m['package_name']),(title,version,product,name))
   build=p.build_identity(header,f,product);self.assertLessEqual(len(build),64);builds.add(build)
   self.assertEqual(p.verify_sfo(self.sfo(m['sfo']),m),m['sfo'])
  self.assertEqual(len(builds),3)
 def test_probe_requires_both_flags_and_never_products(self):
  m=p.metadata('probe','t27');f=flags(True)
  for key in ('X4_VIDEO_PROBE','X4_PROBE_T26','X4_PROBE_T27','X4_STREAM_PROFILE'):
   with self.assertRaises(ValueError,msg=key):p.check_flags(m,dict(f,**{key:0}))
  for mode in ('normal','public'):
   with self.assertRaises(ValueError):p.check_flags(p.metadata(mode),f)
  with self.assertRaises(ValueError):p.check_flags(p.metadata('probe','t26'),f)
  with self.assertRaises(ValueError):p.build_identity((ROOT/'src/core/build_identity.h').read_text(),dict(f,X4_PROBE_T26=0),p.T27_PRODUCT_VERSION)
 def test_source_switches_exclude_benchmarks_by_default(self):
  text=(ROOT/'Makefile').read_text()
  self.assertIn('X4_PROBE_T27 ?= 0',text)
  self.assertIn('filter-out $(wildcard src/video/probe_t27*.c)',text)
  self.assertIn('filter-out $(wildcard src/video/probe_t26*.c)',text)
  self.assertNotIn('filter-out src/video/gpu_convert.c src/video/gpu_quality_ref.c',text)
 @staticmethod
 def sfo(values):
  import struct
  keys=bytearray();payload=bytearray();entries=[]
  for k,v in values.items():
   at=len(keys);keys.extend(k.encode()+b'\0');raw,kind=(struct.pack('<I',v),0x0404) if isinstance(v,int) else (v.encode()+b'\0',0x0204)
   entries.append(struct.pack('<HHIII',at,kind,len(raw),len(raw),len(payload)));payload.extend(raw)
  keyat=20+16*len(entries)
  return b'\0PSF'+struct.pack('<IIII',0x101,keyat,keyat+len(keys),len(entries))+b''.join(entries)+keys+payload
if __name__=='__main__':unittest.main(verbosity=2)
