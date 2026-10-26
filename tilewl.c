#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/util/region.h>

#include <wlr/types/wlr_layer_shell_v1.h>

#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>

#include <wlr/types/wlr_xdg_output_v1.h>

#include <wlr/xwayland.h>

struct server_output {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener destroy;

	char active_workspace;
};

struct server_toplevel {
	struct wl_list link;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_fullscreen;

	struct server_output *output;
	char workspace;

	//xwayland
	struct wlr_xwayland_surface *xwayland_surface;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener xwayland_destroy;
};

struct server_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct server_keyboard {
	struct wl_list link;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct server_pointer_constraint {
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
};

struct layer_surface {
	struct wlr_layer_surface_v1 *surface;
	struct wlr_scene_layer_surface_v1 *scene_surface;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static struct wl_listener new_xdg_toplevel;
static struct wl_listener new_xdg_popup;
static struct wl_list toplevels;

static struct wl_listener cursor_motion;
static struct wl_listener cursor_motion_absolute;
static struct wl_listener cursor_button;
static struct wl_listener cursor_axis;
static struct wl_listener cursor_frame;

static struct wl_listener new_input;
static struct wl_listener request_cursor;
static struct wl_listener pointer_focus_change;
static struct wl_listener request_set_selection;
static struct wl_list keyboards;

static struct wl_list outputs;
static struct wl_listener new_output;

static struct wlr_seat *seat;
static struct wlr_renderer *renderer;
static struct wlr_allocator *allocator;
static struct wlr_scene *scene;
static struct wlr_scene_output_layout *scene_layout;
static struct wlr_cursor *cursor;
static struct wlr_output_layout *output_layout;
static struct wlr_xcursor_manager *cursor_mgr;

static struct server_toplevel *focused_toplevel;

//constraints
static struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
static struct wlr_pointer_constraints_v1 *wlr_pointer_constraints;
static struct wlr_pointer_constraint_v1 *active_constraint;
static struct wl_listener new_pointer_constraint;

static struct wl_listener new_layer_surface;

//xwayland
static struct wl_listener new_xwayland_surface;

static void unfocus_toplevel()
{
	if(!focused_toplevel)
	{
		return;
	}

	if (focused_toplevel->xdg_toplevel)
	{
		wlr_xdg_toplevel_set_activated(focused_toplevel->xdg_toplevel, false);
	}
	else
	{
		wlr_xwayland_surface_activate(focused_toplevel->xwayland_surface, false);
	}

	wlr_seat_keyboard_notify_clear_focus(seat);
	focused_toplevel = NULL;
}

static void focus_toplevel(struct server_toplevel *toplevel) {
	

	if (!toplevel) return;

	if (toplevel == focused_toplevel) return;

	if (toplevel->xwayland_surface && toplevel->xwayland_surface->override_redirect) return;

	unfocus_toplevel();

	struct wlr_surface *surface;
	
	if (toplevel->xdg_toplevel)
	{
		surface = toplevel->xdg_toplevel->base->surface;
		wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	}
	else
	{
		surface = toplevel->xwayland_surface->surface;
		wlr_xwayland_surface_activate(toplevel->xwayland_surface, true);
	}

	focused_toplevel = toplevel;

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard)
	{
		wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

static void spawn(const char *cmd[])
{
	if (fork() == 0) {
		setsid();
		execvp(cmd[0], (char *const *)cmd);
		_exit(127);
	}
}

static void kill_top_level()
{
	if (!focused_toplevel) return;

	//wayland
	if(focused_toplevel->xdg_toplevel)
	{
		wlr_xdg_toplevel_send_close(focused_toplevel->xdg_toplevel);
		return;
	}
	
	//xwayland
	wlr_xwayland_surface_close(focused_toplevel->xwayland_surface);
}

static int count_tiles(struct server_output *output)
{
	int n = 0;

	struct server_toplevel *toplevel;
	wl_list_for_each(toplevel, &toplevels, link)
	{
		if(toplevel->xwayland_surface && toplevel->xwayland_surface->override_redirect) continue;

		if(toplevel->output != output) continue;
		
		if(toplevel->workspace == output->active_workspace)
		{
			wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
			n++;
		}
		else
		{
			wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
		}
	}

	return n;
}

static void tile(struct server_output *output)
{
	int n = count_tiles(output);

	if (n == 0)
	{
		return;
	}

	struct wlr_output_layout_output *lo = wlr_output_layout_get(output_layout, output->wlr_output);

	int output_x = lo->x;
	int output_y = lo->y;

	
	int height = output->wlr_output->height;
	int width = output->wlr_output->width / n;

	int i = 0;
	struct server_toplevel *toplevel;
	wl_list_for_each(toplevel, &toplevels, link)
	{
		if(toplevel->output != output) continue;
	
		if(toplevel->workspace != output->active_workspace) continue;

		if(toplevel->xwayland_surface && toplevel->xwayland_surface->override_redirect) continue;

		if(toplevel->xwayland_surface)
		{
			wlr_xwayland_surface_configure(toplevel->xwayland_surface, width * i, output_y, width, height);
		}
		else
		{
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
		}
		wlr_scene_node_set_position(&toplevel->scene_tree->node, output_x + width * i, output_y);
		i++;
	}
}

static void switch_workspace(int workspace)
{
	struct wlr_output *wlr_output = wlr_output_layout_output_at(output_layout, cursor->x, cursor->y);
	struct server_output *output = wlr_output->data;

	output->active_workspace = workspace;

	tile(output);
	unfocus_toplevel();
}

static void switch_toplevel_workspace(int workspace)
{
	if (!focused_toplevel)
	{
		return;
	}

	focused_toplevel->workspace = workspace;

	tile(focused_toplevel->output);
	unfocus_toplevel();
}


static void switch_output()
{
	struct wlr_output *wlr_output = wlr_output_layout_output_at(output_layout, cursor->x, cursor->y);
	struct server_output *output = wlr_output->data;

	struct server_output *next_output;

	wl_list_for_each(next_output, &outputs, link)
	{
		if(next_output != output)
		{

			struct wlr_output_layout_output *lo = wlr_output_layout_get(output_layout, next_output->wlr_output);

			int output_x = lo->x;
			int output_y = lo->y;

			int width = next_output->wlr_output->width;
			int height = next_output->wlr_output->height;


			wlr_cursor_warp(cursor, NULL, output_x + width/2, output_y + height/2);
			return;
		}
	}
}

static void switch_toplevel_output()
{
	if (!focused_toplevel)
	{
		return;
	}

	struct server_output *prev_output = focused_toplevel->output;
	struct server_output *next_output;

	wl_list_for_each(next_output, &outputs, link)
	{
		if(next_output != prev_output)
		{
			focused_toplevel->output = next_output;
			focused_toplevel->workspace = next_output->active_workspace;

			tile(prev_output);
			tile(next_output);

			return;
		}
	}
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct server_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	
	wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
	
	wlr_seat_keyboard_notify_modifiers(seat, &keyboard->wlr_keyboard->modifiers);
}

static bool handle_keybinding(xkb_keysym_t sym)
{
	static const char *term[] = {"foot", NULL};
	static const char *browser[] = {"firefox", NULL};
	static const char *steam[] = {"steam", NULL};
	static const char *snip[] = {"screenshot", NULL};
	
	switch (sym) 
	{
		case XKB_KEY_q:				spawn(term);		break;
		case XKB_KEY_e:				spawn(browser); 	break;
		case XKB_KEY_s:				spawn(steam); 		break;
		case XKB_KEY_Delete:		spawn(snip); 		break;

		case XKB_KEY_c:				kill_top_level();	break;

		case XKB_KEY_1: 			switch_workspace(1); break;
		case XKB_KEY_2: 			switch_workspace(2); break;
		case XKB_KEY_3: 			switch_workspace(3); break;
		case XKB_KEY_4: 			switch_workspace(4); break;
		case XKB_KEY_5: 			switch_workspace(5); break;

		case XKB_KEY_exclam:		switch_toplevel_workspace(1); break;
		case XKB_KEY_at: 			switch_toplevel_workspace(2); break;
		case XKB_KEY_numbersign: 	switch_toplevel_workspace(3); break;
		case XKB_KEY_dollar: 		switch_toplevel_workspace(4); break;
		case XKB_KEY_percent: 		switch_toplevel_workspace(5); break;

		case XKB_KEY_Tab:			switch_output(); break;
		case XKB_KEY_grave:			switch_toplevel_output(); break;

		default: return false;
	}
	return true;
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	
	struct server_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct wlr_keyboard_key_event *event = data;

	
	uint32_t keycode = event->keycode + 8;
	
	const xkb_keysym_t *syms;																												
	int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {

		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(syms[i]);
		}
	}

	if (!handled) {
		
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	
	struct server_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct server_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);

	
	wl_list_insert(&keyboards, &keyboard->link);
}

static void server_new_pointer(struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(device);
		break;
	default:
		break;
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client = seat->pointer_state.focused_client;
	if (focused_client == event->seat_client) {
		wlr_cursor_set_surface(cursor, event->surface, event->hotspot_x, event->hotspot_y);
	}
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL)
	{
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(seat, event->source, event->serial);
}

static struct wlr_surface *desktop_surface_at(struct wlr_scene_node *node)
{
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	return scene_surface->surface;
}

static struct server_toplevel *desktop_toplevel_at(struct wlr_scene_node *node)
{
	if(!node) return NULL;

	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}

