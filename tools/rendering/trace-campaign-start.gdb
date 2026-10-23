set pagination off
set debuginfod enabled off
handle SIGSEGV nostop noprint pass
python
import gdb, json, struct, time
out = open('logs/rendering/campaign-start.jsonl', 'a', buffering=1)
option_breakpoints=[]
class ResultBreakpoint(gdb.FinishBreakpoint):
    def __init__(self, label, ctx, base, url=0, error=0):
        super().__init__(gdb.newest_frame(), internal=True)
        self.label,self.ctx,self.base,self.url=label,ctx,base,url
        self.error=error
    def stop(self):
        if self.label=='sub_82A0B7A0':
            for bp in option_breakpoints: bp.enabled=False
        inf=gdb.selected_inferior()
        result=struct.unpack('<Q',bytes(inf.read_memory(self.ctx,8)))[0]
        data={'function':self.label,'return':hex(result)}
        def read(addr,size): return bytes(inf.read_memory(self.base+(addr&0xffffffff),size))
        def u32(addr): return struct.unpack('>I',read(addr,4))[0]
        if self.label=='sub_826DACA8' and result:
            data['text']=read(result,160).decode('utf-16-be','replace').split('\0')[0]
        if self.url:
            world=u32(0x83429828)
            wi=u32(u32(world+0x50)+0x3c)
            wi=u32(wi)
            data['world_info']=hex(wi)
            data['net_mode']=read(wi+0x394,1).hex()
            data['world_flags']=hex(u32(wi+0x2b8))
            p,n,cap=struct.unpack('>III',read(self.error,12))
            data['load_error']=read(p,n*2).decode('utf-16-be','replace').rstrip('\0') if p and 0<n<1024 else ''
        out.write(json.dumps(data)+'\n')
        return False
class CampaignBreakpoint(gdb.Breakpoint):
    def __init__(self, name, setup=False):
        super().__init__('*' + name, internal=True)
        self.label, self.setup, self.hits = name, setup, 0
    def stop(self):
        try:
            inf = gdb.selected_inferior()
            ctx, base = int(gdb.parse_and_eval('$rdi')), int(gdb.parse_and_eval('$rsi'))
            raw = struct.unpack('<33Q', bytes(inf.read_memory(ctx, 264)))
            if self.label=='sub_82A0B7A0':
                for bp in option_breakpoints: bp.enabled=True
            if self.label in ['sub_826DACA8','sub_82CC5928','sub_826E80B8'] and not 0x82a0b7a0<=raw[32]<0x82a0bf40:
                return False
            regs = {3:raw[0], **{n:raw[n] for n in range(4, 11)}}
            def read(addr, size): return bytes(inf.read_memory(base + (addr & 0xffffffff), size))
            def u32(addr): return struct.unpack('>I', read(addr, 4))[0]
            entry = {'time':time.time(), 'function':self.label, 'lr':hex(raw[32]),
                     'registers':{str(k):hex(v) for k,v in regs.items()}}
            engine = u32(0x834290bc)
            vt = u32(engine)
            entry['engine'] = hex(engine)
            entry['engine_vtable'] = hex(vt)
            if self.setup:
                entry['methods'] = {hex(off):hex(u32(vt+off)) for off in [0x1c0,0x1c4,0x1c8]}
                for off in [0x1c4,0x1c8]: CampaignBreakpoint('sub_%08X' % u32(vt+off))
            else:
                if self.label in ['sub_826DACA8','sub_82CC5928','sub_82A0B7A0','sub_826E80B8']:
                    ResultBreakpoint(self.label,ctx,base,regs[4] if self.label=='sub_82A0B7A0' else 0,regs[6])
                if self.label=='sub_82A0B7A0':
                    def string(addr):
                        p,n,cap=struct.unpack('>III',read(addr,12))
                        return read(p,n*2).decode('utf-16-be','replace').rstrip('\0') if p and 0<n<1024 else ''
                    entry['map']=string(regs[4]+0x1c)
                    p,n,cap=struct.unpack('>III',read(regs[4]+0x28,12))
                    entry['options']=[string(p+12*i) for i in range(min(n,50))]
                if self.label=='sub_826DACA8':
                    entry['option']=read(regs[4],80).decode('utf-16-be','replace').split('\0')[0]
                entry['memory'] = {}
                for n in range(3,8):
                    p=regs[n]&0xffffffff
                    if p>0x10000:
                        try: entry['memory'][str(n)] = read(p,96).hex()
                        except gdb.MemoryError: pass
                if self.label=='sub_822B7328':
                    p,n,cap=struct.unpack('>III',read(regs[3],12))
                    if p and 0<n<1024: entry['package']=read(p,n*2).decode('utf-16-be','replace').rstrip('\0')
                if self.label=='sub_8253C3F8':
                    p,n,cap=struct.unpack('>III',read(regs[4],12))
                    entry['level_array']=[hex(p),n,cap]
                    if p and 0<n<128: entry['names']=read(p,n*8).hex()
            out.write(json.dumps(entry)+'\n')
        except Exception as err: out.write(json.dumps({'function':self.label,'error':str(err)})+'\n')
        self.hits += 1
        if self.setup or self.hits>=100: self.enabled=False
        return False
CampaignBreakpoint('sub_8299F3D0', True)
for name in ['sub_8252A040','sub_8252A210','sub_8253C3F8','sub_822B7328',
             'sub_82510BF8','sub_825285C0','sub_82A51A10','sub_82A51B20',
             'sub_82A10F38','sub_82A11138','sub_82A0B7A0']:
    CampaignBreakpoint(name)
for name in ['sub_826DACA8','sub_82CC5928','sub_826E80B8']:
    bp=CampaignBreakpoint(name)
    bp.enabled=False
    option_breakpoints.append(bp)
end
continue
detach
quit
