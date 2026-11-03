"""Replay native turret instructions with the actual compiled Windows hooks.

Requires pefile, capstone, unicorn; accepts locally supplied game executable.
Run after a Release build has generated its current symbol header and after
tickrate_policy_test.exe has emitted the adapter JSON. Never launches a game.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ESP

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('exe',type=Path);ap.add_argument('dll',type=Path)
    ap.add_argument('symbols',type=Path);ap.add_argument('adapters',type=Path)
    a=ap.parse_args()
    assert hashlib.sha256(a.exe.read_bytes()).hexdigest()=='8f482e647b250babe098c6e3deb10129f9a935d013e852c99689cf8dff0d12cc','Unsupported native EXE'
    m=Uc(UC_ARCH_X86,UC_MODE_32)
    for path in [a.exe,a.dll]:
        p=pefile.PE(str(path));data=p.get_memory_mapped_image()
        m.mem_map(p.OPTIONAL_HEADER.ImageBase,(len(data)+4095)&~4095)
        m.mem_write(p.OPTIONAL_HEADER.ImageBase,data)
    base=p.OPTIONAL_HEADER.ImageBase
    symbols=a.symbols.read_text(encoding='utf-8-sig')
    def symbol(name):
        match=re.search(r'\{ 0x([0-9a-f]+)u, 0x([0-9a-f]+)u, "'+re.escape(name)+r'"',symbols)
        assert match,'Missing symbol '+name
        return base+int(match[1],16),int(match[2],16)
    def absolute_data(name):
        addr,size=symbol(name);c=Cs(CS_ARCH_X86,CS_MODE_32);c.detail=True
        addresses=[op.mem.disp for insn in c.disasm(bytes(m.mem_read(addr,size)),addr)
                   for op in insn.operands if op.type==X86_OP_MEM and not op.mem.base and not op.mem.index]
        assert len(addresses)==1,(name,addresses)
        return addresses[0]
    entity,turret,adapter,clock,stack,end=0x20000000,0x20002000,0x20010000,0x20011000,0x30008000,0x2001f000
    m.mem_map(entity,0x20000);m.mem_map(0x30000000,0x10000)
    def put(addr,value):m.mem_write(addr,struct.pack('<I',value&0xffffffff))
    def get(addr):return struct.unpack('<i',m.mem_read(addr,4))[0]
    def run(addr,registers,stop=end):
        put(stack,end);m.reg_write(UC_X86_REG_ESP,stack)
        for reg,val in registers.items():m.reg_write(reg,val)
        m.emu_start(addr,stop,count=10000)
    frame_msec=absolute_data('turretSoundClock')
    frame_pointer=absolute_data('tickrate::(anonymous namespace)::turretCountdown')
    countdown=symbol('tickrate::(anonymous namespace)::turretCountdown')[0]
    sound=symbol('turretSoundWin32')[0]
    # Use the emitted production register adapter, replacing only its target.
    cases=json.loads(a.adapters.read_text(encoding='utf-8-sig'))['adapters']
    code=bytearray(bytes.fromhex(next(x[3] for x in cases if x[:3]==[-1,3,0])))
    assert code.count(bytes.fromhex('b800000200'))==1
    code=code.replace(bytes.fromhex('b800000200'),b'\xb8'+struct.pack('<I',countdown))
    m.mem_write(adapter,bytes(code));put(frame_pointer,frame_msec)
    original=bytes(m.mem_read(0x51c89c,6))
    assert original==bytes.fromhex('8b430883c0ce')
    m.mem_write(0x51c89c,b'\xe8'+struct.pack('<i',adapter-(0x51c89c+5))+b'\x90')
    put(entity+0x15c,turret)
    # No stop/loop sound assets: exercise the native timer branch without
    # dispatching unrelated engine sound calls.
    put(turret+64,1)
    results=[]
    for step in [1,8,10,16,25,33,50,100,200,1000]:
        put(frame_msec,step)
        for duration in [1,8,31,50,100,997]:
            put(turret+8,duration);put(turret+40,duration)
            fire_ticks=sound_ticks=0
            while get(turret+8)>0:
                run(0x51c89c,{UC_X86_REG_EBX:turret},0x51c8aa);fire_ticks+=1
                assert fire_ticks<2000
            while get(turret+40)>0:
                run(sound,{UC_X86_REG_EAX:entity});sound_ticks+=1
                assert sound_ticks<2000
            expected=(duration+step-1)//step
            assert fire_ticks==expected and sound_ticks==expected,(step,duration,fire_ticks,sound_ticks,expected)
            # Idle/unmanned sound keeps the original 50 ms per think.
            put(turret+40,duration);idle_ticks=0
            while get(turret+40)>0:
                run(0x51c8e0,{UC_X86_REG_EAX:entity});idle_ticks+=1
                assert idle_ticks<2000
            assert idle_ticks==(duration+49)//50
            results.append({'frame_ms':step,'duration_ms':duration,'manned_callbacks':expected,'elapsed_ms':expected*step,'idle_callbacks':idle_ticks})
    print(json.dumps({'passed':True,'timer_fixtures':len(results),'native_paths':3,'samples':results},indent=2))

if __name__=='__main__':main()
