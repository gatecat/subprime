#include <X11/Xlib.h>

#include <EGL/egl.h>
#include <GL/glx.h>

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <dlfcn.h>

#include <glvnd/libeglabi.h>
#include <glvnd/libglxabi.h>

#include <atomic>
#include <stdexcept>
#include <unordered_map>
#include <memory>
#include <vector>

namespace {

static const __GLXapiExports *api_exports = nullptr;
static bool trace_en;

#define SP_TRACE(...) do { if(trace_en) { \
		fprintf(stderr, "[subprime] %s: ", __func__); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
		} } while(0)

#define SP_CHECK(func) do { EGLBoolean result = func; \
		if (result != EGL_TRUE) \
			fprintf(stderr, "[subprime] `%s` returned %d, err=%d\n", #func, result, eglGetError()); \
		} while (0)

// Helper functions
XID get_new_id(Display *dpy) {
    XID id;
    XLockDisplay(dpy);
    id = XAllocID(dpy);
    XUnlockDisplay(dpy);
    return id;
}

struct GLXConfigImpl {};
GLXContext create_context() {
	return reinterpret_cast<GLXContext>(new GLXConfigImpl());
};
GLXConfigImpl &get_context(GLXContext ctx) {
	return *reinterpret_cast<GLXConfigImpl *>(ctx);
}

static std::atomic_flag egl_initialised{};

EGLDisplay disp() {
	EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!egl_initialised.test_and_set()) {
		SP_CHECK(eglInitialize(d, nullptr, nullptr));
	}
	return d;
}

static XVisualInfo* get_visual(Display *dpy, int screen)
{
    XVisualInfo *ret = new XVisualInfo;
    if (XMatchVisualInfo(dpy, screen,
                         DefaultDepth(dpy, screen),
                         TrueColor,
                         ret) == 0) {
        return nullptr;
    }
    return ret;
}

// GLX-EGL shimming
static const std::unordered_map<int, int> egl_attr_map = {
	{GLX_BUFFER_SIZE, EGL_BUFFER_SIZE},
	{GLX_LEVEL, EGL_LEVEL},
	{GLX_RED_SIZE, EGL_RED_SIZE},
	{GLX_GREEN_SIZE, EGL_GREEN_SIZE},
	{GLX_BLUE_SIZE, EGL_BLUE_SIZE},
	{GLX_ALPHA_SIZE, EGL_ALPHA_SIZE},
	{GLX_DEPTH_SIZE, EGL_DEPTH_SIZE},
	{GLX_STENCIL_SIZE, EGL_STENCIL_SIZE},
};

std::vector<int> convert_attribute_list(const int *attrs) {
	std::vector<int> result;
	int ptr = 0;
	while (true) {
		int attr = attrs[ptr++];
		if (attr == None)
			break;
		int value = attrs[ptr++];
		auto fnd = egl_attr_map.find(attr);
		SP_TRACE("    %d = %d", attr, value);
		if (fnd != egl_attr_map.end()) {
			result.push_back(fnd->second);
			result.push_back(value);
		}
	}
	result.push_back(EGL_NONE);
	return result;
}

// Implementations of GLX API
XVisualInfo *glx_choose_visual(Display *dpy, int screen, int *attribList) {
	SP_TRACE("%d", screen);
	return get_visual(dpy, screen);
}

void glx_copy_context(Display *dpy, GLXContext src, GLXContext dst) {
	SP_TRACE("");
	// TODO
}

GLXContext glx_create_context(Display *dpy, XVisualInfo *vis, GLXContext shareList, Bool direct) {
	SP_TRACE("%d", vis->depth);
	return create_context();
}

GLXPixmap glx_create_glx_pixmap(Display *dpy, XVisualInfo *vis, Pixmap pixmap) {
	SP_TRACE("");
	// TODO
	return get_new_id(dpy);
}

void glx_destroy_context(Display *dpy, GLXContext ctx) {
	SP_TRACE("");
}

void glx_destroy_glx_pixmap(Display *dpy, GLXPixmap pix) {
	SP_TRACE("");
}

int glx_get_config(Display *dpy, XVisualInfo *vis, int attrib, int *value) {
	SP_TRACE("");
	return GLX_BAD_ATTRIBUTE;
}

Bool glx_is_direct(Display *dpy, GLXContext ctx) {
	SP_TRACE("");
	return True;
}

Bool glx_make_current(Display *dpy, GLXDrawable drawable, GLXContext ctx) {
	SP_TRACE("");
	return True;
}

void glx_swap_buffers(Display *dpy, GLXDrawable drawable) {
	SP_TRACE("");
}

void glx_use_x_font(Font font, int first, int count, int listBase) {
	SP_TRACE("");
}

void glx_wait_gl() {}

void glx_wait_x() {}

static const char *glx_vendor = "gatecat";
static const char *glx_version = "1.3";
static const char *glx_extensions = "";


const char *glx_query_server_string(Display *dpy, int screen, int name) {
	switch (name) {
		case GLX_VENDOR: return glx_vendor;
		case GLX_VERSION: return glx_version;
		case GLX_EXTENSIONS: return glx_extensions;
		default: return nullptr;
	}
}

