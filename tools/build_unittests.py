# build_unittests.py - 构建 C++ 单元测试 tools/unittest/unittest.exe。
#
# 与 .scratch/build_hosttest.py 同一套工具链配方（MSVC 路径写死，本机可用）；
# 但 src 不复用 build/obj——把被测的 12 个编译单元直接编进 build/obj-unittest，
# 测试结果不依赖 build.bat 是否跑过。pcre2 / CED 仍复用 build/obj-pcre2、
# build/obj-ced（没有就先跑一次 build.bat 或对应的 build-*.bat）。
#
# 用法：
#   python tools/build_unittests.py        # 只构建
#   python tools/build_unittests.py run    # 构建并立即运行

import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
MSVC_VER = '14.44.35207'
SDK_VER = '10.0.26100.0'
VS = r'C:\VS2022'
SDK = r'C:\Program Files (x86)\Windows Kits\10'
MSVC = os.path.join(VS, 'VC', 'Tools', 'MSVC', MSVC_VER)

env = dict(os.environ)
env['INCLUDE'] = ';'.join([
    os.path.join(MSVC, 'include'),
    os.path.join(SDK, 'Include', SDK_VER, 'ucrt'),
    os.path.join(SDK, 'Include', SDK_VER, 'shared'),
    os.path.join(SDK, 'Include', SDK_VER, 'um'),
    os.path.join(SDK, 'Include', SDK_VER, 'winrt'),
    os.path.join(ROOT, 'src'),
    os.path.join(ROOT, 'tools', 'unittest'),
    os.path.join(ROOT, 'third_party'),
    os.path.join(ROOT, 'third_party', 'compact_enc_det'),
    os.path.join(ROOT, 'third_party', 'pcre2', 'include'),
])
env['LIB'] = ';'.join([
    os.path.join(MSVC, 'lib', 'x64'),
    os.path.join(SDK, 'Lib', SDK_VER, 'ucrt', 'x64'),
    os.path.join(SDK, 'Lib', SDK_VER, 'um', 'x64'),
])
env['PATH'] = ';'.join([
    os.path.join(MSVC, 'bin', 'Hostx64', 'x64'),
    os.path.join(SDK, 'bin', SDK_VER, 'x64'),
    env.get('PATH', ''),
])

CL = os.path.join(MSVC, 'bin', 'Hostx64', 'x64', 'cl.exe')
LINK = os.path.join(MSVC, 'bin', 'Hostx64', 'x64', 'link.exe')
UNITTEST = os.path.join(ROOT, 'tools', 'unittest')
OUT = os.path.join(UNITTEST, 'unittest.exe')
OBJDIR = os.path.join(ROOT, 'build', 'obj-unittest')

# 被测模块：只挑不碰 DOpus SDK / WebView2 的纯逻辑单元。
SRC_UNITS = [
    'ced_wrap', 'config', 'doc_index', 'doc_regex', 'doc_regex8', 'doc_regex16',
    'doc_search', 'encoding_detect', 'localize', 'log', 'msg', 'util',
]
TEST_FILES = [
    'main', 'test_config', 'test_doc_index', 'test_doc_regex', 'test_doc_search',
    'test_encoding', 'test_localize', 'test_msg', 'test_util',
]


def run(name, argv):
    p = subprocess.run(argv, cwd=ROOT, env=env, capture_output=True)
    out = (p.stdout + p.stderr).decode('utf-8', 'replace')
    bad = [ln for ln in out.splitlines()
           if ('error' in ln.lower() and 'warning' not in ln.lower())
           or 'warning C4' in ln]
    print('%-12s rc=%d notes=%d' % (name, p.returncode, len(bad)))
    for ln in bad[:30]:
        print('   ' + ln.strip()[:240])
    return p.returncode


def main():
    os.makedirs(OBJDIR, exist_ok=True)
    for d in (os.path.join(ROOT, 'build', 'obj-ced'),
              os.path.join(ROOT, 'build', 'obj-pcre2', '8')):
        if not os.path.isdir(d):
            print('missing %s — 先跑 build.bat（或对应 build-*.bat）' % d)
            return 2

    objs = []
    for name in SRC_UNITS:
        src = os.path.join(ROOT, 'src', name + '.cpp')
        obj = os.path.join(OBJDIR, name + '.obj')
        objs.append(obj)
        rc = run(name, [CL, '/nologo', '/c', '/MT', '/O2', '/W4', '/EHsc', '/DUNICODE',
                        '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/D_WIN32_WINNT=0x0A00',
                        '/utf-8', '/DPCRE2_STATIC', '/Fo:' + obj, src])
        if rc:
            return rc
    for name in TEST_FILES:
        src = os.path.join(UNITTEST, name + '.cpp')
        obj = os.path.join(OBJDIR, name + '.obj')
        objs.append(obj)
        rc = run(name, [CL, '/nologo', '/c', '/MT', '/O2', '/W4', '/EHsc', '/DUNICODE',
                        '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/D_WIN32_WINNT=0x0A00',
                        '/utf-8', '/Fo:' + obj, src])
        if rc:
            return rc

    libs = []
    for sub in ('obj-ced', os.path.join('obj-pcre2', '8'), os.path.join('obj-pcre2', '16'),
                os.path.join('obj-pcre2', 'common')):
        d = os.path.join(ROOT, 'build', sub)
        libs += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith('.obj')]

    rc = run('link', [LINK, '/nologo', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
                      '/INCREMENTAL:NO', '/OUT:' + OUT] + objs + libs +
             ['kernel32.lib', 'user32.lib', 'version.lib'])
    if rc:
        return rc
    print('OK  ' + OUT)

    if len(sys.argv) > 1 and sys.argv[1] == 'run':
        p = subprocess.run([OUT], env=env)
        return p.returncode
    return 0


if __name__ == '__main__':
    sys.exit(main())
