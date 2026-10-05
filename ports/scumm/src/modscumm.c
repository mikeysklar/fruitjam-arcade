/* fruitjam-scumm - SCUMM v3/v4 on the Adafruit Fruit Jam
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MicroPython glue for scumm.mpy, a thin wrapper over fj_core.h. File
 * access goes back out to a Python object (see emu_scumm.py: ScummIO), and
 * is only ever called from inside a function of this module, on the
 * MicroPython stack: the engine's own coroutine never touches MicroPython.
 *
 *   scumm.init(arena, io) -> 0 or an error code (see scumm.error())
 *   scumm.config(key, value)
 *   scumm.set_output(fb, fb_w, fb_h, bpp, x0, y0)
 *   scumm.set_audio(ring, start)
 *   scumm.samples() -> int
 *   scumm.input(x, y, buttons)
 *   scumm.key(keycode, ascii)
 *   scumm.save(slot) / scumm.load(slot)
 *   scumm.frame() -> bool
 *   scumm.error() / scumm.game() -> str
 *   scumm.mem() -> (arena used, arena peak, engine stack high water)
 */

#include "py/dynruntime.h"

#include "backend/fj_core.h"

/* The io object lives as long as the adapter that passed it; it is also
 * stored as a module attribute so the GC sees it. */
static mp_obj_t io_obj;

static mp_obj_t io_call(qstr name, size_t n, mp_obj_t *args) {
	mp_obj_t fn = mp_load_attr(io_obj, name);
	return mp_call_function_n_kw(fn, n, 0, args);
}

static int io_open(void *ctx, const char *name, int write) {
	mp_obj_t a[2] = { mp_obj_new_str(name, strlen(name)), mp_obj_new_bool(write) };
	return mp_obj_get_int(io_call(MP_QSTR_open, 2, a));
}

static int io_read(void *ctx, int h, void *buf, int len) {
	mp_obj_t a[2] = { mp_obj_new_int(h), mp_obj_new_bytearray_by_ref(len, buf) };
	return mp_obj_get_int(io_call(MP_QSTR_read, 2, a));
}

static int io_write(void *ctx, int h, const void *buf, int len) {
	mp_obj_t a[2] = { mp_obj_new_int(h), mp_obj_new_bytes(buf, len) };
	return mp_obj_get_int(io_call(MP_QSTR_write, 2, a));
}

static int io_seek(void *ctx, int h, int32_t pos) {
	mp_obj_t a[2] = { mp_obj_new_int(h), mp_obj_new_int(pos) };
	return mp_obj_get_int(io_call(MP_QSTR_seek, 2, a));
}

static int32_t io_size(void *ctx, int h) {
	mp_obj_t a[1] = { mp_obj_new_int(h) };
	return mp_obj_get_int(io_call(MP_QSTR_size, 1, a));
}

static void io_close(void *ctx, int h) {
	mp_obj_t a[1] = { mp_obj_new_int(h) };
	io_call(MP_QSTR_close, 1, a);
}

static void io_log(void *ctx, const char *msg) {
	mp_obj_t a[1] = { mp_obj_new_str(msg, strlen(msg)) };
	io_call(MP_QSTR_log, 1, a);
}

static fj_io io_table;

static mp_obj_t mod_init(mp_obj_t arena_in, mp_obj_t io_in) {
	mp_buffer_info_t arena;
	mp_get_buffer_raise(arena_in, &arena, MP_BUFFER_RW);
	io_obj = io_in;
	mp_store_global(MP_QSTR__io, io_in);
	io_table.open = io_open;
	io_table.read = io_read;
	io_table.write = io_write;
	io_table.seek = io_seek;
	io_table.size = io_size;
	io_table.close = io_close;
	io_table.log = io_log;
	io_table.ctx = 0;
	return mp_obj_new_int(fj_core_init(arena.buf, arena.len, &io_table));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_init_obj, mod_init);

static mp_obj_t mod_config(mp_obj_t key, mp_obj_t value) {
	fj_core_config(mp_obj_str_get_str(key), mp_obj_str_get_str(value));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_config_obj, mod_config);

static mp_obj_t mod_set_output(size_t n_args, const mp_obj_t *args) {
	mp_buffer_info_t fb;
	mp_get_buffer_raise(args[0], &fb, MP_BUFFER_WRITE);
	int w = mp_obj_get_int(args[1]), h = mp_obj_get_int(args[2]);
	int bpp = mp_obj_get_int(args[3]);
	int x0 = mp_obj_get_int(args[4]), y0 = mp_obj_get_int(args[5]);
	if (x0 < 0 || y0 < 0 || x0 + FJ_SCREEN_W > w || y0 + FJ_SCREEN_H > h ||
	    (size_t)w * h * (bpp / 8) > fb.len) {
		mp_raise_ValueError(MP_ERROR_TEXT("framebuffer too small"));
	}
	fj_core_set_output(fb.buf, w, h, bpp, x0, y0);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_set_output_obj, 6, 6, mod_set_output);

