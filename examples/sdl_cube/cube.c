#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Vertex { GLfloat position[3], normal[3], color[3]; };
#define V(x, y, z, nx, ny, nz, r, g, b) {{x, y, z}, {nx, ny, nz}, {r, g, b}}
static const struct Vertex vertices[] = {
  V(-1,-1, 1, 0,0, 1, .2f,.35f,1), V( 1,-1, 1, 0,0, 1, .2f,.35f,1),
  V( 1, 1, 1, 0,0, 1, .2f,.35f,1), V(-1, 1, 1, 0,0, 1, .2f,.35f,1),
  V( 1,-1,-1, 0,0,-1, .9f,.3f,1), V(-1,-1,-1, 0,0,-1, .9f,.3f,1),
  V(-1, 1,-1, 0,0,-1, .9f,.3f,1), V( 1, 1,-1, 0,0,-1, .9f,.3f,1),
  V(-1,-1,-1,-1,0, 0, 1,.25f,.15f), V(-1,-1, 1,-1,0, 0, 1,.25f,.15f),
  V(-1, 1, 1,-1,0, 0, 1,.25f,.15f), V(-1, 1,-1,-1,0, 0, 1,.25f,.15f),
  V( 1,-1, 1, 1,0, 0, .8f,.35f,.15f), V( 1,-1,-1, 1,0, 0, .8f,.35f,.15f),
  V( 1, 1,-1, 1,0, 0, .8f,.35f,.15f), V( 1, 1, 1, 1,0, 0, .8f,.35f,.15f),
  V(-1, 1, 1, 0,1, 0, .2f,1,.35f), V( 1, 1, 1, 0,1, 0, .2f,1,.35f),
  V( 1, 1,-1, 0,1, 0, .2f,1,.35f), V(-1, 1,-1, 0,1, 0, .2f,1,.35f),
  V(-1,-1,-1, 0,-1,0, .8f,.2f,.8f), V( 1,-1,-1, 0,-1,0, .8f,.2f,.8f),
  V( 1,-1, 1, 0,-1,0, .8f,.2f,.8f), V(-1,-1, 1, 0,-1,0, .8f,.2f,.8f)
};
#undef V
static const GLushort indices[] = {
   0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9,10, 8,10,11,
  12,13,14,12,14,15,16,17,18,16,18,19,20,21,22,20,22,23
};
static const char vertex_source[] =
  "attribute vec3 a_position;\n"
  "attribute vec3 a_normal;\n"
  "attribute vec3 a_color;\n"
  "uniform mat4 u_model;\n"
  "uniform mat4 u_projection;\n"
  "varying vec3 v_normal;\n"
  "varying vec3 v_color;\n"
  "void main() {\n"
  "  gl_Position = u_projection * u_model * vec4(a_position, 1.0);\n"
  "  v_normal = (u_model * vec4(a_normal, 0.0)).xyz;\n"
  "  v_color = a_color;\n"
  "}\n";
static const char fragment_source[] =
  "#ifdef GL_ES\nprecision mediump float;\n#endif\n"
  "varying vec3 v_normal;\n"
  "varying vec3 v_color;\n"
  "void main() {\n"
  "  float diffuse = max(dot(normalize(v_normal), normalize(vec3(0.4, 0.7, 1.0))), 0.0);\n"
  "  gl_FragColor = vec4(v_color * (0.18 + 0.82 * diffuse), 1.0);\n"
  "}\n";

