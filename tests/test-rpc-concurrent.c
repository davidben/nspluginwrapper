/*
 *  test-rpc-concurrent.c - Test concurrent RPC invoke
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
#include <string.h>

#define DEBUG 1
#include "debug.h"

#define RPC_TEST_USE_IDLE   TRUE

enum
  {
	RPC_TEST_METHOD_PRINT = 1
  };

static gboolean g_is_silent   = TRUE;
static gint     g_print_count = 5000;

static void
invoke_print (const gchar *str)
{
  rpc_connection_t *connection;
  int               error;

  connection = rpc_test_get_connection ();
  g_assert (connection != NULL);

  error = rpc_method_invoke (connection,
							 RPC_TEST_METHOD_PRINT,
							 RPC_TYPE_STRING, str,
							 RPC_TYPE_INVALID);
  /* Error out early for invalid messages types.

	 The problem we are looking at is rpc_method_invoke() waiting for
	 the other side MSG_ACK prior to sending the arguments. Sometimes,
	 the other side sends a new message first (MSG_START). So, we get
	 a mismatch. */
  g_assert (error != RPC_ERROR_MESSAGE_TYPE_INVALID);
  g_assert (error == RPC_ERROR_NO_ERROR);

  error = rpc_method_wait_for_reply (connection, RPC_TYPE_INVALID);
  g_assert (error == RPC_ERROR_NO_ERROR);
}

static int
handle_print (rpc_connection_t *connection)
{
  char *str;
  int   error;

  error = rpc_method_get_args (connection,
							   RPC_TYPE_STRING, &str,
							   RPC_TYPE_INVALID);
  g_assert (error == RPC_ERROR_NO_ERROR);
  g_assert (str != NULL);

  if (!g_is_silent)
	npw_printf ("Got message from the other end: '%s'\n", str);
  free (str);

  return rpc_method_send_reply (connection, RPC_TYPE_INVALID);
}

static inline void
print_msg (void)
{
  invoke_print ("Hello from " NPW_COMPONENT_NAME);
}

static gboolean
invoke_print_cb (gpointer user_data)
{
  print_msg ();

  --g_print_count;
#ifdef BUILD_CLIENT
  if (g_print_count == 0)
	rpc_test_exit_full (0);
#endif
  return g_print_count > 0;
}

int
rpc_test_init (int argc, char *argv[])
{
  rpc_connection_t *connection;

  for (int i = 1; i < argc; i++)
	{
	  const gchar *arg = argv[i];
	  if (strcmp (arg, "--silent") == 0 || strcmp (arg, "-q") == 0)
		g_is_silent = TRUE;
	  else if (strcmp (arg, "--verbose") == 0 || strcmp (arg, "-v") == 0)
		g_is_silent = FALSE;
	  else if (strcmp (arg, "--count") == 0)
		{
		  if (++i < argc)
			{
			  unsigned long v = strtoul (argv[i], NULL, 10);
			  if (v > 0)
				g_print_count = v;
			}
		}
	  else if (strcmp (arg, "--help") == 0)
		{
		  g_print ("Usage: %s [--silent|--verbose] [--count COUNT]\n", argv[0]);
		  rpc_test_exit (0);
		}
	}

  connection = rpc_test_get_connection ();
  g_assert (connection != NULL);

  static const rpc_method_descriptor_t vtable[] = {
	{ RPC_TEST_METHOD_PRINT, handle_print },
  };

  if (rpc_connection_add_method_descriptor (connection, &vtable[0]) < 0)
	g_error ("could not add method descriptors");

  if (RPC_TEST_USE_IDLE)
	{
	  /* XXX: we hope to trigger concurrent rpc_method_invoke() */
	  /* XXX: add a barrier to synchronize both processes? */
	  //g_idle_add (invoke_print_cb, NULL);
	  g_timeout_add (0, invoke_print_cb, NULL);
	}
  else
	{
	  while (--g_print_count >= 0)
		print_msg ();
	}
  return 0;
}

int
rpc_test_execute (gpointer user_data)
{
#ifdef BUILD_CLIENT
  if (RPC_TEST_USE_IDLE)
	return RPC_TEST_EXECUTE_SUCCESS|RPC_TEST_EXECUTE_DONT_QUIT;
#endif
  return RPC_TEST_EXECUTE_SUCCESS;
}
