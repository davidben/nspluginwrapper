/*
 *  npruntime.c - Scripting plugins support
 *
 *  nspluginwrapper (C) 2005-2008 Gwenole Beauchesne
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
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "sysdeps.h"

#include <assert.h>
#include <glib.h> /* <glib/ghash.h> */
#include "utils.h"
#include "rpc.h"
#include "npw-rpc.h"

#define XP_UNIX 1
#define MOZ_X11 1
#include <npapi.h>
#include <npupp.h>
#include <npruntime.h>
#include "npruntime-impl.h"

#define DEBUG 1
#include "debug.h"


// Defined in npw-{wrapper,viewer}.c
extern rpc_connection_t *g_rpc_connection attribute_hidden;


/* ====================================================================== */
/* === NPClass Bridge                                                 === */
/* ====================================================================== */

NPClass npclass_bridge = {
  NP_CLASS_STRUCT_VERSION,
  NULL,
  NULL,
  npclass_invoke_Invalidate,
  npclass_invoke_HasMethod,
  npclass_invoke_Invoke,
  npclass_invoke_InvokeDefault,
  npclass_invoke_HasProperty,
  npclass_invoke_GetProperty,
  npclass_invoke_SetProperty,
  npclass_invoke_RemoveProperty
};

// NPClass::Invalidate
int npclass_handle_Invalidate(rpc_connection_t *connection)
{
  NPObject *npobj;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::Invalidate() get args", error);
	return error;
  }

  if (npobj && npobj->_class && npobj->_class->invalidate) {
	D(bug("NPClass::Invalidate(npobj %p)\n", npobj));
	npobj->_class->invalidate(npobj);
	D(bug(" done\n"));
  }

  return RPC_ERROR_NO_ERROR;
}

void npclass_invoke_Invalidate(NPObject *npobj)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPCLASS_INVALIDATE,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::Invalidate() invoke", error);
	return;
  }

  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::Invalidate() wait for reply", error);
	return;
  }
}

// NPClass::HasMethod
int npclass_handle_HasMethod(rpc_connection_t *connection)
{
  NPObject *npobj;
  NPIdentifier name;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_NP_IDENTIFIER, &name,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::HasMethod() get args", error);
	return error;
  }

  uint32_t ret = 0;
  if (npobj && npobj->_class && npobj->_class->hasMethod) {
	D(bug("NPClass::HasMethod(npobj %p, name id %p)\n", npobj, name));
	ret = npobj->_class->hasMethod(npobj, name);
	D(bug(" return: %d\n", ret));
  }

  return rpc_method_send_reply(connection, RPC_TYPE_UINT32, ret, RPC_TYPE_INVALID);
}

bool npclass_invoke_HasMethod(NPObject *npobj, NPIdentifier name)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPCLASS_HAS_METHOD,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_NP_IDENTIFIER, name,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::HasMethod() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_UINT32, &ret, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::HasMethod() wait for reply", error);
	return false;
  }

  return ret;
}

// NPClass::Invoke
int npclass_handle_Invoke(rpc_connection_t *connection)
{
  NPObject *npobj;
  NPIdentifier name;
  uint32_t argCount;
  NPVariant *args;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_NP_IDENTIFIER, &name,
								  RPC_TYPE_ARRAY, RPC_TYPE_NP_VARIANT, &argCount, &args,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::Invoke() get args", error);
	return error;
  }

  uint32_t ret = 0;
  NPVariant result;
  VOID_TO_NPVARIANT(result);
  if (npobj && npobj->_class && npobj->_class->invoke) {
	D(bug("NPClass::Invoke(npobj %p, name id %p)\n", npobj, name));
	ret = npobj->_class->invoke(npobj, name, args, argCount, &result);
	D(bug(" return: %d\n", ret));
  }

  free(args);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_UINT32, ret,
							   RPC_TYPE_NP_VARIANT, &result,
							   RPC_TYPE_INVALID);
}

bool npclass_invoke_Invoke(NPObject *npobj, NPIdentifier name, const NPVariant *args, uint32_t argCount,
						   NPVariant *result)
{
  if (result == NULL)
	return false;
  VOID_TO_NPVARIANT(*result);

  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPCLASS_INVOKE,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_NP_IDENTIFIER, name,
								RPC_TYPE_ARRAY, RPC_TYPE_NP_VARIANT, argCount, args,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::Invoke() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &ret,
									RPC_TYPE_NP_VARIANT, result,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::Invoke() wait for reply", error);
	return false;
  }

  return ret;
}

// NPClass::InvokeDefault
int npclass_handle_InvokeDefault(rpc_connection_t *connection)
{
  NPObject *npobj;
  uint32_t argCount;
  NPVariant *args;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_ARRAY, RPC_TYPE_NP_VARIANT, &argCount, &args,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::InvokeDefault() get args", error);
	return error;
  }

  uint32_t ret = 0;
  NPVariant result;
  VOID_TO_NPVARIANT(result);
  if (npobj && npobj->_class && npobj->_class->invokeDefault) {
	D(bug("NPClass::InvokeDefault(npobj %p)\n", npobj));
	ret = npobj->_class->invokeDefault(npobj, args, argCount, &result);
	D(bug(" return: %d\n", ret));
  }

  free(args);

  return rpc_method_send_reply(connection,
							   RPC_TYPE_UINT32, ret,
							   RPC_TYPE_NP_VARIANT, &result,
							   RPC_TYPE_INVALID);
}

