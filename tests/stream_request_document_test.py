#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate the public, token-free contracts in the profile design document.

This checks the document, not execution of the production C constructors.
"""
import argparse
import copy
import json
from pathlib import Path
import re

arguments=argparse.ArgumentParser()
arguments.add_argument('--constructor-log',type=Path)
args=arguments.parse_args()
root = Path(__file__).resolve().parents[1]
# Exact historical 0.7.46 document, source 02122f101201bdd27ba9d61c5d6954d6e5e24140.
# SHA-256 48434dbe5a5b4b904b0b9b4a61b1540c940df6fe523c114332d4a82a6ec08e9f.
# Independent wire goldens; its historical prose is not current product policy.
blocks = re.findall(r"```json\n(.*?)\n```", (root / "tests/fixtures/stream_request_profiles_0746.md").read_text(encoding="utf-8"), re.S)
assert len(blocks) == 10
objects = [json.loads(text) for text in blocks]
base_device, base_play = objects[:2]
assert base_play["titleId"] == "TEST"
assert base_device == {
    'appInfo':{'env':{'clientAppId':'XCloud4','clientAppType':'native','clientAppVersion':'0.7.33','httpEnvironment':'prod'}},
    'dev':{'hw':{'make':'Sony','model':'PS4'},'os':{'name':'android','platform':'console'}}}
# Golden public wire object from Better xCloud's pinned generateMsDeviceInfo;
# this test does not run or replace any production constructor.
web_device={
    'appInfo':{'env':{'clientAppId':'www.xbox.com','clientAppType':'browser','clientAppVersion':'26.1.97',
        'clientSdkVersion':'10.3.7','httpEnvironment':'prod','sdkInstallId':''}},
    'dev':{'os':{'name':'windows','ver':'22631.2715','platform':'desktop'},
        'hw':{'make':'Microsoft','model':'unknown','sdktype':'web'},
        'browser':{'browserName':'chrome','browserVersion':'140.0.3485.54'},
        'displayInfo':{'dimensions':{'widthInPixels':4096,'heightInPixels':2160},'pixelDensity':{'dpiX':1,'dpiY':1}}}}
assert base_play["settings"]["timezoneOffsetMinutes"] == -360
for p, os_name in enumerate(("android", "windows", "windows", "windows", "windows")):
    expected_device=copy.deepcopy(web_device if p in (1,3,4) else base_device)
    expected_play = copy.deepcopy(base_play)
    expected_play["settings"]["osName"] = os_name
    expected_device['dev']['os']['name']=os_name
    assert objects[p * 2] == expected_device
    assert objects[p * 2 + 1] == expected_play
    for text in blocks[p * 2 : p * 2 + 2]:
        assert all(v not in text.lower() for v in ("token", "authorization", "bearer", "password"))
        json.loads(text)
if args.constructor_log:
    emitted=re.findall(r'PUBLIC_PROFILE_([0-4])\nX-MS-Device-Info: ([^\n]+)\n/play: ([^\n]+)',
        args.constructor_log.read_text(encoding='utf-8'))
    assert len(emitted)==5
    for expected_p,(p,header,body) in enumerate(emitted):
        assert int(p)==expected_p
        assert header==blocks[expected_p*2] and body==blocks[expected_p*2+1]
    print('PASS ten documented JSON strings byte-identical to real C constructor output; synthetic TEST ID; no credentials')
else:
    print("PASS documented public JSON: native720, native/windows H and pinned web G/HQ/720HQ goldens; valid TEST ID; no credentials (document verification only)")
