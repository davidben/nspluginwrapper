/*
 *  npw-wrapper.c - Host Mozilla plugin (loads the actual viewer)
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

#define _GNU_SOURCE 1 /* RTLD_DEFAULT */
#include "sysdeps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <semaphore.h>
#include <sys/wait.h>

#include <glib.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/StringDefs.h>

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


// XXX unimplemented functions
#define UNIMPLEMENTED() npw_printf("WARNING: Unimplemented function %s at line %d\n", __func__, __LINE__)

// Globally exported plugin ident, used by the "npconfig" tool
const NPW_PluginInfo NPW_Plugin = {
  NPW_PLUGIN_IDENT,
  NPW_DEFAULT_PLUGIN_PATH,
  0,
  HOST_OS,
  HOST_ARCH
};

// Path to plugin to use
static const char *plugin_path = NPW_Plugin.path;

// Netscape exported functions
static NPNetscapeFuncs mozilla_funcs;

// Wrapper plugin data
typedef struct {
  int initialized;
  int viewer_pid;
  int is_wrapper;
  char *name;
  char *description;
  char *formats;
} Plugin;

static Plugin g_plugin = { 0, -1, 0, NULL, NULL, NULL };

// Instance state information about the plugin
typedef struct _PluginInstance {
  NPP instance;
  uint32_t instance_id;
} PluginInstance;

// Plugin side data for an NPStream instance
typedef struct _StreamInstance {
  NPStream *stream;
  uint32_t stream_id;
  int is_plugin_stream;
} StreamInstance;

// Prototypes
static void plugin_init(int is_NP_Initialize);
static void plugin_exit(void);

// Helpers
#ifndef min
#define min(x, y) ((x) < (y) ? (x) : (y))
#endif
#ifndef max
#define max(x, y) ((x) > (y) ? (x) : (y))
#endif


/* ====================================================================== */
/* === RPC communication                                              === */
/* ====================================================================== */

static GSource *g_rpc_source;
static GPollFD g_rpc_poll_fd;
static XtInputId xt_rpc_source_id;
rpc_connection_t *g_rpc_connection attribute_hidden = NULL;

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


/* ====================================================================== */
/* === Browser side plug-in API                                       === */
/* ====================================================================== */

// Does browser have specified feature?
#define NPN_HAS_FEATURE(FEATURE) ((mozilla_funcs.version & 0xff) >= NPVERS_HAS_##FEATURE)

// NPN_MemAlloc
static inline void *g_NPN_MemAlloc(uint32 size)
{
  return CallNPN_MemAllocProc(mozilla_funcs.memalloc, size);
}

// NPN_MemFree
static inline void g_NPN_MemFree(void* ptr)
{
  CallNPN_MemFreeProc(mozilla_funcs.memfree, ptr);
}

// NPN_MemFlush
static inline uint32 g_NPN_MemFlush(uint32 size)
{
  return CallNPN_MemFlushProc(mozilla_funcs.memflush, size);
}

// NPN_UserAgent
static const char *g_NPN_UserAgent(NPP instance)
{
  if (mozilla_funcs.uagent == NULL)
	return NULL;
  return mozilla_funcs.uagent(instance);
}

static int handle_NPN_UserAgent(rpc_connection_t *connection)
{
  D(bug("handle_NPN_UserAgent\n"));

  const char *user_agent = g_NPN_UserAgent(NULL);
  return rpc_method_send_reply(connection, RPC_TYPE_STRING, user_agent, RPC_TYPE_INVALID);
}

// NPN_Status
static int handle_NPN_Status(rpc_connection_t *connection)
{
  D(bug("handle_NPN_Status\n"));

  NPP instance;
  char *message;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_STRING, &message,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_Status() get args", error);
	return error;
  }

  D(bug(" instance=%p, message='%s'\n", instance, message));

  if (mozilla_funcs.status)
	mozilla_funcs.status(instance, message);
  if (message)
	free(message);
  return RPC_ERROR_NO_ERROR;
}

// NPN_GetValue
static NPError
g_NPN_GetValue(NPP instance, NPNVariable variable, void *value)
{
  if (mozilla_funcs.getvalue == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  D(bug("NPN_GetValue instance=%p, variable=%d\n", instance, variable));
  NPError ret = mozilla_funcs.getvalue(instance, variable, value);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

static int handle_NPN_GetValue(rpc_connection_t *connection)
{
  D(bug("handle_NPN_GetValue\n"));

  NPP instance;
  uint32_t variable;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_UINT32, &variable,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetValue() get args", error);
	return error;
  }

  NPError ret = NPERR_GENERIC_ERROR;
  switch (rpc_type_of_NPNVariable(variable)) {
  case RPC_TYPE_BOOLEAN:
	{
	  PRBool b = PR_FALSE;
	  ret = g_NPN_GetValue(instance, variable, (void *)&b);
	  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_BOOLEAN, b, RPC_TYPE_INVALID);
	}
  case RPC_TYPE_NP_OBJECT:
	{
	  NPObject *npobj = NULL;
	  ret = g_NPN_GetValue(instance, variable, (void *)&npobj);
	  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_NP_OBJECT, npobj, RPC_TYPE_INVALID);
	}
  }

  abort();
}