bool npclass_invoke_InvokeDefault(NPObject *npobj, const NPVariant *args, uint32_t argCount,
						   NPVariant *result)
{
  if (result == NULL)
	return false;
  VOID_TO_NPVARIANT(*result);

  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPCLASS_INVOKE_DEFAULT,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_ARRAY, RPC_TYPE_NP_VARIANT, argCount, args,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::InvokeDefault() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &ret,
									RPC_TYPE_NP_VARIANT, result,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::InvokeDefault() wait for reply", error);
	return false;
  }

  return ret;
}

// NPClass::HasProperty
int npclass_handle_HasProperty(rpc_connection_t *connection)
{
  NPObject *npobj;
  NPIdentifier name;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_NP_IDENTIFIER, &name,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::HasProperty() get args", error);
	return error;
  }

  uint32_t ret = 0;
  if (npobj && npobj->_class && npobj->_class->hasProperty) {
	D(bug("NPClass::HasProperty(npobj %p, name id %p)\n", npobj, name));
	ret = npobj->_class->hasProperty(npobj, name);
	D(bug(" return: %d\n", ret));
  }

  return rpc_method_send_reply(connection, RPC_TYPE_UINT32, ret, RPC_TYPE_INVALID);
}

bool npclass_invoke_HasProperty(NPObject *npobj, NPIdentifier name)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPCLASS_HAS_PROPERTY,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_NP_IDENTIFIER, name,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::HasProperty() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_UINT32, &ret, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::HasProperty() wait for reply", error);
	return false;
  }

  return ret;
}

// NPClass::GetProperty
int npclass_handle_GetProperty(rpc_connection_t *connection)
{
  NPObject *npobj;
  NPIdentifier name;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_NP_IDENTIFIER, &name,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::GetProperty() get args", error);
	return error;
  }

  uint32_t ret = 0;
  NPVariant result;
  VOID_TO_NPVARIANT(result);
  if (npobj && npobj->_class && npobj->_class->getProperty) {
	D(bug("NPClass::GetProperty(npobj %p, name id %p)\n", npobj, name));
	ret = npobj->_class->getProperty(npobj, name, &result);
	D(bug(" return: %d\n", ret));
  }

  return rpc_method_send_reply(connection,
							   RPC_TYPE_UINT32, ret,
							   RPC_TYPE_NP_VARIANT, &result,
							   RPC_TYPE_INVALID);
}

bool npclass_invoke_GetProperty(NPObject *npobj, NPIdentifier name, NPVariant *result)
{
  if (result == NULL)
	return false;
  VOID_TO_NPVARIANT(*result);

  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPCLASS_GET_PROPERTY,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_NP_IDENTIFIER, name,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::GetProperty() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &ret,
									RPC_TYPE_NP_VARIANT, result,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::GetProperty() wait for reply", error);
	return false;
  }

  return ret;
}

// NPClass::SetProperty
int npclass_handle_SetProperty(rpc_connection_t *connection)
{
  NPObject *npobj;
  NPIdentifier name;
  NPVariant value;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_NP_IDENTIFIER, &name,
								  RPC_TYPE_NP_VARIANT, &value,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::SetProperty() get args", error);
	return error;
  }

  uint32_t ret = 0;
  if (npobj && npobj->_class && npobj->_class->setProperty) {
	D(bug("NPClass::SetProperty(npobj %p, name id %p)\n", npobj, name));
	ret = npobj->_class->setProperty(npobj, name, &value);
	D(bug(" return: %d\n", ret));
  }

  return rpc_method_send_reply(connection,
							   RPC_TYPE_UINT32, ret,
							   RPC_TYPE_INVALID);
}

bool npclass_invoke_SetProperty(NPObject *npobj, NPIdentifier name, const NPVariant *value)
{
  if (value == NULL)
	return false;

  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPCLASS_SET_PROPERTY,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_NP_IDENTIFIER, name,
								RPC_TYPE_NP_VARIANT, value,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::SetProperty() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection,
									RPC_TYPE_UINT32, &ret,
									RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::SetProperty() wait for reply", error);
	return false;
  }

  return ret;
}

// NPClass::RemoveProperty
int npclass_handle_RemoveProperty(rpc_connection_t *connection)
{
  NPObject *npobj;
  NPIdentifier name;
  int error = rpc_method_get_args(connection,
								  RPC_TYPE_NP_OBJECT, &npobj,
								  RPC_TYPE_NP_IDENTIFIER, &name,
								  RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::RemoveProperty() get args", error);
	return error;
  }

  uint32_t ret = 0;
  if (npobj && npobj->_class && npobj->_class->removeProperty) {
	D(bug("NPClass::RemoveProperty(npobj %p, name id %p)\n", npobj, name));
	ret = npobj->_class->removeProperty(npobj, name);
	D(bug(" return: %d\n", ret));
  }

  return rpc_method_send_reply(connection, RPC_TYPE_UINT32, ret, RPC_TYPE_INVALID);
}

