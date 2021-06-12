#include <X11/Xlib.h>
#include <EGL/egl.h>
#include <GL/glx.h>

#include <stdint.h>
#include <stdio.h>
#include <dlfcn.h>

#include <glvnd/libglxabi.h>

extern "C" Bool __glx_Main(
	uint32_t version,
	const __GLXapiExports *exports,
	__GLXvendorInfo *vendor,
	__GLXapiImports *imports
) {
	fprintf(stderr, "__glx_Main %08x\n", version);
	return False;
}