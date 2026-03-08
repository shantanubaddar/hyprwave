#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include "layout.h"
#include "paths.h"
#include "notification.h"
#include "art.h"
#include "volume.h"
#include "visualizer.h"
#include "vertical_display.h"
#include "morph-animation-system.h" 

typedef struct {
    GtkWidget *window;
    GtkWidget *window_revealer;
    GtkWidget *revealer;
    GtkWidget *play_icon;
    GtkWidget *expand_icon;
    GtkWidget *album_cover;
    GtkWidget *source_label;
    GtkWidget *track_title;
    GtkWidget *artist_label;
    GtkWidget *time_remaining;
    GtkWidget *progress_bar;
    GtkWidget *expanded_with_volume;
    gboolean is_playing;
    gboolean is_expanded;
    gboolean is_visible;
    gboolean is_seeking;
    GDBusProxy *mpris_proxy;
    gchar *current_player;
    guint update_timer;
    LayoutConfig *layout;
    NotificationState *notification;
    VolumeState *volume;
    gchar *last_track_id;
    guint notification_timer;
    gchar *pending_title;
    gchar *pending_artist;
    gchar *pending_art_url;
    GtkWidget *control_bar_container;
    GtkWidget *prev_btn;
    GtkWidget *play_btn;
    GtkWidget *next_btn;
    GtkWidget *expand_btn;
    
    // Ghost button overlay - purely visual fade effect during morph
    GtkWidget *ghost_overlay;      // Container for ghost buttons
    GtkWidget *ghost_prev_btn;
    GtkWidget *ghost_play_btn;
    GtkWidget *ghost_next_btn;
    GtkWidget *ghost_expand_btn;
    
    VisualizerState *visualizer;       // For horizontal layouts
    VerticalDisplayState *vertical_display;  // NEW - For vertical layouts
    MorphAnimation morph_anim;
    EasingCurve morph_easing;
    guint idle_timer;
    gboolean is_idle_mode;
    guint morph_tick_id;      // Tick callback ID for morph animation (horizontal)
    guint morph_timer;        // Timer for button fade animation (vertical)
    gdouble button_fade_opacity;
    
    // Control bar natural size (with buttons visible) - used as exit animation target
    gint control_bar_width;
    gint control_bar_height;
    
    // Player monitoring - NEW
    guint dbus_watch_id;               // D-Bus name watcher
    guint reconnect_timer;             // Timer for reconnection attempts
    
} AppState;

static void update_position(AppState *state);
static void update_metadata(AppState *state);
static void update_playback_status(AppState *state);
static void on_expand_clicked(GtkButton *button, gpointer user_data);
static void on_properties_changed(GDBusProxy *proxy, GVariant *changed_properties,
                                  GStrv invalidated_properties, gpointer user_data);
                                  
static void exit_idle_mode(AppState *state);
static void reset_idle_timer(AppState *state);
static gboolean enter_idle_mode(gpointer user_data);
static gboolean delayed_control_bar_resize(gpointer user_data);
static gboolean enter_vertical_idle_mode(gpointer user_data);
static void exit_vertical_idle_mode(AppState *state);
static void find_active_player(AppState *state);
static void on_shutdown(GtkApplication *app, gpointer user_data);

// Helper to re-enable CSS transitions after animation
static gboolean re_enable_transitions(gpointer user_data) {
    GtkWidget *widget = GTK_WIDGET(user_data);
    gtk_widget_remove_css_class(widget, "no-transition");
    return G_SOURCE_REMOVE;
}

static AppState *global_state = NULL;
static gboolean start_hidden = FALSE;  // For --start-hidden flag

// Delayed reveal for smooth position changes
static gboolean delayed_reveal_window(gpointer user_data) {
    AppState *state = (AppState *)user_data;
    if (state && state->window_revealer) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(state->window_revealer), TRUE);
    }
    return G_SOURCE_REMOVE;
}

static gboolean morph_animation_tick_callback(GtkWidget *widget,
                                               GdkFrameClock *frame_clock,
                                               gpointer user_data) {
    AppState *state = (AppState *)user_data;
    MorphAnimation *anim = &state->morph_anim;
    
    // Get frame time from GTK's frame clock.
    // CRITICAL: start_time MUST come from this same clock.  It is set to -1
    // as a sentinel by enter/exit_idle_mode; we capture the actual start on
    // the first tick.  Using g_get_monotonic_time() here would be wrong —
    // that's a different clock source and when enter_idle_mode is called from
    // a g_timeout the timeout can fire mid-frame, making the first
    // frame_time land BEFORE start_time → negative progress → snap.
    gint64 now = gdk_frame_clock_get_frame_time(frame_clock);
    if (anim->start_time < 0) {
        anim->start_time = now;   // first tick — anchor here
    }
    gint64 elapsed_us = now - anim->start_time;
    gdouble raw_progress = (gdouble)elapsed_us / (anim->duration_ms * 1000.0);
    
    // Clamp both ends
    if (raw_progress < 0.0) raw_progress = 0.0;
    if (raw_progress > 1.0) raw_progress = 1.0;
    
    // Apply easing curve for smooth motion
    gdouble eased_progress = apply_easing(raw_progress, state->morph_easing);
    
    // Interpolate ALL properties simultaneously
    gint current_width = anim->start_width + 
                        (gint)((anim->end_width - anim->start_width) * eased_progress);
    gint current_height = anim->start_height + 
                         (gint)((anim->end_height - anim->start_height) * eased_progress);
    
    gdouble current_viz_opacity = anim->start_viz_opacity + 
                                 (anim->end_viz_opacity - anim->start_viz_opacity) * eased_progress;
    
    // Apply to control bar (size change)
    gtk_widget_set_size_request(state->control_bar_container, current_width, current_height);
    gtk_widget_queue_resize(state->control_bar_container);

    // DEBUG: print requested vs ACTUAL allocated size every frame
    {
        GtkAllocation alloc;
        gtk_widget_get_allocation(state->control_bar_container, &alloc);
        g_print("  TICK t=%.3f eased=%.3f req=%dx%d ALLOC=%dx%d\n",
                raw_progress, eased_progress,
                current_width, current_height,
                alloc.width, alloc.height);
    }
    
    // Buttons are NOT in layout during the morph.  They were hidden at the
    // start of enter_idle_mode and are only restored in the completion block.
    
    // GHOST BUTTON FADE: Animate the visual overlay
    if (state->ghost_overlay) {
        gdouble ghost_opacity;
        if (anim->is_morphing_to_idle) {
            // ENTER: Fading OUT: 1.0 → 0.0
            ghost_opacity = 1.0 - eased_progress;
            
            gtk_widget_set_opacity(state->ghost_prev_btn,   ghost_opacity);
            gtk_widget_set_opacity(state->ghost_play_btn,   ghost_opacity);
            gtk_widget_set_opacity(state->ghost_next_btn,   ghost_opacity);
            gtk_widget_set_opacity(state->ghost_expand_btn, ghost_opacity);
        } else {
            // EXIT: Skip ghost fade - just restore real buttons at 98%
            // (Ghost repositioning during container growth is too noticeable)
            if (eased_progress >= 0.98 && !gtk_widget_get_visible(state->prev_btn)) {
                gtk_widget_set_size_request(state->prev_btn,   44, 44);
                gtk_widget_set_size_request(state->play_btn,   44, 44);
                gtk_widget_set_size_request(state->next_btn,   44, 44);
                gtk_widget_set_size_request(state->expand_btn, 44, 44);
                gtk_widget_set_opacity(state->prev_btn,   1.0);
                gtk_widget_set_opacity(state->play_btn,   1.0);
                gtk_widget_set_opacity(state->next_btn,   1.0);
                gtk_widget_set_opacity(state->expand_btn, 1.0);
                gtk_widget_set_visible(state->prev_btn,   TRUE);
                gtk_widget_set_visible(state->play_btn,   TRUE);
                gtk_widget_set_visible(state->next_btn,   TRUE);
                gtk_widget_set_visible(state->expand_btn, TRUE);
            }
        }
    }
    
    // Apply to visualizer (horizontal) or vertical_display (vertical)
    if (state->visualizer) {
        gtk_widget_set_opacity(state->visualizer->container, current_viz_opacity);
        state->visualizer->fade_opacity = current_viz_opacity;
    } else if (state->vertical_display) {
        gtk_widget_set_opacity(state->vertical_display->container, current_viz_opacity);
    }
    
    // Check if animation is complete
    if (raw_progress >= 1.0) {
        // Animation complete - LOCK IN the final state
        gtk_widget_set_size_request(state->control_bar_container, 
                                     anim->end_width, anim->end_height);
        
        // Final button state.  Buttons have been hidden for the entire
        // animation.  If we morphed back to control, restore them now that
        // the container is at its final size.
        if (anim->is_morphing_to_idle) {
            // Idle — buttons stay hidden, already done
            gtk_widget_set_visible(state->prev_btn,   FALSE);
            gtk_widget_set_visible(state->play_btn,   FALSE);
            gtk_widget_set_visible(state->next_btn,   FALSE);
            gtk_widget_set_visible(state->expand_btn, FALSE);
            
            // Hide ghost overlay (faded to 0)
            if (state->ghost_overlay) {
                gtk_widget_set_visible(state->ghost_overlay, FALSE);
            }
        } else {
            // Control — restore buttons if not already done
            if (!gtk_widget_get_visible(state->prev_btn)) {
                gtk_widget_set_size_request(state->prev_btn,   44, 44);
                gtk_widget_set_size_request(state->play_btn,   44, 44);
                gtk_widget_set_size_request(state->next_btn,   44, 44);
                gtk_widget_set_size_request(state->expand_btn, 44, 44);
                gtk_widget_set_opacity(state->prev_btn,   1.0);
                gtk_widget_set_opacity(state->play_btn,   1.0);
                gtk_widget_set_opacity(state->next_btn,   1.0);
                gtk_widget_set_opacity(state->expand_btn, 1.0);
                gtk_widget_set_visible(state->prev_btn,   TRUE);
                gtk_widget_set_visible(state->play_btn,   TRUE);
                gtk_widget_set_visible(state->next_btn,   TRUE);
                gtk_widget_set_visible(state->expand_btn, TRUE);
            }
            
            // Hide ghost overlay (real buttons now visible)
            if (state->ghost_overlay) {
                gtk_widget_set_visible(state->ghost_overlay, FALSE);
            }
        }
        
        // Set final display opacity (visualizer or vertical_display)
        if (state->visualizer) {
            gtk_widget_set_opacity(state->visualizer->container, anim->end_viz_opacity);
            state->visualizer->fade_opacity = anim->end_viz_opacity;
            
            if (anim->end_viz_opacity < 0.01) {
                gtk_widget_set_visible(state->visualizer->container, FALSE);
            } else if (anim->is_morphing_to_idle) {
                // NOW start audio capture (after animation is done)
                if (!state->visualizer->is_running) {
                    visualizer_start(state->visualizer);
                    g_print("  ✓ Visualizer audio started (post-animation)\n");
                }
            }
        } else if (state->vertical_display) {
            gtk_widget_set_opacity(state->vertical_display->container, anim->end_viz_opacity);
            
            if (anim->end_viz_opacity < 0.01) {
                gtk_widget_set_visible(state->vertical_display->container, FALSE);
            }
        }
        
        // Force immediate layout update
        gtk_widget_queue_resize(state->control_bar_container);
        
        // Re-enable CSS transitions after animation completes
        g_timeout_add(16, re_enable_transitions, state->control_bar_container);
        
        g_print("✓ Animation complete - locked at %dx%d\n", anim->end_width, anim->end_height);
        
        // Remove tick callback
        gtk_widget_remove_tick_callback(widget, state->morph_tick_id);
        state->morph_tick_id = 0;
        return G_SOURCE_REMOVE;
    }
    
    return G_SOURCE_CONTINUE;
}
// Button fade animation (for VERTICAL mode only)
static gboolean animate_button_fade(gpointer user_data) {
    AppState *state = (AppState *)user_data;
    
    if (state->is_idle_mode) {
        // Fade out buttons
        state->button_fade_opacity -= 0.05;
        if (state->button_fade_opacity <= 0.0) {
            state->button_fade_opacity = 0.0;
            
            // Actually HIDE the buttons
            gtk_widget_set_visible(state->prev_btn, FALSE);
            gtk_widget_set_visible(state->play_btn, FALSE);
            gtk_widget_set_visible(state->next_btn, FALSE);
            gtk_widget_set_visible(state->expand_btn, FALSE);
            
            g_print("  Buttons hidden - bar can now shrink\n");
            
            state->morph_timer = 0;
            return G_SOURCE_REMOVE;
        }
    } else {
        // Make buttons visible first if they were hidden
        if (state->button_fade_opacity == 0.0) {
            gtk_widget_set_visible(state->prev_btn, TRUE);
            gtk_widget_set_visible(state->play_btn, TRUE);
            gtk_widget_set_visible(state->next_btn, TRUE);
            gtk_widget_set_visible(state->expand_btn, TRUE);
            g_print("  Buttons visible again\n");
        }
        
        // Fade in buttons
        state->button_fade_opacity += 0.05;
        if (state->button_fade_opacity >= 1.0) {
            state->button_fade_opacity = 1.0;
            state->morph_timer = 0;
            return G_SOURCE_REMOVE;
        }
    }
    
    // Apply opacity to all buttons
    gtk_widget_set_opacity(state->prev_btn, state->button_fade_opacity);
    gtk_widget_set_opacity(state->play_btn, state->button_fade_opacity);
    gtk_widget_set_opacity(state->next_btn, state->button_fade_opacity);
    gtk_widget_set_opacity(state->expand_btn, state->button_fade_opacity);
    
    return G_SOURCE_CONTINUE;
}

