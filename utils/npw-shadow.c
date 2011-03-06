#define _GNU_SOURCE 1
#include "sysdeps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/StringDefs.h>

#define XP_UNIX 1
#define MOZ_X11 1
#include <npapi.h>
#include <npupp.h>

#define DEBUG 1
#include "debug.h"


// XXX unimplemented functions
#define UNIMPLEMENTED() npw_printf("WARNING: Unimplemented function %s at line %d\n", __func__, __LINE__)

// Path to plugin to use
static const char *g_plugin_path = "/home/gb/npshadow-impl.so";

// Netscape exported functions
static NPNetscapeFuncs g_mozilla_funcs;

// Functions supplied by the plug-in
static NPPluginFuncs g_plugin_funcs;

// Plugin instance
typedef struct {
  NPP instance;
  NPP npn_instance;
  Window old_window;
  Window window;
  Widget top_widget;
  Widget form;
  int width, height;
} PluginInstance;

// Plugin implementation
static int g_plugin_initialized = 0;
static void *g_plugin_handle = NULL;

// Allows the browser to query the plug-in supported formats
typedef char * (*NP_GetMIMEDescriptionUPP)(void);
static NP_GetMIMEDescriptionUPP g_NP_GetMIMEDescription = NULL;

// Allows the browser to query the plug-in for information
typedef NPError (*NP_GetValueUPP)(void *instance, NPPVariable variable, void *value);
static NP_GetValueUPP g_NP_GetValue = NULL;

// Provides global initialization for a plug-in
typedef NPError (*NP_InitializeUPP)(NPNetscapeFuncs *moz_funcs, NPPluginFuncs *plugin_funcs);
static NP_InitializeUPP g_NP_Initialize = NULL;

// Provides global deinitialization for a plug-in
typedef NPError (*NP_ShutdownUPP)(void);
static NP_ShutdownUPP g_NP_Shutdown = NULL;


/* ====================================================================== */
/* === X Toolkit glue                                                 === */
/* ====================================================================== */

static Display *x_display;
static XtAppContext x_app_context;

typedef struct _XtTMRec {
    XtTranslations  translations;       /* private to Translation Manager    */
    XtBoundActions  proc_table;         /* procedure bindings for actions    */
    struct _XtStateRec *current_state;  /* Translation Manager state ptr     */
    unsigned long   lastEventTime;
} XtTMRec, *XtTM;   

typedef struct _CorePart {
    Widget          self;               /* pointer to widget itself          */
    WidgetClass     widget_class;       /* pointer to Widget's ClassRec      */
    Widget          parent;             /* parent widget                     */
    XrmName         xrm_name;           /* widget resource name quarkified   */
    Boolean         being_destroyed;    /* marked for destroy                */
    XtCallbackList  destroy_callbacks;  /* who to call when widget destroyed */
    XtPointer       constraints;        /* constraint record                 */
    Position        x, y;               /* window position                   */
    Dimension       width, height;      /* window dimensions                 */
    Dimension       border_width;       /* window border width               */
    Boolean         managed;            /* is widget geometry managed?       */
    Boolean         sensitive;          /* is widget sensitive to user events*/
    Boolean         ancestor_sensitive; /* are all ancestors sensitive?      */
    XtEventTable    event_table;        /* private to event dispatcher       */
    XtTMRec         tm;                 /* translation management            */
    XtTranslations  accelerators;       /* accelerator translations          */
    Pixel           border_pixel;       /* window border pixel               */
    Pixmap          border_pixmap;      /* window border pixmap or NULL      */
    WidgetList      popup_list;         /* list of popups                    */
    Cardinal        num_popups;         /* how many popups                   */
    String          name;               /* widget resource name              */
    Screen          *screen;            /* window's screen                   */
    Colormap        colormap;           /* colormap                          */
    Window          window;             /* window ID                         */
    Cardinal        depth;              /* number of planes in window        */
    Pixel           background_pixel;   /* window background pixel           */
    Pixmap          background_pixmap;  /* window background pixmap or NULL  */
    Boolean         visible;            /* is window mapped and not occluded?*/
    Boolean         mapped_when_managed;/* map window if it's managed?       */
} CorePart;

typedef struct _WidgetRec {
    CorePart    core;
} WidgetRec, CoreRec;   


/* ====================================================================== */
/* === Plugin glue                                                   === */
/* ====================================================================== */

