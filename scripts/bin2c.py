import sys

def bin2c(infn, varname):
    with open(infn, 'rb') as f:
        data = f.read()

    print(f"static const unsigned char {varname}[] = {{")
    for i in range(0, len(data), 12):
        chunk = data[i:i+12]
        print("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    print("};")
    print(f"static const unsigned int {varname}_len = {len(data)};")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: bin2c.py <input_file> <variable_name>")
        sys.exit(1)
    bin2c(sys.argv[1], sys.argv[2])
