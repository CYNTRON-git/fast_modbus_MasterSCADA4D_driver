#!/usr/bin/env python3
"""Pinned baseline compatibility build only; never deploy or execute ARM code."""
from __future__ import annotations
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shlex
import signal
import struct
import subprocess
import sys
import time
import zipfile

LABEL = 'baseline-only compatibility build; NOT production34421'
MANIFEST_SHA = '682c33bb99eddb6416388c6d086b283e9ec15585d732b57b3cedcde78816412d'
BASELINE_SHA = '48700e7cb6b6629e6fd596665463657799fb901f1393ab04954969bc35e754e1'
CPP_LINK_SHA = 'dbdc535366ff169062f5394e083d2373c53be4e21b480ed1a4138fe6da4307d2'
BASELINE = 'reference/new-api-vendor-baseline/mplc_cyntron (2).zip'
LIBS = ('masterplc.so', 'mplc_archive.so', 'mplcshare.so', 'opcua.so', 'liblua.so', 'mplc_events.so')
CPU = '-mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4'

class BuildError(RuntimeError):
    pass

def sha(data):
    return hashlib.sha256(data).hexdigest()

def save_json(path, obj):
    path.write_text(json.dumps(obj, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

def inside(path, parent):
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False

def normalize(data):
    if data.startswith((b'\xff\xfe', b'\xfe\xff')):
        data = data.decode('utf-16').encode('utf-8')
    if data.startswith(b'\xef\xbb\xbf'):
        data = data[3:]
    if b'\0' in data:
        raise BuildError('Unexpected NUL in source/header; no implicit decoding')
    data = re.sub(rb'\r+\n', b'\n', data).replace(b'\r', b'\n')
    return data if not data or data.endswith(b'\n') else data + b'\n'

def once(text, old, new):
    if text.count(old) != 1:
        raise BuildError('Reviewed source anchor mismatch: ' + old[:100])
    return text.replace(old, new, 1)

def patch_recipes(api):
    diffs = []
    def patch(relative, transform):
        path = api / relative
        before = path.read_text(encoding='utf-8')
        after = transform(before)
        path.write_text(after, encoding='utf-8', newline='\n')
        diffs.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
            fromfile='normalized-vendor/API/' + relative, tofile='build-copy/API/' + relative))
    def makedrv(text):
        text = once(text, '#!/bin/bash\n', '#!/bin/bash\nset -euo pipefail\n')
        first = text.index('    linux-armv7hf | nls-con-rsb)')
        last = text.index('    optilogic)', first)
        armv7 = text[first:last]
        armv7 = once(armv7, 'export PATH=/opt/arm-mpssoft-linux-gnueabihf/bin:$PATH',
                    'export PATH="$MPS_TOOLCHAIN_BIN:$PATH"')
        armv7 = once(armv7, 'export CXX=arm-mpssoft-linux-gnueabihf-g++',
                    'export CXX="$MPS_TOOLCHAIN_BIN/arm-mpssoft-linux-gnueabihf-g++"')
        armv7 = once(armv7, 'export CC=arm-mpssoft-linux-gnueabihf-gcc',
                    'export CC="$MPS_TOOLCHAIN_BIN/arm-mpssoft-linux-gnueabihf-gcc"')
        armv7 = once(armv7, 'export CXXFLAGS+=-Wno-psabi -Wno-narrowing',
                     'export CXXFLAGS="' + '$' + '{CXXFLAGS:-} -Wno-psabi -Wno-narrowing"')
        text = text[:first] + armv7 + text[last:]
        start = text.index('export DEPs=(')
        end = text.index('export MPLC_LIB_DIR=./out', start)
        block = '# Pinned copied runtime only; no /opt/mplc4 or interactive fallback.\n'
        block += 'for dep in ' + ' '.join(LIBS) + '; do\n'
        block += '    test -s "$MPLC_RUNTIME/$dep" || { echo "Missing runtime: $dep" >&2; exit 2; }\ndone\n'
        block += 'mkdir -p ./out\nexport DEPs="-Wl,--no-as-needed '
        block += ' '.join('-l:' + name for name in LIBS) + ' -Wl,--as-needed"\n\n'
        text = text[:start] + block + text[end:]
        text = once(text, 'export MPLC_LIB_DIR=./out', 'export MPLC_LIB_DIR="$MPLC_RUNTIME"')
        text = once(text, 'export MPLCLIBS=$(cd $MPLC_LIB_DIR && pwd)', 'export MPLCLIBS="$MPLC_RUNTIME"')
        text = once(text, 'make NAME="$NAME" | tee -a log.txt',
                    'make -j"$JOBS" NAME="$NAME" TARGET=linux-armv7hf | tee -a log.txt')
        return text
    def makefile(text):
        return once(text, 'LDFLAGS  += -shared -fPIC',
            'LDFLAGS  += -L$(MPLCLIBS) -Wl,-z,defs -Wl,--allow-shlib-undefined -shared -fPIC')
    def gpio(text):
        text = once(text, '#include <memory>\n', '#include <memory>\n#include <stdexcept>\n')
        return once(text, 'struct gpiohandle_request req;', 'struct gpiohandle_request req{};')
    patch('platform/linux/api/makedrv.sh', makedrv)
    patch('platform/linux/api/Makefile', makefile)
    patch('examples/mplc_cyntron/gpio_helper.cpp', gpio)
    return ''.join(diffs)

