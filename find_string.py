import struct

with open("F:\\stealthripapi\\SteamCloudPatcher\\steamclient64_dump.bin", "rb") as f:
    data = f.read()

# search for "Cloud rewrite skip"
idx = data.find(b"Cloud rewrite skip")
if idx != -1:
    print(f"Found 'Cloud rewrite skip' at {idx:X}")
    
    # search for references to this string address
    # Usually it's an LEA instruction like `lea rcx, [rip + offset]`
    # We can search for the raw 32-bit offset relative to RIP
    # But it's easier to just search for the bytes in a small window
    
    for i in range(0, len(data)-4):
        # Calculate RIP relative offset
        # RIP is i + 4 (if instruction length after offset is 0, usually 4 for LEA or CALL)
        # We can just brute force search the 32-bit offset
        offset = idx - (i + 4)
        if offset >= -2**31 and offset < 2**31:
            packed = struct.pack("<i", offset)
            if data[i:i+4] == packed:
                print(f"Found reference to string at {i:X}")
else:
    print("String not found")

