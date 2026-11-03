#pragma once

// Minimal GL surface for compiling the production collector against a fake driver.
using GLenum = unsigned int;
using GLint = int;
using GLubyte = unsigned char;

inline constexpr GLenum GL_NO_ERROR = 0;
inline constexpr GLenum GL_INVALID_ENUM = 0x0500;
inline constexpr GLenum GL_INVALID_VALUE = 0x0501;
inline constexpr GLenum GL_INVALID_OPERATION = 0x0502;
inline constexpr GLenum GL_VENDOR = 0x1F00;
inline constexpr GLenum GL_RENDERER = 0x1F01;
inline constexpr GLenum GL_VERSION = 0x1F02;

extern "C" {
GLenum glGetError();
const GLubyte* glGetString(GLenum name);
void glGetIntegerv(GLenum name, GLint* value);
}
