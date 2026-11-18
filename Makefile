XBE_TITLE = xemu\ shader\ benchmark
GEN_XISO = xemu-shaderbench.iso
SRCS = $(CURDIR)/main.c
SHADER_OBJS = vs_light.inl vs_heavy.inl ps_light.inl ps_heavy.inl
NXDK_DIR ?= $(CURDIR)/../nxdk

include $(NXDK_DIR)/Makefile

# Shaders are hand-written NV_vertex_program / nvparse assembly, so they skip
# the Cg compiler and go straight to nxdk's microcode translators.
%.inl: %.vp $(VP20COMPILER)
	@echo "[ VP20     ] $@"
	$(VE) $(VP20COMPILER) $< > $@

%.inl: %.fp $(FP20COMPILER)
	@echo "[ FP20     ] $@"
	$(VE) $(FP20COMPILER) $< > $@
