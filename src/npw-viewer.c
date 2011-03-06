/*
 *  npw-viewer.c - Target plugin loader and viewer
 *
 *  nspluginwrapper (C) 2005-2007 Gwenole Beauchesne
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define _GNU_SOURCE 1 /* RTLD_NEXT */
#include "sysdeps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dlfcn.h>
#include <unistd.h>
#include <errno.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/StringDefs.h>
#include <X11/extensions/XShm.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include "rpc.h"
#include "npw-rpc.h"
#include "utils.h"

#define XP_UNIX 1
#define MOZ_X11 1
#include <npapi.h>
#include <npupp.h>
#include "npruntime-impl.h"

#define DEBUG 1
#include "debug.h"


// [UNIMPLEMENTED] Define to use XPCOM emulation
#define USE_XPCOM 0

// Define to use XEMBED
#define USE_XEMBED 1

// XXX define which widget to use
#define USE_GTK_TOOLKIT 1
#define USE_X11_TOOLKIT 0

// XXX unimplemented functions
#define UNIMPLEMENTED() npw_printf("WARNING: Unimplemented function %s at line %d\n", __func__, __LINE__)


// RPC global connections
rpc_connection_t *g_rpc_connection attribute_hidden = NULL;

// Instance state information about the plugin
typedef struct _PluginInstance {
  NPP instance;
  uint32_t instance_id;
  bool use_xembed;
  NPWindow window;
  uint32_t width, height;
  Window old_window;
  Window focus_window;
  Window old_focus_window;
#if USE_GTK_TOOLKIT
  GtkWidget *top_window;
#else
  Window top_window;
#endif
  Widget top_widget;
  Widget form;
} PluginInstance;

// Browser side data for an NPStream instance
typedef struct _StreamInstance {
  NPStream *stream;
  uint32_t stream_id;
} StreamInstance;


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

extern void XtResizeWidget(
    Widget              /* widget */,
    _XtDimension        /* width */,
    _XtDimension        /* height */,
    _XtDimension        /* border_width */
);

// Update focus to plugin window
// XXX I am not convinced this is the really corrent...
static void xt_client_focus_listener(Widget w, XtPointer user_data, XEvent *event, Boolean *cont)
{
  PluginInstance *plugin = (PluginInstance *)user_data;

  switch (event->type) {
  case ButtonRelease:
	if (plugin->focus_window != XtWindow(plugin->form)) {
	  XSetInputFocus(x_display, XtWindow(plugin->form), RevertToParent, event->xbutton.time);
	  plugin->focus_window = XtWindow(plugin->form);
	  XSync(x_display, False);
	}
	break;
  case LeaveNotify:
	if (plugin->old_focus_window && plugin->focus_window != plugin->old_focus_window) {
	  XSetInputFocus(x_display, plugin->old_focus_window, RevertToParent, event->xcrossing.time);
	  plugin->focus_window = plugin->old_focus_window;
	  XSync(x_display, False);
	}
	break;
  }
}


/* ====================================================================== */
/* === XPCOM glue                                                     === */
/* ====================================================================== */

#if defined(__GNUC__) && (__GNUC__ > 2)
#define NS_LIKELY(x)    (__builtin_expect((x), 1))
#define NS_UNLIKELY(x)  (__builtin_expect((x), 0))
#else
#define NS_LIKELY(x)    (x)
#define NS_UNLIKELY(x)  (x)
#endif

#define NS_FAILED(_nsresult) (NS_UNLIKELY((_nsresult) & 0x80000000))
#define NS_SUCCEEDED(_nsresult) (NS_LIKELY(!((_nsresult) & 0x80000000)))

typedef uint32 nsresult;
typedef struct nsIServiceManager nsIServiceManager;
extern nsresult NS_GetServiceManager(nsIServiceManager **result);


/* ====================================================================== */
/* === Window utilities                                               === */
/* ====================================================================== */

// Reconstruct window attributes
static int create_window_attributes(NPWindow *window)
{
  if (window == NULL || window->window == NULL)
	return -1;
  if (window->ws_info == NULL) {
	if ((window->ws_info = malloc(sizeof(NPSetWindowCallbackStruct))) == NULL) {
	  npw_printf("ERROR: could not allocate window attributes for NPWindow %p\n", window->window);
	  return -2;
	}
  }
  NPSetWindowCallbackStruct *ws_info = window->ws_info;
  ws_info->type = 0; // should be NP_SETWINDOW but Mozilla sets it to 0
  ws_info->display = x_display;
  XWindowAttributes win_attr;
  if (!XGetWindowAttributes(ws_info->display, (Window)window->window, &win_attr)) {
	npw_printf("ERROR: could not reconstruct window attributes from NPWindow\n");
	return -3;
  }
  ws_info->visual = win_attr.visual;
  ws_info->colormap = win_attr.colormap;
  ws_info->depth = win_attr.depth;
  return 0;
}

// Destroy window attributes struct
static void destroy_window_attributes(NPWindow *window)
{
  if (window == NULL)
	return;
  if (window->ws_info) {
	free(window->ws_info);
	window->ws_info = NULL;
  }
}

// Fix size hints in NPWindow (Flash Player doesn't like null width)
static void fixup_size_hints(PluginInstance *plugin)
{
  NPWindow *window = &plugin->window;

  // check global hints (got through EMBED plugin args)
  if (window->width == 0 || window->height == 0) {
	if (plugin->width && plugin->height) {
	  window->width = plugin->width;
	  window->height = plugin->height;
	  return;
	}
  }

  // check actual window size and commit back to plugin data
  if (window->width == 0 || window->height == 0) {
	XWindowAttributes win_attr;
	if (XGetWindowAttributes(x_display, (Window)window->window, &win_attr)) {
	  plugin->width = window->width = win_attr.width;
	  plugin->height = window->height = win_attr.height;
	  return;
	}
  }

  if (window->width == 0 || window->height == 0)
	npw_printf("WARNING: grmpf, despite much effort, I could not determine the actual plugin area size...\n");
}

// Create a new window from NPWindow
static int create_window(PluginInstance *plugin, NPWindow *window)
{
  // cache new window information
  // XXX destroy previous window here?
  memcpy(&plugin->window, window, sizeof(*window));
  window = &plugin->window;
  fixup_size_hints(plugin);

  // reconstruct window attributes
  if (create_window_attributes(window) < 0)
	return -1;
  NPSetWindowCallbackStruct *ws_info = window->ws_info;

  // create the new window
  if (plugin->use_xembed)
	return 0;
#if USE_GTK_TOOLKIT
  plugin->top_window = gtk_plug_new(0);
  gtk_window_resize(GTK_WINDOW(plugin->top_window), plugin->width, plugin->height);
  gtk_widget_set_size_request(plugin->top_window, window->width, window->height);
  gtk_widget_show(plugin->top_window);
  {
	GdkDrawable *win = GDK_DRAWABLE(plugin->top_window->window);
	XReparentWindow(GDK_DRAWABLE_XDISPLAY(win),
					GDK_DRAWABLE_XID(win),
					(Window)window->window, 0, 0);

	XMapWindow(GDK_DRAWABLE_XDISPLAY(win),
			   GDK_DRAWABLE_XID(win));
  }
#else
  XSetWindowAttributes attr;
  attr.bit_gravity = NorthWestGravity;
  attr.colormap = ws_info->colormap;
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

  unsigned long mask = CWBitGravity | CWEventMask;
  if (attr.colormap)
	mask |= CWColormap;

  plugin->top_window = XCreateWindow(x_display, (Window)window->window,
									 0, 0, window->width, window->height,
									 0, ws_info->depth, InputOutput, ws_info->visual, mask, &attr);

  XSelectInput(x_display, plugin->top_window, ExposureMask);

  XMapWindow(x_display, plugin->top_window);
  XFlush(x_display);
#endif

  Widget top_widget = XtAppCreateShell("drawingArea", "npw-viewer", applicationShellWidgetClass, x_display, NULL, 0);
  plugin->top_widget = top_widget;
  XtResizeWidget(top_widget, window->width, window->height, 0);

  Widget form = XtVaCreateWidget("form", compositeWidgetClass, top_widget,
								 XtNdepth, ws_info->depth,
								 XtNvisual, ws_info->visual,
								 XtNcolormap, ws_info->colormap,
								 NULL);
  plugin->form = form;
  XtResizeWidget(form, window->width, window->height, 0);

#if USE_GTK_TOOLKIT
  plugin->old_window = top_widget->core.window;
  top_widget->core.window = GDK_WINDOW_XWINDOW(plugin->top_window->window);
  XtRegisterDrawable(x_display, GDK_WINDOW_XWINDOW(plugin->top_window->window), top_widget);
#else
  plugin->old_window = top_widget->core.window;
  top_widget->core.window = plugin->top_window;
  XtRegisterDrawable(x_display, plugin->top_window, top_widget);
#endif

  XtRealizeWidget(form);
  XtManageChild(form);

  int revert_to;
  plugin->old_focus_window = None;
  XGetInputFocus(x_display, &plugin->old_focus_window, &revert_to);
  XtAddEventHandler(form, (LeaveWindowMask | ButtonReleaseMask), True, xt_client_focus_listener, plugin);
  plugin->focus_window = plugin->old_focus_window;
  XSync(x_display, False);

  window->window = (void *)XtWindow(form);
  return 0;
}

// Update window information from NPWindow
static int update_window(PluginInstance *plugin, NPWindow *window)
{
  if (plugin->window.width != window->width || plugin->window.height != window->height) {
	plugin->window.width = window->width;
	plugin->window.height = window->height;
	if (plugin->form)
	  XtResizeWidget(plugin->form, plugin->window.width, plugin->window.height, 0);
	if (plugin->top_widget)
	  XtResizeWidget(plugin->top_widget, plugin->window.width, plugin->window.height, 0);
  }
  return 0;
}

