#pragma once

#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

int cube_create_program(GLuint *program, GLuint *vertex_shader, GLuint *fragment_shader,
                        GLint *model_location, GLint *projection_location);
void cube_create_buffers(GLuint buffers[2]);
void cube_draw_frame(SDL_Window *window, GLint model_location, GLint projection_location,
                     GLfloat cosine, GLfloat sine, int *width, int *height);
int cube_check_frame(int width, int height, unsigned frame);
