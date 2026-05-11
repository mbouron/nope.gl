/*
 * Copyright 2025 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <Python.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "viewer_python.h"

/*
 * Capture the current Python exception into err_buf. Uses traceback.format_exception
 * for a multi-line traceback when available, falling back to str(value). Also prints
 * to stderr for log-tailing. Clears the exception on its way out.
 */
static void capture_py_error(char *err_buf, size_t err_buf_size)
{
    if (!PyErr_Occurred())
        return;

    PyObject *type = NULL, *value = NULL, *tb = NULL;
    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);

    /* Always echo to stderr too so console users keep the existing experience. */
    PyErr_Display(type, value, tb);

    if (!err_buf || err_buf_size == 0) {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(tb);
        return;
    }

    err_buf[0] = '\0';

    PyObject *tb_mod = PyImport_ImportModule("traceback");
    if (tb_mod) {
        PyObject *fmt = PyObject_GetAttrString(tb_mod, "format_exception");
        if (fmt) {
            PyObject *args = Py_BuildValue("(OOO)", type ? type : Py_None,
                                                    value ? value : Py_None,
                                                    tb ? tb : Py_None);
            if (args) {
                PyObject *lines = PyObject_CallObject(fmt, args);
                Py_DECREF(args);
                if (lines && PyList_Check(lines)) {
                    PyObject *empty = PyUnicode_FromString("");
                    PyObject *joined = empty ? PyObject_CallMethod(empty, "join", "O", lines) : NULL;
                    Py_XDECREF(empty);
                    if (joined) {
                        const char *msg = PyUnicode_AsUTF8(joined);
                        if (msg)
                            snprintf(err_buf, err_buf_size, "%s", msg);
                        Py_DECREF(joined);
                    }
                }
                Py_XDECREF(lines);
            }
            Py_DECREF(fmt);
        }
        Py_DECREF(tb_mod);
    }

    /* Fallback if traceback module isn't reachable for some reason. */
    if (err_buf[0] == '\0' && value) {
        PyObject *s = PyObject_Str(value);
        if (s) {
            const char *msg = PyUnicode_AsUTF8(s);
            if (msg)
                snprintf(err_buf, err_buf_size, "%s", msg);
            Py_DECREF(s);
        }
    }

    PyErr_Clear();
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
}

int viewer_python_init(void)
{
    PyConfig config;
    PyConfig_InitPythonConfig(&config);

    /*
     * If a virtual environment is active but the binary lives outside of it
     * (e.g. running from the meson build directory), point program_name at the
     * venv's python so CPython's pyvenv.cfg detection adds the venv
     * site-packages to sys.path.
     */
    const char *venv = getenv("VIRTUAL_ENV");
    if (venv) {
        char exe_path[4096];
#ifdef _WIN32
        snprintf(exe_path, sizeof(exe_path), "%s\\Scripts\\python.exe", venv);
#else
        snprintf(exe_path, sizeof(exe_path), "%s/bin/python", venv);
#endif
        PyStatus status = PyConfig_SetBytesString(&config, &config.program_name, exe_path);
        if (PyStatus_Exception(status)) {
            PyConfig_Clear(&config);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to set Python program_name");
            return -1;
        }
    }

    PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize Python");
        return -1;
    }
    return 0;
}

void viewer_python_uninit(void)
{
    if (Py_IsInitialized())
        Py_Finalize();
}

PyObject *viewer_python_load_module(const char *path)
{
    const size_t len = strlen(path);
    if (len > 3 && SDL_strcasecmp(path + len - 3, ".py") == 0) {
        PyObject *mod_utils  = PyImport_ImportModule("pynopegl_utils.module");
        if (!mod_utils)
            return NULL;
        PyObject *load_func  = PyObject_GetAttrString(mod_utils, "load_script");
        Py_DECREF(mod_utils);
        if (!load_func)
            return NULL;
        PyObject *py_path    = PyUnicode_FromString(path);
        if (!py_path) {
            Py_DECREF(load_func);
            return NULL;
        }
        PyObject *mod = PyObject_CallFunctionObjArgs(load_func, py_path, NULL);
        Py_DECREF(load_func);
        Py_DECREF(py_path);
        return mod;
    }
    return PyImport_ImportModule(path);
}

