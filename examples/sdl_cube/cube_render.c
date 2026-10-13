#include "cube.h"

#include <stdio.h>

int cube_check_frame(int width, int height, unsigned frame) {
  unsigned char center[4], left[4], top[4], corner[4];
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(width/2, height/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
  glReadPixels(width/2-width/12, height/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
  glReadPixels(width/2, height/2+height/8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, top);
  glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
  GLenum error = glGetError();
  if (error != GL_NO_ERROR || (center[0] + center[1] + center[2]) < 40 ||
      (left[0] + left[1] + left[2]) < 40 || (top[0] + top[1] + top[2]) < 40 ||
      corner[0] || corner[1] || corner[2] || corner[3] != 255 ||
      center[3] != 255 || left[3] != 255 || top[3] != 255
      ) {
    fprintf(stderr, "Cube readback failed: frame=%u GL=%u center=%u,%u,%u,%u left=%u,%u,%u,%u top=%u,%u,%u,%u\n",
        frame, error, center[0], center[1], center[2], center[3],
        left[0], left[1], left[2], left[3], top[0], top[1], top[2], top[3]);
    return 1;
  }
  if (frame == 1 && !(left[0] > left[1] && left[0] > left[2] &&
                      top[1] > top[0] && top[1] > top[2] &&
                      center[2] > center[0] && center[2] > center[1])) {
    fprintf(stderr, "Cube face/depth ordering check failed: center=%u,%u,%u left=%u,%u,%u top=%u,%u,%u\n",
        center[0], center[1], center[2], left[0], left[1], left[2], top[0], top[1], top[2]);
    return 1;
  }
  printf("Cube readback passed: frame=%u center=%u,%u,%u left=%u,%u,%u top=%u,%u,%u; alpha=255; corner=0,0,0\n",
      frame, center[0], center[1], center[2], left[0], left[1], left[2],
      top[0], top[1], top[2]);
  return 0;
}

void cube_draw_frame(SDL_Window *window, GLint model_location, GLint projection_location,
                     GLfloat cosine, GLfloat sine, int *width, int *height) {
  SDL_GL_GetDrawableSize(window, width, height);
  glViewport(0, 0, *width, *height);
  GLfloat model[] = {
    cosine, .422618262f*sine, -.906307787f*sine, 0,
    0, .906307787f, .422618262f, 0,
    sine, -.422618262f*cosine, .906307787f*cosine, 0,
    0, 0, -4.5f, 1
  };
  GLfloat projection[] = {
    1.732050808f*(GLfloat)*height/(GLfloat)*width, 0, 0, 0,
    0, 1.732050808f, 0, 0, 0, 0, -1.002002002f, -1,
    0, 0, -.200200200f, 0
  };
  glUniformMatrix4fv(model_location, 1, GL_FALSE, model);
  glUniformMatrix4fv(projection_location, 1, GL_FALSE, projection);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, (void *)0);
}