// Initialize plugin wrapper
static void __attribute__((constructor))
plugin_init(void)
{
  if (g_plugin_initialized)
	return;

  D(bug("plugin_init\n"));

  XtToolkitInitialize();
  x_app_context = XtCreateApplicationContext();
  int argc = 0;
  x_display = XtOpenDisplay(x_app_context, NULL, "npw-viewer", "npw-viewer", NULL, 0, &argc, NULL);

  if ((g_plugin_handle = dlopen(g_plugin_path, RTLD_LAZY)) == NULL) {
	npw_printf("ERROR: %s\n", dlerror());
	g_plugin_initialized = -1;
	return;
  }
  {
	const char *error;
	dlerror();
	g_NP_GetMIMEDescription = (NP_GetMIMEDescriptionUPP)dlsym(g_plugin_handle, "NP_GetMIMEDescription");
	if ((error = dlerror()) != NULL) {
	  npw_printf("ERROR: %s\n", error);
	  g_plugin_initialized = -1;
	  return;
	}
	g_NP_Initialize = (NP_InitializeUPP)dlsym(g_plugin_handle, "NP_Initialize");
	if ((error = dlerror()) != NULL) {
	  npw_printf("ERROR: %s\n", error);
	  g_plugin_initialized = -1;
	  return;
	}
	g_NP_Shutdown = (NP_ShutdownUPP)dlsym(g_plugin_handle, "NP_Shutdown");
	if ((error = dlerror()) != NULL) {
	  npw_printf("ERROR: %s\n", error);
	  g_plugin_initialized = -1;
	  return;
	}
	g_NP_GetValue = (NP_GetValueUPP)dlsym(g_plugin_handle, "NP_GetValue");
  }

  g_plugin_initialized = 1;
}

// Kill plugin wrapper
static void __attribute__((destructor))
plugin_exit(void)
{
  D(bug("plugin_exit\n"));

  if (g_plugin_handle)
	dlclose(g_plugin_handle);
}


/* ====================================================================== */
/* === Browser side plug-in API                                       === */
/* ====================================================================== */

// Closes and deletes a stream
NPError
g_NPN_DestroyStream(NPP instance, NPStream *stream, NPError reason)
{
  D(bug("NPN_DestroyStream instance=%p\n", instance));
  UNIMPLEMENTED();

  return NPERR_GENERIC_ERROR;
}

// Forces a repaint message for a windowless plug-in
void
g_NPN_ForceRedraw(NPP instance)
{
  D(bug("NPN_ForceRedraw instance=%p\n", instance));
  UNIMPLEMENTED();
}

// Asks the browser to create a stream for the specified URL
NPError
g_NPN_GetURL(NPP instance, const char *url, const char *target)
{
  D(bug("NPN_GetURL instance=%p\n", instance));

  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  PluginInstance *plugin = instance->ndata;

  if (!g_mozilla_funcs.geturl)
	return NPERR_GENERIC_ERROR;

  D(bug(" instance=%p, url='%s', target='%s'\n", plugin->npn_instance, url, target));
  NPError ret = g_mozilla_funcs.geturl(plugin->npn_instance, url, target);

  D(bug(" return: %d\n", ret));
  return ret;
}

// Requests creation of a new stream with the contents of the specified URL
NPError
g_NPN_GetURLNotify(NPP instance, const char *url, const char *target, void *notifyData)
{
  D(bug("NPN_GetURLNotify instance=%p\n", instance));

  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  PluginInstance *plugin = instance->ndata;

  NPP the_instance;
  if (plugin->instance->pdata == plugin)
	the_instance = plugin->npn_instance;
  else
	the_instance = instance;

  if (!g_mozilla_funcs.geturlnotify)
	return NPERR_GENERIC_ERROR;

  D(bug(" instance=%p, url='%s', target='%s', notifyData=%p\n", the_instance, url, target, notifyData));
  NPError ret = g_mozilla_funcs.geturlnotify(plugin->npn_instance, url, target, notifyData);

  D(bug(" return: %d\n", ret));
  return ret;
}

// Allows the plug-in to query the browser for information
NPError
g_NPN_GetValue(NPP instance, NPNVariable variable, void *value)
{
  D(bug("NPN_GetValue instance=%p, variable=%d\n", instance, variable));

#if 0
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  PluginInstance *plugin = instance->ndata;

  if (!g_mozilla_funcs.getvalue)
	return NPERR_GENERIC_ERROR;

  NPError ret = g_mozilla_funcs.getvalue(plugin->npn_instance, variable, value);
#else
  NPError ret;
  switch (variable) {
  case NPNVxDisplay:
	*(void **)value = x_display;
	ret = NPERR_NO_ERROR;
	break;
  case NPNVxtAppContext:
	*(void **)value = XtDisplayToApplicationContext(x_display);
	ret = NPERR_NO_ERROR;
	break;
  default:
	npw_printf("WARNING: unhandled variable %d for NPN_GetValue\n", variable);
	ret = NPERR_INVALID_PARAM;
  }
#endif

  D(bug(" return: %d\n", ret));
  return ret;
}

