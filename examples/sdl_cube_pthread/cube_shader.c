#include "cube.h"

#include <stdio.h>

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

int cube_create_program(GLuint *program, GLuint *vertex_shader, GLuint *fragment_shader,
                        GLint *model_location, GLint *projection_location) {
  *vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_source);
  *fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
  if (!*vertex_shader || !*fragment_shader) return 1;
  *program = glCreateProgram();
  glAttachShader(*program, *vertex_shader);
  glAttachShader(*program, *fragment_shader);
  glBindAttribLocation(*program, 0, "a_position");
  glBindAttribLocation(*program, 1, "a_normal");
  glBindAttribLocation(*program, 2, "a_color");
  glLinkProgram(*program);
  GLint linked;
  glGetProgramiv(*program, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[1024];
    glGetProgramInfoLog(*program, sizeof(log), NULL, log);
    fprintf(stderr, "Program link failed: %s\n", log);
    return 1;
  }
  glUseProgram(*program);
  *model_location = glGetUniformLocation(*program, "u_model");
  *projection_location = glGetUniformLocation(*program, "u_projection");
  if (*model_location < 0 || *projection_location < 0) {
    fprintf(stderr, "Missing transform uniforms\n");
    return 1;
  }
  return 0;
}
