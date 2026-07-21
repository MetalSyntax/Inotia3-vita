import re
import subprocess

def find_writes():
    out = subprocess.check_output(["/home/metalsyntax/vitasdk/bin/arm-vita-eabi-objdump", "-d", "inotia3-1-3-2/lib/armeabi/libgame.so"]).decode('utf-8', errors='ignore')
    
    # We are looking for stores to 16de24 and 16de64.
    # Usually they are PC-relative loads into a register, then a store.
    # This is hard to parse reliably with regex, but we can look for the literal addresses in the GOT.
    pass

find_writes()
