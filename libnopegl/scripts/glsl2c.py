#!/usr/bin/env python3

import os.path as op
import re

_include_re = re.compile(r"\s*#include\s+(?P<filename>.*)$")


def _get_c_lines(fname, includes):
    c_lines = []
    with open(fname, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip()

            include_match = _include_re.match(line)
            if include_match:
                inc_fname = include_match.group("filename")
                inc_path = op.join(op.dirname(fname), inc_fname)
                includes.append(inc_path)
                c_lines += _get_c_lines(inc_path, includes)
                continue

            line = line.replace("\\", "\\\\")
            line = line.replace('"', '\\"')
            line = f'    "{line}\\n"'
            c_lines.append(line)
    return c_lines


def _escape_dep(path):
    return path.replace(" ", "\\ ")


def _run(fname, output, depfile):
    includes = []
    c_lines = _get_c_lines(fname, includes)
    name = op.basename(fname).replace(".", "_")
    c_code = "\n".join(c_lines)
    with open(output, "w", encoding="utf-8") as f:
        f.write(f"static const char * const {name} = \\\n{c_code};\n")

    # The shader and its includes, for the build system to regenerate the header when they change
    deps = " ".join(_escape_dep(path) for path in [fname] + includes)
    with open(depfile, "w", encoding="utf-8") as f:
        f.write(f"{_escape_dep(output)}: {deps}\n")


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("depfile")
    args = parser.parse_args()
    _run(args.input, args.output, args.depfile)
