#include "input_capture.h"

#include "xdpw.h"
#include "logger.h"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>

static const char *INPUTCAPTURE_INTERFACE_NAME = "org.freedesktop.impl.portal.InputCapture";
static const char *OBJECT_PATH_NAME = "/org/freedesktop/portal/desktop";

/* --- static global data --- */

static struct xdpw_input_capture_data interface_data = {
  .xdpw_state = NULL,
  .capabilities = 1 | 2,   // Keyboard | Pointer | Touchscreen (4), not implemented yet
  .version = 1,
  .bus = NULL,
  .eis_context = NULL,
  .wl_display = NULL,
  .wl_registry = NULL,
  .wl_compositor = NULL,
  .wl_layer_shell = NULL,
  .wl_seat = NULL,
  .wl_pointer_constraints = NULL,
  .wl_keyboard_shortcuts_manager = NULL,
  .xkb_context = NULL,
  .active_session = NULL
};

/* --- forward declarations --- */
// dbus properties
static int dbus_property_SupportedCapabilities(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *, sd_bus_error *);
static int dbus_property_version(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *, sd_bus_error *);

// dbus methods
static int dbus_method_CreateSession(sd_bus_message *, void *, sd_bus_error *);
static int dbus_method_GetZones(sd_bus_message *, void *, sd_bus_error *);
static int dbus_method_SetPointerBarriers(sd_bus_message *, void *, sd_bus_error *);
static int dbus_method_Enable(sd_bus_message *, void *, sd_bus_error *);
static int dbus_method_Disable(sd_bus_message *, void *, sd_bus_error *);
static int dbus_method_Release(sd_bus_message *, void *, sd_bus_error *);
static int dbus_method_ConnectToEIS(sd_bus_message *, void *, sd_bus_error *);

// dbus signals
static int dbus_signal_Activated(sd_bus *, struct xdpw_session *, uint32_t, double, double);
static int dbus_signal_Deactivated(sd_bus *, struct xdpw_session *, double, double);
static int dbus_signal_ZonesChanged(sd_bus *, const char *);
static int dbus_signal_Disabled(sd_bus *, const char *) __attribute__((unused));

// dbus helper functions
static int dbus_helper_drain_dict(sd_bus_message *);
static int dbus_helper_parse_CreateSession_options(sd_bus_message *, uint32_t *);
static int dbus_helper_parse_Release_options(sd_bus_message *, uint32_t *, double *, double *);
static char *dbus_helper_create_session_id(void);

// eis helper functions
static struct xdpw_session *eis_helper_find_session(const char *);
static void eis_helper_handle_event(struct eis_event *);

// wayland callback functions
static void wayland_handle_layer_surface_configure(void *, struct zwlr_layer_surface_v1 *, uint32_t, uint32_t, uint32_t);
static void wayland_handle_layer_surface_closed(void *, struct zwlr_layer_surface_v1 *);

static void wayland_registry_global(void *, struct wl_registry *, uint32_t, const char *, uint32_t);
static void wayland_registry_global_remove(void *, struct wl_registry *, uint32_t);
static void cleanup_session_wayland(struct xdpw_session *);

static void wayland_handle_pointer_enter(void *, struct wl_pointer *, uint32_t, struct wl_surface *, wl_fixed_t, wl_fixed_t);
static void wayland_handle_pointer_leave(void *, struct wl_pointer *, uint32_t, struct wl_surface *);
static void wayland_handle_pointer_motion(void *, struct wl_pointer *, uint32_t, wl_fixed_t, wl_fixed_t);
static void wayland_handle_pointer_button(void *, struct wl_pointer *, uint32_t, uint32_t, uint32_t, uint32_t);
static void wayland_handle_pointer_axis(void *, struct wl_pointer *, uint32_t, uint32_t, wl_fixed_t);

static void wayland_handle_keyboard_keymap(void *, struct wl_keyboard *, uint32_t, int32_t, uint32_t);
static void wayland_handle_keyboard_enter(void *, struct wl_keyboard *, uint32_t, struct wl_surface *, struct wl_array *);
static void wayland_handle_keyboard_leave(void *, struct wl_keyboard *, uint32_t, struct wl_surface *);
static void wayland_handle_keyboard_key(void *, struct wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t);
static void wayland_handle_keyboard_modifiers(void *, struct wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static void wayland_handle_keyboard_repeat(void *, struct wl_keyboard *, int32_t, int32_t);

static void wayland_handle_seat_capabilities(void *, struct wl_seat *, uint32_t);
static void wayland_handle_seat_name(void *, struct wl_seat *, const char *);

static void wayland_handle_inhibitor_active(void *, struct zwp_keyboard_shortcuts_inhibitor_v1 *);
static void wayland_handle_inhibitor_inactive(void *, struct zwp_keyboard_shortcuts_inhibitor_v1 *);

static void wayland_handle_locked_pointer_locked(void *, struct zwp_locked_pointer_v1 *);
static void wayland_handle_locked_pointer_unlocked(void *, struct zwp_locked_pointer_v1 *);

static void output_handle_geometry(void *, struct wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *, int32_t);
static void output_handle_mode(void *, struct wl_output *, uint32_t, int32_t, int32_t, int32_t);
static void output_handle_done(void *, struct wl_output *);
static void output_handle_scale(void *, struct wl_output *, int32_t);

static void free_barrier_list(struct xdpw_input_capture_barrier *);

/* --- dbus vtable --- */
static const sd_bus_vtable input_capture_vtable[] = {
  SD_BUS_VTABLE_START(0),
  SD_BUS_PROPERTY("SupportedCapabilities", "u", dbus_property_SupportedCapabilities, offsetof(struct xdpw_input_capture_data, capabilities), SD_BUS_VTABLE_PROPERTY_CONST),
  SD_BUS_PROPERTY("version", "u", dbus_property_version, offsetof(struct xdpw_input_capture_data, version), SD_BUS_VTABLE_PROPERTY_CONST),
  SD_BUS_METHOD("CreateSession", "oossa{sv}", "ua{sv}", dbus_method_CreateSession, SD_BUS_VTABLE_UNPRIVILEGED),
  SD_BUS_METHOD("GetZones", "oosa{sv}", "ua{sv}", dbus_method_GetZones, SD_BUS_VTABLE_UNPRIVILEGED),
  SD_BUS_METHOD("SetPointerBarriers", "oosa{sv}aa{sv}u", "ua{sv}", dbus_method_SetPointerBarriers, SD_BUS_VTABLE_UNPRIVILEGED),
  SD_BUS_METHOD("Enable", "osa{sv}", "ua{sv}", dbus_method_Enable, SD_BUS_VTABLE_UNPRIVILEGED),
  SD_BUS_METHOD("Disable", "osa{sv}", "ua{sv}", dbus_method_Disable, SD_BUS_VTABLE_UNPRIVILEGED),
  SD_BUS_METHOD("Release", "osa{sv}", "ua{sv}", dbus_method_Release, SD_BUS_VTABLE_UNPRIVILEGED),
  SD_BUS_METHOD("ConnectToEIS", "osa{sv}", "h", dbus_method_ConnectToEIS, SD_BUS_VTABLE_UNPRIVILEGED),
  SD_BUS_SIGNAL("Disabled", "oa{sv}", 0),
  SD_BUS_SIGNAL("Activated", "oa{sv}", 0),
  SD_BUS_SIGNAL("Deactivated", "oa{sv}", 0),
  SD_BUS_SIGNAL("ZonesChanged", "oa{sv}", 0),
  SD_BUS_VTABLE_END,
};

/* --- Wayland listeners --- */
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
  .configure = wayland_handle_layer_surface_configure,
  .closed = wayland_handle_layer_surface_closed
};

static const struct wl_registry_listener registry_listener = {
  .global = wayland_registry_global,
  .global_remove = wayland_registry_global_remove
};

static const struct wl_pointer_listener pointer_listener = {
  .enter = wayland_handle_pointer_enter,
  .leave = wayland_handle_pointer_leave,
  .motion = wayland_handle_pointer_motion,
  .button = wayland_handle_pointer_button,
  .axis = wayland_handle_pointer_axis
};

static const struct wl_keyboard_listener keyboard_listener = {
  .keymap = wayland_handle_keyboard_keymap,
  .enter = wayland_handle_keyboard_enter,
  .leave = wayland_handle_keyboard_leave,
  .key = wayland_handle_keyboard_key,
  .modifiers = wayland_handle_keyboard_modifiers,
  .repeat_info = wayland_handle_keyboard_repeat
};

static const struct wl_seat_listener seat_listener = {
  .capabilities = wayland_handle_seat_capabilities,
  .name = wayland_handle_seat_name
};

