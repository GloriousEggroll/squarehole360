set pagination off
set debuginfod enabled off
handle SIGSEGV nostop noprint pass
python
import gdb, json, struct, time
out = open('logs/rendering/native-map-loading.jsonl', 'a', buffering=1)
class LoadingBreakpoint(gdb.Breakpoint):
    def __init__(self, name):
        super().__init__('*' + name, internal=True)
        self.label = name
        self.hits = 0
    def stop(self):
        try:
            inf = gdb.selected_inferior()
            ctx = int(gdb.parse_and_eval('$rdi'))
            base = int(gdb.parse_and_eval('$rsi'))
            regs = struct.unpack('<33Q', bytes(inf.read_memory(ctx, 264)))
            entry = {'time': time.time(), 'function': self.label, 'base': hex(base),
                     'r3': hex(regs[0]), 'r4': hex(regs[4]), 'r5': hex(regs[5]), 'lr': hex(regs[32])}
            if self.label == 'sub_8253C3F8':
                data, count, cap = struct.unpack('>III', bytes(inf.read_memory(base + (regs[4] & 0xffffffff), 12)))
                entry['level_array'] = [hex(data), count, cap]
                if count < 128 and count:
                    entry['names'] = bytes(inf.read_memory(base + data, count * 8)).hex()
            if self.label == 'sub_822B7328':
                data, count, cap = struct.unpack('>III', bytes(inf.read_memory(base + (regs[0] & 0xffffffff), 12)))
                if data and count < 1024:
                    entry['package'] = bytes(inf.read_memory(base + data, count * 2)).decode('utf-16-be', 'replace').rstrip('\0')
            out.write(json.dumps(entry) + '\n')
        except Exception as err:
            out.write(json.dumps({'function': self.label, 'error': str(err)}) + '\n')
        self.hits += 1
        if self.hits > 200:
            self.enabled = False
        return False
for name in ['sub_8252A040', 'sub_8252A210', 'sub_8253C3F8', 'sub_82511F38', 'sub_822B7328']:
    LoadingBreakpoint(name)
end
continue
detach
quit
