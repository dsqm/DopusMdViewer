"""编 tools/skinedit（皮肤编辑器外壳）并同步预览资产。

    python tools/skinedit/build.py            # 全量
    python tools/skinedit/build.py --assets   # 只同步资产（改页面/样式时用，秒级）

产物：

    build\\obj-skinedit\\                    中间文件：obj、res、version.rc.inc，不入库
    tools\\skinedit\\bin\\skinedit.exe       可执行文件，不入库
    tools\\skinedit\\bin\\assets\\           页面 + 预览资产，被映射成 https://skinedit.localhost/
    tools\\skinedit\\bin\\assets\\skins\\     四套内置皮肤，当模板用

bin 是发布目录：exe 与 assets 同级，pack.bat 也在那里，打包就地生成 zip。
中间产物一律留在 build\\obj-skinedit\\，不进发布目录。

零警告门槛：cl /W4、rc、link 只要吐出一条 warning 就算失败。这个程序只有一个 TU，
容忍警告等于让它长期挂着。

exe 不链插件任何 TU（util / skin / log 一概不要），只链 WebView2 静态加载器——皮肤
编辑器要能单独拷走。MSVC 与 Windows SDK 自动探测（本机固定安装 → vswhere / Windows
Kits 最新版），也可用 MDV_MSVC_DIR / MDV_SDK_DIR 覆盖。
cmd.exe 在本机从 shell 起不来，所以这里直接调 cl.exe / rc.exe / link.exe。
"""

import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
TOOL = os.path.join(ROOT, 'tools', 'skinedit')
BIN = os.path.join(TOOL, 'bin')
WEB = os.path.join(ROOT, 'web')
def find_msvc():
    """定位 MSVC 工具集目录：环境变量 → 本机固定安装 → vswhere 找到的 VS。"""
    override = os.environ.get('MDV_MSVC_DIR')
    if override and os.path.isdir(override):
        return override

    candidates = [r'C:\VS2022']
    vswhere = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                           'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    if os.path.isfile(vswhere):
        p = subprocess.run([vswhere, '-latest', '-products', '*',
                            '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                            '-property', 'installationPath'],
                           capture_output=True, text=True)
        root = p.stdout.strip()
        if root:
            candidates.append(root)

    for root in candidates:
        tools = os.path.join(root, 'VC', 'Tools', 'MSVC')
        if not os.path.isdir(tools):
            continue
        versions = sorted(v for v in os.listdir(tools)
                          if os.path.isdir(os.path.join(tools, v)))
        if versions:
            return os.path.join(tools, versions[-1])
    return None


def find_sdk():
    """定位 Windows SDK：环境变量 → 本机固定安装，取最新的 10.x。"""
    root = os.environ.get('MDV_SDK_DIR') or r'C:\Program Files (x86)\Windows Kits\10'
    inc = os.path.join(root, 'Include')
    if not os.path.isdir(inc):
        return None, None
    versions = sorted(v for v in os.listdir(inc) if v.startswith('10.'))
    if not versions:
        return None, None
    return root, versions[-1]


MSVC = find_msvc()
SDK, SDK_VER = find_sdk()

OBJ = os.path.join(ROOT, 'build', 'obj-skinedit')
OUT = os.path.join(BIN, 'skinedit.exe')
ASSETS = os.path.join(BIN, 'assets')

CL = os.path.join(MSVC, 'bin', 'Hostx64', 'x64', 'cl.exe') if MSVC else ''
RC = os.path.join(SDK, 'bin', SDK_VER, 'x64', 'rc.exe') if SDK else ''
LINK = os.path.join(MSVC, 'bin', 'Hostx64', 'x64', 'link.exe') if MSVC else ''

env = dict(os.environ)
if MSVC and SDK:
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

ISSUE = re.compile(r'(error [A-Z]+\d+|fatal error|warning [A-Z]+\d+|LNK\d+)')

# 预览要从插件 web\ 取的资产。katex.css 与 fonts\ 是 node build.mjs 的产物（不入库），
# 缺了就报错——静默少拷一套资产，页面会变成"公式没样式"这种看不出原因的坏法。
WEB_ASSET_FILES = ['base.css', 'katex.css']
WEB_ASSET_DIRS = ['fonts', 'LICENSES']


class BuildError(Exception):
    pass


def run(name, argv):
    """跑一步工具，返回 True/False。警告与错误都算问题：本程序只容忍零条。"""
    t0 = time.perf_counter()
    p = subprocess.run(argv, cwd=ROOT, env=env, capture_output=True)
    dt = (time.perf_counter() - t0) * 1000
    out = (p.stdout + p.stderr).decode('gbk', 'replace')
    issues = [ln.strip() for ln in out.splitlines() if ISSUE.search(ln)]
    print('%-6s %6.0f ms  rc=%d  问题=%d' % (name, dt, p.returncode, len(issues)))
    for ln in issues[:20]:
        print('       ' + ln[:240])
    if p.returncode != 0:
        return False
    return not issues


def copy_file(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copy_tree(src, dst):
    shutil.copytree(src, dst, dirs_exist_ok=True)


# ---------------- 版本 ----------------

def sync_version():
    """调 tools/gen-version.ps1 取版本号，把生成的 inc 拷进本程序的 obj 目录。

    版本的公式只有那一份（年.月.日.时分）：两处各写一遍迟早会漂，所以宁可多起一个
    PowerShell。inc 拷过来是为了 skinedit.rc 的 include 路径不依赖插件的 obj 目录。
    """
    script = os.path.abspath(os.path.join(ROOT, 'tools', 'gen-version.ps1'))
    p = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                        '-File', script],
                       cwd=ROOT, env=env, capture_output=True)
    src = os.path.join(ROOT, 'build', 'obj', 'version.rc.inc')
    if p.returncode != 0 or not os.path.isfile(src):
        raise BuildError('版本号生成失败（tools/gen-version.ps1）：\n' +
                         (p.stdout + p.stderr).decode('gbk', 'replace')[:600])
    with open(src, encoding='ascii') as f:
        text = f.read()
    m = re.search(r'#define\s+MDV_VER_STRING\s+"([^"]+)"', text)
    copy_file(src, os.path.join(OBJ, 'version.rc.inc'))
    return m.group(1) if m else '(未知)'