	if (!tree) return NULL;

	return tree->node.data;
}

static void process_cursor_motion(uint32_t time)
{
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(&scene->tree.node, cursor->x, cursor->y, &sx, &sy);
	struct wlr_surface *surface = desktop_surface_at(node);
	struct server_toplevel *toplevel = desktop_toplevel_at(node);

	if(toplevel) focus_toplevel(toplevel);

	struct wlr_pointer_constraint_v1 *constraint = NULL;
	if (surface)
	{
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);

		constraint = wlr_pointer_constraints_v1_constraint_for_surface(wlr_pointer_constraints, surface, seat);

	} else {
		wlr_seat_pointer_clear_focus(seat);
	}

	if (constraint != active_constraint)
	{
		if(active_constraint)
		{
			wlr_pointer_constraint_v1_send_deactivated(active_constraint);
			active_constraint->data = NULL;
		}

		active_constraint = constraint;

		if(active_constraint)
		{
			wlr_pointer_constraint_v1_send_activated(active_constraint);
			active_constraint->data = node;
		}
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_motion_event *event = data;

	if(!active_constraint)
	{
		wlr_cursor_move(cursor, &event->pointer->base, event->delta_x, event->delta_y);
	}
	else if(active_constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED)
	{
		struct wlr_scene_node *node = active_constraint->data;
		int lx, ly;
		if (wlr_scene_node_coords(node, &lx, &ly))
		{
			double sx = cursor->x - lx;
			double sy = cursor->y - ly;
		
			double confined_x, confined_y;
			if(wlr_region_confine(&active_constraint->region, sx, sy, sx + event->delta_x, sy + event->delta_y, &confined_x, &confined_y))
			{
				wlr_cursor_move(cursor, &event->pointer->base, confined_x - sx, confined_y - sy);
			}
		}
	}

	wlr_relative_pointer_manager_v1_send_relative_motion(
		relative_pointer_manager,
		seat,
		(uint64_t)event->time_msec * 1000,
		event->delta_x,
		event->delta_y,
		event->unaccel_dx,
		event->unaccel_dy
	);

	process_cursor_motion(event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);
	process_cursor_motion(event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct wlr_pointer_button_event *event = data;
	
	wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct wlr_pointer_axis_event *event = data;
	
	wlr_seat_pointer_notify_axis(seat, event->time_msec, event->orientation, event->delta, event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	wlr_seat_pointer_notify_frame(seat);
}

static void output_frame(struct wl_listener *listener, void *data) {
	struct server_output *output = wl_container_of(listener, output, frame);

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr_output);

	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct server_output *output = wl_container_of(listener, output, destroy);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, allocator, renderer);

	
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);


