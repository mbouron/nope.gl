package org.nopeforge.nopegl.specs

import org.nopeforge.nopegl.NodeType

fun nodeType(name: String): NodeType? {
    return when (name) {
        "AnimatedBufferFloat" -> NodeType.ANIMATEDBUFFERFLOAT
        "AnimatedBufferVec2" -> NodeType.ANIMATEDBUFFERVEC2
        "AnimatedBufferVec3" -> NodeType.ANIMATEDBUFFERVEC3
        "AnimatedBufferVec4" -> NodeType.ANIMATEDBUFFERVEC4
        "AnimatedColor" -> NodeType.ANIMATEDCOLOR
        "AnimatedPath" -> NodeType.ANIMATEDPATH
        "AnimatedTime" -> NodeType.ANIMATEDTIME
        "AnimatedFloat" -> NodeType.ANIMATEDFLOAT
        "AnimatedVec2" -> NodeType.ANIMATEDVEC2
        "AnimatedVec3" -> NodeType.ANIMATEDVEC3
        "AnimatedVec4" -> NodeType.ANIMATEDVEC4
        "AnimatedQuat" -> NodeType.ANIMATEDQUAT
        "AnimKeyFrameFloat" -> NodeType.ANIMKEYFRAMEFLOAT
        "AnimKeyFrameVec2" -> NodeType.ANIMKEYFRAMEVEC2
        "AnimKeyFrameVec3" -> NodeType.ANIMKEYFRAMEVEC3
        "AnimKeyFrameVec4" -> NodeType.ANIMKEYFRAMEVEC4
        "AnimKeyFrameQuat" -> NodeType.ANIMKEYFRAMEQUAT
        "AnimKeyFrameColor" -> NodeType.ANIMKEYFRAMECOLOR
        "AnimKeyFrameBuffer" -> NodeType.ANIMKEYFRAMEBUFFER
        "Block" -> NodeType.BLOCK
        "BufferByte" -> NodeType.BUFFERBYTE
        "BufferBVec2" -> NodeType.BUFFERBVEC2
        "BufferBVec3" -> NodeType.BUFFERBVEC3
        "BufferBVec4" -> NodeType.BUFFERBVEC4
        "BufferInt" -> NodeType.BUFFERINT
        "BufferInt64" -> NodeType.BUFFERINT64
        "BufferIVec2" -> NodeType.BUFFERIVEC2
        "BufferIVec3" -> NodeType.BUFFERIVEC3
        "BufferIVec4" -> NodeType.BUFFERIVEC4
        "BufferShort" -> NodeType.BUFFERSHORT
        "BufferSVec2" -> NodeType.BUFFERSVEC2
        "BufferSVec3" -> NodeType.BUFFERSVEC3
        "BufferSVec4" -> NodeType.BUFFERSVEC4
        "BufferUByte" -> NodeType.BUFFERUBYTE
        "BufferUBVec2" -> NodeType.BUFFERUBVEC2
        "BufferUBVec3" -> NodeType.BUFFERUBVEC3
        "BufferUBVec4" -> NodeType.BUFFERUBVEC4
        "BufferUInt" -> NodeType.BUFFERUINT
        "BufferUIVec2" -> NodeType.BUFFERUIVEC2
        "BufferUIVec3" -> NodeType.BUFFERUIVEC3
        "BufferUIVec4" -> NodeType.BUFFERUIVEC4
        "BufferUShort" -> NodeType.BUFFERUSHORT
        "BufferUSVec2" -> NodeType.BUFFERUSVEC2
        "BufferUSVec3" -> NodeType.BUFFERUSVEC3
        "BufferUSVec4" -> NodeType.BUFFERUSVEC4
        "BufferFloat" -> NodeType.BUFFERFLOAT
        "BufferVec2" -> NodeType.BUFFERVEC2
        "BufferVec3" -> NodeType.BUFFERVEC3
        "BufferVec4" -> NodeType.BUFFERVEC4
        "BufferMat4" -> NodeType.BUFFERMAT4
        "Camera" -> NodeType.CAMERA
        "Circle" -> NodeType.CIRCLE
        "ColorKey" -> NodeType.COLORKEY
        "ColorStats" -> NodeType.COLORSTATS
        "Compute" -> NodeType.COMPUTE
        "ComputeProgram" -> NodeType.COMPUTEPROGRAM
        "Draw" -> NodeType.DRAW
        "DrawColor" -> NodeType.DRAWCOLOR
        "DrawDisplace" -> NodeType.DRAWDISPLACE
        "DrawGradient" -> NodeType.DRAWGRADIENT
        "DrawGradient4" -> NodeType.DRAWGRADIENT4
        "DrawHistogram" -> NodeType.DRAWHISTOGRAM
        "DrawNoise" -> NodeType.DRAWNOISE
        "DrawPath" -> NodeType.DRAWPATH
        "DrawTexture" -> NodeType.DRAWTEXTURE
        "DrawWaveform" -> NodeType.DRAWWAVEFORM
        "FilterAlpha" -> NodeType.FILTERALPHA
        "FilterColorMap" -> NodeType.FILTERCOLORMAP
        "FilterContrast" -> NodeType.FILTERCONTRAST
        "FilterExposure" -> NodeType.FILTEREXPOSURE
        "FilterInverseAlpha" -> NodeType.FILTERINVERSEALPHA
        "FilterLinear2sRGB" -> NodeType.FILTERLINEAR2SRGB
        "FilterOpacity" -> NodeType.FILTEROPACITY
        "FilterPremult" -> NodeType.FILTERPREMULT
        "FilterSaturation" -> NodeType.FILTERSATURATION
        "FilterSelector" -> NodeType.FILTERSELECTOR
        "FilterSRGB2Linear" -> NodeType.FILTERSRGB2LINEAR
        "FastGaussianBlur" -> NodeType.FASTGAUSSIANBLUR
        "FontFace" -> NodeType.FONTFACE
        "GaussianBlur" -> NodeType.GAUSSIANBLUR
        "Geometry" -> NodeType.GEOMETRY
        "GraphicConfig" -> NodeType.GRAPHICCONFIG
        "GridLayout" -> NodeType.GRIDLAYOUT
        "Group" -> NodeType.GROUP
        "Identity" -> NodeType.IDENTITY
        "IOInt" -> NodeType.IOINT
        "IOIVec2" -> NodeType.IOIVEC2
        "IOIVec3" -> NodeType.IOIVEC3
        "IOIVec4" -> NodeType.IOIVEC4
        "IOUInt" -> NodeType.IOUINT
        "IOUIvec2" -> NodeType.IOUIVEC2
        "IOUIvec3" -> NodeType.IOUIVEC3
        "IOUIvec4" -> NodeType.IOUIVEC4
        "IOFloat" -> NodeType.IOFLOAT
        "IOVec2" -> NodeType.IOVEC2
        "IOVec3" -> NodeType.IOVEC3
        "IOVec4" -> NodeType.IOVEC4
        "IOMat3" -> NodeType.IOMAT3
        "IOMat4" -> NodeType.IOMAT4
        "IOBool" -> NodeType.IOBOOL
        "EvalFloat" -> NodeType.EVALFLOAT
        "EvalVec2" -> NodeType.EVALVEC2
        "EvalVec3" -> NodeType.EVALVEC3
        "EvalVec4" -> NodeType.EVALVEC4
        "Media" -> NodeType.MEDIA
        "NoiseFloat" -> NodeType.NOISEFLOAT
        "NoiseVec2" -> NodeType.NOISEVEC2
        "NoiseVec3" -> NodeType.NOISEVEC3
        "NoiseVec4" -> NodeType.NOISEVEC4
        "Path" -> NodeType.PATH
        "PathKeyBezier2" -> NodeType.PATHKEYBEZIER2
        "PathKeyBezier3" -> NodeType.PATHKEYBEZIER3
        "PathKeyClose" -> NodeType.PATHKEYCLOSE
        "PathKeyLine" -> NodeType.PATHKEYLINE
        "PathKeyMove" -> NodeType.PATHKEYMOVE
        "Program" -> NodeType.PROGRAM
        "Quad" -> NodeType.QUAD
        "RenderToTexture" -> NodeType.RENDERTOTEXTURE
        "ResourceProps" -> NodeType.RESOURCEPROPS
        "Rotate" -> NodeType.ROTATE
        "RotateQuat" -> NodeType.ROTATEQUAT
        "Scale" -> NodeType.SCALE
        "Skew" -> NodeType.SKEW
        "SmoothPath" -> NodeType.SMOOTHPATH
        "Text" -> NodeType.TEXT
        "TextEffect" -> NodeType.TEXTEFFECT
        "Texture2D" -> NodeType.TEXTURE2D
        "Texture2DArray" -> NodeType.TEXTURE2DARRAY
        "Texture3D" -> NodeType.TEXTURE3D
        "TextureCube" -> NodeType.TEXTURECUBE
        "TextureView" -> NodeType.TEXTUREVIEW
        "Time" -> NodeType.TIME
        "TimeRangeFilter" -> NodeType.TIMERANGEFILTER
        "Transform" -> NodeType.TRANSFORM
        "Translate" -> NodeType.TRANSLATE
        "Triangle" -> NodeType.TRIANGLE
        "StreamedInt" -> NodeType.STREAMEDINT
        "StreamedIVec2" -> NodeType.STREAMEDIVEC2
        "StreamedIVec3" -> NodeType.STREAMEDIVEC3
        "StreamedIVec4" -> NodeType.STREAMEDIVEC4
        "StreamedUInt" -> NodeType.STREAMEDUINT
        "StreamedUIVec2" -> NodeType.STREAMEDUIVEC2
        "StreamedUIVec3" -> NodeType.STREAMEDUIVEC3
        "StreamedUIVec4" -> NodeType.STREAMEDUIVEC4
        "StreamedFloat" -> NodeType.STREAMEDFLOAT
        "StreamedVec2" -> NodeType.STREAMEDVEC2
        "StreamedVec3" -> NodeType.STREAMEDVEC3
        "StreamedVec4" -> NodeType.STREAMEDVEC4
        "StreamedMat4" -> NodeType.STREAMEDMAT4
        "StreamedBufferInt" -> NodeType.STREAMEDBUFFERINT
        "StreamedBufferIVec2" -> NodeType.STREAMEDBUFFERIVEC2
        "StreamedBufferIVec3" -> NodeType.STREAMEDBUFFERIVEC3
        "StreamedBufferIVec4" -> NodeType.STREAMEDBUFFERIVEC4
        "StreamedBufferUInt" -> NodeType.STREAMEDBUFFERUINT
        "StreamedBufferUIVec2" -> NodeType.STREAMEDBUFFERUIVEC2
        "StreamedBufferUIVec3" -> NodeType.STREAMEDBUFFERUIVEC3
        "StreamedBufferUIVec4" -> NodeType.STREAMEDBUFFERUIVEC4
        "StreamedBufferFloat" -> NodeType.STREAMEDBUFFERFLOAT
        "StreamedBufferVec2" -> NodeType.STREAMEDBUFFERVEC2
        "StreamedBufferVec3" -> NodeType.STREAMEDBUFFERVEC3
        "StreamedBufferVec4" -> NodeType.STREAMEDBUFFERVEC4
        "StreamedBufferMat4" -> NodeType.STREAMEDBUFFERMAT4
        "UniformBool" -> NodeType.UNIFORMBOOL
        "UniformInt" -> NodeType.UNIFORMINT
        "UniformIVec2" -> NodeType.UNIFORMIVEC2
        "UniformIVec3" -> NodeType.UNIFORMIVEC3
        "UniformIVec4" -> NodeType.UNIFORMIVEC4
        "UniformUInt" -> NodeType.UNIFORMUINT
        "UniformUIVec2" -> NodeType.UNIFORMUIVEC2
        "UniformUIVec3" -> NodeType.UNIFORMUIVEC3
        "UniformUIVec4" -> NodeType.UNIFORMUIVEC4
        "UniformMat4" -> NodeType.UNIFORMMAT4
        "UniformFloat" -> NodeType.UNIFORMFLOAT
        "UniformVec2" -> NodeType.UNIFORMVEC2
        "UniformVec3" -> NodeType.UNIFORMVEC3
        "UniformVec4" -> NodeType.UNIFORMVEC4
        "UniformColor" -> NodeType.UNIFORMCOLOR
        "UniformQuat" -> NodeType.UNIFORMQUAT
        "UserSelect" -> NodeType.USERSELECT
        "UserSwitch" -> NodeType.USERSWITCH
        "VelocityFloat" -> NodeType.VELOCITYFLOAT
        "VelocityVec2" -> NodeType.VELOCITYVEC2
        "VelocityVec3" -> NodeType.VELOCITYVEC3
        "VelocityVec4" -> NodeType.VELOCITYVEC4
        else -> null
    }
}