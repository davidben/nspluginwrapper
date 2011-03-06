/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim:expandtab:shiftwidth=2:tabstop=2: */
 
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Gtk2XtBin Widget Implementation.
 *
 * The Initial Developer of the Original Code is
 * Sun Microsystems, Inc.
 * Portions created by the Initial Developer are Copyright (C) 2002
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */
 
/*
 * The GtkXtBin widget allows for Xt toolkit code to be used
 * inside a GTK application.  
 */

#include "sysdeps.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <gdk/gdkx.h>
#include "xembed.h"
#include "gtk2xtbin.h"

/* Xlib/Xt stuff */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Shell.h>
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>

/* uncomment this if you want debugging information about widget
   creation and destruction */
#define DEBUG_XTBIN 1

static void            gtk_xtbin_class_init (GtkXtBinClass *klass);
static void            gtk_xtbin_init       (GtkXtBin      *xtbin);
static void            gtk_xtbin_realize    (GtkWidget      *widget);
static void            gtk_xtbin_unrealize    (GtkWidget      *widget);
static void            gtk_xtbin_destroy    (GtkObject      *object);
static void            gtk_xtbin_shutdown   (GtkObject      *object);

/* Xt aware XEmbed */
static void       xt_client_init      (XtClient * xtclient, 
                                       Display *xtdisplay, 
                                       Visual *xtvisual, 
                                       Colormap xtcolormap, 
                                       int xtdepth);
static void       xt_client_create    (XtClient * xtclient, 
                                       Window embeder, 
                                       int height, 
                                       int width );
static void       xt_client_unrealize (XtClient* xtclient);
static void       xt_client_destroy   (XtClient* xtclient);
static void       xt_client_set_info  (Widget xtplug, 
                                       unsigned long flags);
static void       xt_client_event_handler (Widget w, 
                                           XtPointer client_data, 
                                           XEvent *event);
static void       xt_client_handle_xembed_message (Widget w, 
                                                   XtPointer client_data, 
                                                   XEvent *event);
static void       xt_client_focus_listener       (Widget w, 
                                                   XtPointer user_data, 
                                                   XEvent *event);
static void       xt_add_focus_listener( Widget w, XtPointer user_data );
static void       xt_add_focus_listener_tree ( Widget treeroot, XtPointer user_data); 
static void       xt_remove_focus_listener(Widget w, XtPointer user_data);
static void       send_xembed_message (XtClient *xtclient,
                                       long message, 
                                       long detail, 
                                       long data1, 
                                       long data2,
                                       long time);  
static int        error_handler       (Display *display, 
                                       XErrorEvent *error);
/* For error trap of XEmbed */
static void       trap_errors(void);
static int        untrap_errors(void);
static int        (*old_error_handler) (Display *, XErrorEvent *);
static int        trapped_error_code = 0;

static GtkWidgetClass *parent_class = NULL;

GtkType
gtk_xtbin_get_type (void)
{
  static GtkType xtbin_type = 0;

  if (!xtbin_type) {
      static const GtkTypeInfo xtbin_info =
      {
        "GtkXtBin",
        sizeof (GtkXtBin),
        sizeof (GtkXtBinClass),
        (GtkClassInitFunc) gtk_xtbin_class_init,
        (GtkObjectInitFunc) gtk_xtbin_init,
        /* reserved_1 */ NULL,
        /* reserved_2 */ NULL,
        (GtkClassInitFunc) NULL
      };
      xtbin_type = gtk_type_unique (GTK_TYPE_SOCKET, &xtbin_info);
    }
  return xtbin_type;
}

static void
gtk_xtbin_class_init (GtkXtBinClass *klass)
{
  GtkWidgetClass *widget_class;
  GtkObjectClass *object_class;

  parent_class = gtk_type_class (GTK_TYPE_SOCKET);

  widget_class = GTK_WIDGET_CLASS (klass);
  widget_class->realize = gtk_xtbin_realize;
  widget_class->unrealize = gtk_xtbin_unrealize;

  object_class = GTK_OBJECT_CLASS (klass);
  object_class->destroy = gtk_xtbin_destroy;
}

static void
gtk_xtbin_init (GtkXtBin *xtbin)
{
  xtbin->xtdisplay = NULL;
  xtbin->parent_window = NULL;
  xtbin->xtwindow = 0;
  xtbin->x = 0;
  xtbin->y = 0;
}