static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener inhibitor_listener = {
  .active = wayland_handle_inhibitor_active,
  .inactive = wayland_handle_inhibitor_inactive
};

static const struct zwp_locked_pointer_v1_listener locked_pointer_listener = {
  .locked = wayland_handle_locked_pointer_locked,
  .unlocked = wayland_handle_locked_pointer_unlocked
};

static const struct wl_output_listener output_listener = {
  .geometry = output_handle_geometry,
  .mode = output_handle_mode,
  .done = output_handle_done,
  .scale = output_handle_scale
};

static void free_barrier_list(struct xdpw_input_capture_barrier *list) {
  struct xdpw_input_capture_barrier *b = list;
  while (b) {
    struct xdpw_input_capture_barrier *next = b->next;
    free(b);
    b = next;
  }
}

void xdpw_input_capture_session_data_free(struct xdpw_input_capture_session_data *ic) {
  if (!ic) return;
  free_barrier_list(ic->barriers);
}

/*--------------------------------------------- Properties ------------------------------------------------------------*/
static int dbus_property_SupportedCapabilities(sd_bus *bus, const char *path, const char *interface, 
                                           const char *member, sd_bus_message *reply, 
                                           void *userdata, sd_bus_error *ret_error) {
  (void)bus;
  (void)path;
  (void)interface;
  (void)member;
  (void)userdata;
  (void)ret_error;
  return sd_bus_message_append(reply, "u", interface_data.capabilities);
}

static int dbus_property_version(sd_bus *bus, const char *path, const char *interface, 
                                           const char *member, sd_bus_message *reply, 
                                           void *userdata, sd_bus_error *ret_error) {
  (void)bus;
  (void)path;
  (void)interface;
  (void)member;
  (void)userdata;
  (void)ret_error;
  return sd_bus_message_append(reply, "u", interface_data.version);
}

static int dbus_helper_drain_dict(sd_bus_message *m) {
  int r = sd_bus_message_enter_container(m, 'a', "{sv}");
  if (r < 0) {
    logprint(ERROR, "Error entering container: %s", strerror(-r));
    return r;
  }
  while (1) {
    r = sd_bus_message_skip(m, "{sv}");
    if (r < 0) {
      logprint(ERROR, "Error skipping key-value pair in dictionary: %s", strerror(-r));
      return r;
    }
    if (r == 0) break;
  }
  return sd_bus_message_exit_container(m);
}

static int dbus_helper_parse_CreateSession_options(sd_bus_message *m, uint32_t *capabilities) {
  int r;

  r = sd_bus_message_enter_container(m, 'a', "{sv}");
  if (r < 0) {
    logprint(ERROR, "Error entering container: %s", strerror(-r));
    return r;
  }

  while (sd_bus_message_at_end(m, 0) == 0) {
    const char *key;

    r = sd_bus_message_enter_container(m, 'e', "sv");
    if (r < 0) {
      logprint(ERROR, "Failed to enter dict entry: %s", strerror(-r));
      return r;
    }

    r = sd_bus_message_read(m, "s", &key);
    if (r < 0) {
      logprint(ERROR, "Failed to read dict key: %s", strerror(-r));
      return r;
    }

    if (strcmp(key, "capabilities") == 0) {
      r = sd_bus_message_read(m, "v", "u", capabilities);
      if (r < 0) {
        logprint(ERROR, "Failed to read capabilities's value: %s", strerror(-r));
        return r;
      }
    }
    else {
      logprint(DEBUG, "Skipping unknown option: %s", key);
      sd_bus_message_skip(m, "v");
      if (r < 0) {
        logprint(ERROR, "Failed to skip variant for key '%s': %s", key, strerror(-r));
        return r;
      }
    }

    r = sd_bus_message_exit_container(m);
    if (r < 0) {
      logprint(ERROR, "Failed to exit entry: %s", strerror(-r));
      return r;
    }
  }

  r = sd_bus_message_exit_container(m);
  if (r < 0) {
    logprint(ERROR, "Failed to exit container: %s", strerror(-r));
    return r;
  }

  return 0;
}

static char *dbus_helper_create_session_id(void) {
  static uint32_t session_counter = 0;
  // generate a unique session_id string
  // this is used by the client to identify the session in future calls
  // we use a static counter combined with a random number for uniqueness
  int size_needed = snprintf(NULL, 0, "%u_%u", getpid(), ++session_counter);
  char *session_id = (char *)malloc(size_needed + 1);
  if (!session_id) {
    return NULL;
  }
  snprintf(session_id, size_needed + 1, "%u_%u", getpid(), session_counter);
  return session_id;
}

static int dbus_method_CreateSession(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
  (void)userdata;
  int r;
  // IN variables
  const char *handle = NULL;          // object path for request object
  const char *session_handle = NULL;  // object path for session object
  const char *app_id = NULL;          // app id of the caller
  const char *parent_window = NULL;   // window id
  
  // options vardict variables
  uint32_t capabilities = 0;
  
  struct xdpw_session *context = NULL;
  struct xdpw_request *request = NULL;
  sd_bus_message *reply = NULL;

  // parse IN arguments
  r = sd_bus_message_read(m, "ooss", &handle, &session_handle, &app_id, &parent_window);
  if (r < 0) {
    logprint(ERROR, "Failed to parse arguments: %s", strerror(-r));
    return r;
  }

  r = dbus_helper_parse_CreateSession_options(m, &capabilities);
  if (r < 0) return r;

  // validate capabilities 
  capabilities &= interface_data.capabilities;
  if (capabilities == 0) {
    sd_bus_error_setf(ret_error, "org.freedesktop.portal.Error.NotSupported", "Requested capabilities (%u) not supported by this portal", capabilities);
    return -EOPNOTSUPP;
  }

  // create async request object
  request = xdpw_request_create(sd_bus_message_get_bus(m), handle);
  if (!request) return -ENOMEM;

  // perform logic here
  // TODO

  // generate session context
  
  char *tmp = strdup(session_handle); // do not free it here, as xdpw_session_create takes ownership
  if (!tmp) {
    xdpw_request_destroy(request);
    return -ENOMEM;
  }
  context = xdpw_session_create(interface_data.xdpw_state, sd_bus_message_get_bus(m), tmp);
  if (!context) {
    xdpw_request_destroy(request);
    return -ENOMEM;
  }
  context->input_capture_data.capabilities = capabilities;

  // // add context to global linked list
  // context->input_capture_data->next = interface_data.session_list_head;
  // interface_data.session_list_head = context;

  logprint(DEBUG, "CreateSession call: created new session: %s", session_handle);

  // construct the reply  
  r = sd_bus_message_new_method_return(m , &reply);
  if (r < 0) goto cleanup_request;

  // append response code
  r = sd_bus_message_append(reply, "u", 0U);
  if (r < 0) goto cleanup_reply;

  // append results dictionary
  r = sd_bus_message_open_container(reply, 'a', "{sv}");
  if (r < 0) goto cleanup_reply;

  r = sd_bus_message_open_container(reply, 'e', "sv");
  if (r < 0) goto cleanup_reply;
  r = sd_bus_message_append(reply, "s", "session_id");
  if (r < 0) goto cleanup_reply;
  
  char *session_id = dbus_helper_create_session_id();
  if (!session_id) goto cleanup_reply;
  r = sd_bus_message_append(reply, "v", "s", session_id);
  free(session_id);
  if (r < 0) goto cleanup_reply;
  r = sd_bus_message_close_container(reply);
  if (r < 0) goto cleanup_reply;

  r = sd_bus_message_open_container(reply, 'e', "sv");
  if (r < 0) goto cleanup_reply;
  r = sd_bus_message_append(reply, "s", "capabilities");
  if (r < 0) goto cleanup_reply;
  r = sd_bus_message_append(reply, "v", "u", context->input_capture_data.capabilities);
  if (r < 0) goto cleanup_reply;
  r = sd_bus_message_close_container(reply);
  if (r < 0) goto cleanup_reply;
  r = sd_bus_message_close_container(reply);
  if (r < 0) goto cleanup_reply;
  
  r = sd_bus_send(sd_bus_message_get_bus(m), reply, NULL);
  if (r < 0) {
    logprint(ERROR, "Failed to send CreateSession reply: %s", strerror(-r));
  }

cleanup_reply:
  sd_bus_message_unref(reply);
cleanup_request:
  xdpw_request_destroy(request);
  return r;
}