// NPN_GetURL
static NPError g_NPN_GetURL(NPP instance, const char *url, const char *target)
{
  if (mozilla_funcs.geturl == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  D(bug("NPN_GetURL instance=%p, url='%s', target='%s'\n", instance, url, target));
  NPError ret = CallNPN_GetURLProc(mozilla_funcs.geturl, instance, url, target);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

static int handle_NPN_GetURL(rpc_connection_t *connection)
{
  D(bug("handle_NPN_GetURL\n"));

  NPP instance;
  char *url, *target;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_STRING, &url,
								  RPC_TYPE_STRING, &target,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetURL() get args", error);
	return error;
  }

  NPError ret = g_NPN_GetURL(instance, url, target);

  if (url)
	free(url);
  if (target)
	free(target);

  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NPN_GetURLNotify
static NPError g_NPN_GetURLNotify(NPP instance, const char *url, const char *target, void *notifyData)
{
  if (mozilla_funcs.geturlnotify == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  D(bug("NPN_GetURLNotify instance=%p, url='%s', target='%s', notifyData=%p\n", instance, url, target, notifyData));
  NPError ret = CallNPN_GetURLNotifyProc(mozilla_funcs.geturlnotify, instance, url, target, notifyData);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

static int handle_NPN_GetURLNotify(rpc_connection_t *connection)
{
  D(bug("handle_NPN_GetURLNotify\n"));

  NPP instance;
  char *url, *target;
  void *notifyData;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_STRING, &url,
								  RPC_TYPE_STRING, &target,
								  RPC_TYPE_NP_NOTIFY_DATA, &notifyData,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetURLNotify() get args", error);
	return error;
  }

  NPError ret = g_NPN_GetURLNotify(instance, url, target, notifyData);

  if (url)
	free(url);
  if (target)
	free(target);

  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NPN_PostURL
static NPError g_NPN_PostURL(NPP instance, const char *url, const char *target, uint32_t len, const char *buf, NPBool file)
{
  if (mozilla_funcs.posturl == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  D(bug("NPN_PostURL instance=%p, url='%s', target='%s', file='%s'\n", instance, url, target, file ? buf : "<raw-data>"));
  NPError ret = CallNPN_PostURLProc(mozilla_funcs.posturl, instance, url, target, len, buf, file);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

static int handle_NPN_PostURL(rpc_connection_t *connection)
{
  D(bug("handle_NPN_PostURL\n"));

  NPP instance;
  char *url, *target;
  uint32_t len;
  char *buf;
  uint32_t file;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_STRING, &url,
								  RPC_TYPE_STRING, &target,
								  RPC_TYPE_ARRAY, RPC_TYPE_CHAR, &len, &buf,
								  RPC_TYPE_BOOLEAN, &file,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_PostURL() get args", error);
	return error;
  }

  NPError ret = g_NPN_PostURL(instance, url, target, len, buf, file);

  if (url)
	free(url);
  if (target)
	free(target);
  if (buf)
	free(buf);

  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NPN_PostURLNotify
static NPError g_NPN_PostURLNotify(NPP instance, const char *url, const char *target, uint32_t len, const char *buf, NPBool file, void *notifyData)
{
  if (mozilla_funcs.posturlnotify == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  D(bug("NPN_PostURLNotify instance=%p, url='%s', target='%s', file='%s', notifyData=%p\n", instance, url, target, file ? buf : "<raw-data>", notifyData));
  NPError ret = CallNPN_PostURLNotifyProc(mozilla_funcs.posturlnotify, instance, url, target, len, buf, file, notifyData);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

static int handle_NPN_PostURLNotify(rpc_connection_t *connection)
{
  D(bug("handle_NPN_PostURLNotify\n"));

  NPP instance;
  char *url, *target;
  int32_t len;
  char *buf;
  uint32_t file;
  void *notifyData;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_STRING, &url,
								  RPC_TYPE_STRING, &target,
								  RPC_TYPE_ARRAY, RPC_TYPE_CHAR, &len, &buf,
								  RPC_TYPE_BOOLEAN, &file,
								  RPC_TYPE_NP_NOTIFY_DATA, &notifyData,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_PostURLNotify() get args", error);
	return error;
  }

  NPError ret = g_NPN_PostURLNotify(instance, url, target, len, buf, file, notifyData);

  if (url)
	free(url);
  if (target)
	free(target);
  if (buf)
	free(buf);

  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NPN_PrintData
static int handle_NPN_PrintData(rpc_connection_t *connection)
{
  D(bug("handle_NPN_PrintData\n"));

  uint32_t platform_print_id;
  NPPrintData printData;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_UINT32, &platform_print_id,
								  RPC_TYPE_NP_PRINT_DATA, &printData,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_PrintData() get args", error);
	return error;
  }

  NPPrintCallbackStruct *platformPrint = id_lookup(platform_print_id);
  if (platformPrint == NULL)
	return RPC_ERROR_GENERIC;
  D(bug(" platformPrint=%p, printData.size=%d\n", platformPrint, printData.size));
  if (fwrite(printData.data, printData.size, 1, platformPrint->fp) != 1)
	return RPC_ERROR_ERRNO_SET;

  return RPC_ERROR_NO_ERROR;
}

// NPN_RequestRead
static NPError g_NPN_RequestRead(NPStream *stream, NPByteRange *rangeList)
{
  if (mozilla_funcs.requestread == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  D(bug("NPN_RequestRead stream=%p, rangeList=%p\n", stream, rangeList));
  NPError ret = CallNPN_RequestReadProc(mozilla_funcs.requestread, stream, rangeList);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

static int handle_NPN_RequestRead(rpc_connection_t *connection)
{
  D(bug("handle_NPN_RequestRead\n"));

  NPStream *stream;
  NPByteRange *rangeList;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_STREAM, &stream,
								  RPC_TYPE_NP_BYTE_RANGE, &rangeList,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_RequestRead() get args", error);
	return error;
  }

  NPError ret = g_NPN_RequestRead(stream, rangeList);

  while (rangeList) {
	NPByteRange *p = rangeList;
	rangeList = rangeList->next;
	free(p);
  }

  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NPN_NewStream
static NPError g_NPN_NewStream(NPP instance, NPMIMEType type, const char *target, NPStream **stream)
{
  if (mozilla_funcs.newstream == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  if (stream == NULL)
	return NPERR_INVALID_PARAM;

  D(bug("NPN_NewStream instance=%p, type='%s', target='%s'\n", instance, type, target));
  NPError ret = CallNPN_NewStreamProc(mozilla_funcs.newstream, instance, type, target, stream);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));

  if (ret == NPERR_NO_ERROR) {
	StreamInstance *stream_pdata = malloc(sizeof(*stream_pdata));
	if (stream_pdata == NULL)
	  return NPERR_OUT_OF_MEMORY_ERROR;
	memset(stream_pdata, 0, sizeof(*stream_pdata));
	stream_pdata->stream = *stream;
	stream_pdata->stream_id = id_create(stream_pdata);
	stream_pdata->is_plugin_stream = 1;
	(*stream)->pdata = stream_pdata;
  }
  else {
	static const StreamInstance fake_StreamInstance = {
	  .stream = NULL,
	  .stream_id = 0
	};
	static const NPStream fake_NPStream = {
	  .pdata = (void *)&fake_StreamInstance,
	  .url = NULL,
	  .end = 0,
	  .lastmodified = 0,
	  .notifyData = NULL
	};
	*stream = (void *)&fake_NPStream;
  }

  return ret;
}

static int handle_NPN_NewStream(rpc_connection_t *connection)
{
  D(bug("handle_NPN_NewStream\n"));

  NPP instance;
  char *type;
  char *target;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_STRING, &type,
								  RPC_TYPE_STRING, &target,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_NewStream() get args", error);
	return error;
  }

  NPStream *stream;
  NPError ret = g_NPN_NewStream(instance, type, target, &stream);

  if (type)
	free(type);
  if (target)
	free(target);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_INT32, ret,
							   RPC_TYPE_UINT32, ((StreamInstance *)stream->pdata)->stream_id,
							   RPC_TYPE_STRING, stream->url,
							   RPC_TYPE_UINT32, stream->end,
							   RPC_TYPE_UINT32, stream->lastmodified,
							   RPC_TYPE_NP_NOTIFY_DATA, stream->notifyData,
							   RPC_TYPE_STRING, NPN_HAS_FEATURE(RESPONSE_HEADERS) ? stream->headers : NULL,
							   RPC_TYPE_INVALID);
}

// NPN_DestroySream
static NPError
g_NPN_DestroyStream(NPP instance, NPStream *stream, NPReason reason)
{
  if (mozilla_funcs.destroystream == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  if (stream == NULL)
	return NPERR_INVALID_PARAM;

  // Mozilla calls NPP_DestroyStream() for its streams, keep stream
  // info in that case
  StreamInstance *stream_pdata = stream->pdata;
  if (stream_pdata && stream_pdata->is_plugin_stream) {
	id_remove(stream_pdata->stream_id);
	free(stream->pdata);
	stream->pdata = NULL;
  }

  D(bug("NPN_DestroyStream instance=%p, stream=%p, reason=%s\n",
		instance, stream, string_of_NPReason(reason)));
  NPError ret = CallNPN_DestroyStreamProc(mozilla_funcs.destroystream, instance, stream, reason);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));

  return ret;
}

static int handle_NPN_DestroyStream(rpc_connection_t *connection)
{
  D(bug("handle_NPN_DestroyStream\n"));

  NPP instance;
  NPStream *stream;
  int32_t reason;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_STREAM, &stream,
								  RPC_TYPE_INT32, &reason,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_DestroyStream() get args", error);
	return error;
  }

  NPError ret = g_NPN_DestroyStream(instance, stream, reason);
  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NPN_Write
static int32 g_NPN_Write(NPP instance, NPStream *stream, int32 len, void *buf)
{
  if (mozilla_funcs.write == NULL)
	return -1;

  if (stream == NULL)
	return -1;

  D(bug("NPP_Write instance=%p\n", instance));
  int32 ret = CallNPN_WriteProc(mozilla_funcs.write, instance, stream, len, buf);
  D(bug(" return: %d\n", ret));
  return ret;
}

static int handle_NPN_Write(rpc_connection_t *connection)
{
  D(bug("handle_NPN_Write\n"));

  NPP instance;
  NPStream *stream;
  unsigned char *buf;
  int32_t len;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_STREAM, &stream,
								  RPC_TYPE_ARRAY, RPC_TYPE_CHAR, &len, &buf,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_Write() get args", error);
	return error;
  }

  int32 ret = g_NPN_Write(instance, stream, len, buf);

  if (buf)
	free(buf);

  return rpc_method_send_reply(connection, RPC_TYPE_INT32, ret, RPC_TYPE_INVALID);
}

// NPN_PushPopupsEnabledState
static void g_NPN_PushPopupsEnabledState(NPP instance, NPBool enabled)
{
  if (mozilla_funcs.pushpopupsenabledstate == NULL)
	return;

  D(bug("NPN_PushPopupsEnabledState instance=%p, enabled=%d\n", instance, enabled));
  CallNPN_PushPopupsEnabledStateProc(mozilla_funcs.pushpopupsenabledstate, instance, enabled);
  D(bug(" done\n"));
}

static int handle_NPN_PushPopupsEnabledState(rpc_connection_t *connection)
{
  D(bug("handle_NPN_PushPopupsEnabledState\n"));

  NPP instance;
  uint32_t enabled;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_UINT32, &enabled,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_PushPopupsEnabledState() get args", error);
	return error;
  }

  g_NPN_PushPopupsEnabledState(instance, enabled);

  return RPC_ERROR_NO_ERROR;
}

// NPN_PopPopupsEnabledState
static void g_NPN_PopPopupsEnabledState(NPP instance)
{
  if (mozilla_funcs.poppopupsenabledstate == NULL)
	return;

  D(bug("NPN_PopPopupsEnabledState instance=%p\n", instance));
  CallNPN_PopPopupsEnabledStateProc(mozilla_funcs.poppopupsenabledstate, instance);
  D(bug(" done\n"));
}

static int handle_NPN_PopPopupsEnabledState(rpc_connection_t *connection)
{
  D(bug("handle_NPN_PopPopupsEnabledState\n"));

  NPP instance;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_PopPopupsEnabledState() get args", error);
	return error;
  }

  g_NPN_PopPopupsEnabledState(instance);

  return RPC_ERROR_NO_ERROR;
}

// NPN_CreateObject
static int handle_NPN_CreateObject(rpc_connection_t *connection)
{
  NPP instance;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_CreateObject() get args", error);
	return error;
  }

  NPObject *npobj = mozilla_funcs.createobject(instance, &npclass_bridge);

  uint32_t npobj_id = 0;
  if (npobj) {
	NPObjectInfo *npobj_info = npobject_info_new(npobj);
	if (npobj_info) {
	  npobj_id = npobj_info->npobj_id;
	  npobject_associate(npobj, npobj_info);
	}
  }

  return rpc_method_send_reply(connection, RPC_TYPE_UINT32, npobj_id, RPC_TYPE_INVALID);
}

// NPN_RetainObject
static int handle_NPN_RetainObject(rpc_connection_t *connection)
{
  NPObject *npobj;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_RetainObject() get args", error);
	return error;
  }

  if (npobj == NULL) // this shall not happen, let it crash
	npw_printf("ERROR: NPN_RetainObject got a null NPObject\n");

  mozilla_funcs.retainobject(npobj);

  return rpc_method_send_reply(connection, RPC_TYPE_UINT32, npobj->referenceCount, RPC_TYPE_INVALID);
}