// Invalidates specified drawing area prior to repainting or refreshing a windowless plug-in
void
g_NPN_InvalidateRect(NPP instance, NPRect *invalidRect)
{
  D(bug("NPN_InvalidateRect instance=%p\n", instance));
  UNIMPLEMENTED();
}

// Invalidates specified region prior to repainting or refreshing a windowless plug-in
void
g_NPN_InvalidateRegion(NPP instance, NPRegion invalidRegion)
{
  D(bug("NPN_InvalidateRegion instance=%p\n", instance));
  UNIMPLEMENTED();
}

// Allocates memory from the browser's memory space
void *
g_NPN_MemAlloc(uint32 size)
{
  D(bug("NPN_MemAlloc size=%d\n", size));

  return malloc(size);
}

// Requests that the browser free a specified amount of memory
uint32
g_NPN_MemFlush(uint32 size)
{
  D(bug("NPN_MemFlush size=%d\n", size));
  UNIMPLEMENTED();

  return 0;
}

// Deallocates a block of allocated memory
void
g_NPN_MemFree(void *ptr)
{
  D(bug("NPN_MemFree ptr=%p\n", ptr));

  free(ptr);
}

// Requests the creation of a new data stream produced by the plug-in and consumed by the browser
NPError
g_NPN_NewStream(NPP instance, NPMIMEType type, const char *target, NPStream **stream)
{
  D(bug("NPN_NewStream instance=%p\n", instance));
  UNIMPLEMENTED();

  return NPERR_GENERIC_ERROR;
}

// Posts data to a URL
NPError
g_NPN_PostURL(NPP instance, const char *url, const char *target, uint32 len, const char *buf, NPBool file)
{
  D(bug("NPN_PostURL instance=%p\n", instance));
  UNIMPLEMENTED();

  return NPERR_GENERIC_ERROR;
}

// Posts data to a URL, and receives notification of the result
NPError
g_NPN_PostURLNotify(NPP instance, const char *url, const char *target, uint32 len, const char *buf, NPBool file, void *notifyData)
{
  D(bug("NPN_PostURLNotify instance=%p\n", instance));
  UNIMPLEMENTED();

  return NPERR_GENERIC_ERROR;
}

// Posts data to a URL, and receives notification of the result
void
g_NPN_ReloadPlugins(NPBool reloadPages)
{
  D(bug("NPN_ReloadPlugins reloadPages=%d\n", reloadPages));
  UNIMPLEMENTED();
}

// Returns the Java execution environment
JRIEnv *
g_NPN_GetJavaEnv(void)
{
  D(bug("NPN_GetJavaEnv\n"));
  UNIMPLEMENTED();

  return NULL;
}

// Returns the Java object associated with the plug-in instance
jref
g_NPN_GetJavaPeer(NPP instance)
{
  D(bug("NPN_GetJavaPeer instance=%p\n", instance));
  UNIMPLEMENTED();

  return NULL;
}

// Requests a range of bytes for a seekable stream
NPError
g_NPN_RequestRead(NPStream *stream, NPByteRange *rangeList)
{
  D(bug("NPN_RequestRead stream=%p\n", stream));
  UNIMPLEMENTED();

  return NPERR_GENERIC_ERROR;
}

// Sets various modes of plug-in operation
NPError
g_NPN_SetValue(NPP instance, NPPVariable variable, void *value)
{
  D(bug("NPN_SetValue instance=%p\n", instance));
  UNIMPLEMENTED();

  return NPERR_GENERIC_ERROR;
}

// Displays a message on the status line of the browser window
void
g_NPN_Status(NPP instance, const char *message)
{
  D(bug("NPN_Status instance=%p\n", instance));

  if (instance == NULL)
	return;

  PluginInstance *plugin = instance->ndata;

  if (!g_mozilla_funcs.status)
	return;

  g_mozilla_funcs.status(plugin->npn_instance, message);
}

// Returns the browser's user agent field
const char *
g_NPN_UserAgent(NPP instance)
{
  D(bug("NPN_UserAgent instance=%p\n", instance));

  NPP my_instance = NULL;
  if (instance)
	my_instance = ((PluginInstance *)instance->ndata)->npn_instance;

  if (!g_mozilla_funcs.uagent)
	return NULL;

  //  const char *user_agent = g_mozilla_funcs.uagent(my_instance);
  const char *user_agent = NULL;

  D(bug(" user_agent='%s'\n", user_agent));
  return user_agent;
}