static int dbus_method_GetZones(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
  int r;
  const char *handle = NULL;
  const char *session_handle = NULL;
  const char *app_id = NULL;

  struct xdpw_session *context = NULL;
  sd_bus_message *reply = NULL;

  // get IN arguments
  r = sd_bus_message_read(m, "oos", &handle, &session_handle, &app_id);
  if (r < 0) return r;

  // skip optional IN options of type a{sv}
  r = dbus_helper_drain_dict(m);

  context = eis_helper_find_session(session_handle);
  if (!context) {
    sd_bus_error_setf(ret_error, "org.freedesktop.portal.Error.NotFound", "session_handle %s not found", session_handle);
    return -ENOENT;
  }

  logprint(DEBUG, "GetZones: Reporting zone_set %u for session %s", context->input_capture_data.zone_set_id, session_handle);

  r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0) goto cleanup;
  
  r = sd_bus_message_append(reply, "u", 0U);
  if (r < 0) goto cleanup;

  r = sd_bus_message_open_container(reply, 'a', "{sv}");
  if (r < 0) goto cleanup;
  r = sd_bus_message_open_container(reply, 'e', "sv");
  if (r < 0) goto cleanup;
  r = sd_bus_message_append(reply, "s", "zone_set");
  if (r < 0) goto cleanup;
  r = sd_bus_message_append(reply, "v", "u", context->input_capture_data.zone_set_id);
  if (r < 0) goto cleanup;
  r = sd_bus_message_close_container(reply);
  if (r < 0) goto cleanup;
  r = sd_bus_message_open_container(reply, 'e', "sv");
  if (r < 0) goto cleanup;
  r = sd_bus_message_append(reply, "s", "zones");
  if (r < 0) goto cleanup;
  r = sd_bus_message_open_container(reply, 'v', "a(uuii)");
  if (r < 0) goto cleanup;
  r = sd_bus_message_open_container(reply, 'a', "(uuii)");
  if (r < 0) goto cleanup;

  struct xdpw_input_capture_output *iter;
  wl_list_for_each(iter, &interface_data.output_list, link) {
    if (iter->ready) {
      r = sd_bus_message_append(
        reply, 
        "(uuii)",
        iter->width,
        iter->height,
        iter->x,
        iter->y
      );
      if (r < 0) {
        logprint(ERROR, "GetZones: Failed to append zone: %s", strerror(-r));
        goto cleanup;
      }
    }
  }

  r = sd_bus_message_close_container(reply);
  if (r < 0) goto cleanup;
  r = sd_bus_message_close_container(reply);
  if (r < 0) goto cleanup;
  r = sd_bus_message_close_container(reply);
  if (r < 0) goto cleanup;

  r = sd_bus_message_close_container(reply);
  if (r < 0) goto cleanup;

  r = sd_bus_send(sd_bus_message_get_bus(m), reply, NULL);

cleanup:
  sd_bus_message_unref(reply);
  return r;
}

static int parse_and_store_barriers(sd_bus_message *m, struct xdpw_session *context, struct xdpw_input_capture_barrier **out_failed_barriers_head) {
  int r;
  struct xdpw_input_capture_barrier *new_barriers_head = NULL;
  struct xdpw_input_capture_barrier *failed_barriers_head = NULL;

  r = sd_bus_message_enter_container(m, 'a', "a{sv}");
  if (r < 0) return r;

  while ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) > 0) {
    uint32_t barrier_id = 0;
    int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool pos_found = false;
    bool id_found = false;

    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
      const char *key;
      r = sd_bus_message_read(m, "s", &key);
      if (r < 0) break;

      if (strcmp(key, "barrier_id") == 0) {
        sd_bus_message_read(m, "v", "u", &barrier_id);
        id_found = true;
      } else if (strcmp(key, "position") == 0) {
        sd_bus_message_enter_container(m, 'v', "(iiii)");
        sd_bus_message_enter_container(m, 'r', "iiii");
        sd_bus_message_read(m, "i", &x1);
        sd_bus_message_read(m, "i", &y1);
        sd_bus_message_read(m, "i", &x2);
        sd_bus_message_read(m, "i", &y2);
        sd_bus_message_exit_container(m);
        sd_bus_message_exit_container(m);
        pos_found = true;
      } else {
        sd_bus_message_skip(m, "v");
      }
      sd_bus_message_exit_container(m);
    }
    if (r < 0) goto cleanup_error;

    bool failed = false;
    // must have id and position
    if (!id_found || !pos_found) failed = true;
    // id must be non zero
    if (barrier_id == 0) failed = true;
    // must be vertical or horizontal
    if (x1 != x2 && y1 != y2) failed = true;
    // must have length
    if (x1 == x2 && y1 == y2) failed = true;

    struct xdpw_input_capture_barrier *node = (struct xdpw_input_capture_barrier *)calloc(1, sizeof(struct xdpw_input_capture_barrier));
    if (!node) {
      r = -ENOMEM;
      goto cleanup_error;
    }

    node->id = barrier_id;

    if (failed) {
      // add to failed list
      node->next = failed_barriers_head;
      failed_barriers_head = node;
    } else {
      node->x1 = x1; node->y1 = y1;
      node->x2 = x2; node->y2 = y2;
      node->next = new_barriers_head;
      new_barriers_head = node;
      logprint(DEBUG, "  -> Stored valid barrier ID %u (%i, %i to %i, %i)", barrier_id, x1, y1, x2, y2);
    }
    sd_bus_message_exit_container(m);
  }
  if (r < 0) goto cleanup_error;
  r = sd_bus_message_exit_container(m);
  if (r < 0) goto cleanup_error;

  free_barrier_list(context->input_capture_data.barriers);
  context->input_capture_data.barriers = new_barriers_head;

  *out_failed_barriers_head = failed_barriers_head;

  return 0;

cleanup_error:
  free_barrier_list(new_barriers_head);
  free_barrier_list(failed_barriers_head);
  return r;
}

static int dbus_method_SetPointerBarriers(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
  int r;
  const char *handle = NULL;
  const char *session_handle = NULL;
  const char *app_id = NULL;
  uint32_t zone_set = 0;

  struct xdpw_session *context = NULL;
  struct xdpw_input_capture_barrier *failed_barriers_list = NULL;
  sd_bus_message *reply = NULL;

  // parse IN arguments
  r = sd_bus_message_read(m, "oos", &handle, &session_handle, &app_id);
  if (r < 0) {
    logprint(ERROR, "SetPointerBarriers: failed to read arguments: %s", strerror(-r));
    return r;
  }

  r = dbus_helper_drain_dict(m);
  if (r < 0) return r;

  // validate session
  context = eis_helper_find_session(session_handle);
  if (!context) {
    sd_bus_error_setf(ret_error, "org.freedesktop.portal.Error.NotFound", "Session not found: %s", session_handle);
    return -ENOENT;
  }

  r = parse_and_store_barriers(m, context, &failed_barriers_list);
  if (r < 0) {
    sd_bus_error_setf(ret_error, SD_BUS_ERROR_INVALID_ARGS, "Failed to parse barriers array: %s", strerror(-r));
    return r;
  }

  r = sd_bus_message_read(m, "u", &zone_set);
  if (r < 0) {
    free_barrier_list(failed_barriers_list);
    return r;
  }

  if (zone_set != context->input_capture_data.zone_set_id) {
    logprint(WARN, "SetPointerbarriers: zone set id mismatch (client: %u, server: %u)", zone_set, context->input_capture_data.zone_set_id);
    sd_bus_error_setf(ret_error, "org.freedesktop.portal.Error.NotFound", "Zone set id mismatch (client: %u, server: %u)", zone_set, context->input_capture_data.zone_set_id);
    
    free_barrier_list(context->input_capture_data.barriers);
    context->input_capture_data.barriers = NULL;

    free_barrier_list(failed_barriers_list);
    return -EINVAL;
  }

  logprint(DEBUG, "SetPointerBarriers: successfully updated barriers for session %s", session_handle);

  // construct reply
  // signature: ua{sv}
  r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0) {
    free_barrier_list(failed_barriers_list);
    return r;
  }

  r = sd_bus_message_append(reply, "u", 0U);
  if (r < 0) goto cleanup;

  r = sd_bus_message_open_container(reply, 'a', "{sv}");
  if (r < 0) goto cleanup;

  if (failed_barriers_list) {
    r = sd_bus_message_open_container(reply, 'e', "sv");
    if (r < 0) goto cleanup;
    r = sd_bus_message_append(reply, "s", "failed_barriers");
    if (r < 0) goto cleanup;
    r = sd_bus_message_open_container(reply, 'v', "au");
    if (r < 0) goto cleanup;
    r = sd_bus_message_open_container(reply, 'a', "u");
    if (r < 0) goto cleanup;
  
    struct xdpw_input_capture_barrier *iter = failed_barriers_list;
    while (iter) {
      sd_bus_message_append(reply, "u", iter->id);
      iter = iter->next;
    }
    r = sd_bus_message_close_container(reply);
    if (r < 0) goto cleanup;
    r = sd_bus_message_close_container(reply);
    if (r < 0) goto cleanup;
    r = sd_bus_message_close_container(reply);
    if (r < 0) goto cleanup;
  }

  r = sd_bus_message_close_container(reply); 
  if (r < 0) goto cleanup;

  r = sd_bus_send(sd_bus_message_get_bus(m), reply, NULL);

