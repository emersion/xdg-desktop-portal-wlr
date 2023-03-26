#pragma once

#include "protocols/hyprland-global-shortcuts-v1-client-protocol.h"

struct xdpw_state;
struct xdpw_session;

struct globalShortcut {
    char* name;
    char* description;
    struct wl_list link;
    struct hyprland_global_shortcut_v1* hlShortcut;
};

struct globalShortcutsClient {
    struct xdpw_session* session;
    struct wl_list shortcuts;  // struct globalShortcut*
    struct wl_list link;
    char* parent_window;
    bool sentShortcuts;
};

struct globalShortcutsInstance {
    struct hyprland_global_shortcuts_manager_v1* manager;
    struct wl_list shortcutClients;  // struct globalShortcutsClient*
};

void initShortcutsInstance(struct xdpw_state* state, struct globalShortcutsInstance* instance);