// Returns version information for the Plug-in API
void
g_NPN_Version(int *plugin_major, int *plugin_minor, int *netscape_major, int *netscape_minor)
{
  D(bug("NPN_Version\n"));
  UNIMPLEMENTED();
}

// Pushes data into a stream produced by the plug-in and consumed by the browser
int32
g_NPN_Write(NPP instance, NPStream *stream, int32 len, void *buf)
{
  D(bug("NPN_Write instance=%d\n", instance));
  UNIMPLEMENTED();

  return -1;
}


/* ====================================================================== */
/* === Plug-in side data                                              === */
/* ====================================================================== */

// Creates a new instance of a plug-in
static NPError
g_NPP_New(NPMIMEType mime_type, NPP instance,
		  uint16_t mode, int16_t argc, char *argn[], char *argv[],
		  NPSavedData *saved)
{
  D(bug("NPP_New instance=%p\n", instance));

  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  if (!g_plugin_funcs.newp)
	return NPERR_GENERIC_ERROR;

  if ((uintptr_t)instance > 0xffffffffUL) {
	npw_printf("ERROR: FIXME: 64-bit instance doesn't fit!\n");
	return NPERR_GENERIC_ERROR;
  }

  if (saved)
	npw_printf("WARNING: NPP_New with non-null saved arguments\n");

  PluginInstance *plugin = malloc(sizeof(*plugin));
  if (plugin == NULL)
	return NPERR_OUT_OF_MEMORY_ERROR;
  memset(plugin, 0, sizeof(*plugin));
  plugin->instance = malloc(sizeof(*plugin->instance));
  if (plugin->instance == NULL)
	return NPERR_OUT_OF_MEMORY_ERROR;
  plugin->instance->ndata = plugin;

  instance->pdata = (void *)plugin;
  plugin->npn_instance = instance;
  NPError ret = g_plugin_funcs.newp(mime_type, plugin->instance, mode, argc, argn, argv, saved);
  if (plugin->npn_instance != instance)
	npw_printf("############# OOOFDOFDF \n");
  D(bug(" plugin instance: %p\n", plugin->instance));
  instance->pdata = (void *)plugin;
  plugin->npn_instance = instance;

  D(bug(" return: %d\n", ret));
  return ret;
}

// Deletes a specific instance of a plug-in
static NPError
g_NPP_Destroy(NPP instance, NPSavedData **save)
{
  D(bug("NPP_Destroy instance=%p\n", instance));

  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  PluginInstance *plugin = instance->pdata;

  if (!g_plugin_funcs.destroy)
	return NPERR_GENERIC_ERROR;

  NPError ret = g_plugin_funcs.destroy(plugin->instance, save);
  free(plugin->instance);

  D(bug(" return: %d\n", ret));
  return ret;
}