cleanup:
  sd_bus_message_unref(reply);
  free_barrier_list(failed_barriers_list);
  return r;
}

static int dbus_method_Enable(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
  int r;
  const char *session_handle = NULL;
  const char *app_id = NULL;
  sd_bus_message *reply = NULL;
  struct xdpw_session *context = NULL;

  r = sd_bus_message_read(m, "os", &session_handle, &app_id);
  if (r < 0) return r;

  r = dbus_helper_drain_dict(m);
  if (r < 0) return r;

  context = eis_helper_find_session(session_handle);
  if (!context) {
    sd_bus_error_setf(ret_error, "org.freedesktop.portal.Error.NotFound", "session_handle %s not found", session_handle);
    return -ENOENT;
  }
  
  if (context->input_capture_data.enabled) {
    logprint(DEBUG, "Session %s already enabled", session_handle);
    goto send_reply;
  }

  // exclusivity check - only one active session at a time
  if (interface_data.active_session != NULL) {
    sd_bus_error_setf(ret_error, "org.freedesktop.portal.Error.Failed", "Another input capture is already active");
    return -EBUSY;
  }

  if (!interface_data.wl_compositor || !interface_data.wl_layer_shell ||
      !interface_data.wl_seat || !interface_data.wl_pointer_constraints ||
      !interface_data.wl_keyboard_shortcuts_manager) {
    sd_bus_error_setf(ret_error, "org.freedesktop.portal.Error.NotSupported", "Compositor is missing required wayland protocols");
    return -EOPNOTSUPP;
  }

  logprint(DEBUG, "Enabling Input Capture for session %s (app: %s)", session_handle, app_id);


  if (context->input_capture_data.capabilities & 2) {
    context->input_capture_data.wl_pointer = wl_seat_get_pointer(interface_data.wl_seat);
    if (context->input_capture_data.wl_pointer) wl_pointer_add_listener(context->input_capture_data.wl_pointer, &pointer_listener, context);
  }
  if (context->input_capture_data.capabilities & 1) {
    context->input_capture_data.wl_keyboard = wl_seat_get_keyboard(interface_data.wl_seat);
    if (context->input_capture_data.wl_keyboard) wl_keyboard_add_listener(context->input_capture_data.wl_keyboard, &keyboard_listener, context);
  }
  
  context->input_capture_data.wl_surface = wl_compositor_create_surface(interface_data.wl_compositor);
  if (!context->input_capture_data.wl_surface) {
    cleanup_session_wayland(context);
    return -ENOMEM;
  }
  
  context->input_capture_data.wl_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
    interface_data.wl_layer_shell,
    context->input_capture_data.wl_surface,
    NULL, // no output, global
    ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
    "input-capture-portal"
  );
  if (!context->input_capture_data.wl_layer_surface) {
    cleanup_session_wayland(context);
    return -ENOENT;
  }

  zwlr_layer_surface_v1_add_listener(context->input_capture_data.wl_layer_surface, &layer_surface_listener, context);
  zwlr_layer_surface_v1_set_anchor(context->input_capture_data.wl_layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);  
  zwlr_layer_surface_v1_set_size(context->input_capture_data.wl_layer_surface, 0, 0);

  if (context->input_capture_data.capabilities & 1) {
    zwlr_layer_surface_v1_set_keyboard_interactivity(context->input_capture_data.wl_layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
  }
  
  wl_surface_commit(context->input_capture_data.wl_surface);

  if (context->input_capture_data.capabilities & 1) {
    context->input_capture_data.wl_keyboard_inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
      interface_data.wl_keyboard_shortcuts_manager,
      context->input_capture_data.wl_surface,
      interface_data.wl_seat
    );
    if (context->input_capture_data.wl_keyboard_inhibitor) {
      zwp_keyboard_shortcuts_inhibitor_v1_add_listener(
        context->input_capture_data.wl_keyboard_inhibitor,
        &inhibitor_listener,
        context
      );
    }
  }
  
  wl_display_roundtrip(interface_data.wl_display);
  
  context->input_capture_data.enabled = true;
  interface_data.active_session = context;

  double cursor_x = wl_fixed_to_double(context->input_capture_data.last_pointer_x);
  double cursor_y = wl_fixed_to_double(context->input_capture_data.last_pointer_y);

  dbus_signal_Activated(interface_data.bus, context, 0, cursor_x, cursor_y);

  if (context->input_capture_data.device) {
    eis_device_start_emulating(context->input_capture_data.device, context->input_capture_data.activation_id);
  }

send_reply:
  r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0) return r;

  r = sd_bus_message_append(reply, "u", 0U);
  if (r < 0) goto cleanup_reply;

  r = sd_bus_message_open_container(reply, 'a', "{sv}");
  if (r < 0) goto cleanup_reply;
  r = sd_bus_message_close_container(reply);
  if (r < 0) goto cleanup_reply;
  
  r = sd_bus_send(sd_bus_message_get_bus(m), reply, NULL);

cleanup_reply:
  sd_bus_message_unref(reply);
  return r;
}

static void cleanup_session_wayland(struct xdpw_session *context) {
  if (!context) return;

  logprint(DEBUG, "cleaning up wayland resources for session %s", (context->session_handle) ? context->session_handle : "UNKNOWN");

  if (context->input_capture_data.wl_keyboard_inhibitor) {
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(context->input_capture_data.wl_keyboard_inhibitor);
    context->input_capture_data.wl_keyboard_inhibitor = NULL;
  }
  if (context->input_capture_data.wl_locked_pointer) {
    zwp_locked_pointer_v1_destroy(context->input_capture_data.wl_locked_pointer);
    context->input_capture_data.wl_locked_pointer = NULL;
  }
  if (context->input_capture_data.wl_layer_surface) {
    zwlr_layer_surface_v1_destroy(context->input_capture_data.wl_layer_surface);
    context->input_capture_data.wl_layer_surface = NULL;
  }
  if (context->input_capture_data.wl_surface) {
    wl_surface_destroy(context->input_capture_data.wl_surface);
    context->input_capture_data.wl_surface = NULL;
  }
  if (context->input_capture_data.wl_pointer) {
    wl_pointer_destroy(context->input_capture_data.wl_pointer);
    context->input_capture_data.wl_pointer = NULL;
  }
  if (context->input_capture_data.wl_keyboard) {
    wl_keyboard_destroy(context->input_capture_data.wl_keyboard);
    context->input_capture_data.wl_keyboard = NULL;
  }
  if (context->input_capture_data.xkb_state) {
    xkb_state_unref(context->input_capture_data.xkb_state);
    context->input_capture_data.xkb_state = NULL;
  }
  if(context->input_capture_data.xkb_keymap) {
    xkb_keymap_unref(context->input_capture_data.xkb_keymap);
    context->input_capture_data.xkb_keymap = NULL;
  }
  if (interface_data.wl_display) {
    wl_display_flush(interface_data.wl_display);
  }
}

static int dbus_method_Disable(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
int r;
  const char *session_handle = NULL;
  const char *app_id = NULL;
  sd_bus_message *reply = NULL;
  struct xdpw_session *context = NULL;

  r = sd_bus_message_read(m, "os", &session_handle, &app_id);
  if (r < 0) return r;

  r = dbus_helper_drain_dict(m);
  if (r < 0) return r;

  context = eis_helper_find_session(session_handle);
  if (!context) {
    sd_bus_error_setf(ret_error, "org.freedesktop.portal.Error.NotFound", "session_handle %s not found", session_handle);
    return -ENOENT;
  }

  logprint(DEBUG, "Disabling Input Capture for session %s (app: %s)", session_handle, app_id);
  
  if (!context->input_capture_data.enabled) {
    logprint(DEBUG, "Session %s is already disabled", session_handle);
    goto send_reply;
  }

  double final_x = wl_fixed_to_double(context->input_capture_data.last_pointer_x);
  double final_y = wl_fixed_to_double(context->input_capture_data.last_pointer_y);

  cleanup_session_wayland(context);
  context->input_capture_data.enabled = false;

  if (interface_data.active_session == context) {
    interface_data.active_session = NULL;
  }

  if (context->input_capture_data.device) {
    eis_device_stop_emulating(context->input_capture_data.device);
  }

  dbus_signal_Deactivated(interface_data.bus, context, final_x, final_y);

send_reply:
  r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0) return r;

  r = sd_bus_message_append(reply, "u", 0U);
  if (r < 0) goto cleanup_reply;

  r = sd_bus_message_open_container(reply, 'a', "{sv}");
  if (r < 0) goto cleanup_reply;
  r = sd_bus_message_close_container(reply);
  if (r < 0) goto cleanup_reply;

  r = sd_bus_send(sd_bus_message_get_bus(m), reply, NULL);