// NPN_ReleaseObject
static int handle_NPN_ReleaseObject(rpc_connection_t *connection)
{
  NPObject *npobj;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_ReleaseObject() get args", error);
	return error;
  }

  if (npobj == NULL) // this shall not happen, let it crash
	npw_printf("ERROR: NPN_ReleaseObject got a null NPObject\n");

  mozilla_funcs.releaseobject(npobj);

  return rpc_method_send_reply(connection, RPC_TYPE_UINT32, npobj->referenceCount, RPC_TYPE_INVALID);
}

// NPN_Invoke
static int handle_NPN_Invoke(rpc_connection_t *connection)
{
  NPP instance;
  NPObject *npobj;
  NPIdentifier methodName;
  NPVariant *args;
  uint32_t argCount;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_NP_IDENTIFIER, &methodName,
								  RPC_TYPE_ARRAY, RPC_TYPE_NP_VARIANT, &argCount, &args,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_Invoke() get args", error);
	return error;
  }

  NPVariant result;
  VOID_TO_NPVARIANT(result);
  bool ret = mozilla_funcs.invoke(instance, npobj, methodName, args, argCount, &result);

  if (args)
	free(args);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_UINT32, ret,
							   RPC_TYPE_NP_VARIANT, &result,
							   RPC_TYPE_INVALID);
}

// NPN_InvokeDefault
static int handle_NPN_InvokeDefault(rpc_connection_t *connection)
{
  NPP instance;
  NPObject *npobj;
  NPVariant *args;
  uint32_t argCount;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_ARRAY, RPC_TYPE_NP_VARIANT, &argCount, &args,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_Invoke() get args", error);
	return error;
  }

  NPVariant result;
  VOID_TO_NPVARIANT(result);
  bool ret = mozilla_funcs.invokeDefault(instance, npobj, args, argCount, &result);

  if (args)
	free(args);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_UINT32, ret,
							   RPC_TYPE_NP_VARIANT, &result,
							   RPC_TYPE_INVALID);
}

// NPN_Evaluate
static int handle_NPN_Evaluate(rpc_connection_t *connection)
{
  NPP instance;
  NPObject *npobj;
  NPString script;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_NP_STRING, &script,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_Evaluate() get args", error);
	return error;
  }

  NPVariant result;
  VOID_TO_NPVARIANT(result);
  bool ret = mozilla_funcs.evaluate(instance, npobj, &script, &result);

  if (script.utf8characters)
	free((void *)script.utf8characters);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_UINT32, ret,
							   RPC_TYPE_NP_VARIANT, &result,
							   RPC_TYPE_INVALID);
}

// NPN_GetProperty
static int handle_NPN_GetProperty(rpc_connection_t *connection)
{
  NPP instance;
  NPObject *npobj;
  NPIdentifier propertyName;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_NP_IDENTIFIER, &propertyName,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetProperty() get args", error);
	return error;
  }

  NPVariant result;
  VOID_TO_NPVARIANT(result);
  bool ret = mozilla_funcs.getproperty(instance, npobj, propertyName, &result);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_UINT32, ret,
							   RPC_TYPE_NP_VARIANT, &result,
							   RPC_TYPE_INVALID);
}

// NPN_SetProperty
static int handle_NPN_SetProperty(rpc_connection_t *connection)
{
  NPP instance;
  NPObject *npobj;
  NPIdentifier propertyName;
  NPVariant value;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_NP_IDENTIFIER, &propertyName,
								  RPC_TYPE_NP_VARIANT, &value,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_SetProperty() get args", error);
	return error;
  }

  bool ret = mozilla_funcs.setproperty(instance, npobj, propertyName, &value);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_UINT32, ret,
							   RPC_TYPE_INVALID);
}

// NPN_RemoveProperty
static int handle_NPN_RemoveProperty(rpc_connection_t *connection)
{
  NPP instance;
  NPObject *npobj;
  NPIdentifier propertyName;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_NP_IDENTIFIER, &propertyName,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_RemoveProperty() get args", error);
	return error;
  }

  bool ret = mozilla_funcs.removeproperty(instance, npobj, propertyName);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_UINT32, ret,
							   RPC_TYPE_INVALID);
}

// NPN_HasProperty
static int handle_NPN_HasProperty(rpc_connection_t *connection)
{
  NPP instance;
  NPObject *npobj;
  NPIdentifier propertyName;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_NP_IDENTIFIER, &propertyName,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_HasProperty() get args", error);
	return error;
  }

  bool ret = mozilla_funcs.hasproperty(instance, npobj, propertyName);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_UINT32, ret,
							   RPC_TYPE_INVALID);
}

// NPN_HasMethod
static int handle_NPN_HasMethod(rpc_connection_t *connection)
{
  NPP instance;
  NPObject *npobj;
  NPIdentifier methodName;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NPP, &instance,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_NP_IDENTIFIER, &methodName,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_HasMethod() get args", error);
	return error;
  }

  bool ret = mozilla_funcs.hasmethod(instance, npobj, methodName);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_UINT32, ret,
							   RPC_TYPE_INVALID);
}

// NPN_SetException
static int handle_NPN_SetException(rpc_connection_t *connection)
{
  NPObject *npobj;
  NPUTF8 *message;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_STRING, &message,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_SetException() get args", error);
	return error;
  }

  mozilla_funcs.setexception(npobj, message);

  // XXX memory leak (message)

  return RPC_ERROR_NO_ERROR;
}

// NPN_GetStringIdentifier
static int handle_NPN_GetStringIdentifier(rpc_connection_t *connection)
{
  char *name;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_STRING, &name,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetStringIdentifier() get args", error);
	return error;
  }

  NPIdentifier ident = mozilla_funcs.getstringidentifier(name);

  if (name)
	free(name);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_NP_IDENTIFIER, ident,
							   RPC_TYPE_INVALID);
}

// NPN_GetStringIdentifiers
static int handle_NPN_GetStringIdentifiers(rpc_connection_t *connection)
{
  NPUTF8 **names;
  uint32_t nameCount;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_ARRAY, RPC_TYPE_STRING, &nameCount, &names,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetStringIdentifiers() get args", error);
	return error;
  }

  NPIdentifier *idents = malloc(nameCount * sizeof(idents[0]));
  if (idents)
	mozilla_funcs.getstringidentifiers((const NPUTF8 **)names, nameCount, idents);

  if (names) {
	for (int i = 0; i < nameCount; i++)
	  free(names[i]);
	free(names);
  }

  return rpc_method_send_reply(connection,
							   RPC_TYPE_ARRAY, RPC_TYPE_NP_IDENTIFIER, nameCount, idents,
							   RPC_TYPE_INVALID);
}

// NPN_GetIntIdentifier
static int handle_NPN_GetIntIdentifier(rpc_connection_t *connection)
{
  int32_t intid;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_INT32, &intid,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_GetIntIdentifier() get args", error);
	return error;
  }

  NPIdentifier ident = mozilla_funcs.getintidentifier(intid);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_NP_IDENTIFIER, ident,
							   RPC_TYPE_INVALID);
}

// NPN_IdentifierIsString
static int handle_NPN_IdentifierIsString(rpc_connection_t *connection)
{
  NPIdentifier ident;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_IDENTIFIER, &ident,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_IdentifierIsString() get args", error);
	return error;
  }

  bool ret = mozilla_funcs.identifierisstring(ident);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_UINT32, ret,
							   RPC_TYPE_INVALID);
}

// NPN_UTF8FromIdentifier
static int handle_NPN_UTF8FromIdentifier(rpc_connection_t *connection)
{
  NPIdentifier ident;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_IDENTIFIER, &ident,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_UTF8FromIdentifier() get args", error);
	return error;
  }

  NPUTF8 *str = mozilla_funcs.utf8fromidentifier(ident);

  error = rpc_method_send_reply(connection,
								RPC_TYPE_STRING, str,
								RPC_TYPE_INVALID);

  // the caller is responsible for deallocating the memory used by the string
  mozilla_funcs.memfree(str);

  return error;
}

// NPN_IntFromIdentifier
static int handle_NPN_IntFromIdentifier(rpc_connection_t *connection)
{
  NPIdentifier ident;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_IDENTIFIER, &ident,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPN_IntFromIdentifier() get args", error);
	return error;
  }

  int32_t ret = mozilla_funcs.intfromidentifier(ident);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_INT32, ret,
							   RPC_TYPE_INVALID);
}


/* ====================================================================== */
/* === Plug-in side data                                              === */
/* ====================================================================== */

// Creates a new instance of a plug-in
static NPError
invoke_NPP_New(NPMIMEType mime_type, NPP instance,
			   uint16_t mode, int16_t argc, char *argn[], char *argv[],
			   NPSavedData *saved)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPP_NEW,
								RPC_TYPE_UINT32, ((PluginInstance *)instance->pdata)->instance_id,
								RPC_TYPE_STRING, mime_type,
								RPC_TYPE_INT32, (int32_t)mode,
								RPC_TYPE_ARRAY, RPC_TYPE_STRING, (uint32_t)argc, argn,
								RPC_TYPE_ARRAY, RPC_TYPE_STRING, (uint32_t)argc, argv,
								RPC_TYPE_NP_SAVED_DATA, saved,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_New() invoke", error);
	return NPERR_GENERIC_ERROR;
  }

  int32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_INT32, &ret,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_New() wait for reply", error);
	return NPERR_GENERIC_ERROR;
  }

  return ret;
}