// Destroy window
static void destroy_window(PluginInstance *plugin)
{
  if (plugin->old_focus_window) {
	XtRemoveEventHandler(plugin->form, (LeaveWindowMask | ButtonReleaseMask), True, xt_client_focus_listener, plugin);
	plugin->old_focus_window = None;
  }

  if (plugin->top_widget) {
	XSync(x_display, False);
#if USE_GTK_TOOLKIT
	XtUnregisterDrawable(x_display, GDK_WINDOW_XWINDOW(plugin->top_window->window));
#else
	XtUnregisterDrawable(x_display, plugin->top_window);
#endif
	XSync(x_display, False);
	plugin->top_widget->core.window = plugin->old_window;
	XtUnrealizeWidget(plugin->top_widget);
	XtDestroyWidget(plugin->top_widget);
	XSync(x_display, False);
	plugin->top_widget = NULL;
  }

  if (plugin->top_window) {
#if USE_GTK_TOOLKIT
	gtk_widget_destroy((GtkWidget *)plugin->top_window);
	plugin->top_window = NULL;
#else
	XDestroyWindow(x_display, plugin->top_window);
	plugin->top_window = None;
#endif
	XSync(x_display, False);
  }

  destroy_window_attributes(&plugin->window);
}


/* ====================================================================== */
/* === Browser side plug-in API                                       === */
/* ====================================================================== */

static char *g_user_agent = NULL;

// Netscape exported functions
static NPNetscapeFuncs mozilla_funcs;

// Closes and deletes a stream
static NPError
g_NPN_DestroyStream(NPP instance, NPStream *stream, NPError reason)
{
  D(bug("NPN_DestroyStream instance=%p\n", instance));

  UNIMPLEMENTED();

  return NPERR_GENERIC_ERROR;
}

// Forces a repaint message for a windowless plug-in
static void
g_NPN_ForceRedraw(NPP instance)
{
  D(bug("NPN_ForceRedraw instance=%p\n", instance));

  UNIMPLEMENTED();
}

// Asks the browser to create a stream for the specified URL
static NPError
invoke_NPN_GetURL(NPP instance, const char *url, const char *target)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_GET_URL,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_STRING, url,
								RPC_TYPE_STRING, target,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetURL() invoke", error);
	return NPERR_GENERIC_ERROR;
  }

  int32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INT32, &ret, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetURL() wait for reply", error);
	return NPERR_GENERIC_ERROR;
  }

  return ret;
}

static NPError
g_NPN_GetURL(NPP instance, const char *url, const char *target)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  D(bug("NPN_GetURL instance=%p\n", instance));
  NPError ret = invoke_NPN_GetURL(instance, url, target);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

// Requests creation of a new stream with the contents of the specified URL
static NPError
invoke_NPN_GetURLNotify(NPP instance, const char *url, const char *target, void *notifyData)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_GET_URL_NOTIFY,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_STRING, url,
								RPC_TYPE_STRING, target,
								RPC_TYPE_NP_NOTIFY_DATA, notifyData,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetURLNotify() invoke", error);
	return NPERR_GENERIC_ERROR;
  }

  int32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INT32, &ret, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetURLNotify() wait for reply", error);
	return NPERR_GENERIC_ERROR;
  }

  return ret;
}

static NPError
g_NPN_GetURLNotify(NPP instance, const char *url, const char *target, void *notifyData)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  D(bug("NPN_GetURLNotify instance=%p\n", instance));
  NPError ret = invoke_NPN_GetURLNotify(instance, url, target, notifyData);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

// Allows the plug-in to query the browser for information
static NPError
invoke_NPN_GetValue(NPP instance, NPNVariable variable, void *value)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_GET_VALUE,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_UINT32, variable,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetValue() invoke", error);
	return NPERR_GENERIC_ERROR;
  }

  int32_t ret;
  switch (rpc_type_of_NPNVariable(variable)) {
  case RPC_TYPE_BOOLEAN:
	{
	  uint32_t b = 0;
	  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INT32, &ret, RPC_TYPE_BOOLEAN, &b, RPC_TYPE_INVALID);
	  if (error != RPC_ERROR_NO_ERROR) {
		npw_perror("NPN_GetValue() wait for reply", error);
		ret = NPERR_GENERIC_ERROR;
	  }
	  D(bug(" value: %s\n", b ? "true" : "false"));
	  *((PRBool *)value) = b ? PR_TRUE : PR_FALSE;
	  break;
	}
  case RPC_TYPE_NP_OBJECT:
	{
	  NPObject *npobj = NULL;
	  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INT32, &ret, RPC_TYPE_NP_OBJECT, &npobj, RPC_TYPE_INVALID);
	  if (error != RPC_ERROR_NO_ERROR) {
		npw_perror("NPN_GetValue() wait for reply", error);
		ret = NPERR_GENERIC_ERROR;
	  }
	  D(bug(" value: %p\n", npobj));
	  *((NPObject **)value) = npobj;
	  break;
	}
  }

  return ret;
}

static NPError
g_NPN_GetValue(NPP instance, NPNVariable variable, void *value)
{
  D(bug("NPN_GetValue instance=%p, variable=%d [%08x]\n", instance, variable & 0xffff, variable));

  switch (variable) {
  case NPNVxDisplay:
	*(void **)value = x_display;
	break;
  case NPNVxtAppContext:
	*(void **)value = XtDisplayToApplicationContext(x_display);
	break;
  case NPNVToolkit:
	*(NPNToolkitType *)value = NPNVGtk2;
	break;
#if USE_XPCOM
  case NPNVserviceManager: {
	nsIServiceManager *sm;
	int ret = NS_GetServiceManager(&sm);
	if (NS_FAILED(ret))
	  return NPERR_GENERIC_ERROR;
	*(nsIServiceManager **)value = sm;
	break;
  }
#endif
#if USE_XEMBED
  case NPNVSupportsXEmbedBool:
#endif
  case NPNVWindowNPObject:
  case NPNVPluginElementNPObject: {
	int ret = invoke_NPN_GetValue(instance, variable, value);
	if (ret == NPERR_NO_ERROR)
	  return ret;
	// fall-through
  }
  default:
	npw_printf("WARNING: unhandled variable %d in NPN_GetValue()\n", variable);
	return NPERR_INVALID_PARAM;
  }

  return NPERR_NO_ERROR;
}

// Invalidates specified drawing area prior to repainting or refreshing a windowless plug-in
static void
g_NPN_InvalidateRect(NPP instance, NPRect *invalidRect)
{
  D(bug("NPN_InvalidateRect instance=%p\n", instance));

  UNIMPLEMENTED();
}

// Invalidates specified region prior to repainting or refreshing a windowless plug-in
static void
g_NPN_InvalidateRegion(NPP instance, NPRegion invalidRegion)
{
  D(bug("NPN_InvalidateRegion instance=%p\n", instance));

  UNIMPLEMENTED();
}

// Allocates memory from the browser's memory space
static void *
g_NPN_MemAlloc(uint32 size)
{
  D(bug("NPN_MemAlloc size=%d\n", size));

  return malloc(size);
}

// Requests that the browser free a specified amount of memory
static uint32
g_NPN_MemFlush(uint32 size)
{
  D(bug("NPN_MemFlush size=%d\n", size));

  return 0;
}

// Deallocates a block of allocated memory
static void
g_NPN_MemFree(void *ptr)
{
  D(bug("NPN_MemFree ptr=%p\n", ptr));

  free(ptr);
}

// Requests the creation of a new data stream produced by the plug-in and consumed by the browser
static NPError
g_NPN_NewStream(NPP instance, NPMIMEType type, const char *target, NPStream **stream)
{
  D(bug("NPN_NewStream instance=%p\n", instance));

  UNIMPLEMENTED();

  return NPERR_GENERIC_ERROR;
}

// Posts data to a URL
static NPError
invoke_NPN_PostURL(NPP instance, const char *url, const char *target, uint32 len, const char *buf, NPBool file)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_POST_URL,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_STRING, url,
								RPC_TYPE_STRING, target,
								RPC_TYPE_ARRAY, RPC_TYPE_CHAR, len, buf,
								RPC_TYPE_BOOLEAN, file,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_PostURL() invoke", error);
	return NPERR_GENERIC_ERROR;
  }

  int32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INT32, &ret, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_PostURL() wait for reply", error);
	return NPERR_GENERIC_ERROR;
  }

  return ret;
}

static NPError
g_NPN_PostURL(NPP instance, const char *url, const char *target, uint32 len, const char *buf, NPBool file)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  D(bug("NPN_PostURL instance=%p\n", instance));
  NPError ret = invoke_NPN_PostURL(instance, url, target, len, buf, file);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

// Posts data to a URL, and receives notification of the result
static NPError
invoke_NPN_PostURLNotify(NPP instance, const char *url, const char *target, uint32 len, const char *buf, NPBool file, void *notifyData)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_POST_URL_NOTIFY,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_STRING, url,
								RPC_TYPE_STRING, target,
								RPC_TYPE_ARRAY, RPC_TYPE_CHAR, len, buf,
								RPC_TYPE_BOOLEAN, file,
								RPC_TYPE_NP_NOTIFY_DATA, notifyData,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_PostURLNotify() invoke", error);
	return NPERR_GENERIC_ERROR;
  }

  int32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INT32, &ret, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_PostURLNotify() wait for reply", error);
	return NPERR_GENERIC_ERROR;
  }

  return ret;
}

static NPError
g_NPN_PostURLNotify(NPP instance, const char *url, const char *target, uint32 len, const char *buf, NPBool file, void *notifyData)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  D(bug("NPN_PostURLNotify instance=%p\n", instance));
  NPError ret = invoke_NPN_PostURLNotify(instance, url, target, len, buf, file, notifyData);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