// Delayed resize for vertical mode
static gboolean delayed_control_bar_resize_vertical(gpointer user_data) {
    AppState *state = (AppState *)user_data;
    gtk_widget_set_size_request(state->control_bar_container, 32, 280);
    gtk_widget_queue_resize(state->control_bar_container);
    g_print("  Vertical bar resized to: 32x280 (slim mode)\n");
    return G_SOURCE_REMOVE;
}

// FIXED: Smooth contract animation callback
static void on_revealer_transition_done(GObject *revealer_obj, GParamSpec *pspec, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    if (!gtk_revealer_get_child_revealed(GTK_REVEALER(revealer_obj))) {
        // SMOOTH CONTRACT ANIMATION: Set proper control bar size after transition
        if (state->layout->is_vertical) {
            gtk_window_set_default_size(GTK_WINDOW(state->window), -1, 60);
        } else {
            gtk_window_set_default_size(GTK_WINDOW(state->window), 300, -1);
        }
        gtk_widget_queue_resize(state->window);
    }
}



static void on_window_hide_complete(GObject *revealer, GParamSpec *pspec, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    if (!gtk_revealer_get_child_revealed(GTK_REVEALER(state->window_revealer))) {
        gtk_widget_set_visible(state->window, FALSE);
    }
}

static void handle_sigusr1(int sig) {
    if (!global_state) return;
    global_state->is_visible = !global_state->is_visible;
    
    if (!global_state->is_visible) {
        // HIDE
        if (global_state->is_idle_mode) {
            if (global_state->visualizer) {
                visualizer_hide(global_state->visualizer);
            }
            if (global_state->vertical_display) {
                vertical_display_hide(global_state->vertical_display);
            }
        }
        
        // Cancel idle timer
        if (global_state->idle_timer > 0) {
            g_source_remove(global_state->idle_timer);
            global_state->idle_timer = 0;
        }
        
        if (global_state->is_expanded) {
            global_state->is_expanded = FALSE;
            gtk_revealer_set_reveal_child(GTK_REVEALER(global_state->revealer), FALSE);
        }
        gtk_revealer_set_reveal_child(GTK_REVEALER(global_state->window_revealer), FALSE);
    } else {
        // SHOW
        gtk_widget_set_visible(global_state->window, TRUE);
        gtk_revealer_set_reveal_child(GTK_REVEALER(global_state->window_revealer), TRUE);
        
        // Restore idle mode display if we were in it
        if (global_state->is_idle_mode) {
            if (global_state->visualizer) {
                visualizer_show(global_state->visualizer);
            }
            if (global_state->vertical_display) {
                vertical_display_show(global_state->vertical_display);
            }
        }
        
        // Restart idle timer based on layout
        if (!global_state->is_expanded && !global_state->is_idle_mode) {
            if (global_state->layout->is_vertical && global_state->vertical_display &&
                global_state->layout->vertical_display_enabled &&
                global_state->layout->vertical_display_scroll_interval > 0) {
                global_state->idle_timer = g_timeout_add_seconds(
                    global_state->layout->vertical_display_scroll_interval, 
                    enter_vertical_idle_mode, global_state);
            } else if (!global_state->layout->is_vertical && global_state->visualizer &&
                       global_state->layout->visualizer_enabled && 
                       global_state->layout->visualizer_idle_timeout > 0) {
                global_state->idle_timer = g_timeout_add_seconds(
                    global_state->layout->visualizer_idle_timeout, 
                    enter_idle_mode, global_state);
            }
        }
    }
}


static void handle_sigusr2(int sig) {
    if (!global_state) return;
    if (!global_state->is_visible) return;
    
    // If in idle mode, allow expansion but keep display running
    if (global_state->is_idle_mode) {
        // Toggle expansion
        global_state->is_expanded = !global_state->is_expanded;
        
        // Hide volume if collapsing
        if (!global_state->is_expanded && global_state->volume && global_state->volume->is_showing) {
            volume_hide(global_state->volume);
        }
        
        if (global_state->is_expanded) {
            // Cancel idle timer while expanded
            if (global_state->idle_timer > 0) {
                g_source_remove(global_state->idle_timer);
                global_state->idle_timer = 0;
            }
        }
        
        // Update expand icon and revealer
        const gchar *icon_name = layout_get_expand_icon(global_state->layout, global_state->is_expanded);
        gchar *icon_path = get_icon_path(icon_name);
        gtk_image_set_from_file(GTK_IMAGE(global_state->expand_icon), icon_path);
        free_path(icon_path);
        gtk_revealer_set_reveal_child(GTK_REVEALER(global_state->revealer), global_state->is_expanded);
        
        return;
    }
    
    // Normal expand toggle (not in idle mode)
    on_expand_clicked(NULL, global_state);
}

// Forward declarations for signal handler callbacks
static void on_play_clicked(GtkButton *button, gpointer user_data);
static void on_next_clicked(GtkButton *button, gpointer user_data);
static void on_prev_clicked(GtkButton *button, gpointer user_data);

// Idle callbacks for real-time signal handlers (safe to call GTK/D-Bus from main loop)
static gboolean idle_play_toggle(gpointer user_data) {
    AppState *state = (AppState *)user_data;
    on_play_clicked(NULL, state);
    return G_SOURCE_REMOVE;
}

static gboolean idle_next_track(gpointer user_data) {
    AppState *state = (AppState *)user_data;
    on_next_clicked(NULL, state);
    return G_SOURCE_REMOVE;
}

static gboolean idle_prev_track(gpointer user_data) {
    AppState *state = (AppState *)user_data;
    on_prev_clicked(NULL, state);
    return G_SOURCE_REMOVE;
}

static void handle_sigrtmin(int sig) {
    if (!global_state) return;
    g_idle_add(idle_play_toggle, global_state);
}

static void handle_sigrtmin1(int sig) {
    if (!global_state) return;
    g_idle_add(idle_next_track, global_state);
}

static void handle_sigrtmin2(int sig) {
    if (!global_state) return;
    g_idle_add(idle_prev_track, global_state);
}



// Enter idle mode - morph to visualizer
// REPLACE enter_idle_mode with this:
// REPLACE enter_idle_mode with this:
// Helper function for delayed resize

// Sync ghost button icons to match current real button state
static void sync_ghost_button_icons(AppState *state) {
    if (!state->ghost_overlay) return;
    
    // Copy play/pause icon state
    gchar *play_icon_path = NULL;
    if (state->is_playing) {
        play_icon_path = get_icon_path("pause.svg");
    } else {
        play_icon_path = get_icon_path("play.svg");
    }
    GtkWidget *ghost_play_icon = gtk_button_get_child(GTK_BUTTON(state->ghost_play_btn));
    gtk_image_set_from_file(GTK_IMAGE(ghost_play_icon), play_icon_path);
    free_path(play_icon_path);
    
    // Copy expand icon state
    const gchar *expand_icon_name = layout_get_expand_icon(state->layout, state->is_expanded);
    gchar *expand_icon_path = get_icon_path(expand_icon_name);
    GtkWidget *ghost_expand_icon = gtk_button_get_child(GTK_BUTTON(state->ghost_expand_btn));
    gtk_image_set_from_file(GTK_IMAGE(ghost_expand_icon), expand_icon_path);
    free_path(expand_icon_path);
}


