/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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
#include <nopegl/nopegl.h>

#include "viewer_hooks.h"
#include "viewer_python.h"

/*
 * Compute a per-file identifier that mixes the local path, size and mtime,
 * matching the previous Qt viewer's _hash_filename() so existing hook scripts
 * (e.g. android/hooks/android.py) keep behaving the same. The result is
 * "<sha256-hex><ext>", e.g. "ab12...cd.mp4".
 */
static const char *HASH_FILENAME_PY_SRC =
    "import os\n"
    "import os.path\n"
    "import hashlib\n"
    "def _hash_filename(filename):\n"
    "    statinfo = os.stat(filename)\n"
    "    sha = hashlib.sha256()\n"
    "    sha.update(filename.encode())\n"
    "    sha.update(str(statinfo.st_size).encode())\n"
    "    sha.update(str(statinfo.st_mtime).encode())\n"
    "    _, ext = os.path.splitext(filename)\n"
    "    return sha.hexdigest() + ext\n";

struct hooks_ctx {
    PyObject *module;
    PyObject *get_sessions_func;
    PyObject *get_session_info_func;
    PyObject *scene_change_func;
    PyObject *sync_file_func;
    PyObject *hash_filename_func;
};

static PyObject *compile_hash_filename_func(void)
{
    PyObject *globals = PyDict_New();
    if (!globals)
        return NULL;
    if (PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) < 0) {
        Py_DECREF(globals);
        return NULL;
    }
    PyObject *r = PyRun_String(HASH_FILENAME_PY_SRC, Py_file_input, globals, globals);
    if (!r) {
        Py_DECREF(globals);
        return NULL;
    }
    Py_DECREF(r);
    PyObject *func = PyDict_GetItemString(globals, "_hash_filename"); /* Borrowed. */
    Py_XINCREF(func);
    Py_DECREF(globals);
    return func;
}

struct hooks_ctx *hooks_create(const char *script_path)
{
    struct hooks_ctx *s = SDL_calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->module = viewer_python_load_module(script_path);
    if (!s->module) {
        if (PyErr_Occurred())
            PyErr_PrintEx(0);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "hooks: failed to load module '%s'", script_path);
        goto fail;
    }

    s->get_sessions_func = PyObject_GetAttrString(s->module, "get_sessions");
    s->get_session_info_func = PyObject_GetAttrString(s->module, "get_session_info");
    s->scene_change_func = PyObject_GetAttrString(s->module, "scene_change");
    s->sync_file_func = PyObject_GetAttrString(s->module, "sync_file");

    if (!s->get_sessions_func || !s->scene_change_func) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "hooks: module '%s' missing required functions (get_sessions, scene_change)",
                     script_path);
        PyErr_Clear();
        goto fail;
    }

    PyErr_Clear(); /* Clear any errors from optional missing functions. */

    s->hash_filename_func = compile_hash_filename_func();
    if (!s->hash_filename_func) {
        if (PyErr_Occurred())
            PyErr_PrintEx(0);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "hooks: failed to compile hash_filename helper");
        goto fail;
    }
    return s;

fail:
    hooks_freep(&s);
    return NULL;
}

int hooks_get_sessions(struct hooks_ctx *s, struct hooks_session **sessionsp, size_t *nb_sessionsp)
{
    *sessionsp = NULL;
    *nb_sessionsp = 0;

    if (!s->get_sessions_func)
        return -1;

    PyObject *result = PyObject_CallNoArgs(s->get_sessions_func);
    if (!result) {
        if (PyErr_Occurred())
            PyErr_PrintEx(0);
        return -1;
    }

    if (!PyList_Check(result)) {
        Py_DECREF(result);
        return -1;
    }

    const Py_ssize_t nb = PyList_Size(result);
    struct hooks_session *sessions = NULL;
    if (nb > 0) {
        sessions = SDL_calloc((size_t)nb, sizeof(*sessions));
        if (!sessions) {
            Py_DECREF(result);
            return -1;
        }
    }

    size_t count = 0;
    for (Py_ssize_t i = 0; i < nb; i++) {
        PyObject *item = PyList_GetItem(result, i); /* Borrowed. */
        Py_ssize_t item_len = 0;
        if (PyTuple_Check(item))
            item_len = PyTuple_Size(item);
        else if (PyList_Check(item))
            item_len = PyList_Size(item);
        else
            continue;
        if (item_len < 2)
            continue;

        PyObject *id_obj = PyTuple_Check(item) ? PyTuple_GetItem(item, 0) : PyList_GetItem(item, 0);
        PyObject *desc_obj = PyTuple_Check(item) ? PyTuple_GetItem(item, 1) : PyList_GetItem(item, 1);
        const char *id = PyUnicode_AsUTF8(id_obj);
        const char *desc = PyUnicode_AsUTF8(desc_obj);
        if (!id || !desc)
            continue;

        sessions[count].id = SDL_strdup(id);
        sessions[count].description = SDL_strdup(desc);
        if (!sessions[count].id || !sessions[count].description) {
            hooks_free_sessions(sessions, count + 1);
            Py_DECREF(result);
            return -1;
        }
        count++;
    }

    Py_DECREF(result);
    *sessionsp = sessions;
    *nb_sessionsp = count;
    return 0;
}

