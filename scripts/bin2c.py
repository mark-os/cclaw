#!/usr/bin/env python3
import sys

if len(sys.argv) < 3:
    print(f"Usage: {sys.argv[0]} <binary_file> <output_header>")
    sys.exit(1)

with open(sys.argv[1], "rb") as f:
    data = f.read()

var_name = "preload_net_blob"

with open(sys.argv[2], "w") as f:
    f.write(f"static const unsigned char {var_name}[] = {{\n")
    for i in range(0, len(data), 12):
        chunk = data[i:i+12]
        f.write("  " + ", ".join([f"0x{b:02x}" for b in chunk]))
        if i + 12 < len(data):
            f.write(",\n")
        else:
            f.write("\n")
    f.write(f"}};\n")
    f.write(f"static const unsigned int {var_name}_len = {len(data)};\n")
