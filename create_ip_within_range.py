#!/usr/bin/env python3

import random

def parse_cidr_as_int(cidr):
    addr=cidr.split('/')
    mask=addr[1]
    addr=addr[0].split('.')
    return int(addr[0]) * 256 * 256 * 256 + int(addr[1]) * 256 * 256 + int(addr[2]) * 256 + int(addr[3]), int(mask)
    
def int_to_ip(val):
    a = int(val / 256 / 256 / 256)
    val -= a * 256 * 256 * 256
    b = int(val / 256 / 256)
    val -= b * 256 * 256
    c = int(val / 256)
    val -= c * 256
    d = val
    return '{a}.{b}.{c}.{d}'.format(a=a, b=b, c=c, d=d)
    
while True:
    try:
        line=input()
    except EOFError:
        break
    try:
        netseg = parse_cidr_as_int(line)
        print(int_to_ip(netseg[0] + random.randint(0, 2 ** (32 - netseg[1]) - 1)))
    except:
        pass