const char *glx_get_client_string(Display *dpy, int name) {
	switch (name) {
		case GLX_VENDOR: return glx_vendor;
		case GLX_VERSION: return glx_version;
		case GLX_EXTENSIONS: return glx_extensions;
		default: return nullptr;
	}
}

const char *glx_query_extensions_string(Display *dpy, int screen) {
	return glx_extensions;
}

std::vector<std::unique_ptr<EGLConfig>> config_store;

GLXFBConfig *glx_choose_fb_config(Display *dpy, int screen, const int *attrib_list, int *nelements) {
	SP_TRACE("");
	auto conv_attrs = convert_attribute_list(attrib_list);
	std::array<EGLConfig, 256> configs_out;
	int config_count = 0;
	SP_CHECK(eglChooseConfig(disp(), conv_attrs.data(), configs_out.data(), 256, &config_count));

	GLXFBConfig *result = reinterpret_cast<GLXFBConfig*>(malloc(sizeof(GLXFBConfig) * config_count));
	SP_TRACE("count=%d", config_count);
	for (int i = 0; i < config_count; i++) {
		config_store.emplace_back(std::make_unique<EGLConfig>(configs_out.at(i)));
		result[i] = reinterpret_cast<GLXFBConfig>(config_store.back().get());
	}

	*nelements = config_count;

	return result;
}

GLXContext glx_create_new_context(Display *dpy, GLXFBConfig config, int render_type, GLXContext share_list, Bool direct) {
	SP_TRACE("");
	return create_context();
}

GLXPbuffer glx_create_pbuffer(Display *dpy, GLXFBConfig config, const int * attrib_list) {
	SP_TRACE("");
	return get_new_id(dpy);
}

GLXPixmap glx_create_pixmap(Display *dpy, GLXFBConfig config, Pixmap pixmap, const int *attrib_list) {
	SP_TRACE("");
	return get_new_id(dpy);
}

GLXWindow glx_create_window(Display *dpy, GLXFBConfig config, Window win, const int *attrib_list) {
	SP_TRACE("");
	return get_new_id(dpy);
}

void glx_destroy_pbuffer(Display *dpy, GLXPbuffer pbuf) {
	SP_TRACE("");
}

void glx_destroy_pixmap(Display *dpy, GLXPixmap pixmap) {
	SP_TRACE("");
}

void glx_destroy_window(Display *dpy, GLXWindow win) {
	SP_TRACE("");
}

int glx_get_fb_config_attrib(Display *dpy, GLXFBConfig config, int attribute, int *value) {
	SP_TRACE("%d", attribute);
	const EGLConfig &egl_cfg = *reinterpret_cast<EGLConfig*>(config);
	auto fnd_attr = egl_attr_map.find(attribute);
	if (fnd_attr != egl_attr_map.end()) {
		eglGetConfigAttrib(disp(), egl_cfg, fnd_attr->second, value);
		SP_TRACE(" val=%d", *value);
	} else if (attribute == GLX_VISUAL_ID) {
		*value = XVisualIDFromVisual(get_visual(dpy, 0)->visual);
	}
	return Success;
}

GLXFBConfig *glx_get_fb_configs(Display *dpy, int screen, int *nelements) {
	SP_TRACE("");
	*nelements = 0;
	return nullptr;
}

XVisualInfo *glx_get_visual_from_fb_config(Display *dpy, GLXFBConfig config) {
	return get_visual(dpy, 0);
}

void glx_get_selected_event(Display *dpy, GLXDrawable draw, unsigned long *event_mask) {
	SP_TRACE("");
	*event_mask = 0;
}

void glx_select_event(Display *dpy, GLXDrawable draw, unsigned long event_mask) {
	SP_TRACE("mask=%lu", event_mask);
}

Bool glx_make_context_current(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx) {
	SP_TRACE("");
	return False;
}

int glx_query_context(Display *dpy, GLXContext ctx, int attribute, int *value) {
	SP_TRACE("");
	return GLX_BAD_ATTRIBUTE;
}

int glx_query_drawable(Display *dpy, GLXDrawable draw, int attribute, unsigned int *value) {
	SP_TRACE("");
	return GLX_BAD_ATTRIBUTE;
}

