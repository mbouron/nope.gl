/*
 * Copyright 2023 Nope Forge
 * Copyright 2017-2022 GoPro Inc.
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

#ifndef NODES_REGISTER_H
#define NODES_REGISTER_H

#define NODE_MAP_TYPE2CLASS(action)                                             \
    action(NGL_NODE_ANIMATEDCOLOR,          ngli_animatedcolor_class)           \
    action(NGL_NODE_ANIMATEDPATH,           ngli_animatedpath_class)            \
    action(NGL_NODE_ANIMATEDTIME,           ngli_animatedtime_class)            \
    action(NGL_NODE_ANIMATEDFLOAT,          ngli_animatedfloat_class)           \
    action(NGL_NODE_ANIMATEDVEC2,           ngli_animatedvec2_class)            \
    action(NGL_NODE_ANIMATEDVEC3,           ngli_animatedvec3_class)            \
    action(NGL_NODE_ANIMATEDVEC4,           ngli_animatedvec4_class)            \
    action(NGL_NODE_ANIMATEDQUAT,           ngli_animatedquat_class)            \
    action(NGL_NODE_ANIMKEYFRAMEFLOAT,      ngli_animkeyframefloat_class)       \
    action(NGL_NODE_ANIMKEYFRAMEVEC2,       ngli_animkeyframevec2_class)        \
    action(NGL_NODE_ANIMKEYFRAMEVEC3,       ngli_animkeyframevec3_class)        \
    action(NGL_NODE_ANIMKEYFRAMEVEC4,       ngli_animkeyframevec4_class)        \
    action(NGL_NODE_ANIMKEYFRAMEQUAT,       ngli_animkeyframequat_class)        \
    action(NGL_NODE_ANIMKEYFRAMECOLOR,      ngli_animkeyframecolor_class)       \
    action(NGL_NODE_BLOCK,                  ngli_block_class)                   \
    action(NGL_NODE_BUFFERBYTE,             ngli_bufferbyte_class)              \
    action(NGL_NODE_BUFFERBVEC2,            ngli_bufferbvec2_class)             \
    action(NGL_NODE_BUFFERBVEC3,            ngli_bufferbvec3_class)             \
    action(NGL_NODE_BUFFERBVEC4,            ngli_bufferbvec4_class)             \
    action(NGL_NODE_BUFFERINT,              ngli_bufferint_class)               \
    action(NGL_NODE_BUFFERINT64,            ngli_bufferint64_class)             \
    action(NGL_NODE_BUFFERIVEC2,            ngli_bufferivec2_class)             \
    action(NGL_NODE_BUFFERIVEC3,            ngli_bufferivec3_class)             \
    action(NGL_NODE_BUFFERIVEC4,            ngli_bufferivec4_class)             \
    action(NGL_NODE_BUFFERSHORT,            ngli_buffershort_class)             \
    action(NGL_NODE_BUFFERSVEC2,            ngli_buffersvec2_class)             \
    action(NGL_NODE_BUFFERSVEC3,            ngli_buffersvec3_class)             \
    action(NGL_NODE_BUFFERSVEC4,            ngli_buffersvec4_class)             \
    action(NGL_NODE_BUFFERUBYTE,            ngli_bufferubyte_class)             \
    action(NGL_NODE_BUFFERUBVEC2,           ngli_bufferubvec2_class)            \
    action(NGL_NODE_BUFFERUBVEC3,           ngli_bufferubvec3_class)            \
    action(NGL_NODE_BUFFERUBVEC4,           ngli_bufferubvec4_class)            \
    action(NGL_NODE_BUFFERUINT,             ngli_bufferuint_class)              \
    action(NGL_NODE_BUFFERUIVEC2,           ngli_bufferuivec2_class)            \
    action(NGL_NODE_BUFFERUIVEC3,           ngli_bufferuivec3_class)            \
    action(NGL_NODE_BUFFERUIVEC4,           ngli_bufferuivec4_class)            \
    action(NGL_NODE_BUFFERUSHORT,           ngli_bufferushort_class)            \
    action(NGL_NODE_BUFFERUSVEC2,           ngli_bufferusvec2_class)            \
    action(NGL_NODE_BUFFERUSVEC3,           ngli_bufferusvec3_class)            \
    action(NGL_NODE_BUFFERUSVEC4,           ngli_bufferusvec4_class)            \
    action(NGL_NODE_BUFFERFLOAT,            ngli_bufferfloat_class)             \
    action(NGL_NODE_BUFFERVEC2,             ngli_buffervec2_class)              \
    action(NGL_NODE_BUFFERVEC3,             ngli_buffervec3_class)              \
    action(NGL_NODE_BUFFERVEC4,             ngli_buffervec4_class)              \
    action(NGL_NODE_BUFFERMAT4,             ngli_buffermat4_class)              \
    action(NGL_NODE_COMPUTE,                ngli_compute_class)                 \
    action(NGL_NODE_COMPUTEPROGRAM,         ngli_computeprogram_class)          \
    action(NGL_NODE_CUSTOMTEXTURE,          ngli_customtexture_class)           \
    action(NGL_NODE_COLORFILL,              ngli_colorfill_class)               \
    action(NGL_NODE_CUSTOMFILL,             ngli_customfill_class)              \
    action(NGL_NODE_DRAWRECT,               ngli_drawrect_class)                \
    action(NGL_NODE_FASTGAUSSIANBLUR,       ngli_fgblur_class)                  \
    action(NGL_NODE_GRADIENT4FILL,          ngli_gradient4fill_class)           \
    action(NGL_NODE_GRADIENTFILL,           ngli_gradientfill_class)            \
    action(NGL_NODE_NOISEFILL,              ngli_noisefill_class)               \
    action(NGL_NODE_STROKE,                 ngli_stroke_class)                  \
    action(NGL_NODE_STROKEGRADIENT,         ngli_strokegradient_class)          \
    action(NGL_NODE_STROKEGRADIENT4,        ngli_strokegradient4_class)         \
    action(NGL_NODE_TEXTUREFILL,            ngli_texturefill_class)             \
    action(NGL_NODE_GAUSSIANBLUR,           ngli_gblur_class)                   \
    action(NGL_NODE_GROUP,                  ngli_group_class)                   \
    action(NGL_NODE_HEXAGONALBLUR,          ngli_hblur_class)                   \
    action(NGL_NODE_IDENTITY,               ngli_identity_class)                \
    action(NGL_NODE_IOINT,                  ngli_ioint_class)                   \
    action(NGL_NODE_IOIVEC2,                ngli_ioivec2_class)                 \
    action(NGL_NODE_IOIVEC3,                ngli_ioivec3_class)                 \
    action(NGL_NODE_IOIVEC4,                ngli_ioivec4_class)                 \
    action(NGL_NODE_IOUINT,                 ngli_iouint_class)                  \
    action(NGL_NODE_IOUIVEC2,               ngli_iouivec2_class)                \
    action(NGL_NODE_IOUIVEC3,               ngli_iouivec3_class)                \
    action(NGL_NODE_IOUIVEC4,               ngli_iouivec4_class)                \
    action(NGL_NODE_IOFLOAT,                ngli_iofloat_class)                 \
    action(NGL_NODE_IOVEC2,                 ngli_iovec2_class)                  \
    action(NGL_NODE_IOVEC3,                 ngli_iovec3_class)                  \
    action(NGL_NODE_IOVEC4,                 ngli_iovec4_class)                  \
    action(NGL_NODE_IOMAT3,                 ngli_iomat3_class)                  \
    action(NGL_NODE_IOMAT4,                 ngli_iomat4_class)                  \
    action(NGL_NODE_IOBOOL,                 ngli_iobool_class)                  \
    action(NGL_NODE_EVALFLOAT,              ngli_evalfloat_class)               \
    action(NGL_NODE_EVALVEC2,               ngli_evalvec2_class)                \
    action(NGL_NODE_EVALVEC3,               ngli_evalvec3_class)                \
    action(NGL_NODE_EVALVEC4,               ngli_evalvec4_class)                \
    action(NGL_NODE_MEDIA,                  ngli_media_class)                   \
    action(NGL_NODE_NOISEFLOAT,             ngli_noisefloat_class)              \
    action(NGL_NODE_NOISEVEC2,              ngli_noisevec2_class)               \
    action(NGL_NODE_NOISEVEC3,              ngli_noisevec3_class)               \
    action(NGL_NODE_NOISEVEC4,              ngli_noisevec4_class)               \
    action(NGL_NODE_PROGRAM,                ngli_program_class)                 \
    action(NGL_NODE_RENDERTOTEXTURE,        ngli_rtt_class)                     \
    action(NGL_NODE_RESOURCEPROPS,          ngli_resourceprops_class)           \
    action(NGL_NODE_ROTATE,                 ngli_rotate_class)                  \
    action(NGL_NODE_ROTATEQUAT,             ngli_rotatequat_class)              \
    action(NGL_NODE_SCALE,                  ngli_scale_class)                   \
    action(NGL_NODE_SKEW,                   ngli_skew_class)                    \
    action(NGL_NODE_SMOOTHPATH,             ngli_smoothpath_class)              \
    action(NGL_NODE_TEXTURE2D,              ngli_texture2d_class)               \
    action(NGL_NODE_TEXTURE2DARRAY,         ngli_texture2darray_class)          \
    action(NGL_NODE_TEXTURE3D,              ngli_texture3d_class)               \
    action(NGL_NODE_TEXTURECUBE,            ngli_texturecube_class)             \
    action(NGL_NODE_TEXTUREVIEW,            ngli_textureview_class)             \
    action(NGL_NODE_TIME,                   ngli_time_class)                    \
    action(NGL_NODE_TIMERANGEFILTER,        ngli_timerangefilter_class)         \
    action(NGL_NODE_TRANSFORM,              ngli_transform_class)               \
    action(NGL_NODE_TRANSLATE,              ngli_translate_class)               \
    action(NGL_NODE_UNIFORMBOOL,            ngli_uniformbool_class)             \
    action(NGL_NODE_UNIFORMINT,             ngli_uniformint_class)              \
    action(NGL_NODE_UNIFORMIVEC2,           ngli_uniformivec2_class)            \
    action(NGL_NODE_UNIFORMIVEC3,           ngli_uniformivec3_class)            \
    action(NGL_NODE_UNIFORMIVEC4,           ngli_uniformivec4_class)            \
    action(NGL_NODE_UNIFORMUINT,            ngli_uniformuint_class)             \
    action(NGL_NODE_UNIFORMUIVEC2,          ngli_uniformuivec2_class)           \
    action(NGL_NODE_UNIFORMUIVEC3,          ngli_uniformuivec3_class)           \
    action(NGL_NODE_UNIFORMUIVEC4,          ngli_uniformuivec4_class)           \
    action(NGL_NODE_UNIFORMMAT4,            ngli_uniformmat4_class)             \
    action(NGL_NODE_UNIFORMFLOAT,           ngli_uniformfloat_class)            \
    action(NGL_NODE_UNIFORMVEC2,            ngli_uniformvec2_class)             \
    action(NGL_NODE_UNIFORMVEC3,            ngli_uniformvec3_class)             \
    action(NGL_NODE_UNIFORMVEC4,            ngli_uniformvec4_class)             \
    action(NGL_NODE_UNIFORMCOLOR,           ngli_uniformcolor_class)            \
    action(NGL_NODE_UNIFORMQUAT,            ngli_uniformquat_class)             \
    action(NGL_NODE_USERSELECT,             ngli_userselect_class)              \
    action(NGL_NODE_USERSWITCH,             ngli_userswitch_class)              \

#endif