// Posts data to a URL, and receives notification of the result
static void
g_NPN_ReloadPlugins(NPBool reloadPages)
{
  D(bug("NPN_ReloadPlugins reloadPages=%d\n", reloadPages));

  UNIMPLEMENTED();
}

// Returns the Java execution environment
static JRIEnv *
g_NPN_GetJavaEnv(void)
{
  D(bug("NPN_GetJavaEnv\n"));

  return NULL;
}

// Returns the Java object associated with the plug-in instance
static jref
g_NPN_GetJavaPeer(NPP instance)
{
  D(bug("NPN_GetJavaPeer instance=%p\n", instance));

  return NULL;
}

// Requests a range of bytes for a seekable stream
static NPError
invoke_NPN_RequestRead(NPStream *stream, NPByteRange *rangeList)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_REQUEST_READ,
								RPC_TYPE_NP_STREAM, stream,
								RPC_TYPE_NP_BYTE_RANGE, rangeList,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_RequestRead() invoke", error);
	return NPERR_GENERIC_ERROR;
  }

  int32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INT32, &ret, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_RequestRead() wait for reply", error);
	return NPERR_GENERIC_ERROR;
  }

  return ret;
}

static NPError
g_NPN_RequestRead(NPStream *stream, NPByteRange *rangeList)
{
  if (stream == NULL || stream->ndata == NULL || rangeList == NULL)
	return NPERR_INVALID_PARAM;

  D(bug("NPN_RequestRead stream=%p\n", stream));
  NPError ret = invoke_NPN_RequestRead(stream, rangeList);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

// Sets various modes of plug-in operation
static NPError
g_NPN_SetValue(NPP instance, NPPVariable variable, void *value)
{
  D(bug("NPN_SetValue instance=%p\n", instance));

  UNIMPLEMENTED();

  return NPERR_GENERIC_ERROR;
}

// Displays a message on the status line of the browser window
static void
invoke_NPN_Status(NPP instance, const char *message)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_STATUS,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_STRING, message,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_Status() invoke", error);
	return;
  }

  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_Status() wait for reply", error);
	return;
  }
}

static void
g_NPN_Status(NPP instance, const char *message)
{
  D(bug("NPN_Status instance=%p\n", instance));
  invoke_NPN_Status(instance, message);
  D(bug(" done\n"));
}

// Returns the browser's user agent field
static char *
invoke_NPN_UserAgent(void)
{
  int error = rpc_method_invoke(g_rpc_connection, RPC_METHOD_NPN_USER_AGENT, RPC_TYPE_INVALID);
  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_UserAgent() invoke", error);
	return NULL;
  }

  char *user_agent;
  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_STRING, &user_agent, RPC_TYPE_INVALID);
  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_UserAgent() wait for reply", error);
	return NULL;
  }

  return user_agent;
}

static const char *
g_NPN_UserAgent(NPP instance)
{
  D(bug("NPN_UserAgent instance=%p\n", instance));
  if (g_user_agent == NULL)
	g_user_agent = invoke_NPN_UserAgent();
  D(bug(" user_agent='%s'\n", g_user_agent));
  return g_user_agent;
}

// Pushes data into a stream produced by the plug-in and consumed by the browser
static int32
g_NPN_Write(NPP instance, NPStream *stream, int32 len, void *buf)
{
  D(bug("NPN_Write instance=%d\n", instance));

  UNIMPLEMENTED();

  return -1;
}


/* ====================================================================== */
/* === NPRuntime glue                                                 === */
/* ====================================================================== */

// Allocates a new NPObject
static uint32_t
invoke_NPN_CreateObject(NPP instance)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_CREATE_OBJECT,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_CreateObject() invoke", error);
	return 0;
  }

  uint32_t npobj_id = 0;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &npobj_id,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_CreateObject() wait for reply", error);
	return 0;
  }

  return npobj_id;
}

static NPObject *
g_NPN_CreateObject(NPP instance, NPClass *class)
{
  if (instance == NULL)
	return NULL;

  if (class == NULL)
	return NULL;

  D(bug("NPN_CreateObject\n"));
  uint32_t npobj_id = invoke_NPN_CreateObject(instance);
  D(bug(" return: %d\n", npobj_id));
  return npobject_new(npobj_id, instance, class);
}

// Increments the reference count of the given NPObject
static uint32_t
invoke_NPN_RetainObject(NPObject *npobj)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_RETAIN_OBJECT,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_RetainObject() invoke", error);
	return npobj->referenceCount;
  }

  uint32_t refcount;
  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_UINT32, &refcount, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_RetainObject() wait for reply", error);
	return npobj->referenceCount;
  }

  return refcount;
}

static NPObject *
g_NPN_RetainObject(NPObject *npobj)
{
  if (npobj == NULL)
	return NULL;

  D(bug("NPN_RetainObject %p\n", npobj));
  uint32_t refcount = invoke_NPN_RetainObject(npobj);
  D(bug(" return: %d\n", refcount));
  npobj->referenceCount = refcount;
  return npobj;
}

// Decrements the reference count of the give NPObject
static uint32_t
invoke_NPN_ReleaseObject(NPObject *npobj)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_RELEASE_OBJECT,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_ReleaseObject() invoke", error);
	return npobj->referenceCount;
  }

  uint32_t refcount;
  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_UINT32, &refcount, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_ReleaseObject() wait for reply", error);
	return npobj->referenceCount;
  }

  return refcount;
}

static void
g_NPN_ReleaseObject(NPObject *npobj)
{
  if (npobj == NULL)
	return;

  D(bug("NPN_ReleaseObject %p\n", npobj));
  uint32_t refcount = invoke_NPN_ReleaseObject(npobj);
  D(bug(" return: %d\n", refcount));

  if ((npobj->referenceCount = refcount) == 0)
	npobject_destroy(npobj);
}

// Invokes a method on the given NPObject
static bool
invoke_NPN_Invoke(NPP instance, NPObject *npobj, NPIdentifier methodName,
				  const NPVariant *args, uint32_t argCount, NPVariant *result)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_INVOKE,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_NP_IDENTIFIER, methodName,
								RPC_TYPE_ARRAY, RPC_TYPE_NP_VARIANT, argCount, args,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_Invoke() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &ret,
									RPC_TYPE_NP_VARIANT, result,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_Invoke() wait for reply", error);
	return false;
  }

  return ret;
}

static bool
g_NPN_Invoke(NPP instance, NPObject *npobj, NPIdentifier methodName,
			 const NPVariant *args, uint32_t argCount, NPVariant *result)
{
  if (!instance || !npobj || !npobj->_class || !npobj->_class->invoke)
	return false;

  D(bug("NPN_Invoke instance=%p, npobj=%p\n", instance, npobj));
  bool ret = invoke_NPN_Invoke(instance, npobj, methodName, args, argCount, result);
  D(bug(" return: %d\n", ret));
  return ret;
}

// Invokes the default method on the given NPObject
static bool
invoke_NPN_InvokeDefault(NPP instance, NPObject *npobj,
						 const NPVariant *args, uint32_t argCount, NPVariant *result)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_INVOKE_DEFAULT,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_ARRAY, RPC_TYPE_NP_VARIANT, argCount, args,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_InvokeDefault() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &ret,
									RPC_TYPE_NP_VARIANT, result,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_InvokeDefault() wait for reply", error);
	return false;
  }

  return ret;
}

static bool
g_NPN_InvokeDefault(NPP instance, NPObject *npobj,
					const NPVariant *args, uint32_t argCount, NPVariant *result)
{
  if (!instance || !npobj || !npobj->_class || !npobj->_class->invokeDefault)
	return false;

  D(bug("NPN_InvokeDefault instance=%p, npobj=%p\n", instance, npobj));
  bool ret = invoke_NPN_InvokeDefault(instance, npobj, args, argCount, result);
  D(bug(" return: %d\n", ret));
  return ret;
}

// Evaluates a script on the scope of a given NPObject
static bool
invoke_NPN_Evaluate(NPP instance, NPObject *npobj, NPString *script, NPVariant *result)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_EVALUATE,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_NP_STRING, script,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_Evaluate() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &ret,
									RPC_TYPE_NP_VARIANT, result,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_Evaluate() wait for reply", error);
	return false;
  }

  return ret;
}

static bool
g_NPN_Evaluate(NPP instance, NPObject *npobj, NPString *script, NPVariant *result)
{
  if (!instance || !npobj)
	return false;

  if (!script || !script->utf8length || !script->utf8characters)
	return true; // nothing to evaluate

  D(bug("NPN_Evaluate instance=%p, npobj=%p\n", instance, npobj));
  bool ret = invoke_NPN_Evaluate(instance, npobj, script, result);
  D(bug(" return: %d\n", ret));
  return ret;
}

// Gets the value of a property on the given NPObject
static bool
invoke_NPN_GetProperty(NPP instance, NPObject *npobj, NPIdentifier propertyName,
					   NPVariant *result)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_GET_PROPERTY,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_NP_IDENTIFIER, propertyName,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetProperty() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &ret,
									RPC_TYPE_NP_VARIANT, result,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetProperty() wait for reply", error);
	return false;
  }

  return ret;
}

static bool
g_NPN_GetProperty(NPP instance, NPObject *npobj, NPIdentifier propertyName,
				  NPVariant *result)
{
  if (!instance || !npobj || !npobj->_class || !npobj->_class->getProperty)
	return false;

  D(bug("NPN_GetProperty instance=%p, npobj=%p\n", instance, npobj));
  bool ret = invoke_NPN_GetProperty(instance, npobj, propertyName, result);
  D(bug(" return: %d\n", ret));
  return ret;
}