static gboolean enter_idle_mode(gpointer user_data) {
    AppState *state = (AppState *)user_data;
    
    if (state->is_idle_mode || !state->visualizer || !state->layout->visualizer_enabled) {
        state->idle_timer = 0;
        return G_SOURCE_REMOVE;
    }
    
    g_print("→ Morphing to idle mode (Dynamic Island animation)\n");
    state->is_idle_mode = TRUE;
    
    // Cancel any existing animation
    if (state->morph_tick_id > 0) {
        gtk_widget_remove_tick_callback(state->control_bar_container, state->morph_tick_id);
        state->morph_tick_id = 0;
    }
    
    // DON'T start audio capture yet - wait for animation to complete
    // This prevents visualizer bars from updating during the morph animation
    
    // CRITICAL: Capture control bar size WITH buttons BEFORE hiding them.
    // This is the size we'll animate back to on exit.
    GtkAllocation allocation_with_buttons;
    gtk_widget_get_allocation(state->control_bar_container, &allocation_with_buttons);
    state->control_bar_width = allocation_with_buttons.width;
    state->control_bar_height = allocation_with_buttons.height;
    g_print("  Captured control bar size (with buttons): %dx%d\n",
            state->control_bar_width, state->control_bar_height);
    
    // Setup animation parameters
    MorphAnimation *anim = &state->morph_anim;
    anim->start_time = -1;  // sentinel: tick callback sets this from frame_clock on first tick
    anim->duration_ms = 300;  // Snappy!
    anim->is_morphing_to_idle = TRUE;
    
    // CRITICAL: Disable CSS transitions during animation
    gtk_widget_add_css_class(state->control_bar_container, "no-transition");
    
    // CRITICAL: Remove buttons from layout BEFORE capturing start size.
    // GTK enforces min-height:44 on .control-button via CSS — we cannot
    // override that mid-animation.  Buttons must be out of layout for the
    // entire morph so the container height is free to animate.
    
    // GHOST OVERLAY: Sync icons then show visual fade effect
    if (state->ghost_overlay) {
        sync_ghost_button_icons(state);
        gtk_widget_set_visible(state->ghost_overlay, TRUE);
        gtk_widget_set_opacity(state->ghost_prev_btn,   1.0);
        gtk_widget_set_opacity(state->ghost_play_btn,   1.0);
        gtk_widget_set_opacity(state->ghost_next_btn,   1.0);
        gtk_widget_set_opacity(state->ghost_expand_btn, 1.0);
    }
    
    // Hide real buttons instantly
    gtk_widget_set_visible(state->prev_btn,   FALSE);
    gtk_widget_set_visible(state->play_btn,   FALSE);
    gtk_widget_set_visible(state->next_btn,   FALSE);
    gtk_widget_set_visible(state->expand_btn, FALSE);
    gtk_widget_set_opacity(state->prev_btn,   0.0);
    gtk_widget_set_opacity(state->play_btn,   0.0);
    gtk_widget_set_opacity(state->next_btn,   0.0);
    gtk_widget_set_opacity(state->expand_btn, 0.0);
    
    // Force a layout pass so the allocation updates to the button-less size
    // BEFORE we capture start_height.
    gtk_widget_queue_resize(state->control_bar_container);
    gtk_widget_queue_draw(state->control_bar_container);
    
    // Capture CURRENT size AFTER buttons are gone
    GtkAllocation allocation;
    gtk_widget_get_allocation(state->control_bar_container, &allocation);
    anim->start_width = allocation.width;
    anim->start_height = allocation.height;
    anim->end_width = 280;
    anim->end_height = 32;
    
    g_print("  Starting animation from ACTUAL size: %dx%d → 280x32\n", 
            anim->start_width, anim->start_height);
    
    // Visualizer opacity: start from current, animate to 1.0
    anim->start_viz_opacity = state->visualizer ? gtk_widget_get_opacity(state->visualizer->container) : 0.0;
    anim->end_viz_opacity = 1.0;

    g_print("  viz_opacity start=%.3f end=%.3f\n",
            anim->start_viz_opacity, anim->end_viz_opacity);
    
    // Make visualizer visible (but transparent initially)
    gtk_widget_set_visible(state->visualizer->container, TRUE);
    gtk_widget_set_opacity(state->visualizer->container, 0.0);
    
    // Start animation loop at 60fps
state->morph_tick_id = gtk_widget_add_tick_callback(
    state->control_bar_container,
    morph_animation_tick_callback,
    state,
    NULL
);
    
    state->idle_timer = 0;
    return G_SOURCE_REMOVE;
}


// =============================================================================
// PART 4: NEW exit_idle_mode (REPLACE existing function completely)
// =============================================================================

static void exit_idle_mode(AppState *state) {
    if (!state->is_idle_mode || !state->visualizer) return;
    
    g_print("← Morphing to control mode (Dynamic Island animation)\n");
    state->is_idle_mode = FALSE;
    
    // Cancel any existing animation
    if (state->morph_tick_id > 0) {
        gtk_widget_remove_tick_callback(state->control_bar_container, state->morph_tick_id);
        state->morph_tick_id = 0;
    }
    
    // Setup animation parameters (REVERSE of enter_idle_mode)
    MorphAnimation *anim = &state->morph_anim;
    anim->start_time = -1;  // sentinel: tick callback sets this from frame_clock on first tick
    anim->duration_ms = 300;  // Snappy!
    anim->is_morphing_to_idle = FALSE;
    
    // CRITICAL: Disable CSS transitions during animation
    gtk_widget_add_css_class(state->control_bar_container, "no-transition");
    
    // GHOST OVERLAY: Sync icons but DON'T show yet - tick callback will show it
    // when container is nearly at final size (avoids repositioning during growth)
    if (state->ghost_overlay) {
        sync_ghost_button_icons(state);
        gtk_widget_set_opacity(state->ghost_prev_btn,   0.0);
        gtk_widget_set_opacity(state->ghost_play_btn,   0.0);
        gtk_widget_set_opacity(state->ghost_next_btn,   0.0);
        gtk_widget_set_opacity(state->ghost_expand_btn, 0.0);
        // Note: visibility will be set by tick callback at ~90% progress
    }
    
    // Real buttons stay hidden during entire animation
    
    // Capture CURRENT size (buttons are already hidden from enter)
    GtkAllocation allocation;
    gtk_widget_get_allocation(state->control_bar_container, &allocation);
    anim->start_width = allocation.width;
    anim->start_height = allocation.height;
    
    // Use the saved control bar size (with buttons) as target
    anim->end_width = state->control_bar_width;
    anim->end_height = state->control_bar_height;
    
    g_print("  Starting animation from ACTUAL size: %dx%d → %dx%d\n", 
            anim->start_width, anim->start_height,
            anim->end_width, anim->end_height);
    
    // Visualizer opacity: start from current, animate to 0.0
    anim->start_viz_opacity = state->visualizer ? gtk_widget_get_opacity(state->visualizer->container) : 1.0;
    anim->end_viz_opacity = 0.0;

    g_print("  viz_opacity start=%.3f end=%.3f\n",
            anim->start_viz_opacity, anim->end_viz_opacity);
    
    // Start animation loop at 60fps
state->morph_tick_id = gtk_widget_add_tick_callback(
    state->control_bar_container,
    morph_animation_tick_callback,
    state,
    NULL
);
    
    // Restart idle timer after animation completes
    if (state->is_visible && !state->is_expanded && state->layout->visualizer_enabled && 
        state->layout->visualizer_idle_timeout > 0) {
        state->idle_timer = g_timeout_add_seconds(state->layout->visualizer_idle_timeout, 
                                                   enter_idle_mode, state);
    }
}


// Vertical display idle mode functions
static gboolean enter_vertical_idle_mode(gpointer user_data) {
    AppState *state = (AppState *)user_data;
    
    // Don't enter if not visible, expanded, or in horizontal layout
    if (state->is_idle_mode || !state->is_visible || state->is_expanded || 
        !state->layout->is_vertical || !state->vertical_display) {
        return G_SOURCE_REMOVE;
    }
    
    g_print("→ Morphing to vertical idle mode (Dynamic Island animation)\n");
    state->is_idle_mode = TRUE;
    
    // Hide volume if showing
    if (state->volume && state->volume->is_showing) {
        volume_hide(state->volume);
    }
    
    // Cancel any existing animation
    if (state->morph_tick_id > 0) {
        gtk_widget_remove_tick_callback(state->control_bar_container, state->morph_tick_id);
        state->morph_tick_id = 0;
    }
    
    // Capture control bar size WITH buttons (vertical: 70x240)
    GtkAllocation allocation_with_buttons;
    gtk_widget_get_allocation(state->control_bar_container, &allocation_with_buttons);
    state->control_bar_width = allocation_with_buttons.width;
    state->control_bar_height = allocation_with_buttons.height;
    g_print("  Captured control bar size (with buttons): %dx%d\n",
            state->control_bar_width, state->control_bar_height);
    
    // Setup animation parameters
    MorphAnimation *anim = &state->morph_anim;
    anim->start_time = -1;
    anim->duration_ms = 300;
    anim->is_morphing_to_idle = TRUE;
    
    gtk_widget_add_css_class(state->control_bar_container, "no-transition");
    
    // GHOST OVERLAY: Sync icons then show visual fade effect
    if (state->ghost_overlay) {
        sync_ghost_button_icons(state);
        gtk_widget_set_visible(state->ghost_overlay, TRUE);
        gtk_widget_set_opacity(state->ghost_prev_btn,   1.0);
        gtk_widget_set_opacity(state->ghost_play_btn,   1.0);
        gtk_widget_set_opacity(state->ghost_next_btn,   1.0);
        gtk_widget_set_opacity(state->ghost_expand_btn, 1.0);
    }
    
    // Hide real buttons instantly
    gtk_widget_set_visible(state->prev_btn,   FALSE);
    gtk_widget_set_visible(state->play_btn,   FALSE);
    gtk_widget_set_visible(state->next_btn,   FALSE);
    gtk_widget_set_visible(state->expand_btn, FALSE);
    gtk_widget_set_opacity(state->prev_btn,   0.0);
    gtk_widget_set_opacity(state->play_btn,   0.0);
    gtk_widget_set_opacity(state->next_btn,   0.0);
    gtk_widget_set_opacity(state->expand_btn, 0.0);
    
    gtk_widget_queue_resize(state->control_bar_container);
    gtk_widget_queue_draw(state->control_bar_container);
    
    // Capture CURRENT size AFTER buttons are gone
    GtkAllocation allocation;
    gtk_widget_get_allocation(state->control_bar_container, &allocation);
    anim->start_width = allocation.width;
    anim->start_height = allocation.height;
    anim->end_width = 32;    // Vertical slim mode
    anim->end_height = 280;
    
    g_print("  Starting animation from ACTUAL size: %dx%d → 32x280\n", 
            anim->start_width, anim->start_height);
    
    // Vertical display opacity: start from current, animate to 1.0
    anim->start_viz_opacity = gtk_widget_get_opacity(state->vertical_display->container);
    anim->end_viz_opacity = 1.0;

    g_print("  vertical_display opacity start=%.3f end=%.3f\n",
            anim->start_viz_opacity, anim->end_viz_opacity);
    
    // Make vertical display visible (but transparent initially)
    gtk_widget_set_visible(state->vertical_display->container, TRUE);
    gtk_widget_set_opacity(state->vertical_display->container, 0.0);
    
    // Start animation loop at 60fps (same tick callback as horizontal!)
    state->morph_tick_id = gtk_widget_add_tick_callback(
        state->control_bar_container,
        morph_animation_tick_callback,
        state,
        NULL
    );
    
    state->idle_timer = 0;
    return G_SOURCE_REMOVE;
}

