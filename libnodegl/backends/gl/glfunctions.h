/* DO NOT EDIT - This file is autogenerated */

#ifndef NGL_GLFUNCS_H
#define NGL_GLFUNCS_H

#include "glincludes.h"

struct glfunctions {
    void (NGLI_GL_APIENTRY *ActiveTexture)(GLenum texture);
    void (NGLI_GL_APIENTRY *AttachShader)(GLuint program, GLuint shader);
    void (NGLI_GL_APIENTRY *BeginQuery)(GLenum target, GLuint id);
    void (NGLI_GL_APIENTRY *BeginQueryEXT)(GLenum target, GLuint id);
    void (NGLI_GL_APIENTRY *BindAttribLocation)(GLuint program, GLuint index, const GLchar * name);
    void (NGLI_GL_APIENTRY *BindBuffer)(GLenum target, GLuint buffer);
    void (NGLI_GL_APIENTRY *BindBufferBase)(GLenum target, GLuint index, GLuint buffer);
    void (NGLI_GL_APIENTRY *BindBufferRange)(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
    void (NGLI_GL_APIENTRY *BindFramebuffer)(GLenum target, GLuint framebuffer);
    void (NGLI_GL_APIENTRY *BindImageTexture)(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access, GLenum format);
    void (NGLI_GL_APIENTRY *BindRenderbuffer)(GLenum target, GLuint renderbuffer);
    void (NGLI_GL_APIENTRY *BindTexture)(GLenum target, GLuint texture);
    void (NGLI_GL_APIENTRY *BindVertexArray)(GLuint array);
    void (NGLI_GL_APIENTRY *BlendColor)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
    void (NGLI_GL_APIENTRY *BlendEquation)(GLenum mode);
    void (NGLI_GL_APIENTRY *BlendEquationSeparate)(GLenum modeRGB, GLenum modeAlpha);
    void (NGLI_GL_APIENTRY *BlendFunc)(GLenum sfactor, GLenum dfactor);
    void (NGLI_GL_APIENTRY *BlendFuncSeparate)(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
    void (NGLI_GL_APIENTRY *BlitFramebuffer)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
    void (NGLI_GL_APIENTRY *BufferData)(GLenum target, GLsizeiptr size, const void * data, GLenum usage);
    void (NGLI_GL_APIENTRY *BufferStorage)(GLenum target, GLsizeiptr size, const void * data, GLbitfield flags);
    void (NGLI_GL_APIENTRY *BufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size, const void * data);
    GLenum (NGLI_GL_APIENTRY *CheckFramebufferStatus)(GLenum target);
    void (NGLI_GL_APIENTRY *Clear)(GLbitfield mask);
    void (NGLI_GL_APIENTRY *ClearBufferfi)(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
    void (NGLI_GL_APIENTRY *ClearBufferfv)(GLenum buffer, GLint drawbuffer, const GLfloat * value);
    void (NGLI_GL_APIENTRY *ClearColor)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
    GLenum (NGLI_GL_APIENTRY *ClientWaitSync)(GLsync sync, GLbitfield flags, GLuint64 timeout);
    void (NGLI_GL_APIENTRY *ColorMask)(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
    void (NGLI_GL_APIENTRY *CompileShader)(GLuint shader);
    GLuint (NGLI_GL_APIENTRY *CreateProgram)();
    GLuint (NGLI_GL_APIENTRY *CreateShader)(GLenum type);
    void (NGLI_GL_APIENTRY *CullFace)(GLenum mode);
    void (NGLI_GL_APIENTRY *DebugMessageCallback)(GLDEBUGPROC callback, const void * userParam);
    void (NGLI_GL_APIENTRY *DeleteBuffers)(GLsizei n, const GLuint * buffers);
    void (NGLI_GL_APIENTRY *DeleteFramebuffers)(GLsizei n, const GLuint * framebuffers);
    void (NGLI_GL_APIENTRY *DeleteProgram)(GLuint program);
    void (NGLI_GL_APIENTRY *DeleteQueries)(GLsizei n, const GLuint * ids);
    void (NGLI_GL_APIENTRY *DeleteQueriesEXT)(GLsizei n, const GLuint * ids);
    void (NGLI_GL_APIENTRY *DeleteRenderbuffers)(GLsizei n, const GLuint * renderbuffers);
    void (NGLI_GL_APIENTRY *DeleteShader)(GLuint shader);
    void (NGLI_GL_APIENTRY *DeleteTextures)(GLsizei n, const GLuint * textures);
    void (NGLI_GL_APIENTRY *DeleteVertexArrays)(GLsizei n, const GLuint * arrays);
    void (NGLI_GL_APIENTRY *DepthFunc)(GLenum func);
    void (NGLI_GL_APIENTRY *DepthMask)(GLboolean flag);
    void (NGLI_GL_APIENTRY *DetachShader)(GLuint program, GLuint shader);
    void (NGLI_GL_APIENTRY *Disable)(GLenum cap);
    void (NGLI_GL_APIENTRY *DisableVertexAttribArray)(GLuint index);
    void (NGLI_GL_APIENTRY *DispatchCompute)(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z);
    void (NGLI_GL_APIENTRY *DrawArrays)(GLenum mode, GLint first, GLsizei count);
    void (NGLI_GL_APIENTRY *DrawArraysInstanced)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
    void (NGLI_GL_APIENTRY *DrawBuffer)(GLenum buf);
    void (NGLI_GL_APIENTRY *DrawBuffers)(GLsizei n, const GLenum * bufs);
    void (NGLI_GL_APIENTRY *DrawElements)(GLenum mode, GLsizei count, GLenum type, const void * indices);
    void (NGLI_GL_APIENTRY *DrawElementsInstanced)(GLenum mode, GLsizei count, GLenum type, const void * indices, GLsizei instancecount);
    void (NGLI_GL_APIENTRY *EGLImageTargetTexture2DOES)(GLenum target, GLeglImageOES image);
    void (NGLI_GL_APIENTRY *Enable)(GLenum cap);
    void (NGLI_GL_APIENTRY *EnableVertexAttribArray)(GLuint index);
    void (NGLI_GL_APIENTRY *EndQuery)(GLenum target);
    void (NGLI_GL_APIENTRY *EndQueryEXT)(GLenum target);
    GLsync (NGLI_GL_APIENTRY *FenceSync)(GLenum condition, GLbitfield flags);
    void (NGLI_GL_APIENTRY *Finish)();
    void (NGLI_GL_APIENTRY *Flush)();
    void (NGLI_GL_APIENTRY *FramebufferRenderbuffer)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
    void (NGLI_GL_APIENTRY *FramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    void (NGLI_GL_APIENTRY *GenBuffers)(GLsizei n, GLuint * buffers);
    void (NGLI_GL_APIENTRY *GenFramebuffers)(GLsizei n, GLuint * framebuffers);
    void (NGLI_GL_APIENTRY *GenQueries)(GLsizei n, GLuint * ids);
    void (NGLI_GL_APIENTRY *GenQueriesEXT)(GLsizei n, GLuint * ids);
    void (NGLI_GL_APIENTRY *GenRenderbuffers)(GLsizei n, GLuint * renderbuffers);
    void (NGLI_GL_APIENTRY *GenTextures)(GLsizei n, GLuint * textures);
    void (NGLI_GL_APIENTRY *GenVertexArrays)(GLsizei n, GLuint * arrays);
    void (NGLI_GL_APIENTRY *GenerateMipmap)(GLenum target);
    void (NGLI_GL_APIENTRY *GetActiveAttrib)(GLuint program, GLuint index, GLsizei bufSize, GLsizei * length, GLint * size, GLenum * type, GLchar * name);
    void (NGLI_GL_APIENTRY *GetActiveUniform)(GLuint program, GLuint index, GLsizei bufSize, GLsizei * length, GLint * size, GLenum * type, GLchar * name);
    void (NGLI_GL_APIENTRY *GetActiveUniformBlockName)(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei * length, GLchar * uniformBlockName);
    void (NGLI_GL_APIENTRY *GetActiveUniformBlockiv)(GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint * params);
    void (NGLI_GL_APIENTRY *GetAttachedShaders)(GLuint program, GLsizei maxCount, GLsizei * count, GLuint * shaders);
    GLint (NGLI_GL_APIENTRY *GetAttribLocation)(GLuint program, const GLchar * name);
    void (NGLI_GL_APIENTRY *GetBooleanv)(GLenum pname, GLboolean * data);
    GLenum (NGLI_GL_APIENTRY *GetError)();
    void (NGLI_GL_APIENTRY *GetIntegeri_v)(GLenum target, GLuint index, GLint * data);
    void (NGLI_GL_APIENTRY *GetIntegerv)(GLenum pname, GLint * data);
    void (NGLI_GL_APIENTRY *GetInternalformativ)(GLenum target, GLenum internalformat, GLenum pname, GLsizei count, GLint * params);
    void (NGLI_GL_APIENTRY *GetProgramInfoLog)(GLuint program, GLsizei bufSize, GLsizei * length, GLchar * infoLog);
    void (NGLI_GL_APIENTRY *GetProgramInterfaceiv)(GLuint program, GLenum programInterface, GLenum pname, GLint * params);
    GLuint (NGLI_GL_APIENTRY *GetProgramResourceIndex)(GLuint program, GLenum programInterface, const GLchar * name);
    GLint (NGLI_GL_APIENTRY *GetProgramResourceLocation)(GLuint program, GLenum programInterface, const GLchar * name);
    void (NGLI_GL_APIENTRY *GetProgramResourceName)(GLuint program, GLenum programInterface, GLuint index, GLsizei bufSize, GLsizei * length, GLchar * name);
    void (NGLI_GL_APIENTRY *GetProgramResourceiv)(GLuint program, GLenum programInterface, GLuint index, GLsizei propCount, const GLenum * props, GLsizei count, GLsizei * length, GLint * params);
    void (NGLI_GL_APIENTRY *GetProgramiv)(GLuint program, GLenum pname, GLint * params);
    void (NGLI_GL_APIENTRY *GetQueryObjectui64v)(GLuint id, GLenum pname, GLuint64 * params);
    void (NGLI_GL_APIENTRY *GetQueryObjectui64vEXT)(GLuint id, GLenum pname, GLuint64 * params);
    void (NGLI_GL_APIENTRY *GetRenderbufferParameteriv)(GLenum target, GLenum pname, GLint * params);
    void (NGLI_GL_APIENTRY *GetShaderInfoLog)(GLuint shader, GLsizei bufSize, GLsizei * length, GLchar * infoLog);
    void (NGLI_GL_APIENTRY *GetShaderSource)(GLuint shader, GLsizei bufSize, GLsizei * length, GLchar * source);
    void (NGLI_GL_APIENTRY *GetShaderiv)(GLuint shader, GLenum pname, GLint * params);
    const GLubyte * (NGLI_GL_APIENTRY *GetString)(GLenum name);
    const GLubyte * (NGLI_GL_APIENTRY *GetStringi)(GLenum name, GLuint index);
    GLuint (NGLI_GL_APIENTRY *GetUniformBlockIndex)(GLuint program, const GLchar * uniformBlockName);
    GLint (NGLI_GL_APIENTRY *GetUniformLocation)(GLuint program, const GLchar * name);
    void (NGLI_GL_APIENTRY *GetUniformiv)(GLuint program, GLint location, GLint * params);
    void (NGLI_GL_APIENTRY *InvalidateFramebuffer)(GLenum target, GLsizei numAttachments, const GLenum * attachments);
    void (NGLI_GL_APIENTRY *LinkProgram)(GLuint program);
    void * (NGLI_GL_APIENTRY *MapBufferRange)(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
    void (NGLI_GL_APIENTRY *MemoryBarrier)(GLbitfield barriers);
    void (NGLI_GL_APIENTRY *PixelStorei)(GLenum pname, GLint param);
    void (NGLI_GL_APIENTRY *PolygonMode)(GLenum face, GLenum mode);
    void (NGLI_GL_APIENTRY *QueryCounter)(GLuint id, GLenum target);
    void (NGLI_GL_APIENTRY *QueryCounterEXT)(GLuint id, GLenum target);
    void (NGLI_GL_APIENTRY *ReadBuffer)(GLenum src);
    void (NGLI_GL_APIENTRY *ReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void * pixels);
    void (NGLI_GL_APIENTRY *ReleaseShaderCompiler)();
    void (NGLI_GL_APIENTRY *RenderbufferStorage)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
    void (NGLI_GL_APIENTRY *RenderbufferStorageMultisample)(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height);
    void (NGLI_GL_APIENTRY *Scissor)(GLint x, GLint y, GLsizei width, GLsizei height);
    void (NGLI_GL_APIENTRY *ShaderBinary)(GLsizei count, const GLuint * shaders, GLenum binaryFormat, const void * binary, GLsizei length);
    void (NGLI_GL_APIENTRY *ShaderSource)(GLuint shader, GLsizei count, const GLchar *const* string, const GLint * length);
    void (NGLI_GL_APIENTRY *StencilFunc)(GLenum func, GLint ref, GLuint mask);
    void (NGLI_GL_APIENTRY *StencilFuncSeparate)(GLenum face, GLenum func, GLint ref, GLuint mask);
    void (NGLI_GL_APIENTRY *StencilMask)(GLuint mask);
    void (NGLI_GL_APIENTRY *StencilMaskSeparate)(GLenum face, GLuint mask);
    void (NGLI_GL_APIENTRY *StencilOp)(GLenum fail, GLenum zfail, GLenum zpass);
    void (NGLI_GL_APIENTRY *StencilOpSeparate)(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
    void (NGLI_GL_APIENTRY *TexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void * pixels);
    void (NGLI_GL_APIENTRY *TexImage3D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void * pixels);
    void (NGLI_GL_APIENTRY *TexParameteri)(GLenum target, GLenum pname, GLint param);
    void (NGLI_GL_APIENTRY *TexStorage2D)(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
    void (NGLI_GL_APIENTRY *TexStorage3D)(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth);
    void (NGLI_GL_APIENTRY *TexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void * pixels);
    void (NGLI_GL_APIENTRY *TexSubImage3D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void * pixels);
    void (NGLI_GL_APIENTRY *Uniform1fv)(GLint location, GLsizei count, const GLfloat * value);
    void (NGLI_GL_APIENTRY *Uniform1i)(GLint location, GLint v0);
    void (NGLI_GL_APIENTRY *Uniform1iv)(GLint location, GLsizei count, const GLint * value);
    void (NGLI_GL_APIENTRY *Uniform1uiv)(GLint location, GLsizei count, const GLuint * value);
    void (NGLI_GL_APIENTRY *Uniform2fv)(GLint location, GLsizei count, const GLfloat * value);
    void (NGLI_GL_APIENTRY *Uniform2iv)(GLint location, GLsizei count, const GLint * value);
    void (NGLI_GL_APIENTRY *Uniform2uiv)(GLint location, GLsizei count, const GLuint * value);
    void (NGLI_GL_APIENTRY *Uniform3fv)(GLint location, GLsizei count, const GLfloat * value);
    void (NGLI_GL_APIENTRY *Uniform3iv)(GLint location, GLsizei count, const GLint * value);
    void (NGLI_GL_APIENTRY *Uniform3uiv)(GLint location, GLsizei count, const GLuint * value);
    void (NGLI_GL_APIENTRY *Uniform4fv)(GLint location, GLsizei count, const GLfloat * value);
    void (NGLI_GL_APIENTRY *Uniform4iv)(GLint location, GLsizei count, const GLint * value);
    void (NGLI_GL_APIENTRY *Uniform4uiv)(GLint location, GLsizei count, const GLuint * value);
    void (NGLI_GL_APIENTRY *UniformBlockBinding)(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding);
    void (NGLI_GL_APIENTRY *UniformMatrix2fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat * value);
    void (NGLI_GL_APIENTRY *UniformMatrix3fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat * value);
    void (NGLI_GL_APIENTRY *UniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat * value);
    GLboolean (NGLI_GL_APIENTRY *UnmapBuffer)(GLenum target);
    void (NGLI_GL_APIENTRY *UseProgram)(GLuint program);
    void (NGLI_GL_APIENTRY *VertexAttribDivisor)(GLuint index, GLuint divisor);
    void (NGLI_GL_APIENTRY *VertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void * pointer);
    void (NGLI_GL_APIENTRY *Viewport)(GLint x, GLint y, GLsizei width, GLsizei height);
    void (NGLI_GL_APIENTRY *WaitSync)(GLsync sync, GLbitfield flags, GLuint64 timeout);
};

#endif
