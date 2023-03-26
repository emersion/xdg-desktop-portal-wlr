#include "include/global_shortcuts.h"

#include <string.h>

#include "include/xdpw.h"

static void wlr_registry_handle_add(void *data, struct wl_registry *reg, uint32_t id, const char *interface, uint32_t ver) {
    struct globalShortcutsInstance *instance = data;

    if (!strcmp(interface, hyprland_global_shortcuts_manager_v1_interface.name) && !instance->manager) {
        uint32_t version = ver;

        logprint(DEBUG, "hyprland: |-- registered to interface %s (Version %u)", interface, version);

        instance->manager = wl_registry_bind(reg, id, &hyprland_global_shortcuts_manager_v1_interface, version);
    }
}

static void wlr_registry_handle_remove(void *data, struct wl_registry *reg, uint32_t id) {
    ;  // ignored
}

static const struct wl_registry_listener wlr_registry_listener = {
    .global = wlr_registry_handle_add,
    .global_remove = wlr_registry_handle_remove,
};

static const char object_path[] = "/org/freedesktop/portal/desktop";
static const char interface_name[] = "org.freedesktop.impl.portal.GlobalShortcuts";

static const sd_bus_vtable gs_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}", method_gs_create_session, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("BindShortcuts", "ooa(sa{sv})sa{sv}", "ua{sv}", method_gs_bind_shortcuts, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ListShortcuts", "oo", "ua{sv}", method_gs_list_shortcuts, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Activated", "osta{sv}", NULL),
    SD_BUS_SIGNAL("Deactivated", "osta{sv}", NULL),
    SD_BUS_SIGNAL("ShortcutsChanged", "oa(sa{sv})", NULL),
    SD_BUS_VTABLE_END};

static void handleActivated(void *data, struct hyprland_global_shortcut_v1 *hyprland_global_shortcut_v1, uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                            uint32_t tv_nsec) {
    struct xdpw_state *state = data;

    struct globalShortcut *curr;
    struct globalShortcutClient *currc;
    wl_list_for_each(currc, &state->shortcutsInstance.shortcutClients, link) {
        wl_list_for_each(curr, &currc->shortcuts, link) {
            if (curr->hlShortcut == hyprland_global_shortcut_v1) {
                goto found;
            }
        }
    }
found:

    sd_bus_emit_signal(state->bus, object_path, interface_name, "Activated", "osta{sv}", currc->session->session_handle, curr->name,
                       ((uint64_t)tv_sec_hi << 32) | (uint64_t)(tv_sec_lo), 0);
}

static void handleDeactivated(void *data, struct hyprland_global_shortcut_v1 *hyprland_global_shortcut_v1, uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                              uint32_t tv_nsec) {
    struct xdpw_state *state = data;

    struct globalShortcut *curr;
    struct globalShortcutClient *currc;
    wl_list_for_each(currc, &state->shortcutsInstance.shortcutClients, link) {
        wl_list_for_each(curr, &currc->shortcuts, link) {
            if (curr->hlShortcut == hyprland_global_shortcut_v1) {
                goto found;
            }
        }
    }
found:

    sd_bus_emit_signal(state->bus, object_path, interface_name, "Deactivated", "osta{sv}", currc->session->session_handle, curr->name,
                       ((uint64_t)tv_sec_hi << 32) | (uint64_t)(tv_sec_lo), 0);
}

static const struct hyprland_global_shortcut_v1_listener shortcutListener = {
    .pressed = handleActivated,
    .released = handleDeactivated,
};

