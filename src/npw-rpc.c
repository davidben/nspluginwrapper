/*
 *  npw-rpc.c - Remote Procedure Calls (NPAPI specialisation)
 *
 *  nspluginwrapper (C) 2005-2006 Gwenole Beauchesne
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

#include "sysdeps.h"
#include <assert.h>

#include "rpc.h"
#include "npw-rpc.h"
#include "utils.h"

#define XP_UNIX 1
#define MOZ_X11 1
#include <npapi.h>
#include <npruntime.h>
#include "npruntime-impl.h"

#define DEBUG 1
#include "debug.h"


/*
 *  RPC types of NPP/NPN variables
 */

int rpc_type_of_NPNVariable(int variable)
{
  int type;
  switch (variable) {
  case NPNVjavascriptEnabledBool:
  case NPNVasdEnabledBool:
  case NPNVisOfflineBool:
  case NPNVSupportsXEmbedBool:
	type = RPC_TYPE_BOOLEAN;
	break;
  case NPNVWindowNPObject:
  case NPNVPluginElementNPObject:
	type = RPC_TYPE_NP_OBJECT;
	break;
  default:
	type = RPC_ERROR_GENERIC;
	break;
  }
  return type;
}

int rpc_type_of_NPPVariable(int variable)
{
  int type;
  switch (variable) {
  case NPPVpluginNameString:
  case NPPVpluginDescriptionString:
	type = RPC_TYPE_STRING;
	break;
  case NPPVpluginWindowBool:
  case NPPVpluginTransparentBool:
  case NPPVpluginWindowSize:
  case NPPVpluginTimerInterval:
	type = RPC_TYPE_INT32;
	break;
  case NPPVpluginNeedsXEmbed:
	type = RPC_TYPE_BOOLEAN;
	break;
  case NPPVpluginScriptableNPObject:
	type = RPC_TYPE_NP_OBJECT;
	break;
  default:
	type = RPC_ERROR_GENERIC;
	break;
  }
  return type;
}


/*
 *  Process NPP objects
 */

typedef struct {
  NPP instance;
  uint32_t instance_id;
} PluginInstance;

#ifdef  BUILD_WRAPPER
#define PLUGIN_INSTANCE(instance) ((instance)->pdata)
#endif
#ifdef  BUILD_VIEWER
#define PLUGIN_INSTANCE(instance) ((instance)->ndata)
#endif

static int do_send_NPP(rpc_message_t *message, void *p_value)
{
  uint32_t instance_id = 0;
  NPP instance = (NPP)p_value;
  if (instance) {
	PluginInstance *plugin = PLUGIN_INSTANCE(instance);
	if (plugin)
	  instance_id = plugin->instance_id;
  }
  return rpc_message_send_uint32(message, instance_id);
}