static void
gtk_xtbin_realize (GtkWidget *widget)
{
  GtkXtBin     *xtbin;
  GtkAllocation allocation = { 0, 0, 200, 200 };
  gint  x, y, w, h, d; /* geometry of window */

#ifdef DEBUG_XTBIN
  printf("gtk_xtbin_realize()\n");
#endif

  g_return_if_fail (GTK_IS_XTBIN (widget));

  xtbin = GTK_XTBIN (widget);

  /* caculate the allocation before realize */
  gdk_window_get_geometry(xtbin->parent_window, &x, &y, &w, &h, &d);
  allocation.width = w;
  allocation.height = h;
  gtk_widget_size_allocate (widget, &allocation);

#ifdef DEBUG_XTBIN
  printf("initial allocation %d %d %d %d\n", x, y, w, h);
#endif

  xtbin->width = widget->allocation.width;
  xtbin->height = widget->allocation.height;

  /* use GtkSocket's realize */
  (*GTK_WIDGET_CLASS(parent_class)->realize)(widget);

  /* create the Xt client widget */
  xt_client_create(&(xtbin->xtclient), 
       gtk_socket_get_id(GTK_SOCKET(xtbin)), 
       xtbin->height, 
       xtbin->width);
  xtbin->xtwindow = XtWindow(xtbin->xtclient.child_widget);

  gdk_flush();

  /* now that we have created the xt client, add it to the socket. */
  gtk_socket_add_id(GTK_SOCKET(widget), xtbin->xtwindow);
}



GtkWidget*
gtk_xtbin_new (GdkWindow *parent_window,
               Display   *xtdisplay,
               Visual    *xtvisual,
               Colormap   xtcolormap,
               int        xtdepth)
{
  GtkXtBin *xtbin;
  gpointer user_data;

  assert(parent_window != NULL);
  xtbin = gtk_type_new (GTK_TYPE_XTBIN);

  if (!xtbin)
    return (GtkWidget*)NULL;

  /* Initialize the Xt toolkit */
  xtbin->parent_window = parent_window;

  xt_client_init(&(xtbin->xtclient), xtdisplay, xtvisual, xtcolormap, xtdepth);

  /* Build the hierachy */
  assert(xtbin->xtclient.xtdisplay != NULL);
  xtbin->xtdisplay = xtbin->xtclient.xtdisplay;
  gtk_widget_set_parent_window(GTK_WIDGET(xtbin), parent_window);
  gdk_window_get_user_data(xtbin->parent_window, &user_data);
  if (user_data)
    gtk_container_add(GTK_CONTAINER(user_data), GTK_WIDGET(xtbin));

  return GTK_WIDGET (xtbin);
}

void
gtk_xtbin_set_position (GtkXtBin *xtbin,
                        gint       x,
                        gint       y)
{
  xtbin->x = x;
  xtbin->y = y;

  if (GTK_WIDGET_REALIZED (xtbin))
    gdk_window_move (GTK_WIDGET (xtbin)->window, x, y);
}

void
gtk_xtbin_resize (GtkWidget *widget,
                  gint       width,
                  gint       height)
{
  Arg args[2];
  GtkXtBin *xtbin = GTK_XTBIN (widget);
  GtkAllocation allocation;

#ifdef DEBUG_XTBIN
  printf("gtk_xtbin_resize %p %d %d\n", (void *)widget, width, height);
#endif

  XtSetArg(args[0], XtNheight, height);
  XtSetArg(args[1], XtNwidth,  width);
  XtSetValues(xtbin->xtclient.top_widget, args, 2);
  xtbin->height = height;
  xtbin->width  = width;

  /* we need to send a size allocate so the socket knows about the
     size changes */
  allocation.x = xtbin->x;
  allocation.y = xtbin->y;
  allocation.width = xtbin->width;
  allocation.height = xtbin->height;

  gtk_widget_size_allocate(widget, &allocation);
}

static void
gtk_xtbin_unrealize (GtkWidget *object)
{
  GtkXtBin *xtbin;
  GtkWidget *widget;

#ifdef DEBUG_XTBIN
  printf("gtk_xtbin_unrealize()\n");
#endif

  /* gtk_object_destroy() will already hold a refcount on object
   */
  xtbin = GTK_XTBIN(object);
  widget = GTK_WIDGET(object);

  GTK_WIDGET_UNSET_FLAGS (widget, GTK_VISIBLE);
  if (GTK_WIDGET_REALIZED (widget)) {
    xt_client_unrealize(&(xtbin->xtclient));
  }

  (*GTK_WIDGET_CLASS (parent_class)->unrealize)(widget);
}

