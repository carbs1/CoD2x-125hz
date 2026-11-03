"""Execute emitted x86 adapters and archive arithmetic without launching CoD2.
Requires the JSON output of tickrate_policy_test and the Unicorn Python package.
"""
import argparse
import json
from pathlib import Path
import random
import struct
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_EAX,UC_X86_REG_ECX,UC_X86_REG_EDX,
    UC_X86_REG_EBX,UC_X86_REG_ESP,UC_X86_REG_EBP,UC_X86_REG_ESI,UC_X86_REG_EDI,UC_X86_REG_EFLAGS)

REGS=[UC_X86_REG_EAX,UC_X86_REG_ECX,UC_X86_REG_EDX,UC_X86_REG_EBX,
    UC_X86_REG_ESP,UC_X86_REG_EBP,UC_X86_REG_ESI,UC_X86_REG_EDI]
CODE,END,HELPER,STACK=0x10000,0x11000,0x20000,0x30000
RESULT=0x13572468

def machine(code):
    m=Uc(UC_ARCH_X86,UC_MODE_32)
    m.mem_map(CODE,0x2000);m.mem_map(HELPER,0x1000);m.mem_map(STACK,0x4000)
    m.mem_write(CODE,code);m.reg_write(UC_X86_REG_ESP,STACK+0x2000)
    m.mem_write(STACK+0x2000,struct.pack('<I',END))
    return m

def test_adapters(cases):
    for seq,client,out,hexcode in cases:
        m=machine(bytes.fromhex(hexcode))
        initial=[0x44440000+i*0x1111 for i in range(8)]
        initial[4]=STACK+0x2000;initial[5]=STACK+0x1000
        for register,value in zip(REGS,initial):m.reg_write(register,value)
        m.mem_write(initial[5]+8,struct.pack('<I',0x76543210))
        flags=0x246;m.reg_write(UC_X86_REG_EFLAGS,flags)
        expected_seq=0 if seq<0 else initial[seq]
        expected_client=0x76543210 if client==8 else initial[client]
        calls=[]
        def hook(uc,address,size,data):
            if address==HELPER:
                sp=uc.reg_read(UC_X86_REG_ESP)
                args=struct.unpack('<II',bytes(uc.mem_read(sp+4,8)))
                assert args==(expected_client,expected_seq),(seq,client,out,args)
                calls.append(args)
        m.hook_add(UC_HOOK_CODE,hook)
        # A conforming callee may clobber EAX/ECX/EDX and flags.
        m.mem_write(HELPER,b'\xb8'+struct.pack('<I',RESULT)+b'\x31\xc9\x31\xd2\xc3')
        m.emu_start(CODE,END,count=200)
        assert len(calls)==1
        for i,register in enumerate(REGS):
            expected=RESULT if i==out else initial[i]+(4 if i==4 else 0)
            assert m.reg_read(register)==expected,(seq,client,out,i)
        assert m.reg_read(UC_X86_REG_EFLAGS)==flags
    return len(cases)

def test_remainders(cases):
    rng=random.Random(100)
    count=0
    for divisor,hexcode in cases:
        code=bytes.fromhex(hexcode)
        # The sequence only pushes and pops EBX, so one machine serves every value.
        m=machine(code)
        values=[-2**31,-2**31+1,-divisor-1,-divisor,-1,0,1,divisor-1,divisor,
            divisor+1,2**31-1]+[rng.randint(-2**31,2**31-1) for _ in range(100)]
        for value in values:
            m.reg_write(UC_X86_REG_ECX,value&0xffffffff)
            m.reg_write(UC_X86_REG_EBX,0x12345678)
            m.reg_write(UC_X86_REG_ESI,0xabcdef00)
            m.emu_start(CODE,CODE+len(code),count=200)
            q=abs(value)//divisor*(-1 if value<0 else 1)
            expected=[q*divisor,value-q*divisor,q]
            for register,result in zip(REGS[:3],expected):
                assert m.reg_read(register)==result&0xffffffff,(divisor,value,register)
            assert m.reg_read(UC_X86_REG_EBX)==0x12345678
            assert m.reg_read(UC_X86_REG_ESI)==0xabcdef00
            assert m.reg_read(UC_X86_REG_ESP)==STACK+0x2000
            count+=1
    return count

def test_byte_rings():
    rng=random.Random(32)
    count=0
    for mb in [32,64,128,256]:
        size=mb*1024*1024
        shift=size.bit_length()-1
        windows=b'\x81\xe6'+struct.pack('<I',0x80000000|(size-1))+b'\x79\x08\x4e\x81\xce'+struct.pack('<I',(-size)&0xffffffff)+b'\x46'
        linux=b'\x89\xc8\x85\xc0\x79\x05\x05'+struct.pack('<I',size-1)+bytes([0xc1,0xf8,shift,0xc1,0xe0,shift,0x29,0xc1])
        values=[-2**31,-size-1,-size,-1,0,1,size-1,size,size+1,2**31-1]+[rng.randint(-2**31,2**31-1) for _ in range(100)]
        for code,register in [(windows,UC_X86_REG_ESI),(linux,UC_X86_REG_ECX)]:
            m=machine(code)  # register-only arithmetic, reused for every value
            for value in values:
                expected=value-(abs(value)//size*(-1 if value<0 else 1))*size
                m.reg_write(register,value&0xffffffff)
                m.emu_start(CODE,CODE+len(code),count=100)
                assert m.reg_read(register)==expected&0xffffffff,(mb,value,register)
                count+=1
    return count

def test_history_bounds():
    # Exact replacement emitted at CL_GetSnapshot, including its signed branch.
    code=bytes.fromhex('83f87f7f3a')
    branch=CODE+5+0x3a
    jump=lambda here:b'\xe9'+struct.pack('<i',END-(here+5))
    retrieval=machine(code)
    retrieval.mem_write(CODE+5,b'\xb9\x01\x00\x00\x00'+jump(CODE+10))
    retrieval.mem_write(branch,b'\x31\xc9'+jump(branch+2))
    invalidation=bytes.fromhex('83ff7f7e038d4181')
    m=machine(invalidation)
    for age in range(513):
        retrieval.reg_write(UC_X86_REG_EAX,age)
        retrieval.emu_start(CODE,END,count=30)
        assert retrieval.reg_read(UC_X86_REG_ECX)==int(age<128)
        m.reg_write(UC_X86_REG_EDI,age);m.reg_write(UC_X86_REG_ECX,age+1);m.reg_write(UC_X86_REG_EAX,1)
        m.emu_start(CODE,CODE+len(invalidation),count=30)
        assert m.reg_read(UC_X86_REG_EAX)==(age+1-127 if age>=128 else 1)
    return 513*2

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('json',type=Path)
    args=parser.parse_args()
    data=json.loads(args.json.read_text(encoding='utf-8-sig'))
    print(json.dumps({'configuration_checks':'passed','accepted_configurations':data['accepted_configurations'],
        'adapter_cases':test_adapters(data['adapters']),
        'signed_remainder_cases':test_remainders(data['remainders']),
        'archive_byte_ring_cases':test_byte_rings(),'client_history_boundary_cases':test_history_bounds()},indent=2))
