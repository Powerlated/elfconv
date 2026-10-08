#include "cube.h"

#include <stddef.h>

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

void cube_create_buffers(GLuint buffers[2]) {
  glGenBuffers(2, buffers);
  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct Vertex),
                        (void *)offsetof(struct Vertex, position));
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(struct Vertex),
                        (void *)offsetof(struct Vertex, normal));
  glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(struct Vertex),
                        (void *)offsetof(struct Vertex, color));
}
