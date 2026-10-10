#include "cube.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Rotation {
  pthread_mutex_t mutex;
  pthread_cond_t request, ready;
  pthread_t renderer;
  unsigned requested, produced;
  int stop;
  GLfloat cosine, sine;
};

static void pthread_check(int error, const char *operation) {
  if (error) {
    fprintf(stderr, "%s failed: %d\n", operation, error);
    exit(1);
  }
}

static void *rotate(void *argument) {
  struct Rotation *rotation = argument;
  if (pthread_equal(pthread_self(), rotation->renderer)) {
    fprintf(stderr, "Rotation must execute on a different thread\n");
    exit(1);
  }
  GLfloat cosine = .819152044f, sine = .573576436f;
  unsigned turn = 0;
  pthread_check(pthread_mutex_lock(&rotation->mutex), "worker mutex lock");
  for (;;) {
    while (!rotation->stop && rotation->requested == rotation->produced)
      pthread_check(pthread_cond_wait(&rotation->request, &rotation->mutex), "request wait");
    if (rotation->stop) break;
    rotation->cosine = cosine;
    rotation->sine = sine;
    rotation->produced = rotation->requested;
    pthread_check(pthread_cond_signal(&rotation->ready), "rotation ready");
    GLfloat next_cosine = cosine*.999847695f - sine*.017452406f;
    sine = sine*.999847695f + cosine*.017452406f;
    cosine = next_cosine;
    if (++turn == 360) { turn = 0; cosine = .819152044f; sine = .573576436f; }
  }
  unsigned produced = rotation->produced;
  pthread_check(pthread_mutex_unlock(&rotation->mutex), "worker mutex unlock");
  return (void *)(uintptr_t)produced;
}

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
  SDL_Window *window = SDL_CreateWindow("Lifted i386 pthread spinning cube",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, SDL_WINDOW_OPENGL);
  SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
  int result = 1;
  GLuint vertex_shader = 0, fragment_shader = 0, program = 0, buffers[2] = {0, 0};
  GLint model_location = -1, projection_location = -1;
  struct Rotation rotation = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .request = PTHREAD_COND_INITIALIZER,
    .ready = PTHREAD_COND_INITIALIZER,
    .renderer = pthread_self()
  };
  pthread_t worker;
  int worker_started = 0;
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
  unsigned rendered = 0;
  pthread_check(pthread_create(&worker, NULL, rotate, &rotation), "pthread_create");
  worker_started = 1;
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
    pthread_check(pthread_mutex_lock(&rotation.mutex), "renderer mutex lock");
    rotation.requested = rendered + 1;
    pthread_check(pthread_cond_signal(&rotation.request), "rotation request");
    while (rotation.produced != rotation.requested)
      pthread_check(pthread_cond_wait(&rotation.ready, &rotation.mutex), "rotation wait");
    GLfloat cosine = rotation.cosine, sine = rotation.sine;
    pthread_check(pthread_mutex_unlock(&rotation.mutex), "renderer mutex unlock");
    cube_draw_frame(window, model_location, projection_location, cosine, sine, &width, &height);
    ++rendered;
    if (frames && (rendered == 1 || rendered == frames)) {
      result = cube_check_frame(width, height, rendered);
      if (result) break;
    }
    SDL_GL_SwapWindow(window);
    if (frames && rendered >= frames) running = 0;
  }
  printf("Cube rendered %u frames\n", rendered);
  glDeleteBuffers(2, buffers);
  buffers[0] = buffers[1] = 0;
done:
  if (worker_started) {
    pthread_check(pthread_mutex_lock(&rotation.mutex), "shutdown mutex lock");
    rotation.stop = 1;
    pthread_check(pthread_cond_signal(&rotation.request), "shutdown signal");
    pthread_check(pthread_mutex_unlock(&rotation.mutex), "shutdown mutex unlock");
    void *worker_result;
    pthread_check(pthread_join(worker, &worker_result), "pthread_join");
    if ((uintptr_t)worker_result != rendered) {
      fprintf(stderr, "Worker returned %lu rotations for %u frames\n",
          (unsigned long)(uintptr_t)worker_result, rendered);
      result = 1;
    }
    printf("Rotation worker joined: %u rotations on a separate thread\n", rendered);
  }
  pthread_check(pthread_cond_destroy(&rotation.ready), "ready destroy");
  pthread_check(pthread_cond_destroy(&rotation.request), "request destroy");
  pthread_check(pthread_mutex_destroy(&rotation.mutex), "mutex destroy");
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