// Sets the value of a property on the given NPObject
static bool
invoke_NPN_SetProperty(NPP instance, NPObject *npobj, NPIdentifier propertyName,
					   const NPVariant *value)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_SET_PROPERTY,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_NP_IDENTIFIER, propertyName,
								RPC_TYPE_NP_VARIANT, value,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_SetProperty() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &ret,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_SetProperty() wait for reply", error);
	return false;
  }

  return ret;
}

static bool
g_NPN_SetProperty(NPP instance, NPObject *npobj, NPIdentifier propertyName,
				  const NPVariant *value)
{
  if (!instance || !npobj || !npobj->_class || !npobj->_class->setProperty)
	return false;

  D(bug("NPN_SetProperty instance=%p, npobj=%p\n", instance, npobj));
  bool ret = invoke_NPN_SetProperty(instance, npobj, propertyName, value);
  D(bug(" return: %d\n", ret));
  return ret;
}

// Removes a property on the given NPObject
static bool
invoke_NPN_RemoveProperty(NPP instance, NPObject *npobj, NPIdentifier propertyName)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_REMOVE_PROPERTY,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_NP_IDENTIFIER, propertyName,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_RemoveProperty() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &ret,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_RemoveProperty() wait for reply", error);
	return false;
  }

  return ret;
}

static bool
g_NPN_RemoveProperty(NPP instance, NPObject *npobj, NPIdentifier propertyName)
{
  if (!instance || !npobj || !npobj->_class || !npobj->_class->removeProperty)
	return false;

  D(bug("NPN_RemoveProperty instance=%p, npobj=%p\n", instance, npobj));
  bool ret = invoke_NPN_RemoveProperty(instance, npobj, propertyName);
  D(bug(" return: %d\n", ret));
  return ret;
}

// Checks if a given property exists on the given NPObject
static bool
invoke_NPN_HasProperty(NPP instance, NPObject *npobj, NPIdentifier propertyName)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_HAS_PROPERTY,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_NP_IDENTIFIER, propertyName,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_HasProperty() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &ret,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_HasProperty() wait for reply", error);
	return false;
  }

  return ret;
}

static bool
g_NPN_HasProperty(NPP instance, NPObject *npobj, NPIdentifier propertyName)
{
  if (!instance || !npobj || !npobj->_class || !npobj->_class->hasProperty)
	return false;

  D(bug("NPN_HasProperty instance=%p, npobj=%p\n", instance, npobj));
  bool ret = invoke_NPN_HasProperty(instance, npobj, propertyName);
  D(bug(" return: %d\n", ret));
  return ret;
}

// Checks if a given method exists on the given NPObject
static bool
invoke_NPN_HasMethod(NPP instance, NPObject *npobj, NPIdentifier methodName)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_HAS_METHOD,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_NP_IDENTIFIER, methodName,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_HasMethod() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &ret,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_HasMethod() wait for reply", error);
	return false;
  }

  return ret;
}

static bool
g_NPN_HasMethod(NPP instance, NPObject *npobj, NPIdentifier methodName)
{
  if (!instance || !npobj || !npobj->_class || !npobj->_class->hasMethod)
	return false;

  D(bug("NPN_HasMethod instance=%p, npobj=%p\n", instance, npobj));
  bool ret = invoke_NPN_HasMethod(instance, npobj, methodName);
  D(bug(" return: %d\n", ret));
  return ret;
}

// Indicates that a call to one of the plugins NPObjects generated an error
static void
invoke_NPN_SetException(NPObject *npobj, const NPUTF8 *message)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_SET_EXCEPTION,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_STRING, message,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_SetException() invoke", error);
	return;
  }

  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_SetException() wait for reply", error);
	return;
  }
}

static void
g_NPN_SetException(NPObject *npobj, const NPUTF8 *message)
{
  D(bug("NPN_SetException npobj=%p, message='%s'\n", npobj, message));
  invoke_NPN_SetException(npobj, message);
  D(bug(" done\n"));
}

// Releases the value in the given variant
static void
g_NPN_ReleaseVariantValue(NPVariant *variant)
{
  D(bug("NPN_ReleaseVariantValue\n"));

  switch (variant->type) {
  case NPVariantType_String:
	{
	  NPString *s = &NPVARIANT_TO_STRING(*variant);
	  if (s->utf8characters)
		free((void *)s->utf8characters);
	  break;
	}
  case NPVariantType_Object:
	{
	  NPObject *npobj = NPVARIANT_TO_OBJECT(*variant);
	  if (npobj)
		g_NPN_ReleaseObject(npobj);
	  break;
	}
  }

  D(bug(" done\n"));
}

// Returns an opaque identifier for the string that is passed in
static NPIdentifier
invoke_NPN_GetStringIdentifier(const NPUTF8 *name)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_GET_STRING_IDENTIFIER,
								RPC_TYPE_STRING, name,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetStringIdentifier() invoke", error);
	return NULL;
  }

  NPIdentifier ident;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_NP_IDENTIFIER, &ident,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetStringIdentifier() wait for reply", error);
	return NULL;
  }

  return ident;
}

static NPIdentifier
g_NPN_GetStringIdentifier(const NPUTF8 *name)
{
  if (name == NULL)
	return NULL;

  D(bug("NPN_GetStringIdentifier name='%s'\n", name));
  NPIdentifier ret = invoke_NPN_GetStringIdentifier(name);
  D(bug(" return: %p\n", ret));
  return ret;
}

// Returns an array of opaque identifiers for the names that are passed in
static void
invoke_NPN_GetStringIdentifiers(const NPUTF8 **names, uint32_t nameCount, NPIdentifier *identifiers)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_GET_STRING_IDENTIFIERS,
								RPC_TYPE_ARRAY, RPC_TYPE_STRING, nameCount, names,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetStringIdentifiers() invoke", error);
	return;
  }

  uint32_t n_idents;
  NPIdentifier *idents;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_ARRAY, RPC_TYPE_NP_IDENTIFIER, &n_idents, &idents,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetStringIdentifiers() wait for reply", error);
	return;
  }

  if (identifiers) {
	if (n_idents != nameCount) {
	  npw_printf("ERROR: NPN_GetStringIdentifiers returned fewer NPIdentifiers than expected\n");
	  if (n_idents > nameCount)
		n_idents = nameCount;
	}
	for (int i = 0; i < n_idents; i++)
	  identifiers[i] = idents[i];
	free(idents);
  }
}

static void
g_NPN_GetStringIdentifiers(const NPUTF8 **names, uint32_t nameCount, NPIdentifier *identifiers)
{
  if (names == NULL)
	return;

  if (identifiers == NULL)
	return;

  D(bug("NPN_GetStringIdentifiers names=%p\n", names));
  invoke_NPN_GetStringIdentifiers(names, nameCount, identifiers);
  D(bug(" done\n"));
}

// Returns an opaque identifier for the integer that is passed in
static NPIdentifier
invoke_NPN_GetIntIdentifier(int32_t intid)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_GET_INT_IDENTIFIER,
								RPC_TYPE_INT32, intid,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetIntIdentifier() invoke", error);
	return NULL;
  }

  NPIdentifier ident;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_NP_IDENTIFIER, &ident,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetIntIdentifier() wait for reply", error);
	return NULL;
  }

  return ident;
}

static NPIdentifier
g_NPN_GetIntIdentifier(int32_t intid)
{
  D(bug("NPN_GetIntIdentifier intid=%d\n", intid));
  NPIdentifier ret = invoke_NPN_GetIntIdentifier(intid);
  D(bug(" return: %p\n", ret));
  return ret;
}

// Returns true if the given identifier is a string identifier, or false if it is an integer identifier
static bool
invoke_NPN_IdentifierIsString(NPIdentifier identifier)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_IDENTIFIER_IS_STRING,
								RPC_TYPE_NP_IDENTIFIER, identifier,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_IdentifierIsString() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &ret,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_IdentifierIsString() wait for reply", error);
	return false;
  }

  return ret;
}

static bool
g_NPN_IdentifierIsString(NPIdentifier identifier)
{
  D(bug("NPN_IdentifierIsString identifier=%p\n", identifier));
  bool ret = invoke_NPN_IdentifierIsString(identifier);
  D(bug(" return: %d\n", ret));
  return ret;
}

// Returns a pointer to a UTF-8 string as a sequence of 8-bit units (NPUTF8)
static NPUTF8 *
invoke_NPN_UTF8FromIdentifier(NPIdentifier identifier)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_UTF8_FROM_IDENTIFIER,
								RPC_TYPE_NP_IDENTIFIER, identifier,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_UTF8FromIdentifier() invoke", error);
	return NULL;
  }

  char *str;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_STRING, &str,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_UTF8FromIdentifier() wait for reply", error);
	return NULL;
  }

  return str;
}

static NPUTF8 *
g_NPN_UTF8FromIdentifier(NPIdentifier identifier)
{
  D(bug("NPN_UTF8FromIdentifier identifier=%p\n", identifier));
  NPUTF8 *ret = invoke_NPN_UTF8FromIdentifier(identifier);
  D(bug(" return: '%s'\n", ret));
  return ret;
}

// Returns the integer value for the given integer identifier
// NOTE: if the given identifier is not a integer identifier, the behavior is undefined (we return -1)
static int32_t
invoke_NPN_IntFromIdentifier(NPIdentifier identifier)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_INT_FROM_IDENTIFIER,
								RPC_TYPE_NP_IDENTIFIER, identifier,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_IntFromIdentifier() invoke", error);
	return -1;
  }

  int32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_INT32, &ret,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_IntFromIdentifier() wait for reply", error);
	return -1;
  }

  return ret;
}

static int32_t
g_NPN_IntFromIdentifier(NPIdentifier identifier)
{
  D(bug("NPN_IntFromIdentifier identifier=%p\n", identifier));
  int32_t ret = invoke_NPN_IntFromIdentifier(identifier);
  D(bug(" return: %d\n", ret));
  return ret;
}


