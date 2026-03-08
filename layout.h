#ifndef LAYOUT_H
#define LAYOUT_H

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

typedef enum {
    EDGE_RIGHT,
    EDGE_LEFT,
    EDGE_TOP,
    EDGE_BOTTOM
} ScreenEdge;

typedef struct {
    ScreenEdge edge;
    int margin;
    GtkLayerShellLayer layer;
    int exclusive_zone;
    gboolean is_vertical;
    gchar *toggle_visibility_bind;
    gchar *toggle_expand_bind;
    gboolean notifications_enabled;
    gboolean now_playing_enabled;
    gboolean visualizer_enabled;           // NEW
    gint visualizer_idle_timeout;          // NEW
    gboolean vertical_display_enabled;
    gint vertical_display_scroll_interval;
    gchar **player_preference;             // NEW - Array of preferred players (e.g., ["spotify", "vlc"])
    gint player_preference_count;          // NEW - Number of preferred players
} LayoutConfig;

typedef struct {
    GtkWidget *album_cover;
    GtkWidget *source_label;
    GtkWidget *track_title;
    GtkWidget *artist_label;
    GtkWidget *progress_bar;
    GtkWidget *time_remaining;
} ExpandedWidgets;

// Config management
LayoutConfig* layout_load_config(void);
void layout_free_config(LayoutConfig *config);

// Window setup
void layout_setup_window_anchors(GtkWindow *window, LayoutConfig *config);

// UI construction
GtkWidget* layout_create_control_bar(LayoutConfig *config,
                                      GtkWidget **prev_btn,
                                      GtkWidget **play_btn,
                                      GtkWidget **next_btn,
                                      GtkWidget **expand_btn);

GtkWidget* layout_create_expanded_section(LayoutConfig *config, ExpandedWidgets *widgets);

GtkWidget* layout_create_main_container(LayoutConfig *config,
                                         GtkWidget *control_bar,
                                         GtkWidget *revealer);

// Helper functions
const gchar* layout_get_expand_icon(LayoutConfig *config, gboolean is_expanded);
GtkRevealerTransitionType layout_get_transition_type(LayoutConfig *config);

#endif // LAYOUT_H