int viewer_python_list_scenes(const char *path, char ***namesp, size_t *nb_namesp,
                              char *err_buf, size_t err_buf_size)
{
    *namesp = NULL;
    *nb_namesp = 0;

    PyGILState_STATE gstate = PyGILState_Ensure();
    int ret = -1;

    PyObject *mod = viewer_python_load_module(path);
    if (!mod) {
        capture_py_error(err_buf, err_buf_size);
        goto end;
    }

    PyObject *dir_list = PyObject_Dir(mod);
    if (!dir_list) {
        capture_py_error(err_buf, err_buf_size);
        Py_DECREF(mod);
        goto end;
    }

    const Py_ssize_t nb = PyList_Size(dir_list);
    char **names = NULL;
    size_t count = 0;

    for (Py_ssize_t i = 0; i < nb; i++) {
        PyObject *name_obj = PyList_GetItem(dir_list, i); /* Borrowed. */
        const char *name = PyUnicode_AsUTF8(name_obj);
        if (!name)
            continue;

        PyObject *attr = PyObject_GetAttrString(mod, name);
        if (!attr) {
            PyErr_Clear();
            continue;
        }

        PyObject *flag = PyObject_GetAttrString(attr, "iam_a_ngl_scene_func");
        Py_DECREF(attr);
        if (!flag) {
            PyErr_Clear();
            continue;
        }

        const int is_scene = PyObject_IsTrue(flag);
        Py_DECREF(flag);
        if (!is_scene)
            continue;

        char **new_names = SDL_realloc(names, (count + 1) * sizeof(*names));
        if (!new_names) {
            Py_DECREF(dir_list);
            Py_DECREF(mod);
            viewer_python_free_scenes(names, count);
            goto end;
        }
        names = new_names;
        names[count] = SDL_strdup(name);
        if (!names[count]) {
            Py_DECREF(dir_list);
            Py_DECREF(mod);
            viewer_python_free_scenes(names, count);
            goto end;
        }
        count++;
    }

    Py_DECREF(dir_list);
    Py_DECREF(mod);

    *namesp = names;
    *nb_namesp = count;
    ret = 0;

end:
    PyGILState_Release(gstate);
    return ret;
}

void viewer_python_free_scenes(char **names, size_t nb_names)
{
    if (!names)
        return;
    for (size_t i = 0; i < nb_names; i++)
        SDL_free(names[i]);
    SDL_free(names);
}

struct ngl_scene *viewer_python_build_scene(const char *path, const char *func_name,
                                            char *err_buf, size_t err_buf_size)
{
    struct ngl_scene *scene = NULL;
    PyGILState_STATE gstate = PyGILState_Ensure();

    PyObject *mod = viewer_python_load_module(path);
    if (!mod) {
        capture_py_error(err_buf, err_buf_size);
        PyGILState_Release(gstate);
        return NULL;
    }

    PyObject *scene_func = PyObject_GetAttrString(mod, func_name);
    if (!scene_func) {
        capture_py_error(err_buf, err_buf_size);
        Py_DECREF(mod);
        PyGILState_Release(gstate);
        return NULL;
    }

    /*
     * Call the scene function with no arguments so the @ngl.scene decorator
     * creates its own default SceneCfg (which includes backend probing).
     * This matches what python_get_scene() does in python_utils.c.
     */
    PyObject *scene_info = PyObject_CallFunctionObjArgs(scene_func, NULL);
    if (!scene_info) {
        capture_py_error(err_buf, err_buf_size);
        goto end;
    }

    /*
     * Serialize the scene to a string in Python, then deserialize in C.
     * This avoids lifetime issues with the Python scene object and matches
     * the approach used by the ngl-desktop IPC protocol.
     */
    PyObject *py_scene = PyObject_GetAttrString(scene_info, "scene");
    Py_DECREF(scene_info);
    if (!py_scene) {
        capture_py_error(err_buf, err_buf_size);
        goto end;
    }

    PyObject *serialize_ret = PyObject_CallMethod(py_scene, "serialize", NULL);
    Py_DECREF(py_scene);
    if (!serialize_ret) {
        capture_py_error(err_buf, err_buf_size);
        goto end;
    }

    const char *scene_str = PyBytes_AsString(serialize_ret);
    if (!scene_str) {
        Py_DECREF(serialize_ret);
        goto end;
    }

    scene = ngl_scene_create();
    if (!scene) {
        Py_DECREF(serialize_ret);
        goto end;
    }

    int sret = ngl_scene_init_from_str(scene, scene_str);
    Py_DECREF(serialize_ret);
    if (sret < 0) {
        if (err_buf && err_buf_size)
            snprintf(err_buf, err_buf_size, "ngl_scene_init_from_str failed (ret=%d)", sret);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "viewer_python: failed to deserialize scene (ret=%d)", sret);
        ngl_scene_unrefp(&scene);
        goto end;
    }

end:
    Py_XDECREF(scene_func);
    Py_DECREF(mod);
    PyGILState_Release(gstate);
    return scene;
}