def prepare(kit, work):
    raw = (kit / 'manifest.json').read_bytes()
    if sha(raw) != MANIFEST_SHA:
        raise BuildError('Input-kit manifest pin mismatch')
    rows = {row['path']: row for row in json.loads(raw)['files']}
    copied, normalized_files = [], []
    def verified(relative):
        source = kit / relative
        if not inside(source, kit) or source.is_symlink() or not source.is_file():
            raise BuildError('Unsafe/missing input: ' + relative)
        data = source.read_bytes()
        if len(data) != rows[relative]['bytes'] or sha(data) != rows[relative]['sha256']:
            raise BuildError('Input changed: ' + relative)
        return data
    def write_copy(data, target, origin, text):
        result = normalize(data) if text else data
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(result)
        record = {'input': origin, 'input_sha256': sha(data),
            'work_path': target.relative_to(work).as_posix(), 'copied_sha256': sha(result)}
        copied.append(record)
        if data != result:
            normalized_files.append(record)
    api, runtime = work / 'API', work / 'runtime'
    sdk_prefix = 'sdk-1.3.10.35699/API/'
    for relative in sorted(rows):
        if relative.startswith(sdk_prefix):
            write_copy(verified(relative), api / relative[len(sdk_prefix):], relative, True)
    for name in (*LIBS, 'libmpsc++.so', 'version.txt'):
        relative = 'runtime-cyntron-1.3.10.35699/' + name
        write_copy(verified(relative), runtime / name, relative, False)
    baseline = verified(BASELINE)
    if sha(baseline) != BASELINE_SHA:
        raise BuildError('Vendor baseline ZIP pin mismatch')
    (work / 'vendor-baseline.zip').write_bytes(baseline)
    with zipfile.ZipFile(work / 'vendor-baseline.zip') as archive:
        names = set()
        for member in archive.infolist():
            path = PurePosixPath(member.filename)
            if member.is_dir() or path.suffix not in ('.cpp', '.h'):
                continue
            if len(path.parts) != 2 or path.parts[0] != 'mplc_cyntron' or path.name in names or '..' in path.parts:
                raise BuildError('Unexpected baseline member')
            names.add(path.name)
            write_copy(archive.read(member), api / 'examples' / member.filename,
                       BASELINE + '!' + member.filename, True)
    if len(names) != 12:
        raise BuildError('Expected 12 original source/header members')
    diff = patch_recipes(api)
    (work / 'changes.diff').write_text(diff, encoding='utf-8', newline='\n')
    save_json(work / 'normalization.json', normalized_files)
    save_json(work / 'source-provenance.json', {'classification': LABEL,
        'input_manifest_sha256': MANIFEST_SHA, 'vendor_zip_sha256': BASELINE_SHA,
        'script_sha256': sha(Path(__file__).read_bytes()), 'copied_inputs': copied,
        'semantic_patch_sha256': sha(diff.encode()), 'semantic_patch': 'changes.diff',
        'patched_files': [{'path': name, 'sha256': sha((api / name).read_bytes())} for name in
            ('platform/linux/api/makedrv.sh', 'platform/linux/api/Makefile', 'examples/mplc_cyntron/gpio_helper.cpp')],
        'standalone_beeper_used': False, 'production_features_added': False})
    return api, runtime