static int method_gs_create_session(sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
    struct xdpw_state *state = data;
    struct globalShortcutsClient *client = calloc(1, sizeof(struct globalShortcutsClient));
    wl_list_insert(&state->shortcutsInstance.shortcutClients, &client->link);
    wl_list_init(&client->shortcuts);

    int ret = 0;

    logprint(INFO, "dbus: create session method invoked");

    char *request_handle, *session_handle, *app_id;
    ret = sd_bus_message_read(msg, "oos", &request_handle, &session_handle, &app_id);
    if (ret < 0) {
        return ret;
    }

    ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
    if (ret < 0) {
        return ret;
    }

    logprint(INFO, "dbus: request_handle: %s", request_handle);
    logprint(INFO, "dbus: session_handle: %s", session_handle);
    logprint(INFO, "dbus: app_id: %s", app_id);

    char *key;
    int innerRet = 0;
    while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
        innerRet = sd_bus_message_read(msg, "s", &key);
        if (innerRet < 0) {
            return innerRet;
        }

        if (strcmp(key, "session_handle_token") == 0) {
            char *token;
            sd_bus_message_read(msg, "v", "s", &token);
            logprint(INFO, "dbus: option token: %s", token);
        } else if (strcmp(key, "shortcuts") == 0) {
            // init shortcuts
            client->sentShortcuts = true;

            innerRet = sd_bus_message_enter_container(msg, 'v', "a(sa{sv})");
            if (innerRet < 0) {
                return innerRet;
            }

            innerRet = sd_bus_message_enter_container(msg, 'a', "(sa{sv})");

            while (innerRet > 0) {
                char type;
                char *container;
                sd_bus_message_peek_type(msg, &type, &container);

                if (type != 'r') break;

                innerRet = sd_bus_message_enter_container(msg, 'r', "sa{sv}");
                if (innerRet == -ENXIO) break;

                sd_bus_message_peek_type(msg, &type, &container);

                innerRet = sd_bus_message_read(msg, "s", &key);

                if (innerRet == -ENXIO) break;

                if (innerRet < 0) {
                    return innerRet;
                }

                logprint(DEBUG, "shortcut name %s", key);

                struct globalShortcut *shortcut = calloc(1, sizeof(struct globalShortcut));
                shortcut->name = malloc(strlen(key) + 1);
                strcpy(shortcut->name, key);
                shortcut->description = calloc(1, 1);  // todo
                wl_list_insert(&client->shortcuts, &shortcut->link);

                // sd_bus_message_enter_container(msg, 'e', "sv");
                // sd_bus_message_exit_container(msg);
                sd_bus_message_skip(msg, "a{sv}");
                sd_bus_message_exit_container(msg);
            }

            innerRet = sd_bus_message_exit_container(msg);
            innerRet = sd_bus_message_exit_container(msg);
            if (innerRet < 0) {
                return innerRet;
            }

        } else {
            logprint(WARN, "dbus: unknown option: %s", key);
            sd_bus_message_skip(msg, "v");
        }

        innerRet = sd_bus_message_exit_container(msg);
        if (innerRet < 0) {
            return innerRet;
        }
    }
    if (ret < 0) {
        return ret;
    }

    ret = sd_bus_message_exit_container(msg);
    if (ret < 0) {
        return ret;
    }

    struct xdpw_request *req = xdpw_request_create(sd_bus_message_get_bus(msg), request_handle);
    if (req == NULL) {
        return -ENOMEM;
    }

    struct xdpw_session *sess = xdpw_session_create(state, sd_bus_message_get_bus(msg), strdup(session_handle));
    if (sess == NULL) {
        return -ENOMEM;
    }

    sess->app_id = malloc(strlen(app_id) + 1);
    strcpy(sess->app_id, app_id);

    sd_bus_message *reply = NULL;
    ret = sd_bus_message_new_method_return(msg, &reply);
    if (ret < 0) {
        return ret;
    }
    ret = sd_bus_message_append(reply, "u", PORTAL_RESPONSE_SUCCESS, 0);
    if (ret < 0) {
        return ret;
    }
    ret = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (ret < 0) {
        return ret;
    }
    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append(reply, "s", "shortcuts");
    sd_bus_message_open_container(reply, 'v', "a(sa{sv})");
    sd_bus_message_open_container(reply, 'a', "(sa{sv})");
    struct globalShortcut *curr;
    wl_list_for_each(curr, &client->shortcuts, link) {
        sd_bus_message_append(reply, "(sa{sv})", curr->name, 1, "description", "s", curr->description);
    }

    ret = sd_bus_message_close_container(reply);
    ret = sd_bus_message_close_container(reply);
    ret = sd_bus_message_close_container(reply);
    ret = sd_bus_message_close_container(reply);
    if (ret < 0) {
        return ret;
    }
    ret = sd_bus_send(NULL, reply, NULL);
    if (ret < 0) {
        return ret;
    }

    sd_bus_message_unref(reply);
    return 0;
}

