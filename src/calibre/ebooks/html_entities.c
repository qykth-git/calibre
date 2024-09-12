/*
 * html_entities.cpp
 * Copyright (C) 2024 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#define PY_SSIZE_T_CLEAN
#define UNICODE
#define _UNICODE
#include <Python.h>
#include <stdbool.h>
#include "html_entities.h"

unsigned int
encode_utf8(uint32_t ch, char* dest) {
    if (ch < 0x80) { // only lower 7 bits can be 1
        dest[0] = (char)ch;  // 0xxxxxxx
        return 1;
    }
    if (ch < 0x800) { // only lower 11 bits can be 1
        dest[0] = (ch>>6) | 0xC0; // 110xxxxx
        dest[1] = (ch & 0x3F) | 0x80;  // 10xxxxxx
        return 2;
    }
    if (ch < 0x10000) { // only lower 16 bits can be 1
        dest[0] = (ch>>12) | 0xE0; // 1110xxxx
        dest[1] = ((ch>>6) & 0x3F) | 0x80;  // 10xxxxxx
        dest[2] = (ch & 0x3F) | 0x80;       // 10xxxxxx
        return 3;
    }
    if (ch < 0x110000) { // only lower 21 bits can be 1
        dest[0] = (ch>>18) | 0xF0; // 11110xxx
        dest[1] = ((ch>>12) & 0x3F) | 0x80; // 10xxxxxx
        dest[2] = ((ch>>6) & 0x3F) | 0x80;  // 10xxxxxx
        dest[3] = (ch & 0x3F) | 0x80; // 10xxxxxx
        return 4;
    }
    return 0;
}

static uint32_t
parse_base10_integer(const char *input, size_t sz, bool *ok) {
    uint32_t ans = 0;
    *ok = true;
    for (size_t i = 0; i < sz; i++) {
        char ch = input[i];
        if (ch < '0' || ch > '9') { *ok = false; return 0; }
        uint32_t digit = ch - '0';
        ans = ans * 10 + digit;
    }
    return ans;
}

static uint32_t
parse_base16_integer(const char *input, size_t sz, bool *ok) {
    uint32_t ans = 0;
    *ok = true;
    for (size_t i = 0; i < sz; i++) {
        char ch = input[i];
        uint32_t digit;
        if ('a' <= ch && ch <= 'f') digit = 10 + ch - 'a';
        else if ('A' <= ch && ch <= 'F') digit = 10 + ch - 'A';
        else {
            if (ch < '0' || ch > '9') { *ok = false; return 0; }
            digit = ch - '0';
        }
        ans = ans * 10 + digit;
    }
    return ans;
}

static size_t
add_entity(const char *entity, const size_t elen, char *output) {
    size_t ans = 0;
    if (elen > 64) {
bad_entity:
        output[ans++] = '&';
        memcpy(output + ans, entity, elen);
        ans += elen;
        output[ans++] = ';';
        return ans;
    }
    if (!elen) {
        output[ans++] = '&';
        output[ans++] = ';';
        return ans;
    }
    if (entity[0] == '#') {
        if (elen < 2) goto bad_entity;
        uint32_t codepoint = 0;
        bool ok;
        if (entity[1] == 'x' || entity[1] == 'X') {
            if (elen < 3) goto bad_entity;
            codepoint = parse_base16_integer(entity + 2, elen - 2, &ok);
            if (!ok || !codepoint) goto bad_entity;
        } else {
            codepoint = parse_base10_integer(entity + 1, elen - 1, &ok);
            if (!ok || !codepoint) goto bad_entity;
        }
        unsigned num = encode_utf8(codepoint, output);
        if (!num) goto bad_entity;
        return num;
    } else {
        struct html_entity *s = in_word_set(entity, elen);
        if (!s) goto bad_entity;
        ans = strlen(s->val);
        memcpy(output, s->val, ans);
        return ans;
    }
    goto bad_entity;
}


static size_t
process_entity(const char *input, size_t input_sz, char *output, size_t *output_pos) {
    size_t input_pos = 0;
    while (input_pos < input_sz) {
        char ch = input[input_pos++];
        if (('a' <= ch && ch <= 'z') || ('A' <= ch && ch <= 'Z') || ('0' <= ch && ch <= '9') || (ch == '#' && input_pos == 1));
        else if (ch == ';') *output_pos += add_entity(input, input_pos-1, output + *output_pos);
        else {
            output[(*output_pos)++] = '&';
            memcpy(output + *output_pos, input, input_pos);
            *output_pos += input_pos;
        }
    }
    return input_pos;
}

static size_t
replace(const char *input, size_t input_sz, char *output, int keep_xml_entities) {
    size_t input_pos = 0, output_pos = 0;
    while (input_pos < input_sz) {
        const char *p = (const char*)memchr(input + input_pos, '&', input_sz - input_pos);
        if (p) {
            if (p > input + input_pos) {
                size_t sz = p - (input + input_pos);
                memcpy(output + output_pos, input + input_pos, sz);
                output_pos += sz;
                input_pos += sz;
            }
            input_pos += process_entity(p, input_sz - (p - input), output, &output_pos);
        } else {
            memcpy(output + output_pos, input + input_pos, input_sz - input_pos);
            output_pos += input_sz - input_pos;
            input_pos = input_sz;
        }
    }
    return output_pos;
}

static PyObject*
replace_entities(PyObject *self, PyObject *const *args, Py_ssize_t nargs) {
    if (nargs < 1) { PyErr_SetString(PyExc_TypeError, "Must specify string tp process"); return NULL; }
    const char *input = NULL; Py_ssize_t input_sz = 0;
    int keep_xml_entities = false;
    if (PyUnicode_Check(args[0])) {
        input = PyUnicode_AsUTF8AndSize(args[0], &input_sz);
        if (!input) return NULL;
    } else if (PyBytes_Check(args[0])) {
        input = PyBytes_AS_STRING(args[0]); input_sz = PyBytes_GET_SIZE(args[0]);
    } else {
        PyErr_SetString(PyExc_TypeError, "string must be unicode object or UTF-8 encoded bytes"); return NULL;
    }
    if (nargs > 1) keep_xml_entities = PyObject_IsTrue(args[1]);
    char *output = malloc(input_sz + 1);
    if (!output) { return PyErr_NoMemory(); }
    size_t output_sz = replace(input, input_sz, output, keep_xml_entities);
    PyObject *retval;
    if (PyErr_Occurred()) retval = NULL;
    else if (!output_sz) retval = Py_NewRef(args[0]);
    else if (PyUnicode_Check(args[0])) retval = PyUnicode_FromStringAndSize(output, output_sz);
    else retval = PyBytes_FromStringAndSize(output, output_sz);
    free(output);
    return retval;
}

static PyMethodDef methods[] = {
    {"replace_entities", (PyCFunction)replace_entities, METH_FASTCALL,
     "Replace entities in the specified string"
    },
    {NULL, NULL, 0, NULL}
};

static int
exec_module(PyObject *m) {
    return 0;
}

CALIBRE_MODINIT_FUNC PyInit_fast_html_entities(void) {
    static PyModuleDef_Slot slots[] = { {Py_mod_exec, (void*)exec_module}, {0, NULL} };
    static struct PyModuleDef module_def = {PyModuleDef_HEAD_INIT};

    module_def.m_name     = "fast_html_entities";
    module_def.m_doc      = "Fast conversion of HTML entities";
    module_def.m_methods  = methods;
    module_def.m_slots    = slots;
	return PyModuleDef_Init(&module_def);
}
