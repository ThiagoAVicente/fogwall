#ifndef FOGWALL_SHADER_H
#define FOGWALL_SHADER_H

#include <GLES2/gl2.h>

/* Returns 0 on failure (error is printed to stderr). */
GLuint shader_program_create(const char *vert_src, const char *frag_src);

#endif