# ---------------- 资产 ----------------

def extract_sprite(html_path, out_path):
    """从插件的 shell.html 抽出 <svg id="mdv-sprite"> 那段图标定义。

    图标只该有一份：插件改了形状，编辑器跟着变。抽成独立 .svg 而不是让页面去解析
    shell.html，是为了页面既能 fetch 后内联注入、也能直接 <use href="sprite.svg#id">。
    补 xmlns 是因为独立成文件后它就是一份文档，没有命名空间会被解析器丢掉。
    """
    with open(html_path, encoding='utf-8') as f:
        html = f.read()
    m = re.search(r'<svg id="mdv-sprite"[\s\S]*?</svg>', html)
    if not m:
        raise BuildError('web\\shell.html 里找不到 <svg id="mdv-sprite">…</svg>')
    svg = m.group(0)
    symbols = svg.count('<symbol ')
    if symbols == 0:
        raise BuildError('抽出来的 sprite 里一个 <symbol> 都没有')
    svg = svg.replace('<svg ', '<svg xmlns="http://www.w3.org/2000/svg" ', 1)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(svg + '\n')
    return symbols


def build_preview():
    """把预览页的 markdown-it 全家桶打成单文件 preview.js。

    产物放在 tools/skinedit/web/ 下，sync_assets 会把它当普通页面资产拷走；
    源码在 tools/skinedit/src/preview.mjs，依赖装在 webbuild/node_modules。
    """
    p = subprocess.run(['node', os.path.join('tools', 'skinedit', 'build-preview.mjs')],
                       cwd=ROOT, env=env, capture_output=True)
    if p.returncode != 0:
        raise BuildError('预览页打包失败：\n' +
                         (p.stdout + p.stderr).decode('utf-8', 'replace')[:800])