static void exit_vertical_idle_mode(AppState *state) {
    if (!state->is_idle_mode || !state->vertical_display) return;
    
    g_print("← Morphing to vertical control mode (Dynamic Island animation)\n");
    state->is_idle_mode = FALSE;
    
    // Cancel any existing animation
    if (state->morph_tick_id > 0) {
        gtk_widget_remove_tick_callback(state->control_bar_container, state->morph_tick_id);
        state->morph_tick_id = 0;
    }
    
    // Setup animation parameters (REVERSE of enter_vertical_idle_mode)
    MorphAnimation *anim = &state->morph_anim;
    anim->start_time = -1;
    anim->duration_ms = 300;
    anim->is_morphing_to_idle = FALSE;
    
    gtk_widget_add_css_class(state->control_bar_container, "no-transition");
    
    // GHOST OVERLAY: Sync icons but DON'T show yet - tick callback will show it
    if (state->ghost_overlay) {
        sync_ghost_button_icons(state);
        gtk_widget_set_opacity(state->ghost_prev_btn,   0.0);
        gtk_widget_set_opacity(state->ghost_play_btn,   0.0);
        gtk_widget_set_opacity(state->ghost_next_btn,   0.0);
        gtk_widget_set_opacity(state->ghost_expand_btn, 0.0);
    }
    
    // Real buttons stay hidden during entire animation
    
    // Capture CURRENT size (buttons are already hidden from enter)
    GtkAllocation allocation;
    gtk_widget_get_allocation(state->control_bar_container, &allocation);
    anim->start_width = allocation.width;
    anim->start_height = allocation.height;
    
    // Use the saved control bar size (with buttons) as target
    anim->end_width = state->control_bar_width;
    anim->end_height = state->control_bar_height;
    
    g_print("  Starting animation from ACTUAL size: %dx%d → %dx%d\n", 
            anim->start_width, anim->start_height,
            anim->end_width, anim->end_height);
    
    // Vertical display opacity: start from current, animate to 0.0
    anim->start_viz_opacity = gtk_widget_get_opacity(state->vertical_display->container);
    anim->end_viz_opacity = 0.0;

    g_print("  vertical_display opacity start=%.3f end=%.3f\n",
            anim->start_viz_opacity, anim->end_viz_opacity);
    
    // Start animation loop at 60fps (same tick callback!)
    state->morph_tick_id = gtk_widget_add_tick_callback(
        state->control_bar_container,
        morph_animation_tick_callback,
        state,
        NULL
    );
    
    // Restart idle timer after animation completes
    if (state->is_visible && !state->is_expanded && state->layout->is_vertical &&
        state->vertical_display && state->layout->vertical_display_enabled &&
        state->layout->vertical_display_scroll_interval > 0) {
        state->idle_timer = g_timeout_add_seconds(state->layout->vertical_display_scroll_interval, 
                                                   enter_vertical_idle_mode, state);
    }
}

static void reset_idle_timer(AppState *state) {
    // Cancel existing timer
    if (state->idle_timer > 0) {
        g_source_remove(state->idle_timer);
        state->idle_timer = 0;
    }
    
    // Exit idle mode if currently in it (restore buttons)
    if (state->is_idle_mode) {
        if (state->layout->is_vertical && state->vertical_display) {
            exit_vertical_idle_mode(state);
        } else {
            exit_idle_mode(state);
        }
        return;
    }
    
    // Start new idle timer based on layout
    if (state->is_visible && !state->is_expanded) {
        if (state->layout->is_vertical && state->vertical_display && 
            state->layout->vertical_display_enabled && 
            state->layout->vertical_display_scroll_interval > 0) {
            // Vertical: configurable idle timeout
            state->idle_timer = g_timeout_add_seconds(state->layout->vertical_display_scroll_interval, 
                                                       enter_vertical_idle_mode, state);
        } else if (!state->layout->is_vertical && state->visualizer &&
                   state->layout->visualizer_enabled && 
                   state->layout->visualizer_idle_timeout > 0) {
            // Horizontal: configurable timeout
            state->idle_timer = g_timeout_add_seconds(state->layout->visualizer_idle_timeout, 
                                                       enter_idle_mode, state);
        }
    }
}

// Mouse motion handler (for detecting user activity)
static gboolean on_mouse_motion(GtkEventControllerMotion *controller,
                                 gdouble x, gdouble y,
                                 gpointer user_data) {
    AppState *state = (AppState *)user_data;
    reset_idle_timer(state);
    return FALSE;
}


static gint64 get_variant_as_int64(GVariant *value) {
    if (value == NULL) return 0;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT64)) return g_variant_get_int64(value);
    else if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT64)) return (gint64)g_variant_get_uint64(value);
    else if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT32)) return (gint64)g_variant_get_int32(value);
    else if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) return (gint64)g_variant_get_uint32(value);
    else if (g_variant_is_of_type(value, G_VARIANT_TYPE_DOUBLE)) return (gint64)g_variant_get_double(value);
    return 0;
}

static gboolean update_position_tick(gpointer user_data) {
    AppState *state = (AppState *)user_data;
    update_position(state);
    return G_SOURCE_CONTINUE;
}

static gboolean clear_seeking_flag(gpointer user_data) {
    AppState *state = (AppState *)user_data;
    state->is_seeking = FALSE;
    return G_SOURCE_REMOVE;
}

static void perform_seek(AppState *state, gdouble fraction) {
    if (!state->mpris_proxy) return;
    
    GVariant *metadata = g_dbus_proxy_get_cached_property(state->mpris_proxy, "Metadata");
    if (!metadata) return;
    
    gint64 length = 0;
    const gchar *track_id = NULL;
    GVariantIter iter;
    gchar *key;
    GVariant *val;

    g_variant_iter_init(&iter, metadata);
    while (g_variant_iter_loop(&iter, "{sv}", &key, &val)) {
        if (g_strcmp0(key, "mpris:length") == 0) {
            length = get_variant_as_int64(val);
        } else if (g_strcmp0(key, "mpris:trackid") == 0) {
            track_id = g_variant_get_string(val, NULL);
        }
    }

    if (length > 0 && track_id) {
        gint64 target_position = (gint64)(fraction * length);
        g_dbus_proxy_call(state->mpris_proxy, "SetPosition",
            g_variant_new("(ox)", track_id, target_position),
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        g_print("Seeking to %.1f%% (position: %ld µs)\n", fraction * 100, target_position);
    }
    g_variant_unref(metadata);
}

static void on_change_value(GtkRange *range, GtkScrollType scroll, gdouble value, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    state->is_seeking = TRUE;
    
    if (state->mpris_proxy) {
        GVariant *metadata = g_dbus_proxy_get_cached_property(state->mpris_proxy, "Metadata");
        if (metadata) {
            gint64 length = 0;
            GVariantIter iter;
            gchar *key;
            GVariant *val;
            
            g_variant_iter_init(&iter, metadata);
            while (g_variant_iter_loop(&iter, "{sv}", &key, &val)) {
                if (g_strcmp0(key, "mpris:length") == 0) {
                    length = get_variant_as_int64(val);
                    break;
                }
            }
            g_variant_unref(metadata);

            if (length > 0) {
                gint64 target_pos = (gint64)(value * length);
                gint64 pos_seconds = target_pos / 1000000;
                gint64 len_seconds = length / 1000000;
                gint64 rem_seconds = len_seconds - pos_seconds;
                
                char time_str[32];
                if (rem_seconds >= 0) {
                    snprintf(time_str, sizeof(time_str), "-%ld:%02ld", 
                            rem_seconds / 60, rem_seconds % 60);
                } else {
                    snprintf(time_str, sizeof(time_str), "%ld:%02ld", 
                            pos_seconds / 60, pos_seconds % 60);
                }
                gtk_label_set_text(GTK_LABEL(state->time_remaining), time_str);
            }
        }
    }
}

static gboolean on_button_release_event(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    
    if (gdk_event_get_event_type(event) != GDK_BUTTON_RELEASE) {
        return FALSE;
    }
    
    gdouble value = gtk_range_get_value(GTK_RANGE(state->progress_bar));
    g_print("Button released - seeking to %.1f%%\n", value * 100);
    perform_seek(state, value);
    g_timeout_add(500, clear_seeking_flag, state);
    
    return FALSE;
}

static void on_position_received(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    GError *error = NULL;
    
    GVariant *position_container = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object), res, &error);
    if (error) {
        g_error_free(error);
        return;
    }
    
    GVariant *position_val_wrapped;
    g_variant_get(position_container, "(v)", &position_val_wrapped);
    gint64 position = get_variant_as_int64(position_val_wrapped);
    g_variant_unref(position_val_wrapped);
    g_variant_unref(position_container);
    
    gint64 length = 0;
    GVariant *metadata_var = g_dbus_proxy_get_cached_property(state->mpris_proxy, "Metadata");
    
    if (metadata_var) {
        GVariantIter iter;
        gchar *key;
        GVariant *val;
        g_variant_iter_init(&iter, metadata_var);
        while (g_variant_iter_loop(&iter, "{sv}", &key, &val)) {
            if (g_strcmp0(key, "mpris:length") == 0) {
                length = get_variant_as_int64(val);
                break;
            }
        }
        g_variant_unref(metadata_var);
    }

    char time_str[32];
    double fraction = 0.0;
    gint64 pos_seconds = position / 1000000;

    if (length > 0) {
        gint64 len_seconds = length / 1000000;
        gint64 rem_seconds = len_seconds - pos_seconds;
        if (rem_seconds < 0) rem_seconds = 0;
        int mins = rem_seconds / 60;
        int secs = rem_seconds % 60;
        snprintf(time_str, sizeof(time_str), "-%d:%02d", mins, secs);
        fraction = (double)position / (double)length;
    } else {
        int mins = pos_seconds / 60;
        int secs = pos_seconds % 60;
        snprintf(time_str, sizeof(time_str), "%d:%02d", mins, secs);
        fraction = 0.0;
    }

    if (fraction > 1.0) fraction = 1.0;
    if (fraction < 0.0) fraction = 0.0;

    gtk_label_set_text(GTK_LABEL(state->time_remaining), time_str);
    g_signal_handlers_block_by_func(state->progress_bar, on_change_value, state);
    gtk_range_set_value(GTK_RANGE(state->progress_bar), fraction);
    g_signal_handlers_unblock_by_func(state->progress_bar, on_change_value, state);
    
            if (state->vertical_display) {
        vertical_display_update_position(state->vertical_display, position, length);
    }
}

