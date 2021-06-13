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
#include <exception>

namespace {

static const __GLXapiExports *api_exports = nullptr;
static bool trace_en;

static __EGLapiExports egl_exports;
static __EGLapiImports egl_imports;


// EGL functions, dynamically loaded
static EGLBoolean (*fn_eglInitialize)(EGLDisplay display, EGLint *major, EGLint *minor);
static EGLDisplay (*fn_eglGetDisplay)(NativeDisplayType native_display);
static EGLBoolean (*fn_eglChooseConfig)(EGLDisplay display, EGLint const *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config);
static EGLBoolean (*fn_eglGetConfigAttrib)(EGLDisplay display, EGLConfig config, EGLint attribute, EGLint *value);
static EGLContext (*fn_eglCreateContext)(EGLDisplay display, EGLConfig config, EGLContext share_context, EGLint const *attrib_list);
static EGLSurface (*fn_eglCreatePbufferSurface)(EGLDisplay display, EGLConfig config, EGLint const *attrib_list);
static EGLBoolean (*fn_eglMakeCurrent)(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context);
static EGLBoolean (*fn_eglSwapBuffers)(EGLDisplay display, EGLSurface surface);
static EGLBoolean (*fn_eglBindAPI)(EGLenum api);
static EGLBoolean (*fn_eglDestroySurface)(EGLDisplay disp, EGLSurface surface);

static EGLint (*fn_eglGetError)(void);

static void (*fn_glReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* data);
static void (*fn_glReadBuffer)(GLenum mode);
static void (*fn_glFinish)(void);

#define SP_TRACE(...) do { if(trace_en) { \
		fprintf(stderr, "[subprime] %s: ", __func__); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
		} } while(0)

#define SP_CHECK(func) do { EGLBoolean result = func; \
		if (result != EGL_TRUE) { \
			fprintf(stderr, "[subprime] `%s` returned %d, err=%d\n", #func, result, fn_eglGetError()); \
			std::terminate(); \
		} } while (0)

#define SP_ASSERT(x) do { if(!(x)) {\
			fprintf(stderr, "[subprime] assert failed %s:%d '%s'\n", __FILE__, __LINE__, #x); \
			std::terminate(); \
		}} while (0)

// Helper functions
XID get_new_id(Display *dpy) {
    XID id;
    XLockDisplay(dpy);
    id = XAllocID(dpy);
    XUnlockDisplay(dpy);
    return id;
}

static std::atomic_flag egl_initialised{};

EGLDisplay disp() {
	EGLDisplay d = egl_imports.getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, nullptr, nullptr);
	if (!egl_initialised.test_and_set()) {
		SP_CHECK(fn_eglInitialize(d, nullptr, nullptr));
	}
	return d;
}


struct GLXConfigImpl {
	GLXConfigImpl(EGLContext egl_ctx) : egl_ctx(egl_ctx) {};
	EGLContext egl_ctx;
};

GLXConfigImpl &get_context(GLXContext ctx) {
	return *reinterpret_cast<GLXConfigImpl *>(ctx);
}

void get_config(EGLDisplay dp, EGLConfig *egl_cfg) {
	// TODO: proper config choosing
	int cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_NONE};
	int num_configs;
	SP_CHECK(fn_eglChooseConfig(dp, cfg_attrs, egl_cfg, 1, &num_configs));
	SP_ASSERT(num_configs > 0);
}

