#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "protocol.h"

enum {
  DEBUGGER_BIRTH = 1,
  DEBUGGER_INITIALIZED = 2,
  DEBUGGER_PROGRAM_LAUNCHED = 3,
  DEBUGGER_PROGRAM_RUNNING = 4,
  DEBUGGER_PROGRAM_STOPPED = 5,
  DEBUGGER_PROGRAM_TERMINATED = 6
};


static FILE *log = NULL;
static int thread_event_sent = 0;
static int debugger_state = DEBUGGER_BIRTH;


/*
* Send VSCode a StoppedEvent
* The event indicates that the execution of the debuggee has stopped due to some condition.
* This can be caused by a break point previously set, a stepping action has completed, by executing a debugger statement etc.
*/
static void send_stopped_event(ProtocolMessage *res, const char *msg,
  FILE *out) {
  char buf[1024];
  vscode_make_stopped_event(res, msg);
  vscode_serialize_event(buf, sizeof buf, res);
  fprintf(log, "%s\n", buf);
  fprintf(out, buf);
}

/*
* Send VSCode a ThreadEvent
*/
static void send_thread_event(ProtocolMessage *res, bool started,
  FILE *out) {
  char buf[1024];
  vscode_make_thread_event(res, started);
  vscode_serialize_event(buf, sizeof buf, res);
  fprintf(log, "%s\n", buf);
  fprintf(out, buf);
}

/*
* Send VSCode a TerminatedEvent
*/
static void send_terminated_event(ProtocolMessage *res, FILE *out) {
  char buf[1024];
  vscode_make_terminated_event(res);
  vscode_serialize_event(buf, sizeof buf, res);
  fprintf(log, "%s\n", buf);
  fprintf(out, buf);
}


static void send_output_event(ProtocolMessage *res, const char *msg,
                              FILE *out) {
  char buf[1024];
  vscode_make_output_event(res, msg);
  vscode_serialize_event(buf, sizeof buf, res);
  fprintf(log, "%s\n", buf);
  fprintf(out, buf);
}

static void send_error_response(ProtocolMessage *req, ProtocolMessage *res,
                                int responseType, const char *msg, FILE *out) {
  char buf[1024];
  vscode_make_error_response(req, res, responseType, msg);
  vscode_serialize_response(buf, sizeof buf, res);
  fprintf(log, "%s\n", buf);
  fprintf(out, buf);
}

static void send_success_response(ProtocolMessage *req, ProtocolMessage *res,
                                  int responseType, FILE *out) {
  char buf[1024];
  vscode_make_success_response(req, res, responseType);
  vscode_serialize_response(buf, sizeof buf, res);
  fprintf(log, "%s\n", buf);
  fprintf(out, buf);
}

/*
* Generate response to InitializeRequest
* Send InitializedEvent
*/
static void handle_initialize_request(ProtocolMessage *req,
                                      ProtocolMessage *res, FILE *out) {
  char buf[1024];
  if (debugger_state >= DEBUGGER_INITIALIZED) {
    send_error_response(req, res, VSCODE_INITIALIZE_RESPONSE, "already initialized", out);
    return;
  }
  /* Send InitializedEvent */
  vscode_make_initialized_event(res);
  vscode_serialize_event(buf, sizeof buf, res);
  fprintf(log, "%s\n", buf);
  fprintf(out, buf);

  /* Send InitializeResponse */
  vscode_make_success_response(req, res, VSCODE_INITIALIZE_RESPONSE);
  res->u.Response.u.InitializeResponse.body.supportsConfigurationDoneRequest =
      1;
  vscode_serialize_response(buf, sizeof buf, res);
  fprintf(log, "%s\n", buf);
  fprintf(out, buf);

  /* Send notification */
  send_output_event(res, "Debugger initialized", out);
  debugger_state = DEBUGGER_INITIALIZED;
}

/*
* Generate response to ThreadRequest
*/
static void handle_thread_request(ProtocolMessage *req,
  ProtocolMessage *res, FILE *out) {
  char buf[1024];
  vscode_make_success_response(req, res, VSCODE_THREAD_RESPONSE);
  res->u.Response.u.ThreadResponse.threads[0].id = 1;
  strncpy(res->u.Response.u.ThreadResponse.threads[0].name, "Lua Thread", sizeof res->u.Response.u.ThreadResponse.threads[0].name);
  vscode_serialize_response(buf, sizeof buf, res);
  fprintf(log, "%s\n", buf);
  fprintf(out, buf);
}