static NPError
g_NPP_New(NPMIMEType mime_type, NPP instance,
		  uint16_t mode, int16_t argc, char *argn[], char *argv[],
		  NPSavedData *saved)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  PluginInstance *plugin = malloc(sizeof(*plugin));
  if (plugin == NULL)
	return NPERR_OUT_OF_MEMORY_ERROR;
  memset(plugin, 0, sizeof(*plugin));
  plugin->instance = instance;
  plugin->instance_id = id_create(plugin);
  instance->pdata = plugin;

  D(bug("NPP_New instance=%p\n", instance));
  NPError ret = invoke_NPP_New(mime_type, instance, mode, argc, argn, argv, saved);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));

  if (saved) {
	if (saved->buf)
	  free(saved->buf);
	free(saved);
  }

  return ret;
}

// Deletes a specific instance of a plug-in
static NPError
invoke_NPP_Destroy(NPP instance, NPSavedData **save)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPP_DESTROY,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_Destroy() invoke", error);
	return NPERR_GENERIC_ERROR;
  }

  int32_t ret;
  NPSavedData *save_area = NULL;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_INT32, &ret,
									RPC_TYPE_NP_SAVED_DATA, &save_area,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_Destroy() wait for reply", error);
	return NPERR_GENERIC_ERROR;
  }

  if (save)
	*save = save_area;
  else if (save_area) {
	if (save_area->len > 0 && save_area->buf)
	  free(save_area->buf);
	free(save_area);
  }

  return ret;
}

static NPError
g_NPP_Destroy(NPP instance, NPSavedData **save)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  D(bug("NPP_Destroy instance=%p\n", instance));
  NPError ret = invoke_NPP_Destroy(instance, save);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));

  PluginInstance *plugin = instance->pdata;
  if (plugin) {
	id_remove(plugin->instance_id);
	free(plugin);
	instance->pdata = NULL;
  }

  return ret;
}

// Tells the plug-in when a window is created, moved, sized, or destroyed
static NPError
invoke_NPP_SetWindow(NPP instance, NPWindow *window)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPP_SET_WINDOW,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_NP_WINDOW, window,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_SetWindow() invoke", error);
	return NPERR_GENERIC_ERROR;
  }

  int32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_INT32, &ret,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_SetWindow() wait for reply", error);
	return NPERR_GENERIC_ERROR;
  }

  return ret;
}

static NPError
g_NPP_SetWindow(NPP instance, NPWindow *window)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  D(bug("NPP_SetWindow instance=%p\n", instance));
  NPError ret = invoke_NPP_SetWindow(instance, window);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

// Allows the browser to query the plug-in for information
static NPError
invoke_NPP_GetValue(NPP instance, NPPVariable variable, void *value)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPP_GET_VALUE,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_INT32, variable,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_GetValue() invoke", error);
	return NPERR_GENERIC_ERROR;
  }

  int32_t ret;
  switch (rpc_type_of_NPPVariable(variable)) {
  case RPC_TYPE_STRING:
	{
	  char *str = NULL;
	  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INT32, &ret, RPC_TYPE_STRING, &str, RPC_TYPE_INVALID);
	  if (error != RPC_ERROR_NO_ERROR) {
		npw_perror("NPP_GetValue() wait for reply", error);
		ret = NPERR_GENERIC_ERROR;
	  }
	  D(bug(" value: %s\n", str));
	  switch (variable) {
	  case NPPVformValue:
		// this is a '\0'-terminated UTF-8 string data allocated by NPN_MemAlloc()
		if (ret == NPERR_NO_ERROR && str) {
		  char *utf8_str = g_NPN_MemAlloc(strlen(str) + 1);
		  if (utf8_str == NULL)
			ret = NPERR_OUT_OF_MEMORY_ERROR;
		  else
			strcpy(utf8_str, str);
		  free(str);
		  str = utf8_str;
		}
		break;
	  default:
		// XXX memory leak (add to a deallocation pool?)
		break;
	  }
	  *((char **)value) = str;
	  break;
	}
  case RPC_TYPE_INT32:
	{
	  int32_t n = 0;
	  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INT32, &ret, RPC_TYPE_INT32, &n, RPC_TYPE_INVALID);
	  if (error != RPC_ERROR_NO_ERROR) {
		npw_perror("NPP_GetValue() wait for reply", error);
		ret = NPERR_GENERIC_ERROR;
	  }
	  D(bug(" value: %d\n", n));
	  *((int *)value) = n;
	  break;
	}
  case RPC_TYPE_BOOLEAN:
	{
	  uint32_t b = 0;
	  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INT32, &ret, RPC_TYPE_BOOLEAN, &b, RPC_TYPE_INVALID);
	  if (error != RPC_ERROR_NO_ERROR) {
		npw_perror("NPP_GetValue() wait for reply", error);
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
		npw_perror("NPP_GetValue() wait for reply", error);
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
g_NPP_GetValue(NPP instance, NPPVariable variable, void *value)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  switch (rpc_type_of_NPPVariable(variable)) {
  case RPC_TYPE_STRING:
  case RPC_TYPE_INT32:
  case RPC_TYPE_BOOLEAN:
  case RPC_TYPE_NP_OBJECT:
	break;
  default:
	npw_printf("WARNING: unhandled variable %d in NPP_GetValue()\n", variable);
	return NPERR_INVALID_PARAM;
  }

  D(bug("NPP_GetValue instance=%p, variable=%d\n", instance, variable));
  NPError ret = invoke_NPP_GetValue(instance, variable, value);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

// Sets information about the plug-in
static NPError
invoke_NPP_SetValue(NPP instance, NPPVariable variable, void *value)
{
  UNIMPLEMENTED();

  return NPERR_GENERIC_ERROR;
}

static NPError
g_NPP_SetValue(NPP instance, NPPVariable variable, void *value)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  D(bug("NPP_SetValue instance=%p, variable=%d\n", instance, variable));
  NPError ret = invoke_NPP_SetValue(instance, variable, value);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return NPERR_GENERIC_ERROR;
}

// Notifies the instance of the completion of a URL request
static void
invoke_NPP_URLNotify(NPP instance, const char *url, NPReason reason, void *notifyData)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPP_URL_NOTIFY,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_STRING, url,
								RPC_TYPE_INT32, reason,
								RPC_TYPE_NP_NOTIFY_DATA, notifyData,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_URLNotify() invoke", error);
	return;
  }

  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INVALID);
  
  if (error != RPC_ERROR_NO_ERROR)
	npw_perror("NPP_URLNotify() wait for reply", error);
}

static void
g_NPP_URLNotify(NPP instance, const char *url, NPReason reason, void *notifyData)
{
  if (instance == NULL)
	return;

  D(bug("NPP_URLNotify instance=%p, url='%s', reason=%d, notifyData=%p\n", instance, url, reason, notifyData));
  invoke_NPP_URLNotify(instance, url, reason, notifyData);
  D(bug(" done\n"));
}

// Notifies a plug-in instance of a new data stream
static NPError
invoke_NPP_NewStream(NPP instance, NPMIMEType type, NPStream *stream, NPBool seekable, uint16 *stype)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPP_NEW_STREAM,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_STRING, type,
								RPC_TYPE_UINT32, ((StreamInstance *)stream->pdata)->stream_id,
								RPC_TYPE_STRING, stream->url,
								RPC_TYPE_UINT32, stream->end,
								RPC_TYPE_UINT32, stream->lastmodified,
								RPC_TYPE_NP_NOTIFY_DATA, stream->notifyData,
								RPC_TYPE_STRING, NPN_HAS_FEATURE(RESPONSE_HEADERS) ? stream->headers : NULL,
								RPC_TYPE_BOOLEAN, seekable,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_NewStream() invoke", error);
	return NPERR_GENERIC_ERROR;
  }

  int32_t ret;
  uint32_t r_stype;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_INT32, &ret,
									RPC_TYPE_UINT32, &r_stype,
									RPC_TYPE_NP_NOTIFY_DATA, &stream->notifyData,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_NewStream() wait for reply", error);
	return NPERR_GENERIC_ERROR;
  }

  *stype = r_stype;
  return ret;
}

static NPError
g_NPP_NewStream(NPP instance, NPMIMEType type, NPStream *stream, NPBool seekable, uint16 *stype)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  StreamInstance *stream_pdata = malloc(sizeof(*stream_pdata));
  if (stream_pdata == NULL)
	return NPERR_OUT_OF_MEMORY_ERROR;
  memset(stream_pdata, 0, sizeof(*stream_pdata));
  stream_pdata->stream = stream;
  stream_pdata->stream_id = id_create(stream_pdata);
  stream_pdata->is_plugin_stream = 0;
  stream->pdata = stream_pdata;

  D(bug("NPP_NewStream instance=%p\n", instance));
  NPError ret = invoke_NPP_NewStream(instance, type, stream, seekable, stype);
  D(bug(" return: %d [%s], stype=%s\n", ret, string_of_NPError(ret), string_of_NPStreamType(*stype)));
  return ret;
}

// Tells the plug-in that a stream is about to be closed or destroyed
static NPError
invoke_NPP_DestroyStream(NPP instance, NPStream *stream, NPReason reason)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPP_DESTROY_STREAM,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_NP_STREAM, stream,
								RPC_TYPE_INT32, reason,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_DestroyStream() invoke", error);
	return NPERR_GENERIC_ERROR;
  }

  int32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_INT32, &ret,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_DestroyStream() wait for reply", error);
	return NPERR_GENERIC_ERROR;
  }

  return ret;
}