	struct wlr_output_mode *mode, *best = NULL;

	wl_list_for_each(mode, &wlr_output->modes, link)
	{
		if(best == NULL || best->width * best->height + best->refresh < mode->width * mode->height + mode->refresh)
		{
			best = mode;
		}
	}

	if (best != NULL) {
		wlr_output_state_set_mode(&state, best);
	}

	
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	
	struct server_output *output = calloc(1, sizeof(*output));
	
	output->wlr_output = wlr_output;
	output->active_workspace = 1;

	wlr_output->data = output;

	
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&outputs, &output->link);

	struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(output_layout, wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, wlr_output);
	wlr_scene_output_layout_add_output(scene_layout, l_output, scene_output);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	
	struct server_toplevel *toplevel = wl_container_of(listener, toplevel, map);

	wl_list_insert(&toplevels, &toplevel->link);
	focus_toplevel(toplevel);
	tile(toplevel->output);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	
	struct server_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	
	if (toplevel == focused_toplevel)
	{
		unfocus_toplevel();
	}
	wl_list_remove(&toplevel->link);

	tile(toplevel->output);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data)
{
	
	struct server_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

	int n = count_tiles(toplevel->output);
	int width = toplevel->output->wlr_output->width / (n+1);
	int height = toplevel->output->wlr_output->height;

	if (toplevel->xdg_toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	
	struct server_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_fullscreen.link);

	free(toplevel);
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data)
{
	struct server_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
	
	if (toplevel->xdg_toplevel->base->initialized)
	{
		wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, toplevel->xdg_toplevel->requested.fullscreen);
	}
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel *xdg_toplevel = data;
	
	struct server_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->xdg_toplevel = xdg_toplevel;
	
	struct wlr_output *wlr_output = wlr_output_layout_output_at(output_layout, cursor->x, cursor->y);
	struct server_output *output = wlr_output->data;

	toplevel->output = output;
	toplevel->workspace = toplevel->output->active_workspace;

	toplevel->scene_tree = wlr_scene_xdg_surface_create(&scene->tree, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	
	struct server_popup *popup = wl_container_of(listener, popup, commit);

	if (popup->xdg_popup->base->initial_commit) {
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	
	struct server_popup *popup = wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);

	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	
	struct wlr_xdg_popup *xdg_popup = data;

	struct server_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	struct wlr_scene_tree *parent_tree = parent->data;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}


