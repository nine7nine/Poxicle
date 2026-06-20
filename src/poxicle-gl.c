/* poxicle-gl.c — GLES3 instanced/SDF renderer backend.
 *
 * Uploads PoxInstance[] straight into a vertex buffer (no conversion: the GL
 * attributes read the struct fields directly, including `shape` as an integer
 * attribute) and draws every particle in a single glDrawArraysInstanced. Shapes
 * are signed-distance fields evaluated in the fragment shader, so they stay
 * crisp at any size/rotation with analytic anti-aliasing.
 */
#include "poxicle-gl.h"

#include <GLES3/gl3.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

struct PoxGL {
  GLuint prog, vao, quad_vbo, inst_vbo;
  GLint  u_mvp;
  size_t cap;          /* inst_vbo capacity, in instances */
};

static const char *VS =
  "#version 300 es\n"
  "precision highp float;\n"
  "layout(location=0) in vec2  a_corner;\n"   /* unit quad [-0.5,0.5] */
  "layout(location=1) in vec2  i_pos;\n"      /* top-left px */
  "layout(location=2) in float i_size;\n"
  "layout(location=3) in float i_angle;\n"    /* degrees */
  "layout(location=4) in vec4  i_color;\n"
  "layout(location=5) in int   i_shape;\n"
  "uniform mat4 u_mvp;\n"
  "out vec2 v_local;\n"
  "out vec4 v_color;\n"
  "flat out int v_shape;\n"
  "void main(){\n"
  "  float a = radians(i_angle);\n"
  "  float c = cos(a), s = sin(a);\n"
  "  mat2 R = mat2(c, s, -s, c);\n"
  "  vec2 center = i_pos + vec2(i_size * 0.5);\n"
  "  vec2 world = center + R * (a_corner * i_size);\n"
  "  gl_Position = u_mvp * vec4(world, 0.0, 1.0);\n"
  "  v_local = a_corner;\n"
  "  v_color = i_color;\n"
  "  v_shape = i_shape;\n"
  "}\n";

static const char *FS =
  "#version 300 es\n"
  "precision highp float;\n"
  "in vec2 v_local;\n"
  "in vec4 v_color;\n"
  "flat in int v_shape;\n"
  "out vec4 frag;\n"
  "float sd_edge(vec2 p, vec2 a, vec2 b){\n"
  "  vec2 e = b - a; vec2 n = normalize(vec2(e.y, -e.x)); return dot(p - a, n);\n"
  "}\n"
  "void main(){\n"
  "  vec2 p = v_local;\n"
  "  float d;\n"
  "  if (v_shape == 1) { d = length(p) - 0.5; }\n"               /* circle */
  "  else if (v_shape == 2) { d = abs(p.x) + abs(p.y) - 0.5; }\n" /* diamond */
  "  else if (v_shape == 3) {\n"                                  /* triangle */
  "    vec2 A = vec2(0.0, -0.5), B = vec2(0.5, 0.5), C = vec2(-0.5, 0.5);\n"
  "    d = max(max(sd_edge(p, A, B), sd_edge(p, B, C)), sd_edge(p, C, A));\n"
  "  } else { d = max(abs(p.x), abs(p.y)) - 0.5; }\n"             /* square */
  "  float aa = fwidth(d);\n"
  "  float cov = 1.0 - smoothstep(-aa, aa, d);\n"
  "  frag = vec4(v_color.rgb * v_color.a * cov, v_color.a * cov);\n"
  "}\n";

static GLuint compile(GLenum type, const char *src)
{
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    fprintf(stderr, "poxicle-gl: shader compile failed: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

PoxGL *pox_gl_new(void)
{
  GLuint vs = compile(GL_VERTEX_SHADER, VS);
  GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
  if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return NULL; }

  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetProgramInfoLog(prog, sizeof log, NULL, log);
    fprintf(stderr, "poxicle-gl: link failed: %s\n", log);
    glDeleteProgram(prog);
    return NULL;
  }

  PoxGL *r = calloc(1, sizeof *r);
  if (!r) { glDeleteProgram(prog); return NULL; }
  r->prog = prog;
  r->u_mvp = glGetUniformLocation(prog, "u_mvp");

  static const float quad[] = {
    -0.5f, -0.5f,  0.5f, -0.5f, -0.5f, 0.5f,  0.5f, 0.5f,
  };

  glGenVertexArrays(1, &r->vao);
  glBindVertexArray(r->vao);

  glGenBuffers(1, &r->quad_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, r->quad_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *) 0);

  /* Instance attributes read straight from PoxInstance — no staging/convert. */
  glGenBuffers(1, &r->inst_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, r->inst_vbo);
  GLsizei stride = sizeof(PoxInstance);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *) offsetof(PoxInstance, x));
  glVertexAttribDivisor(1, 1);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void *) offsetof(PoxInstance, size));
  glVertexAttribDivisor(2, 1);
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void *) offsetof(PoxInstance, angle));
  glVertexAttribDivisor(3, 1);
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void *) offsetof(PoxInstance, color));
  glVertexAttribDivisor(4, 1);
  glEnableVertexAttribArray(5);
  glVertexAttribIPointer(5, 1, GL_INT, stride, (void *) offsetof(PoxInstance, shape));
  glVertexAttribDivisor(5, 1);

  glBindVertexArray(0);
  return r;
}

void pox_gl_free(PoxGL *r)
{
  if (!r) return;
  glDeleteBuffers(1, &r->inst_vbo);
  glDeleteBuffers(1, &r->quad_vbo);
  glDeleteVertexArrays(1, &r->vao);
  glDeleteProgram(r->prog);
  free(r);
}

/* The shared draw: premultiplied blend, one instanced draw of all particles
 * through the supplied projection. Does NOT touch the viewport or clear — the
 * caller owns both. `mvp` is column-major (GL order). */
static void pox_gl_render_common(PoxGL *r, const PoxInstance *insts, size_t n,
                                 const float mvp[16])
{
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   /* premultiplied */

  glUseProgram(r->prog);
  glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, mvp);
  glBindVertexArray(r->vao);
  glBindBuffer(GL_ARRAY_BUFFER, r->inst_vbo);

  if (n > r->cap) {
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) n * sizeof(PoxInstance), insts, GL_DYNAMIC_DRAW);
    r->cap = n;
  } else if (n > 0) {
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr) n * sizeof(PoxInstance), insts);
  }

  if (n > 0)
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei) n);
}

void pox_gl_render(PoxGL *r, const PoxInstance *insts, size_t n, int width, int height)
{
  /* Column-major ortho mapping top-left-origin surface pixels (0..w, 0..h, y
   * down) straight to clip space — exactly what the old u_viewport path baked
   * in, so wl/byoc/demo hosts are unchanged. */
  float mvp[16] = {0};

  if (!r) return;

  mvp[0]  =  2.0f / (float) width;
  mvp[5]  = -2.0f / (float) height;
  mvp[10] =  1.0f;
  mvp[12] = -1.0f;
  mvp[13] =  1.0f;
  mvp[15] =  1.0f;

  /* Self-contained host: we own the viewport. */
  glViewport(0, 0, width, height);
  pox_gl_render_common(r, insts, n, mvp);
}

void pox_gl_render_mvp(PoxGL *r, const PoxInstance *insts, size_t n, const float mvp[16])
{
  /* Embedded host (e.g. a compositor effect): the host owns the viewport and
   * supplies its own projection (e.g. KWin's RenderViewport::projectionMatrix()).
   * Particle positions are expressed in whatever space that matrix maps from. */
  if (!r) return;
  pox_gl_render_common(r, insts, n, mvp);
}