static NPError
g_NPP_DestroyStream(NPP instance, NPStream *stream, NPReason reason)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  D(bug("NPP_DestroyStream instance=%p\n", instance));
  NPError ret = invoke_NPP_DestroyStream(instance, stream, reason);
  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));

  StreamInstance *stream_pdata = stream->pdata;
  if (stream_pdata) {
	id_remove(stream_pdata->stream_id);
	free(stream->pdata);
	stream->pdata = NULL;
  }

  return ret;
}

// Provides a local file name for the data from a stream
static void
invoke_NPP_StreamAsFile(NPP instance, NPStream *stream, const char *fname)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPP_STREAM_AS_FILE,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_NP_STREAM, stream,
								RPC_TYPE_STRING, fname,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_StreamAsFile() invoke", error);
	return;
  }

  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR)
	npw_perror("NPP_StreamAsFile() wait for reply", error);
}

static void
g_NPP_StreamAsFile(NPP instance, NPStream *stream, const char *fname)
{
  if (instance == NULL)
	return;

  D(bug("NPP_StreamAsFile instance=%p\n", instance));
  invoke_NPP_StreamAsFile(instance, stream, fname);
  D(bug(" done\n"));
}

// Determines maximum number of bytes that the plug-in can consume
static int32
invoke_NPP_WriteReady(NPP instance, NPStream *stream)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPP_WRITE_READY,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_NP_STREAM, stream,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_WriteReady() invoke", error);
	return 0;
  }

  int32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_INT32, &ret,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_WriteReady() wait for reply", error);
	return 0;
  }

  return ret;
}

static int32
g_NPP_WriteReady(NPP instance, NPStream *stream)
{
  if (instance == NULL)
	return -1;

  D(bug("NPP_WriteReady instance=%p\n", instance));
  int32 ret = invoke_NPP_WriteReady(instance, stream);
  D(bug(" return: %d\n", ret));
  return ret;
}


// Delivers data to a plug-in instance
static int32
invoke_NPP_Write(NPP instance, NPStream *stream, int32 offset, int32 len, void *buf)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPP_WRITE,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_NP_STREAM, stream,
								RPC_TYPE_INT32, offset,
								RPC_TYPE_ARRAY, RPC_TYPE_CHAR, len, buf,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_Write() invoke", error);
	return -1;
  }

  int32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_INT32, &ret,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_Write() wait for reply", error);
	return -1;
  }

  return ret;
}

static int32
g_NPP_Write(NPP instance, NPStream *stream, int32 offset, int32 len, void *buf)
{
  if (instance == NULL)
	return -1;

  D(bug("NPP_Write instance=%p\n", instance));
  int32 ret = invoke_NPP_Write(instance, stream, offset, len, buf);
  D(bug(" return: %d\n", ret));
  return ret;
}


// Requests a platform-specific print operation for an embedded or full-screen plug-in
static void invoke_NPP_Print(NPP instance, NPPrint *PrintInfo)
{
  NPPrintCallbackStruct *platformPrint;
  switch (PrintInfo->mode) {
  case NP_FULL:
	platformPrint = PrintInfo->print.fullPrint.platformPrint;
	break;
  case NP_EMBED:
	platformPrint = PrintInfo->print.embedPrint.platformPrint;
	break;
  default:
	npw_printf("WARNING: PrintInfo mode %d is not supported\n", PrintInfo->mode);
	return;
  }
  uint32_t platform_print_id = 0;
  if (platformPrint)
	platform_print_id = id_create(platformPrint);
  D(bug(" platformPrint=%p\n", platformPrint));

  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPP_PRINT,
								RPC_TYPE_NPP, instance,
								RPC_TYPE_UINT32, platform_print_id,
								RPC_TYPE_NP_PRINT, PrintInfo,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_Print() invoke", error);
	return;
  }

  uint32_t pluginPrinted;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_BOOLEAN, &pluginPrinted,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPP_Print() wait for reply", error);
	return;
  }

  // update browser-side NPPrint struct
  if (PrintInfo->mode == NP_FULL)
	PrintInfo->print.fullPrint.pluginPrinted = pluginPrinted;

  if (platform_print_id)
	id_remove(platform_print_id);
}

static void g_NPP_Print(NPP instance, NPPrint *PrintInfo)
{
  if (instance == NULL)
	return;

  if (PrintInfo == NULL)
	return;

  D(bug("NPP_Print instance=%p\n", instance));
  invoke_NPP_Print(instance, PrintInfo);
  D(bug(" done\n"));
}

// Delivers a platform-specific window event to the instance
static int16 invoke_NPP_HandleEvent(NPP instance, void *event)
{
  UNIMPLEMENTED();

  return 0;
}

static int16 g_NPP_HandleEvent(NPP instance, void *event)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  D(bug("NPP_HandleEvent instance=%p\n", instance));
  int16 ret = invoke_NPP_HandleEvent(instance, event);
  D(bug(" return: ret\n", ret));
  return ret;
}

// Allows the browser to query the plug-in for information
NPError
NP_GetValue(void *future, NPPVariable variable, void *value)
{
  D(bug("NP_GetValue\n"));

  if (g_plugin.initialized == 0)
	plugin_init(0);
  if (g_plugin.initialized <= 0)
	return NPERR_GENERIC_ERROR;

  char *str = NULL;
  int ret = NPERR_GENERIC_ERROR;
  switch (variable) {
  case NPPVpluginNameString:
	if (g_plugin.is_wrapper) {
	  str = "NPAPI Plugins Wrapper " NPW_VERSION;
	  ret = NPERR_NO_ERROR;
	}
	else if (g_plugin.name) {
	  str = g_plugin.name;
	  ret = NPERR_NO_ERROR;
	}
	break;
  case NPPVpluginDescriptionString:
	if (g_plugin.is_wrapper) {
	  str =
		"<a href=\"http://gwenole.beauchesne.info/projects/nspluginwrapper/\">nspluginwrapper</a> "
		" is a cross-platform NPAPI plugin viewer, in particular for linux/i386 plugins.<br>"
		"This is <b>beta</b> software available under the terms of the GNU General Public License.<br>"
		;
	  ret = NPERR_NO_ERROR;
	}
	else if (g_plugin.description) {
	  str = g_plugin.description;
	  ret = NPERR_NO_ERROR;
	}
	break;
  default:
	return NPERR_INVALID_PARAM;
  }
  *((char **)value) = str;

  D(bug(" return: %d ['%s']\n", ret, str));
  return ret;
}

// Allows the browser to query the plug-in supported formats
char *
NP_GetMIMEDescription(void)
{
  D(bug("NP_GetMIMEDescription\n"));

  if (g_plugin.initialized == 0)
	plugin_init(0);
  if (g_plugin.initialized <= 0)
	return NULL;

  if (g_plugin.is_wrapper)
	return "unknown/mime-type:none:Do not open";

  D(bug(" formats: '%s'\n", g_plugin.formats));
  return g_plugin.formats;
}


/* ====================================================================== */
/* === LONG64 NPAPI support                                           === */
/* ====================================================================== */

/*
 * Dependent on NPSavedData
 *  NPP_New
 *  NPP_Destroy
 * NOTE: the browsers don't seem to care about NPSavedData
 *
 * Dependent on NPWindow / NPSetWindowCallbackStruct
 *  NPP_SetWindow
 *
 * Dependent on NPStream (plug-in side)
 *  NPP_NewStream
 *  NPP_DestroyStream
 *  NPP_WriteReady
 *  NPP_Write
 *  NPP_StreamAsFile
 *
 * Dependent on NPStream (browser-side)
 *  NPN_RequestRead
 *  NPN_NewStream
 *  NPN_DestroyStream
 *  NPN_Write
 * NOTE: Konqueror does not implement those
 *
 * Dependent on NPPrintCallbackStruct
 *  NPP_Print
 */

// Check if another thunking layer is necessary
static int g_use_long64_thunks = -1;

static void set_use_long64_thunks(bool enabled)
{
  g_use_long64_thunks = enabled;

  if (g_use_long64_thunks) {
	// XXX update mozilla_funcs with g_LONG64_*() variants
  }
}

#define NP_CVT32(VAL) ptr32->VAL = ptr64->VAL
#define NP_CVT64(VAL) ptr64->VAL = ptr32->VAL

// Check display is valid
static bool is_browser_display(Display *display)
{
  Display *browser_display = NULL;
  if (mozilla_funcs.getvalue == NULL)
	return 0;
  if (mozilla_funcs.getvalue(NULL, NPNVxDisplay, (void *)&browser_display) != NPERR_NO_ERROR)
	return 0;
  return display == browser_display;
}

// NPStream
typedef struct _LONG64_NPStream {
  void*  pdata;
  void*  ndata;
  const  char* url;
  uint64 end;
  uint64 lastmodified;
  void*  notifyData;
  const  char* headers;
} LONG64_NPStream;

static void convert_from_LONG64_NPStream(NPStream *ptr32, const LONG64_NPStream *ptr64)
{
  NP_CVT32(pdata);
  NP_CVT32(ndata);
  NP_CVT32(url);
  NP_CVT32(end);
  NP_CVT32(lastmodified);
  NP_CVT32(notifyData);
  NP_CVT32(headers);
}

#define NP_STREAM32(STREAM) get_stream32(STREAM)

static inline NPStream *get_stream32(LONG64_NPStream *stream64)
{
  NPStream *stream32 = stream64->pdata;
  if (stream32 && stream32->ndata == stream64)
	return stream32;
  return (NPStream *)stream64;
}

// NPByteRange
typedef struct _LONG64_NPByteRange {
  int64  offset;
  uint64 length;
  struct _LONG64_NPByteRange* next;
} LONG64_NPByteRange;