static void xwayland_associate(struct wl_listener *listener, void *data)
{
	struct server_toplevel *toplevel = wl_container_of(listener, toplevel, associate);

	struct wlr_surface *surface = toplevel->xwayland_surface->surface;

	toplevel->scene_tree = wlr_scene_subsurface_tree_create(&scene->tree, surface);
	toplevel->scene_tree->node.data = toplevel;

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&surface->events.map, &toplevel->map);

	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&surface->events.unmap, &toplevel->unmap);
}

static void xwayland_dissociate(struct wl_listener *listener, void *data)
{
	struct server_toplevel *toplevel = wl_container_of(listener, toplevel, dissociate);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);

	if(toplevel->scene_tree != NULL)
	{
		wlr_scene_node_destroy(&toplevel->scene_tree->node);
		toplevel->scene_tree = NULL;
	}
}


static void xwayland_destroy(struct wl_listener *listener, void *data) 
{
	struct server_toplevel *toplevel = wl_container_of(listener, toplevel, xwayland_destroy);

	wl_list_remove(&toplevel->associate.link);
	wl_list_remove(&toplevel->dissociate.link);
	wl_list_remove(&toplevel->xwayland_destroy.link);

	free(toplevel);
}

static void server_new_xwayland_surface(struct wl_listener *listener, void *data)
{
	struct wlr_xwayland_surface *xwayland_surface = data;
	
	struct server_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->xwayland_surface = xwayland_surface;
	
	struct wlr_output *wlr_output = wlr_output_layout_output_at(output_layout, cursor->x, cursor->y);
	struct server_output *output = wlr_output->data;

	toplevel->output = output;
	toplevel->workspace = toplevel->output->active_workspace;

	toplevel->associate.notify = xwayland_associate;
	wl_signal_add(&xwayland_surface->events.associate, &toplevel->associate);

	toplevel->dissociate.notify = xwayland_dissociate;
	wl_signal_add(&xwayland_surface->events.dissociate, &toplevel->dissociate);

	toplevel->xwayland_destroy.notify = xwayland_destroy;
	wl_signal_add(&xwayland_surface->events.destroy, &toplevel->xwayland_destroy);
}