/* ====================================================================== */
/* === Plug-in side data                                              === */
/* ====================================================================== */

// Functions supplied by the plug-in
static NPPluginFuncs plugin_funcs;

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
/* === RPC communication                                              === */
/* ====================================================================== */

// NP_GetMIMEDescription
static int handle_NP_GetMIMEDescription(rpc_connection_t *connection)
{
  D(bug("handle_NP_GetMIMEDescription\n"));

  char *str = g_NP_GetMIMEDescription();
  return rpc_method_send_reply(connection, RPC_TYPE_STRING, str, RPC_TYPE_INVALID);
}

// NP_GetValue
static int handle_NP_GetValue(rpc_connection_t *connection)
{
  D(bug("handle_NP_GetValue\n"));

  int32_t variable;
  int error = rpc_method_get_args(connection, RPC_TYPE_INT32, &variable, RPC_TYPE_INVALID);
  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NP_GetValue() get args", error);
	return error;
  }

  char *str = NULL;
  NPError ret = g_NP_GetValue ? g_NP_GetValue(NULL, variable, (void *)&str) : NPERR_GENERIC_ERROR;
  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_STRING, str, RPC_TYPE_INVALID);
}

// NP_Initialize
static int handle_NP_Initialize(rpc_connection_t *connection)
{
  D(bug("handle_NP_Initialize\n"));

  uint32_t has_npruntime = 0;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_UINT32, &has_npruntime,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NP_Initialize() get args", error);
	return error;
  }

  memset(&plugin_funcs, 0, sizeof(plugin_funcs));
  plugin_funcs.size = sizeof(plugin_funcs);

  memset(&mozilla_funcs, 0, sizeof(mozilla_funcs));
  mozilla_funcs.size = sizeof(mozilla_funcs);
  mozilla_funcs.version = (NP_VERSION_MAJOR << 8) + NP_VERSION_MINOR;
  mozilla_funcs.geturl = NewNPN_GetURLProc(g_NPN_GetURL);
  mozilla_funcs.posturl = NewNPN_PostURLProc(g_NPN_PostURL);
  mozilla_funcs.requestread = NewNPN_RequestReadProc(g_NPN_RequestRead);
  mozilla_funcs.newstream = NewNPN_NewStreamProc(g_NPN_NewStream);
  mozilla_funcs.write = NewNPN_WriteProc(g_NPN_Write);
  mozilla_funcs.destroystream = NewNPN_DestroyStreamProc(g_NPN_DestroyStream);
  mozilla_funcs.status = NewNPN_StatusProc(g_NPN_Status);
  mozilla_funcs.uagent = NewNPN_UserAgentProc(g_NPN_UserAgent);
  mozilla_funcs.memalloc = NewNPN_MemAllocProc(g_NPN_MemAlloc);
  mozilla_funcs.memfree = NewNPN_MemFreeProc(g_NPN_MemFree);
  mozilla_funcs.memflush = NewNPN_MemFlushProc(g_NPN_MemFlush);
  mozilla_funcs.reloadplugins = NewNPN_ReloadPluginsProc(g_NPN_ReloadPlugins);
  mozilla_funcs.getJavaEnv = NewNPN_GetJavaEnvProc(g_NPN_GetJavaEnv);
  mozilla_funcs.getJavaPeer = NewNPN_GetJavaPeerProc(g_NPN_GetJavaPeer);
  mozilla_funcs.geturlnotify = NewNPN_GetURLNotifyProc(g_NPN_GetURLNotify);
  mozilla_funcs.posturlnotify = NewNPN_PostURLNotifyProc(g_NPN_PostURLNotify);
  mozilla_funcs.getvalue = NewNPN_GetValueProc(g_NPN_GetValue);
  mozilla_funcs.setvalue = NewNPN_SetValueProc(g_NPN_SetValue);
  mozilla_funcs.invalidaterect = NewNPN_InvalidateRectProc(g_NPN_InvalidateRect);
  mozilla_funcs.invalidateregion = NewNPN_InvalidateRegionProc(g_NPN_InvalidateRegion);
  mozilla_funcs.forceredraw = NewNPN_ForceRedrawProc(g_NPN_ForceRedraw);

  npruntime_init_callbacks(&mozilla_funcs);

  if (has_npruntime && getenv("NPW_DISABLE_NPRUNTIME")) {
	D(bug(" user disabled npruntime support\n"));
	has_npruntime = false;
  }

  if (has_npruntime) {
	D(bug(" browser supports scripting through npruntime\n"));
	mozilla_funcs.getstringidentifier = NewNPN_GetStringIdentifierProc(g_NPN_GetStringIdentifier);
	mozilla_funcs.getstringidentifiers = NewNPN_GetStringIdentifiersProc(g_NPN_GetStringIdentifiers);
	mozilla_funcs.getintidentifier = NewNPN_GetIntIdentifierProc(g_NPN_GetIntIdentifier);
	mozilla_funcs.identifierisstring = NewNPN_IdentifierIsStringProc(g_NPN_IdentifierIsString);
	mozilla_funcs.utf8fromidentifier = NewNPN_UTF8FromIdentifierProc(g_NPN_UTF8FromIdentifier);
	mozilla_funcs.intfromidentifier = NewNPN_IntFromIdentifierProc(g_NPN_IntFromIdentifier);
	mozilla_funcs.createobject = NewNPN_CreateObjectProc(g_NPN_CreateObject);
	mozilla_funcs.retainobject = NewNPN_RetainObjectProc(g_NPN_RetainObject);
	mozilla_funcs.releaseobject = NewNPN_ReleaseObjectProc(g_NPN_ReleaseObject);
	mozilla_funcs.invoke = NewNPN_InvokeProc(g_NPN_Invoke);
	mozilla_funcs.invokeDefault = NewNPN_InvokeDefaultProc(g_NPN_InvokeDefault);
	mozilla_funcs.evaluate = NewNPN_EvaluateProc(g_NPN_Evaluate);
	mozilla_funcs.getproperty = NewNPN_GetPropertyProc(g_NPN_GetProperty);
	mozilla_funcs.setproperty = NewNPN_SetPropertyProc(g_NPN_SetProperty);
	mozilla_funcs.removeproperty = NewNPN_RemovePropertyProc(g_NPN_RemoveProperty);
	mozilla_funcs.hasproperty = NewNPN_HasPropertyProc(g_NPN_HasProperty);
	mozilla_funcs.hasmethod = NewNPN_HasMethodProc(g_NPN_HasMethod);
	mozilla_funcs.releasevariantvalue = NewNPN_ReleaseVariantValueProc(g_NPN_ReleaseVariantValue);
	mozilla_funcs.setexception = NewNPN_SetExceptionProc(g_NPN_SetException);
  }

  NPError ret = NPERR_NO_ERROR;

  if (!npobject_bridge_new())
	ret = NPERR_OUT_OF_MEMORY_ERROR;

  if (ret == NPERR_NO_ERROR)
	ret = g_NP_Initialize(&mozilla_funcs, &plugin_funcs);

  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NP_Shutdown
static int handle_NP_Shutdown(rpc_connection_t *connection)
{
  D(bug("handle_NP_Shutdown\n"));

  NPError ret = g_NP_Shutdown();

  npobject_bridge_destroy();
  gtk_main_quit();

  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NPP_New
static NPError g_NPP_New(NPMIMEType plugin_type, uint32_t instance_id,
						 uint16_t mode, int16_t argc, char *argn[], char *argv[],
						 NPSavedData *saved)
{
  PluginInstance *plugin = malloc(sizeof(*plugin));
  if (plugin == NULL)
	return NPERR_OUT_OF_MEMORY_ERROR;
  memset(plugin, 0, sizeof(*plugin));
  plugin->instance_id = instance_id;
  id_link(instance_id, plugin);

  NPP instance = malloc(sizeof(*instance));
  if (instance == NULL)
	return NPERR_OUT_OF_MEMORY_ERROR;
  memset(instance, 0, sizeof(*instance));
  instance->ndata = plugin;
  plugin->instance = instance;

  // check for size hints
  for (int i = 0; i < argc; i++) {
	if (argn[i] == NULL)
	  continue;
	if (strcasecmp(argn[i], "width") == 0) {
	  if (i < argc && argv[i])
		plugin->width = atoi(argv[i]);
	}
	else if (strcasecmp(argn[i], "height") == 0) {
	  if (i < argc && argv[i])
		plugin->height = atoi(argv[i]);
	}
  }

  D(bug("NPP_New instance=%p\n", instance));
  NPError ret = plugin_funcs.newp(plugin_type, instance, mode, argc, argn, argv, saved);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));

  // check if XEMBED is to be used
  PRBool supports_XEmbed = PR_FALSE;
  if (mozilla_funcs.getvalue) {
	NPError error = mozilla_funcs.getvalue(NULL, NPNVSupportsXEmbedBool, (void *)&supports_XEmbed);
	if (error == NPERR_NO_ERROR && plugin_funcs.getvalue) {
	  PRBool needs_XEmbed = PR_FALSE;
	  error = plugin_funcs.getvalue(instance, NPPVpluginNeedsXEmbed, (void *)&needs_XEmbed);
	  if (error == NPERR_NO_ERROR)
		plugin->use_xembed = supports_XEmbed && needs_XEmbed;
	}
  }

  return ret;
}

