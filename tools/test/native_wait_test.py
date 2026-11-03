"""Replay native VM waits through the compiled Windows script-clock hook.

Requires pefile/unicorn, a supported local EXE, the built DLL, its generated
symbols and tickrate_policy_test's adapter output. Does not launch the game.
"""
import argparse
import hashlib
import json
import re
import struct
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
    UC_X86_REG_EDI, UC_X86_REG_ESI, UC_X86_REG_EBP, UC_X86_REG_ESP,
    UC_X86_REG_EFLAGS, UC_X86_REG_FPCW)
from pathlib import Path


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ['exe','dll','symbols','adapters']:ap.add_argument(name,type=Path)
    a=ap.parse_args()
    assert hashlib.sha256(a.exe.read_bytes()).hexdigest()=='8f482e647b250babe098c6e3deb10129f9a935d013e852c99689cf8dff0d12cc','Unsupported EXE'
    m=Uc(UC_ARCH_X86,UC_MODE_32)
    for path in [a.exe,a.dll]:
        pe=pefile.PE(str(path));data=pe.get_memory_mapped_image()
        m.mem_map(pe.OPTIONAL_HEADER.ImageBase,(len(data)+4095)&~4095)
        m.mem_write(pe.OPTIONAL_HEADER.ImageBase,data)
    base=pe.OPTIONAL_HEADER.ImageBase
    symbols=a.symbols.read_text(encoding='utf-8-sig')
    def symbol(name):
        match=re.search(r'\{ 0x([0-9a-f]+)u, 0x[0-9a-f]+u, "'+re.escape(name)+r'"',symbols)
        assert match,'Missing symbol '+name
        return base+int(match[1],16)
    frame,value,adapter,end,stack=0x20001000,0x20002000,0x20003000,0x2001f000,0x30008000
    m.mem_map(0x20000000,0x20000);m.mem_map(0x30000000,0x10000)
    m.reg_write(UC_X86_REG_FPCW,0x37f)
    def put(addr,value):m.mem_write(addr,struct.pack('<I',value&0xffffffff))
    def get(addr):return struct.unpack('<I',m.mem_read(addr,4))[0]
    def call(addr,*args):
        put(stack,end)
        for i,arg in enumerate(args):put(stack+4+4*i,arg)
        m.reg_write(UC_X86_REG_ESP,stack);m.emu_start(addr,end,count=20000)
    cases=json.loads(a.adapters.read_text(encoding='utf-8-sig'))['adapters']
    code=bytes.fromhex(next(x[3] for x in cases if x[:3]==[7,5,3]))
    code=code.replace(bytes.fromhex('b800000200'),b'\xb8'+struct.pack('<I',symbol('tickrate::(anonymous namespace)::scriptWaitClock')))
    original=bytes.fromhex('81fbffffff00')
    assert bytes(m.mem_read(0x47e8eb,6))==original
    m.mem_write(adapter,code[:-1]+original+b'\xc3')
    m.mem_write(0x47e8eb,b'\xe8'+struct.pack('<i',adapter-0x47e8f0)+b'\x90')
    configure=symbol('tickrate::configureScriptWaits')
    reset=symbol('tickrate::resetScriptWaits')
    pool=0xe08f00
    def status(thread,wake=0,parent=None):put(pool+16*thread+8,(wake<<8)|15 if parent is None else (parent<<8)|18)
    def wait(seconds,now,thread=100,integer=False,check_registers=False):
        put(0xf08fa0,now);put(frame+0x70,thread)
        m.mem_write(value,struct.pack('<iI',seconds,6) if integer else struct.pack('<fI',seconds,5))
        seed={UC_X86_REG_ECX:0x11223344,UC_X86_REG_EDX:0x22334455,UC_X86_REG_ESI:0x33445566,
              UC_X86_REG_EDI:value,UC_X86_REG_EBP:frame}
        for reg,n in seed.items():m.reg_write(reg,n)
        put(stack,end);m.reg_write(UC_X86_REG_ESP,stack)
        m.emu_start(0x47e8a8 if integer else 0x47e862,0x47e8f1,count=20000)
        ticks=get(frame+0x50)
        assert m.reg_read(UC_X86_REG_EBX)==ticks
        # Successful values must leave the native range-check's carry flag.
        assert bool(m.reg_read(UC_X86_REG_EFLAGS)&1)==(ticks<0xffffff)
        if check_registers:
            for reg,n in seed.items():assert m.reg_read(reg)==n,(reg,n,m.reg_read(reg))
            assert m.reg_read(UC_X86_REG_ESP)==stack
        return ticks
    count=0
    for step in [8,10,33,50]:
        call(configure,step,1)
        for seconds,repeats in [(.05,100),(.05,200),(1,100),(60,1)]:
            status(100);now=100
            for _ in range(repeats):
                ticks=wait(seconds,now,check_registers=True);now=(now+ticks)&0xffffff
                status(100,now);count+=1
            assert abs((now-100)*step-seconds*repeats*1000)<=step/2,(step,seconds,repeats,now)
    call(configure,8,1)
    for integer in [False,True]:
        status(100);assert wait(60,100,integer=integer)==7500
    # Child function locals resolve to the root thread's fractional phase.
    status(100);status(101,parent=100)
    assert wait(.05,100,101)==6
    status(100,106);assert wait(.05,106,101)==7
    # Reused IDs, a late event resume, and map resets cannot inherit its phase.
    status(100);assert wait(.05,113)==6
    status(100,119);assert wait(.05,140)==6
    status(100,146);call(reset);assert wait(.05,146)==6
    assert wait(0,152)==0
    assert wait(.001,152)==1
    call(configure,8,0);status(100)
    assert wait(60,100,integer=True)==1200
    assert wait(.05,100)==1
    assert wait(375,100,integer=True)==7500  # zPAM compensated 60 seconds
    # HUD deadlines stay native milliseconds regardless of script clock mode.
    m.mem_write(0x482db0,b'\xd9\x05'+struct.pack('<I',value)+b'\xc3')
    m.mem_write(value,struct.pack('<f',60));put(0xf4b91c,1);put(0x193a96c,10000)
    call(0x5267b0,0x10000)
    assert get(0x194cf80+0x68)-10000==60000
    print(f'Native script clock: {count} repeated waits, mode/lifecycle/register checks and 60s HUD deadline passed')


if __name__=='__main__':main()