static void pointer_constraint_destroy(struct wl_listener *listener, void *data)
{
	struct server_pointer_constraint *constraint = wl_container_of(listener, constraint, destroy);

	if(constraint->constraint == active_constraint)
	{
		active_constraint = NULL;
	}

	wl_list_remove(&constraint->destroy.link);

	free(constraint);
}

static void server_new_pointer_constraint(struct wl_listener *listener, void *data)
{
	struct server_pointer_constraint *constraint = calloc(1, sizeof(*constraint));

	constraint->constraint = data;

	constraint->destroy.notify = pointer_constraint_destroy;
	wl_signal_add(&constraint->constraint->events.destroy, &constraint->destroy);
}

static void layer_surface_commit(struct wl_listener *listener, void *data)
{
	struct layer_surface *layer = wl_container_of(listener, layer, commit);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	
	if (layer->surface->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
	{
		wlr_seat_keyboard_notify_enter(seat, layer->surface->surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}

	struct wlr_box full_area = {0};
	struct wlr_output_layout_output *lo = wlr_output_layout_get(output_layout, layer->surface->output);

	full_area.x = lo->x;
	full_area.y = lo->y;

	wlr_output_effective_resolution(layer->surface->output, &full_area.width, &full_area.height);

	wlr_scene_layer_surface_v1_configure(layer->scene_surface, &full_area, &full_area);
}

static void layer_surface_destroy(struct wl_listener *listener, void *data)
{
	struct layer_surface *layer = wl_container_of(listener, layer, destroy);

	wl_list_remove(&layer->commit.link);
	wl_list_remove(&layer->destroy.link);

	free(layer);
}

static void server_new_layer_surface(struct wl_listener *listener, void *data)
{
	struct layer_surface *layer = calloc(1, sizeof(*layer));
	layer->surface = data;

	layer->scene_surface = wlr_scene_layer_surface_v1_create(&scene->tree, layer->surface);

	layer->commit.notify = layer_surface_commit;
	wl_signal_add(&layer->surface->surface->events.commit, &layer->commit);

	layer->destroy.notify = layer_surface_destroy;
	wl_signal_add(&layer->surface->surface->events.destroy, &layer->destroy);
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_ERROR, NULL);

	struct wl_display *wl_display = wl_display_create();
	struct wlr_backend *backend = wlr_backend_autocreate(wl_display_get_event_loop(wl_display), NULL);
	if (backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}
	renderer = wlr_renderer_autocreate(backend);
	if (renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}

	wlr_renderer_init_wl_display(renderer, wl_display);

	allocator = wlr_allocator_autocreate(backend, renderer);
	if (allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	struct wlr_compositor *compositor = wlr_compositor_create(wl_display, 6, renderer);
	wlr_subcompositor_create(wl_display);
	wlr_data_device_manager_create(wl_display);

	output_layout = wlr_output_layout_create(wl_display);

	wl_list_init(&outputs);
	new_output.notify = server_new_output;
	wl_signal_add(&backend->events.new_output, &new_output);

	scene = wlr_scene_create();
	scene_layout = wlr_scene_attach_output_layout(scene, output_layout);

	wl_list_init(&toplevels);
	struct wlr_xdg_shell *xdg_shell = wlr_xdg_shell_create(wl_display, 7);
	new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&xdg_shell->events.new_toplevel, &new_xdg_toplevel);
	new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&xdg_shell->events.new_popup, &new_xdg_popup);

	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);

	cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&cursor->events.motion, &cursor_motion);

	cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);

	cursor_button.notify = server_cursor_button;
	wl_signal_add(&cursor->events.button, &cursor_button);

	cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&cursor->events.axis, &cursor_axis);

	cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&cursor->events.frame, &cursor_frame);

	wl_list_init(&keyboards);
	new_input.notify = server_new_input;
	wl_signal_add(&backend->events.new_input, &new_input);
	seat = wlr_seat_create(wl_display, "seat0");
	request_cursor.notify = seat_request_cursor;
	wl_signal_add(&seat->events.request_set_cursor, &request_cursor);

	pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&seat->pointer_state.events.focus_change, &pointer_focus_change);

	request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&seat->events.request_set_selection, &request_set_selection);

	
	const char *socket = wl_display_add_socket_auto(wl_display);
	if (!socket) {
		wlr_backend_destroy(backend);
		return 1;
	}

	if (!wlr_backend_start(backend)) {
		wlr_backend_destroy(backend);
		wl_display_destroy(wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);

	//start xwayland
	struct wlr_xwayland *xwayland = wlr_xwayland_create(wl_display, compositor, true);
	wlr_xwayland_set_seat(xwayland, seat);
	setenv("DISPLAY", xwayland->display_name, true);

	new_xwayland_surface.notify = server_new_xwayland_surface;
	wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

	//relative pointer
	relative_pointer_manager = wlr_relative_pointer_manager_v1_create(wl_display);

	//pointer constraints
	wlr_pointer_constraints = wlr_pointer_constraints_v1_create(wl_display);

	new_pointer_constraint.notify = server_new_pointer_constraint;
	wl_signal_add(&wlr_pointer_constraints->events.new_constraint, &new_pointer_constraint);

	struct wlr_layer_shell_v1 *layer_shell = wlr_layer_shell_v1_create(wl_display, 5);

	new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&layer_shell->events.new_surface, &new_layer_surface);

	wlr_ext_image_copy_capture_manager_v1_create(wl_display, 1);
	wlr_ext_output_image_capture_source_manager_v1_create(wl_display, 1);

	wlr_xdg_output_manager_v1_create(wl_display, output_layout);

	wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

	wl_display_run(wl_display);

	wl_display_destroy_clients(wl_display);

	wl_list_remove(&new_xdg_toplevel.link);
	wl_list_remove(&new_xdg_popup.link);

	wl_list_remove(&cursor_motion.link);
	wl_list_remove(&cursor_motion_absolute.link);
	wl_list_remove(&cursor_button.link);
	wl_list_remove(&cursor_axis.link);
	wl_list_remove(&cursor_frame.link);

	wl_list_remove(&new_input.link);
	wl_list_remove(&request_cursor.link);
	wl_list_remove(&pointer_focus_change.link);
	wl_list_remove(&request_set_selection.link);

	wl_list_remove(&new_output.link);

	wlr_scene_node_destroy(&scene->tree.node);
	wlr_xcursor_manager_destroy(cursor_mgr);
	wlr_cursor_destroy(cursor);
	wlr_allocator_destroy(allocator);
	wlr_renderer_destroy(renderer);
	wlr_backend_destroy(backend);
	wl_display_destroy(wl_display);
	return 0;
}