def capture(command, env, cwd):
    process = subprocess.run([str(x) for x in command], cwd=cwd, env=env,
        capture_output=True, text=True, timeout=30)
    if process.returncode:
        raise BuildError('Command failed: ' + shlex.join([str(x) for x in command]) + '\n' + process.stderr[-4000:])
    return process.stdout.strip()

def preflight(toolchain, work, env):
    arm = toolchain / 'bin'
    cpp = arm / 'arm-mpssoft-linux-gnueabihf-g++'
    for name in ('g++', 'gcc', 'readelf', 'nm'):
        if not os.access(arm / ('arm-mpssoft-linux-gnueabihf-' + name), os.X_OK):
            raise BuildError('Missing compiler tool: ' + name)
    version = capture([cpp, '-dumpfullversion'], env, work)
    machine = capture([cpp, '-dumpmachine'], env, work)
    if version != '13.3.0' or not re.fullmatch(r'arm-[\w-]*linux-gnueabihf', machine):
        raise BuildError('Expected vendor GCC13.3.0 ARMhf, got ' + version + ' / ' + machine)
    library = Path(capture([cpp, '-print-file-name=libstdc++.so'], env, work)).resolve()
    if not inside(library, toolchain) or sha(library.read_bytes()) != CPP_LINK_SHA:
        raise BuildError('Vendor C++ link-library pin mismatch')
    dynamic = capture([arm / 'arm-mpssoft-linux-gnueabihf-readelf', '-d', library], env, work)
    if not re.search(r'\(SONAME\).*\[libmpsc\+\+\.so\]', dynamic):
        raise BuildError('Expected vendor libstdc++.so SONAME libmpsc++.so')
    result = {'compiler': str(cpp), 'compiler_sha256': sha(cpp.read_bytes()), 'version': version,
        'dumpmachine': machine, 'cpp_link_library': str(library), 'cpp_link_sha256': CPP_LINK_SHA,
        'cpp_soname': 'libmpsc++.so', 'make_version': capture(['make', '--version'], env, work).splitlines()[0]}
    save_json(work / 'toolchain.json', result)
    return result