// Tells the plug-in when a window is created, moved, sized, or destroyed
static NPError
g_NPP_SetWindow(NPP instance, NPWindow *io_window)
{
  D(bug("NPP_SetWindow instance=%p\n", instance));

  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  PluginInstance *plugin = instance->pdata;

  if (!g_plugin_funcs.setwindow)
	return NPERR_GENERIC_ERROR;

  if (io_window == NULL)
	npw_printf("NULL window\n");

  if (io_window == NULL)
	return NPERR_NO_ERROR;

#if 0
  NPWindow the_window, *window = &the_window;
  window->window = io_window->window;
  window->x = io_window->x;
  window->y = io_window->y;
  window->width = io_window->width;
  window->height = io_window->height;
  window->type = io_window->type;
  window->clipRect.top = io_window->clipRect.top;
  window->clipRect.left = io_window->clipRect.left;
  window->clipRect.bottom = io_window->clipRect.bottom;
  window->clipRect.right = io_window->clipRect.right;

  NPSetWindowCallbackStruct *io_ws_info = io_window->ws_info;

  NPSetWindowCallbackStruct the_ws_info, *ws_info = &the_ws_info;
  window->ws_info = ws_info;
  ws_info->type = io_ws_info->type;
  ws_info->depth = io_ws_info->depth;
  ws_info->display = x_display;
  XVisualInfo visualInfo;
  int screen = DefaultScreen(ws_info->display);
  if (!XMatchVisualInfo(ws_info->display, screen, ws_info->depth, io_ws_info->visual->class, &visualInfo)) {
	npw_printf("ERROR: could not reconstruct visual info for NPP_SetWindow\n");
	return NPERR_GENERIC_ERROR;
  }
  ws_info->colormap = DefaultColormap(ws_info->display, screen);
  ws_info->visual = visualInfo.visual;

#if 0
  ws_info->display = io_ws_info->display;
  ws_info->visual = io_ws_info->visual;
  ws_info->colormap = io_ws_info->colormap;
#endif

  D(bug(" window size = %d x %d\n", window->width, window->height));

#if 1
  if (plugin->window) {
	if (plugin->width != window->width || plugin->height != window->height) {
	  D(bug(" resize window\n"));
	  plugin->width = window->width;
	  plugin->height = window->height;
	  XResizeWindow(x_display, plugin->window, plugin->width, plugin->height);
	}
  }
  else {
	Arg args[7];
	XSetWindowAttributes attr;
	unsigned long mask;
	int n;
	Widget top_widget, form;

	mask = CWEventMask;
	attr.event_mask =
	  ButtonMotionMask |
	  ButtonPressMask |
	  ButtonReleaseMask |
	  KeyPressMask |
	  KeyReleaseMask |
	  EnterWindowMask |
	  LeaveWindowMask |
	  PointerMotionMask |
	  StructureNotifyMask |
	  VisibilityChangeMask |
	  FocusChangeMask |
	  ExposureMask;

	plugin->width = window->width;
	plugin->height = window->height;
	plugin->window = XCreateWindow(x_display, (Window)window->window,
								   0, 0, window->width, window->height,
								   0, ws_info->depth, InputOutput, ws_info->visual, mask, &attr);
	XMapWindow(x_display, plugin->window);
	XFlush(x_display);

	String app_name, app_class;
	XtGetApplicationNameAndClass(x_display, &app_name, &app_class);

	top_widget = XtAppCreateShell("drawingArea", app_class, applicationShellWidgetClass, x_display, NULL, 0);
	plugin->top_widget = top_widget;

	n = 0;
	XtSetArg(args[n], XtNwidth, window->width); n++;
	XtSetArg(args[n], XtNheight, window->height); n++;
	XtSetArg(args[n], XtNvisual, ws_info->visual); n++;
	XtSetArg(args[n], XtNdepth, ws_info->depth); n++;
	XtSetArg(args[n], XtNcolormap, ws_info->colormap ); n++;
	XtSetArg(args[n], XtNborderWidth, 0); n++;
	XtSetValues(top_widget, args, n);

	form = XtVaCreateWidget("form", compositeWidgetClass, top_widget, NULL);
	plugin->form = form;

	n = 0;
	XtSetArg(args[n], XtNwidth, window->width); n++;
	XtSetArg(args[n], XtNheight, window->height); n++;
	XtSetArg(args[n], XtNvisual, ws_info->visual); n++;
	XtSetArg(args[n], XtNdepth, ws_info->depth); n++;
	XtSetArg(args[n], XtNcolormap, ws_info->colormap ); n++;
	XtSetArg(args[n], XtNborderWidth, 0); n++;
	XtSetValues(form, args, n);

#if 1
	plugin->old_window = top_widget->core.window;
	top_widget->core.window = plugin->window;
#endif

	XtRegisterDrawable(x_display, plugin->window, top_widget);
	XtRealizeWidget(form);
	XtManageChild(form);

	plugin->window = XtWindow(plugin->form);

	XSelectInput(x_display, XtWindow(top_widget), 0x0fffff);
	XSelectInput(x_display, XtWindow(form), 0x0fffff);

	XSync(x_display, False);
  }
  D(bug(" old window id %p\n", window->window));
  window->window = (void *)XtWindow(plugin->form);
#endif

#if 0
  D(bug(" window=%d:%p at (%d,%d), size=%dx%d\n", (Window)window->window, window->window, window->x, window->y, window->width, window->height));
  D(bug(" clipRect={ {%d,%d}, {%d,%d} }\n", window->clipRect.top, window->clipRect.left, window->clipRect.bottom, window->clipRect.right));
  D(bug(" type=%s, %d:%d\n", window->type == NPWindowTypeWindow ? "Window" : (window->type == NPWindowTypeDrawable ? "Drawable" : "Unknown"), window->type, ws_info->type));
  D(bug(" display=%p[%s], visual=%p, colormap=%d, depth=%d\n", ws_info->display, DisplayString(ws_info->display), ws_info->visual, ws_info->colormap, ws_info->depth));
  Visual *vis = ws_info->visual;
  D(bug(" Visual from visualInfo=%p { %d, %d, %x, %x, %x, %d, %d }\n", vis,
		vis->visualid, vis->class, vis->red_mask, vis->green_mask, vis->blue_mask, vis->bits_per_rgb, vis->map_entries));
#endif
#else
  NPWindow *window = io_window;
#endif

  D(bug(" window id %p\n", window->window));
  NPError ret = g_plugin_funcs.setwindow(plugin->instance, window);

  D(bug(" return: %d\n", ret));
  return ret;
}