GLXContext create_context(GLXFBConfig cfg, GLXContext share_context) {
	EGLDisplay dp = disp();
	SP_CHECK(fn_eglBindAPI(EGL_OPENGL_API));

	EGLConfig egl_cfg;
	if (cfg == nullptr) {
		get_config(dp, &egl_cfg);
	} else {
		egl_cfg = *reinterpret_cast<EGLConfig*>(cfg);
	}

	std::array<int, 1> ctx_attrs{EGL_NONE};
	EGLContext share = EGL_NO_CONTEXT;
	if (share_context)
		share = get_context(share_context).egl_ctx;

	EGLContext egl_ctx = fn_eglCreateContext(dp, egl_cfg, share, ctx_attrs.data());

	return reinterpret_cast<GLXContext>(new GLXConfigImpl(egl_ctx));
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
	std::vector<int> result{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT};
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

struct SurfaceData {
	EGLSurface egl_sfc;
	unsigned width;
	unsigned height;
	GLXFBConfig cfg;
};

std::unordered_map<GLXDrawable, SurfaceData> drawable2surface;

std::pair<unsigned, unsigned> get_drawable_dims(Display *dpy, GLXDrawable drawable) {
	Window root;
	int x, y;
	unsigned int width = 0, height = 0, border_width, depth;
	auto st = XGetGeometry(dpy, drawable, &root, &x, &y, &width, &height, &border_width, &depth);
	return std::make_pair(width, height);
}

EGLSurface lookup_drawable(Display *dpy, GLXDrawable drawable, GLXFBConfig cfg = nullptr) {
	if (drawable2surface.count(drawable))
		return drawable2surface.at(drawable).egl_sfc;
	// Create a new backing PBuffer
	EGLDisplay dp = disp();
	EGLConfig egl_cfg;
	if (cfg == nullptr) {
		get_config(dp, &egl_cfg);
	} else {
		egl_cfg = *reinterpret_cast<EGLConfig*>(cfg);
	}
	unsigned width, height;
	std::tie(width, height) = get_drawable_dims(dpy, drawable);
	std::vector<int> attrs{EGL_WIDTH, int(width), EGL_HEIGHT, int(height), EGL_NONE};
	EGLSurface surface = fn_eglCreatePbufferSurface(dp, egl_cfg, attrs.data());
	drawable2surface[drawable].egl_sfc = surface;
	drawable2surface[drawable].width = width;
	drawable2surface[drawable].height = height;
	drawable2surface[drawable].cfg = cfg;
	return surface;
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
	return create_context(nullptr, shareList);
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

static GLXDrawable curr_drawable;
static EGLSurface curr_surface;
static EGLContext last_valid_context;
static EGLContext curr_context;

Bool glx_make_current(Display *dpy, GLXDrawable drawable, GLXContext ctx) {
	SP_TRACE("");
	EGLDisplay dp = disp();
	curr_surface = drawable ? lookup_drawable(dpy, drawable) : EGL_NO_SURFACE;
	curr_context = ctx ? get_context(ctx).egl_ctx : EGL_NO_CONTEXT;
	SP_CHECK(fn_eglMakeCurrent(dp, curr_surface, curr_surface, curr_context));
	curr_drawable = drawable;
	if (curr_context != EGL_NO_CONTEXT)
		last_valid_context = curr_context;
	return True;
}


void glx_swap_buffers(Display *dpy, GLXDrawable drawable) {
	if (!drawable2surface.count(drawable))
		return;
	auto &sfc = drawable2surface.at(drawable);
	size_t buf_size = 4 * sfc.width * sfc.height;
	uint8_t *pixel_buf = reinterpret_cast<uint8_t*>(malloc(buf_size));
	// Make sure the surface is current
	SP_CHECK(fn_eglMakeCurrent(disp(), sfc.egl_sfc, sfc.egl_sfc, last_valid_context));
	fn_glFinish();
	fn_glReadPixels(0, 0, sfc.width, sfc.height, GL_BGRA, GL_UNSIGNED_BYTE, pixel_buf);
	SP_CHECK(fn_eglMakeCurrent(disp(), curr_surface, curr_surface, curr_context));
	// Unmirror
	for (int y = 0; y < (sfc.height / 2); y++) {
		std::swap_ranges(pixel_buf + (y * sfc.width * 4), pixel_buf + ((y + 1) * sfc.width * 4),
			pixel_buf + (((sfc.height - 1) - y) * sfc.width * 4));
	}
	XImage *img = XCreateImage(dpy, get_visual(dpy, 0)->visual, 24, ZPixmap, 0,
		reinterpret_cast<char*>(pixel_buf), sfc.width, sfc.height, 32, 0);
	SP_ASSERT(img != nullptr);
	GC gc = XCreateGC(dpy, drawable, 0, NULL);
	XPutImage(dpy, drawable, gc, img, 0, 0, 0, 0, sfc.width, sfc.height);
	XFlush(dpy);
	XDestroyImage(img);
	// Also, check if the buffer was resized
	unsigned new_width, new_height;
	std::tie(new_width, new_height) = get_drawable_dims(dpy, drawable);
	if (new_width != sfc.width || new_height != sfc.height) {
		// Destroy old surface and recreate with new size
		if (curr_drawable == drawable)
			SP_CHECK(fn_eglMakeCurrent(disp(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
		SP_CHECK(fn_eglDestroySurface(disp(), sfc.egl_sfc));
		GLXFBConfig cfg = sfc.cfg;
		drawable2surface.erase(drawable);
		EGLSurface new_sfc = lookup_drawable(dpy, drawable, cfg);
		// If it was current before, make it current again
		if (curr_drawable == drawable)
			SP_CHECK(fn_eglMakeCurrent(disp(), new_sfc, new_sfc, curr_context));
	}
}

void glx_use_x_font(Font font, int first, int count, int listBase) {
	SP_TRACE("");
}

void glx_wait_gl() {}

void glx_wait_x() {}

static const char *glx_vendor = "gatecat";
static const char *glx_version = "1.4 subprime";
static const char *glx_extensions = "GLX_ARB_create_context GLX_ARB_create_context_profile";


const char *glx_query_server_string(Display *dpy, int screen, int name) {
	switch (name) {
		case GLX_VENDOR: return glx_vendor;
		case GLX_VERSION: return glx_version;
		case GLX_EXTENSIONS: return glx_extensions;
		default: return nullptr;;
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
	SP_CHECK(fn_eglChooseConfig(disp(), conv_attrs.data(), configs_out.data(), 256, &config_count));

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
	return create_context(config, share_list);
}

GLXPbuffer glx_create_pbuffer(Display *dpy, GLXFBConfig config, const int * attrib_list) {
	SP_TRACE("");
	return get_new_id(dpy);
}

GLXPixmap glx_create_pixmap(Display *dpy, GLXFBConfig config, Pixmap pixmap, const int *attrib_list) {
	SP_TRACE("");
	lookup_drawable(dpy, pixmap, config);
	return pixmap;
}

GLXWindow glx_create_window(Display *dpy, GLXFBConfig config, Window win, const int *attrib_list) {
	SP_TRACE("");
	lookup_drawable(dpy, win, config);
	return win;
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
		fn_eglGetConfigAttrib(disp(), egl_cfg, fnd_attr->second, value);
		SP_TRACE(" val=%d", *value);
	} else if (attribute == GLX_VISUAL_ID) {
		*value = XVisualIDFromVisual(get_visual(dpy, 0)->visual);
	} else if (attribute == GLX_DRAWABLE_TYPE) {
		*value = (GLX_WINDOW_BIT | GLX_PIXMAP_BIT | GLX_PBUFFER_BIT);
	} else if (attribute == GLX_RENDER_TYPE) {
		*value = GLX_RGBA_BIT;
	} else if (attribute == GLX_X_RENDERABLE) {
		*value = True;
	} else if (attribute == GLX_MAX_PBUFFER_WIDTH || attribute == GLX_MAX_PBUFFER_HEIGHT) {
		*value = 4096;
	} else if (attribute == GLX_MAX_PBUFFER_PIXELS) {
		*value = 4096*4096;
	}
	return Success;
}

GLXFBConfig *glx_get_fb_configs(Display *dpy, int screen, int *nelements) {
	SP_TRACE("");
	int default_attrs[] = {None};
	return glx_choose_fb_config(dpy, screen, default_attrs, nelements);
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

GLXContext glx_create_context_attribs_arb(Display *dpy, GLXFBConfig config, GLXContext share_list, Bool direct, const int *attrib_list) {
	SP_TRACE("");
	return create_context(config, share_list);
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
	{"glXCreateNewContext", reinterpret_cast<void*>(glx_create_new_context)},
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
	{"glXCreateContextAttribsARB", reinterpret_cast<void*>(glx_create_context_attribs_arb)}
};

// Implementations of GLX vendor API functions
Bool is_screen_supported(Display *dpy, int screen) {
	return True;
};

void *get_proc_address(const GLubyte *procName) {
	std::string name_str(reinterpret_cast<const char*>(procName));
	// SP_TRACE("%s", name_str.c_str());

	auto glx_fnd = glx_procs.find(name_str);
	if (glx_fnd != glx_procs.end())
		return glx_fnd->second;
	// Unsupported extensions
	if (name_str == "glXImportContextEXT" ||
		name_str == "glXFreeContextEXT")
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

#define LOAD_FN(x) do { fn_##x = reinterpret_cast<decltype(fn_##x)>(egl_imports.getProcAddress(#x)); } while (0)

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

		LOAD_FN(eglInitialize);
		LOAD_FN(eglGetDisplay);
		LOAD_FN(eglChooseConfig);
		LOAD_FN(eglGetConfigAttrib);
		LOAD_FN(eglCreateContext);
		LOAD_FN(eglGetError);
		LOAD_FN(eglCreatePbufferSurface);
		LOAD_FN(eglMakeCurrent);
		LOAD_FN(eglSwapBuffers);
		LOAD_FN(eglBindAPI);
		LOAD_FN(eglDestroySurface);
		LOAD_FN(glReadBuffer);
		LOAD_FN(glReadPixels);
		LOAD_FN(glFinish);

		if (egl_imports.patchThreadAttach) {
			egl_imports.patchThreadAttach();
		}

		return True;
	}
	return False;
}