// NPSavedData
typedef struct _LONG64_NPSavedData {
  int64	len;
  void*	buf;
} LONG64_NPSavedData;

static void convert_from_LONG64_NPSavedData(NPSavedData *ptr32, const LONG64_NPSavedData *ptr64)
{
  NP_CVT32(len);
  NP_CVT32(buf);
}

static void convert_from_NPSavedData(LONG64_NPSavedData *ptr64, const NPSavedData *ptr32)
{
  NP_CVT64(len);
  NP_CVT64(buf);
}

// NPSetWindowCallbackStruct
typedef struct {
  int64        type;
#ifdef MOZ_X11
  Display*     display;
  Visual*      visual;
  Colormap     colormap;
  unsigned int depth;
#endif
} LONG64_NPSetWindowCallbackStruct;

static bool is_LONG64_NPSetWindowCallbackStruct(void *ws_info)
{
  LONG64_NPSetWindowCallbackStruct *ws_info64 = (LONG64_NPSetWindowCallbackStruct *)ws_info;

  return (/* LONG64_NPSetWindowCallbacStruct.type valid? */
		  (ws_info64->type == 0 || ws_info64->type == NP_SETWINDOW) &&
#ifdef MOZ_X11
		  /* LONG64_NPSetWindowCallbacStruct.display valid? */
		  is_browser_display(ws_info64->display) &&
#endif
		  1);
}

static void convert_from_LONG64_NPSetWindowCallbackStruct(NPSetWindowCallbackStruct *ptr32,
														  const LONG64_NPSetWindowCallbackStruct *ptr64)
{
  NP_CVT32(type);
#ifdef MOZ_X11
  NP_CVT32(display);
  NP_CVT32(visual);
  NP_CVT32(colormap);
  NP_CVT32(depth);
#endif
}

// NPPrintCallbackStruct
typedef struct {
  int64 type;
  FILE* fp;
} LONG64_NPPrintCallbackStruct;

static bool is_LONG64_NPPrintCallbackStruct(void *platformPrint)
{
  LONG64_NPPrintCallbackStruct *platformPrint64 = (LONG64_NPPrintCallbackStruct *)platformPrint;

  return (/* LONG64_NPPrintCallbackStruct.type valid? */
		  platformPrint64->type == NP_PRINT &&
		  /* LONG64_NPPrintCallbackStruct.file valid? */
		  platformPrint64->fp != NULL);
}

static void convert_from_LONG64_NPPrintCallbackStruct(NPPrintCallbackStruct *ptr32,
													  const LONG64_NPPrintCallbackStruct *ptr64)
{
  NP_CVT32(type);
  NP_CVT32(fp);
}

// NPWindow
typedef struct _LONG64_NPWindow {
  void* window;
  int64 x;
  int64 y;
  uint64 width;
  uint64 height;
  NPRect clipRect;
#if defined(XP_UNIX) && !defined(XP_MACOSX)
  void * ws_info;
#endif /* XP_UNIX */
  NPWindowType type;
} LONG64_NPWindow;

static bool is_LONG64_NPWindow(void *window)
{
  NPWindow *window32 = (NPWindow *)window;
  LONG64_NPWindow *window64 = (LONG64_NPWindow *)window;

  return (/* MSW32(LONG64_NPWindow.x) */
		  (window32->x == 0 || window32->x == 0xffffffff) &&
		  /* MSW32(LONG64_NPWindow.y) */
		  (window32->width == 0 || window32->width == 0xffffffff) &&
		  /* LONG64_NPWindow.clipRect.top, LONG64_NPWindow.clipRect.left */
		  (window32->type != NPWindowTypeWindow && window32->type != NPWindowTypeDrawable) &&
		  /* LONG64_NPWindow.type valid? */
		  (window64->type == NPWindowTypeWindow || window64->type == NPWindowTypeDrawable) &&
		  /* LONG64_NPWindow.ws_info valid? */
		  is_LONG64_NPSetWindowCallbackStruct(window64->ws_info));
}

static void convert_from_LONG64_NPWindow(NPWindow *ptr32, const LONG64_NPWindow *ptr64)
{
  NP_CVT32(type);
  NP_CVT32(window);
  NP_CVT32(x);
  NP_CVT32(y);
  NP_CVT32(width);
  NP_CVT32(height);
  NP_CVT32(clipRect.top);
  NP_CVT32(clipRect.left);
  NP_CVT32(clipRect.bottom);
  NP_CVT32(clipRect.right);
  convert_from_LONG64_NPSetWindowCallbackStruct(ptr32->ws_info, ptr64->ws_info);
}

// NPP_SetWindow (LONG64)
static NPError
g_LONG64_NPP_SetWindow(NPP instance, void *window)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  // Detect broken 64-bit NPAPI
  if (g_use_long64_thunks < 0)
	set_use_long64_thunks(is_LONG64_NPWindow(window));

  NPWindow window32;
  NPSetWindowCallbackStruct ws_info32;
  if (g_use_long64_thunks) {
	window32.ws_info = &ws_info32;
	convert_from_LONG64_NPWindow(&window32, window);
	window = &window32;
  }

  return g_NPP_SetWindow(instance, window);
}

// NPP_New (LONG64)
static NPError
g_LONG64_NPP_New(NPMIMEType mime_type, NPP instance,
				 uint16_t mode, int16_t argc, char *argn[], char *argv[],
				 void *saved)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  NPSavedData saved32;
  if (saved && g_use_long64_thunks > 0) {
	convert_from_LONG64_NPSavedData(&saved32, saved);
	saved = &saved32;
  }

  return g_NPP_New(mime_type, instance, mode, argc, argn, argv, saved);
}

// NPP_Destroy (LONG64)
static NPError
g_LONG64_NPP_Destroy(NPP instance, void **save)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  NPSavedData *save_area32 = NULL;
  NPError ret = g_NPP_Destroy(instance, &save_area32);

  if (save && g_use_long64_thunks > 0) {
	LONG64_NPSavedData *save_area64 = NULL;
	if (ret == NPERR_NO_ERROR && save_area32) {
	  if ((save_area64 = g_NPN_MemAlloc(save_area32->len)) != NULL)
		convert_from_NPSavedData(save_area64, save_area32);
	  free(save_area32);
	}
	*save = save_area64;
  }

  return ret;
}

// NPP_NewStream (LONG64)
static NPError
g_LONG64_NPP_NewStream(NPP instance, NPMIMEType type, void *stream, NPBool seekable, uint16 *stype)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  // Detect broken 64-bit NPAPI
  if (g_use_long64_thunks < 0) {
	npw_printf("WARNING: function using an NPStream was called too early, could not determine LONG64 data structure\n");
	set_use_long64_thunks(false);
  }

  if (g_use_long64_thunks) {
	NPStream *stream32;
	if ((stream32 = malloc(sizeof(*stream32))) == NULL)
	  return NPERR_OUT_OF_MEMORY_ERROR;
	convert_from_LONG64_NPStream(stream32, stream);
	stream32->ndata = stream;
	((NPStream *)stream)->pdata = stream32;
  }

  return g_NPP_NewStream(instance, type, NP_STREAM32(stream), seekable, stype);
}

// NPP_DestroyStream (LONG64)
static NPError
g_LONG64_NPP_DestroyStream(NPP instance, void *stream, NPReason reason)
{
  if (instance == NULL)
	return NPERR_INVALID_INSTANCE_ERROR;

  if (stream == NULL)
	return NPERR_INVALID_PARAM;

  NPError ret = g_NPP_DestroyStream(instance, NP_STREAM32(stream), reason);

  if (g_use_long64_thunks) {
	free(((NPStream *)stream)->pdata);
	((NPStream *)stream)->pdata = NULL;
  }

  return ret;
}

// NPP_WriteReady (LONG64)
static int64
g_LONG64_NPP_WriteReady(NPP instance, void *stream)
{
  if (instance == NULL)
	return -1L;

  if (stream == NULL)
	return -1L;

  return (int64)(int32)g_NPP_WriteReady(instance, NP_STREAM32(stream));
}

// NPP_Write (LONG64)
static int64
g_LONG64_NPP_Write(NPP instance, void *stream, int64 offset, int64 len, void *buf)
{
  if (instance == NULL)
	return -1L;

  if (stream == NULL)
	return -1L;

  return (int64)(int32)g_NPP_Write(instance, NP_STREAM32(stream), offset, len, buf);
}

// NPP_StreamAsFile (LONG64)
static void
g_LONG64_NPP_StreamAsFile(NPP instance, void *stream, const char *fname)
{
  if (instance == NULL)
	return;

  if (stream == NULL)
	return;

  g_NPP_StreamAsFile(instance, NP_STREAM32(stream), fname);
}

// NPP_Print (LONG64)
static void g_LONG64_NPP_Print(NPP instance, void *PrintInfo)
{
  if (instance == NULL)
	return;

  if (PrintInfo == NULL)
	return;

  // Detect broken 64-bit NPAPI
  if (g_use_long64_thunks < 0)
	set_use_long64_thunks(is_LONG64_NPPrintCallbackStruct(PrintInfo));

  NPPrint PrintInfo32;
  NPPrintCallbackStruct platformPrint32;
  if (g_use_long64_thunks) {
	memcpy(&PrintInfo32, PrintInfo, sizeof(PrintInfo32));
	void *platformPrint;
	switch (((NPPrint *)PrintInfo)->mode) {
	case NP_FULL:
	  platformPrint = ((NPPrint *)PrintInfo)->print.fullPrint.platformPrint;
	  convert_from_LONG64_NPPrintCallbackStruct(&platformPrint32, platformPrint);
	  PrintInfo32.print.fullPrint.platformPrint = &platformPrint32;
	  break;
	case NP_EMBED:
	  platformPrint = ((NPPrint *)PrintInfo)->print.embedPrint.platformPrint;
	  convert_from_LONG64_NPPrintCallbackStruct(&platformPrint32, platformPrint);
	  PrintInfo32.print.embedPrint.platformPrint = &platformPrint32;
	  break;
	}
	PrintInfo = &PrintInfo32;
  }

  g_NPP_Print(instance, PrintInfo);
}