// Allows the browser to query the plug-in for information
static NPError
g_NPP_GetValue(NPP instance, NPPVariable variable, void *value)
{
  D(bug("NPP_GetValue instance=%p, variable=%d\n", instance, variable));

  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  PluginInstance *plugin = instance->pdata;

  if (!g_plugin_funcs.getvalue)
	return NPERR_GENERIC_ERROR;

  NPError ret = NPERR_GENERIC_ERROR;

  switch (variable) {
  case NPPVpluginNameString:
  case NPPVpluginDescriptionString:
	{
	  char *s;
	  ret = g_plugin_funcs.getvalue(plugin->instance, variable, &s);
	  if (ret == NPERR_NO_ERROR) {
		D(bug(" value='%s'\n", s));
		*((char **)value) = s;
	  }
	  break;
	}
  case NPPVpluginWindowBool:
  case NPPVpluginTransparentBool:
  case NPPVpluginWindowSize:
  case NPPVpluginTimerInterval:
	{
	  uint32_t n;
	  ret = g_plugin_funcs.getvalue(plugin->instance, variable, &n);
	  if (ret == NPERR_NO_ERROR) {
		D(bug(" value=%d\n", n));
		*((int *)value) = n;
	  }
	  break;
	}
  case NPPVpluginNeedsXEmbed:
	{
	  PRBool b;
	  ret = g_plugin_funcs.getvalue(plugin->instance, variable, &b);
	  if (ret == NPERR_NO_ERROR) {
#if 1
		// XXX Flash plugin hack
		npw_printf(" XEmbed not supported yet\n");
		b = FALSE;
#else
		D(bug(" value=%s\n", b ? "true" : "false"));
#endif
		*((PRBool *)value) = b;
	  }
	  break;
	}
  }

  D(bug(" return: %d\n", ret));
  return ret;
}

// Sets information about the plug-in
static NPError
g_NPP_SetValue(NPP instance, NPPVariable variable, void *value)
{
  D(bug("NPP_SetValue instance=%p, variable=%d\n", instance, variable));
  UNIMPLEMENTED();

  return NPERR_GENERIC_ERROR;
}

// Notifies the instance of the completion of a URL request
static void
g_NPP_URLNotify(NPP instance, const char *url, NPReason reason, void *notifyData)
{
  D(bug("NPP_URLNotify instance=%p, url='%s', reason=%d, notifyData=%p\n", instance, url, reason, notifyData));

  if (instance == NULL)
	return;

  PluginInstance *plugin = instance->pdata;

  if (!g_plugin_funcs.urlnotify)
	return;

  g_plugin_funcs.urlnotify(plugin->instance, url, reason, notifyData);
}

// Notifies a plug-in instance of a new data stream
static NPError
g_NPP_NewStream(NPP instance, NPMIMEType type, NPStream *io_stream, NPBool seekable, uint16 *stype)
{
  D(bug("NPP_NewStream instance=%p\n", instance));

  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  PluginInstance *plugin = instance->pdata;

  if (!g_plugin_funcs.newstream)
	return NPERR_GENERIC_ERROR;

  uint16 v_stype = NP_NORMAL;
  if (stype)
	v_stype = *stype;

  NPStream *stream = malloc(sizeof(*stream));
  if (stream == NULL)
	return NPERR_OUT_OF_MEMORY_ERROR;
  stream->url = io_stream->url;
  stream->end = io_stream->end;
  stream->lastmodified = io_stream->lastmodified;

  NPError ret = g_plugin_funcs.newstream(plugin->instance, type, stream, seekable, &v_stype);
  io_stream->pdata = stream;
  io_stream->notifyData = stream->notifyData;
  D(bug(" io_stream=%p, stream=%p, notifyData=%p\n", io_stream, stream, stream->notifyData));

  if (stype)
	*stype = v_stype;

  D(bug(" return: %d [stype=%d]\n", ret, *stype));
  return ret;
}