void hooks_free_sessions(struct hooks_session *sessions, size_t nb_sessions)
{
    if (!sessions)
        return;
    for (size_t i = 0; i < nb_sessions; i++) {
        SDL_free(sessions[i].id);
        SDL_free(sessions[i].description);
    }
    SDL_free(sessions);
}

/*
 * Upload one local asset via the user hook's sync_file(session_id, ifile, ofile)
 * and update the scene's filepath at `index` to whatever path the hook returns
 * (typically a session-specific remote path, e.g. on an Android device).
 */
static int sync_and_rewrite_filepath(struct hooks_ctx *s, const char *session_id,
                                     struct ngl_scene *scene, size_t index,
                                     const char *localfile)
{
    if (!s->sync_file_func) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "hooks: scene references '%s' but module has no sync_file()", localfile);
        return -1;
    }

    PyObject *py_local = PyUnicode_FromString(localfile);
    if (!py_local)
        return -1;

    PyObject *py_hashed = PyObject_CallFunctionObjArgs(s->hash_filename_func, py_local, NULL);
    if (!py_hashed) {
        if (PyErr_Occurred())
            PyErr_PrintEx(0);
        Py_DECREF(py_local);
        return -1;
    }

    PyObject *py_session = PyUnicode_FromString(session_id);
    if (!py_session) {
        Py_DECREF(py_local);
        Py_DECREF(py_hashed);
        return -1;
    }

    PyObject *result = PyObject_CallFunctionObjArgs(
        s->sync_file_func, py_session, py_local, py_hashed, NULL);
    Py_DECREF(py_session);
    Py_DECREF(py_local);
    Py_DECREF(py_hashed);
    if (!result) {
        if (PyErr_Occurred())
            PyErr_PrintEx(0);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "hooks: sync_file failed for '%s'", localfile);
        return -1;
    }

    const char *remotefile = PyUnicode_AsUTF8(result);
    if (!remotefile) {
        Py_DECREF(result);
        return -1;
    }

    int ret = ngl_scene_update_filepath(scene, index, remotefile);
    Py_DECREF(result);
    if (ret < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "hooks: ngl_scene_update_filepath(%zu) failed (ret=%d)", index, ret);
        return -1;
    }
    return 0;
}

int hooks_scene_change(struct hooks_ctx *s, const char *session_id,
                       struct ngl_scene *scene, uint32_t clear_color, int samples)
{
    if (!s->scene_change_func || !scene)
        return -1;

    char **filepaths = NULL;
    size_t nb_filepaths = 0;
    int gret = ngl_scene_get_filepaths(scene, &filepaths, &nb_filepaths);
    if (gret < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "hooks: ngl_scene_get_filepaths failed (ret=%d)", gret);
        return -1;
    }
    for (size_t i = 0; i < nb_filepaths; i++) {
        if (sync_and_rewrite_filepath(s, session_id, scene, i, filepaths[i]) < 0)
            return -1;
    }

    char *scene_str = ngl_scene_serialize(scene);
    if (!scene_str)
        return -1;

    char *prefpath = SDL_GetPrefPath("nopeforge", "ngl-viewer");
    if (!prefpath) {
        free(scene_str);
        return -1;
    }
    char tmppath[512];
    int r = SDL_snprintf(tmppath, sizeof(tmppath), "%sscene-%" SDL_PRIu64 ".ngl",
                         prefpath, SDL_GetPerformanceCounter());
    SDL_free(prefpath);
    if (r < 0 || (size_t)r >= sizeof(tmppath)) {
        free(scene_str);
        return -1;
    }

    const bool saved = SDL_SaveFile(tmppath, scene_str, strlen(scene_str));
    /* scene_str was allocated by libnopegl with the standard allocator. */
    free(scene_str);
    if (!saved)
        return -1;

    PyObject *py_session = PyUnicode_FromString(session_id);
    PyObject *py_scenefile = PyUnicode_FromString(tmppath);
    PyObject *py_clear_color = PyLong_FromUnsignedLong(clear_color);
    PyObject *py_samples = PyLong_FromLong(samples);

    int ret = 0;
    /* PyObject_CallFunctionObjArgs treats the first NULL as end-of-args, so a
     * failed constructor would silently truncate the call. Bail explicitly. */
    if (!py_session || !py_scenefile || !py_clear_color || !py_samples) {
        if (PyErr_Occurred())
            PyErr_PrintEx(0);
        ret = -1;
    } else {
        PyObject *result = PyObject_CallFunctionObjArgs(
            s->scene_change_func, py_session, py_scenefile, py_clear_color, py_samples, NULL);
        if (!result) {
            if (PyErr_Occurred())
                PyErr_PrintEx(0);
            ret = -1;
        } else {
            Py_DECREF(result);
        }
    }

    Py_XDECREF(py_session);
    Py_XDECREF(py_scenefile);
    Py_XDECREF(py_clear_color);
    Py_XDECREF(py_samples);

    SDL_RemovePath(tmppath);
    return ret;
}

void hooks_freep(struct hooks_ctx **sp)
{
    struct hooks_ctx *s = *sp;
    if (!s)
        return;

    Py_XDECREF(s->hash_filename_func);
    Py_XDECREF(s->sync_file_func);
    Py_XDECREF(s->scene_change_func);
    Py_XDECREF(s->get_session_info_func);
    Py_XDECREF(s->get_sessions_func);
    Py_XDECREF(s->module);
    SDL_free(s);
    *sp = NULL;
}