/*
* Handle StackTraceRequest
*/
static void handle_stack_trace_request(ProtocolMessage *req,
  ProtocolMessage *res, lua_State *L, FILE *out) {
  char buf[1024];
  lua_Debug entry;
  int depth = 0;
  vscode_make_success_response(req, res, VSCODE_STACK_TRACE_RESPONSE);
  while (lua_getstack(L, depth, &entry) && depth < req->u.Request.u.StackTraceRequest.levels && depth < MAX_STACK_FRAMES)
  {
    int status = lua_getinfo(L, "Sln", &entry);
    assert(status);
    const char *src = entry.source;
    if (*src == '@') src++;
    const char *last_path_delim = strrchr(src, '/');
    char path[1024];
    char name[256];
    if (last_path_delim) {
      strncpy(name, last_path_delim + 1, sizeof name);
    }
    else {
      strncpy(name, src, sizeof name);
    }
    strncpy(path, src, sizeof path);
    res->u.Response.u.StackTraceResponse.stackFrames[depth].id = depth;
    strncpy(res->u.Response.u.StackTraceResponse.stackFrames[depth].source.path, path, sizeof res->u.Response.u.StackTraceResponse.stackFrames[depth].source.path);
    strncpy(res->u.Response.u.StackTraceResponse.stackFrames[depth].source.name, name, sizeof res->u.Response.u.StackTraceResponse.stackFrames[depth].source.name);
    res->u.Response.u.StackTraceResponse.stackFrames[depth].line = entry.currentline;
    const char *funcname = entry.name ? entry.name : "?";
    strncpy(res->u.Response.u.StackTraceResponse.stackFrames[depth].name, funcname, sizeof res->u.Response.u.StackTraceResponse.stackFrames[depth].name);
    depth++;
  }
  res->u.Response.u.StackTraceResponse.totalFrames = depth;
  vscode_serialize_response(buf, sizeof buf, res);
  fprintf(log, "%s\n", buf);
  fprintf(out, buf);
}

/*
* Handle ScopeRequest
*/
static void handle_scopes_request(ProtocolMessage *req,
  ProtocolMessage *res, lua_State *L, FILE *out) {
  char buf[1024];
  lua_Debug entry;
  int depth = 0;
  vscode_make_success_response(req, res, VSCODE_SCOPES_RESPONSE);
  depth = req->u.Request.u.ScopesRequest.frameId;
  if (lua_getstack(L, depth, &entry))
  {
    int status = lua_getinfo(L, "u", &entry);
    assert(status);
    int i = 0;
    strncpy(res->u.Response.u.ScopesResponse.scopes[i].name, "Locals", sizeof res->u.Response.u.ScopesResponse.scopes[0].name);
    res->u.Response.u.ScopesResponse.scopes[i].variablesReference = 1000000 + depth;
    res->u.Response.u.ScopesResponse.scopes[i++].expensive = 0;
    if (entry.nups > 0) {
      strncpy(res->u.Response.u.ScopesResponse.scopes[i].name, "Up Values", sizeof res->u.Response.u.ScopesResponse.scopes[0].name);
      res->u.Response.u.ScopesResponse.scopes[i].variablesReference = 2000000 + depth;
      res->u.Response.u.ScopesResponse.scopes[i++].expensive = 0;
    }
    strncpy(res->u.Response.u.ScopesResponse.scopes[i].name, "Globals", sizeof res->u.Response.u.ScopesResponse.scopes[0].name);
    res->u.Response.u.ScopesResponse.scopes[i].variablesReference = 3000000 + depth;
    res->u.Response.u.ScopesResponse.scopes[i].expensive = 1;
  }
  else {
    vscode_make_error_response(req, res, VSCODE_SCOPES_RESPONSE, "Error retrieving stack frame");
  }
  vscode_serialize_response(buf, sizeof buf, res);
  fprintf(log, "%s\n", buf);
  fprintf(out, buf);
}

