#include "layout.h"
#include <stdio.h>
#include <string.h>

// ========================================
// CONFIG LOADING
// ========================================

LayoutConfig* layout_load_config(void) {
    LayoutConfig *config = g_new0(LayoutConfig, 1);
    
    gchar *config_dir = g_build_filename(g_get_user_config_dir(), "hyprwave", NULL);
    gchar *config_file = g_build_filename(config_dir, "config.conf", NULL);
    
    // Create config directory if it doesn't exist
    g_mkdir_with_parents(config_dir, 0755);
    
    // Check if config file exists, create default if not
    if (!g_file_test(config_file, G_FILE_TEST_EXISTS)) {
// Around line 22, add to the default config string:
gchar *default_config = 
    "# HyprWave Configuration File\n"
    "\n"
    "[General]\n"
    "# Edge to anchor HyprWave to\n"
    "# Options: right, left, top, bottom\n"
    "edge = right\n"
    "\n"
    "# Margin from the screen edge (in pixels)\n"
    "margin = 10\n"
    "\n"
    "# Layer to render on\n"
    "# Options: background, bottom, top, overlay\n"
    "layer = top\n"
    "\n"
    "# Exclusive zone: 0 = respect other surfaces, -1 = overlap everything\n"
    "exclusive_zone = 0\n"
    "\n"
    "[MusicPlayer]\n"
    "# Comma-separated list of preferred music players (first = highest priority)\n"
    "# HyprWave will search for these in order and latch onto the first one found\n"
    "# Leave empty to connect to any available player\n"
    "# Common names: spotify, vlc, firefox, chromium, mpd, rhythmbox, strawberry\n"
    "preference = spotify,vlc\n"
    "\n"
    "[Keybinds]\n"
    "# Toggle HyprWave visibility (hide/show entire window)\n"
    "toggle_visibility = Super+Shift+M\n"
    "\n"
    "# Toggle expanded section (show/hide album details)\n"
    "toggle_expand = Super+M\n"
    "\n"
    "[Notifications]\n"
    "# Enable/disable notifications\n"
    "enabled = true\n"
    "\n"
    "# Show notification when song changes\n"
    "now_playing = true\n"
    "\n"
    "[Visualizer]\n"                          // NEW SECTION
    "# Enable/disable visualizer (horizontal layout only)\n"
    "enabled = true\n"
    "\n"
    "# Idle timeout in seconds before visualizer appears\n"
    "# Set to 0 to disable auto-activation (visualizer only shows on demand)\n"
    "idle_timeout = 30\n"
    "[VerticalDisplay]\n"                    // ADD THIS ENTIRE SECTION
    "# Enable/disable vertical display (vertical layout only)\n"
    "enabled = true\n"
    "\n"
    "# Idle timeout in seconds before vertical display appears\n"
    "# Set to 0 to disable auto-activation (display only shows on demand)\n"
    "idle_timeout = 5\n";
        
        g_file_set_contents(config_file, default_config, -1, NULL);
        g_print("Created default config at: %s\n", config_file);
    }
    
    // Load config
    GKeyFile *keyfile = g_key_file_new();
    
// Set defaults
config->edge = EDGE_RIGHT;
config->margin = 10;
config->layer = GTK_LAYER_SHELL_LAYER_OVERLAY;
config->exclusive_zone = 0;
config->toggle_visibility_bind = g_strdup("Super+Shift+M");
config->toggle_expand_bind = g_strdup("Super+M");
config->notifications_enabled = TRUE;
config->now_playing_enabled = TRUE;
config->visualizer_enabled = TRUE;          // NEW
config->visualizer_idle_timeout = 30;
config->vertical_display_enabled = TRUE;        // ADD THIS
config->vertical_display_scroll_interval = 5;  // 5 second idle timeout (was 60)
config->player_preference = NULL;               // NEW - No preference by default
config->player_preference_count = 0;            // NEW

    
    if (g_key_file_load_from_file(keyfile, config_file, G_KEY_FILE_NONE, NULL)) {
        // Load General section
        gchar *edge_str = g_key_file_get_string(keyfile, "General", "edge", NULL);
        
        if (edge_str) {
            if (g_strcmp0(edge_str, "left") == 0) {
                config->edge = EDGE_LEFT;
            } else if (g_strcmp0(edge_str, "top") == 0) {
                config->edge = EDGE_TOP;
            } else if (g_strcmp0(edge_str, "bottom") == 0) {
                config->edge = EDGE_BOTTOM;
            } else {
                config->edge = EDGE_RIGHT;
            }
            g_free(edge_str);
        }
        
        config->margin = g_key_file_get_integer(keyfile, "General", "margin", NULL);
        if (config->margin == 0) config->margin = 10;

        gchar *layer_str = g_key_file_get_string(keyfile, "General", "layer", NULL);
        if (layer_str) {
            if (g_strcmp0(layer_str, "background") == 0) {
                config->layer = GTK_LAYER_SHELL_LAYER_BACKGROUND;
            } else if (g_strcmp0(layer_str, "bottom") == 0) {
                config->layer = GTK_LAYER_SHELL_LAYER_BOTTOM;
            } else if (g_strcmp0(layer_str, "top") == 0) {
                config->layer = GTK_LAYER_SHELL_LAYER_TOP;
            } else {
                config->layer = GTK_LAYER_SHELL_LAYER_OVERLAY;
            }
            g_free(layer_str);
        }

        GError *ez_err = NULL;
        gint ez_val = g_key_file_get_integer(keyfile, "General", "exclusive_zone", &ez_err);
        if (!ez_err) {
            config->exclusive_zone = ez_val;
        }
        g_clear_error(&ez_err);

        // Load Keybinds section (optional)
        gchar *vis_bind = g_key_file_get_string(keyfile, "Keybinds", "toggle_visibility", NULL);
        if (vis_bind) {
            g_free(config->toggle_visibility_bind);
            config->toggle_visibility_bind = vis_bind;
        }
        
        gchar *exp_bind = g_key_file_get_string(keyfile, "Keybinds", "toggle_expand", NULL);
        if (exp_bind) {
            g_free(config->toggle_expand_bind);
            config->toggle_expand_bind = exp_bind;
        }
        
        // Load Notifications section (optional)
        GError *error = NULL;
        gboolean notif_enabled = g_key_file_get_boolean(keyfile, "Notifications", "enabled", &error);
        if (!error) {
            config->notifications_enabled = notif_enabled;
        } else {
            g_error_free(error);
            error = NULL;
        }
        
gboolean now_playing = g_key_file_get_boolean(keyfile, "Notifications", "now_playing", &error);
        if (!error) {
            config->now_playing_enabled = now_playing;
        } else {
            g_error_free(error);
            error = NULL;
        }
        
        // Load Visualizer section (optional) - NEW
        gboolean viz_enabled = g_key_file_get_boolean(keyfile, "Visualizer", "enabled", &error);
        if (!error) {
            config->visualizer_enabled = viz_enabled;
        } else {
            g_error_free(error);
            error = NULL;
        }
        
        gint viz_timeout = g_key_file_get_integer(keyfile, "Visualizer", "idle_timeout", &error);
        if (!error) {
            config->visualizer_idle_timeout = viz_timeout;
            if (config->visualizer_idle_timeout < 0) config->visualizer_idle_timeout = 0;
        } else {
            g_error_free(error);
        }
    
    
        gboolean vert_enabled = g_key_file_get_boolean(keyfile, "VerticalDisplay", "enabled", &error);
        if (!error) {
            config->vertical_display_enabled = vert_enabled;
        } else {
            g_error_free(error);
            error = NULL;
        }
        
        gint vert_timeout = g_key_file_get_integer(keyfile, "VerticalDisplay", "idle_timeout", &error);
        if (!error) {
            config->vertical_display_scroll_interval = vert_timeout;
            if (config->vertical_display_scroll_interval < 0) config->vertical_display_scroll_interval = 0;
        } else {
            g_error_free(error);
        }
        
        // Load MusicPlayer section - NEW
        error = NULL;
        gchar *player_pref_str = g_key_file_get_string(keyfile, "MusicPlayer", "preference", &error);
        if (!error && player_pref_str && strlen(player_pref_str) > 0) {
            // Split by comma and trim whitespace
            gchar **raw_list = g_strsplit(player_pref_str, ",", -1);
            gint raw_count = g_strv_length(raw_list);
            
            // Count non-empty entries
            gint valid_count = 0;
            for (gint i = 0; i < raw_count; i++) {
                gchar *trimmed = g_strstrip(g_strdup(raw_list[i]));
                if (strlen(trimmed) > 0) {
                    valid_count++;
                }
                g_free(trimmed);
            }
            
            // Allocate array for valid entries
            if (valid_count > 0) {
                config->player_preference = g_new0(gchar*, valid_count + 1);  // NULL-terminated
                config->player_preference_count = valid_count;
                
                gint idx = 0;
                for (gint i = 0; i < raw_count; i++) {
                    gchar *trimmed = g_strstrip(g_strdup(raw_list[i]));
                    if (strlen(trimmed) > 0) {
                        config->player_preference[idx++] = trimmed;  // Transfer ownership
                    } else {
                        g_free(trimmed);
                    }
                }
                
                g_print("Player preference: ");
                for (gint i = 0; i < config->player_preference_count; i++) {
                    g_print("%s%s", config->player_preference[i], 
                           i < config->player_preference_count - 1 ? ", " : "");
                }
                g_print("\n");
            }
            
            g_strfreev(raw_list);
            g_free(player_pref_str);
        } else if (error) {
            g_error_free(error);
        }
    }
    config->is_vertical = (config->edge == EDGE_RIGHT || config->edge == EDGE_LEFT);
    
    g_key_file_free(keyfile);
    g_free(config_file);
    g_free(config_dir);
    
    g_print("Layout: %s edge (%s)\n",
            config->edge == EDGE_RIGHT ? "right" :
            config->edge == EDGE_LEFT ? "left" :
            config->edge == EDGE_TOP ? "top" : "bottom",
            config->is_vertical ? "vertical" : "horizontal");
    
    return config;
}