cleanup_reply:
  sd_bus_message_unref(reply);
  return r;
}

static int dbus_helper_parse_Release_options(sd_bus_message *m, uint32_t *activation_id, double *cursor_position_x, double *cursor_position_y) {
  int r;

  r = sd_bus_message_enter_container(m, 'a', "{sv}");
  if (r < 0) {
    logprint(ERROR, "Error entering container: %s", strerror(-r));
    return r;
  }

  while (sd_bus_message_at_end(m, 0) == 0) {
    const char *key;

    r = sd_bus_message_enter_container(m, 'e', "sv");
    if (r < 0) {
      logprint(ERROR, "Failed to enter dict entry: %s", strerror(-r));
      return r;
    }

    r = sd_bus_message_read(m, "s", &key);
    if (r < 0) {
      logprint(ERROR, "Failed to read dict key: %s", strerror(-r));
      return r;
    }

    if (strcmp(key, "activation_id") == 0) {
      r = sd_bus_message_read(m, "v", "u", activation_id);
      if (r < 0) {
        logprint(ERROR, "Failed to read activation_id's value: %s", strerror(-r));
        return r;
      }
    } else if (strcmp(key, "cursor_position") == 0) {
      r = sd_bus_message_enter_container(m, 'v', "(dd)");
      if (r < 0) return r;
      
      r = sd_bus_message_enter_container(m, 'r', "dd");
      if (r >= 0) {
        r = sd_bus_message_read(m, "d", cursor_position_x);
        r = sd_bus_message_read(m, "d", cursor_position_y);

        sd_bus_message_exit_container(m);
        if (r < 0) return r;

      }
    } else {
      logprint(DEBUG, "Skipping unknown option: %s", key);
      sd_bus_message_skip(m, "v");
      if (r < 0) {
        logprint(ERROR, "Failed to skip variant for key '%s': %s", key, strerror(-r));
        return r;
      }
    }

    r = sd_bus_message_exit_container(m);
    if (r < 0) {
      logprint(ERROR, "Failed to exit entry: %s", strerror(-r));
      return r;
    }
  }

  r = sd_bus_message_exit_container(m);
  if (r < 0) {
    logprint(ERROR, "Failed to exit container: %s", strerror(-r));
    return r;
  }

  return 0;
}

static int dbus_method_Release(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
  int r;
  const char *session_handle = NULL;
  const char *app_id = NULL;
  uint32_t activation_id = 0;
  double cursor_position_x = 0, cursor_position_y = 0;
  sd_bus_message *reply = NULL;

  r = sd_bus_message_read(m, "os", &session_handle, &app_id);
  if (r < 0) {
    logprint(ERROR, "Error reading object path: %s", strerror(-r));
    return r;
  }

  r = dbus_helper_parse_Release_options(m, &activation_id, &cursor_position_x, &cursor_position_y);
  if (r < 0) {
    logprint(ERROR, "Error draining dictionary: %s", strerror(-r));
    return r;
  }

  struct xdpw_session *context = eis_helper_find_session(session_handle);
  if (!context) {
    sd_bus_error_setf(ret_error, "org.freedesktop.portal.Error.NotFound", "%s session_handle not found", session_handle);
    return -ENOENT;
  }

  logprint(DEBUG, "Release call with session_handle %s", session_handle);

  if (context->input_capture_data.enabled) {
    cleanup_session_wayland(context);
    context->input_capture_data.enabled = false;

    if (interface_data.active_session == context) interface_data.active_session = NULL;
  }

  if (context->input_capture_data.device) eis_device_stop_emulating(context->input_capture_data.device);

  r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0) return r;

  r = sd_bus_message_append(reply, "u", 0U);
  if (r < 0) goto cleanup_reply;

  r = sd_bus_message_open_container(reply, 'a', "{sv}");
  if (r < 0) goto cleanup_reply;
  r = sd_bus_message_close_container(reply);
  if (r < 0) goto cleanup_reply;

  r = sd_bus_send(sd_bus_message_get_bus(m), reply, NULL);

cleanup_reply:
  sd_bus_message_unref(reply);
  return r;
}

static int dbus_method_ConnectToEIS(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
  int r;
  const char *session_handle = NULL;
  const char *app_id = NULL;
  struct xdpw_session *context = NULL;
  sd_bus_message *reply = NULL;

  r = sd_bus_message_read(m, "os", &session_handle, &app_id);
  if (r < 0) return r;

  r = dbus_helper_drain_dict(m);
  if (r < 0) return r;

  context = eis_helper_find_session(session_handle);
  if (!context) {
    logprint(ERROR, "Could not find session at %s session_handle", session_handle);
    sd_bus_error_setf(ret_error, "org.freedesktop.portal.Error.NotFound", "Session handle '%s' not found", session_handle);
    return -ENOENT;
  }
  
  logprint(DEBUG, "ConnectToEIS: Setting up EIS socket for session %s (app: %s)", session_handle, app_id);

  int client_fd = eis_backend_fd_add_client(interface_data.eis_context);
  if (client_fd < 0) {
    sd_bus_error_setf(ret_error, "org.freedesktop.portal.Error.Failed", "Failed to add EIS client: %s", strerror(-client_fd));
    return -EOPNOTSUPP;
  }

  r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0) {
    close(client_fd);
    return r;
  }

  r = sd_bus_message_append(reply, "h", client_fd);
  if (r < 0) goto cleanup;

  r = sd_bus_send(sd_bus_message_get_bus(m), reply, NULL);

cleanup:
  sd_bus_message_unref(reply);
  close(client_fd);

  return r;
}

static int dbus_signal_Disabled(sd_bus *bus, const char *session_handle)
{
  int r;
  sd_bus_message *signal = NULL;

  logprint(DEBUG, "Emitting Disabled signal for session %s", session_handle);

  r = sd_bus_message_new_signal(bus, &signal, OBJECT_PATH_NAME, INPUTCAPTURE_INTERFACE_NAME, "Disabled");
  if (r < 0) {
    logprint(ERROR, "Error creating Disabled signal: %s", strerror(-r));
    return r;
  }

  r = sd_bus_message_append(signal, "o", session_handle);
  if (r < 0) goto cleanup;

  r = sd_bus_message_open_container(signal, 'a', "{sv}");
  if (r < 0) goto cleanup;
  r = sd_bus_message_close_container(signal);
  if (r < 0) goto cleanup;

  r = sd_bus_send(bus, signal, NULL);
  if (r < 0) {
    logprint(ERROR, "Failed to send Disabled signal: %s", strerror(-r));
  }
cleanup:
  sd_bus_message_unref(signal);
  return r;
}

static int dbus_signal_Activated(sd_bus *bus, struct xdpw_session *context, uint32_t barrier_id, double cursor_x, double cursor_y)
{
  int r;
  sd_bus_message *signal = NULL;

  context->input_capture_data.activation_id++;

  logprint(DEBUG, "Emitting Activated signal for session %s (ID: %u, xdpw_input_capture_barrier %u)", context->session_handle, context->input_capture_data.activation_id, barrier_id);

  r = sd_bus_message_new_signal(bus, &signal, OBJECT_PATH_NAME, INPUTCAPTURE_INTERFACE_NAME, "Activated");
  if (r < 0) {
    logprint(ERROR, "Error creating Activated signal: %s", strerror(-r));
    return r;
  }

  r = sd_bus_message_append(signal, "o", context->session_handle);
  if (r < 0) goto cleanup;

  r = sd_bus_message_open_container(signal, 'a', "{sv}");
  if (r < 0) goto cleanup;

  sd_bus_message_open_container(signal, 'e', "sv");
  sd_bus_message_append(signal, "s", "activation_id");
  sd_bus_message_open_container(signal, 'v', "u");
  sd_bus_message_append(signal, "u", context->input_capture_data.activation_id);
  sd_bus_message_close_container(signal);
  sd_bus_message_close_container(signal);

  sd_bus_message_open_container(signal, 'e', "sv");
  sd_bus_message_append(signal, "s", "cursor_position");

  sd_bus_message_open_container(signal, 'v', "(dd)");
  sd_bus_message_open_container(signal, 'r', "dd");
  sd_bus_message_append(signal, "d", cursor_x);
  sd_bus_message_append(signal, "d", cursor_y);
  sd_bus_message_close_container(signal);
  sd_bus_message_close_container(signal);
  sd_bus_message_close_container(signal);

  // only append if a valid barrier_id was passed
  if (barrier_id > 0) {
    sd_bus_message_open_container(signal, 'e', "sv");
    sd_bus_message_append(signal, "s", "barrier_id");
    sd_bus_message_open_container(signal, 'v', "u");
    sd_bus_message_append(signal, "u", barrier_id);
    sd_bus_message_close_container(signal);
    sd_bus_message_close_container(signal);
  }
  
  r = sd_bus_message_close_container(signal);
  if (r < 0) goto cleanup;

  r = sd_bus_send(bus, signal, NULL);
  if (r < 0) {
    logprint(ERROR, "Failed to send Activated signal: %s", strerror(-r));
  }
cleanup:
  sd_bus_message_unref(signal);
  return r;
}

