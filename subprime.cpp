#include <X11/Xlib.h>

#include <EGL/egl.h>
#include <GL/glx.h>

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <dlfcn.h>

#include <atomic>
#include <stdexcept>
#include <unordered_map>
#include <memory>
#include <vector>

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
static XID get_new_id(Display *dpy) {
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
extern "C" XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attribList) {
	SP_TRACE("%d", screen);
	return get_visual(dpy, screen);
}

extern "C" void glXCopyContext(Display *dpy, GLXContext src, GLXContext dst, unsigned long mask) {
	SP_TRACE("");
	// TODO
}

extern "C" GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext shareList, Bool direct) {
	SP_TRACE("%d", vis->depth);
	return create_context();
}

extern "C" GLXPixmap glXCreateGLXPixmap(Display *dpy, XVisualInfo *vis, Pixmap pixmap) {
	SP_TRACE("");
	// TODO
	return get_new_id(dpy);
}

extern "C" void glXDestroyContext(Display *dpy, GLXContext ctx) {
	SP_TRACE("");
}

extern "C" void glXDestroyGLXPixmap(Display *dpy, GLXPixmap pix) {
	SP_TRACE("");
}

extern "C" int glXGetConfig(Display *dpy, XVisualInfo *vis, int attrib, int *value) {
	SP_TRACE("");
	return GLX_BAD_ATTRIBUTE;
}

extern "C" Bool glXIsDirect(Display *dpy, GLXContext ctx) {
	SP_TRACE("");
	return True;
}

extern "C" Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx) {
	SP_TRACE("");
	return True;
}

extern "C" void glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
	SP_TRACE("");
}

extern "C" void glXUseXFont(Font font, int first, int count, int listBase) {
	SP_TRACE("");
}

extern "C" void glXWaitGL() {}

extern "C" void glXWaitX() {}

static const char *glx_vendor = "gatecat";
static const char *glx_version = "1.3";
static const char *glx_extensions = "";


extern "C" const char *glXQueryServerString(Display *dpy, int screen, int name) {
	switch (name) {
		case GLX_VENDOR: return glx_vendor;
		case GLX_VERSION: return glx_version;
		case GLX_EXTENSIONS: return glx_extensions;
		default: return nullptr;
	}
}

extern "C" const char *glXGetClientString(Display *dpy, int name) {
	switch (name) {
		case GLX_VENDOR: return glx_vendor;
		case GLX_VERSION: return glx_version;
		case GLX_EXTENSIONS: return glx_extensions;
		default: return nullptr;
	}
}

extern "C" const char *glXQueryExtensionsString(Display *dpy, int screen) {
	return glx_extensions;
}

std::vector<std::unique_ptr<EGLConfig>> config_store;

extern "C" GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen, const int *attrib_list, int *nelements) {
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

extern "C" GLXContext glXCreateNewContext(Display *dpy, GLXFBConfig config, int render_type, GLXContext share_list, Bool direct) {
	SP_TRACE("");
	return create_context();
}

extern "C" GLXPbuffer glXCreatePbuffer(Display *dpy, GLXFBConfig config, const int * attrib_list) {
	SP_TRACE("");
	return get_new_id(dpy);
}

extern "C" GLXPixmap glXCreatePixmap(Display *dpy, GLXFBConfig config, Pixmap pixmap, const int *attrib_list) {
	SP_TRACE("");
	return get_new_id(dpy);
}

extern "C" GLXWindow glXCreateWindow(Display *dpy, GLXFBConfig config, Window win, const int *attrib_list) {
	SP_TRACE("");
	return get_new_id(dpy);
}

extern "C" void glXDestroyPbuffer(Display *dpy, GLXPbuffer pbuf) {
	SP_TRACE("");
}

extern "C" void glXDestroyPixmap(Display *dpy, GLXPixmap pixmap) {
	SP_TRACE("");
}

extern "C" void glXDestroyWindow(Display *dpy, GLXWindow win) {
	SP_TRACE("");
}

extern "C" int glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config, int attribute, int *value) {
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

extern "C" GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen, int *nelements) {
	SP_TRACE("");
	*nelements = 0;
	return nullptr;
}

extern "C" XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config) {
	return get_visual(dpy, 0);
}

extern "C" void glXGetSelectedEvent(Display *dpy, GLXDrawable draw, unsigned long *event_mask) {
	SP_TRACE("");
	*event_mask = 0;
}

extern "C" void glXSelectEvent(Display *dpy, GLXDrawable draw, unsigned long event_mask) {
	SP_TRACE("mask=%lu", event_mask);
}