/*
* Handle ScopeRequest
*/
static void handle_variables_request(ProtocolMessage *req,
  ProtocolMessage *res, lua_State *L, FILE *out) {
  char buf[1024];
  lua_Debug entry;
  vscode_make_success_response(req, res, VSCODE_VARIABLES_RESPONSE);
  int varRef = req->u.Request.u.VariablesRequest.variablesReference;
  int depth = 0;
  char type = 0;
  if (varRef >= 3000000) {
    type = 'g';
    depth = varRef - 3000000;
  }
  else if (varRef >= 2000000) {
    type = 'u';
    depth = varRef - 2000000;
  }
  else {
    type = 'l';
    depth = varRef - 1000000;
  }
  if (type == 'l' && lua_getstack(L, depth, &entry)) {
    int x = 0;
    for (int n = 1; n < MAX_VARIABLES; n++) {
      const char *name = lua_getlocal(L, &entry, n);
      if (name) {
        strncpy(res->u.Response.u.VariablesResponse.variables[x].name, name, sizeof res->u.Response.u.VariablesResponse.variables[x].name);
        lua_pop(L, 1);
      }
      else {
        break;
      }
    }
  }
  else {
    vscode_make_error_response(req, res, VSCODE_VARIABLES_RESPONSE, "Error retrieving variables");
  }
  vscode_serialize_response(buf, sizeof buf, res);
  fprintf(log, "%s\n", buf);
  fprintf(out, buf);
}

static void handle_launch_request(ProtocolMessage *req, ProtocolMessage *res,
                                  lua_State *L, FILE *out) {
  if (debugger_state != DEBUGGER_INITIALIZED) {
    send_error_response(req, res, VSCODE_LAUNCH_RESPONSE,
                               "not initialized or unexpected state", out);
    return;
  }

  const char *progname = req->u.Request.u.LaunchRequest.program;
  fprintf(log, "\n--> Launching '%s'\n", progname);
  int status = luaL_loadfile(L, progname);
  if (status != LUA_OK) {
    char temp[1024];
    snprintf(temp, sizeof temp, "Failed to launch %s due to error: %s",
             progname, lua_tostring(L, -1));
    send_output_event(res, temp, out);
    send_error_response(req, res, VSCODE_LAUNCH_RESPONSE, "Launch failed", out);
    lua_pop(L, 1);
    return;
  }
  else {
    send_success_response(req, res, VSCODE_LAUNCH_RESPONSE, out);
  }
  debugger_state = DEBUGGER_PROGRAM_RUNNING;
  if (lua_pcall(L, 0, 0, 0)) {
    send_output_event(res, "Program terminated with error", out);
    send_output_event(res, lua_tostring(L, -1), out);
    lua_pop(L, 1);
  }
  send_terminated_event(res, out);
  debugger_state = DEBUGGER_PROGRAM_TERMINATED;
}

/**
 * Called via Lua Hook or via main
 * If called from main then init is true and ar == NULL
 */