// Dispatch table for GLX functions
static const std::unordered_map<std::string, void*> glx_procs = {
	{"glXChooseVisual", reinterpret_cast<void*>(glx_choose_visual)},
	{"glXCopyContext", reinterpret_cast<void*>(glx_copy_context)},
	{"glXCreateContext", reinterpret_cast<void*>(glx_create_context)},
	{"glXCreateGLXPixmap", reinterpret_cast<void*>(glx_create_glx_pixmap)},
	{"glXDestroyContext", reinterpret_cast<void*>(glx_destroy_context)},
	{"glXDestroyGLXPixmap", reinterpret_cast<void*>(glx_destroy_glx_pixmap)},
	{"glXGetConfig", reinterpret_cast<void*>(glx_get_config)},
	{"glXIsDirect", reinterpret_cast<void*>(glx_is_direct)},
	{"glXMakeCurrent", reinterpret_cast<void*>(glx_make_current)},
	{"glXSwapBuffers", reinterpret_cast<void*>(glx_swap_buffers)},
	{"glXUseXFont", reinterpret_cast<void*>(glx_use_x_font)},
	{"glXWaitGL", reinterpret_cast<void*>(glx_wait_gl)},
	{"glXWaitX", reinterpret_cast<void*>(glx_wait_x)},
	{"glXQueryServerString", reinterpret_cast<void*>(glx_query_server_string)},
	{"glXGetClientString", reinterpret_cast<void*>(glx_get_client_string)},
	{"glXQueryExtensionsString", reinterpret_cast<void*>(glx_query_extensions_string)},
	{"glXChooseFBConfig", reinterpret_cast<void*>(glx_choose_fb_config)},
	{"glXCreateNewContext", reinterpret_cast<void*>(glx_create_context)},
	{"glXCreatePbuffer", reinterpret_cast<void*>(glx_create_pbuffer)},
	{"glXCreatePixmap", reinterpret_cast<void*>(glx_create_pixmap)},
	{"glXCreateWindow", reinterpret_cast<void*>(glx_create_window)},
	{"glXDestroyPbuffer", reinterpret_cast<void*>(glx_destroy_pbuffer)},
	{"glXDestroyPixmap", reinterpret_cast<void*>(glx_destroy_pixmap)},
	{"glXDestroyWindow", reinterpret_cast<void*>(glx_destroy_window)},
	{"glXGetFBConfigAttrib", reinterpret_cast<void*>(glx_get_fb_config_attrib)},
	{"glXGetFBConfigs", reinterpret_cast<void*>(glx_get_fb_configs)},
	{"glXGetVisualFromFBConfig", reinterpret_cast<void*>(glx_get_visual_from_fb_config)},
	{"glXGetSelectedEvent", reinterpret_cast<void*>(glx_get_selected_event)},
	{"glXSelectEvent", reinterpret_cast<void*>(glx_select_event)},
	{"glXMakeContextCurrent", reinterpret_cast<void*>(glx_make_context_current)},
	{"glXQueryContext", reinterpret_cast<void*>(glx_query_context)},
	{"glXQueryDrawable", reinterpret_cast<void*>(glx_query_drawable)},
};

// Implementations of GLX vendor API functions
Bool is_screen_supported(Display *dpy, int screen) {
	return True;
};

static __EGLapiExports egl_exports;
static __EGLapiImports egl_imports;

void *get_proc_address(const GLubyte *procName) {
	std::string name_str(reinterpret_cast<const char*>(procName));
	SP_TRACE("%s", name_str.c_str());

	auto glx_fnd = glx_procs.find(name_str);
	if (glx_fnd != glx_procs.end())
		return glx_fnd->second;
	// Unsupported extensions
	if (name_str == "glXImportContextEXT" ||
		name_str == "glXFreeContextEXT" ||
		name_str == "glXCreateContextAttribsARB")
		return nullptr;
	// Use EGL for the base OpenGL functions
	return egl_imports.getProcAddress(name_str.c_str());
}

void *get_dispatch_address(const GLubyte *procName) {
	return nullptr;
}

void set_dispatch_index(const GLubyte *procName, int index) {

}

}

typedef EGLBoolean (*egl_main_t)(
	uint32_t version,
	const __EGLapiExports *exports,
	void *vendor,
	__EGLapiImports *imports
);

// The GLX vendor API entry point
extern "C" Bool __glx_Main(
	uint32_t version,
	const __GLXapiExports *exports,
	__GLXvendorInfo *vendor,
	__GLXapiImports *imports
) {
	if (GLX_VENDOR_ABI_GET_MAJOR_VERSION(version) == GLX_VENDOR_ABI_MAJOR_VERSION
		&& GLX_VENDOR_ABI_GET_MINOR_VERSION(version) >= GLX_VENDOR_ABI_MINOR_VERSION) {
		fprintf(stderr, "subprime vendor initialised (version=%08x).\n", version);
		api_exports = exports;
		imports->isScreenSupported = is_screen_supported;
		imports->getProcAddress = get_proc_address;
		imports->getDispatchAddress = get_dispatch_address;
		imports->setDispatchIndex = set_dispatch_index;

		const char *trc_env = getenv("SUBPRIME_TRACE");
		trace_en = (trc_env && std::stoi(trc_env));

		void *egl_lib = dlopen("libEGL_nvidia.so.0", RTLD_LOCAL | RTLD_LAZY);
		if (!egl_lib)
		{
			fprintf(stderr, "failed to open EGL vendor library!\n");
			exit(1);
			return False;
		}
		auto egl_main = reinterpret_cast<egl_main_t>(dlsym(egl_lib, "__egl_Main"));
		EGLBoolean egl_result = egl_main(0x0001, &egl_exports, nullptr, &egl_imports);
		SP_TRACE("egl_result=%d", egl_result);

		return True;
	}
	return False;
}
