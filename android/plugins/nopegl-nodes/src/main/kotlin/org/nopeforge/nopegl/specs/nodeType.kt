/*
 * Copyright 2024 Nope Forge
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

package org.nopeforge.nopegl.specs

import org.nopeforge.nopegl.NGLNodeType

fun nodeType(name: String): NGLNodeType? {
    return when (name) {
        "AnimatedBufferFloat" -> NGLNodeType.ANIMATEDBUFFERFLOAT
        "AnimatedBufferVec2" -> NGLNodeType.ANIMATEDBUFFERVEC2
        "AnimatedBufferVec3" -> NGLNodeType.ANIMATEDBUFFERVEC3
        "AnimatedBufferVec4" -> NGLNodeType.ANIMATEDBUFFERVEC4
        "AnimatedColor" -> NGLNodeType.ANIMATEDCOLOR
        "AnimatedPath" -> NGLNodeType.ANIMATEDPATH
        "AnimatedTime" -> NGLNodeType.ANIMATEDTIME
        "AnimatedFloat" -> NGLNodeType.ANIMATEDFLOAT
        "AnimatedVec2" -> NGLNodeType.ANIMATEDVEC2
        "AnimatedVec3" -> NGLNodeType.ANIMATEDVEC3
        "AnimatedVec4" -> NGLNodeType.ANIMATEDVEC4
        "AnimatedQuat" -> NGLNodeType.ANIMATEDQUAT
        "AnimKeyFrameFloat" -> NGLNodeType.ANIMKEYFRAMEFLOAT
        "AnimKeyFrameVec2" -> NGLNodeType.ANIMKEYFRAMEVEC2
        "AnimKeyFrameVec3" -> NGLNodeType.ANIMKEYFRAMEVEC3
        "AnimKeyFrameVec4" -> NGLNodeType.ANIMKEYFRAMEVEC4
        "AnimKeyFrameQuat" -> NGLNodeType.ANIMKEYFRAMEQUAT
        "AnimKeyFrameColor" -> NGLNodeType.ANIMKEYFRAMECOLOR
        "AnimKeyFrameBuffer" -> NGLNodeType.ANIMKEYFRAMEBUFFER
        "Block" -> NGLNodeType.BLOCK
        "BufferByte" -> NGLNodeType.BUFFERBYTE
        "BufferBVec2" -> NGLNodeType.BUFFERBVEC2
        "BufferBVec3" -> NGLNodeType.BUFFERBVEC3
        "BufferBVec4" -> NGLNodeType.BUFFERBVEC4
        "BufferInt" -> NGLNodeType.BUFFERINT
        "BufferInt64" -> NGLNodeType.BUFFERINT64
        "BufferIVec2" -> NGLNodeType.BUFFERIVEC2
        "BufferIVec3" -> NGLNodeType.BUFFERIVEC3
        "BufferIVec4" -> NGLNodeType.BUFFERIVEC4
        "BufferShort" -> NGLNodeType.BUFFERSHORT
        "BufferSVec2" -> NGLNodeType.BUFFERSVEC2
        "BufferSVec3" -> NGLNodeType.BUFFERSVEC3
        "BufferSVec4" -> NGLNodeType.BUFFERSVEC4
        "BufferUByte" -> NGLNodeType.BUFFERUBYTE
        "BufferUBVec2" -> NGLNodeType.BUFFERUBVEC2
        "BufferUBVec3" -> NGLNodeType.BUFFERUBVEC3
        "BufferUBVec4" -> NGLNodeType.BUFFERUBVEC4
        "BufferUInt" -> NGLNodeType.BUFFERUINT
        "BufferUIVec2" -> NGLNodeType.BUFFERUIVEC2
        "BufferUIVec3" -> NGLNodeType.BUFFERUIVEC3
        "BufferUIVec4" -> NGLNodeType.BUFFERUIVEC4
        "BufferUShort" -> NGLNodeType.BUFFERUSHORT
        "BufferUSVec2" -> NGLNodeType.BUFFERUSVEC2
        "BufferUSVec3" -> NGLNodeType.BUFFERUSVEC3
        "BufferUSVec4" -> NGLNodeType.BUFFERUSVEC4
        "BufferFloat" -> NGLNodeType.BUFFERFLOAT
        "BufferVec2" -> NGLNodeType.BUFFERVEC2
        "BufferVec3" -> NGLNodeType.BUFFERVEC3
        "BufferVec4" -> NGLNodeType.BUFFERVEC4
        "BufferMat4" -> NGLNodeType.BUFFERMAT4
        "Camera" -> NGLNodeType.CAMERA
        "Circle" -> NGLNodeType.CIRCLE
        "ColorKey" -> NGLNodeType.COLORKEY
        "ColorStats" -> NGLNodeType.COLORSTATS
        "Compute" -> NGLNodeType.COMPUTE
        "ComputeProgram" -> NGLNodeType.COMPUTEPROGRAM
        "Draw" -> NGLNodeType.DRAW
        "DrawColor" -> NGLNodeType.DRAWCOLOR
        "DrawDisplace" -> NGLNodeType.DRAWDISPLACE
        "DrawGradient" -> NGLNodeType.DRAWGRADIENT
        "DrawGradient4" -> NGLNodeType.DRAWGRADIENT4
        "DrawHistogram" -> NGLNodeType.DRAWHISTOGRAM
        "DrawMask" -> NGLNodeType.DRAWMASK
        "DrawNoise" -> NGLNodeType.DRAWNOISE
        "DrawPath" -> NGLNodeType.DRAWPATH
        "DrawTexture" -> NGLNodeType.DRAWTEXTURE
        "DrawWaveform" -> NGLNodeType.DRAWWAVEFORM
        "FilterAlpha" -> NGLNodeType.FILTERALPHA
        "FilterColorMap" -> NGLNodeType.FILTERCOLORMAP
        "FilterContrast" -> NGLNodeType.FILTERCONTRAST
        "FilterExposure" -> NGLNodeType.FILTEREXPOSURE
        "FilterInverseAlpha" -> NGLNodeType.FILTERINVERSEALPHA
        "FilterLinear2sRGB" -> NGLNodeType.FILTERLINEAR2SRGB
        "FilterOpacity" -> NGLNodeType.FILTEROPACITY
        "FilterPremult" -> NGLNodeType.FILTERPREMULT
        "FilterSaturation" -> NGLNodeType.FILTERSATURATION
        "FilterSelector" -> NGLNodeType.FILTERSELECTOR
        "FilterSRGB2Linear" -> NGLNodeType.FILTERSRGB2LINEAR
        "FastGaussianBlur" -> NGLNodeType.FASTGAUSSIANBLUR
        "FontFace" -> NGLNodeType.FONTFACE
        "GaussianBlur" -> NGLNodeType.GAUSSIANBLUR
        "Geometry" -> NGLNodeType.GEOMETRY
        "GraphicConfig" -> NGLNodeType.GRAPHICCONFIG
        "GridLayout" -> NGLNodeType.GRIDLAYOUT
        "Group" -> NGLNodeType.GROUP
        "HexagonalBlur" -> NGLNodeType.HEXAGONALBLUR
        "Identity" -> NGLNodeType.IDENTITY
        "IOInt" -> NGLNodeType.IOINT
        "IOIVec2" -> NGLNodeType.IOIVEC2
        "IOIVec3" -> NGLNodeType.IOIVEC3
        "IOIVec4" -> NGLNodeType.IOIVEC4
        "IOUInt" -> NGLNodeType.IOUINT
        "IOUIvec2" -> NGLNodeType.IOUIVEC2
        "IOUIvec3" -> NGLNodeType.IOUIVEC3
        "IOUIvec4" -> NGLNodeType.IOUIVEC4
        "IOFloat" -> NGLNodeType.IOFLOAT
        "IOVec2" -> NGLNodeType.IOVEC2
        "IOVec3" -> NGLNodeType.IOVEC3
        "IOVec4" -> NGLNodeType.IOVEC4
        "IOMat3" -> NGLNodeType.IOMAT3
        "IOMat4" -> NGLNodeType.IOMAT4
        "IOBool" -> NGLNodeType.IOBOOL
        "EvalFloat" -> NGLNodeType.EVALFLOAT
        "EvalVec2" -> NGLNodeType.EVALVEC2
        "EvalVec3" -> NGLNodeType.EVALVEC3
        "EvalVec4" -> NGLNodeType.EVALVEC4
        "Media" -> NGLNodeType.MEDIA
        "NoiseFloat" -> NGLNodeType.NOISEFLOAT
        "NoiseVec2" -> NGLNodeType.NOISEVEC2
        "NoiseVec3" -> NGLNodeType.NOISEVEC3
        "NoiseVec4" -> NGLNodeType.NOISEVEC4
        "Path" -> NGLNodeType.PATH
        "PathKeyBezier2" -> NGLNodeType.PATHKEYBEZIER2
        "PathKeyBezier3" -> NGLNodeType.PATHKEYBEZIER3
        "PathKeyClose" -> NGLNodeType.PATHKEYCLOSE
        "PathKeyLine" -> NGLNodeType.PATHKEYLINE
        "PathKeyMove" -> NGLNodeType.PATHKEYMOVE
        "Program" -> NGLNodeType.PROGRAM
        "Quad" -> NGLNodeType.QUAD
        "RenderToTexture" -> NGLNodeType.RENDERTOTEXTURE
        "ResourceProps" -> NGLNodeType.RESOURCEPROPS
        "Rotate" -> NGLNodeType.ROTATE
        "RotateQuat" -> NGLNodeType.ROTATEQUAT
        "Scale" -> NGLNodeType.SCALE
        "Skew" -> NGLNodeType.SKEW
        "SmoothPath" -> NGLNodeType.SMOOTHPATH
        "Text" -> NGLNodeType.TEXT
        "TextEffect" -> NGLNodeType.TEXTEFFECT
        "Texture2D" -> NGLNodeType.TEXTURE2D
        "Texture2DArray" -> NGLNodeType.TEXTURE2DARRAY
        "Texture3D" -> NGLNodeType.TEXTURE3D
        "TextureCube" -> NGLNodeType.TEXTURECUBE
        "TextureView" -> NGLNodeType.TEXTUREVIEW
        "Time" -> NGLNodeType.TIME
        "TimeRangeFilter" -> NGLNodeType.TIMERANGEFILTER
        "Transform" -> NGLNodeType.TRANSFORM
        "Translate" -> NGLNodeType.TRANSLATE
        "Triangle" -> NGLNodeType.TRIANGLE
        "StreamedInt" -> NGLNodeType.STREAMEDINT
        "StreamedIVec2" -> NGLNodeType.STREAMEDIVEC2
        "StreamedIVec3" -> NGLNodeType.STREAMEDIVEC3
        "StreamedIVec4" -> NGLNodeType.STREAMEDIVEC4
        "StreamedUInt" -> NGLNodeType.STREAMEDUINT
        "StreamedUIVec2" -> NGLNodeType.STREAMEDUIVEC2
        "StreamedUIVec3" -> NGLNodeType.STREAMEDUIVEC3
        "StreamedUIVec4" -> NGLNodeType.STREAMEDUIVEC4
        "StreamedFloat" -> NGLNodeType.STREAMEDFLOAT
        "StreamedVec2" -> NGLNodeType.STREAMEDVEC2
        "StreamedVec3" -> NGLNodeType.STREAMEDVEC3
        "StreamedVec4" -> NGLNodeType.STREAMEDVEC4
        "StreamedMat4" -> NGLNodeType.STREAMEDMAT4
        "StreamedBufferInt" -> NGLNodeType.STREAMEDBUFFERINT
        "StreamedBufferIVec2" -> NGLNodeType.STREAMEDBUFFERIVEC2
        "StreamedBufferIVec3" -> NGLNodeType.STREAMEDBUFFERIVEC3
        "StreamedBufferIVec4" -> NGLNodeType.STREAMEDBUFFERIVEC4
        "StreamedBufferUInt" -> NGLNodeType.STREAMEDBUFFERUINT
        "StreamedBufferUIVec2" -> NGLNodeType.STREAMEDBUFFERUIVEC2
        "StreamedBufferUIVec3" -> NGLNodeType.STREAMEDBUFFERUIVEC3
        "StreamedBufferUIVec4" -> NGLNodeType.STREAMEDBUFFERUIVEC4
        "StreamedBufferFloat" -> NGLNodeType.STREAMEDBUFFERFLOAT
        "StreamedBufferVec2" -> NGLNodeType.STREAMEDBUFFERVEC2
        "StreamedBufferVec3" -> NGLNodeType.STREAMEDBUFFERVEC3
        "StreamedBufferVec4" -> NGLNodeType.STREAMEDBUFFERVEC4
        "StreamedBufferMat4" -> NGLNodeType.STREAMEDBUFFERMAT4
        "UniformBool" -> NGLNodeType.UNIFORMBOOL
        "UniformInt" -> NGLNodeType.UNIFORMINT
        "UniformIVec2" -> NGLNodeType.UNIFORMIVEC2
        "UniformIVec3" -> NGLNodeType.UNIFORMIVEC3
        "UniformIVec4" -> NGLNodeType.UNIFORMIVEC4
        "UniformUInt" -> NGLNodeType.UNIFORMUINT
        "UniformUIVec2" -> NGLNodeType.UNIFORMUIVEC2
        "UniformUIVec3" -> NGLNodeType.UNIFORMUIVEC3
        "UniformUIVec4" -> NGLNodeType.UNIFORMUIVEC4
        "UniformMat4" -> NGLNodeType.UNIFORMMAT4
        "UniformFloat" -> NGLNodeType.UNIFORMFLOAT
        "UniformVec2" -> NGLNodeType.UNIFORMVEC2
        "UniformVec3" -> NGLNodeType.UNIFORMVEC3
        "UniformVec4" -> NGLNodeType.UNIFORMVEC4
        "UniformColor" -> NGLNodeType.UNIFORMCOLOR
        "UniformQuat" -> NGLNodeType.UNIFORMQUAT
        "UserSelect" -> NGLNodeType.USERSELECT
        "UserSwitch" -> NGLNodeType.USERSWITCH
        "VelocityFloat" -> NGLNodeType.VELOCITYFLOAT
        "VelocityVec2" -> NGLNodeType.VELOCITYVEC2
        "VelocityVec3" -> NGLNodeType.VELOCITYVEC3
        "VelocityVec4" -> NGLNodeType.VELOCITYVEC4
        else -> null
    }
}