static int do_recv_NPP(rpc_message_t *message, void *p_value)
{
  int error;
  uint32_t instance_id;

  if ((error = rpc_message_recv_uint32(message, &instance_id)) < 0)
	return error;

  PluginInstance *plugin = id_lookup(instance_id);
  if (instance_id && plugin == NULL)
	npw_printf("ERROR: passing an unknown instance\n");
  if (plugin && plugin->instance == NULL)
	npw_printf("ERROR: passing a NULL instance through plugin instance id\n");
  *((NPP *)p_value) = plugin ? plugin->instance : NULL;
  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NPStream objects
 */

typedef struct {
  NPStream *stream;
  uint32_t stream_id;
} StreamInstance;

#ifdef  BUILD_WRAPPER
#define STREAM_INSTANCE(instance) ((instance)->pdata)
#endif
#ifdef  BUILD_VIEWER
#define STREAM_INSTANCE(instance) ((instance)->ndata)
#endif

static int do_send_NPStream(rpc_message_t *message, void *p_value)
{
  uint32_t stream_id = 0;
  NPStream *stream = (NPStream *)p_value;
  if (stream) {
	StreamInstance *sip = STREAM_INSTANCE(stream);
	if (sip)
	  stream_id = sip->stream_id;
  }
  return rpc_message_send_uint32(message, stream_id);
}

static int do_recv_NPStream(rpc_message_t *message, void *p_value)
{
  int error;
  uint32_t stream_id;

  if ((error = rpc_message_recv_uint32(message, &stream_id)) < 0)
	return error;

  StreamInstance *stream = id_lookup(stream_id);
  *((NPStream **)p_value) = stream ? stream->stream : NULL;
  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NPByteRange objects
 */

static int do_send_NPByteRange(rpc_message_t *message, void *p_value)
{
  NPByteRange *range = (NPByteRange *)p_value;
  while (range) {
	int error;
	if ((error = rpc_message_send_uint32(message, 1)) < 0)
	  return error;
	if ((error = rpc_message_send_int32(message, range->offset)) < 0)
	  return error;
	if ((error = rpc_message_send_uint32(message, range->length)) < 0)
	  return error;
	range = range->next;
  }
  return rpc_message_send_uint32(message, 0);
}

static int do_recv_NPByteRange(rpc_message_t *message, void *p_value)
{
  NPByteRange **rangeListPtr = (NPByteRange **)p_value;
  if (rangeListPtr == NULL)
	return RPC_ERROR_MESSAGE_ARGUMENT_INVALID;
  *rangeListPtr = NULL;

  for (;;) {
	int error;
	uint32_t cont;

	if ((error = rpc_message_recv_uint32(message, &cont)) < 0)
	  return error;
	if (!cont)
	  break;
	NPByteRange *range = malloc(sizeof(*range));
	if (range == NULL)
	  return RPC_ERROR_NO_MEMORY;
	range->next = NULL;
	if ((error = rpc_message_recv_int32(message, &range->offset)) < 0)
	  return error;
	if ((error = rpc_message_recv_uint32(message, &range->length)) < 0)
	  return error;
	*rangeListPtr = range;
	rangeListPtr = &range->next;
  }
  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NPSavedData objects
 */

static int do_send_NPSavedData(rpc_message_t *message, void *p_value)
{
  NPSavedData *save_area = (NPSavedData *)p_value;
  int error;

  if (save_area == NULL) {
	if ((error = rpc_message_send_int32(message, 0)) < 0)
	  return error;
  }
  else {
	if ((error = rpc_message_send_int32(message, save_area->len)) < 0)
	  return error;
	if ((error = rpc_message_send_bytes(message, save_area->buf, save_area->len)) < 0)
	  return error;
  }

  return RPC_ERROR_NO_ERROR;
}

static int do_recv_NPSavedData(rpc_message_t *message, void *p_value)
{
  NPSavedData *save_area;
  int error;
  int32_t len;
  unsigned char *buf;

  if ((error = rpc_message_recv_int32(message, &len)) < 0)
	return error;
  if (len == 0)
	save_area = NULL;
  else {
	if ((save_area = malloc(sizeof(*save_area))) == NULL)
	  return RPC_ERROR_NO_MEMORY;
	if ((buf = malloc(len)) == NULL)
	  return RPC_ERROR_NO_MEMORY;
	if ((error = rpc_message_recv_bytes(message, buf, len)) < 0)
	  return error;
	save_area->len = len;
	save_area->buf = buf;
  }

  if (p_value)
	*((NPSavedData **)p_value) = save_area;
  else if (save_area) {
	free(save_area->buf);
	free(save_area);
  }

  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NotifyData objects
 */

// Rationale: NotifyData objects are allocated on the plugin side
// only. IDs are passed through to the browser, and they have no
// meaning on that side as they are only used to get passed back to
// the plugin side
//
// XXX 64-bit viewers in 32-bit wrappers are not supported
static int do_send_NotifyData(rpc_message_t *message, void *p_value)
{
  void *notifyData = (void *)p_value;
  return rpc_message_send_uint64(message, (uintptr_t)notifyData);
}

static int do_recv_NotifyData(rpc_message_t *message, void *p_value)
{
  int error;
  uint64_t id;

  if ((error = rpc_message_recv_uint64(message, &id)) < 0)
	return error;

  if (sizeof(void *) == 4 && ((uint32_t)(id >> 32)) != 0) {
	npw_printf("ERROR: 64-bit viewers in 32-bit wrappers are not supported\n");
	abort();
  }

  *((void **)p_value) = (void *)(uintptr_t)id;
  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NPRect objects
 */

static int do_send_NPRect(rpc_message_t *message, void *p_value)
{
  NPRect *rect = (NPRect *)p_value;
  int error;

  if ((error = rpc_message_send_uint32(message, rect->top)) < 0)
	return error;
  if ((error = rpc_message_send_uint32(message, rect->left)) < 0)
	return error;
  if ((error = rpc_message_send_uint32(message, rect->bottom)) < 0)
	return error;
  if ((error = rpc_message_send_uint32(message, rect->right)) < 0)
	return error;

  return RPC_ERROR_NO_ERROR;
}

static int do_recv_NPRect(rpc_message_t *message, void *p_value)
{
  NPRect *rect = (NPRect *)p_value;
  uint32_t top, left, bottom, right;
  int error;

  if ((error = rpc_message_recv_uint32(message, &top)) < 0)
	return error;
  if ((error = rpc_message_recv_uint32(message, &left)) < 0)
	return error;
  if ((error = rpc_message_recv_uint32(message, &bottom)) < 0)
	return error;
  if ((error = rpc_message_recv_uint32(message, &right)) < 0)
	return error;

  rect->top = top;
  rect->left = left;
  rect->bottom = bottom;
  rect->right = right;
  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NPWindow objects
 */

static int do_send_NPWindow(rpc_message_t *message, void *p_value)
{
  NPWindow *window = (NPWindow *)p_value;
  int error;

  if (window == NULL) {
	if ((error = rpc_message_send_uint32(message, 0)) < 0)
	  return error;
  }
  else {
	if ((error = rpc_message_send_uint32(message, (Window)window->window)) < 0)
	  return error;
	if ((error = rpc_message_send_int32(message, window->x)) < 0)
	  return error;
	if ((error = rpc_message_send_int32(message, window->y)) < 0)
	  return error;
	if ((error = rpc_message_send_uint32(message, window->width)) < 0)
	  return error;
	if ((error = rpc_message_send_uint32(message, window->height)) < 0)
	  return error;
	if ((error = do_send_NPRect(message, &window->clipRect)) < 0)
	  return error;
	if ((error = rpc_message_send_int32(message, window->type)) < 0)
	  return error;
  }

  return RPC_ERROR_NO_ERROR;
}

static int do_recv_NPWindow(rpc_message_t *message, void *p_value)
{
  NPWindow **window_p = (NPWindow **)p_value;
  NPWindow *window;
  uint32_t window_id;
  int32_t window_type;
  int error;

  if ((error = rpc_message_recv_uint32(message, &window_id)) < 0)
	return error;
  *window_p = NULL;
  if (window_id) {
	if ((window = *window_p = malloc(sizeof(NPWindow))) == NULL)
	  return RPC_ERROR_NO_MEMORY;
	if ((error = rpc_message_recv_int32(message, &window->x)) < 0)
	  return error;
	if ((error = rpc_message_recv_int32(message, &window->y)) < 0)
	  return error;
	if ((error = rpc_message_recv_uint32(message, &window->width)) < 0)
	  return error;
	if ((error = rpc_message_recv_uint32(message, &window->height)) < 0)
	  return error;
	if ((error = do_recv_NPRect(message, &window->clipRect)) < 0)
	  return error;
	if ((error = rpc_message_recv_int32(message, &window_type)) < 0)
	  return error;
	window->type = window_type;
	window->ws_info = NULL; // to be filled in by the plugin
	window->window = (void *)(Window)window_id;
  }

  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NPFullPrint objects
 */

static int do_send_NPFullPrint(rpc_message_t *message, void *p_value)
{
  NPFullPrint *fullPrint = (NPFullPrint *)p_value;
  int error;

  if ((error = rpc_message_send_uint32(message, fullPrint->pluginPrinted)) < 0)
	return error;
  if ((error = rpc_message_send_uint32(message, fullPrint->printOne)) < 0)
	return error;

  return RPC_ERROR_NO_ERROR;
}

static int do_recv_NPFullPrint(rpc_message_t *message, void *p_value)
{
  NPFullPrint *fullPrint = (NPFullPrint *)p_value;
  uint32_t pluginPrinted, printOne;
  int error;

  if ((error = rpc_message_recv_uint32(message, &pluginPrinted)) < 0)
	return error;
  if ((error = rpc_message_recv_uint32(message, &printOne)) < 0)
	return error;

  fullPrint->pluginPrinted = pluginPrinted;
  fullPrint->printOne = printOne;
  fullPrint->platformPrint = NULL; // to be filled in by the plugin
  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NPEmbedPrint objects
 */

static int do_send_NPEmbedPrint(rpc_message_t *message, void *p_value)
{
  NPEmbedPrint *embedPrint = (NPEmbedPrint *)p_value;
  int error;

  if ((error = do_send_NPWindow(message, &embedPrint->window)) < 0)
	return error;

  return RPC_ERROR_NO_ERROR;
}

static int do_recv_NPEmbedPrint(rpc_message_t *message, void *p_value)
{
  NPEmbedPrint *embedPrint = (NPEmbedPrint *)p_value;
  int error;

  if ((error = do_recv_NPWindow(message, &embedPrint->window)) < 0)
	return error;

  embedPrint->platformPrint = NULL; // to be filled in by the plugin
  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NPPrint objects
 */

static int do_send_NPPrint(rpc_message_t *message, void *p_value)
{
  NPPrint *printInfo = (NPPrint *)p_value;
  int error;

  if ((error = rpc_message_send_uint32(message, printInfo->mode)) < 0)
	return error;
  switch (printInfo->mode) {
  case NP_FULL:
	if ((error = do_send_NPFullPrint(message, &printInfo->print.fullPrint)) < 0)
	  return error;
	break;
  case NP_EMBED:
	if ((error = do_send_NPEmbedPrint(message, &printInfo->print.embedPrint)) < 0)
	  return error;
	break;
  default:
	return RPC_ERROR_GENERIC;
  }

  return RPC_ERROR_NO_ERROR;
}

static int do_recv_NPPrint(rpc_message_t *message, void *p_value)
{
  NPPrint *printInfo = (NPPrint *)p_value;
  uint32_t print_mode;
  int error;

  if ((error = rpc_message_recv_uint32(message, &print_mode)) < 0)
	return error;
  switch (print_mode) {
  case NP_FULL:
	if ((error = do_recv_NPFullPrint(message, &printInfo->print.fullPrint)) < 0)
	  return error;
	break;
  case NP_EMBED:
	if ((error = do_recv_NPEmbedPrint(message, &printInfo->print.embedPrint)) < 0)
	  return error;
	break;
  default:
	return RPC_ERROR_GENERIC;
  }

  printInfo->mode = print_mode;
  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NPPrintData objects
 */

static int do_send_NPPrintData(rpc_message_t *message, void *p_value)
{
  NPPrintData *printData = (NPPrintData *)p_value;
  int error;

  if ((error = rpc_message_send_uint32(message, printData->size)) < 0)
	return error;
  if ((error = rpc_message_send_bytes(message, printData->data, printData->size)) < 0)
	return error;

  return RPC_ERROR_NO_ERROR;
}

static int do_recv_NPPrintData(rpc_message_t *message, void *p_value)
{
  NPPrintData *printData = (NPPrintData *)p_value;
  int error;

  if ((error = rpc_message_recv_uint32(message, &printData->size)) < 0)
	return error;
  if ((error = rpc_message_recv_bytes(message, printData->data, printData->size)) < 0)
	return error;

  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NPObject objects
 */

// XXX propagate reference counters?
static int do_send_NPObject(rpc_message_t *message, void *p_value)
{
  uint32_t npobj_id = 0;
  NPObject *npobj = (NPObject *)p_value;
  if (npobj) {
	NPObjectInfo *npobj_info = npobject_info_lookup(npobj);
	if (npobj_info)
	  npobj_id = npobj_info->npobj_id;
#ifdef BUILD_WRAPPER
	else {
	  // create a new mapping (browser-side)
	  if ((npobj_info = npobject_info_new(npobj)) == NULL)
		return RPC_ERROR_NO_MEMORY;
	  npobj_id = npobj_info->npobj_id;
	  npobject_associate(npobj, npobj_info);
	}
#endif
	assert(npobj_id != 0);
  }
  return rpc_message_send_uint32(message, npobj_id);
}

static int do_recv_NPObject(rpc_message_t *message, void *p_value)
{
  int error;
  uint32_t npobj_id;

  if ((error = rpc_message_recv_uint32(message, &npobj_id)) < 0)
	return error;

  NPObject *npobj = NULL;
  if (npobj_id) {
	npobj = npobject_lookup(npobj_id);
#ifdef BUILD_VIEWER
	// create a new mapping (plugin-side)
	if (npobj == NULL) {
	  if ((npobj = npobject_new(npobj_id, NULL, NULL)) == NULL)
		return RPC_ERROR_NO_MEMORY;
	}
#endif
	assert(npobj != NULL);
  }
  *((NPObject **)p_value) = npobj;
  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NPIdentifier objects
 */

// Rationale: NPIdentifiers are allocated on the browser side
// only. IDs are passed through to the viewer, and they have no
// meaning on that side as they are only used to get passed back to
// the browser side
static int do_send_NPIdentifier(rpc_message_t *message, void *p_value)
{
  NPIdentifier ident = (NPIdentifier)p_value;
  int id = 0;
  if (ident) {
#ifdef BUILD_WRAPPER
	id = id_lookup_value(ident);
	if (id < 0)
	  id = id_create(ident);
#endif
#ifdef BUILD_VIEWER
	id = (uintptr_t)ident;
#endif
	assert(id != 0);
  }
  return rpc_message_send_uint32(message, id);
}

static int do_recv_NPIdentifier(rpc_message_t *message, void *p_value)
{
  int error;
  uint32_t id;

  if ((error = rpc_message_recv_uint32(message, &id)) < 0)
	return error;

  NPIdentifier ident = NULL;
  if (id) {
#ifdef BUILD_WRAPPER
	ident = id_lookup(id);
#endif
#ifdef BUILD_VIEWER
	ident = (void *)(uintptr_t)id;
#endif
	assert(ident != NULL);
  }
  *((NPIdentifier *)p_value) = ident;
  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NPString objects
 */

static int do_send_NPString(rpc_message_t *message, void *p_value)
{
  NPString *string = (NPString *)p_value;
  if (string == NULL)
	return RPC_ERROR_MESSAGE_ARGUMENT_INVALID;

  int error = rpc_message_send_uint32(message, string->utf8length);
  if (error < 0)
	return error;
  if (string->utf8length && string->utf8characters)
	return rpc_message_send_bytes(message, (unsigned char *)string->utf8characters, string->utf8length);
  return RPC_ERROR_NO_ERROR;
}

static int do_recv_NPString(rpc_message_t *message, void *p_value)
{
  NPString *string = (NPString *)p_value;
  if (string == NULL)
	return RPC_ERROR_MESSAGE_ARGUMENT_INVALID;
  string->utf8length = 0;
  string->utf8characters = NULL;

  int error = rpc_message_recv_uint32(message, &string->utf8length);
  if (error < 0)
	return error;

  if (string->utf8length > 0) {
	// calloc() will make the string nul-terminated, even if utf8characters is a const NPUTF8 *
	if ((string->utf8characters = calloc(1, string->utf8length + 1)) == NULL)
	  return RPC_ERROR_NO_MEMORY;
	if ((error = rpc_message_recv_bytes(message, (unsigned char *)string->utf8characters, string->utf8length)) < 0)
	  return error;
  }
  
  return RPC_ERROR_NO_ERROR;
}


/*
 *  Process NPVariant objects
 */

static int do_send_NPVariant(rpc_message_t *message, void *p_value)
{
  NPVariant *variant = (NPVariant *)p_value;
  if (variant == NULL)
	return RPC_ERROR_MESSAGE_ARGUMENT_INVALID;

  int error = rpc_message_send_uint32(message, variant->type);
  if (error < 0)
	return error;

  switch (variant->type) {
  case NPVariantType_Void:
  case NPVariantType_Null:
	// nothing to do (initialized in receiver)
	break;
  case NPVariantType_Bool:
	if ((error = rpc_message_send_uint32(message, variant->value.boolValue)) < 0)
	  return error;
	break;
  case NPVariantType_Int32:
	if ((error = rpc_message_send_uint32(message, variant->value.intValue)) < 0)
	  return error;
	break;
  case NPVariantType_Double:
	if ((error = rpc_message_send_double(message, variant->value.doubleValue)) < 0)
	  return error;
	break;
  case NPVariantType_String:
	if ((error = do_send_NPString(message, &variant->value.stringValue)) < 0)
	  return error;
	break;
  case NPVariantType_Object:
	if ((error = do_send_NPObject(message, variant->value.objectValue)) < 0)
	  return error;
	break;
  }

  return RPC_ERROR_NO_ERROR;
}

static int do_recv_NPVariant(rpc_message_t *message, void *p_value)
{
  NPVariant *variant = (NPVariant *)p_value;
  if (variant)
	VOID_TO_NPVARIANT(*variant);

  uint32_t type;
  int error = rpc_message_recv_uint32(message, &type);
  if (error < 0)
	return error;

  NPVariant result;
  VOID_TO_NPVARIANT(result);
  switch (type) {
  case NPVariantType_Void:
	VOID_TO_NPVARIANT(result);
	break;
  case NPVariantType_Null:
	NULL_TO_NPVARIANT(result);
	break;
  case NPVariantType_Bool: {
	uint32_t value;
	if ((error = rpc_message_recv_uint32(message, &value)) < 0)
	  return error;
	result.value.boolValue = value;
	break;
  }
  case NPVariantType_Int32:
	if ((error = rpc_message_recv_uint32(message, &result.value.intValue)) < 0)
	  return error;
	break;
  case NPVariantType_Double:
	if ((error = rpc_message_recv_double(message, &result.value.doubleValue)) < 0)
	  return error;
	break;
  case NPVariantType_String:
	if ((error = do_recv_NPString(message, &result.value.stringValue)) < 0)
	  return error;
	break;
  case NPVariantType_Object:
	if ((error = do_recv_NPObject(message, &result.value.objectValue)) < 0)
	  return error;
	break;
  }

  if (variant) {
	*variant = result;
	variant->type = type;
  }
  
  return RPC_ERROR_NO_ERROR;
}


/*
 *  Initialize marshalers for NPAPI types
 */

static const rpc_message_descriptor_t message_descs[] = {
  {
	RPC_TYPE_NPP,
	sizeof(NPP),
	do_send_NPP,
	do_recv_NPP
  },
  {
	RPC_TYPE_NP_STREAM,
	sizeof(NPStream *),
	do_send_NPStream,
	do_recv_NPStream
  },
  {
	RPC_TYPE_NP_BYTE_RANGE,
	sizeof(NPByteRange *),
	do_send_NPByteRange,
	do_recv_NPByteRange
  },
  {
	RPC_TYPE_NP_SAVED_DATA,
	sizeof(NPSavedData *),
	do_send_NPSavedData,
	do_recv_NPSavedData
  },
  {
	RPC_TYPE_NP_NOTIFY_DATA,
	sizeof(void *),
	do_send_NotifyData,
	do_recv_NotifyData
  },
  {
	RPC_TYPE_NP_RECT,
	sizeof(NPRect),
	do_send_NPRect,
	do_recv_NPRect
  },
  {
	RPC_TYPE_NP_WINDOW,
	sizeof(NPWindow *),
	do_send_NPWindow,
	do_recv_NPWindow
  },
  {
	RPC_TYPE_NP_PRINT,
	sizeof(NPPrint),
	do_send_NPPrint,
	do_recv_NPPrint
  },
  {
	RPC_TYPE_NP_FULL_PRINT,
	sizeof(NPFullPrint),
	do_send_NPFullPrint,
	do_recv_NPFullPrint
  },
  {
	RPC_TYPE_NP_EMBED_PRINT,
	sizeof(NPEmbedPrint),
	do_send_NPEmbedPrint,
	do_recv_NPEmbedPrint
  },
  {
	RPC_TYPE_NP_PRINT_DATA,
	sizeof(NPPrintData),
	do_send_NPPrintData,
	do_recv_NPPrintData
  },
  {
	RPC_TYPE_NP_OBJECT,
	sizeof(NPObject *),
	do_send_NPObject,
	do_recv_NPObject
  },
  {
	RPC_TYPE_NP_IDENTIFIER,
	sizeof(NPIdentifier),
	do_send_NPIdentifier,
	do_recv_NPIdentifier
  },
  {
	RPC_TYPE_NP_STRING,
	sizeof(NPString),
	do_send_NPString,
	do_recv_NPString
  },
  {
	RPC_TYPE_NP_VARIANT,
	sizeof(NPVariant),
	do_send_NPVariant,
	do_recv_NPVariant
  }
};

int rpc_add_np_marshalers(void)
{
  return rpc_message_add_callbacks(message_descs, sizeof(message_descs) / sizeof(message_descs[0]));
}
