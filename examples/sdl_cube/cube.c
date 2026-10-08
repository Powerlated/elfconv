#include "cube.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  unsigned frames = 0;
  if (argc == 3 && strcmp(argv[1], "--frames") == 0) {
    char *end;
    unsigned long value = strtoul(argv[2], &end, 10);
    if (!*argv[2] || *end || !value || value > 1000000) return 2;
    frames = (unsigned)value;
  } else if (argc != 1) {
    fprintf(stderr, "Usage: %s [--frames count]\n", argv[0]);
    return 2;
  }
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_Window *window = SDL_CreateWindow("Lifted i386 VBO / shader spinning cube",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, SDL_WINDOW_OPENGL);
  SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
  int result = 1;
  GLuint vertex_shader = 0, fragment_shader = 0, program = 0, buffers[2] = {0, 0};
  GLint model_location = -1, projection_location = -1;
  if (!context) {
    fprintf(stderr, "GL context: %s\n", SDL_GetError());
    goto done;
  }
  printf("OpenGL: %s; renderer: %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
  SDL_GL_SetSwapInterval(1);
  if (cube_create_program(&program, &vertex_shader, &fragment_shader,
                          &model_location, &projection_location)) goto done;
  cube_create_buffers(buffers);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glClearColor(0, 0, 0, 1);
  printf("Cube pipeline: 24 VBO vertices, 36 indexed vertices, GLSL lighting, depth test; no immediate mode\n");
  unsigned rendered = 0, turn = 0;
  GLfloat cosine = 0.819152044f, sine = 0.573576436f;
  int running = 1;
  result = 0;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) running = 0;
    }
    if (!running) break;
    int width, height;
    cube_draw_frame(window, model_location, projection_location, cosine, sine, &width, &height);
    ++rendered;
    if (frames && (rendered == 1 || rendered == frames)) {
      result = cube_check_frame(width, height, rendered);
      if (result) break;
    }
    SDL_GL_SwapWindow(window);
    if (frames && rendered >= frames) running = 0;
    // Rotate one degree per presented frame. Reset after a full turn to bound
    // floating-point drift; no guest libm or per-vertex trigonometry is needed.
    GLfloat next_cosine = cosine*.999847695f - sine*.017452406f;
    sine = sine*.999847695f + cosine*.017452406f;
    cosine = next_cosine;
    if (++turn == 360) { turn = 0; cosine = .819152044f; sine = .573576436f; }
  }
  printf("Cube rendered %u frames\n", rendered);
  glDeleteBuffers(2, buffers);
  buffers[0] = buffers[1] = 0;
done:
  if (context) {
    if (buffers[0] || buffers[1]) glDeleteBuffers(2, buffers);
    if (program) glDeleteProgram(program);
    if (vertex_shader) glDeleteShader(vertex_shader);
    if (fragment_shader) glDeleteShader(fragment_shader);
    SDL_GL_DeleteContext(context);
  }
  if (window) SDL_DestroyWindow(window);
  SDL_Quit();
  return result;
}