static void update_position(AppState *state) {
    if (state->is_seeking) return;
    if (!state->mpris_proxy) return;
    
    g_dbus_proxy_call(state->mpris_proxy,
        "org.freedesktop.DBus.Properties.Get",
        g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "Position"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_position_received, state);

}

static gint notification_retry_count = 0;
#define MAX_NOTIFICATION_RETRIES 5

static gboolean show_pending_notification(gpointer user_data) {
    AppState *state = (AppState *)user_data;
    gboolean has_title = state->pending_title && strlen(state->pending_title) > 0;
    gboolean has_artist = state->pending_artist && strlen(state->pending_artist) > 0;

    if (!has_title || !has_artist) {
        notification_retry_count++;
        if (notification_retry_count < MAX_NOTIFICATION_RETRIES) {
            state->notification_timer = g_timeout_add(200, show_pending_notification, state);
            return G_SOURCE_REMOVE;
        }
        g_print("Notification skipped - metadata incomplete\n");
    } else {
        notification_show(state->notification, state->pending_title,
                          state->pending_artist, state->pending_art_url, "Now Playing");
    }

    g_free(state->pending_title);
    g_free(state->pending_artist);
    g_free(state->pending_art_url);
    state->pending_title = NULL;
    state->pending_artist = NULL;
    state->pending_art_url = NULL;
    state->notification_timer = 0;
    notification_retry_count = 0;
    
    
    return G_SOURCE_REMOVE;
}

static void update_metadata(AppState *state) {
    if (!state->mpris_proxy) return;
    GVariant *metadata = g_dbus_proxy_get_cached_property(state->mpris_proxy, "Metadata");
    if (!metadata) return;

    GVariantIter iter;
    GVariant *value;
    gchar *key;
    gchar *title = NULL;
    gchar *artist = NULL;
    gchar *art_url = NULL;
    gchar *track_id = NULL;

    g_variant_iter_init(&iter, metadata);
    while (g_variant_iter_loop(&iter, "{sv}", &key, &value)) {
        if (g_strcmp0(key, "xesam:title") == 0) {
            g_free(title);
            title = g_strdup(g_variant_get_string(value, NULL));
        }
        else if (g_strcmp0(key, "xesam:artist") == 0) {
            if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING_ARRAY)) {
                gsize length;
                const gchar **artists = g_variant_get_strv(value, &length);
                if (length > 0) {
                    g_free(artist);
                    artist = g_strdup(artists[0]);
                }
                g_free(artists);
            }
        }
        else if (g_strcmp0(key, "mpris:artUrl") == 0) {
            g_free(art_url);
            art_url = g_strdup(g_variant_get_string(value, NULL));
        }
        else if (g_strcmp0(key, "mpris:trackid") == 0) {
            g_free(track_id);
            track_id = g_strdup(g_variant_get_string(value, NULL));
        }
    }
    
    gboolean track_changed = FALSE;
    if (track_id && state->last_track_id) {
        track_changed = (g_strcmp0(track_id, state->last_track_id) != 0);
    } else if (track_id && !state->last_track_id) {
        track_changed = TRUE;
    }
    
    if (track_id) {
        g_free(state->last_track_id);
        state->last_track_id = g_strdup(track_id);
    }
    
    if (state->layout->notifications_enabled && state->layout->now_playing_enabled && 
        state->notification && track_changed) {
        if (state->notification_timer > 0) {
            g_source_remove(state->notification_timer);
            state->notification_timer = 0;
        }
        notification_retry_count = 0;
        g_free(state->pending_title);
        g_free(state->pending_artist);
        g_free(state->pending_art_url);
        state->pending_title = g_strdup(title);
        state->pending_artist = g_strdup(artist);
        state->pending_art_url = g_strdup(art_url);
        if (state->notification->album_cover) {
            clear_album_art_container(state->notification->album_cover);
            load_album_art_to_container(art_url, state->notification->album_cover, 70);
        }
        state->notification_timer = g_timeout_add(300, show_pending_notification, state);
    }
    
    if (title && strlen(title) > 0) {
        gtk_label_set_text(GTK_LABEL(state->track_title), title);
    } else {
        gtk_label_set_text(GTK_LABEL(state->track_title), "No Track Playing");
    }
    
    if (artist && strlen(artist) > 0) {
        gtk_label_set_text(GTK_LABEL(state->artist_label), artist);
    } else {
        gtk_label_set_text(GTK_LABEL(state->artist_label), "Unknown Artist");
    }
    
    // CRITICAL: Clear old album art before loading new to prevent memory leak
    clear_album_art_container(state->album_cover);
    load_album_art_to_container(art_url, state->album_cover, 120);
    
    if (state->current_player) {
        GError *error = NULL;
        GDBusProxy *player_proxy = g_dbus_proxy_new_for_bus_sync(
            G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
            state->current_player, "/org/mpris/MediaPlayer2",
            "org.mpris.MediaPlayer2", NULL, &error);

        if (player_proxy && !error) {
            GVariant *identity = g_dbus_proxy_get_cached_property(player_proxy, "Identity");
            if (identity) {
                gtk_label_set_text(GTK_LABEL(state->source_label), 
                                   g_variant_get_string(identity, NULL));
                g_variant_unref(identity);
            }
            g_object_unref(player_proxy);
        } else if (error) {
            g_error_free(error);
        }
    }
    
        if (state->vertical_display && title && artist) {
        vertical_display_update_track(state->vertical_display, title, artist);
    }
    

    g_free(title);
    g_free(artist);
    g_free(art_url);
    g_free(track_id);
    g_variant_unref(metadata);
    update_position(state);
    
}

static void update_playback_status(AppState *state) {
    if (!state->mpris_proxy) return;
    GVariant *status_var = g_dbus_proxy_get_cached_property(state->mpris_proxy, "PlaybackStatus");
    if (status_var) {
        const gchar *status = g_variant_get_string(status_var, NULL);
        gboolean was_playing = state->is_playing;
        state->is_playing = g_strcmp0(status, "Playing") == 0;
        
        gchar *icon_path = get_icon_path(state->is_playing ? "pause.svg" : "play.svg");
        gtk_image_set_from_file(GTK_IMAGE(state->play_icon), icon_path);
        free_path(icon_path);
        
        // UPDATE VERTICAL DISPLAY
        if (state->vertical_display) {
            // Only notify if status changed
            if (was_playing != state->is_playing) {
                vertical_display_set_paused(state->vertical_display, !state->is_playing);
            }
        }
        
        g_variant_unref(status_var);
    }
}

static void on_properties_changed(GDBusProxy *proxy, GVariant *changed_properties,
                                  GStrv invalidated_properties, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    update_metadata(state);
    update_playback_status(state);
}

// Callback when player name appears/disappears on D-Bus
static void on_player_name_changed(GDBusConnection *connection,
                                    const gchar *sender_name,
                                    const gchar *object_path,
                                    const gchar *interface_name,
                                    const gchar *signal_name,
                                    GVariant *parameters,
                                    gpointer user_data) {
    AppState *state = (AppState *)user_data;
    
    const gchar *name;
    const gchar *old_owner;
    const gchar *new_owner;
    g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
    
    // Check if this is our current player
    if (state->current_player && g_strcmp0(name, state->current_player) == 0) {
        if (strlen(new_owner) == 0) {
            // Our player disappeared!
            g_print("⚠ Player disappeared: %s\n", state->current_player);
            
            if (state->mpris_proxy) {
                g_object_unref(state->mpris_proxy);
                state->mpris_proxy = NULL;
            }
            g_free(state->current_player);
            state->current_player = NULL;
            
            // Clear UI
            gtk_label_set_text(GTK_LABEL(state->track_title), "No Player");
            gtk_label_set_text(GTK_LABEL(state->artist_label), "Waiting for music...");
            gtk_label_set_text(GTK_LABEL(state->source_label), "");
            clear_album_art_container(state->album_cover);
            
            // Try to reconnect after 2 seconds
            if (state->reconnect_timer > 0) {
                g_source_remove(state->reconnect_timer);
            }
            state->reconnect_timer = g_timeout_add_seconds(2, (GSourceFunc)find_active_player, state);
        }
    } else if (!state->current_player && g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) {
        // A new player appeared and we're not connected to anything
        if (strlen(new_owner) > 0) {
            g_print("✓ New player detected: %s\n", name);
            find_active_player(state);
        }
    }
}

static void connect_to_player(AppState *state, const gchar *bus_name) {
    if (state->mpris_proxy) g_object_unref(state->mpris_proxy);
    g_free(state->current_player);
    state->current_player = g_strdup(bus_name);

    GError *error = NULL;
    state->mpris_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, bus_name,
        "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", NULL, &error);

    if (error) {
        g_printerr("Failed to connect: %s\n", error->message);
        g_error_free(error);
        return;
    }

    g_signal_connect(state->mpris_proxy, "g-properties-changed",
                     G_CALLBACK(on_properties_changed), state);
    update_metadata(state);
    
    if (state->volume) {
        state->volume->mpris_proxy = state->mpris_proxy;
    }
    
    g_print("Connected to player: %s\n", bus_name);
}

