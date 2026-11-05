"""编 tools/skinedit 的外壳程序（开发期用）。

    python tools/skinedit/build.py

产物：
    build/obj-skinedit/skinedit.obj
    build/tools/skinedit.exe
    build/tools/assets/…            从 tools/skinedit/web 与 web 拷过去的分发资产

只做编译与链接，不碰版本号与图标（那是后面的事）。MSVC 与 SDK 的位置写死在本机
实际安装处；换机器要改下面三个常量。
"""

import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
MSVC_VER = '14.44.35207'
SDK_VER = '10.0.26100.0'
VS = r'C:\VS2022'
SDK = r'C:\Program Files (x86)\Windows Kits\10'

MSVC = os.path.join(VS, 'VC', 'Tools', 'MSVC', MSVC_VER)
OBJ = os.path.join(ROOT, 'build', 'obj-skinedit')
OUT = os.path.join(ROOT, 'build', 'tools', 'skinedit.exe')
ASSETS = os.path.join(ROOT, 'build', 'tools', 'assets')
TOOL = os.path.join(ROOT, 'tools', 'skinedit')

env = dict(os.environ)
env['INCLUDE'] = ';'.join([
    os.path.join(MSVC, 'include'),
    os.path.join(SDK, 'Include', SDK_VER, 'ucrt'),
    os.path.join(SDK, 'Include', SDK_VER, 'shared'),
    os.path.join(SDK, 'Include', SDK_VER, 'um'),
    os.path.join(SDK, 'Include', SDK_VER, 'winrt'),
    os.path.join(ROOT, 'sdk', 'webview2', 'include'),
])
env['LIB'] = ';'.join([
    os.path.join(MSVC, 'lib', 'x64'),
    os.path.join(SDK, 'Lib', SDK_VER, 'ucrt', 'x64'),
    os.path.join(SDK, 'Lib', SDK_VER, 'um', 'x64'),
])
env['PATH'] = ';'.join([
    os.path.join(MSVC, 'bin', 'Hostx64', 'x64'),
    os.path.join(SDK, 'bin', SDK_VER, 'x64'),
    os.environ.get('PATH', ''),
])

CL = os.path.join(MSVC, 'bin', 'Hostx64', 'x64', 'cl.exe')
LINK = os.path.join(MSVC, 'bin', 'Hostx64', 'x64', 'link.exe')


def run(name, argv):
    p = subprocess.run(argv, cwd=ROOT, env=env, capture_output=True)
    out = (p.stdout + p.stderr).decode('gbk', 'replace')
    bad = [ln for ln in out.splitlines()
           if re.search(r'(error [A-Z]+\d+|fatal error|warning C\d+|LNK\d+)', ln)]
    print('%s rc=%d issues=%d' % (name, p.returncode, len(bad)))
    for ln in bad[:20]:
        print('   ' + ln.strip()[:240])
    return p.returncode


def main():
    os.makedirs(OBJ, exist_ok=True)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)

    if run('compile', [CL, '/nologo', '/c', '/MT', '/O2', '/W4', '/EHsc', '/std:c++17',
                       '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN',
                       '/D_WIN32_WINNT=0x0A00', '/utf-8',
                       '/Fo:' + os.path.join(OBJ, 'skinedit.obj'),
                       os.path.join('tools', 'skinedit', 'src', 'skinedit.cpp')]):
        return 1

    if run('link', [LINK, '/nologo', '/MACHINE:X64', '/SUBSYSTEM:WINDOWS',
                    '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF',
                    '/OUT:' + OUT,
                    os.path.join(OBJ, 'skinedit.obj'),
                    os.path.join(ROOT, 'sdk', 'webview2', 'x64', 'WebView2LoaderStatic.lib'),
                    'kernel32.lib', 'user32.lib', 'gdi32.lib', 'ole32.lib', 'oleaut32.lib',
                    'shell32.lib', 'shlwapi.lib', 'advapi32.lib', 'version.lib']):
        return 1

    if os.path.isdir(ASSETS):
        shutil.rmtree(ASSETS)
    shutil.copytree(os.path.join(TOOL, 'web'), ASSETS)
    print('assets  %s' % os.path.relpath(ASSETS, ROOT))
    print('done    %s' % os.path.relpath(OUT, ROOT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
