/*
 * Geforce NV2A PGRAPH Vulkan Renderer: IOSurface display interop (macOS)
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_IOSURFACE_H
#define HW_XBOX_NV2A_PGRAPH_VK_IOSURFACE_H

#include <stdbool.h>

/*
 * Kept free of Apple headers so it can be included next to epoxy. Surfaces
 * are BGRA8, 4 bytes per pixel.
 */
void *pgraph_vk_iosurface_create(int width, int height);
void pgraph_vk_iosurface_release(void *surface);

/*
 * Back the GL_TEXTURE_RECTANGLE currently bound in the current CGL context
 * with surface.
 */
bool pgraph_vk_iosurface_bind_gl_texture_rectangle(void *surface, int width,
                                                   int height);

#endif
