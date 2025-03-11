#!/usr/bin/env python3
import os

def generate_small_file(filename="small.txt"):
    # Build a file with repeated lines until ~900 bytes are written.
    # We'll write around 18 lines.
    with open(filename, "w") as f:
        for i in range(18):
            line = f"This is line {i+1} of the small test file. " * 2 + "\n"
            f.write(line)
    size = os.path.getsize(filename)
    print(f"{filename} generated with size {size} bytes.")

def generate_medium_file(filename="medium.txt"):
    # Build a file until the file size is at least 50 KB (~51200 bytes)
    with open(filename, "w") as f:
        text_line = "This is a line in the medium test file. It is repeated many times.\n"
        while f.tell() < 51200:
            f.write(text_line)
    size = os.path.getsize(filename)
    print(f"{filename} generated with size {size} bytes.")

def generate_big_file(filename="big.txt"):
    # Build a file until the file size is at least 420 KB (~430000 bytes)
    with open(filename, "w") as f:
        text_line = "This is a line in the big test file. It is repeated many times to generate a large file.\n"
        while f.tell() < 420000:
            f.write(text_line)
    size = os.path.getsize(filename)
    print(f"{filename} generated with size {size} bytes.")

if __name__ == "__main__":
    generate_small_file()
    generate_medium_file()
    generate_big_file()
    print("Test files generated: small.txt, medium.txt, big.txt")