def sync_assets():
    build_preview()
    missing = [n for n in WEB_ASSET_FILES + WEB_ASSET_DIRS
               if not os.path.exists(os.path.join(WEB, n))]
    if missing:
        raise BuildError('web\\ 里缺 ' + '、'.join(missing) +
                         '——先跑 build.bat，或 node webbuild/build.mjs 生成前端产物')

    shutil.rmtree(ASSETS, ignore_errors=True)
    os.makedirs(ASSETS)

    # 1. 编辑器自己的页面（只有 html/js/css，改完重跑这条命令即可见）
    copy_tree(os.path.join(TOOL, 'web'), ASSETS)

    # 2. 预览要用的插件资产：base.css 是皮肤变量的老家，katex.css + fonts 渲染公式
    for name in WEB_ASSET_FILES:
        copy_file(os.path.join(WEB, name), os.path.join(ASSETS, name))
    for name in WEB_ASSET_DIRS:
        copy_tree(os.path.join(WEB, name), os.path.join(ASSETS, name))

    # 3. 图标定义（从 shell.html 抽）
    symbols = extract_sprite(os.path.join(WEB, 'shell.html'),
                             os.path.join(ASSETS, 'sprite.svg'))

    # 4. 预览样例
    sample = os.path.join(TOOL, 'samples', 'skin-preview.md')
    if not os.path.isfile(sample):
        raise BuildError('缺预览样例 tools\\skinedit\\samples\\skin-preview.md')
    copy_file(sample, os.path.join(ASSETS, 'samples', 'skin-preview.md'))

    # 5. 四套内置皮肤当模板：放在 assets 下，页面才 fetch 得到（虚拟主机映射的是
    #    <exe>\assets，不是整个 <exe>）
    copy_tree(os.path.join(WEB, 'skins'), os.path.join(ASSETS, 'skins'))
    skins = sorted(f for f in os.listdir(os.path.join(ASSETS, 'skins'))
                   if f.endswith('.css'))

    files = sum(len(fs) for _, _, fs in os.walk(ASSETS))
    print('资产    %d 个文件  sprite=%d 个 symbol  模板=%d 套'
          % (files, symbols, len(skins)))


# ---------------- 主流程 ----------------

def main():
    t0 = time.perf_counter()

    if '--assets' in sys.argv[1:]:
        try:
            sync_assets()
        except BuildError as e:
            print('失败：' + str(e))
            return 1
        print('资产已同步  总耗时 %.1f s' % (time.perf_counter() - t0))
        return 0

    os.makedirs(OBJ, exist_ok=True)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)

    if not MSVC or not SDK:
        print('找不到 MSVC 或 Windows SDK：可用 MDV_MSVC_DIR / MDV_SDK_DIR 指定')
        return 1

    for exe in (CL, RC, LINK):
        if not os.path.isfile(exe):
            print('缺工具：' + exe)
            return 1

    try:
        version = sync_version()
        print('版本    ' + version)

        if not run('编译', [CL, '/nologo', '/c', '/MT', '/O2', '/W4', '/EHsc', '/std:c++17',
                           '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN',
                           '/D_WIN32_WINNT=0x0A00', '/utf-8',
                           '/Fo:' + os.path.join(OBJ, 'skinedit.obj'),
                           os.path.join('tools', 'skinedit', 'src', 'skinedit.cpp')]):
            return 1

        res = os.path.join(OBJ, 'skinedit.res')
        if not run('资源', [RC, '/nologo', '/I' + OBJ, '/fo' + res,
                           os.path.join('tools', 'skinedit', 'skinedit.rc')]):
            return 1

        if not run('链接', [LINK, '/nologo', '/MACHINE:X64', '/SUBSYSTEM:WINDOWS',
                           '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF',
                           '/OUT:' + OUT,
                           os.path.join(OBJ, 'skinedit.obj'),
                           res,
                           os.path.join(ROOT, 'sdk', 'webview2', 'x64',
                                        'WebView2LoaderStatic.lib'),
                           'kernel32.lib', 'user32.lib', 'gdi32.lib', 'ole32.lib',
                           'oleaut32.lib', 'shell32.lib', 'shlwapi.lib', 'advapi32.lib',
                           'version.lib', 'comctl32.lib']):
            return 1

        sync_assets()
    except BuildError as e:
        print('失败：' + str(e))
        return 1

    size = os.path.getsize(OUT) / 1048576.0
    print('产物    %s  (%.1f MB)  总耗时 %.1f s'
          % (os.path.relpath(OUT, ROOT), size, time.perf_counter() - t0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
