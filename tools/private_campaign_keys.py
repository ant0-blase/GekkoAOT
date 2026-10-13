#!/usr/bin/env python3
"""Send bounded keys only to an owned private headless campaign and capture its screen."""
from pathlib import Path
import argparse,json,os,subprocess,time
ap=argparse.ArgumentParser();ap.add_argument('campaign',type=Path);ap.add_argument('game');ap.add_argument('keys',nargs='*');ap.add_argument('--gap',type=float,default=.25);ap.add_argument('--hold',type=float,default=.05);ap.add_argument('--capture',default='manual-screen.png');a=ap.parse_args()
assert .04<=a.hold<=.5 and .05<=a.gap<=5
r=a.campaign.resolve();st=json.loads((r/'status.json').read_text());assert st['phase']=='running'
c=Path(f"/proc/{st['compositor_pid']}/cmdline").read_bytes().split(b'\0');assert b'--backend' in c and c[c.index(b'--backend')+1]==b'headless'
p=next(x for x in (r/'boots').glob(a.game+'*/*/pid.json') if not(x.parent/'result.json').exists());pid=json.loads(p.read_text())['pid']
env={os.fsdecode(k):os.fsdecode(v) for x in Path(f'/proc/{pid}/environ').read_bytes().split(b'\0') if b'=' in x for k,v in [x.split(b'=',1)]};assert env['DISPLAY']==st['display'] and env['DISPLAY']!=os.environ.get('DISPLAY');env.pop('XDOTOOL_DEBUG',None)
c=Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0');assert os.fsencode(str(p.parent/'stop.request')) in c
(p.parent/'stop-input').touch();time.sleep(.4)
def command(*cmd):return subprocess.run(cmd,env=env,capture_output=True,text=True,check=True,timeout=3)
w=command('xdotool','search','--onlyvisible','--pid',str(pid)).stdout.split()[0];command('xdotool','windowfocus','--sync',w);assert command('xdotool','getwindowfocus','getwindowpid').stdout.strip()==str(pid)
actions=[]
for key in a.keys:
 assert key in ['j','Return','h','g','f','t','w','a','s','d','k','u','i','q','e','r','Left','Right','Up','Down']
 try:command('xdotool','keydown',key);time.sleep(a.hold)
 finally:command('xdotool','keyup',key)
 actions.append(dict(key=key,pid=pid,display=env['DISPLAY'],time=time.time()));time.sleep(a.gap)
(p.parent/('keys-'+a.capture+'.json')).write_text(json.dumps(actions,indent=2))
time.sleep(.8);env['WAYLAND_DISPLAY']=env['GAMESCOPE_WAYLAND_DISPLAY'];out=p.parent/a.capture;command('gamescopectl','screenshot',str(out));deadline=time.monotonic()+3
while not (out.is_file() and out.stat().st_size):
 assert time.monotonic()<deadline
 time.sleep(.05)
print(out)