static void find_active_player(AppState *state) {
    GError *error = NULL;
    GDBusProxy *dbus_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", NULL, &error);

    if (error) {
        g_error_free(error);
        return;
    }

    GVariant *result = g_dbus_proxy_call_sync(dbus_proxy, "ListNames", NULL,
                                              G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (error) {
        g_error_free(error);
        g_object_unref(dbus_proxy);
        return;
    }

    // Get list of all available MPRIS players
    GVariantIter *iter;
    g_variant_get(result, "(as)", &iter);
    
    // Build array of available players
    GPtrArray *available_players = g_ptr_array_new_with_free_func(g_free);
    const gchar *name;
    while (g_variant_iter_loop(iter, "&s", &name)) {
        if (g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) {
            // Extract player name (e.g., "spotify" from "org.mpris.MediaPlayer2.spotify")
            const gchar *player_name = name + strlen("org.mpris.MediaPlayer2.");
            g_ptr_array_add(available_players, g_strdup(name));  // Store full bus name
        }
    }
    g_variant_iter_free(iter);
    g_variant_unref(result);
    
    if (available_players->len == 0) {
        g_ptr_array_free(available_players, TRUE);
        g_object_unref(dbus_proxy);
        g_print("No MPRIS players found\n");
        return;
    }
    
    const gchar *selected_player = NULL;
    
    // If we have preferences, search in order
    if (state->layout->player_preference && state->layout->player_preference_count > 0) {
        g_print("Searching for preferred players...\n");
        
        for (gint i = 0; i < state->layout->player_preference_count; i++) {
            const gchar *pref = state->layout->player_preference[i];
            g_print("  Looking for: %s\n", pref);
            
            // Check each available player
            for (guint j = 0; j < available_players->len; j++) {
                const gchar *bus_name = g_ptr_array_index(available_players, j);
                const gchar *player_name = bus_name + strlen("org.mpris.MediaPlayer2.");
                
                // Match preference (case-insensitive, partial match)
                if (g_ascii_strcasecmp(player_name, pref) == 0 ||
                    g_str_has_prefix(player_name, pref)) {
                    selected_player = bus_name;
                    g_print("✓ Found preferred player: %s (%s)\n", pref, bus_name);
                    break;
                }
            }
            
            if (selected_player) break;  // Found a match, stop searching
        }
        
        if (!selected_player) {
            g_print("⚠ No preferred players found, skipping connection\n");
        }
    } else {
        // No preference - connect to first available player (old behavior)
        selected_player = g_ptr_array_index(available_players, 0);
        g_print("No player preference set, connecting to: %s\n", selected_player);
    }
    
    if (selected_player) {
        connect_to_player(state, selected_player);
    }
    
    g_ptr_array_free(available_players, TRUE);
    g_object_unref(dbus_proxy);
}

static void on_play_clicked(GtkButton *button, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    if (!state->mpris_proxy) {
        find_active_player(state);
        return;
    }
    g_dbus_proxy_call(state->mpris_proxy, "PlayPause", NULL,
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void on_next_clicked(GtkButton *button, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    if (!state->mpris_proxy) return;
    
    // Notify vertical display about skip
    if (state->vertical_display) {
        vertical_display_notify_skip(state->vertical_display);
    }
    
    g_dbus_proxy_call(state->mpris_proxy, "Next", NULL,
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void on_prev_clicked(GtkButton *button, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    if (!state->mpris_proxy) return;
    
    // Notify vertical display about skip
    if (state->vertical_display) {
        vertical_display_notify_skip(state->vertical_display);
    }
    
    g_dbus_proxy_call(state->mpris_proxy, "Previous", NULL,
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void on_expand_clicked(GtkButton *button, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    
    // SAFETY: Don't process if already transitioning
    if (state->morph_timer > 0) {
        g_print("⸻ Expand blocked - morph animation in progress\n");
        return;
    }
    
    // Exit idle mode first if active (mouse click path)
    if (state->is_idle_mode) {
        exit_idle_mode(state);
        // Don't toggle expansion while exiting idle mode
        return;
    }
    
    state->is_expanded = !state->is_expanded;
    
    if (!state->is_expanded && state->volume && state->volume->is_showing) {
        volume_hide(state->volume);
    }
    
    const gchar *icon_name = layout_get_expand_icon(state->layout, state->is_expanded);
    gchar *icon_path = get_icon_path(icon_name);
    gtk_image_set_from_file(GTK_IMAGE(state->expand_icon), icon_path);
    free_path(icon_path);
    gtk_revealer_set_reveal_child(GTK_REVEALER(state->revealer), state->is_expanded);
    
    // MANAGE IDLE TIMER:
    if (state->is_expanded) {
        // Cancel idle timer when expanded
        if (state->idle_timer > 0) {
            g_source_remove(state->idle_timer);
            state->idle_timer = 0;
        }
    } else {
        // Restart idle timer when collapsed (direct, not via reset_idle_timer)
        if (state->is_visible && !state->layout->is_vertical && state->visualizer &&
            state->layout->visualizer_enabled && state->layout->visualizer_idle_timeout > 0) {
            state->idle_timer = g_timeout_add_seconds(state->layout->visualizer_idle_timeout, 
                                                       enter_idle_mode, state);
        }
    }
}

static void on_album_double_click(GtkGestureClick *gesture,
                                   int n_press,
                                   double x, double y,
                                   gpointer user_data) {
    AppState *state = (AppState *)user_data;
    
    if (n_press == 2 && state->volume) {
        if (state->volume->is_showing) {
            volume_hide(state->volume);
            g_print("Volume control hidden via double-click\n");
        } else {
            volume_show(state->volume);
            g_print("Volume control activated via double-click\n");
        }
    }
}

static gboolean enable_smooth_transitions(gpointer user_data) {
    GtkWidget *window_revealer = GTK_WIDGET(user_data);
    gtk_revealer_set_transition_duration(GTK_REVEALER(window_revealer), 300);
    g_print("Smooth transitions enabled\n");
    return G_SOURCE_REMOVE;
}

static void on_volume_visibility_changed(GObject *revealer, GParamSpec *pspec, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    
    if (!state->layout->is_vertical && state->expanded_with_volume) {
        gtk_widget_queue_resize(state->expanded_with_volume);
        gtk_widget_queue_allocate(state->expanded_with_volume);
    }
}

static void load_css() {
    gchar *css_path = get_style_path();
    GtkCssProvider *provider = gtk_css_provider_new();
    GFile *base_file = g_file_new_for_path(css_path);
    GError *base_error = NULL;
    gchar *base_contents = NULL;
    gsize base_length = 0;

    if (g_file_load_contents(base_file, NULL, &base_contents, &base_length, NULL, &base_error)) {
        gtk_css_provider_load_from_string(provider, base_contents);
gtk_style_context_add_provider_for_display(gdk_display_get_default(),
    GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
        g_free(base_contents);
    } else {
        g_warning("Failed to load CSS: %s", base_error ? base_error->message : "Unknown");
        if (base_error) g_error_free(base_error);
    }
    g_object_unref(base_file);
    g_object_unref(provider);
    free_path(css_path);
}

static gboolean delayed_window_show(gpointer user_data) {
    gtk_widget_set_visible(GTK_WIDGET(user_data), TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean delayed_revealer_reveal(gpointer user_data) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(user_data), TRUE);
    return G_SOURCE_REMOVE;
}


static void activate(GtkApplication *app, gpointer user_data) {
    AppState *state = g_new0(AppState, 1);
    state->is_playing = FALSE;
    state->is_expanded = FALSE;
    state->is_visible = TRUE;
    state->is_seeking = FALSE;
    state->mpris_proxy = NULL;
    state->current_player = NULL;
    state->last_track_id = NULL;
    state->layout = layout_load_config();
    state->notification = notification_init(app);
    state->volume = NULL;
    state->is_idle_mode = FALSE;
    state->idle_timer = 0;
    state->morph_timer = 0;
    state->button_fade_opacity = 1.0;
    
    // Create window FIRST
    GtkWidget *window = gtk_application_window_new(app);
    state->window = window;
    gtk_window_set_title(GTK_WINDOW(window), "HyprWave");
    
    // Set window size IMMEDIATELY to match control_bar
    if (state->layout->is_vertical) {
        gtk_window_set_default_size(GTK_WINDOW(window), 70, -1);
        gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    } else {
        gtk_window_set_default_size(GTK_WINDOW(window), -1, 60);
        gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    }
    
    // LAYER SHELL SETUP
    gtk_layer_init_for_window(GTK_WINDOW(window));
    gtk_layer_set_layer(GTK_WINDOW(window), state->layout->layer);
    gtk_layer_set_namespace(GTK_WINDOW(window), "hyprwave");
    layout_setup_window_anchors(GTK_WINDOW(window), state->layout);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(window), state->layout->exclusive_zone);
    gtk_widget_set_name(window, "hyprwave-window");
    gtk_widget_add_css_class(window, "hyprwave-window");
    
    // Album cover setup
    GtkWidget *album_cover = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    state->album_cover = album_cover;
    gtk_widget_add_css_class(album_cover, "album-cover");
    gtk_widget_set_size_request(album_cover, 120, 120);
    gtk_widget_set_halign(album_cover, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(album_cover, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(album_cover, FALSE);
    gtk_widget_set_vexpand(album_cover, FALSE);
    gtk_widget_set_overflow(album_cover, GTK_OVERFLOW_HIDDEN);
    
    GtkGesture *double_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(double_click), GDK_BUTTON_PRIMARY);
    gtk_widget_add_controller(album_cover, GTK_EVENT_CONTROLLER(double_click));
    g_signal_connect(double_click, "pressed", G_CALLBACK(on_album_double_click), state);
    
    GtkWidget *source_label = gtk_label_new("No Source");
    state->source_label = source_label;
    gtk_widget_add_css_class(source_label, "source-label");
    
    GtkWidget *track_title = gtk_label_new("No Track Playing");
    state->track_title = track_title;
    gtk_widget_add_css_class(track_title, "track-title");
    gtk_label_set_ellipsize(GTK_LABEL(track_title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(track_title), 20);

    GtkWidget *artist_label = gtk_label_new("Unknown Artist");
    state->artist_label = artist_label;
    gtk_widget_add_css_class(artist_label, "artist-label");
    gtk_label_set_ellipsize(GTK_LABEL(artist_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(artist_label), 20);

    GtkWidget *progress_bar = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.001);
    state->progress_bar = progress_bar;
    gtk_widget_add_css_class(progress_bar, "track-progress");
    gtk_scale_set_draw_value(GTK_SCALE(progress_bar), FALSE);
    gtk_widget_set_size_request(progress_bar, 140, 14);
    g_signal_connect(progress_bar, "change-value", G_CALLBACK(on_change_value), state);
    GtkEventController *controller = gtk_event_controller_legacy_new();
    g_signal_connect(controller, "event", G_CALLBACK(on_button_release_event), state);
    gtk_widget_add_controller(progress_bar, controller);

    GtkWidget *time_remaining = gtk_label_new("--:--");
    state->time_remaining = time_remaining;
    gtk_widget_add_css_class(time_remaining, "time-remaining");

    ExpandedWidgets expanded_widgets = {
        .album_cover = album_cover, .source_label = source_label,
        .track_title = track_title, .artist_label = artist_label,
        .progress_bar = progress_bar, .time_remaining = time_remaining
    };
    GtkWidget *expanded_section = layout_create_expanded_section(state->layout, &expanded_widgets);

    // Initialize volume
    state->volume = volume_init(NULL, state->layout->is_vertical);

    GtkWidget *expanded_with_volume;
    if (state->layout->is_vertical) {
        expanded_with_volume = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_append(GTK_BOX(expanded_with_volume), state->volume->revealer);
        gtk_box_append(GTK_BOX(expanded_with_volume), expanded_section);
        gtk_widget_set_size_request(expanded_with_volume, -1, 160);
        gtk_widget_set_vexpand(expanded_section, TRUE);
        gtk_widget_set_vexpand(state->volume->revealer, FALSE);
    } else {
        expanded_with_volume = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_append(GTK_BOX(expanded_with_volume), expanded_section);
        gtk_box_append(GTK_BOX(expanded_with_volume), state->volume->revealer);
    }
    
    state->expanded_with_volume = expanded_with_volume;
    g_signal_connect(state->volume->revealer, "notify::child-revealed",
                     G_CALLBACK(on_volume_visibility_changed), state);

    GtkWidget *revealer = gtk_revealer_new();
    state->revealer = revealer;
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), layout_get_transition_type(state->layout));
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 300);
    gtk_revealer_set_child(GTK_REVEALER(revealer), expanded_with_volume);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
    g_signal_connect(revealer, "notify::child-revealed", G_CALLBACK(on_revealer_transition_done), state);

    // ========================================
    // CONTROL BUTTONS - Create all buttons
    // ========================================
    GtkWidget *prev_btn = gtk_button_new();
    gtk_widget_set_size_request(prev_btn, 44, 44);
    gchar *prev_icon_path = get_icon_path("previous.svg");
    GtkWidget *prev_icon = gtk_image_new_from_file(prev_icon_path);
    free_path(prev_icon_path);
    gtk_image_set_pixel_size(GTK_IMAGE(prev_icon), 20);
    gtk_button_set_child(GTK_BUTTON(prev_btn), prev_icon);
    gtk_widget_add_css_class(prev_btn, "control-button");
    gtk_widget_add_css_class(prev_btn, "prev-button");
    g_signal_connect(prev_btn, "clicked", G_CALLBACK(on_prev_clicked), state);

    GtkWidget *play_btn = gtk_button_new();
    gtk_widget_set_size_request(play_btn, 44, 44);
    gchar *play_icon_path = get_icon_path("play.svg");
    GtkWidget *play_icon = gtk_image_new_from_file(play_icon_path);
    free_path(play_icon_path);
    state->play_icon = play_icon;
    gtk_image_set_pixel_size(GTK_IMAGE(play_icon), 20);
    gtk_button_set_child(GTK_BUTTON(play_btn), play_icon);
    gtk_widget_add_css_class(play_btn, "control-button");
    gtk_widget_add_css_class(play_btn, "play-button");
    g_signal_connect(play_btn, "clicked", G_CALLBACK(on_play_clicked), state);

    GtkWidget *next_btn = gtk_button_new();
    gtk_widget_set_size_request(next_btn, 44, 44);
    gchar *next_icon_path = get_icon_path("next.svg");
    GtkWidget *next_icon = gtk_image_new_from_file(next_icon_path);
    free_path(next_icon_path);
    gtk_image_set_pixel_size(GTK_IMAGE(next_icon), 20);
    gtk_button_set_child(GTK_BUTTON(next_btn), next_icon);
    gtk_widget_add_css_class(next_btn, "control-button");
    gtk_widget_add_css_class(next_btn, "next-button");
    g_signal_connect(next_btn, "clicked", G_CALLBACK(on_next_clicked), state);

    GtkWidget *expand_btn = gtk_button_new();
    gtk_widget_set_size_request(expand_btn, 44, 44);
    const gchar *initial_icon_name = layout_get_expand_icon(state->layout, FALSE);
    gchar *expand_icon_path = get_icon_path(initial_icon_name);
    GtkWidget *expand_icon = gtk_image_new_from_file(expand_icon_path);
    free_path(expand_icon_path);
    state->expand_icon = expand_icon;
    gtk_image_set_pixel_size(GTK_IMAGE(expand_icon), 20);
    gtk_button_set_child(GTK_BUTTON(expand_btn), expand_icon);
    gtk_widget_add_css_class(expand_btn, "control-button");
    gtk_widget_add_css_class(expand_btn, "expand-button");
    g_signal_connect(expand_btn, "clicked", G_CALLBACK(on_expand_clicked), state);
    
    // Store button references in state
    state->prev_btn = prev_btn;
    state->play_btn = play_btn;
    state->next_btn = next_btn;
    state->expand_btn = expand_btn;

    // ========================================
    // CONTROL BAR + VISUALIZER SETUP
    // ========================================
    GtkWidget *final_control_widget;  // What goes into main_container
    
if (state->layout->is_vertical) {
    // Vertical: Create vertical display if enabled
    if (state->layout->vertical_display_enabled) {
        state->vertical_display = vertical_display_init();
    } else {
        state->vertical_display = NULL;
    }
    state->visualizer = NULL;
    
    GtkWidget *control_bar = layout_create_control_bar(state->layout, 
        &prev_btn, &play_btn, &next_btn, &expand_btn);
    
    state->control_bar_container = control_bar;
    
    if (state->vertical_display) {
        // Create overlay: control bar as base, vertical display on top
        GtkWidget *overlay = gtk_overlay_new();
        gtk_overlay_set_child(GTK_OVERLAY(overlay), control_bar);
        
        // Vertical display must pass through clicks
        gtk_widget_set_can_target(state->vertical_display->container, FALSE);
        
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), state->vertical_display->container);
        
        // Start hidden and transparent
        gtk_widget_set_visible(state->vertical_display->container, TRUE);
        gtk_widget_set_opacity(state->vertical_display->container, 0.0);
        
        // GHOST BUTTON OVERLAY for vertical mode (same as horizontal)
        GtkWidget *ghost_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        state->ghost_overlay = ghost_box;
        gtk_widget_add_css_class(ghost_box, "control-bar");
        gtk_widget_set_halign(ghost_box, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(ghost_box, GTK_ALIGN_CENTER);
        gtk_widget_set_can_target(ghost_box, FALSE);
        
        // Create ghost buttons
        state->ghost_prev_btn = gtk_button_new();
        state->ghost_play_btn = gtk_button_new();
        state->ghost_next_btn = gtk_button_new();
        state->ghost_expand_btn = gtk_button_new();
        
        GtkWidget *ghost_buttons[] = {
            state->ghost_prev_btn,
            state->ghost_play_btn,
            state->ghost_next_btn,
            state->ghost_expand_btn
        };
        
        const gchar *ghost_icons[] = {
            "previous.svg", "play.svg", "next.svg",
            layout_get_expand_icon(state->layout, FALSE)
        };
        
        for (int i = 0; i < 4; i++) {
            gtk_widget_add_css_class(ghost_buttons[i], "control-button");
            gtk_widget_set_size_request(ghost_buttons[i], 44, 44);
            gtk_widget_set_can_target(ghost_buttons[i], FALSE);
            
            gchar *icon_path = get_icon_path(ghost_icons[i]);
            GtkWidget *icon = gtk_image_new_from_file(icon_path);
            free_path(icon_path);
            gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);
            gtk_button_set_child(GTK_BUTTON(ghost_buttons[i]), icon);
            
            gtk_box_append(GTK_BOX(ghost_box), ghost_buttons[i]);
        }
        
        gtk_widget_set_visible(ghost_box, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), ghost_box);
        
        g_print("✓ Ghost button overlay created for vertical mode\n");
        
        final_control_widget = overlay;
    } else {
        state->ghost_overlay = NULL;
        final_control_widget = control_bar;
      }
    
}  else {
        // Horizontal: Create visualizer if enabled, otherwise just buttons
        if (state->layout->visualizer_enabled) {
            state->visualizer = visualizer_init();
        } else {
            state->visualizer = NULL;
        }
        
        // Create control bar
        GtkWidget *control_bar = layout_create_control_bar(state->layout, 
            &prev_btn, &play_btn, &next_btn, &expand_btn);
        
        // CRITICAL: Store reference to the ACTUAL control bar for resizing
        state->control_bar_container = control_bar;
        
        if (state->visualizer) {
            // Create overlay: control bar as base, visualizer on top
            GtkWidget *overlay = gtk_overlay_new();
            gtk_overlay_set_child(GTK_OVERLAY(overlay), control_bar);
            
            // CRITICAL: Visualizer must pass through clicks to buttons below
            gtk_widget_set_can_target(state->visualizer->container, FALSE);
            
            gtk_overlay_add_overlay(GTK_OVERLAY(overlay), state->visualizer->container);
            
            // Visualizer starts hidden and fully transparent
            gtk_widget_set_visible(state->visualizer->container, TRUE);
            gtk_widget_set_opacity(state->visualizer->container, 0.0);
            state->visualizer->fade_opacity = 0.0;  // Start at 0
            
            // GHOST BUTTON OVERLAY: Purely visual fade effect during morph
            // This overlay sits on top of everything and fades while real buttons pop in/out
            GtkWidget *ghost_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            state->ghost_overlay = ghost_box;
            gtk_widget_add_css_class(ghost_box, "control-bar");
            gtk_widget_set_halign(ghost_box, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(ghost_box, GTK_ALIGN_CENTER);
            gtk_widget_set_can_target(ghost_box, FALSE);  // Pass through clicks
            
            // Create ghost buttons (visual copies)
            state->ghost_prev_btn = gtk_button_new();
            state->ghost_play_btn = gtk_button_new();
            state->ghost_next_btn = gtk_button_new();
            state->ghost_expand_btn = gtk_button_new();
            
            GtkWidget *ghost_buttons[] = {
                state->ghost_prev_btn,
                state->ghost_play_btn,
                state->ghost_next_btn,
                state->ghost_expand_btn
            };
            
            const gchar *ghost_icons[] = {
                "previous.svg", "play.svg", "next.svg",
                layout_get_expand_icon(state->layout, FALSE)
            };
            
            for (int i = 0; i < 4; i++) {
                gtk_widget_add_css_class(ghost_buttons[i], "control-button");
                gtk_widget_set_size_request(ghost_buttons[i], 44, 44);
                gtk_widget_set_can_target(ghost_buttons[i], FALSE);  // Not clickable
                
                gchar *icon_path = get_icon_path(ghost_icons[i]);
                GtkWidget *icon = gtk_image_new_from_file(icon_path);
                free_path(icon_path);
                gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);
                gtk_button_set_child(GTK_BUTTON(ghost_buttons[i]), icon);
                
                gtk_box_append(GTK_BOX(ghost_box), ghost_buttons[i]);
            }
            
            // Start hidden
            gtk_widget_set_visible(ghost_box, FALSE);
            
            // Add ghost overlay on top
            gtk_overlay_add_overlay(GTK_OVERLAY(overlay), ghost_box);
            
            g_print("✓ Ghost button overlay created\n");
            
            // Use overlay as the widget that goes into main_container
            final_control_widget = overlay;
        } else {
            // No visualizer - just use control bar directly
            state->ghost_overlay = NULL;  // No morph animation without visualizer
            final_control_widget = control_bar;
        }
    }
    
    // Create main container
    GtkWidget *main_container = layout_create_main_container(state->layout, 
        final_control_widget, revealer);

    // ========================================
    // WINDOW REVEALER
    // ========================================
    GtkWidget *window_revealer = gtk_revealer_new();
    state->window_revealer = window_revealer;
    
    GtkRevealerTransitionType window_transition;
    if (state->layout->edge == EDGE_RIGHT) {
        window_transition = GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT;
    } else if (state->layout->edge == EDGE_LEFT) {
        window_transition = GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT;
    } else if (state->layout->edge == EDGE_TOP) {
        window_transition = GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN;
    } else {
        window_transition = GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP;
    }
    gtk_revealer_set_transition_type(GTK_REVEALER(window_revealer), window_transition);
    gtk_revealer_set_transition_duration(GTK_REVEALER(window_revealer), 300);
    gtk_revealer_set_child(GTK_REVEALER(window_revealer), main_container);
    gtk_revealer_set_reveal_child(GTK_REVEALER(window_revealer), FALSE);
    g_signal_connect(window_revealer, "notify::child-revealed", 
                     G_CALLBACK(on_window_hide_complete), state);

    gtk_window_set_child(GTK_WINDOW(window), window_revealer);

    // ========================================
    // PRE-WARM REVEALERS (for smooth animations)
    // ========================================
    gtk_widget_realize(window);
    gtk_window_present(GTK_WINDOW(window));
    
    while (g_main_context_pending(NULL)) {
        g_main_context_iteration(NULL, FALSE);
    }
    
    guint window_duration = gtk_revealer_get_transition_duration(GTK_REVEALER(window_revealer));
    guint internal_duration = gtk_revealer_get_transition_duration(GTK_REVEALER(revealer));
    gtk_revealer_set_transition_duration(GTK_REVEALER(window_revealer), 0);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 0);
    
    // Handle --start-hidden flag for smooth position changes
    if (start_hidden) {
        // Start hidden, restore transition, then reveal after 100ms
        gtk_revealer_set_reveal_child(GTK_REVEALER(window_revealer), FALSE);
        gtk_revealer_set_transition_duration(GTK_REVEALER(window_revealer), window_duration);
        gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), internal_duration);
        g_timeout_add(100, delayed_reveal_window, state);
    } else {
        // Normal startup - show immediately
        gtk_revealer_set_reveal_child(GTK_REVEALER(window_revealer), TRUE);
    }
    gtk_widget_queue_allocate(window);
    while (g_main_context_pending(NULL)) {
        g_main_context_iteration(NULL, FALSE);
    }
    
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), TRUE);
    gtk_widget_queue_allocate(window);
    while (g_main_context_pending(NULL)) {
        g_main_context_iteration(NULL, FALSE);
    }
    
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
    gtk_widget_queue_allocate(window);
    while (g_main_context_pending(NULL)) {
        g_main_context_iteration(NULL, FALSE);
    }
    state->morph_easing = EASE_IN_OUT_CUBIC;  // Dynamic Island style: slow-fast-slow
    memset(&state->morph_anim, 0, sizeof(MorphAnimation));
    g_print("✓ Dynamic Island animation system initialized (easing: OUT_SINE)\n");
    
    gtk_revealer_set_transition_duration(GTK_REVEALER(window_revealer), window_duration);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), internal_duration);
    