static void
gtk_xtbin_destroy (GtkObject *object)
{
  GtkXtBin *xtbin;

#ifdef DEBUG_XTBIN
  printf("gtk_xtbin_destroy()\n");
#endif

  g_return_if_fail (object != NULL);
  g_return_if_fail (GTK_IS_XTBIN (object));

  xtbin = GTK_XTBIN (object);

  if(xtbin->xtwindow) {
    /* remove the event handler */
    xt_client_destroy(&(xtbin->xtclient));
    xtbin->xtwindow = 0;
  }

  GTK_OBJECT_CLASS(parent_class)->destroy(object);
}

/*
* Following is the implementation of Xt XEmbedded for client side
*/

/* Initial Xt plugin */
static void
xt_client_init( XtClient * xtclient, 
                Display *xtdisplay,
                Visual *xtvisual, 
                Colormap xtcolormap,
                int xtdepth)
{
  xtclient->top_widget = NULL;
  xtclient->child_widget = NULL;
  xtclient->xtdisplay  = xtdisplay;
  xtclient->xtvisual   = xtvisual;
  xtclient->xtcolormap = xtcolormap;
  xtclient->xtdepth    = xtdepth;
}

/* Create the Xt client widgets
*  */
static void
xt_client_create ( XtClient* xtclient , 
                   Window embedderid, 
                   int height, 
                   int width ) 
{
  int           n;
  Arg           args[6];
  Widget        child_widget;
  Widget        top_widget;

#ifdef DEBUG_XTBIN
  printf("xt_client_create() \n");
#endif
  String app_name, app_class;
  XtGetApplicationNameAndClass(xtclient->xtdisplay, &app_name, &app_class);
  top_widget = XtAppCreateShell("drawingArea", app_class, 
                                applicationShellWidgetClass, 
                                xtclient->xtdisplay, 
                                NULL, 0);
  xtclient->top_widget = top_widget;

  /* set size of Xt window */
  n = 0;
  XtSetArg(args[n], XtNheight,   height);n++;
  XtSetArg(args[n], XtNwidth,    width);n++;
  XtSetValues(top_widget, args, n);

  child_widget = XtVaCreateWidget("form", 
                                  compositeWidgetClass, 
                                  top_widget, NULL);

  n = 0;
  XtSetArg(args[n], XtNheight,   height);n++;
  XtSetArg(args[n], XtNwidth,    width);n++;
  XtSetArg(args[n], XtNvisual,   xtclient->xtvisual ); n++;
  XtSetArg(args[n], XtNdepth,    xtclient->xtdepth ); n++;
  XtSetArg(args[n], XtNcolormap, xtclient->xtcolormap ); n++;
  XtSetArg(args[n], XtNborderWidth, 0); n++;
  XtSetValues(child_widget, args, n);

  XSync(xtclient->xtdisplay, FALSE);
  xtclient->oldwindow = top_widget->core.window;
  top_widget->core.window = embedderid;

  /* this little trick seems to finish initializing the widget */
#if XlibSpecificationRelease >= 6
  XtRegisterDrawable(xtclient->xtdisplay, 
                     embedderid,
                     top_widget);
#else
  _XtRegisterWindow( embedderid,
                     top_widget);
#endif
  XtRealizeWidget(child_widget);

  /* listen to all Xt events */
  XSelectInput(xtclient->xtdisplay, 
               XtWindow(top_widget), 
               0x0FFFFF);
  xt_client_set_info (child_widget, 0);

  XtManageChild(child_widget);
  xtclient->child_widget = child_widget;

  /* set the event handler */
#if 0
  XtAddEventHandler(child_widget,
                    0x0FFFFF & ~ResizeRedirectMask,
                    TRUE, 
                    (XtEventHandler)xt_client_event_handler, xtclient);
  XtAddEventHandler(child_widget, 
                    SubstructureNotifyMask | ButtonReleaseMask, 
                    TRUE, 
                    (XtEventHandler)xt_client_focus_listener, 
                    xtclient);
#else
  XtAddEventHandler(child_widget,
                    (SubstructureNotifyMask | ButtonReleaseMask),
                    TRUE, 
                    (XtEventHandler)xt_client_event_handler, xtclient);
#endif
  XSync(xtclient->xtdisplay, FALSE);
}

static void
xt_client_unrealize ( XtClient* xtclient )
{
#if XlibSpecificationRelease >= 6
  XtUnregisterDrawable(xtclient->xtdisplay,
                       xtclient->top_widget->core.window);
#else
  _XtUnregisterWindow(xtclient->top_widget->core.window,
                      xtclient->top_widget);
#endif

  /* flush the queue before we returning origin top_widget->core.window
     or we can get X error since the window is gone */
  XSync(xtclient->xtdisplay, False);

  xtclient->top_widget->core.window = xtclient->oldwindow;
  XtUnrealizeWidget(xtclient->top_widget);
}