void layout_free_config(LayoutConfig *config) {
    if (config) {
        g_free(config->toggle_visibility_bind);
        g_free(config->toggle_expand_bind);
        if (config->player_preference) {
            g_strfreev(config->player_preference);
        }
        g_free(config);
    }
}

// ========================================
// WINDOW SETUP
// ========================================

void layout_setup_window_anchors(GtkWindow *window, LayoutConfig *config) {
    // Set anchors based on edge
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT, config->edge == EDGE_RIGHT);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, config->edge == EDGE_LEFT);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, config->edge == EDGE_TOP);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, config->edge == EDGE_BOTTOM);
    
    // Set margin
    if (config->edge == EDGE_RIGHT) {
        gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_RIGHT, config->margin);
    } else if (config->edge == EDGE_LEFT) {
        gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_LEFT, config->margin);
    } else if (config->edge == EDGE_TOP) {
        gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, config->margin);
    } else {
        gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_BOTTOM, config->margin);
    }
}

// ========================================
// CONTROL BAR
// ========================================

GtkWidget* layout_create_control_bar(LayoutConfig *config,
                                      GtkWidget **prev_btn,
                                      GtkWidget **play_btn,
                                      GtkWidget **next_btn,
                                      GtkWidget **expand_btn) {
    GtkOrientation orientation = config->is_vertical ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;
    GtkWidget *control_bar = gtk_box_new(orientation, 8);
    
    gtk_widget_add_css_class(control_bar, config->is_vertical ? "control-container" : "control-container-horizontal");
    gtk_widget_set_halign(control_bar, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(control_bar, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(control_bar, FALSE);
    gtk_widget_set_vexpand(control_bar, FALSE);
    
    if (config->is_vertical) {
        gtk_widget_set_size_request(control_bar, 70, 240);
    } else {
        gtk_widget_set_size_request(control_bar, 240, 60);
    }
    
    // Create buttons (widgets created externally, we just arrange them)
    gtk_box_append(GTK_BOX(control_bar), *prev_btn);
    gtk_box_append(GTK_BOX(control_bar), *play_btn);
    gtk_box_append(GTK_BOX(control_bar), *next_btn);
    gtk_box_append(GTK_BOX(control_bar), *expand_btn);
    
    return control_bar;
}

// ========================================
// EXPANDED SECTION
// ========================================

GtkWidget* layout_create_expanded_section(LayoutConfig *config, ExpandedWidgets *widgets) {
    GtkWidget *expanded_section;
    
    if (config->is_vertical) {
        // Vertical layout: stack everything vertically
        expanded_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_add_css_class(expanded_section, "expanded-section");
        gtk_widget_set_halign(expanded_section, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(expanded_section, GTK_ALIGN_CENTER);
        
        gtk_box_append(GTK_BOX(expanded_section), widgets->album_cover);
        gtk_box_append(GTK_BOX(expanded_section), widgets->source_label);
        gtk_box_append(GTK_BOX(expanded_section), widgets->track_title);
        gtk_box_append(GTK_BOX(expanded_section), widgets->artist_label);
        gtk_box_append(GTK_BOX(expanded_section), widgets->progress_bar);
        gtk_box_append(GTK_BOX(expanded_section), widgets->time_remaining);
        
    } else {
        // Horizontal layout: album on left, info on right
        expanded_section = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_add_css_class(expanded_section, "expanded-section-horizontal");
        gtk_widget_set_halign(expanded_section, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(expanded_section, GTK_ALIGN_CENTER);
        
        // Info panel (right side)
        GtkWidget *info_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_valign(info_panel, GTK_ALIGN_CENTER);
        
        gtk_label_set_xalign(GTK_LABEL(widgets->source_label), 0.0);
        gtk_label_set_xalign(GTK_LABEL(widgets->track_title), 0.0);
        gtk_label_set_xalign(GTK_LABEL(widgets->artist_label), 0.0);
        gtk_label_set_xalign(GTK_LABEL(widgets->time_remaining), 0.0);
        
        // Increase max width for horizontal layout
        gtk_label_set_max_width_chars(GTK_LABEL(widgets->track_title), 25);
        gtk_label_set_max_width_chars(GTK_LABEL(widgets->artist_label), 25);
        gtk_widget_set_size_request(widgets->progress_bar, 180, 4);
        
        gtk_box_append(GTK_BOX(info_panel), widgets->source_label);
        gtk_box_append(GTK_BOX(info_panel), widgets->track_title);
        gtk_box_append(GTK_BOX(info_panel), widgets->artist_label);
        gtk_box_append(GTK_BOX(info_panel), widgets->progress_bar);
        gtk_box_append(GTK_BOX(info_panel), widgets->time_remaining);
        
        gtk_box_append(GTK_BOX(expanded_section), widgets->album_cover);
        gtk_box_append(GTK_BOX(expanded_section), info_panel);
    }
    
    return expanded_section;
}

// ========================================
// MAIN CONTAINER
// ========================================

GtkWidget* layout_create_main_container(LayoutConfig *config,
                                         GtkWidget *control_bar,
                                         GtkWidget *revealer) {
    GtkOrientation orientation = config->is_vertical ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL;
    GtkWidget *main_container = gtk_box_new(orientation, 0);
    
    gtk_widget_add_css_class(main_container, "main-container");
    gtk_widget_set_hexpand(main_container, FALSE);
    gtk_widget_set_vexpand(main_container, FALSE);
    
    if (config->is_vertical) {
        // Vertical: control bar on edge, revealer slides outward
        gtk_box_append(GTK_BOX(main_container), control_bar);
        gtk_box_append(GTK_BOX(main_container), revealer);
    } else {
        // Horizontal: order depends on top or bottom
        if (config->edge == EDGE_TOP) {
            gtk_box_append(GTK_BOX(main_container), control_bar);
            gtk_box_append(GTK_BOX(main_container), revealer);
        } else {
            gtk_box_append(GTK_BOX(main_container), revealer);
            gtk_box_append(GTK_BOX(main_container), control_bar);
        }
    }
    
    return main_container;
}

// ========================================
// HELPER FUNCTIONS
// ========================================

const gchar* layout_get_expand_icon(LayoutConfig *config, gboolean is_expanded) {
    if (config->is_vertical) {
        return is_expanded ? "arrow-right.svg" : "arrow-left.svg";
    } else {
        if (config->edge == EDGE_TOP) {
            return is_expanded ? "arrow-up.svg" : "arrow-down.svg";
        } else {
            return is_expanded ? "arrow-down.svg" : "arrow-up.svg";
        }
    }
}

GtkRevealerTransitionType layout_get_transition_type(LayoutConfig *config) {
    if (config->is_vertical) {
        return GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT;
    } else {
        if (config->edge == EDGE_TOP) {
            return GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN;
        } else {
            return GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP;
        }
    }
}
