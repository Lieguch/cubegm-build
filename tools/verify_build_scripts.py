#!/usr/bin/env python3
"""
Deterministic pre-push linter for bash build scripts.

Purpose: catch the class of bug that `bash -n` CANNOT -- a call to a helper
function that is referenced but never defined in the same file (e.g. calling
`warn` when only `log`/`die` exist). This is a static, deterministic check:
it parses the file, it does not execute it.

Design goals (so it can be a real CI gate, not a noisy false-positive source):
  * Complete builtin/keyword allowlist embedded (no fragile subprocess/bash
    invocation, which crashes on the sandbox proxy's non-UTF8 output).
  * Skip files that are actually Python (mis-named .sh).
  * Strip comments and quoted strings before tokenizing, so words inside
    "# comment" or "log \"done\"" are not mistaken for commands.
  * Skip `for VAR in` / `case VAR in` loop variables.
  * Flag a command-position bareword ONLY if it matches ^[a-z][a-z0-9_]*$ ,
    is NOT defined in this file, and is NOT a known command.

A bareword that passes all exclusions but is never defined is, with high
confidence, a missing helper -- exactly the bug we want to block.

Run:  python3 verify_bash_helpers.py <file-or-dir> [<file-or-dir> ...]
Exit: 0 = clean, 1 = undefined helper call(s) found.
"""
import sys, os, re

# --- bash builtins -----------------------------------------------------------
BUILTINS = set('''
. : [ [[ alias bg bind break builtin caller cd command compgen complete compopt
continue declare dirs disown echo enable eval exec exit export false fc fg
getopts hash help history jobs kill let local logout mapfile popd printf pushd
pwd read readonly return set shift shopt source suspend test times trap true
type typeset ulimit umask unalias unset wait
'''.split())

# --- bash keywords -----------------------------------------------------------
KEYWORDS = set('''
if then else elif fi for while until do done case esac in of function time
select coproc [[ ]] { } (( )) !
'''.split())

# --- common external commands used by these build scripts --------------------
EXTERNALS = set('''
echo printf cd pwd ls cp mv rm mkdir rmdir touch cat head tail grep sed awk tr
cut sort uniq wc find tar curl wget git make sudo chown chmod chgrp ln read test
sleep true false exit return source python3 python base64 sha256sum sha1sum
md5sum nproc id command export set unset eval expr date which env xargs install
patch gpg ssh scp rsync tee dd kill wait getopts mktemp readlink realpath dirname
basename nice nohup timeout logger dpkg apt apt-get yum dnf apk pip pip3 cmake
ninja meson autoconf automake libtool pkg-config gcc g++ cc ld ar ranlib strip
nm objcopy objdump as ldconfig mount umount modprobe insmod lsmod systemctl
service crontab sshpass expect tmux screen gunzip gzip bzip2 xz unzip zip stat
seq file strings flex bison texinfo gawk gperf help2man autoreconf makeinfo
bash sh
'''.split())

# --- project binaries referenced as commands --------------------------------
PROJECT_BINS = set('''
picoarch frogui rkgame icube
'''.split())

# --- case-pattern tokens (arch/OS names used in `case $X in ...)`) -----------
CASE_PATTERNS = set('''
x86_64 amd64 aarch64 armv7l armv6l arm arm64 i386 i686 mips mipsel mips64
riscv64 linux darwin freebsd openbsd netbsd msys cygwin mingw mingw32 mingw64
windows macos gnu solaris hpux aix
'''.split())

KNOWN = BUILTINS | KEYWORDS | EXTERNALS | PROJECT_BINS | CASE_PATTERNS

FUNC_DEF_RE = re.compile(r'^\s*(?:function\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\)\s*\{?')
CONTROL = KEYWORDS | {'if', 'elif', 'else', 'then', 'do', 'while', 'until',
                      'for', 'case', 'function', 'time', 'fi', 'in', 'of', 'select'}

def looks_like_python(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            first = f.readline()
            if 'python' in first:
                return True
            for i, line in enumerate(f):
                if i >= 20:
                    break
                s = line.strip()
                if s.startswith('import ') or s.startswith('from ') or s.startswith('def '):
                    return True
    except Exception:
        pass
    return False

def extract_defined_functions(path):
    names = set()
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = FUNC_DEF_RE.match(line)
            if m:
                names.add(m.group(1))
    return names

def extract_loop_vars(path):
    """Collect `for VAR in` and `case VAR in` variable names to skip them."""
    vars_ = set()
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            for m in re.finditer(r'\b(?:for|case)\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\b', line):
                vars_.add(m.group(1))
    return vars_

def clean(line):
    # remove single/double quoted strings
    line = re.sub(r"'[^']*'", ' ', line)
    line = re.sub(r'"[^"]*"', ' ', line)
    # remove comments: # preceded by whitespace or at start of line
    line = re.sub(r'(?:^|\s)#.*$', '', line)
    return line

def candidate_commands(line):
    """Yield command-position barewords from a (cleaned) line."""
    for part in re.split(r'[;&\|\n]+', line):
        p = part.strip()
        if not p:
            continue
        p = re.sub(r'^[\(\{\[]+', '', p)
        toks = p.split()
        i = 0
        while i < len(toks) and toks[i] in CONTROL:
            i += 1
        if i < len(toks):
            yield toks[i].strip('(){}[]')

def main():
    paths = []
    for arg in sys.argv[1:]:
        if os.path.isdir(arg):
            for root, _, files in os.walk(arg):
                for fn in files:
                    if fn.endswith('.sh') or fn.endswith('.bash'):
                        paths.append(os.path.join(root, fn))
        elif os.path.isfile(arg):
            paths.append(arg)
    if not paths:
        print('[VERIFY] no shell scripts to scan')
        sys.exit(0)

    problems = []
    scanned = 0
    for p in paths:
        if looks_like_python(p):
            continue
        scanned += 1
        defined = extract_defined_functions(p)
        loop_vars = extract_loop_vars(p)
        in_dq = False  # inside an unterminated double-quoted string from a prev line
        with open(p, encoding='utf-8', errors='replace') as f:
            for n, raw in enumerate(f, 1):
                # Handle multi-line double-quoted strings: while we are inside one,
                # the entire line is string content until its closing quote.
                if in_dq:
                    q = raw.find('"')
                    if q == -1:
                        in_dq = True
                        continue
                    raw = raw[q + 1:]
                    in_dq = False
                cleaned = clean(raw)
                # Detect an unterminated double quote that carries to the next line.
                if cleaned.count('"') % 2 == 1:
                    in_dq = True
                for cmd in candidate_commands(cleaned):
                    if not re.match(r'^[a-z][a-z0-9_]*$', cmd):
                        continue
                    if cmd in defined or cmd in KNOWN or cmd in loop_vars:
                        continue
                    problems.append((p, n, cmd, raw.strip()))

    if problems:
        print('[VERIFY-FAIL] undefined helper function call(s) detected:')
        for p, n, cmd, line in problems:
            print(f'  {p}:{n}: call to undefined helper `{cmd}`  ->  {line}')
        sys.exit(1)
    print(f'[VERIFY-OK] scanned {scanned} shell script(s); no undefined helper calls.')
    sys.exit(0)

if __name__ == '__main__':
    main()