static int dbus_signal_Deactivated(sd_bus *bus, struct xdpw_session *context, double cursor_x, double cursor_y)
{
  int r;
  sd_bus_message *signal = NULL;
  
  logprint(DEBUG, "Emitting Deactivated signal for session %s (last ID: %u)", context->session_handle, context->input_capture_data.activation_id);

  r = sd_bus_message_new_signal(bus, &signal, OBJECT_PATH_NAME, INPUTCAPTURE_INTERFACE_NAME, "Deactivated");
  if (r < 0) {
    logprint(ERROR, "Failed to create Deactivated signal: %s", strerror(-r));
    return r;
  }

  r = sd_bus_message_append(signal, "o", context->session_handle);
  if (r < 0) goto cleanup;

  r = sd_bus_message_open_container(signal, 'a', "{sv}");
  if (r < 0) goto cleanup;

  sd_bus_message_open_container(signal, 'e', "sv");
  sd_bus_message_append(signal, "s", "activation_id");
  sd_bus_message_open_container(signal, 'v', "u");
  sd_bus_message_append(signal, "u", context->input_capture_data.activation_id);
  sd_bus_message_close_container(signal);
  sd_bus_message_close_container(signal);

  sd_bus_message_open_container(signal, 'e', "sv");
  sd_bus_message_append(signal, "s", "cursor_position");

  sd_bus_message_open_container(signal, 'v', "(dd)");
  sd_bus_message_open_container(signal, 'r', "dd");
  sd_bus_message_append(signal, "d", cursor_x);
  sd_bus_message_append(signal, "d", cursor_y);
  sd_bus_message_close_container(signal);
  sd_bus_message_close_container(signal);
  sd_bus_message_close_container(signal);

  r = sd_bus_message_close_container(signal);
  if (r < 0) goto cleanup;

  r = sd_bus_send(bus, signal, NULL);
  if (r < 0) {
    logprint(ERROR, "Failed to send Deactivated signal: %s", strerror(-r));
  }

cleanup:
  sd_bus_message_unref(signal);
  return r;
}

static int dbus_signal_ZonesChanged(sd_bus *bus, const char *session_handle)
{
  int r;
  sd_bus_message *signal = NULL;

  logprint(DEBUG, "Emitting ZonesChanged signal for session %s", session_handle);

  r = sd_bus_message_new_signal(bus, &signal, OBJECT_PATH_NAME, INPUTCAPTURE_INTERFACE_NAME, "ZonesChanged");
  if (r < 0) {
    logprint(ERROR, "Failed to create ZonesChanged signal: %s", strerror(-r));
    return r;
  }

  r = sd_bus_message_append(signal, "o", session_handle);
  if (r < 0) goto cleanup;

  r = sd_bus_message_open_container(signal, 'a', "{sv}");
  if (r < 0) goto cleanup;
  r = sd_bus_message_close_container(signal);
  if (r < 0) goto cleanup;

  r = sd_bus_send(bus, signal, NULL);
  if (r < 0) {
    logprint(ERROR, "Failed to send ZonesChanged signal: %s", strerror(-r));
  }
cleanup:
  sd_bus_message_unref(signal);
  return r;
}

// static struct xdpw_input_capture_session_data* eis_helper_find_session(const char *session_path) {
//   struct xdpw_input_capture_session_data *iter = interface_data.session_list_head;
//   while (iter) {
//     if (strcmp(iter->session_handle, session_path) == 0) {
//       return iter;
//     }
//     iter = iter->next;
//   }
//   return NULL;
// }

static struct xdpw_session *eis_helper_find_session(const char *session_path) {
  struct xdpw_session *session;
  wl_list_for_each(session, &(interface_data.xdpw_state->xdpw_sessions), link) {
    if (strcmp(session->session_handle, session_path) == 0) return session;
  }
  return NULL;
}

static void eis_helper_handle_event(struct eis_event *event) {
  logprint(DEBUG, "EIS Event: %s", eis_event_type_to_string(eis_event_get_type(event)));

  switch (eis_event_get_type(event)) {
    case EIS_EVENT_CLIENT_CONNECT: {
      struct eis_client *client = eis_event_get_client(event);
      logprint(DEBUG, "New EIS client connected");
      eis_client_connect(client);
      break;
    }
    case EIS_EVENT_CLIENT_DISCONNECT: {
      struct eis_client *client = eis_event_get_client(event);
      struct xdpw_session *context = (struct xdpw_session *)eis_client_get_user_data(client);
      if (context) {
        context->input_capture_data.device = NULL;
        logprint(DEBUG, "EIS client disconnected (session_path: %s)", context->session_handle);
      }
      break;
    }
    case EIS_EVENT_SEAT_BIND: {
      struct eis_seat *seat = eis_event_get_seat(event);
      struct eis_client *client = eis_seat_get_client(seat);

      const char *seat_name = eis_seat_get_name(seat);
      logprint(DEBUG, "EIS client bound seat: %s", seat_name);

      struct xdpw_session *context = eis_helper_find_session(seat_name);
      if (!context) {
        logprint(ERROR, "EIS Error: unknown session_path used as seat name: %s", seat_name);
        eis_client_disconnect(client);
        return;
      }

      logprint(DEBUG, "Linking EIS client to session %s", context->session_handle);
      eis_client_set_user_data(client, context);

      if (context->input_capture_data.capabilities & 1) eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
      if (context->input_capture_data.capabilities & 2) eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER);
      if (context->input_capture_data.capabilities & 4) eis_seat_configure_capability(seat, EIS_DEVICE_CAP_TOUCH);
      eis_seat_add(seat);

      logprint(DEBUG, "Creating new virtual device for session %s", context->session_handle);

      struct eis_device *device = eis_seat_new_device(seat);
      eis_device_configure_name(device, "Portal Virtual Input");
      eis_device_configure_type(device, EIS_DEVICE_TYPE_VIRTUAL);
      
      if (context->input_capture_data.capabilities & 1) eis_device_configure_capability(device, EIS_DEVICE_CAP_KEYBOARD);
      if (context->input_capture_data.capabilities & 2) eis_device_configure_capability(device, EIS_DEVICE_CAP_POINTER);
      if (context->input_capture_data.capabilities & 4) eis_device_configure_capability(device, EIS_DEVICE_CAP_TOUCH);
      
      eis_device_add(device);
      eis_device_resume(device);

      context->input_capture_data.device = device;
      break;
    }
    default: 
      logprint(TRACE, "EIS event not handled: %s", eis_event_type_to_string(eis_event_get_type(event)));
      break;
  }
}

static void wayland_registry_global(void *data, struct wl_registry *registry,
                                    uint32_t name, const char *interface, uint32_t version) {
  struct xdpw_input_capture_data *d = (struct xdpw_input_capture_data *)data;

  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    d->wl_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    logprint(DEBUG, "Wayland: bound wl_compositor");
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    d->wl_layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    logprint(DEBUG, "Wayland: bound zwlr_layer_shell_v1");
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    d->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
    wl_seat_add_listener(d->wl_seat, &seat_listener, d);
    logprint(DEBUG, "Wayland: bound wl_seat");
  } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
    d->wl_pointer_constraints = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
    logprint(DEBUG, "Wayland: boudn zwp_pointer_constraints_v1");
  } else if (strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0) {
    d->wl_keyboard_shortcuts_manager = wl_registry_bind(registry, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    logprint(DEBUG, "Wayland: bound zwp_keyboard_shortcuts_inhibit_manager_v1");
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    struct xdpw_input_capture_output *output = calloc(1, sizeof(struct xdpw_input_capture_output));
    if (!output) return;
    output->data = d;
    output->name = name;
    output->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 3);
    wl_output_add_listener(output->wl_output, &output_listener, output);
    wl_list_insert(&d->output_list, &output->link);
    logprint(DEBUG, "Wayland: bound wl_output %u", name);
  }
}