def run_build(command, cwd, env, log, timeout):
    with log.open('w', encoding='utf-8') as stream:
        stream.write('$ ' + shlex.join([str(x) for x in command]) + '\n')
        stream.flush()
        process = subprocess.Popen([str(x) for x in command], cwd=cwd, env=env,
            stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
            raise BuildError('Build interrupted/timed out; logs and work copy retained')
    if code:
        raise BuildError('Build exited ' + str(code) + '; see ' + str(log))

def verify(artifact, toolchain, work, env):
    data = artifact.read_bytes()
    if len(data) < 52 or data[:6] != b'\x7fELF\x01\x01':
        raise BuildError('Not little-endian ELF32')
    kind, machine = struct.unpack_from('<HH', data, 16)
    flags = struct.unpack_from('<I', data, 36)[0]
    if kind != 3 or machine != 40 or not flags & 0x400:
        raise BuildError('Not an ARM hard-float shared object')
    arm = toolchain / 'bin'
    reports = {}
    for name, options in [('header', ['-h']), ('attributes', ['-A']), ('dynamic', ['-d']),
                           ('comment', ['-p', '.comment']), ('symbols', ['--dyn-syms', '--wide'])]:
        reports[name] = capture([arm / 'arm-mpssoft-linux-gnueabihf-readelf', *options, artifact], env, work)
        (work / ('abi-' + name + '.txt')).write_text(reports[name] + '\n', encoding='utf-8')
    needed = re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', reports['dynamic'])
    if any('/' in name or 'libstdc++' in name for name in needed):
        raise BuildError('Forbidden NEEDED dependency: ' + repr(needed))
    if not set((*LIBS, 'libmpsc++.so')).issubset(needed):
        raise BuildError('Missing required vendor/runtime dependencies: ' + repr(needed))
    if 'VFP registers' not in reports['attributes'] or '13.3.0' not in reports['comment']:
        raise BuildError('Missing hard-float attributes or GCC13.3 comment')
    exports = capture([arm / 'arm-mpssoft-linux-gnueabihf-nm', '-D', '--defined-only', artifact], env, work)
    (work / 'abi-exports.txt').write_text(exports + '\n', encoding='utf-8')
    if not any(line.split()[-1] == 'InitAddin' for line in exports.splitlines() if line.split()):
        raise BuildError('Missing InitAddin export')
    return {'artifact': str(artifact), 'bytes': len(data), 'sha256': sha(data), 'needed': needed,
        'elf': 'ELF32 ARM shared object, hard-float', 'own_object_undefined_references_rejected': True,
        'transitive_runtime_dependencies_validated': False, 'target_executed': False,
        'patchelf_or_needed_rewrite_used': False}

def main():
    parser = argparse.ArgumentParser(description=LABEL)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True, help='New directory; never reused or deleted')
    parser.add_argument('--toolchain', type=Path, default=Path('/work/toolchains/arm-mpssoft-linux-gnueabihf'))
    parser.add_argument('--jobs', type=int, choices=(1, 2), default=1)
    parser.add_argument('--timeout', type=int, default=900)
    parser.add_argument('--prepare-only', action='store_true', help='Copy/patch only; run no compiler or make')
    args = parser.parse_args()
    kit, work, toolchain = args.inputs.resolve(), args.work_dir.resolve(), args.toolchain.resolve()
    if work.exists() or inside(work, kit) or inside(kit, work):
        raise BuildError('Work directory must be new and separate from immutable input kit')
    if not args.prepare_only and (platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'amd64')):
        raise BuildError('Compilation requires Linux amd64')
    if not args.prepare_only and any(re.search(r'\s', str(p)) for p in (work, toolchain)):
        raise BuildError('Vendor Makefile requires work/toolchain paths without whitespace')
    if args.timeout < 1:
        raise BuildError('Timeout must be positive')
    work.mkdir(parents=True)
    start = time.monotonic()
    result = {'classification': LABEL, 'production_34421_verified': False, 'hardware_validation': False}
    try:
        api, runtime = prepare(kit, work)
        if args.prepare_only:
            result.update(status='PREPARED_ONLY', compilation_run=False)
        else:
            for name in ('home', 'tmp'):
                (work / name).mkdir()
            env = {'PATH': str(toolchain / 'bin') + ':/usr/bin:/bin', 'HOME': str(work / 'home'),
                'LC_ALL': 'C', 'LANG': 'C', 'SHELL': '/bin/bash', 'TMPDIR': str(work / 'tmp')}
            tools = preflight(toolchain, work, env)
            env.update(MPS_TOOLCHAIN_BIN=str(toolchain / 'bin'), MPLC_RUNTIME=str(runtime),
                CXXFLAGS='-std=gnu++17 ' + CPU, CFLAGS=CPU, NAME='mplc_cyntron', JOBS=str(args.jobs))
            cwd = api / 'platform/linux/api'
            capture(['/bin/bash', '-n', cwd / 'makedrv.sh'], env, cwd)
            run_build(['/bin/bash', cwd / 'makedrv.sh', 'linux-armv7hf'], cwd, env, work / 'build.log', args.timeout)
            abi = verify(cwd / 'mplc_cyntron.so', toolchain, work, env)
            result.update(status='PASS_BASELINE_COMPILE_LINK_ABI', compilation_run=True, tools=tools, abi=abi)
    except BaseException as exc:
        result.update(status='FAILED', error=str(exc), compilation_pass=False)
        raise
    finally:
        result['elapsed_seconds'] = round(time.monotonic() - start, 2)
        save_json(work / 'result.json', result)
    print(json.dumps(result, ensure_ascii=False, indent=2))

if __name__ == '__main__':
    def stop(signum, frame):
        raise KeyboardInterrupt('Termination requested')
    signal.signal(signal.SIGTERM, stop)
    try:
        main()
    except (BuildError, OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        print('ERROR: ' + str(exc), file=sys.stderr)
        raise SystemExit(1)