static int method_gs_bind_shortcuts(sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
    struct xdpw_state *state = data;

    int ret = 0;

    logprint(INFO, "dbus: bind shortcuts invoked");

    char *request_handle, *session_handle;
    ret = sd_bus_message_read(msg, "oo", &request_handle, &session_handle);
    if (ret < 0) {
        return ret;
    }

    logprint(INFO, "dbus: request_handle: %s", request_handle);
    logprint(INFO, "dbus: session_handle: %s", session_handle);

    struct xdpw_session *session, *tmp_session;
    wl_list_for_each_reverse_safe(session, tmp_session, &state->xdpw_sessions, link) {
        if (strcmp(session->session_handle, session_handle) == 0) {
            logprint(DEBUG, "dbus: bind shortcuts: found matching session %s", session->session_handle);
            break;
        }
    }

    struct globalShortcutsClient *client, *tmp_client;
    wl_list_for_each_reverse_safe(client, tmp_client, &state->shortcutsInstance.shortcutClients, link) {
        if (strcmp(client->session, session_handle) == 0) {
            logprint(DEBUG, "dbus: bind shortcuts: found matching client %s", client->session_handle);
            break;
        }
    }

    if (!client->sentShortcuts) {
        char *key;
        int innerRet = 0;
        client->sentShortcuts = true;

        innerRet = sd_bus_message_enter_container(msg, 'a', "(sa{sv})");

        while (innerRet > 0) {
            char type;
            char *container;
            sd_bus_message_peek_type(msg, &type, &container);

            if (type != 'r') break;

            innerRet = sd_bus_message_enter_container(msg, 'r', "sa{sv}");
            if (innerRet == -ENXIO) break;

            sd_bus_message_peek_type(msg, &type, &container);

            innerRet = sd_bus_message_read(msg, "s", &key);

            if (innerRet == -ENXIO) break;

            if (innerRet < 0) {
                return innerRet;
            }

            logprint(DEBUG, "shortcut name %s", key);

            struct globalShortcut *shortcut = calloc(1, sizeof(struct globalShortcut));
            shortcut->name = malloc(strlen(key) + 1);
            strcpy(shortcut->name, key);
            shortcut->description = calloc(1, 1);  // todo
            wl_list_insert(&client->shortcuts, &shortcut->link);

            // sd_bus_message_enter_container(msg, 'e', "sv");
            // sd_bus_message_exit_container(msg);
            sd_bus_message_skip(msg, "a{sv}");
            sd_bus_message_exit_container(msg);
        }

        innerRet = sd_bus_message_exit_container(msg);
        if (innerRet < 0) {
            return innerRet;
        }
    }

    ret = sd_bus_message_exit_container(msg);
    if (ret < 0) {
        return ret;
    }

    char *parent_window;
    ret = sd_bus_message_read(msg, "s", &parent_window);

    logprint(DEBUG, "dbus: parent_window %s", parent_window);

    client->parent_window = malloc(strlen(parent_window) + 1);
    strcpy(client->parent_window, parent_window);

    sd_bus_message *reply = NULL;
    ret = sd_bus_message_new_method_return(msg, &reply);
    if (ret < 0) {
        return ret;
    }
    ret = sd_bus_message_append(reply, "u", PORTAL_RESPONSE_SUCCESS, 0);
    if (ret < 0) {
        return ret;
    }
    ret = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (ret < 0) {
        return ret;
    }
    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append(reply, "s", "shortcuts");
    sd_bus_message_open_container(reply, 'v', "a(sa{sv})");
    sd_bus_message_open_container(reply, 'a', "(sa{sv})");
    struct globalShortcut *curr;
    wl_list_for_each(curr, &client->shortcuts, link) {
        sd_bus_message_append(reply, "(sa{sv})", curr->name, 1, "description", "s", curr->description);
        curr->hlShortcut = hyprland_global_shortcuts_manager_v1_register_shortcut(state->shortcutsInstance.manager, curr->name, client->parent_window,
                                                                                  curr->description, "");
        hyprland_global_shortcut_v1_add_listener(curr->hlShortcut, &shortcutListener, state);
    }

    ret = sd_bus_message_close_container(reply);
    ret = sd_bus_message_close_container(reply);
    ret = sd_bus_message_close_container(reply);
    ret = sd_bus_message_close_container(reply);
    if (ret < 0) {
        return ret;
    }
    ret = sd_bus_send(NULL, reply, NULL);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

static int method_gs_list_shortcuts(sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
    struct xdpw_state *state = data;

    int ret = 0;

    logprint(INFO, "dbus: list shortcuts invoked");

    char *request_handle, *session_handle;
    ret = sd_bus_message_read(msg, "oo", &request_handle, &session_handle);
    if (ret < 0) {
        return ret;
    }

    logprint(INFO, "dbus: request_handle: %s", request_handle);
    logprint(INFO, "dbus: session_handle: %s", session_handle);

    struct globalShortcutsClient *client, *tmp_client;
    wl_list_for_each_reverse_safe(client, tmp_client, &state->shortcutsInstance.shortcutClients, link) {
        if (strcmp(client->session, session_handle) == 0) {
            logprint(DEBUG, "dbus: bind shortcuts: found matching client %s", client->session_handle);
            break;
        }
    }

    sd_bus_message *reply = NULL;
    ret = sd_bus_message_new_method_return(msg, &reply);
    if (ret < 0) {
        return ret;
    }
    ret = sd_bus_message_append(reply, "u", PORTAL_RESPONSE_SUCCESS, 0);
    if (ret < 0) {
        return ret;
    }
    ret = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (ret < 0) {
        return ret;
    }
    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append(reply, "s", "shortcuts");
    sd_bus_message_open_container(reply, 'v', "a(sa{sv})");
    sd_bus_message_open_container(reply, 'a', "(sa{sv})");
    struct globalShortcut *curr;
    wl_list_for_each(curr, &client->shortcuts, link) {
        sd_bus_message_append(reply, "(sa{sv})", curr->name, 1, "description", "s", curr->description);
    }

    ret = sd_bus_message_close_container(reply);
    ret = sd_bus_message_close_container(reply);
    ret = sd_bus_message_close_container(reply);
    ret = sd_bus_message_close_container(reply);
    if (ret < 0) {
        return ret;
    }
    ret = sd_bus_send(NULL, reply, NULL);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

void initShortcutsInstance(struct xdpw_state *state, struct globalShortcutsInstance *instance) {
    instance->manager = NULL;
    wl_list_init(&instance->shortcutClients);

    struct wl_registry *registry = wl_display_get_registry(state->wl_display);
    wl_registry_add_listener(registry, &wlr_registry_listener, instance);

    wl_display_roundtrip(state->wl_display);

    if (!instance->manager) {
        logprint(ERROR, "hyprland shortcut protocol unavailable!");
        return;
    }

    sd_bus_slot *slot = NULL;
    int ret = sd_bus_add_object_vtable(state->bus, &slot, object_path, interface_name, gs_vtable, state);

    logprint(DEBUG, "dbus: gs: ret bind %d", ret);

    // register to hl
}