static void wayland_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
  struct xdpw_input_capture_data *d = (struct xdpw_input_capture_data *)data;
  struct xdpw_input_capture_output *iter, *tmp;

  wl_list_for_each_safe(iter, tmp, &d->output_list, link) {
    if (iter->name == name) {
      wl_list_remove(&iter->link);
      wl_output_destroy(iter->wl_output);
      free(iter);
      logprint(DEBUG, "Wayland: output %u removed", name);
      return;
    }
  }

}

static void wayland_handle_seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
  logprint(DEBUG, "Wayland: Seat capabilities changed");
}

static void wayland_handle_seat_name(void *data, struct wl_seat *seat, const char *name) {
  logprint(DEBUG, "Wayland: Seat name changed");
}

static void wayland_handle_inhibitor_active(void *data, struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor) {
  struct xdpw_session *context = (struct xdpw_session *)data;
  logprint(DEBUG, "Wayland: Keyboard inhibitor ACTIVE for session %s", context->session_handle);

  if (interface_data.active_session == context) {
    double cursor_x = wl_fixed_to_double(context->input_capture_data.last_pointer_x);
    double cursor_y = wl_fixed_to_double(context->input_capture_data.last_pointer_y);

    dbus_signal_Activated(interface_data.bus, context, 0, cursor_x, cursor_y);

    if (context->input_capture_data.device) {
      eis_device_start_emulating(context->input_capture_data.device, context->input_capture_data.activation_id);
    }
  }
}

static void wayland_handle_inhibitor_inactive(void *data, struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor) {
  struct xdpw_session *context = (struct xdpw_session *)data;
  logprint(DEBUG, "Wayland: keyboard inhibitor INACTIVE for session %s", context->session_handle);
  
  if (context->input_capture_data.enabled) {
    double final_x = wl_fixed_to_double(context->input_capture_data.last_pointer_x);
    double final_y = wl_fixed_to_double(context->input_capture_data.last_pointer_y);

    if (context->input_capture_data.device) {
      eis_device_stop_emulating(context->input_capture_data.device);
    }
    
    dbus_signal_Deactivated(interface_data.bus, context, final_x, final_y);
  }
}

static void wayland_handle_layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface, uint32_t serial, uint32_t w, uint32_t h) {
  struct xdpw_session *context = (struct xdpw_session *)data;
  logprint(DEBUG, "Wayland: layer surface configured: %ux%u", w, h);
  zwlr_layer_surface_v1_ack_configure(surface, serial);
  wl_surface_commit(context->input_capture_data.wl_surface);
}

static void wayland_handle_layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
  struct xdpw_session *context = (struct xdpw_session *)data;
  logprint(WARN, "Wayland: Layer surface closed unexpectedly! Disabling session ");
  cleanup_session_wayland(context);
  context->input_capture_data.enabled = false;
  interface_data.active_session = NULL;
  // dbus_signal_Deactivated(interface_data.bus, context->session_handle);
}

static void wayland_handle_pointer_enter(void *data, struct wl_pointer *ptr, uint32_t serial,
                                        struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
  struct xdpw_session *context = (struct xdpw_session *)data;
  logprint(DEBUG, "Wayland pointer entered surface");

  if (interface_data.wl_pointer_constraints && !context->input_capture_data.wl_locked_pointer) {
    context->input_capture_data.wl_locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
      interface_data.wl_pointer_constraints,
      surface,
      context->input_capture_data.wl_pointer,
      NULL,
      ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT
    );
    zwp_locked_pointer_v1_add_listener(context->input_capture_data.wl_locked_pointer, &locked_pointer_listener, context);
  }

  // store initial position for delta calculation
  context->input_capture_data.last_pointer_x = sx;
  context->input_capture_data.last_pointer_y = sy;
}

static void wayland_handle_pointer_leave(void *data, struct wl_pointer *ptr, uint32_t serial, struct wl_surface *surface) {
  logprint(DEBUG, "Wayland: pointer left surface");
}

static void wayland_handle_pointer_motion(void *data, struct wl_pointer *ptr, uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
  struct xdpw_session *context = (struct xdpw_session *)data;
  if (!context->input_capture_data.device) return;

  logprint(DEBUG, "POINTER MOTION");

  // calculate delta from last position
  wl_fixed_t dx = sx - context->input_capture_data.last_pointer_x;
  wl_fixed_t dy = sy - context->input_capture_data.last_pointer_y;

  eis_device_pointer_motion(context->input_capture_data.device, wl_fixed_to_double(dx), wl_fixed_to_double(dy));

  context->input_capture_data.last_pointer_x = sx;
  context->input_capture_data.last_pointer_y = sy;
}

static void wayland_handle_pointer_button(void *data, struct wl_pointer *ptr, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
  struct xdpw_session *context = (struct xdpw_session *)data;
  if (!context->input_capture_data.device) return;

  eis_device_button_button(context->input_capture_data.device, button, (state == WL_POINTER_BUTTON_STATE_PRESSED));
}

static void wayland_handle_pointer_axis(void *data, struct wl_pointer *ptr, uint32_t time, uint32_t axis, wl_fixed_t value) {
  struct xdpw_session *context = (struct xdpw_session *)data;
  if (!context->input_capture_data.device) return;

  double dx = 0.0;
  double dy = 0.0;

  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
    dy = wl_fixed_to_double(value);
  } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
    dx = wl_fixed_to_double(value);
  } else {
    return;
  }
  eis_device_scroll_delta(context->input_capture_data.device, dx, dy);
}

static void wayland_handle_keyboard_keymap(void *data, struct wl_keyboard *kbd, uint32_t format, int32_t fd, uint32_t size) {
  struct xdpw_session *context = (struct xdpw_session *)data;
  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    close(fd);
    return;
  }
  logprint(DEBUG, "Wayland: Got keymap (fd: %d, size %u)", fd, size);

  int eis_fd = -1;
  if (context->input_capture_data.device) {
    eis_fd = dup(fd);
    if (eis_fd < 0) {
      logprint(ERROR, "Wayland: failed to dup keymap fd: %s", strerror(errno));
    }
  }

  char *map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  if (map_shm == MAP_FAILED) {
    logprint(ERROR, "Wayland: mmap failed for keymap: %s", strerror(errno));
    close(fd);
    if (eis_fd >= 0) close(eis_fd);
    return;
  }

  if (context->input_capture_data.xkb_keymap) xkb_keymap_unref(context->input_capture_data.xkb_keymap);
  if (context->input_capture_data.xkb_state) xkb_state_unref(context->input_capture_data.xkb_state);

  context->input_capture_data.xkb_keymap = xkb_keymap_new_from_string(
    interface_data.xkb_context,
    map_shm,
    XKB_KEYMAP_FORMAT_TEXT_V1,
    XKB_KEYMAP_COMPILE_NO_FLAGS
  );
  munmap(map_shm, size);
  close(fd);

  if (!context->input_capture_data.xkb_keymap) {
    logprint(ERROR, "Wayland: failed to create xkb_keymap from string");
    if (eis_fd >= 0) close(eis_fd);
    return;
  }

  context->input_capture_data.xkb_state = xkb_state_new(context->input_capture_data.xkb_keymap);
  if (!context->input_capture_data.xkb_state) {
    logprint(ERROR, "Wayland: failed to create xkb_state");
  }

  if (context->input_capture_data.device && eis_fd >= 0) {
    logprint(DEBUG, "EIS: forwarding keymap fd %d", eis_fd);
    struct eis_keymap *keymap = eis_device_new_keymap(context->input_capture_data.device, EIS_KEYMAP_TYPE_XKB, eis_fd, size);
    if (keymap) {
      eis_keymap_add(keymap);
      eis_keymap_unref(keymap);
    } else {
      logprint(ERROR, "EIS: failed to create new keymap");
      close(eis_fd);
    }
  } else if (eis_fd >= 0) {
    close(eis_fd);
  }
}

static void wayland_handle_keyboard_enter(void *data, struct wl_keyboard *kbd, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
  logprint(DEBUG, "Wayland: keyboard focus acquired");
}

static void wayland_handle_keyboard_leave(void *data, struct wl_keyboard *kbd, uint32_t serial, struct wl_surface *surface) {
  logprint(DEBUG, "Wayland: keyboard focus lost");
}

static void wayland_handle_keyboard_key(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
  struct xdpw_session *context = (struct xdpw_session *)data;
  if (!context->input_capture_data.device) return;
  eis_device_keyboard_key(context->input_capture_data.device, key, (state == WL_KEYBOARD_KEY_STATE_PRESSED));
}