static GLuint compile_shader(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint compiled;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    char log[1024];
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    fprintf(stderr, "Shader compile failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static int check_frame(int width, int height, unsigned frame) {
  unsigned char center[3], left[3], top[3], corner[3];
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(width/2, height/2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, center);
  glReadPixels(width/2-width/12, height/2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, left);
  glReadPixels(width/2, height/2+height/8, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, top);
  glReadPixels(1, 1, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, corner);
  GLenum error = glGetError();
  if (error != GL_NO_ERROR || (center[0] + center[1] + center[2]) < 40 ||
      (left[0] + left[1] + left[2]) < 40 || (top[0] + top[1] + top[2]) < 40 ||
      corner[0] || corner[1] || corner[2]) {
    fprintf(stderr, "Cube readback failed: frame=%u GL=%u center=%u,%u,%u left=%u,%u,%u top=%u,%u,%u\n",
        frame, error, center[0], center[1], center[2], left[0], left[1], left[2], top[0], top[1], top[2]);
    return 1;
  }
  if (frame == 1 && !(left[0] > left[1] && left[0] > left[2] &&
                      top[1] > top[0] && top[1] > top[2] &&
                      center[2] > center[0] && center[2] > center[1])) {
    fprintf(stderr, "Cube face/depth check failed: center=%u,%u,%u left=%u,%u,%u top=%u,%u,%u\n",
        center[0], center[1], center[2], left[0], left[1], left[2], top[0], top[1], top[2]);
    return 1;
  }
  printf("Cube readback passed: frame=%u center=%u,%u,%u left=%u,%u,%u top=%u,%u,%u; corner=0,0,0\n",
      frame, center[0], center[1], center[2], left[0], left[1], left[2], top[0], top[1], top[2]);
  return 0;
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
  if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_Window *window = SDL_CreateWindow("Lifted i386 VBO / shader spinning cube",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, SDL_WINDOW_OPENGL);
  SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
  int result = 1;
  GLuint vs = 0, fs = 0, program = 0, buffers[2] = {0, 0};
  if (!context) { fprintf(stderr, "GL context: %s\n", SDL_GetError()); goto done; }
  printf("OpenGL: %s; renderer: %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
  SDL_GL_SetSwapInterval(1);
  vs = compile_shader(GL_VERTEX_SHADER, vertex_source);
  fs = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
  if (!vs || !fs) goto done;
  program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glBindAttribLocation(program, 0, "a_position");
  glBindAttribLocation(program, 1, "a_normal");
  glBindAttribLocation(program, 2, "a_color");
  glLinkProgram(program);
  GLint linked;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[1024];
    glGetProgramInfoLog(program, sizeof(log), NULL, log);
    fprintf(stderr, "Program link failed: %s\n", log);
    goto done;
  }
  glUseProgram(program);
  GLint model_location = glGetUniformLocation(program, "u_model");
  GLint projection_location = glGetUniformLocation(program, "u_projection");
  if (model_location < 0 || projection_location < 0) {
    fprintf(stderr, "Missing transform uniforms\n"); goto done;
  }
  glGenBuffers(2, buffers);
  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct Vertex), (void *)offsetof(struct Vertex, position));
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(struct Vertex), (void *)offsetof(struct Vertex, normal));
  glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(struct Vertex), (void *)offsetof(struct Vertex, color));
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
    SDL_GL_GetDrawableSize(window, &width, &height);
    glViewport(0, 0, width, height);
    GLfloat model[] = {
      cosine, .422618262f*sine, -.906307787f*sine, 0,
      0, .906307787f, .422618262f, 0,
      sine, -.422618262f*cosine, .906307787f*cosine, 0,
      0, 0, -4.5f, 1
    };
    GLfloat projection[] = {
      1.732050808f*(GLfloat)height/(GLfloat)width, 0, 0, 0,
      0, 1.732050808f, 0, 0, 0, 0, -1.002002002f, -1,
      0, 0, -.200200200f, 0
    };
    glUniformMatrix4fv(model_location, 1, GL_FALSE, model);
    glUniformMatrix4fv(projection_location, 1, GL_FALSE, projection);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, (void *)0);
    ++rendered;
    if (frames && (rendered == 1 || rendered == frames)) {
      result = check_frame(width, height, rendered);
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
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    SDL_GL_DeleteContext(context);
  }
  if (window) SDL_DestroyWindow(window);
  SDL_Quit();
  return result;
}