// Around line 950, replace the motion controller setup with:

    if (state->layout->is_vertical && state->vertical_display) {
        GtkEventController *motion_controller = gtk_event_controller_motion_new();
        g_signal_connect(motion_controller, "motion", G_CALLBACK(on_mouse_motion), state);
        gtk_widget_add_controller(state->control_bar_container, motion_controller);
        g_print("✓ Mouse motion detector attached to vertical control bar\n");
    } else if (!state->layout->is_vertical && state->visualizer) {
        GtkEventController *motion_controller = gtk_event_controller_motion_new();
        g_signal_connect(motion_controller, "motion", G_CALLBACK(on_mouse_motion), state);
        gtk_widget_add_controller(state->control_bar_container, motion_controller);
        g_print("✓ Mouse motion detector attached to horizontal control bar\n");
    }


    global_state = state;
    signal(SIGUSR1, handle_sigusr1);
    signal(SIGUSR2, handle_sigusr2);
    signal(SIGRTMIN, handle_sigrtmin);
    signal(SIGRTMIN+1, handle_sigrtmin1);
    signal(SIGRTMIN+2, handle_sigrtmin2);

    // Setup D-Bus name watcher to monitor player appearance/disappearance
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (bus) {
        state->dbus_watch_id = g_dbus_connection_signal_subscribe(
            bus,
            "org.freedesktop.DBus",
            "org.freedesktop.DBus",
            "NameOwnerChanged",
            "/org/freedesktop/DBus",
            NULL,
            G_DBUS_SIGNAL_FLAGS_NONE,
            on_player_name_changed,
            state,
            NULL
        );
        g_print("✓ D-Bus name watcher enabled\n");
    }

    find_active_player(state);
    state->update_timer = g_timeout_add_seconds(1, update_position_tick, state);
    
    // Start idle timer based on layout
    if (state->layout->is_vertical && state->vertical_display && 
        state->layout->vertical_display_enabled &&
        state->layout->vertical_display_scroll_interval > 0) {
        reset_idle_timer(state);
    } else if (!state->layout->is_vertical && state->visualizer && 
               state->layout->visualizer_enabled && 
               state->layout->visualizer_idle_timeout > 0) {
        reset_idle_timer(state);
    }
    
    // Connect shutdown callback for cleanup
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), state);
    g_print("✓ Shutdown callback registered\n");
}

