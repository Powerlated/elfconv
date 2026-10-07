#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  unsigned frames = 0;
  if (argc == 3 && strcmp(argv[1], "--frames") == 0) {
    char *end;
    unsigned long value = strtoul(argv[2], &end, 10);
    if (!*argv[2] || *end || value == 0 || value > 1000000) return 2;
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
  SDL_Window *window = SDL_CreateWindow("i386 SDL / OpenGL 2 triangle",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, SDL_WINDOW_OPENGL);
  SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
  if (!context) {
    fprintf(stderr, "OpenGL context: %s\n", SDL_GetError());
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  printf("OpenGL: %s; renderer: %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
  SDL_GL_SetSwapInterval(1);
  unsigned rendered = 0;
  int running = 1, result = 0;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) running = 0;
    }
    if (!running) break;
    int width, height;
    SDL_GL_GetDrawableSize(window, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex2f(-0.8f, -0.8f);
    glColor3f(0, 1, 0); glVertex2f(0.8f, -0.8f);
    glColor3f(0, 0, 1); glVertex2f(0, 0.8f);
    glEnd();
    if (frames && rendered == 0) {
      unsigned char center[3], corner[3];
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(width / 2, height / 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, center);
      glReadPixels(1, 1, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, corner);
      GLenum error = glGetError();
      if (error != GL_NO_ERROR || center[0] < 20 || center[1] < 20 || center[2] < 20 ||
          corner[0] != 0 || corner[1] != 0 || corner[2] != 0) {
        fprintf(stderr, "Triangle readback failed: GL=%u center=%u,%u,%u corner=%u,%u,%u\n",
            error, center[0], center[1], center[2], corner[0], corner[1], corner[2]);
        result = 1;
        break;
      }
      printf("Triangle readback passed: center=%u,%u,%u; corner=0,0,0\n",
          center[0], center[1], center[2]);
    }
    SDL_GL_SwapWindow(window);
    if (frames && ++rendered >= frames) running = 0;
  }
  SDL_GL_DeleteContext(context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return result;
}