static void            
xt_client_destroy   (XtClient* xtclient)
{
  if(xtclient->top_widget) {
    XtRemoveEventHandler(xtclient->child_widget, 0x0FFFFF, TRUE, 
                         (XtEventHandler)xt_client_event_handler, xtclient);
    XtDestroyWidget(xtclient->top_widget);
    xtclient->top_widget = NULL;
  }
}

static void         
xt_client_set_info (Widget xtplug, unsigned long flags)
{
  unsigned long buffer[2];

  Atom infoAtom = XInternAtom(XtDisplay(xtplug), "_XEMBED_INFO", False); 

  buffer[1] = 0;                /* Protocol version */
  buffer[1] = flags;

  XChangeProperty (XtDisplay(xtplug), XtWindow(xtplug),
                   infoAtom, infoAtom, 32,
                   PropModeReplace,
                   (unsigned char *)buffer, 2);
}

static void
xt_client_handle_xembed_message(Widget w, XtPointer client_data, XEvent *event)
{
  XtClient *xtplug = (XtClient*)client_data;
  switch (event->xclient.data.l[1])
  {
  case XEMBED_EMBEDDED_NOTIFY:
    break;
  case XEMBED_WINDOW_ACTIVATE:
#ifdef DEBUG_XTBIN
    printf("Xt client get XEMBED_WINDOW_ACTIVATE\n");
#endif
    break;
  case XEMBED_WINDOW_DEACTIVATE:
#ifdef DEBUG_XTBIN
    printf("Xt client get XEMBED_WINDOW_DEACTIVATE\n");
#endif
    break;
  case XEMBED_MODALITY_ON:
#ifdef DEBUG_XTBIN
    printf("Xt client get XEMBED_MODALITY_ON\n");
#endif
    break;
  case XEMBED_MODALITY_OFF:
#ifdef DEBUG_XTBIN
    printf("Xt client get XEMBED_MODALITY_OFF\n");
#endif
    break;
  case XEMBED_FOCUS_IN:
  case XEMBED_FOCUS_OUT:
    {
      XEvent xevent;
      memset(&xevent, 0, sizeof(xevent));

      if(event->xclient.data.l[1] == XEMBED_FOCUS_IN) {
#ifdef DEBUG_XTBIN
        printf("XTEMBED got focus in\n");
#endif
        xevent.xfocus.type = FocusIn;
      }
      else {
#ifdef DEBUG_XTBIN
        printf("XTEMBED got focus out\n");
#endif
        xevent.xfocus.type = FocusOut;
      }

      xevent.xfocus.window = XtWindow(xtplug->child_widget);
      xevent.xfocus.display = XtDisplay(xtplug->child_widget);
      XSendEvent(XtDisplay(xtplug->child_widget), 
                 xevent.xfocus.window,
                 False, NoEventMask,
                 &xevent );
      XSync( XtDisplay(xtplug->child_widget), False);
    }
    break;
  default:
    break;
  } /* End of XEmbed Message */
}

static void         
xt_client_event_handler( Widget w, XtPointer client_data, XEvent *event)
{
  XtClient *xtplug = (XtClient*)client_data;
  
  switch(event->type)
    {
    case ClientMessage:
      /* Handle xembed message */
      if (event->xclient.message_type==
                 XInternAtom (XtDisplay(xtplug->child_widget),
                              "_XEMBED", False)) {
        xt_client_handle_xembed_message(w, client_data, event);
      }
      break;
    case ReparentNotify:
      break;
    case MappingNotify:
      xt_client_set_info (w, XEMBED_MAPPED);
      break;
    case UnmapNotify:
      xt_client_set_info (w, 0);
      break;
    case FocusIn:
      send_xembed_message ( xtplug,
                            XEMBED_REQUEST_FOCUS, 0, 0, 0, 0);
      break;
    case FocusOut:
      break;
    case KeyPress:
#ifdef DEBUG_XTBIN
      printf("Key Press Got!\n");
#endif
      break;
    default:
      break;
    } /* End of switch(event->type) */
}