static mp_obj_t mod_set_audio(mp_obj_t ring_in, mp_obj_t start_in) {
	mp_buffer_info_t ring;
	mp_get_buffer_raise(ring_in, &ring, MP_BUFFER_WRITE);
	fj_core_set_audio(ring.buf, ring.len / 4, mp_obj_get_int(start_in));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_set_audio_obj, mod_set_audio);

static mp_obj_t mod_samples(void) {
	return mp_obj_new_int_from_uint(fj_core_samples());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_samples_obj, mod_samples);

static mp_obj_t mod_input(mp_obj_t x, mp_obj_t y, mp_obj_t b) {
	fj_core_input(mp_obj_get_int(x), mp_obj_get_int(y), mp_obj_get_int(b));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_input_obj, mod_input);

static mp_obj_t mod_key(mp_obj_t code, mp_obj_t ascii) {
	fj_core_key(mp_obj_get_int(code), mp_obj_get_int(ascii));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_key_obj, mod_key);

static mp_obj_t mod_save(mp_obj_t slot) {
	fj_core_save(mp_obj_get_int(slot));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_save_obj, mod_save);

static mp_obj_t mod_load(mp_obj_t slot) {
	fj_core_load(mp_obj_get_int(slot));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_load_obj, mod_load);

static mp_obj_t mod_frame(void) {
	return mp_obj_new_bool(fj_core_frame());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_frame_obj, mod_frame);

static mp_obj_t mod_error(void) {
	const char *s = fj_core_error();
	return mp_obj_new_str(s, strlen(s));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_error_obj, mod_error);

static mp_obj_t mod_game(void) {
	const char *s = fj_core_game();
	return mp_obj_new_str(s, strlen(s));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_game_obj, mod_game);

static mp_obj_t mod_mem(void) {
	mp_obj_t t[3] = { mp_obj_new_int(fj_core_mem_used()), mp_obj_new_int(fj_core_mem_peak()),
			  mp_obj_new_int(fj_core_stack_used()) };
	return mp_obj_new_tuple(3, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_mem_obj, mod_mem);

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args) {
	MP_DYNRUNTIME_INIT_ENTRY

	mp_store_global(MP_QSTR_init, MP_OBJ_FROM_PTR(&mod_init_obj));
	mp_store_global(MP_QSTR_config, MP_OBJ_FROM_PTR(&mod_config_obj));
	mp_store_global(MP_QSTR_set_output, MP_OBJ_FROM_PTR(&mod_set_output_obj));
	mp_store_global(MP_QSTR_set_audio, MP_OBJ_FROM_PTR(&mod_set_audio_obj));
	mp_store_global(MP_QSTR_samples, MP_OBJ_FROM_PTR(&mod_samples_obj));
	mp_store_global(MP_QSTR_input, MP_OBJ_FROM_PTR(&mod_input_obj));
	mp_store_global(MP_QSTR_key, MP_OBJ_FROM_PTR(&mod_key_obj));
	mp_store_global(MP_QSTR_save, MP_OBJ_FROM_PTR(&mod_save_obj));
	mp_store_global(MP_QSTR_load, MP_OBJ_FROM_PTR(&mod_load_obj));
	mp_store_global(MP_QSTR_frame, MP_OBJ_FROM_PTR(&mod_frame_obj));
	mp_store_global(MP_QSTR_error, MP_OBJ_FROM_PTR(&mod_error_obj));
	mp_store_global(MP_QSTR_game, MP_OBJ_FROM_PTR(&mod_game_obj));
	mp_store_global(MP_QSTR_mem, MP_OBJ_FROM_PTR(&mod_mem_obj));
	mp_store_global(MP_QSTR_AUDIO_RATE, MP_OBJ_NEW_SMALL_INT(FJ_AUDIO_RATE));
	mp_store_global(MP_QSTR_WIDTH, MP_OBJ_NEW_SMALL_INT(FJ_SCREEN_W));
	mp_store_global(MP_QSTR_HEIGHT, MP_OBJ_NEW_SMALL_INT(FJ_SCREEN_H));
	mp_store_global(MP_QSTR_BTN_LEFT, MP_OBJ_NEW_SMALL_INT(FJ_BTN_LEFT));
	mp_store_global(MP_QSTR_BTN_RIGHT, MP_OBJ_NEW_SMALL_INT(FJ_BTN_RIGHT));

	MP_DYNRUNTIME_INIT_EXIT
}