// Tells the plug-in that a stream is about to be closed or destroyed
static NPError
g_NPP_DestroyStream(NPP instance, NPStream *io_stream, NPReason reason)
{
  D(bug("NPP_DestroyStream instance=%p\n", instance));

  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  PluginInstance *plugin = instance->pdata;

  if (!g_plugin_funcs.destroystream)
	return NPERR_GENERIC_ERROR;

  NPStream *stream = io_stream->pdata;

  NPError ret = g_plugin_funcs.destroystream(plugin->instance, stream, reason);

  free(stream);

  D(bug(" return: %d\n", ret));
  return ret;
}

// Provides a local file name for the data from a stream
static void
g_NPP_StreamAsFile(NPP instance, NPStream *io_stream, const char *fname)
{
  D(bug("NPP_StreamAsFile instance=%p\n", instance));

  if (instance == NULL)
	return;

  PluginInstance *plugin = instance->pdata;

  if (!g_plugin_funcs.asfile)
	return;

  NPStream *stream = io_stream->pdata;

  D(bug(" fname='%s'\n", fname ? fname : "<error>"));
  g_plugin_funcs.asfile(plugin->instance, stream, fname);
}

// Determines maximum number of bytes that the plug-in can consume
static int32
g_NPP_WriteReady(NPP instance, NPStream *io_stream)
{
  D(bug("NPP_WriteReady instance=%p\n", instance));

  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  PluginInstance *plugin = instance->pdata;

  if (!g_plugin_funcs.writeready)
	return NPERR_GENERIC_ERROR;

  NPStream *stream = io_stream->pdata;

  int32 ret = g_plugin_funcs.writeready(plugin->instance, stream);

  D(bug(" return: %d\n", ret));
  return ret;
}

// Delivers data to a plug-in instance
static int32
g_NPP_Write(NPP instance, NPStream *io_stream, int32 offset, int32 len, void *buf)
{
  D(bug("NPP_Write instance=%p\n", instance));

  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  PluginInstance *plugin = instance->pdata;

  if (!g_plugin_funcs.write)
	return NPERR_GENERIC_ERROR;

  NPStream *stream = io_stream->pdata;

  int32 ret = g_plugin_funcs.write(plugin->instance, stream, offset, len, buf);

  D(bug(" return: %d\n", ret));
  return ret;
}

// Requests a platform-specific print operation for an embedded or full-screen plug-in
static void
g_NPP_Print(NPP instance, NPPrint *PrintInfo)
{
  D(bug("NPP_Write instance=%p\n", instance));
  UNIMPLEMENTED();
}

// Delivers a platform-specific window event to the instance
static int16
g_NPP_HandleEvent(NPP instance, void *event)
{
  D(bug("NPP_HandleEvent instance=%p\n", instance));
  UNIMPLEMENTED();

  return NPERR_GENERIC_ERROR;
}

// Allows the browser to query the plug-in for information
NPError
NP_GetValue(void *future, NPPVariable variable, void *value)
{
  D(bug("NP_GetValue\n"));

  if (g_plugin_initialized <= 0) {
	npw_printf("ERROR: could not initialize underlying plugin\n");
	return NPERR_GENERIC_ERROR;
  }

  // Handle NPPVpluginNameString and NPPVpluginDescriptionString only
  switch (variable) {
  case NPPVpluginNameString:
	break;
  case NPPVpluginDescriptionString:
	break;
  default:
	return NPERR_INVALID_PARAM;
  }

  char *str = NULL;
  NPError ret = g_NP_GetValue ? g_NP_GetValue(NULL, variable, (void *)&str) : NPERR_GENERIC_ERROR;

  if (ret == NPERR_NO_ERROR)
	*((char **) value) = str;

  D(bug(" return: %d\n", ret));
  return ret;
}

// Allows the browser to query the plug-in supported formats
char *
NP_GetMIMEDescription(void)
{
  D(bug("NP_GetMIMEDescription\n"));

  if (g_plugin_initialized <= 0) {
	npw_printf("ERROR: could not initialize underlying plugin\n");
	return NULL;
  }

  char *formats = g_NP_GetMIMEDescription();
  D(bug(" formats='%s'\n", formats));
  return formats;
}