extern "C" Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx) {
	SP_TRACE("");
	return False;
}

extern "C" int glXQueryContext(Display *dpy, GLXContext ctx, int attribute, int *value) {
	SP_TRACE("");
	return GLX_BAD_ATTRIBUTE;
}

extern "C" void glXQueryDrawable(Display *dpy, GLXDrawable draw, int attribute, unsigned int *value) {
	SP_TRACE("");
}

extern "C" Bool glXQueryVersion(Display *dpy, int *major, int *minor) {
	SP_TRACE("");
	*major = 1;
	*minor = 3;
	return True;
}

// Dispatch table for GLX functions
static const std::unordered_map<std::string, void*> glx_procs = {
	{"glXChooseVisual", reinterpret_cast<void*>(glXChooseVisual)},
	{"glXCopyContext", reinterpret_cast<void*>(glXCopyContext)},
	{"glXCreateContext", reinterpret_cast<void*>(glXCreateContext)},
	{"glXCreateGLXPixmap", reinterpret_cast<void*>(glXCreateGLXPixmap)},
	{"glXDestroyContext", reinterpret_cast<void*>(glXDestroyContext)},
	{"glXDestroyGLXPixmap", reinterpret_cast<void*>(glXDestroyGLXPixmap)},
	{"glXGetConfig", reinterpret_cast<void*>(glXGetConfig)},
	{"glXIsDirect", reinterpret_cast<void*>(glXIsDirect)},
	{"glXMakeCurrent", reinterpret_cast<void*>(glXMakeCurrent)},
	{"glXSwapBuffers", reinterpret_cast<void*>(glXSwapBuffers)},
	{"glXUseXFont", reinterpret_cast<void*>(glXUseXFont)},
	{"glXWaitGL", reinterpret_cast<void*>(glXWaitGL)},
	{"glXWaitX", reinterpret_cast<void*>(glXWaitX)},
	{"glXQueryServerString", reinterpret_cast<void*>(glXQueryServerString)},
	{"glXGetClientString", reinterpret_cast<void*>(glXGetClientString)},
	{"glXQueryExtensionsString", reinterpret_cast<void*>(glXQueryExtensionsString)},
	{"glXChooseFBConfig", reinterpret_cast<void*>(glXChooseFBConfig)},
	{"glXCreateNewContext", reinterpret_cast<void*>(glXCreateNewContext)},
	{"glXCreatePbuffer", reinterpret_cast<void*>(glXCreatePbuffer)},
	{"glXCreatePixmap", reinterpret_cast<void*>(glXCreatePixmap)},
	{"glXCreateWindow", reinterpret_cast<void*>(glXCreateWindow)},
	{"glXDestroyPbuffer", reinterpret_cast<void*>(glXDestroyPbuffer)},
	{"glXDestroyPixmap", reinterpret_cast<void*>(glXDestroyPixmap)},
	{"glXDestroyWindow", reinterpret_cast<void*>(glXDestroyWindow)},
	{"glXGetFBConfigAttrib", reinterpret_cast<void*>(glXGetFBConfigAttrib)},
	{"glXGetFBConfigs", reinterpret_cast<void*>(glXGetFBConfigs)},
	{"glXGetVisualFromFBConfig", reinterpret_cast<void*>(glXGetVisualFromFBConfig)},
	{"glXGetSelectedEvent", reinterpret_cast<void*>(glXGetSelectedEvent)},
	{"glXSelectEvent", reinterpret_cast<void*>(glXSelectEvent)},
	{"glXMakeContextCurrent", reinterpret_cast<void*>(glXMakeContextCurrent)},
	{"glXQueryContext", reinterpret_cast<void*>(glXQueryContext)},
	{"glXQueryDrawable", reinterpret_cast<void*>(glXQueryDrawable)},
	{"glXQueryVersion", reinterpret_cast<void*>(glXQueryVersion)},
};

extern "C" void (*glXGetProcAddress(const GLubyte *procName))(void) {
	std::string name_str(reinterpret_cast<const char*>(procName));
	SP_TRACE("%s", name_str.c_str());

	auto glx_fnd = glx_procs.find(name_str);
	if (glx_fnd != glx_procs.end())
		return reinterpret_cast<void(*)(void)>(glx_fnd->second);
	// Unsupported extensions
	if (name_str == "glXImportContextEXT" ||
		name_str == "glXFreeContextEXT" ||
		name_str == "glXCreateContextAttribsARB")
		return reinterpret_cast<void(*)(void)>(0);
	// Use EGL for the base OpenGL functions
	return eglGetProcAddress(name_str.c_str());
}
