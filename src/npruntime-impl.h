/*
 *  npruntime.c - Scripting plugins support
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

#ifndef NPRUNTIME_IMPL_H
#define NPRUNTIME_IMPL_H

// NPObjectInfo is used to hold additional information for an NPObject instance
typedef struct {
  NPObject *npobj;
  uint32_t npobj_id;
} NPObjectInfo;

extern NPObjectInfo *npobject_info_new(NPObject *npobj) attribute_hidden;
extern void npobject_info_destroy(NPObjectInfo *npobj_info) attribute_hidden;
extern NPObjectInfo *npobject_info_lookup(NPObject *npobj) attribute_hidden;

extern NPObject *npobject_new(uint32_t npobj_id, NPP instance, NPClass *class) attribute_hidden;
extern void npobject_destroy(NPObject *npobj) attribute_hidden;
extern NPObject *npobject_lookup(uint32_t npobj_id) attribute_hidden;
extern void npobject_associate(NPObject *npobj, NPObjectInfo *npobj_info) attribute_hidden;

extern bool npobject_bridge_new(void) attribute_hidden;
extern void npobject_bridge_destroy(void) attribute_hidden;

extern NPClass npclass_bridge;

extern void npclass_invoke_Invalidate(NPObject *npobj) attribute_hidden;
extern bool npclass_invoke_HasMethod(NPObject *npobj, NPIdentifier name) attribute_hidden;
extern bool npclass_invoke_Invoke(NPObject *npobj, NPIdentifier name, const NPVariant *args, uint32_t argCount, NPVariant *result) attribute_hidden;
extern bool npclass_invoke_InvokeDefault(NPObject *npobj, const NPVariant *args, uint32_t argCount, NPVariant *result) attribute_hidden;
extern bool npclass_invoke_HasProperty(NPObject *npobj, NPIdentifier name) attribute_hidden;
extern bool npclass_invoke_GetProperty(NPObject *npobj, NPIdentifier name, NPVariant *result) attribute_hidden;
extern bool npclass_invoke_SetProperty(NPObject *npobj, NPIdentifier name, const NPVariant *value) attribute_hidden;
extern bool npclass_invoke_RemoveProperty(NPObject *npobj, NPIdentifier name) attribute_hidden;

extern int npclass_handle_Invalidate(rpc_connection_t *connection) attribute_hidden;
extern int npclass_handle_HasMethod(rpc_connection_t *connection) attribute_hidden;
extern int npclass_handle_Invoke(rpc_connection_t *connection) attribute_hidden;
extern int npclass_handle_InvokeDefault(rpc_connection_t *connection) attribute_hidden;
extern int npclass_handle_HasProperty(rpc_connection_t *connection) attribute_hidden;
extern int npclass_handle_GetProperty(rpc_connection_t *connection) attribute_hidden;
extern int npclass_handle_SetProperty(rpc_connection_t *connection) attribute_hidden;
extern int npclass_handle_RemoveProperty(rpc_connection_t *connection) attribute_hidden;
extern int npclass_handle_Invalidate(rpc_connection_t *connection) attribute_hidden;

struct _NPNetscapeFuncs;
extern void npruntime_init_callbacks(struct _NPNetscapeFuncs *mozilla_funcs);

#endif /* NPRUNTIME_IMPL_H */
