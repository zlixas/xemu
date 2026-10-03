/*
 * Geforce NV2A PGRAPH Vulkan Renderer: IOSurface display interop (macOS)
 *
 * The display image is rendered by Vulkan (MoltenVK) into an IOSurface which
 * the OpenGL UI samples without a copy through the CPU.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/gl3.h>

#include "iosurface.h"

static void set_int(CFMutableDictionaryRef dict, CFStringRef key, int32_t v)
{
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    CFDictionarySetValue(dict, key, num);
    CFRelease(num);
}

void *pgraph_vk_iosurface_create(int width, int height)
{
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    set_int(props, kIOSurfaceWidth, width);
    set_int(props, kIOSurfaceHeight, height);
    set_int(props, kIOSurfaceBytesPerElement, 4);
    set_int(props, kIOSurfacePixelFormat, 'BGRA');

    IOSurfaceRef surface = IOSurfaceCreate(props);
    CFRelease(props);
    return (void *)surface;
}

void pgraph_vk_iosurface_release(void *surface)
{
    if (surface) {
        CFRelease((IOSurfaceRef)surface);
    }
}

bool pgraph_vk_iosurface_bind_gl_texture_rectangle(void *surface, int width,
                                                   int height)
{
    CGLError err = CGLTexImageIOSurface2D(
        CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE, GL_RGBA, width, height,
        GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, (IOSurfaceRef)surface, 0);
    return err == kCGLNoError;
}