static void
send_xembed_message (XtClient  *xtclient,
                     long      message,
                     long      detail, 
                     long      data1,  
                     long      data2,  
                     long      time)   
{
  XEvent xevent; 
  Window w=XtWindow(xtclient->top_widget);
  Display* dpy=xtclient->xtdisplay;
  int errorcode;

  memset(&xevent,0,sizeof(xevent));
  xevent.xclient.window = w;
  xevent.xclient.type = ClientMessage;
  xevent.xclient.message_type = XInternAtom(dpy,"_XEMBED",False);
  xevent.xclient.format = 32;
  xevent.xclient.data.l[0] = time; 
  xevent.xclient.data.l[1] = message;
  xevent.xclient.data.l[2] = detail; 
  xevent.xclient.data.l[3] = data1;
  xevent.xclient.data.l[4] = data2;

  trap_errors ();
  XSendEvent (dpy, w, False, NoEventMask, &xevent);
  XSync (dpy,False);

  if((errorcode = untrap_errors())) {
#ifdef DEBUG_XTBIN
    printf("send_xembed_message error(%d)!!!\n",errorcode);
#endif
  }
}

static int             
error_handler(Display *display, XErrorEvent *error)
{
  trapped_error_code = error->error_code;
  return 0;
}

static void          
trap_errors(void)
{
  trapped_error_code =0;
  old_error_handler = XSetErrorHandler(error_handler);
}

static int         
untrap_errors(void)
{
  XSetErrorHandler(old_error_handler);
  if(trapped_error_code) {
#ifdef DEBUG_XTBIN
    printf("Get X Window Error = %d\n", trapped_error_code);
#endif
  }
  return trapped_error_code;
}

static void         
xt_client_focus_listener( Widget w, XtPointer user_data, XEvent *event)
{
  Display *dpy = XtDisplay(w);
  XtClient *xtclient = user_data;
  Window win = XtWindow(w);

  switch(event->type)
    {
    case CreateNotify:
      if(event->xcreatewindow.parent == win) {
        Widget child=XtWindowToWidget( dpy, event->xcreatewindow.window);
        if (child)
          xt_add_focus_listener_tree(child, user_data);
      }
      break;
    case DestroyNotify:
      xt_remove_focus_listener( w, user_data);
      break;
    case ReparentNotify:
      if(event->xreparent.parent == win) {
        /* I am the new parent */
        Widget child=XtWindowToWidget(dpy, event->xreparent.window);
        if (child)
          xt_add_focus_listener_tree( child, user_data);
      }
      else if(event->xreparent.window == win) {
        /* I am the new child */
      }
      else {
        /* I am the old parent */
      }
      break;
    case ButtonRelease:
#if 0
      XSetInputFocus(dpy, XtWindow(xtclient->child_widget), RevertToParent, event->xbutton.time);
#endif
      send_xembed_message ( xtclient,
                            XEMBED_REQUEST_FOCUS, 0, 0, 0, 0);
      break;
    default:
      break;
    } /* End of switch(event->type) */
}

static void
xt_add_focus_listener( Widget w, XtPointer user_data)
{
  XWindowAttributes attr;
  long eventmask;
  XtClient *xtclient = user_data;
  int errorcode;

  trap_errors ();
  XGetWindowAttributes(XtDisplay(w), XtWindow(w), &attr);
  eventmask = attr.your_event_mask | SubstructureNotifyMask | ButtonReleaseMask;
  XSelectInput(XtDisplay(w),
               XtWindow(w), 
               eventmask);

  XtAddEventHandler(w, 
                    SubstructureNotifyMask | ButtonReleaseMask, 
                    TRUE, 
                    (XtEventHandler)xt_client_focus_listener, 
                    xtclient);
  untrap_errors();
}

static void
xt_remove_focus_listener(Widget w, XtPointer user_data)
{
  int errorcode;

  trap_errors ();
  XtRemoveEventHandler(w, SubstructureNotifyMask | ButtonReleaseMask, TRUE, 
                      (XtEventHandler)xt_client_focus_listener, user_data);

  untrap_errors();
}

static void
xt_add_focus_listener_tree ( Widget treeroot, XtPointer user_data) 
{
  Window win = XtWindow(treeroot);
  Window *children;
  Window root, parent;
  Display *dpy = XtDisplay(treeroot);
  unsigned int i, nchildren;

  /* ensure we don't add more than once */
  xt_remove_focus_listener( treeroot, user_data);
  xt_add_focus_listener( treeroot, user_data);
  trap_errors();
  if(!XQueryTree(dpy, win, &root, &parent, &children, &nchildren)) {
    untrap_errors();
    return;
  }

  if(untrap_errors()) 
    return;

  for(i=0; i<nchildren; ++i) {
    Widget child = XtWindowToWidget(dpy, children[i]);
    if (child) 
      xt_add_focus_listener_tree( child, user_data);
  }
  XFree((void*)children);

  return;
}

