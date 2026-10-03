if vulkan.found()

specific_ss.add([volk, libglslang, vma, vulkan, spirv_reflect, 
	files(
		'blit.c',
		'buffer.c',
		'command.c',
		'debug.c',
		'display.c',
		'draw.c',
		'glsl.c',
		'gpuprops.c',
		'image.c',
		'instance.c',
		'renderer.c',
		'reports.c',
		'shaders.c',
		'surface-compute.c',
		'surface.c',
		'texture.c',
		'vertex.c',
		)
	])

if host_os == 'darwin'
  specific_ss.add(dependency('appleframeworks',
                             modules: ['CoreFoundation', 'IOSurface', 'OpenGL']),
                  files('iosurface.c'))
endif

endif