static int handle_NPP_New(rpc_connection_t *connection)
{
  D(bug("handle_NPP_New\n"));

  uint32_t instance_id;
  NPMIMEType plugin_type;
  int32_t mode;
  int argn_count, argv_count;
  char **argn, **argv;
  NPSavedData *saved;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_UINT32, &instance_id,
								  RPC_TYPE_STRING, &plugin_type,
								  RPC_TYPE_INT32, &mode,
								  RPC_TYPE_ARRAY, RPC_TYPE_STRING, &argn_count, &argn,
								  RPC_TYPE_ARRAY, RPC_TYPE_STRING, &argv_count, &argv,
								  RPC_TYPE_NP_SAVED_DATA, &saved,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_New() get args", error);
	return error;
  }

  assert(argn_count == argv_count);
  NPError ret = g_NPP_New(plugin_type, instance_id, mode, argn_count, argn, argv, saved);

  if (plugin_type)
	free(plugin_type);
  if (argn) {
	for (int i = 0; i < argn_count; i++)
	  free(argn[i]);
	free(argn);
  }
  if (argv) {
	for (int i = 0; i < argv_count; i++)
	  free(argv[i]);
	free(argv);
  }

  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NPP_Destroy
static NPError g_NPP_Destroy(NPP instance, NPSavedData **sdata)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  if (sdata)
	*sdata = NULL;

  D(bug("NPP_Destroy instance=%p\n", instance));
  NPError ret = plugin_funcs.destroy(instance, sdata);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));

  PluginInstance *plugin = instance->ndata;
  if (plugin) {
	destroy_window(plugin);
	id_remove(plugin->instance_id);
	free(plugin);
  }
  free(instance);

  return ret;
}

static int handle_NPP_Destroy(rpc_connection_t *connection)
{
  int error;
  NPP instance;
  error = rpc_method_get_args(connection, RPC_TYPE_NPP, &instance, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_Destroy() get args", error);
	return error;
  }

  NPSavedData *save_area;
  NPError ret = g_NPP_Destroy(instance, &save_area);
  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_NP_SAVED_DATA, save_area, RPC_TYPE_INVALID);
}

// NPP_SetWindow
static NPError
g_NPP_SetWindow(NPP instance, NPWindow *np_window)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  if (plugin_funcs.setwindow == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  PluginInstance *plugin = instance->ndata;
  if (plugin == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  NPWindow *window = np_window;
  if (window) {
	if (plugin->top_window) {
	  if (update_window(plugin, window) < 0)
		return NPERR_GENERIC_ERROR;
	}
	else {
	  if (create_window(plugin, window) < 0)
		return NPERR_GENERIC_ERROR;
	}
	window = &plugin->window;
  }

  D(bug("NPP_SetWindow instance=%p, window=%p\n", instance, window ? window->window : NULL));
  NPError ret = plugin_funcs.setwindow(instance, window);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));

  if (np_window == NULL)
	destroy_window(plugin);

  return ret;
}

static int handle_NPP_SetWindow(rpc_connection_t *connection)
{
  D(bug("handle_NPP_SetWindow\n"));

  int error;
  NPP instance;
  NPWindow *window;

  error = rpc_method_get_args(connection,
							  RPC_TYPE_NPP, &instance,
							  RPC_TYPE_NP_WINDOW, &window,
							  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_SetWindow() get args", error);
	return error;
  }

  NPError ret = g_NPP_SetWindow(instance, window);
  if (window)
	free(window);
  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NPP_GetValue
static NPError
g_NPP_GetValue(NPP instance, NPPVariable variable, void *value)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  if (plugin_funcs.getvalue == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  D(bug("NPP_GetValue instance=%p, variable=%d\n", instance, variable));
  NPError ret = plugin_funcs.getvalue(instance, variable, value);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

static int handle_NPP_GetValue(rpc_connection_t *connection)
{
  int error;
  NPP instance;
  int32_t variable;

  error = rpc_method_get_args(connection,
							  RPC_TYPE_NPP, &instance,
							  RPC_TYPE_INT32, &variable,
							  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_printf("ERROR: could not get NPP_GetValue variable\n");
	return error;
  }

  NPError ret = NPERR_GENERIC_ERROR;
  int variable_type = rpc_type_of_NPPVariable(variable);

  switch (variable_type) {
  case RPC_TYPE_STRING:
	{
	  char *str = NULL;
	  ret = g_NPP_GetValue(instance, variable, (void *)&str);
	  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_STRING, str, RPC_TYPE_INVALID);
	}
  case RPC_TYPE_INT32:
	{
	  uint32_t n = 0;
	  ret = g_NPP_GetValue(instance, variable, (void *)&n);
	  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INT32, n, RPC_TYPE_INVALID);
	}
  case RPC_TYPE_BOOLEAN:
	{
	  PRBool b = PR_FALSE;
	  ret = g_NPP_GetValue(instance, variable, (void *)&b);
	  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_BOOLEAN, b, RPC_TYPE_INVALID);
	}
  case RPC_TYPE_NP_OBJECT:
	{
	  NPObject *npobj = NULL;
	  ret = g_NPP_GetValue(instance, variable, (void *)&npobj);
	  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_NP_OBJECT, npobj, RPC_TYPE_INVALID);
	}
  }

  abort();
}

// NPP_URLNotify
static void
g_NPP_URLNotify(NPP instance, const char *url, NPReason reason, void *notifyData)
{
  if (instance)
	return;

  if (plugin_funcs.urlnotify == NULL)
	return;

  D(bug("NPP_URLNotify instance=%p, url='%s', reason=%s, notifyData=%p\n",
		instance, url, string_of_NPReason(reason), notifyData));
  plugin_funcs.urlnotify(instance, url, reason, notifyData);
  D(bug(" done\n"));
}

static int handle_NPP_URLNotify(rpc_connection_t *connection)
{
  int error;
  NPP instance;
  char *url;
  int32_t reason;
  void *notifyData;

  error = rpc_method_get_args(connection,
							  RPC_TYPE_NPP, &instance,
							  RPC_TYPE_STRING, &url,
							  RPC_TYPE_INT32, &reason,
							  RPC_TYPE_NP_NOTIFY_DATA, &notifyData,
							  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_URLNotify() get args", error);
	return error;
  }

  g_NPP_URLNotify(instance, url, reason, notifyData);

  if (url)
	free(url);

  return RPC_ERROR_NO_ERROR;
}

// NPP_NewStream
static NPError
g_NPP_NewStream(NPP instance, NPMIMEType type, NPStream *stream, NPBool seekable, uint16 *stype)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  if (plugin_funcs.newstream == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  D(bug("NPP_NewStream instance=%p, stream=%p, type='%s', seekable=%d, stype=%s\n",
		instance, stream, type, seekable, string_of_NPStreamType(*stype)));
  NPError ret = plugin_funcs.newstream(instance, type, stream, seekable, stype);
  D(bug(" return: %d [%s], stype=%s\n", ret, string_of_NPError(ret), string_of_NPStreamType(*stype)));
  return ret;
}

static int handle_NPP_NewStream(rpc_connection_t *connection)
{
  int error;
  NPP instance;
  uint32_t stream_id;
  uint32_t seekable;
  NPMIMEType type;

  NPStream *stream;
  if ((stream = malloc(sizeof(*stream))) == NULL)
	return RPC_ERROR_NO_MEMORY;
  memset(stream, 0, sizeof(*stream));

  error = rpc_method_get_args(connection,
							  RPC_TYPE_NPP, &instance,
							  RPC_TYPE_STRING, &type,
							  RPC_TYPE_UINT32, &stream_id,
							  RPC_TYPE_STRING, &stream->url,
							  RPC_TYPE_UINT32, &stream->end,
							  RPC_TYPE_UINT32, &stream->lastmodified,
							  RPC_TYPE_NP_NOTIFY_DATA, &stream->notifyData,
							  RPC_TYPE_BOOLEAN, &seekable,
							  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_NewStream() get args", error);
	return error;
  }

  StreamInstance *stream_ndata;
  if ((stream_ndata = malloc(sizeof(*stream_ndata))) == NULL)
	return RPC_ERROR_NO_MEMORY;
  stream->ndata = stream_ndata;
  memset(stream_ndata, 0, sizeof(*stream_ndata));
  stream_ndata->stream_id = stream_id;
  id_link(stream_id, stream_ndata);
  stream_ndata->stream = stream;

  uint16 stype = NP_NORMAL;
  NPError ret = g_NPP_NewStream(instance, type, stream, seekable, &stype);

  if (type)
	free(type);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_INT32, ret,
							   RPC_TYPE_UINT32, (uint32_t)stype,
							   RPC_TYPE_NP_NOTIFY_DATA, stream->notifyData,
							   RPC_TYPE_INVALID);
}

// NPP_DestroyStream
static NPError
g_NPP_DestroyStream(NPP instance, NPStream *stream, NPReason reason)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  if (plugin_funcs.destroystream == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  if (stream == NULL)
	return NPERR_INVALID_PARAM;

  D(bug("NPP_DestroyStream instance=%p, stream=%p, reason=%s\n",
		instance, stream, string_of_NPReason(reason)));
  NPError ret = plugin_funcs.destroystream(instance, stream, reason);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));

  StreamInstance *stream_ndata = stream->ndata;
  if (stream_ndata) {
	id_remove(stream_ndata->stream_id);
	free(stream_ndata);
  }
  free((char *)stream->url);
  free(stream);

  return ret;
}

static int handle_NPP_DestroyStream(rpc_connection_t *connection)
{
  D(bug("handle_NPP_DestroyStream\n"));

  NPP instance;
  NPStream *stream;
  int32_t reason;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_STREAM, &stream,
								  RPC_TYPE_INT32, &reason,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_DestroyStream() get args", error);
	return error;
  }

  NPError ret = g_NPP_DestroyStream(instance, stream, reason);
  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NPP_WriteReady
static int32
g_NPP_WriteReady(NPP instance, NPStream *stream)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  if (plugin_funcs.writeready == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  if (stream == NULL)
	return NPERR_INVALID_PARAM;

  D(bug("NPP_WriteReady instance=%p, stream=%p\n", instance, stream));
  int32 ret = plugin_funcs.writeready(instance, stream);
  D(bug(" return: %d\n", ret));
  return ret;
}

