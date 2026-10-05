# SPDX-FileCopyrightText: 2026 Mikey Sklar
# SPDX-License-Identifier: GPL-3.0-or-later
"""CPython stand-in for scumm.mpy: the same functions over the host build of
the core (build/libfjscumm.dylib, `make -f Makefile.host
build/libfjscumm.dylib`), so emu_scumm.py can run on a computer."""

import ctypes
import os

_lib = ctypes.CDLL(os.path.join(os.path.dirname(__file__), "..", "..", "build", "libfjscumm.dylib"))

AUDIO_RATE = 22050
WIDTH = 320
HEIGHT = 200
BTN_LEFT = 1
BTN_RIGHT = 2

_OPEN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int)
_READ = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int)
_WRITE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int)
_SEEK = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int32)
_SIZE = ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_void_p, ctypes.c_int)
_CLOSE = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int)
_LOG = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p)


class _IO(ctypes.Structure):
    _fields_ = [("open", _OPEN), ("read", _READ), ("write", _WRITE), ("seek", _SEEK),
                ("size", _SIZE), ("close", _CLOSE), ("log", _LOG), ("ctx", ctypes.c_void_p)]


_lib.fj_core_error.restype = ctypes.c_char_p
_lib.fj_core_game.restype = ctypes.c_char_p
_lib.fj_core_samples.restype = ctypes.c_uint32
_lib.fj_core_mem_used.restype = ctypes.c_size_t
_lib.fj_core_mem_peak.restype = ctypes.c_size_t
_lib.fj_core_stack_used.restype = ctypes.c_size_t
_keep = []


def _ptr(buf):
    return ctypes.addressof((ctypes.c_char * len(buf)).from_buffer(buf))


def init(arena, io):
    def c_open(ctx, name, write):
        return io.open(name.decode(), bool(write))

    def c_read(ctx, h, buf, n):
        return io.read(h, memoryview((ctypes.c_char * n).from_address(buf)).cast("B"))

    def c_write(ctx, h, buf, n):
        return io.write(h, ctypes.string_at(buf, n))

    table = _IO(_OPEN(c_open), _READ(c_read), _WRITE(c_write), _SEEK(lambda c, h, p: io.seek(h, p)),
                _SIZE(lambda c, h: io.size(h)), _CLOSE(lambda c, h: io.close(h)),
                _LOG(lambda c, m: io.log(m.decode())), None)
    _keep[:] = [table, arena, io]
    return _lib.fj_core_init(ctypes.c_void_p(_ptr(arena)), ctypes.c_size_t(len(arena)), ctypes.byref(table))


def config(key, value):
    _lib.fj_core_config(key.encode(), value.encode())


def set_output(fb, w, h, bpp, x0, y0):
    _keep.append(fb)
    _lib.fj_core_set_output(ctypes.c_void_p(_ptr(fb)), w, h, bpp, x0, y0)


def set_audio(ring, start):
    _keep.append(ring)
    _lib.fj_core_set_audio(ctypes.c_void_p(_ptr(ring)), len(ring) * ring.itemsize // 4, start)


def samples():
    return _lib.fj_core_samples()


def input(x, y, b):
    _lib.fj_core_input(x, y, b)


def key(code, ascii):
    _lib.fj_core_key(code, ascii)


def save(slot):
    _lib.fj_core_save(slot)


def load(slot):
    _lib.fj_core_load(slot)


def frame():
    return bool(_lib.fj_core_frame())


def error():
    return _lib.fj_core_error().decode()


def game():
    return _lib.fj_core_game().decode()


def mem():
    return _lib.fj_core_mem_used(), _lib.fj_core_mem_peak(), _lib.fj_core_stack_used()