// Cleanup callback - prevents memory leaks
static void on_shutdown(GtkApplication *app, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    if (!state) return;
    
    g_print("🧹 Cleaning up HyprWave...\n");
    
    // Stop all timers
    if (state->update_timer > 0) {
        g_source_remove(state->update_timer);
        state->update_timer = 0;
    }
    if (state->idle_timer > 0) {
        g_source_remove(state->idle_timer);
        state->idle_timer = 0;
    }
    if (state->morph_tick_id > 0) {
        gtk_widget_remove_tick_callback(state->control_bar_container, state->morph_tick_id);
        state->morph_tick_id = 0;
    }
    if (state->morph_timer > 0) {
        g_source_remove(state->morph_timer);
        state->morph_timer = 0;
    }
    if (state->notification_timer > 0) {
        g_source_remove(state->notification_timer);
        state->notification_timer = 0;
    }
    if (state->reconnect_timer > 0) {
        g_source_remove(state->reconnect_timer);
        state->reconnect_timer = 0;
    }
    
    // Cleanup subsystems
    if (state->visualizer) {
        visualizer_cleanup(state->visualizer);
        state->visualizer = NULL;
    }
    if (state->vertical_display) {
        vertical_display_cleanup(state->vertical_display);
        state->vertical_display = NULL;
    }
    if (state->volume) {
        volume_cleanup(state->volume);
        state->volume = NULL;
    }
    if (state->notification) {
        notification_cleanup(state->notification);
        state->notification = NULL;
    }
    
    // Free D-Bus resources
    if (state->dbus_watch_id > 0) {
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        if (bus) {
            g_dbus_connection_signal_unsubscribe(bus, state->dbus_watch_id);
        }
        state->dbus_watch_id = 0;
    }
    if (state->mpris_proxy) {
        g_object_unref(state->mpris_proxy);
        state->mpris_proxy = NULL;
    }
    
    // Free strings
    g_free(state->current_player);
    g_free(state->last_track_id);
    g_free(state->pending_title);
    g_free(state->pending_artist);
    g_free(state->pending_art_url);
    
    state->current_player = NULL;
    state->last_track_id = NULL;
    state->pending_title = NULL;
    state->pending_artist = NULL;
    state->pending_art_url = NULL;
    
    // Free config
    if (state->layout) {
        layout_free_config(state->layout);
        state->layout = NULL;
    }
    
    g_print("✓ Cleanup complete\n");
}


int main(int argc, char **argv) {
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--start-hidden") == 0) {
            start_hidden = TRUE;
            g_print("🎬 Starting hidden for smooth transition\n");
        }
    }
    
    GtkApplication *app = gtk_application_new("com.hyprwave.app", G_APPLICATION_DEFAULT_FLAGS);
    
    // Connect signals
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "startup", G_CALLBACK(load_css), NULL);
    
    // Note: shutdown callback will be connected in activate() since we need AppState*
    
    // Don't pass argc/argv to GTK - it doesn't understand our custom --start-hidden flag
    int status = g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);
    return status;
}