/* ====================================================================== */
/* === Plug-in initialization                                         === */
/* ====================================================================== */

// Detect Konqueror
static bool is_konqueror(void)
{
  if (dlsym(RTLD_DEFAULT, "qApp") == NULL)
	return false;
  if (mozilla_funcs.getvalue == NULL)
	return false;
  Display *x_display = NULL;
  if (mozilla_funcs.getvalue(NULL, NPNVxDisplay, (void *)&x_display) != NPERR_NO_ERROR)
	return false;
  XtAppContext x_app_context = NULL;
  if (mozilla_funcs.getvalue(NULL, NPNVxtAppContext, (void *)&x_app_context) != NPERR_NO_ERROR)
	return false;
  if (x_display == NULL || x_app_context == NULL)
	return false;
  String name, class;
  XtGetApplicationNameAndClass(x_display, &name, &class);
  if (strcmp(name, "nspluginviewer") == 0)
	return true;
  // XXX user-agent string can be changed, but it's still an heuristic
  const char *user_agent = g_NPN_UserAgent(NULL);
  if (user_agent == NULL)
	return false;
  if (strstr(user_agent, "Konqueror") != NULL)
	return true;
  return false;
}

// Provides global initialization for a plug-in
NPError
NP_Initialize(NPNetscapeFuncs *moz_funcs, NPPluginFuncs *plugin_funcs)
{
  D(bug("NP_Initialize\n"));

  if (moz_funcs == NULL || plugin_funcs == NULL)
	return NPERR_INVALID_FUNCTABLE_ERROR;

  if ((moz_funcs->version >> 8) != NP_VERSION_MAJOR)
    return NPERR_INCOMPATIBLE_VERSION_ERROR;
  // for now, we only need fields up to including forceRedraw
  if (moz_funcs->size < (offsetof(NPNetscapeFuncs, forceredraw) + sizeof(NPN_ForceRedrawUPP)))
    return NPERR_INVALID_FUNCTABLE_ERROR;
  if (plugin_funcs->size < sizeof(NPPluginFuncs))
    return NPERR_INVALID_FUNCTABLE_ERROR;
  if (g_plugin.is_wrapper)
	return NPERR_NO_ERROR;

  // copy mozilla_funcs table here as plugin_init() will need it
  memcpy(&mozilla_funcs, moz_funcs, min(moz_funcs->size, sizeof(mozilla_funcs)));

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

  // override function table with an additional thunking layer for
  // possibly broken 64-bit Konqueror versions (NPAPI 0.11)
  if (is_konqueror() && sizeof(void *) == 8 && ! NPN_HAS_FEATURE(NPRUNTIME_SCRIPTING)) {
	D(bug("Installing Konqueror workarounds\n"));
	plugin_funcs->setwindow = NewNPP_SetWindowProc(g_LONG64_NPP_SetWindow);
	plugin_funcs->newstream = NewNPP_NewStreamProc(g_LONG64_NPP_NewStream);
	plugin_funcs->destroystream = NewNPP_DestroyStreamProc(g_LONG64_NPP_DestroyStream);
	plugin_funcs->asfile = NewNPP_StreamAsFileProc(g_LONG64_NPP_StreamAsFile);
	plugin_funcs->writeready = NewNPP_WriteReadyProc(g_LONG64_NPP_WriteReady);
	plugin_funcs->write = NewNPP_WriteProc(g_LONG64_NPP_Write);
	plugin_funcs->print = NewNPP_PrintProc(g_LONG64_NPP_Print);
	plugin_funcs->newp = NewNPP_NewProc(g_LONG64_NPP_New);
	plugin_funcs->destroy = NewNPP_DestroyProc(g_LONG64_NPP_Destroy);
  }

  if (g_plugin.initialized == 0 || g_plugin.initialized == 1)
	plugin_init(1);
  if (g_plugin.initialized <= 0)
	return NPERR_MODULE_LOAD_FAILED_ERROR;

  if (!npobject_bridge_new())
	return NPERR_MODULE_LOAD_FAILED_ERROR;

  // pass down common NPAPI version supported by both the underlying
  // browser and the thunking capabilities of nspluginwrapper
  uint32_t version = min(moz_funcs->version, plugin_funcs->version);

  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NP_INITIALIZE,
								RPC_TYPE_UINT32, (uint32_t)version,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NP_Initialize() invoke", error);
	return NPERR_MODULE_LOAD_FAILED_ERROR;
  }

  int32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INT32, &ret, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NP_Initialize() wait for reply", error);
	return NPERR_MODULE_LOAD_FAILED_ERROR;
  }

  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

// Provides global deinitialization for a plug-in
static NPError
invoke_NP_Shutdown(void)
{
  int error = rpc_method_invoke(g_rpc_connection, RPC_METHOD_NP_SHUTDOWN, RPC_TYPE_INVALID);
  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NP_Shutdown() invoke", error);
	return NPERR_GENERIC_ERROR;
  }

  int32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INT32, &ret, RPC_TYPE_INVALID);
  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NP_Shutdown() wait for reply", error);
	return NPERR_GENERIC_ERROR;
  }

  npobject_bridge_destroy();

  return ret;
}

NPError
NP_Shutdown(void)
{
  D(bug("NP_Shutdown\n"));

  int32_t ret = NPERR_NO_ERROR;

  if (g_rpc_connection)
	ret = invoke_NP_Shutdown();

  if (!g_plugin.is_wrapper)
	plugin_exit();

  D(bug(" return: %d [%s]\n", ret, string_of_NPError(ret)));
  return ret;
}

