import struct
import capstone

# Read the dump
with open("F:\\stealthripapi\\SteamCloudPatcher\\steamclient64_dump.bin", "rb") as f:
    data = f.read()

# The STFixer signature for Cloud Rewrite Skip was:
# 85 C0 0F 85 ?? ?? 00 00 45 85 FF 0F 84 ?? ?? 00 00
# Let's search for similar patterns and disassemble them
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

def is_similar_pattern(insns):
    # We expect:
    # 1. test reg1, reg1
    # 2. jne / jnz
    # 3. test reg2, reg2
    # 4. je / jz
    if len(insns) < 4: return False
    
    if not insns[0].mnemonic.startswith('test'): return False
    if not insns[1].mnemonic.startswith('jne'): return False
    if not insns[2].mnemonic.startswith('test'): return False
    if not insns[3].mnemonic.startswith('je'): return False
    
    return True

matches = []
# We just do a sliding window search for 'test eax, eax' or similar
for i in range(len(data) - 20):
    if data[i] == 0x85 and data[i+1] == 0xC0 and data[i+2] == 0x0F and data[i+3] == 0x85:
        # found 'test eax, eax; jne ...'
        try:
            insns = list(md.disasm(data[i:i+30], i))
            if len(insns) >= 4 and is_similar_pattern(insns):
                matches.append((i, insns))
        except:
            pass

print(f"Found {len(matches)} matches for Cloud Rewrite Skip pattern")
for addr, insns in matches:
    print(f"\n--- Match at offset 0x{addr:X} ---")
    for insn in insns[:6]:
        print(f"0x{insn.address:X}:\t{insn.mnemonic}\t{insn.op_str}")

