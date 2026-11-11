/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 *
 * This file is available under the terms of either the Apache License,
 * Version 2.0, or the GNU Lesser General Public License, version 2.1 or (at
 * your option) any later version, at your option.
 */

#ifndef NOPE_MEDIA_ANDROID_ASSET_H
#define NOPE_MEDIA_ANDROID_ASSET_H

#include <libavformat/avio.h>

#define NMDI_ANDROID_ASSET_PREFIX "file:///android_asset/"

int nmdi_android_asset_open(void *log_ctx, const char *path, AVIOContext **pbp);
void nmdi_android_asset_close(AVIOContext **pbp);

#endif /* NOPE_MEDIA_ANDROID_ASSET_H */