static void wayland_handle_keyboard_modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
  struct xdpw_session *context = (struct xdpw_session *)data;
  if (!context->input_capture_data.device) return;
  if (context->input_capture_data.xkb_state) {
    xkb_state_update_mask(context->input_capture_data.xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
  }
  eis_device_keyboard_send_xkb_modifiers(context->input_capture_data.device, mods_depressed, mods_latched, mods_locked, group);
}

static void wayland_handle_keyboard_repeat(void *data, struct wl_keyboard *kbd, int32_t, int32_t) {
  // wayland handles repeats by sending multiple .key events
}

static void wayland_handle_locked_pointer_locked(void *data, struct zwp_locked_pointer_v1 *locked_pointer) {
  logprint(DEBUG, "Wayland: Pointer locked");
}
static void wayland_handle_locked_pointer_unlocked(void *data, struct zwp_locked_pointer_v1 *locked_pointer) {
  logprint(DEBUG, "Wayland: Pointer unlocked");
}

static void output_handle_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t phys_w, int32_t phys_h, int32_t subpixel, const char *make, const char *model, int32_t transform) {
  struct xdpw_input_capture_output *output = (struct xdpw_input_capture_output *)data;
  output->x = x;
  output->y = y;
}

static void output_handle_mode(void * data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
  struct xdpw_input_capture_output *output = (struct xdpw_input_capture_output *)data;
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    output->width = width;
    output->height = height;
  }
}

// static void output_handle_done(void *data, struct wl_output *wl_output) {
//   struct xdpw_input_capture_output *output = (struct xdpw_input_capture_output *)data;
//   output->ready = true;
//   logprint(DEBUG, "Wayland: output %u is ready (%dx%d @ %d,%d)", output->name, output->width, output->height, output->x, output->y);

//   struct xdpw_input_capture_session_data *iter = output->data->session_list_head;
//   while (iter) {
//     if (iter->enabled) {
//       // invalidate the clients current zone_set_id and notify them
//       // the spec says to increment by a "sensible amount"
//       if (iter->zone_set_id == 0) iter->zone_set_id = 1;
//       else iter->zone_set_id++;
//       dbus_signal_ZonesChanged(output->data->bus, iter->session_handle);
//     }
//     iter = iter->next;
//   }
// }

static void output_handle_done(void *data, struct wl_output *wl_output) {
  struct xdpw_input_capture_output *output = (struct xdpw_input_capture_output *)data;
  
  output->ready = true;
  logprint(DEBUG, "Wayland: output %u is ready (%dx%d @ %d,%d)", output->name, output->width, output->height, output->x, output->y);

  struct xdpw_session *iter = NULL;
  wl_list_for_each(iter, &(interface_data.xdpw_state->xdpw_sessions), link) {
    struct xdpw_input_capture_session_data *ic_data = &(iter->input_capture_data);

    if (ic_data->enabled) {
      if (ic_data->zone_set_id == 0) ic_data->zone_set_id = 1;
      else ic_data->zone_set_id++;
      dbus_signal_ZonesChanged(interface_data.bus, iter->session_handle);
    }
  }
}

static void output_handle_scale(void *data, struct wl_output *wl_output, int32_t factor) {
  // Not needed for this portal
}

/* --- public api functions --- */
int xdpw_input_capture_init(struct xdpw_state *state) {
  int r;
  struct eis *eis_context = NULL;

  wl_list_init(&interface_data.output_list);

  // use the existing bus and display from the main state
  interface_data.xdpw_state = state;
  interface_data.bus = state->bus;
  interface_data.wl_display = state->wl_display;

  interface_data.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!interface_data.xkb_context) {
    logprint(ERROR, "Failed to create xkb_context");
    r = -EIO;
    goto cleanup_wayland;
  }

  interface_data.wl_registry = wl_display_get_registry(interface_data.wl_display);
  wl_registry_add_listener(interface_data.wl_registry, &registry_listener, &interface_data);
  wl_display_roundtrip(interface_data.wl_display);

  // check if we got essential globals
  if ( !interface_data.wl_compositor || !interface_data.wl_layer_shell ||
    !interface_data.wl_seat || !interface_data.wl_pointer_constraints ||
    !interface_data.wl_keyboard_shortcuts_manager ) {
    logprint(ERROR, "Compositor is missing required wayland protocols:");
    if (!interface_data.wl_compositor) logprint(ERROR, "  - wl_compositor is missing");
    if (!interface_data.wl_layer_shell) logprint(ERROR, " - zwlr_layer_shell_v1 is missing (are you on sway/hyprland?)");
    if (!interface_data.wl_seat) logprint(ERROR, "  - wl_seat is missing");
    if (!interface_data.wl_pointer_constraints) logprint(ERROR, " - zwp_pointer_constraints_v1 is missing");
    if (!interface_data.wl_keyboard_shortcuts_manager) logprint(ERROR, "  - zwp_keyboard_shortcuts_inhibit_manager_v1 is missing");
    r = -EPROTONOSUPPORT;
    goto cleanup_wayland;
  }

  eis_context = eis_new(&interface_data);
  if (!eis_context) {
    logprint(ERROR, "Failed to create EIS context");
    r = -ENOMEM;
    goto cleanup_wayland;
  }

  r = eis_setup_backend_fd(eis_context);
  if (r < 0) {
    logprint(ERROR, "Failed to setup EIS FD backend", strerror(-r));
    goto cleanup_eis;
  }

  logprint(INFO, "Eis server listening");

  int eis_fd = eis_get_fd(eis_context);
  if (eis_fd < 0) {
    logprint(ERROR, "Failed to get EIS fd, got: %d", eis_fd);
    r = EINVAL;
    goto cleanup_eis;
  }

  r = sd_bus_add_object_vtable(
    interface_data.bus,
    NULL,
    OBJECT_PATH_NAME,
    INPUTCAPTURE_INTERFACE_NAME,
    input_capture_vtable,
    &interface_data
  );

  if (r < 0) {
    logprint(ERROR, "Failed to add D-BUS vtable: %s", strerror(-r));
    goto cleanup_eis;
  }

	interface_data.eis_context = eis_context;
  state->input_capture.libei_fd = eis_fd;

  return 0;
cleanup_eis:
  eis_unref(eis_context);
cleanup_wayland:
  if (interface_data.xkb_context) xkb_context_unref(interface_data.xkb_context);
  if (interface_data.wl_seat) wl_seat_destroy(interface_data.wl_seat);
  if (interface_data.wl_pointer_constraints) zwp_pointer_constraints_v1_destroy(interface_data.wl_pointer_constraints);
  if (interface_data.wl_keyboard_shortcuts_manager) zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(interface_data.wl_keyboard_shortcuts_manager);
  if (interface_data.wl_layer_shell) zwlr_layer_shell_v1_destroy(interface_data.wl_layer_shell);
  if (interface_data.wl_compositor) wl_compositor_destroy(interface_data.wl_compositor);
  if (interface_data.wl_registry) wl_registry_destroy(interface_data.wl_registry);

  return r;
}

void xdpw_input_capture_destroy(void) {
  if (!interface_data.bus) return;

  logprint(DEBUG, "InputCapture portal shutting down");

  struct xdpw_input_capture_output *iter, *tmp;
  wl_list_for_each_safe(iter, tmp, &interface_data.output_list, link) {
    wl_list_remove(&iter->link);
    wl_output_destroy(iter->wl_output);
    free(iter);
  }

  if (interface_data.eis_context) eis_unref(interface_data.eis_context);
  if (interface_data.xkb_context) xkb_context_unref(interface_data.xkb_context);
  if (interface_data.wl_seat) wl_seat_destroy(interface_data.wl_seat);
  if (interface_data.wl_pointer_constraints) zwp_pointer_constraints_v1_destroy(interface_data.wl_pointer_constraints);
  if (interface_data.wl_keyboard_shortcuts_manager) zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(interface_data.wl_keyboard_shortcuts_manager);
  if (interface_data.wl_layer_shell) zwlr_layer_shell_v1_destroy(interface_data.wl_layer_shell);
  if (interface_data.wl_compositor) wl_compositor_destroy(interface_data.wl_compositor);
  if (interface_data.wl_registry) wl_registry_destroy(interface_data.wl_registry);
}

void xdpw_input_capture_dispatch_eis(struct xdpw_state *state) {
  struct eis *eis = interface_data.eis_context;
  if (!eis) return;

  eis_dispatch(eis);
  struct eis_event *event;
  while ((event = eis_get_event(eis))) {
    eis_helper_handle_event(event);
    eis_event_unref(event);
  }
}
