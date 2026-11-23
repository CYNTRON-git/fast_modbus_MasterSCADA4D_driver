#!/usr/bin/env python3
"""Offline SDK-35699 candidate builder. Never connects, installs or executes ARM code."""
from pathlib import Path
import argparse, hashlib, importlib.util, json, os, platform, sys, time

HERE=Path(__file__).resolve().parent
PIN='58d240c5736ae621ad8a19917803a824ddef0a8a48dd0efff7fa39e62fa5ebc3'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inputs',type=Path,required=True)
    p.add_argument('--work-dir',type=Path,required=True)
    p.add_argument('--toolchain',type=Path,required=True)
    p.add_argument('--build',action='store_true',help='Requires an allocated Mac slot; default only prepares files')
    a=p.parse_args()
    helper=HERE/'vendor_baseline_35699.py'
    if hashlib.sha256(helper.read_bytes()).hexdigest()!=PIN:raise ValueError('Pinned baseline helper changed')
    spec=importlib.util.spec_from_file_location('baseline',helper);b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
    kit,work,tc=a.inputs.resolve(),a.work_dir.resolve(),a.toolchain.resolve()
    if work.exists() or b.inside(work,kit) or b.inside(kit,work):raise ValueError('Use a new work directory outside immutable SDK inputs')
    if a.build and (platform.system()!='Linux' or platform.machine() not in ('x86_64','amd64')):raise ValueError('Vendor build needs Linux amd64 container')
    work.mkdir(parents=True)
    result={'status':'PREPARING','sdkVersion':'1.3.10.35699','production34421Verified':False,'hardwareValidated':False,'deployed':False}
    start=time.monotonic()
    try:
        api,runtime=b.prepare(kit,work)
        source=HERE.parent/'mplc_protocol_fast_modbus';target=api/'examples/mplc_protocol_fast_modbus';target.mkdir()
        hashes={}
        for file in sorted(source.iterdir()):
            if file.suffix not in ('.h','.cpp') or file.name=='dllmain.cpp':continue
            data=b.normalize(file.read_bytes());(target/file.name).write_bytes(data)
            hashes[file.name]=hashlib.sha256(data).hexdigest()
        b.save_json(work/'driver-sources.json',hashes)
        result.update(status='PREPARED_ONLY',compilationRun=False)
        if a.build:
            for n in ('home','tmp'):(work/n).mkdir(exist_ok=True)
            env={'PATH':str(tc/'bin')+':/usr/bin:/bin','HOME':str(work/'home'),'LC_ALL':'C','LANG':'C','SHELL':'/bin/bash','TMPDIR':str(work/'tmp')}
            result['toolchain']=b.preflight(tc,work,env)
            env.update(MPS_TOOLCHAIN_BIN=str(tc/'bin'),MPLC_RUNTIME=str(runtime),CXXFLAGS='-std=gnu++17 '+b.CPU,CFLAGS=b.CPU,NAME='mplc_protocol_fast_modbus',JOBS='1')
            cwd=api/'platform/linux/api'
            b.run_build(['/bin/bash',cwd/'makedrv.sh','linux-armv7hf'],cwd,env,work/'driver-build.log',900)
            result.update(status='PASS_CANDIDATE_COMPILE_LINK_ABI',compilationRun=True,abi=b.verify(cwd/'mplc_protocol_fast_modbus.so',tc,work,env))
    except BaseException as ex:
        result.update(status='FAILED',error=str(ex));raise
    finally:
        result['elapsedSeconds']=round(time.monotonic()-start,2)
        b.save_json(work/'fast-modbus-result.json',result)
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