static int handle_NPP_WriteReady(rpc_connection_t *connection)
{
  D(bug("handle_NPP_WriteReady\n"));

  NPP instance;
  NPStream *stream;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_STREAM, &stream,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_WriteReady() get args", error);
	return error;
  }

  int32 ret = g_NPP_WriteReady(instance, stream);

  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NPP_Write
static int32
g_NPP_Write(NPP instance, NPStream *stream, int32 offset, int32 len, void *buf)
{
  if (instance == NULL)
	return -1;

  if (plugin_funcs.write == NULL)
	return -1;

  if (stream == NULL)
	return -1;

  D(bug("NPP_Write instance=%p, stream=%p, offset=%d, len=%d, buf=%p\n", instance, stream, offset, len, buf));
  int32 ret = plugin_funcs.write(instance, stream, offset, len, buf);
  D(bug(" return: %d\n", ret));
  return ret;
}

static int handle_NPP_Write(rpc_connection_t *connection)
{
  NPP instance;
  NPStream *stream;
  unsigned char *buf;
  int32_t offset, len;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_STREAM, &stream,
								  RPC_TYPE_INT32, &offset,
								  RPC_TYPE_ARRAY, RPC_TYPE_CHAR, &len, &buf,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_Write() get args", error);
	return error;
  }

  int32 ret = g_NPP_Write(instance, stream, offset, len, buf);

  if (buf)
	free(buf);

  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NPP_StreamAsFile
static void
g_NPP_StreamAsFile(NPP instance, NPStream *stream, const char *fname)
{
  if (instance == NULL)
	return;

  if (plugin_funcs.asfile == NULL)
	return;

  if (stream == NULL)
	return;

  D(bug("NPP_StreamAsFile instance=%p, stream=%p, fname='%s'\n", instance, stream, fname));
  plugin_funcs.asfile(instance, stream, fname);
  D(bug(" done\n"));
}

static int handle_NPP_StreamAsFile(rpc_connection_t *connection)
{
  NPP instance;
  NPStream *stream;
  char *fname;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_STREAM, &stream,
								  RPC_TYPE_STRING, &fname,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_StreamAsFile() get args", error);
	return error;
  }

  g_NPP_StreamAsFile(instance, stream, fname);

  if (fname)
	free(fname);

  return RPC_ERROR_NO_ERROR;
}

// NPP_Print
static void
g_NPP_Print(NPP instance, NPPrint *printInfo)
{
  if (plugin_funcs.print == NULL)
	return;

  if (printInfo == NULL)
	return;

  D(bug("NPP_Print instance=%p, printInfo->mode=%d\n", instance, printInfo->mode));
  plugin_funcs.print(instance, printInfo);
  D(bug(" done\n"));
}

static void invoke_NPN_PrintData(PluginInstance *plugin, uint32_t platform_print_id, NPPrintData *printData)
{
  if (printData == NULL)
	return;

  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPN_PRINT_DATA,
								RPC_TYPE_UINT32, platform_print_id,
								RPC_TYPE_NP_PRINT_DATA, printData,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_PrintData() invoke", error);
	return;
  }

  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_PrintData() wait for reply", error);
	return;
  }
}

static int handle_NPP_Print(rpc_connection_t *connection)
{
  NPP instance;
  NPPrint printInfo;
  uint32_t platform_print_id;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_UINT32, &platform_print_id,
								  RPC_TYPE_NP_PRINT, &printInfo,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_Print() get args", error);
	return error;
  }

  // reconstruct printer info
  NPPrintCallbackStruct printer;
  printer.type = NP_PRINT;
  printer.fp = platform_print_id ? tmpfile() : NULL;
  switch (printInfo.mode) {
  case NP_FULL:
	printInfo.print.fullPrint.platformPrint = &printer;
	break;
  case NP_EMBED:
	printInfo.print.embedPrint.platformPrint = &printer;
	// XXX the window ID is unlikely to work here as is. The NPWindow
	// is probably only used as a bounding box?
	create_window_attributes(&printInfo.print.embedPrint.window);
	break;
  }

  g_NPP_Print(instance, &printInfo);

  // send back the printed data
  if (printer.fp) {
	long file_size = ftell(printer.fp);
	D(bug(" writeback data [%d bytes]\n", file_size));
	rewind(printer.fp);
	if (file_size > 0) {
	  NPPrintData printData;
	  const int printDataMaxSize = sizeof(printData.data);
	  int n = file_size / printDataMaxSize;
	  while (--n >= 0) {
		printData.size = printDataMaxSize;
		if (fread(&printData.data, sizeof(printData.data), 1, printer.fp) != 1) {
		  npw_printf("ERROR: unexpected end-of-file or error condition in NPP_Print\n");
		  break;
		}
		invoke_NPN_PrintData(instance->ndata, platform_print_id, &printData);
	  }
	  printData.size = file_size % printDataMaxSize;
	  if (fread(&printData.data, printData.size, 1, printer.fp) != 1)
		npw_printf("ERROR: unexpected end-of-file or error condition in NPP_Print\n");
	  invoke_NPN_PrintData(instance->ndata, platform_print_id, &printData);
	}
	fclose(printer.fp);
  }

  if (printInfo.mode == NP_EMBED)
	destroy_window_attributes(&printInfo.print.embedPrint.window);

  uint32_t plugin_printed = FALSE;
  if (printInfo.mode == NP_FULL)
	plugin_printed = printInfo.print.fullPrint.pluginPrinted;
  return rpc_method_send_reply(connection, RPC_TYPE_BOOLEAN, plugin_printed, RPC_TYPE_INVALID);
}


/* ====================================================================== */
/* === Events processing                                              === */
/* ====================================================================== */

typedef gboolean (*GSourcePrepare)(GSource *, gint *);
typedef gboolean (*GSourceCheckFunc)(GSource *);
typedef gboolean (*GSourceDispatchFunc)(GSource *, GSourceFunc, gpointer);
typedef void (*GSourceFinalizeFunc)(GSource *);

// Xt events
static GPollFD xt_event_poll_fd;

static gboolean xt_event_prepare(GSource *source, gint *timeout)
{
  int mask = XtAppPending(x_app_context);
  return mask & XtIMXEvent;
}

static gboolean xt_event_check(GSource *source)
{
  if (xt_event_poll_fd.revents & G_IO_IN) {
	int mask = XtAppPending(x_app_context);
	if (mask & XtIMXEvent)
	  return TRUE;
  }
  return FALSE;
}

static gboolean xt_event_dispatch(GSource *source, GSourceFunc callback, gpointer user_data)
{
  int i;
  for (i = 0; i < 5; i++) {
	int mask = XtAppPending(x_app_context);
	if ((mask & XtIMXEvent) == 0)
	  break;
	XtAppProcessEvent(x_app_context, XtIMXEvent);
  }
  return TRUE;
}

static GSourceFuncs xt_event_funcs = {
  xt_event_prepare,
  xt_event_check,
  xt_event_dispatch,
  (GSourceFinalizeFunc)g_free,
  (GSourceFunc)NULL,
  (GSourceDummyMarshal)NULL
};

static gboolean xt_event_polling_timer_callback(gpointer user_data)
{
  int i;
  for (i = 0; i < 5; i++) {
	if ((XtAppPending(x_app_context) & (XtIMAll & ~XtIMXEvent)) == 0)
	  break;
	XtAppProcessEvent(x_app_context, XtIMAll & ~XtIMXEvent);
  }
  return TRUE;
}

// RPC events
static GPollFD rpc_event_poll_fd;

static gboolean rpc_event_prepare(GSource *source, gint *timeout)
{
  *timeout = -1;
  return FALSE;
}

static gboolean rpc_event_check(GSource *source)
{
  return rpc_wait_dispatch(g_rpc_connection, 0) > 0;
}

static gboolean rpc_event_dispatch(GSource *source, GSourceFunc callback, gpointer connection)
{
  return rpc_dispatch(connection) != RPC_ERROR_CONNECTION_CLOSED;
}

static GSourceFuncs rpc_event_funcs = {
  rpc_event_prepare,
  rpc_event_check,
  rpc_event_dispatch,
  (GSourceFinalizeFunc)g_free,
  (GSourceFunc)NULL,
  (GSourceDummyMarshal)NULL
};


/* ====================================================================== */
/* === Main program                                                   === */
/* ====================================================================== */

static int do_test(void);