// Provides global initialization for a plug-in
NPError
NP_Initialize(NPNetscapeFuncs *moz_funcs, NPPluginFuncs *plugin_funcs)
{
  D(bug("NP_Initialize\n"));

  if (g_plugin_initialized <= 0) {
	npw_printf("ERROR: could not initialize underlying plugin\n");
	return NPERR_GENERIC_ERROR;
  }

  if (moz_funcs == NULL || plugin_funcs == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;
  if ((moz_funcs->version >> 8) > NP_VERSION_MAJOR)
    return NPERR_INCOMPATIBLE_VERSION_ERROR;
  if (moz_funcs->size < sizeof(NPNetscapeFuncs))
    return NPERR_INVALID_FUNCTABLE_ERROR;
  if (plugin_funcs->size < sizeof(NPPluginFuncs))
    return NPERR_INVALID_FUNCTABLE_ERROR;

  memcpy(&g_mozilla_funcs, moz_funcs, sizeof(g_mozilla_funcs));

  memset(&g_plugin_funcs, 0, sizeof(g_plugin_funcs));
  g_plugin_funcs.size = sizeof(g_plugin_funcs);

  memset(moz_funcs, 0, sizeof(*moz_funcs));
  moz_funcs->size = sizeof(*moz_funcs);
  moz_funcs->version = (NP_VERSION_MAJOR << 8) + NP_VERSION_MINOR;
  moz_funcs->geturl = g_NPN_GetURL;
  moz_funcs->posturl = g_NPN_PostURL;
  moz_funcs->requestread = g_NPN_RequestRead;
  moz_funcs->newstream = g_NPN_NewStream;
  moz_funcs->write = g_NPN_Write;
  moz_funcs->destroystream = g_NPN_DestroyStream;
  moz_funcs->status = g_NPN_Status;
  moz_funcs->uagent = g_NPN_UserAgent;
  moz_funcs->memalloc = g_NPN_MemAlloc;
  moz_funcs->memfree = g_NPN_MemFree;
  moz_funcs->memflush = g_NPN_MemFlush;
  moz_funcs->reloadplugins = g_NPN_ReloadPlugins;
  moz_funcs->getJavaEnv = g_NPN_GetJavaEnv;
  moz_funcs->getJavaPeer = g_NPN_GetJavaPeer;
  moz_funcs->geturlnotify = g_NPN_GetURLNotify;
  moz_funcs->posturlnotify = g_NPN_PostURLNotify;
  moz_funcs->getvalue = g_NPN_GetValue;
  moz_funcs->setvalue = g_NPN_SetValue;
  moz_funcs->invalidaterect = g_NPN_InvalidateRect;
  moz_funcs->invalidateregion = g_NPN_InvalidateRegion;
  moz_funcs->forceredraw = g_NPN_ForceRedraw;

  memset(plugin_funcs, 0, sizeof(*plugin_funcs));
  plugin_funcs->size = sizeof(NPPluginFuncs);
  plugin_funcs->version = (NP_VERSION_MAJOR << 8) + NP_VERSION_MINOR;
  plugin_funcs->newp = NewNPP_NewProc(g_NPP_New);
  plugin_funcs->destroy = NewNPP_DestroyProc(g_NPP_Destroy);
  plugin_funcs->setwindow = NewNPP_SetWindowProc(g_NPP_SetWindow);
  plugin_funcs->newstream = NewNPP_NewStreamProc(g_NPP_NewStream);
  plugin_funcs->destroystream = NewNPP_DestroyStreamProc(g_NPP_DestroyStream);
  plugin_funcs->asfile = NewNPP_StreamAsFileProc(g_NPP_StreamAsFile);
  plugin_funcs->writeready = NewNPP_WriteReadyProc(g_NPP_WriteReady);
  plugin_funcs->write = NewNPP_WriteProc(g_NPP_Write);
  plugin_funcs->print = NewNPP_PrintProc(g_NPP_Print);
  plugin_funcs->event = NewNPP_HandleEventProc(g_NPP_HandleEvent);
  plugin_funcs->urlnotify = NewNPP_URLNotifyProc(g_NPP_URLNotify);
  plugin_funcs->javaClass = NULL;
  plugin_funcs->getvalue = NewNPP_GetValueProc(g_NPP_GetValue);
  plugin_funcs->setvalue = NewNPP_SetValueProc(g_NPP_SetValue);

  NPError ret = g_NP_Initialize(moz_funcs, &g_plugin_funcs);
  D(bug(" return: %d\n", ret));
  return ret;
}

// Provides global deinitialization for a plug-in
NPError
NP_Shutdown(void)
{
  D(bug("NP_Shutdown\n"));

  if (g_plugin_initialized <= 0) {
	npw_printf("ERROR: could not initialize underlying plugin\n");
	return NPERR_GENERIC_ERROR;
  }

  NPError ret = g_NP_Shutdown();
  D(bug(" return: %d\n", ret));
  return ret;
}