// Initialize wrapper plugin and execute viewer
static void plugin_init(int is_NP_Initialize)
{
  if (g_plugin.initialized < 0)
	return;
  g_plugin.initialized = -1;

  D(bug("plugin_init for %s\n", plugin_path));
  if (strcmp(plugin_path, NPW_DEFAULT_PLUGIN_PATH) == 0) {
	g_plugin.is_wrapper = 1;
	g_plugin.initialized = 1 + is_NP_Initialize;
	return;
  }

  static const char *plugin_file_name = NULL;
  if (plugin_file_name == NULL) {
	const char *p;
	for (p = &plugin_path[strlen(plugin_path) - 1]; p > plugin_path; p--) {
	  if (*p == '/') {
		plugin_file_name = p + 1;
		break;
	  }
	}
	if (plugin_file_name == NULL)
	  return;
  }

  static int init_count = 0;
  ++init_count;
  char viewer_path[PATH_MAX];
  sprintf(viewer_path, "%s/%s/%s/%s", NPW_LIBDIR, NPW_Plugin.target_arch, NPW_Plugin.target_os, NPW_VIEWER);
  char connection_path[128];
  sprintf(connection_path, "%s/%s/%d-%d", NPW_CONNECTION_PATH, plugin_file_name, getpid(), init_count);

  // Cache MIME info and plugin name/description
  if (g_plugin.name == NULL && g_plugin.description == NULL && g_plugin.formats == NULL) {
	char command[1024];
	if (snprintf(command, sizeof(command), "%s --info --plugin %s", viewer_path, plugin_path) >= sizeof(command))
	  return;
	FILE *viewer_fp = popen(command, "r");
	if (viewer_fp == NULL)
	  return;
	char line[256];
	while (fgets(line, sizeof(line), viewer_fp)) {
	  // Read line
	  int len = strlen(line);
	  if (len == 0)
		continue;
	  line[len - 1] = '\0';

	  // Parse line
	  char tag[sizeof(line)];
	  if (sscanf(line, "%s %d", tag, &len) == 2) {
		char *str = malloc(++len);
		if (str && fgets(str, len, viewer_fp)) {
		  if (strcmp(tag, "PLUGIN_NAME") == 0)
			g_plugin.name = str;
		  else if (strcmp(tag, "PLUGIN_DESC") == 0)
			g_plugin.description = str;
		  else if (strcmp(tag, "PLUGIN_MIME") == 0)
			g_plugin.formats = str;
		}
	  }
	}
	pclose(viewer_fp);
	g_plugin.initialized = 1;
  }

  if (!is_NP_Initialize)
	return;

  // Start plug-in viewer
  if ((g_plugin.viewer_pid = fork()) == 0) {
	char *argv[8];
	int argc = 0;

	argv[argc++] = NPW_VIEWER;
	argv[argc++] = "--plugin";
	argv[argc++] = (char *)plugin_path;
	argv[argc++] = "--connection";
	argv[argc++] = connection_path;
	argv[argc] = NULL;

	execv(viewer_path, argv);
	npw_printf("ERROR: failed to execute NSPlugin viewer\n");
	_Exit(255);
  }

  // Initialize browser-side RPC communication channel
  if (rpc_add_np_marshalers() < 0) {
	npw_printf("ERROR: failed to initialize browser-side marshalers\n");
	return;
  }
  if ((g_rpc_connection = rpc_init_client(connection_path)) == NULL) {
	npw_printf("ERROR: failed to initialize plugin-side RPC client connection\n");
	return;
  }
  static const rpc_method_descriptor_t vtable[] = {
	{ RPC_METHOD_NPN_USER_AGENT,						handle_NPN_UserAgent },
	{ RPC_METHOD_NPN_GET_VALUE,							handle_NPN_GetValue },
	{ RPC_METHOD_NPN_GET_URL,							handle_NPN_GetURL },
	{ RPC_METHOD_NPN_GET_URL_NOTIFY,					handle_NPN_GetURLNotify },
	{ RPC_METHOD_NPN_POST_URL,							handle_NPN_PostURL },
	{ RPC_METHOD_NPN_POST_URL_NOTIFY,					handle_NPN_PostURLNotify },
	{ RPC_METHOD_NPN_STATUS,							handle_NPN_Status },
	{ RPC_METHOD_NPN_PRINT_DATA,						handle_NPN_PrintData },
	{ RPC_METHOD_NPN_REQUEST_READ,						handle_NPN_RequestRead },
	{ RPC_METHOD_NPN_NEW_STREAM,						handle_NPN_NewStream },
	{ RPC_METHOD_NPN_DESTROY_STREAM,					handle_NPN_DestroyStream },
	{ RPC_METHOD_NPN_WRITE,								handle_NPN_Write },
	{ RPC_METHOD_NPN_PUSH_POPUPS_ENABLED_STATE,			handle_NPN_PushPopupsEnabledState },
	{ RPC_METHOD_NPN_POP_POPUPS_ENABLED_STATE,			handle_NPN_PopPopupsEnabledState },
	{ RPC_METHOD_NPN_CREATE_OBJECT,						handle_NPN_CreateObject },
	{ RPC_METHOD_NPN_RETAIN_OBJECT,						handle_NPN_RetainObject },
	{ RPC_METHOD_NPN_RELEASE_OBJECT,					handle_NPN_ReleaseObject },
	{ RPC_METHOD_NPN_INVOKE,							handle_NPN_Invoke },
	{ RPC_METHOD_NPN_INVOKE_DEFAULT,					handle_NPN_InvokeDefault },
	{ RPC_METHOD_NPN_EVALUATE,							handle_NPN_Evaluate },
	{ RPC_METHOD_NPN_GET_PROPERTY,						handle_NPN_GetProperty },
	{ RPC_METHOD_NPN_SET_PROPERTY,						handle_NPN_SetProperty },
	{ RPC_METHOD_NPN_REMOVE_PROPERTY,					handle_NPN_RemoveProperty },
	{ RPC_METHOD_NPN_HAS_PROPERTY,						handle_NPN_HasProperty },
	{ RPC_METHOD_NPN_HAS_METHOD,						handle_NPN_HasMethod },
	{ RPC_METHOD_NPN_SET_EXCEPTION,						handle_NPN_SetException },
	{ RPC_METHOD_NPN_GET_STRING_IDENTIFIER,				handle_NPN_GetStringIdentifier },
	{ RPC_METHOD_NPN_GET_STRING_IDENTIFIERS,			handle_NPN_GetStringIdentifiers },
	{ RPC_METHOD_NPN_GET_INT_IDENTIFIER,				handle_NPN_GetIntIdentifier },
	{ RPC_METHOD_NPN_IDENTIFIER_IS_STRING,				handle_NPN_IdentifierIsString },
	{ RPC_METHOD_NPN_UTF8_FROM_IDENTIFIER,				handle_NPN_UTF8FromIdentifier },
	{ RPC_METHOD_NPN_INT_FROM_IDENTIFIER,				handle_NPN_IntFromIdentifier },
	{ RPC_METHOD_NPCLASS_INVALIDATE,					npclass_handle_Invalidate },
	{ RPC_METHOD_NPCLASS_HAS_METHOD,					npclass_handle_HasMethod },
	{ RPC_METHOD_NPCLASS_INVOKE,						npclass_handle_Invoke },
	{ RPC_METHOD_NPCLASS_INVOKE_DEFAULT,				npclass_handle_InvokeDefault },
	{ RPC_METHOD_NPCLASS_HAS_PROPERTY,					npclass_handle_HasProperty },
	{ RPC_METHOD_NPCLASS_GET_PROPERTY,					npclass_handle_GetProperty },
	{ RPC_METHOD_NPCLASS_SET_PROPERTY,					npclass_handle_SetProperty },
	{ RPC_METHOD_NPCLASS_REMOVE_PROPERTY,				npclass_handle_RemoveProperty },
  };
  if (rpc_method_add_callbacks(g_rpc_connection, vtable, sizeof(vtable) / sizeof(vtable[0])) < 0) {
	npw_printf("ERROR: failed to setup NPN method callbacks\n");
	return;
  }

  // Retrieve toolkit information
  if (mozilla_funcs.getvalue == NULL)
	return;
  NPNToolkitType toolkit = 0;
  mozilla_funcs.getvalue(NULL, NPNVToolkit, (void *)&toolkit);

  // Initialize RPC events listener, try to attach it to the main event loop
  if (toolkit == NPNVGtk12 || toolkit == NPNVGtk2) {	// GLib
	D(bug("  trying to attach RPC listener to main GLib event loop\n"));
	static GSourceFuncs rpc_event_funcs = {
	  rpc_event_prepare,
	  rpc_event_check,
	  rpc_event_dispatch,
	  (void (*)(GSource *))g_free,
	  (GSourceFunc)NULL,
	  (GSourceDummyMarshal)NULL
	};
	g_rpc_source = g_source_new(&rpc_event_funcs, sizeof(GSource));
	if (g_rpc_source) {
	  g_source_set_priority(g_rpc_source, G_PRIORITY_DEFAULT);
	  g_source_set_callback(g_rpc_source, (GSourceFunc)rpc_dispatch, g_rpc_connection, NULL); 
	  g_source_attach(g_rpc_source, NULL);
	  g_rpc_poll_fd.fd = rpc_socket(g_rpc_connection);
	  g_rpc_poll_fd.events = G_IO_IN;
	  g_rpc_poll_fd.revents = 0;
	  g_source_add_poll(g_rpc_source, &g_rpc_poll_fd);
	}
  }
  if (g_rpc_source == NULL) {							// X11
	D(bug("  trying to attach RPC listener to main X11 event loop\n"));
	XtAppContext x_app_context = NULL;
	int error = mozilla_funcs.getvalue(NULL, NPNVxtAppContext, (void *)&x_app_context);
	if (error != NPERR_NO_ERROR || x_app_context == NULL) {
	  D(bug("  ... getting raw application context through X display\n"));
	  Display *x_display = NULL;
	  error = mozilla_funcs.getvalue(NULL, NPNVxDisplay, (void *)&x_display);
	  if (error == NPERR_NO_ERROR && x_display)
		x_app_context = XtDisplayToApplicationContext(x_display);
	}
	if (x_app_context) {
	  xt_rpc_source_id = XtAppAddInput(x_app_context,
									   rpc_socket(g_rpc_connection),
									   (XtPointer)XtInputReadMask,
									   (XtInputCallbackProc)rpc_dispatch, g_rpc_connection);
	}
  }
  if (g_rpc_source == NULL && xt_rpc_source_id == 0) {
	npw_printf("ERROR: failed to initialize brower-side RPC events listener\n");
	return;
  }

  if (!id_init()) {
	npw_printf("ERROR: failed to allocate ID hash table\n");
	return;
  }

  g_plugin.initialized = 1 + is_NP_Initialize;
  D(bug("--- INIT ---\n"));
}

// Kill NSPlugin Viewer process
static void plugin_exit(void)
{
  D(bug("plugin_exit\n"));

  if (xt_rpc_source_id) {
	XtRemoveInput(xt_rpc_source_id);
	xt_rpc_source_id = 0;
  }

  if (g_rpc_source) {
	g_source_destroy(g_rpc_source);
	g_rpc_source = NULL;
  }

  if (g_rpc_connection) {
	rpc_exit(g_rpc_connection);
	g_rpc_connection = NULL;
  }

  if (g_plugin.viewer_pid != -1) {
	// let it shutdown gracefully, then kill it gently to no mercy
	const int WAITPID_DELAY_TO_SIGTERM = 3;
	const int WAITPID_DELAY_TO_SIGKILL = 3;
	int counter = 0;
	while (waitpid(g_plugin.viewer_pid, NULL, WNOHANG) == 0) {
	  if (++counter > WAITPID_DELAY_TO_SIGTERM) {
		kill(g_plugin.viewer_pid, SIGTERM);
		counter = 0;
		while (waitpid(g_plugin.viewer_pid, NULL, WNOHANG) == 0) {
		  if (++counter > WAITPID_DELAY_TO_SIGKILL) {
			kill(g_plugin.viewer_pid, SIGKILL);
			break;
		  }
		  sleep(1);
		}
		break;
	  }
	  sleep(1);
	}
	g_plugin.viewer_pid = -1;
  }

  id_kill();

  g_plugin.initialized = 0;
}

static void __attribute__((destructor)) plugin_exit_sentinel(void)
{
  plugin_exit();

  if (g_plugin.formats) {
	free(g_plugin.formats);
	g_plugin.formats = NULL;
  }

  if (g_plugin.name) {
	free(g_plugin.name);
	g_plugin.name = NULL;
  }

  if (g_plugin.description) {
	free(g_plugin.description);
	g_plugin.description = NULL;
  }
}
