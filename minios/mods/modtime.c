/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <math.h>

#include "py/runtime.h"
#include "py/smallint.h"
#include "py/mphal.h"

#define sleep_select select

#define FLOAT_NSEC_TO_SEC(_nsec)   ((float)(_nsec) / (float)1000000000ULL)

// mingw32 defines CLOCKS_PER_SEC as ((clock_t)<somevalue>) but preprocessor does not handle casts
#if defined(__MINGW32__) && !defined(__MINGW64_VERSION_MAJOR)
#define MP_REMOVE_BRACKETSA(x)
#define MP_REMOVE_BRACKETSB(x) MP_REMOVE_BRACKETSA x
#define MP_REMOVE_BRACKETSC(x) MP_REMOVE_BRACKETSB x
#define MP_CLOCKS_PER_SEC MP_REMOVE_BRACKETSC(CLOCKS_PER_SEC)
#else
#define MP_CLOCKS_PER_SEC CLOCKS_PER_SEC
#endif

#if defined(MP_CLOCKS_PER_SEC)
#define CLOCK_DIV (MP_CLOCKS_PER_SEC / 1000.0F)
#else
#error Unsupported clock() implementation
#endif

STATIC mp_obj_t mod_time_time(void) {
#if MICROPY_PY_BUILTINS_FLOAT
    struct timeval tv;
    gettimeofday(&tv, NULL);
    mp_float_t val = tv.tv_sec + (mp_float_t)tv.tv_usec / 1000000;
    return mp_obj_new_float(val);
#else
    return mp_obj_new_int((mp_int_t)time(NULL));
#endif
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mod_time_time_obj, mod_time_time);

STATIC mp_obj_t mod_time_ticks_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    mp_uint_t us = tv.tv_sec * 1000000 + tv.tv_usec;
    return MP_OBJ_NEW_SMALL_INT(us & MP_SMALL_INT_POSITIVE_MASK);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mod_time_ticks_us_obj, mod_time_ticks_us);

STATIC mp_obj_t mod_time_ticks_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    mp_uint_t ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return MP_OBJ_NEW_SMALL_INT(ms & MP_SMALL_INT_POSITIVE_MASK);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mod_time_ticks_ms_obj, mod_time_ticks_ms);

STATIC mp_obj_t mod_time_ticks_diff(mp_obj_t oldval, mp_obj_t newval) {
    mp_uint_t old = MP_OBJ_SMALL_INT_VALUE(oldval);
    mp_uint_t new = MP_OBJ_SMALL_INT_VALUE(newval);
    return MP_OBJ_NEW_SMALL_INT((new - old) & MP_SMALL_INT_POSITIVE_MASK);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_time_ticks_diff_obj, mod_time_ticks_diff);

// Note: this is deprecated since CPy3.3, but pystone still uses it.
STATIC mp_obj_t mod_time_clock(void) {
  return mp_obj_new_float(FLOAT_NSEC_TO_SEC(monotonic_clock()));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mod_time_clock_obj, mod_time_clock);

STATIC mp_obj_t mod_time_sleep(mp_obj_t arg) {
#if MICROPY_PY_BUILTINS_FLOAT
    struct timeval tv;
    mp_float_t val = mp_obj_get_float(arg);
    double ipart;
    tv.tv_usec = round(modf(val, &ipart) * 1000000);
    tv.tv_sec = ipart;
    int res;
    while (1) {
        res = sleep_select(0, NULL, NULL, NULL, &tv);
        #if MICROPY_SELECT_REMAINING_TIME
        // TODO: This assumes Linux behavior of modifying tv to the remaining
        // time.
        if (res != -1 || errno != EINTR) {
            break;
        }
        if (MP_STATE_VM(mp_pending_exception) != MP_OBJ_NULL) {
            return mp_const_none;
        }
        //printf("select: EINTR: %ld:%ld\n", tv.tv_sec, tv.tv_usec);
        #else
        break;
        #endif
    }
    RAISE_ERRNO(res, errno);
#else
    // TODO: Handle EINTR
    sleep(mp_obj_get_int(arg));
#endif
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_time_sleep_obj, mod_time_sleep);

STATIC mp_obj_t mod_time_sleep_ms(mp_obj_t arg) {
    usleep(mp_obj_get_int(arg) * 1000);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_time_sleep_ms_obj, mod_time_sleep_ms);

STATIC mp_obj_t mod_time_sleep_us(mp_obj_t arg) {
    usleep(mp_obj_get_int(arg));
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_time_sleep_us_obj, mod_time_sleep_us);

STATIC mp_obj_t mod_time_strftime(size_t n_args, const mp_obj_t *args) {
    time_t t;
    if (n_args == 1) {
        t = time(NULL);
    } else {
        // CPython requires passing struct tm, but we allow to pass time_t
        // (and don't support struct tm so far).
        t = mp_obj_get_int(args[1]);
    }
    struct tm *tm = localtime(&t);
    char buf[32];
    size_t sz = strftime(buf, sizeof(buf), mp_obj_str_get_str(args[0]), tm);
    return mp_obj_new_str(buf, sz, false);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_time_strftime_obj, 1, 2, mod_time_strftime);

STATIC const mp_rom_map_elem_t mp_module_time_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_utime) },
    { MP_ROM_QSTR(MP_QSTR_clock), MP_ROM_PTR(&mod_time_clock_obj) },
    { MP_ROM_QSTR(MP_QSTR_sleep), MP_ROM_PTR(&mod_time_sleep_obj) },
    { MP_ROM_QSTR(MP_QSTR_sleep_ms), MP_ROM_PTR(&mod_time_sleep_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_sleep_us), MP_ROM_PTR(&mod_time_sleep_us_obj) },
    { MP_ROM_QSTR(MP_QSTR_time), MP_ROM_PTR(&mod_time_time_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks_ms), MP_ROM_PTR(&mod_time_ticks_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks_us), MP_ROM_PTR(&mod_time_ticks_us_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks_diff), MP_ROM_PTR(&mod_time_ticks_diff_obj) },
    { MP_ROM_QSTR(MP_QSTR_strftime), MP_ROM_PTR(&mod_time_strftime_obj) },
};

STATIC MP_DEFINE_CONST_DICT(mp_module_time_globals, mp_module_time_globals_table);

const mp_obj_module_t mp_module_time = {
    .base = { &mp_type_module },
    .name = MP_QSTR_utime,
    .globals = (mp_obj_dict_t*)&mp_module_time_globals,
};

