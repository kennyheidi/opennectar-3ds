// Type-only stand-in so upstream's pc_opengl.h (and every header that includes it) parses on 3DS.
// There is no OpenGL on the console; files that call gl* (pc_gfx.cpp, pc_postprocess.cpp,
// pc_texpack.cpp) are NOT built for 3DS -- n3ds_port/gfx replaces them.
#ifndef N3DS_GL_SHIM_H
#define N3DS_GL_SHIM_H
#include <stddef.h>
#include <stdint.h>
typedef unsigned int GLenum; typedef unsigned char GLboolean; typedef unsigned int GLbitfield;
typedef void GLvoid; typedef signed char GLbyte; typedef short GLshort; typedef int GLint;
typedef unsigned char GLubyte; typedef unsigned short GLushort; typedef unsigned int GLuint;
typedef int GLsizei; typedef float GLfloat; typedef float GLclampf; typedef double GLdouble;
typedef double GLclampd; typedef char GLchar; typedef ptrdiff_t GLintptr; typedef ptrdiff_t GLsizeiptr;
typedef int64_t GLint64; typedef uint64_t GLuint64;
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif
#endif