bool npclass_invoke_RemoveProperty(NPObject *npobj, NPIdentifier name)
{
  int error = rpc_method_invoke(g_rpc_connection,
								RPC_METHOD_NPCLASS_REMOVE_PROPERTY,
								RPC_TYPE_NP_OBJECT, npobj,
								RPC_TYPE_NP_IDENTIFIER, name,
								RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::RemoveProperty() invoke", error);
	return false;
  }

  uint32_t ret;
  error = rpc_method_wait_for_reply(g_rpc_connection, RPC_TYPE_UINT32, &ret, RPC_TYPE_INVALID);

  if (error != RPC_ERROR_NO_ERROR) {
	npw_perror("NPClass::RemoveProperty() wait for reply", error);
	return false;
  }

  return ret;
}


/* ====================================================================== */
/* === NPObjectInfo                                                   === */
/* ====================================================================== */

NPObjectInfo *npobject_info_new(NPObject *npobj)
{
  NPObjectInfo *npobj_info = malloc(sizeof(*npobj_info));
  if (npobj_info) {
	static uint32_t id;
	npobj_info->npobj = npobj;
	npobj_info->npobj_id = ++id;
  }
  return npobj_info;
}

void npobject_info_destroy(NPObjectInfo *npobj_info)
{
  if (npobj_info)
	free(npobj_info);
}


/* ====================================================================== */
/* === NPObject                                                       === */
/* ====================================================================== */

static void npobject_hash_table_insert(NPObject *npobj, NPObjectInfo *npobj_info);
static bool npobject_hash_table_remove(NPObject *npobj);

static NPObject *_npobject_new(NPP instance, NPClass *class)
{
  NPObject *npobj;
  if (class && class->allocate)
	npobj = class->allocate(instance, class);
  else
	npobj = malloc(sizeof(*npobj));
  if (npobj) {
	npobj->_class = class ? class : &npclass_bridge;
	npobj->referenceCount = 1;
  }
  return npobj;
}

static void _npobject_destroy(NPObject *npobj)
{
  if (npobj) {
	if (npobj->_class && npobj->_class->deallocate)
	  npobj->_class->deallocate(npobj);
	else
	  free(npobj);
  }
}

NPObject *npobject_new(uint32_t npobj_id, NPP instance, NPClass *class)
{
  NPObject *npobj = _npobject_new(instance, class);
  if (npobj == NULL)
	return NULL;

  NPObjectInfo *npobj_info = npobject_info_new(npobj);
  if (npobj_info == NULL) {
	_npobject_destroy(npobj);
	return NULL;
  }
  npobj_info->npobj_id = npobj_id;
  npobject_associate(npobj, npobj_info);
  return npobj;
}

void npobject_destroy(NPObject *npobj)
{
  if (npobj)
	npobject_hash_table_remove(npobj);

  _npobject_destroy(npobj);
}

void npobject_associate(NPObject *npobj, NPObjectInfo *npobj_info)
{
  assert(npobj && npobj_info && npobj_info > 0);
  npobject_hash_table_insert(npobj, npobj_info);
}


/* ====================================================================== */
/* === NPObject Repository                                            === */
/* ====================================================================== */

// NOTE: those hashes must be maintained in a whole, not separately
static GHashTable *g_npobjects = NULL;			// (NPObject *)  -> (NPObjectInfo *)
static GHashTable *g_npobject_ids = NULL;		// (NPObject ID) -> (NPObject *)

bool npobject_bridge_new(void)
{
  if ((g_npobjects = g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)npobject_info_destroy)) == NULL)
	return false;
  if ((g_npobject_ids = g_hash_table_new(NULL, NULL)) == NULL)
	return false;
  return true;
}

void npobject_bridge_destroy(void)
{
  if (g_npobject_ids)
	g_hash_table_destroy(g_npobject_ids);
  if (g_npobjects)
	g_hash_table_destroy(g_npobjects);
}

void npobject_hash_table_insert(NPObject *npobj, NPObjectInfo *npobj_info)
{
  g_hash_table_insert(g_npobjects, npobj, npobj_info);
  g_hash_table_insert(g_npobject_ids, (void *)(uintptr_t)npobj_info->npobj_id, npobj);
}

bool npobject_hash_table_remove(NPObject *npobj)
{
  NPObjectInfo *npobj_info = npobject_info_lookup(npobj);
  assert(npobj_info != NULL);
  bool removed_all = true;
  if (!g_hash_table_remove(g_npobject_ids, (void *)(uintptr_t)npobj_info->npobj_id))
	removed_all = false;
  if (!g_hash_table_remove(g_npobjects, npobj))
	removed_all = false;
  return removed_all;
}

NPObjectInfo *npobject_info_lookup(NPObject *npobj)
{
  return g_hash_table_lookup(g_npobjects, npobj);
}

NPObject *npobject_lookup(uint32_t npobj_id)
{
  return g_hash_table_lookup(g_npobject_ids, (void *)(uintptr_t)npobj_id);
}
