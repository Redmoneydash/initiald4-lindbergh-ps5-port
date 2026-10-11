# mihawk-99 PS5_PayloadSDK build (rev adc8dd7): target libs are byte-identical
# to the old SDK plus libps5platform.a — mspace heap, thread stacks, TLS dtors.
PS5_PAYLOAD_SDK=/tmp/id4_vulkan/.deps/native/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
NATIVE_TOOL=/tmp/ps5-native-app-boilerplate/build/host/ps5-native-tool

PS5_OPENGL_PREFIX=/tmp/ps5-opengl/build/sdk/ps5-opengl-gl46

GEN=/tmp/id4_lift
RT=/tmp/lgj/lindberghrecomp/src/runtime
INC=-I$(RT) -I/tmp/lgj/lindberghrecomp/pcrecomp/runtime/recomp32_cpu -I$(GEN) \
    -I$(PS5_OPENGL_PREFIX)/include

CFLAGS=-O2 -ffreestanding -fno-builtin -fno-stack-protector \
       -Wno-attributes -Wno-unused -w \
       -DORBIS_EGL=1 -DORBIS_PAD=1 -DID4_PS5=1 \
       $(INC)

SRCS=$(wildcard $(GEN)/recomp_funcs_*.c) \
	$(RT)/guest.c $(RT)/dispatch.c $(RT)/syscall.c $(RT)/hle.c $(RT)/hle_libc.c \
	$(RT)/hle_pthread_orbis.c $(RT)/hle_matrix.c $(RT)/hle_sega.c $(RT)/hle_glut.c \
	$(RT)/hle_cxx.c $(RT)/hle_window_orbis.c $(RT)/hle_vidmode.c $(RT)/hle_gl_orbis.c $(RT)/hle_cg.c \
	$(RT)/crash_report.c $(RT)/overrides.c host.c gl_stub.c tls_fix.c crt_app.c isatty_stub.c \
	libsegaapi_ps5.c link_support.c id4_diag.c emutls_ps5.c

OBJS=$(SRCS:.c=.o)

# Linker wraps — SDK platform provides __wrap_* for the allocator family,
# fclose/fflush and pthread create/join/detach (2MiB direct-memory stacks);
# link_support.c adds pthread_exit (C++ TLS dtors) and abort (caller PC).
WRAPS=--wrap=malloc --wrap=calloc --wrap=realloc --wrap=free \
	--wrap=posix_memalign --wrap=aligned_alloc --wrap=memalign \
	--wrap=malloc_usable_size --wrap=reallocf --wrap=reallocarray \
	--wrap=getline --wrap=getdelim \
	--wrap=fclose --wrap=fflush --wrap=abort \
	--wrap=pthread_create --wrap=pthread_join --wrap=pthread_detach \
	--wrap=pthread_exit

LLD=$(PS5_PAYLOAD_SDK)/bin/prospero-lld
SDKLIB=$(PS5_PAYLOAD_SDK)/target/lib
BOILER=/tmp/ps5-native-app-boilerplate

id4.elf: $(OBJS) locals.map aco_ir_noopt.o aco_interface_noopt.o
	$(LLD) -T ps5-pie.ld --eh-frame-hdr \
	    --version-script=locals.map -z max-page-size=16384 -z now -z relro -e _start \
	    $(WRAPS) \
	    -o $@ $(OBJS) libc_bits/*.o aco_ir_noopt.o aco_interface_noopt.o \
	    --start-group -L$(PS5_OPENGL_PREFIX)/lib -L$(SDKLIB) -lPS5OpenGL -lps5platform \
	    $(SDKLIB)/libc++.a $(SDKLIB)/libc++abi.a $(SDKLIB)/libunwind.a \
	    --end-group --as-needed \
	    $(SDKLIB)/libkernel.so $(SDKLIB)/libc_stub_weak.so \
	    $(SDKLIB)/libkernel_stub_weak.so \
	    $(SDKLIB)/libSceUserService.so \
	    $(SDKLIB)/libScePad.so $(SDKLIB)/libSceVideoOut.so \
	    $(SDKLIB)/libSceSystemService.so \
	    $(SDKLIB)/libSceNet.so \
	    $(SDKLIB)/libSceAudioOut.so \
	    $(PS5_OPENGL_PREFIX)/lib/libSceAgc.so \
	    $(PS5_OPENGL_PREFIX)/lib/libSceAgcDriver.so

# eboot.elf must go through 'link' to add the process-parameters LOAD
# segment (5th LOAD). Signing raw id4.elf produces a binary the managed-app
# launcher rejects with 0x80020008 ("can't start the game or app").
eboot.elf: id4.elf
	$(NATIVE_TOOL) link --in $< --out $@ --stub-dir /tmp/stubdir \
	    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name $@

eboot.bin: eboot.elf
	$(NATIVE_TOOL) self --sign --in $< --out $@ --magic 0x1D3D154F

# --- gl_sanity: minimal known-content render test, same GL stack as the
# game but no recompiler runtime. Deployed over the same eboot.bin path. ---
GLSAN_OBJS=crt_app.o gl_sanity.o id4_diag.o link_support.o emutls_ps5.o isatty_stub.o tls_fix.o \
	libc_bits/*.o aco_ir_noopt.o aco_interface_noopt.o

glsanity.elf: crt_app.o gl_sanity.o id4_diag.o link_support.o emutls_ps5.o isatty_stub.o tls_fix.o \
	locals.map aco_ir_noopt.o aco_interface_noopt.o
	$(LLD) -T ps5-pie.ld --eh-frame-hdr \
	    --version-script=locals.map -z max-page-size=16384 -z now -z relro -e _start \
	    $(WRAPS) \
	    -o $@ $(GLSAN_OBJS) \
	    --start-group -L$(PS5_OPENGL_PREFIX)/lib -L$(SDKLIB) -lPS5OpenGL -lps5platform \
	    $(SDKLIB)/libc++.a $(SDKLIB)/libc++abi.a $(SDKLIB)/libunwind.a \
	    --end-group --as-needed \
	    $(SDKLIB)/libkernel.so $(SDKLIB)/libc_stub_weak.so \
	    $(SDKLIB)/libkernel_stub_weak.so \
	    $(SDKLIB)/libSceUserService.so \
	    $(SDKLIB)/libScePad.so $(SDKLIB)/libSceVideoOut.so \
	    $(SDKLIB)/libSceSystemService.so \
	    $(SDKLIB)/libSceNet.so \
	    $(SDKLIB)/libSceAudioOut.so \
	    $(PS5_OPENGL_PREFIX)/lib/libSceAgc.so \
	    $(PS5_OPENGL_PREFIX)/lib/libSceAgcDriver.so

glsanity_eb.elf: glsanity.elf
	$(NATIVE_TOOL) link --in $< --out $@ --stub-dir /tmp/stubdir \
	    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf

glsanity.bin: glsanity_eb.elf
	$(NATIVE_TOOL) self --sign --in $< --out $@ --magic 0x1D3D154F

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) id4.elf eboot.bin