static void debugger(lua_State *L, bool init, lua_Debug *ar, FILE *in,
                     FILE *out) {
  char buf[4096] = {0};
  ProtocolMessage req, res;
  if (debugger_state == DEBUGGER_PROGRAM_TERMINATED) {
    return;
  }
  if (debugger_state == DEBUGGER_PROGRAM_RUNNING) {
    /* running within Lua at line change */
    if (!thread_event_sent) {
      /* thread started - only sent once in the debug session */
      thread_event_sent = 1;
      send_thread_event(&res, true, out);
      /* Inform VSCode we have stopped */
      send_stopped_event(&res, "entry", out);
    }
    else {
      /* Inform VSCode we have stopped */
      send_stopped_event(&res, "step", out);
    }
    debugger_state = DEBUGGER_PROGRAM_STOPPED;
  }
  bool get_command = true;
  /* Get command from VSCode 
   * VSCode commands begin with the sequence:
   * 'Content-Length: nnn\r\n\r\n'
   * This is followed by nnn bytes which has a JSON
   * format request message
   * We currently don't bother checking the
   * \r\n\r\n sequence for incoming messages 
   */
  while (get_command && fgets(buf, sizeof buf, in) != NULL) {
    buf[sizeof buf - 1] = 0; /* NULL terminate - just in case */
    const char *bufp = strstr(buf, "Content-Length: ");
    if (bufp != NULL) {
      bufp += 16;
      /* get the message length */
      int len = atoi(bufp);
      if (len >= sizeof buf) {
        /* FIXME */
        fprintf(log, "FATAL ERROR - Content-Length = %d is greater than bufsize\n", len);
        exit(1);
      }
      buf[0] = 0;
      /* skip blank line - actually \r\n */
      if (fgets(buf, sizeof buf, stdin) == NULL) break;
      /* Now read exact number of characters */
      buf[0] = 0;
      if (fread(buf, len, 1, stdin) != 1) {
        fprintf(log, "FATAL ERROR - cannot read %d bytes\n", len);
        exit(1);
      }
      buf[len] = 0;
      fprintf(log, "Content-Length: %d\r\n\r\n%s", len, buf);
      fflush(log);

      /* Parse the VSCode request */
      int command = vscode_parse_message(buf, sizeof buf, &req, log);
      switch (command) {
        case VSCODE_INITIALIZE_REQUEST: {
          handle_initialize_request(&req, &res, out);
          break;
        }
        case VSCODE_LAUNCH_REQUEST: {
          handle_launch_request(&req, &res, L, out);
          break;
        }
        case VSCODE_STACK_TRACE_REQUEST: {
          handle_stack_trace_request(&req, &res, L, out);
          break;
        }
        case VSCODE_SCOPES_REQUEST: {
          handle_scopes_request(&req, &res, L, out);
          break;
        }
        case VSCODE_VARIABLES_REQUEST: {
          handle_variables_request(&req, &res, L, out);
          break;
        }
        case VSCODE_DISCONNECT_REQUEST: {
          send_success_response(&req, &res, VSCODE_DISCONNECT_RESPONSE, out);
          exit(0);
        }
        case VSCODE_SET_EXCEPTION_BREAKPOINTS_REQUEST: {
          send_success_response(&req, &res, VSCODE_SET_EXCEPTION_BREAKPOINTS_RESPONSE, out);
          break;
        }
        case VSCODE_CONFIGURATION_DONE_REQUEST: {
          send_success_response(&req, &res, VSCODE_CONFIGURATION_DONE_RESPONSE, out);
          break;
        }
        case VSCODE_THREAD_REQUEST: {
          handle_thread_request(&req, &res, out);
          break;
        }
        case VSCODE_STEPIN_REQUEST: {
          send_success_response(&req, &res, VSCODE_STEPIN_RESPONSE, out);
          get_command = false;
          break;
        }
        case VSCODE_STEPOUT_REQUEST: {
          send_success_response(&req, &res, VSCODE_STEPOUT_RESPONSE, out);
          get_command = false;
          break;
        }
        case VSCODE_NEXT_REQUEST: {
          send_success_response(&req, &res, VSCODE_NEXT_RESPONSE, out);
          get_command = false;
          break;
        }
        default: {
          char msg[100];
          snprintf(msg, sizeof msg, "%s not yet implemented", req.u.Request.command);
          fprintf(log, "%s\n", msg);
          send_error_response(&req, &res, command, msg, out);
          break;
        }
      }
    }
    else {
      fprintf(log, "\nUnexpected: %s\n", buf);
    }
    fprintf(log, "\nWaiting for command\n");
  }
  debugger_state = DEBUGGER_PROGRAM_RUNNING;
}

/* 
* Lua Hook used by the debugger 
* Setup to intercept at every line change
*/
void ravi_debughook(lua_State *L, lua_Debug *ar) {
  int event = ar->event;
  if (event == LUA_HOOKLINE) {
    debugger(L, false, ar, stdin, stdout);
  }
}

/*
* Entry point for the debugger
* The debugger will use stdin/stdout to interact with VSCode
* The protocol used is described in protocol.h.
*/
int main(int argc, const char *argv[]) {
  log = fopen("\\temp\\out1.txt", "w");
  if (log == NULL) exit(1);
#ifdef _WIN32
  /* The VSCode debug protocol requires binary IO */
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  /* switch off buffering */
  setbuf(log, NULL);
  setbuf(stdout, NULL);
  lua_State *L = luaL_newstate(); /* create Lua state */
  if (L == NULL) { return EXIT_FAILURE; }
  luaL_checkversion(L); /* check that interpreter has correct version */
  /* TODO need to redirect the stdin/stdout used by Lua */
  luaL_openlibs(L);     /* open standard libraries */
  lua_sethook(L, ravi_debughook, LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET, 0);
  debugger_state = DEBUGGER_BIRTH;
  debugger(L, true, NULL, stdin, stdout);
  lua_close(L);
  fclose(log);
  return 0;
}