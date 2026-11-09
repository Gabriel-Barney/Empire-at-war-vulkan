#!/usr/bin/env python3
r"""launch_foc.py - start FoC (corruption\StarWarsG.exe) from a Claude shell so that it keeps running.

  toolchain\venv\Scripts\python.exe -I tools\probe_mod\common\launch_foc.py [--game-root DIR] [--no-windowed] [--watch]
                                                                           [game args...]

Background (2026-11-09): a StarWarsG.exe started from a Claude shell (Start-Process, cmd start, subprocess) exits at once,
and since 2026-11-09 a one-line .cmd started with `Start-Process explorer.exe <cmd>` dies silently too. What works:
CreateProcess with DEBUG_ONLY_THIS_PROCESS, then DebugActiveProcessStop at the first debug event (CREATE_PROCESS):
the game runs on, detached, with no debugger attached. Used for the storyb t09 and a2m t13 runs.

Default game args: WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 (--no-windowed drops them); any further arguments are
passed on as they are, e.g. MODPATH=Mods\Q2RebelSlice. Prints "pid <n>" and exits 0 once detached (the game keeps
running). --watch stays attached instead (the old dbg_launch.py without --early): prints OutputDebugString text,
non-breakpoint exceptions and the exit code, and detaches after 30 s without a debug event. Exit codes: 0 started,
1 CreateProcess failed, 2 the game folder was not found.
"""
import argparse
import ctypes
import ctypes.wintypes as W
import os
import sys

DEFAULT_ROOT = r'F:\SteamLibrary\steamapps\common\Star Wars Empire at War'   # q2common.ps1 $Q2DefaultGameRoot
WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
DEBUG_ONLY_THIS_PROCESS = 0x2
CREATE_PROCESS_DEBUG_EVENT, EXIT_PROCESS_DEBUG_EVENT, OUTPUT_DEBUG_STRING_EVENT, EXCEPTION_DEBUG_EVENT = 3, 5, 8, 1
DBG_CONTINUE, DBG_EXCEPTION_NOT_HANDLED = 0x00010002, 0x80010001
EXCEPTION_BREAKPOINT = 0x80000003


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [('cb', W.DWORD), ('lpReserved', W.LPWSTR), ('lpDesktop', W.LPWSTR), ('lpTitle', W.LPWSTR),
                ('dw', W.DWORD * 7), ('dwFlags', W.DWORD), ('wShowWindow', W.WORD), ('cbReserved2', W.WORD),
                ('lpReserved2', ctypes.c_void_p), ('hStd', W.HANDLE * 3)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [('hProcess', W.HANDLE), ('hThread', W.HANDLE), ('dwProcessId', W.DWORD), ('dwThreadId', W.DWORD)]


class DEBUG_EVENT(ctypes.Structure):
    # x64 layout: three DWORDs, 4 bytes padding, then the 8-aligned union (160 bytes)
    _fields_ = [('code', W.DWORD), ('pid', W.DWORD), ('tid', W.DWORD), ('pad', W.DWORD), ('u', ctypes.c_byte * 160)]


def game_root(hint):
    for c in (hint, DEFAULT_ROOT):
        if c and os.path.isfile(os.path.join(c, 'corruption', 'StarWarsG.exe')):
            return c
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game-root', help='folder containing corruption\\StarWarsG.exe (default: %s)' % DEFAULT_ROOT)
    ap.add_argument('--no-windowed', action='store_true', help='do not add WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080')
    ap.add_argument('--watch', action='store_true', help='stay attached and print debug output instead of detaching')
    ap.add_argument('--early', action='store_true', help='detach at the first debug event (the default; kept for old commands)')
    a, extra = ap.parse_known_args()
    root = game_root(a.game_root)
    if not root:
        print('game not found (pass --game-root <folder containing corruption\\StarWarsG.exe>)')
        return 2
    cwd = os.path.join(root, 'corruption')
    exe = os.path.join(cwd, 'StarWarsG.exe')
    args = ([] if a.no_windowed else WINDOWED) + extra
    cmd = '"%s" %s' % (exe, ' '.join(args))
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(STARTUPINFOW)
    pi = PROCESS_INFORMATION()
    if not k.CreateProcessW(None, ctypes.create_unicode_buffer(cmd), None, None, False, DEBUG_ONLY_THIS_PROCESS, None,
                            cwd, ctypes.byref(si), ctypes.byref(pi)):
        print('CreateProcess failed, error %d' % ctypes.get_last_error())
        return 1
    print('pid %d' % pi.dwProcessId)
    print('cmd %s' % cmd)
    sys.stdout.flush()
    k.DebugSetProcessKillOnExit(False)   # never take the game down with this script
    ev = DEBUG_EVENT()
    while True:
        if not k.WaitForDebugEvent(ctypes.byref(ev), 30000):
            print('no debug event for 30 s: detaching (the game runs on)')
            break
        cont = DBG_CONTINUE
        if ev.code == CREATE_PROCESS_DEBUG_EVENT:
            h_file = ctypes.c_void_p.from_buffer(ev.u, 0).value
            if h_file:
                k.CloseHandle(ctypes.c_void_p(h_file))
            if not a.watch:
                k.ContinueDebugEvent(ev.pid, ev.tid, cont)
                ok = k.DebugActiveProcessStop(pi.dwProcessId)
                print('detached' if ok else 'DebugActiveProcessStop failed, error %d' % ctypes.get_last_error())
                return 0
        elif ev.code == OUTPUT_DEBUG_STRING_EVENT:
            addr = ctypes.c_void_p.from_buffer(ev.u, 0).value
            uni = ctypes.c_ushort.from_buffer(ev.u, 8).value
            ln = ctypes.c_ushort.from_buffer(ev.u, 10).value
            buf = ctypes.create_string_buffer(ln * (2 if uni else 1))
            n = ctypes.c_size_t()
            k.ReadProcessMemory(pi.hProcess, ctypes.c_void_p(addr), buf, len(buf), ctypes.byref(n))
            text = buf.raw.decode('utf-16-le', 'replace') if uni else buf.raw.decode('mbcs', 'replace')
            print('ODS: ' + text.rstrip('\x00\r\n'))
        elif ev.code == EXCEPTION_DEBUG_EVENT:
            code = ctypes.c_uint32.from_buffer(ev.u, 0).value
            first = ctypes.c_uint32.from_buffer(ev.u, 152).value
            if code != EXCEPTION_BREAKPOINT:
                print('exception %#x first_chance=%d' % (code, first))
                cont = DBG_EXCEPTION_NOT_HANDLED
        elif ev.code == EXIT_PROCESS_DEBUG_EVENT:
            print('exit code %d' % ctypes.c_uint32.from_buffer(ev.u, 0).value)
            k.ContinueDebugEvent(ev.pid, ev.tid, cont)
            return 0
        sys.stdout.flush()
        k.ContinueDebugEvent(ev.pid, ev.tid, cont)
    k.DebugActiveProcessStop(pi.dwProcessId)
    return 0


if __name__ == '__main__':
    sys.exit(main())
