"""Regression: restore latent Papyrus state in a separate process without executing history.

Run --mode write, then --mode read. Requires the existing quest fixture and native bridge build.
"""
import argparse
import hashlib
import json
import ctypes as c
import os
from pathlib import Path
import time
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, default=Path.cwd(), help='TES5onOpenMW workspace')
parser.add_argument('--mode', choices=('write','read'), required=True)
parser.add_argument('--original', action='store_true', help='Use original MQ101 instead of the latent quest fixture')
parser.add_argument('--dependencies', type=Path,
                    default=Path(__file__).resolve().parents[1] / 'Binaries/ThirdParty/Win64')
args=parser.parse_args()
root=args.root.resolve()
checkpoint=root/'build'/('t5-mq101-checkpoint.bin' if args.original else 't5-quest-checkpoint.bin')
expected=checkpoint.with_suffix('.json')
deps=args.dependencies.resolve()
with os.add_dll_directory(str(deps)), os.add_dll_directory(str(root/'build/openmw-tes5/Release')):
 d=c.CDLL(str(root/'build/openmw-tes5/Release/T5DataBridge.dll'))
 def fn(name,result,*args):
  f=getattr(d,name);f.restype=result;f.argtypes=args;return f
 p=c.c_void_p; u=c.c_uint32; i=c.c_int;s=c.c_char_p; q=c.c_uint64
 load=fn('T5DataLoadGameLocation',p,s,s,s,s,u,i,i,i,q,p,u,u,p,u)
 error=fn('T5DataGetError',s,p);release=fn('T5DataRelease',None,p)
 create=fn('T5PapyrusCreate',p,s);destroy=fn('T5PapyrusDestroy',None,p)
 configure=fn('T5PapyrusConfigure',None,p,i,u,i);attach=fn('T5PapyrusAttach',i,p,p)
 tick=fn('T5PapyrusTick',None,p,c.c_double);activate=fn('T5PapyrusActivate',i,p,u,u)
 save=fn('T5PapyrusSaveCheckpoint',p,p);restore=fn('T5PapyrusRestoreCheckpoint',i,p,p,p,q)
 get=fn('T5DataGetBuffer',p,p,s,c.POINTER(q));errors=fn('T5PapyrusErrors',s,p)
 disabled=fn('T5PapyrusIsDisabled',i,p,u)
 fixture=root/('build/t5-mq101-stock-profile' if args.original else 'build/t5-quest-autostart-fixture')
 plugins=root/'config/core-plugins.txt' if args.original else fixture/'plugins.txt'
 location=load(os.fsencode(fixture/'Data'),os.fsencode(plugins),os.fsencode(deps/'Resources'),os.fsencode(deps/'osgPlugins-3.6.5'),*( (0x3c,4,-20,1) if args.original else (0x01000800,0,0,0) ),0,None,0,0x3372b if args.original else 0,None,0)
 assert location and not error(location),error(location)
 sessions=[]
 def new():
  h=create(os.fsencode(root/'build/t5-snapshot-test-errors.log'));sessions.append(h);return h
 def snapshot(h):
  result=save(h);assert result,errors(h)
  n=q();buf=get(result,b'checkpoint',c.byref(n));data=c.string_at(buf,n.value);release(result);return data
 try:
  if args.mode == 'write':
   a=new(); configure(a,1,0x3372b if args.original else 0x01000910,1); assert attach(a,location)
   for step in range(2400 if args.original else 30): tick(a,.05)
   if not args.original:
    assert disabled(a,0x01000a01)==1
    assert activate(a,0x01000a00,0x14)
   tick(a,.05)
   data=snapshot(a);checkpoint.write_bytes(data)
   for step in range(120): tick(a,.05)
   expected.write_text(json.dumps({'sha256':hashlib.sha256(snapshot(a)).hexdigest(),'bytes':len(data)}))
   print('WROTE',checkpoint,len(data),'bytes',flush=True)
  else:
   data=checkpoint.read_bytes();b=new();start=time.perf_counter()
   assert restore(b,location,data,len(data)),errors(b)
   print('RESTORED',len(data),'bytes in',time.perf_counter()-start,'seconds',flush=True)
   assert snapshot(b)==data,'State differs immediately after restoration'
   for step in range(120): tick(b,.05)
   assert hashlib.sha256(snapshot(b)).hexdigest()==json.loads(expected.read_text())['sha256'],'Continuation diverged'
   if not args.original: assert disabled(b,0x01000a01)==0
   bad=new();assert not restore(bad,location,data[:len(data)//2],len(data)//2)
   bad2=new();corrupt=b'bad format'+data[10:];assert not restore(bad2,location,corrupt,len(corrupt))
   print('PASS separate-process restoration, latent continuations, shared wait links and invalid-file rejection',flush=True)
 finally:
  for h in sessions:destroy(h)
  release(location)