static int do_main(int argc, char **argv, const char *connection_path)
{
  if (do_test() != 0)
	return 1;
  if (connection_path == NULL) {
	npw_printf("ERROR: missing connection path argument\n");
	return 1;
  }
  D(bug("  Plugin connection: %s\n", connection_path));

  // Cleanup environment, the program may fork/exec a native shell
  // script and having 32-bit libraries in LD_PRELOAD is not right,
  // though not a fatal error
#if defined(__linux__)
  if (getenv("LD_PRELOAD"))
	unsetenv("LD_PRELOAD");
#endif

  // Xt and GTK initialization
  XtToolkitInitialize();
  x_app_context = XtCreateApplicationContext();
  x_display = XtOpenDisplay(x_app_context, NULL, "npw-viewer", "npw-viewer", NULL, 0, &argc, argv);
  gtk_init(&argc, &argv);

  // Initialize RPC communication channel
  if (rpc_add_np_marshalers() < 0) {
	npw_printf("ERROR: failed to initialize plugin-side marshalers\n");
	return 1;
  }
  if ((g_rpc_connection = rpc_init_server(connection_path)) == NULL) {
	npw_printf("ERROR: failed to initialize plugin-side RPC server connection\n");
	return 1;
  }
  static const rpc_method_descriptor_t vtable[] = {
	{ RPC_METHOD_NP_GET_MIME_DESCRIPTION,		handle_NP_GetMIMEDescription },
	{ RPC_METHOD_NP_GET_VALUE,					handle_NP_GetValue },
	{ RPC_METHOD_NP_INITIALIZE,					handle_NP_Initialize },
	{ RPC_METHOD_NP_SHUTDOWN,					handle_NP_Shutdown },
	{ RPC_METHOD_NPP_NEW,						handle_NPP_New },
	{ RPC_METHOD_NPP_DESTROY,					handle_NPP_Destroy },
	{ RPC_METHOD_NPP_GET_VALUE,					handle_NPP_GetValue },
	{ RPC_METHOD_NPP_SET_WINDOW,				handle_NPP_SetWindow },
	{ RPC_METHOD_NPP_URL_NOTIFY,				handle_NPP_URLNotify },
	{ RPC_METHOD_NPP_NEW_STREAM,				handle_NPP_NewStream },
	{ RPC_METHOD_NPP_DESTROY_STREAM,			handle_NPP_DestroyStream },
	{ RPC_METHOD_NPP_WRITE_READY,				handle_NPP_WriteReady },
	{ RPC_METHOD_NPP_WRITE,						handle_NPP_Write },
	{ RPC_METHOD_NPP_STREAM_AS_FILE,			handle_NPP_StreamAsFile },
	{ RPC_METHOD_NPP_PRINT,						handle_NPP_Print },
	{ RPC_METHOD_NPCLASS_INVALIDATE,			npclass_handle_Invalidate },
	{ RPC_METHOD_NPCLASS_HAS_METHOD,			npclass_handle_HasMethod },
	{ RPC_METHOD_NPCLASS_INVOKE,				npclass_handle_Invoke },
	{ RPC_METHOD_NPCLASS_INVOKE_DEFAULT,		npclass_handle_InvokeDefault },
	{ RPC_METHOD_NPCLASS_HAS_PROPERTY,			npclass_handle_HasProperty },
	{ RPC_METHOD_NPCLASS_GET_PROPERTY,			npclass_handle_GetProperty },
	{ RPC_METHOD_NPCLASS_SET_PROPERTY,			npclass_handle_SetProperty },
	{ RPC_METHOD_NPCLASS_REMOVE_PROPERTY,		npclass_handle_RemoveProperty },
  };
  if (rpc_method_add_callbacks(g_rpc_connection, vtable, sizeof(vtable) / sizeof(vtable[0])) < 0) {
	npw_printf("ERROR: failed to setup NPP method callbacks\n");
	return 1;
  }

  id_init();

  // Initialize Xt events listener (integrate X events into GTK events loop)
  GSource *xt_source = g_source_new(&xt_event_funcs, sizeof(GSource));
  if (xt_source == NULL) {
	npw_printf("ERROR: failed to initialize Xt events listener\n");
	return 1;
  }
  g_source_set_priority(xt_source, GDK_PRIORITY_EVENTS);
  g_source_set_can_recurse(xt_source, TRUE);
  g_source_attach(xt_source, NULL);
  xt_event_poll_fd.fd = ConnectionNumber(x_display);
  xt_event_poll_fd.events = G_IO_IN;
  xt_event_poll_fd.revents = 0;
  g_source_add_poll(xt_source, &xt_event_poll_fd);

  gint xt_polling_timer_id = g_timeout_add(25,
										   xt_event_polling_timer_callback,
										   NULL);

  // Initialize RPC events listener
  GSource *rpc_source = g_source_new(&rpc_event_funcs, sizeof(GSource));
  if (rpc_source == NULL) {
	npw_printf("ERROR: failed to initialize plugin-side RPC events listener\n");
	return 1;
  }
  g_source_set_priority(rpc_source, GDK_PRIORITY_EVENTS);
  g_source_attach(rpc_source, NULL);
  rpc_event_poll_fd.fd = rpc_listen_socket(g_rpc_connection);
  rpc_event_poll_fd.events = G_IO_IN;
  rpc_event_poll_fd.revents = 0;
  g_source_set_callback(rpc_source, (GSourceFunc)rpc_dispatch, g_rpc_connection, NULL);
  g_source_add_poll(rpc_source, &rpc_event_poll_fd);

  gtk_main();
  D(bug("--- EXIT ---\n"));

  g_source_remove(xt_polling_timer_id);
  g_source_destroy(rpc_source);
  g_source_destroy(xt_source);

  if (g_user_agent)
	free(g_user_agent);
  if (g_rpc_connection)
	rpc_exit(g_rpc_connection);

  id_kill();
  return 0;
}

// Flash Player 9 beta 1 is not stable enough and will generally
// freeze on NP_Shutdown when multiple Flash movies are active
static int is_flash_player9_beta1(void)
{
  if (g_NP_GetValue) {
	const char *plugin_desc = NULL;
	if (g_NP_GetValue(NULL, NPPVpluginDescriptionString, &plugin_desc) == NPERR_NO_ERROR
		&& plugin_desc && strcmp(plugin_desc, "Shockwave Flash 9.0 d55") == 0) {
	  npw_printf("WARNING: Flash Player 9 beta 1 detected and rejected\n");
	  return 1;
	}
  }
  return 0;
}

static int do_test(void)
{
  if (g_NP_GetMIMEDescription == NULL)
	return 1;
  if (g_NP_Initialize == NULL)
	return 2;
  if (g_NP_Shutdown == NULL)
	return 3;
  if (is_flash_player9_beta1())
	return 4;
  return 0;
}

static int do_info(void)
{
  if (do_test() != 0)
	return 1;
  if (g_NP_GetValue == NULL)
	printf("0\n\n0\n\n");
  else {
	const char *plugin_name = NULL;
	if (g_NP_GetValue(NULL, NPPVpluginNameString, &plugin_name) == NPERR_NO_ERROR && plugin_name)
	  printf("%zd\n%s\n", strlen(plugin_name) + 1, plugin_name);
	else
	  printf("0\n\n");
	const char *plugin_desc = NULL;
	if (g_NP_GetValue(NULL, NPPVpluginDescriptionString, &plugin_desc) == NPERR_NO_ERROR && plugin_desc)
	  printf("%zd\n%s\n", strlen(plugin_desc) + 1, plugin_desc);
	else
	  printf("0\n\n");
  }
  const char *mime_info = g_NP_GetMIMEDescription();
  if (mime_info)
	printf("%zd\n%s\n", strlen(mime_info) + 1, mime_info);
  else
	printf("0\n\n");
  return 0;
}

static int do_help(const char *prog)
{
  printf("%s, NPAPI plugin viewer. Version %s\n", NPW_VIEWER, NPW_VERSION);
  printf("\n");
  printf("usage: %s [GTK flags] [flags]\n", prog);
  printf("   -h --help               print this message\n");
  printf("   -t --test               check plugin is compatible\n");
  printf("   -i --info               print plugin information\n");
  printf("   -p --plugin             set plugin path\n");
  printf("   -c --connection         set connection path\n");
  return 0;
}

int main(int argc, char **argv)
{
  const char *plugin_path = NULL;
  const char *connection_path = NULL;

  enum {
	CMD_RUN,
	CMD_TEST,
	CMD_INFO,
	CMD_HELP
  };
  int cmd = CMD_RUN;

  // Parse command line arguments
  for (int i = 0; i < argc; i++) {
	const char *arg = argv[i];
	if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
	  argv[i] = NULL;
	  cmd = CMD_HELP;
	}
	else if (strcmp(arg, "-t") == 0 || strcmp(arg, "--test") == 0) {
	  argv[i] = NULL;
	  cmd = CMD_TEST;
	}
	else if (strcmp(arg, "-i") == 0 || strcmp(arg, "--info") == 0) {
	  argv[i] = NULL;
	  cmd = CMD_INFO;
	}
	else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--plugin") == 0) {
	  argv[i] = NULL;
	  if (++i < argc) {
		plugin_path = argv[i];
		argv[i] = NULL;
	  }
	}
	else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--connection") == 0) {
	  argv[i] = NULL;
	  if (++i < argc) {
		connection_path = argv[i];
		argv[i] = NULL;
	  }
	}
  }

  // Remove processed arguments
  for (int i = 1, j = 1, n = argc; i < n; i++) {
	if (argv[i])
	  argv[j++] = argv[i];
	else
	  --argc;
  }

  // Open plug-in and get exported lib functions
  void *handle = NULL;
  if (plugin_path == NULL)
	cmd = CMD_HELP;
  else {
	const char *error;
	D(bug("  %s\n", plugin_path));
	if ((handle = dlopen(plugin_path, RTLD_LAZY)) == NULL) {
	  npw_printf("ERROR: %s\n", dlerror());
	  return 1;
	}
	dlerror();
	g_NP_GetMIMEDescription = (NP_GetMIMEDescriptionUPP)dlsym(handle, "NP_GetMIMEDescription");
	if ((error = dlerror()) != NULL) {
	  npw_printf("ERROR: %s\n", error);
	  return 1;
	}
	g_NP_Initialize = (NP_InitializeUPP)dlsym(handle, "NP_Initialize");
	if ((error = dlerror()) != NULL) {
	  npw_printf("ERROR: %s\n", error);
	  return 1;
	}
	g_NP_Shutdown = (NP_ShutdownUPP)dlsym(handle, "NP_Shutdown");
	if ((error = dlerror()) != NULL) {
	  npw_printf("ERROR: %s\n", error);
	  return 1;
	}
	g_NP_GetValue = (NP_GetValueUPP)dlsym(handle, "NP_GetValue");
  }

  int ret = 1;
  switch (cmd) {
  case CMD_RUN:
	ret = do_main(argc, argv, connection_path);
	break;
  case CMD_TEST:
	ret = do_test();
	break;
  case CMD_INFO:
	ret = do_info();
	break;
  case CMD_HELP:
	ret = do_help(argv[0]);
	break;
  }

  if (handle)
	dlclose(handle);
  return ret;
}
