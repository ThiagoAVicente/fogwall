#!/usr/bin/env python3
"""Embed a text file as a C string constant: embed.py <symbol> <input> <output>"""
import sys

symbol, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]

with open(src, "r", encoding="utf-8") as f:
    text = f.read()

with open(dst, "w", encoding="utf-8") as f:
    f.write("/* Generated from %s — do not edit. */\n" % src)
    f.write("static const char %s[] =\n" % symbol)
    for line in text.splitlines():
        line = line.replace("\\", "\\\\").replace('"', '\\"')
        f.write('    "%s\\n"\n' % line)
    f.write("    ;\n")
