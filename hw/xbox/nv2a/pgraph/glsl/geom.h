/*
 * Geforce NV2A PGRAPH GLSL Shader Generator
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2020-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_GLSL_GEOM_H
#define HW_XBOX_NV2A_PGRAPH_GLSL_GEOM_H

#include "common.h"
#include "hw/xbox/nv2a/pgraph/vsh_regs.h"

typedef struct {
    enum ShaderPrimitiveMode primitive_mode;
    enum ShaderPolygonMode polygon_front_mode;
    enum ShaderPolygonMode polygon_back_mode;
    bool smooth_shading;
    bool first_vertex_is_provoking;
    bool z_perspective;
    bool fast_depth;
    bool no_geometry_shader;
    short tri_rot0;
    short tri_rot1;
} GeomState;

typedef struct GenGeomGlslOptions {
    bool vulkan;
} GenGeomGlslOptions;

void pgraph_glsl_set_geom_state(PGRAPHState *pg, GeomState *geom);

bool pgraph_glsl_need_geom(const GeomState *state);

/*
 * True when quads/quad strips are drawn as indexed triangle lists instead of
 * through the geometry shader (fast depth, fill mode, smooth shading).
 */
bool pgraph_glsl_quads_as_triangles(const GeomState *state);

/* Upper bound of triangle list indices for count quad (strip) vertices */
static inline size_t pgraph_quad_tri_index_count(enum ShaderPrimitiveMode mode,
                                                 size_t count)
{
    if (mode == PRIM_TYPE_QUADS) {
        return count / 4 * 6;
    }
    return count >= 4 ? (count - 2) / 2 * 6 : 0;
}

/*
 * Convert count quad (strip) vertices to triangle list indices, using the
 * same split and winding as the geometry shader. Vertex i is in[i] if in is
 * not NULL, otherwise first + i. Returns the number of indices written.
 */
size_t pgraph_quads_to_triangles(const GeomState *state,
                                 const uint32_t *in, uint32_t first,
                                 size_t count, uint32_t *out);

MString *pgraph_glsl_gen_geom(const GeomState *state, GenGeomGlslOptions opts);

#endif
