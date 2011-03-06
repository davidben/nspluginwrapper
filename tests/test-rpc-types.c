/*
 *  test-rpc-types.c - Test marshaling of common data types
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
#include "test-rpc-common.h"
#include "utils.h"
#include <stdarg.h>

#define DEBUG 1
#include "debug.h"

#define RPC_TEST_MAX_ARGS 32

enum
  {
	RPC_TEST_METHOD_VOID__VOID = 1,
	RPC_TEST_METHOD_VOID__CHAR,
	RPC_TEST_METHOD_VOID__CHARx10,
	RPC_TEST_METHOD_VOID__BOOL, 
	RPC_TEST_METHOD_VOID__BOOLx10,
	RPC_TEST_METHOD_VOID__INT32x10,
	RPC_TEST_METHOD_VOID__UINT32x10,
	RPC_TEST_METHOD_VOID__UINT64x10,
	RPC_TEST_METHOD_VOID__DOUBLEx5,
	RPC_TEST_METHOD_VOID__STRINGx3,
 };

const gchar *
rpc_test_get_connection_path (void)
{
  return NPW_CONNECTION_PATH "/Test.RPC.Types";
}

static void
rpc_test_signature (gboolean is_invoke, ...)
{
  va_list  args;
  gint     type, n_args;
  gboolean was_array;
  GString *str;

  if ((str = g_string_new (NULL)) == NULL)
	return;
  n_args = 0;
  was_array = FALSE;
  va_start (args, is_invoke);
  while ((type = va_arg (args, gint)) != RPC_TYPE_INVALID)
	{
	  if (++n_args > 1 && !was_array)
		g_string_append (str, ", ");
	  was_array = FALSE;
	  switch (type)
		{
		case RPC_TYPE_CHAR:
		  g_string_append (str, "char");
		  if (is_invoke)
			va_arg (args, int);
		  break;
		case RPC_TYPE_BOOLEAN:
		  g_string_append (str, "bool");
		  if (is_invoke)
			va_arg (args, int);
		  break;
		case RPC_TYPE_INT32:
		  g_string_append (str, "int32");
		  if (is_invoke)
			va_arg (args, gint32);
		  break;
		case RPC_TYPE_UINT32:
		  g_string_append (str, "uint32");
		  if (is_invoke)
			va_arg (args, guint32);
		  break;
		case RPC_TYPE_UINT64:
		  g_string_append (str, "uint64");
		  if (is_invoke)
			va_arg (args, guint64);
		  break;
		case RPC_TYPE_DOUBLE:
		  g_string_append (str, "double");
		  if (is_invoke)
			va_arg (args, gdouble);
		  break;
		case RPC_TYPE_STRING:
		  g_string_append (str, "string");
		  if (is_invoke)
			va_arg (args, gchar *);
		  break;
		case RPC_TYPE_ARRAY:
		  g_string_append (str, "array of");
		  was_array = TRUE;
		  break;
		}
	  if (!is_invoke && type != RPC_TYPE_ARRAY)
		va_arg (args, gpointer);
	}
  va_end (args);
  if (n_args == 0)
	g_string_append (str, "void");
  g_print ("void f (%s)\n", str->str);
  g_string_free (str, TRUE);
}

#define rpc_test_invoke(method, ...) do {							\
  int error;														\
  rpc_connection_t *connection;										\
  connection = rpc_test_get_connection ();							\
  rpc_test_signature (TRUE, __VA_ARGS__);							\
  error = rpc_method_invoke (connection, method, __VA_ARGS__);		\
  g_assert (error == RPC_ERROR_NO_ERROR);							\
  error = rpc_method_wait_for_reply (connection, RPC_TYPE_INVALID); \
  g_assert (error == RPC_ERROR_NO_ERROR);							\
} while (0)

#define rpc_test_get_args(connection, ...) do {						\
  int error;														\
  rpc_test_signature (FALSE, __VA_ARGS__);							\
  error = rpc_method_get_args (connection, __VA_ARGS__);			\
  g_assert (error == RPC_ERROR_NO_ERROR);							\
} while (0)

#ifdef BUILD_SERVER
typedef union _RPCTestArg RPCTestArg;

union _RPCTestArg
{
  gchar   c;
  guint   b;
  gint32  i;
  guint32 u;
  guint64 j;
  gdouble d;
  gchar  *s;
};

static RPCTestArg g_args[RPC_TEST_MAX_ARGS];

static int
handle_VOID__VOID (rpc_connection_t *connection)
{
  rpc_test_get_args (connection,
					 RPC_TYPE_INVALID);

  return rpc_method_send_reply (connection, RPC_TYPE_INVALID);
}

static int
handle_VOID__CHAR (rpc_connection_t *connection)
{
  rpc_test_get_args (connection,
					 RPC_TYPE_CHAR, &g_args[0].c,
					 RPC_TYPE_INVALID);

  g_assert (g_args[0].c == 'a');

  return rpc_method_send_reply (connection, RPC_TYPE_INVALID);
}

static int
handle_VOID__CHARx10 (rpc_connection_t *connection)
{
  rpc_test_get_args (connection,
					 RPC_TYPE_CHAR, &g_args[0].c,
					 RPC_TYPE_CHAR, &g_args[1].c,
					 RPC_TYPE_CHAR, &g_args[2].c,
					 RPC_TYPE_CHAR, &g_args[3].c,
					 RPC_TYPE_CHAR, &g_args[4].c,
					 RPC_TYPE_CHAR, &g_args[5].c,
					 RPC_TYPE_CHAR, &g_args[6].c,
					 RPC_TYPE_CHAR, &g_args[7].c,
					 RPC_TYPE_CHAR, &g_args[8].c,
					 RPC_TYPE_CHAR, &g_args[9].c,
					 RPC_TYPE_INVALID);

  g_assert (g_args[0].c == 'a');
  g_assert (g_args[1].c == 'b');
  g_assert (g_args[2].c == 'c');
  g_assert (g_args[3].c == 'd');
  g_assert (g_args[4].c == 'e');
  g_assert (g_args[5].c == '1');
  g_assert (g_args[6].c == '2');
  g_assert (g_args[7].c == '3');
  g_assert (g_args[8].c == '4');
  g_assert (g_args[9].c == '5');

  return rpc_method_send_reply (connection, RPC_TYPE_INVALID);
}

static int
handle_VOID__BOOL (rpc_connection_t *connection)
{
  rpc_test_get_args (connection,
					 RPC_TYPE_BOOLEAN, &g_args[0].b,
					 RPC_TYPE_INVALID);

  g_assert (g_args[0].b == TRUE);

  return rpc_method_send_reply (connection, RPC_TYPE_INVALID);
}

static int
handle_VOID__BOOLx10 (rpc_connection_t *connection)
{
  rpc_test_get_args (connection,
					 RPC_TYPE_BOOLEAN, &g_args[0].b,
					 RPC_TYPE_BOOLEAN, &g_args[1].b,
					 RPC_TYPE_BOOLEAN, &g_args[2].b,
					 RPC_TYPE_BOOLEAN, &g_args[3].b,
					 RPC_TYPE_BOOLEAN, &g_args[4].b,
					 RPC_TYPE_BOOLEAN, &g_args[5].b,
					 RPC_TYPE_BOOLEAN, &g_args[6].b,
					 RPC_TYPE_BOOLEAN, &g_args[7].b,
					 RPC_TYPE_BOOLEAN, &g_args[8].b,
					 RPC_TYPE_BOOLEAN, &g_args[9].b,
					 RPC_TYPE_INVALID);

  g_assert (g_args[0].b == TRUE);
  g_assert (g_args[1].b == FALSE);
  g_assert (g_args[2].b == TRUE);
  g_assert (g_args[3].b == FALSE);
  g_assert (g_args[4].b == TRUE);
  g_assert (g_args[5].b == FALSE);
  g_assert (g_args[6].b == TRUE);
  g_assert (g_args[7].b == FALSE);
  g_assert (g_args[8].b == TRUE);
  g_assert (g_args[9].b == FALSE);

  return rpc_method_send_reply (connection, RPC_TYPE_INVALID);
}

static int
handle_VOID__INT32x10 (rpc_connection_t *connection)
{
  rpc_test_get_args (connection,
					 RPC_TYPE_INT32, &g_args[0].i,
					 RPC_TYPE_INT32, &g_args[1].i,
					 RPC_TYPE_INT32, &g_args[2].i,
					 RPC_TYPE_INT32, &g_args[3].i,
					 RPC_TYPE_INT32, &g_args[4].i,
					 RPC_TYPE_INT32, &g_args[5].i,
					 RPC_TYPE_INT32, &g_args[6].i,
					 RPC_TYPE_INT32, &g_args[7].i,
					 RPC_TYPE_INT32, &g_args[8].i,
					 RPC_TYPE_INT32, &g_args[9].i,
					 RPC_TYPE_INVALID);

  g_assert (g_args[0].i == 0);
  g_assert (g_args[1].i == 1);
  g_assert (g_args[2].i == -1);
  g_assert (g_args[3].i == 2);
  g_assert (g_args[4].i == -2);
  g_assert (g_args[5].i == G_MAXINT32);
  g_assert (g_args[6].i == G_MININT32);
  g_assert (g_args[7].i == G_MAXINT32 - 1);
  g_assert (g_args[8].i == G_MININT32 + 1);
  g_assert (g_args[9].i == 0);

  return rpc_method_send_reply (connection, RPC_TYPE_INVALID);
}

static int
handle_VOID__UINT32x10 (rpc_connection_t *connection)
{
  rpc_test_get_args (connection,
					 RPC_TYPE_UINT32, &g_args[0].u,
					 RPC_TYPE_UINT32, &g_args[1].u,
					 RPC_TYPE_UINT32, &g_args[2].u,
					 RPC_TYPE_UINT32, &g_args[3].u,
					 RPC_TYPE_UINT32, &g_args[4].u,
					 RPC_TYPE_UINT32, &g_args[5].u,
					 RPC_TYPE_UINT32, &g_args[6].u,
					 RPC_TYPE_UINT32, &g_args[7].u,
					 RPC_TYPE_UINT32, &g_args[8].u,
					 RPC_TYPE_UINT32, &g_args[9].u,
					 RPC_TYPE_INVALID);

  g_assert (g_args[0].u == 0);
  g_assert (g_args[1].u == 1);
  g_assert (g_args[2].u == 0xffffffff);
  g_assert (g_args[3].u == 2);
  g_assert (g_args[4].u == 0xfffffffe);
  g_assert (g_args[5].u == G_MAXUINT32);
  g_assert (g_args[6].u == G_MAXUINT32 - 1);
  g_assert (g_args[7].u == 0x80000000);
  g_assert (g_args[8].u == 0x80000001);
  g_assert (g_args[9].u == 0);

  return rpc_method_send_reply (connection, RPC_TYPE_INVALID);
}

static int
handle_VOID__UINT64x10 (rpc_connection_t *connection)
{
  rpc_test_get_args (connection,
					 RPC_TYPE_UINT64, &g_args[0].j,
					 RPC_TYPE_UINT64, &g_args[1].j,
					 RPC_TYPE_UINT64, &g_args[2].j,
					 RPC_TYPE_UINT64, &g_args[3].j,
					 RPC_TYPE_UINT64, &g_args[4].j,
					 RPC_TYPE_UINT64, &g_args[5].j,
					 RPC_TYPE_UINT64, &g_args[6].j,
					 RPC_TYPE_UINT64, &g_args[7].j,
					 RPC_TYPE_UINT64, &g_args[8].j,
					 RPC_TYPE_UINT64, &g_args[9].j,
					 RPC_TYPE_INVALID);

  g_assert (g_args[0].j == 0);
  g_assert (g_args[1].j == G_GINT64_CONSTANT (0x00000000000000ffU));
  g_assert (g_args[2].j == G_GINT64_CONSTANT (0x000000000000ff00U));
  g_assert (g_args[3].j == G_GINT64_CONSTANT (0x0000000000ff0000U));
  g_assert (g_args[4].j == G_GINT64_CONSTANT (0x00000000ff000000U));
  g_assert (g_args[5].j == G_GINT64_CONSTANT (0x000000ff00000000U));
  g_assert (g_args[6].j == G_GINT64_CONSTANT (0x0000ff0000000000U));
  g_assert (g_args[7].j == G_GINT64_CONSTANT (0x00ff000000000000U));
  g_assert (g_args[8].j == G_GINT64_CONSTANT (0xff00000000000000U));
  g_assert (g_args[9].j == G_GINT64_CONSTANT (0x0123456789abcdefU));

  return rpc_method_send_reply (connection, RPC_TYPE_INVALID);
}

static int
handle_VOID__DOUBLEx5 (rpc_connection_t *connection)
{
  rpc_test_get_args (connection,
					 RPC_TYPE_DOUBLE, &g_args[0].d,
					 RPC_TYPE_DOUBLE, &g_args[1].d,
					 RPC_TYPE_DOUBLE, &g_args[2].d,
					 RPC_TYPE_DOUBLE, &g_args[3].d,
					 RPC_TYPE_DOUBLE, &g_args[4].d,
					 RPC_TYPE_INVALID);

  g_assert (g_args[0].d == 0.0);
  g_assert (g_args[1].d == 1.0);
  g_assert (g_args[2].d == -1.0);
  g_assert (g_args[3].d == 2.0);
  g_assert (g_args[4].d == -2.0);

  return rpc_method_send_reply (connection, RPC_TYPE_INVALID);
}

static int
handle_VOID__STRINGx3 (rpc_connection_t *connection)
{
  rpc_test_get_args (connection,
					 RPC_TYPE_STRING, &g_args[0].s,
					 RPC_TYPE_STRING, &g_args[1].s,
					 RPC_TYPE_STRING, &g_args[2].s,
					 RPC_TYPE_INVALID);

  g_assert (g_args[0].s && strcmp (g_args[0].s, "") == 0);
  free (g_args[0].s);
  g_assert (g_args[1].s && strcmp (g_args[1].s, "one") == 0);
  free (g_args[1].s);
  g_assert (g_args[2].s == NULL);

  return rpc_method_send_reply (connection, RPC_TYPE_INVALID);
}
#endif

int
rpc_test_init (int argc, char *argv[])
{
#ifdef BUILD_SERVER
  rpc_connection_t *connection;

  static const rpc_method_descriptor_t vtable[] =
	{
	  { RPC_TEST_METHOD_VOID__VOID,			handle_VOID__VOID			},
	  { RPC_TEST_METHOD_VOID__CHAR,			handle_VOID__CHAR			},
	  { RPC_TEST_METHOD_VOID__CHARx10,		handle_VOID__CHARx10		},
	  { RPC_TEST_METHOD_VOID__BOOL,			handle_VOID__BOOL			},
	  { RPC_TEST_METHOD_VOID__BOOLx10,		handle_VOID__BOOLx10		},
	  { RPC_TEST_METHOD_VOID__INT32x10,		handle_VOID__INT32x10		},
	  { RPC_TEST_METHOD_VOID__UINT32x10,	handle_VOID__UINT32x10		},
	  { RPC_TEST_METHOD_VOID__UINT64x10,	handle_VOID__UINT64x10		},
	  { RPC_TEST_METHOD_VOID__DOUBLEx5,		handle_VOID__DOUBLEx5		},
	  { RPC_TEST_METHOD_VOID__STRINGx3,		handle_VOID__STRINGx3		},
	};

  connection = rpc_test_get_connection ();
  g_assert (connection != NULL);

  if (rpc_connection_add_method_descriptors(connection,
											vtable,
											G_N_ELEMENTS (vtable)) < 0)
	g_error ("could not add method descriptors");
#endif

  return 0;
}

int
rpc_test_execute (gpointer user_data)
{
#ifdef BUILD_CLIENT
  rpc_test_invoke (RPC_TEST_METHOD_VOID__VOID,
				   RPC_TYPE_INVALID);

  rpc_test_invoke (RPC_TEST_METHOD_VOID__CHAR,
				   RPC_TYPE_CHAR, 'a',
				   RPC_TYPE_INVALID);

  rpc_test_invoke (RPC_TEST_METHOD_VOID__CHARx10,
				   RPC_TYPE_CHAR, 'a',
				   RPC_TYPE_CHAR, 'b',
				   RPC_TYPE_CHAR, 'c',
				   RPC_TYPE_CHAR, 'd',
				   RPC_TYPE_CHAR, 'e',
				   RPC_TYPE_CHAR, '1',
				   RPC_TYPE_CHAR, '2',
				   RPC_TYPE_CHAR, '3',
				   RPC_TYPE_CHAR, '4',
				   RPC_TYPE_CHAR, '5',
				   RPC_TYPE_INVALID);

  rpc_test_invoke (RPC_TEST_METHOD_VOID__BOOL,
				   RPC_TYPE_BOOLEAN, TRUE,
				   RPC_TYPE_INVALID);

  rpc_test_invoke (RPC_TEST_METHOD_VOID__BOOLx10,
				   RPC_TYPE_BOOLEAN, TRUE,
				   RPC_TYPE_BOOLEAN, FALSE,
				   RPC_TYPE_BOOLEAN, TRUE,
				   RPC_TYPE_BOOLEAN, FALSE,
				   RPC_TYPE_BOOLEAN, TRUE,
				   RPC_TYPE_BOOLEAN, FALSE,
				   RPC_TYPE_BOOLEAN, TRUE,
				   RPC_TYPE_BOOLEAN, FALSE,
				   RPC_TYPE_BOOLEAN, TRUE,
				   RPC_TYPE_BOOLEAN, FALSE,
				   RPC_TYPE_INVALID);

  rpc_test_invoke (RPC_TEST_METHOD_VOID__INT32x10,
				   RPC_TYPE_INT32, 0,
				   RPC_TYPE_INT32, 1,
				   RPC_TYPE_INT32, -1,
				   RPC_TYPE_INT32, 2,
				   RPC_TYPE_INT32, -2,
				   RPC_TYPE_INT32, G_MAXINT32,
				   RPC_TYPE_INT32, G_MININT32,
				   RPC_TYPE_INT32, G_MAXINT32 - 1,
				   RPC_TYPE_INT32, G_MININT32 + 1,
				   RPC_TYPE_INT32, 0,
				   RPC_TYPE_INVALID);

  rpc_test_invoke (RPC_TEST_METHOD_VOID__UINT32x10,
				   RPC_TYPE_UINT32, 0,
				   RPC_TYPE_UINT32, 1,
				   RPC_TYPE_UINT32, 0xffffffff,
				   RPC_TYPE_UINT32, 2,
				   RPC_TYPE_UINT32, 0xfffffffe,
				   RPC_TYPE_UINT32, G_MAXUINT32,
				   RPC_TYPE_UINT32, G_MAXUINT32 - 1,
				   RPC_TYPE_UINT32, 0x80000000,
				   RPC_TYPE_UINT32, 0x80000001,
				   RPC_TYPE_UINT32, 0,
				   RPC_TYPE_INVALID);

  rpc_test_invoke (RPC_TEST_METHOD_VOID__UINT64x10,
				   RPC_TYPE_UINT64, 0,
				   RPC_TYPE_UINT64, G_GINT64_CONSTANT (0x00000000000000ffU),
				   RPC_TYPE_UINT64, G_GINT64_CONSTANT (0x000000000000ff00U),
				   RPC_TYPE_UINT64, G_GINT64_CONSTANT (0x0000000000ff0000U),
				   RPC_TYPE_UINT64, G_GINT64_CONSTANT (0x00000000ff000000U),
				   RPC_TYPE_UINT64, G_GINT64_CONSTANT (0x000000ff00000000U),
				   RPC_TYPE_UINT64, G_GINT64_CONSTANT (0x0000ff0000000000U),
				   RPC_TYPE_UINT64, G_GINT64_CONSTANT (0x00ff000000000000U),
				   RPC_TYPE_UINT64, G_GINT64_CONSTANT (0xff00000000000000U),
				   RPC_TYPE_UINT64, G_GINT64_CONSTANT (0x0123456789abcdefU),
				   RPC_TYPE_INVALID);

  rpc_test_invoke (RPC_TEST_METHOD_VOID__DOUBLEx5,
				   RPC_TYPE_DOUBLE, 0.0,
				   RPC_TYPE_DOUBLE, 1.0,
				   RPC_TYPE_DOUBLE, -1.0,
				   RPC_TYPE_DOUBLE, 2.0,
				   RPC_TYPE_DOUBLE, -2.0,
				   RPC_TYPE_INVALID);

  rpc_test_invoke (RPC_TEST_METHOD_VOID__STRINGx3,
				   RPC_TYPE_STRING, "",
				   RPC_TYPE_STRING, "one",
				   RPC_TYPE_STRING, NULL,
				   RPC_TYPE_INVALID);
#endif
  return RPC_TEST_EXECUTE_SUCCESS;
}
