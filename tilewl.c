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

//xwayland
#include <wlr/xwayland.h>

struct tinywl_output {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener destroy;

	char active_workspace;
};

struct tinywl_toplevel {
	struct wl_list link;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_fullscreen;

	struct tinywl_output *output;
	char workspace;

	//xwayland
	struct wlr_xwayland_surface *xwayland_surface;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener xwayland_destroy;
};

struct tinywl_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct tinywl_keyboard {
	struct wl_list link;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct tinywl_pointer_constraint {
	struct wlr_pointer_constraint_v1 *constraint;
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

static struct wlr_xdg_shell *xdg_shell;
static struct wlr_seat *seat;
static struct wl_display *wl_display;
static struct wlr_backend *backend;
static struct wlr_renderer *renderer;
static struct wlr_allocator *allocator;
static struct wlr_scene *scene;
static struct wlr_scene_output_layout *scene_layout;
static struct wlr_cursor *cursor;
static struct wlr_output_layout *output_layout;
static struct wlr_xcursor_manager *cursor_mgr;

static struct tinywl_toplevel *focused_toplevel;

//constraints
static struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
static struct wlr_pointer_constraints_v1 *wlr_pointer_constraints;
static struct wlr_pointer_constraint_v1 *active_constraint;
static struct wl_listener new_pointer_constraint;

//xwayland
static struct wlr_xwayland *xwayland;
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

	focused_toplevel = NULL;
}

static void focus_toplevel(struct tinywl_toplevel *toplevel) {
	/* Note: this function only deals with keyboard focus. */
	if (toplevel == NULL)
	{
		return;
	}

	if (toplevel == focused_toplevel)
	{
		return;
	}

	unfocus_toplevel();

	struct wlr_surface *surface;
	/* Activate the new surface */
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
	if (!focused_toplevel)
	{
		return;
	}

	//wayland
	if(focused_toplevel->xdg_toplevel)
	{
		wlr_xdg_toplevel_send_close(focused_toplevel->xdg_toplevel);
		return;
	}
	
	//xwayland
	wlr_xwayland_surface_close(focused_toplevel->xwayland_surface);
}

static int count_tiles(struct tinywl_output *output)
{
	int n = 0;

	struct tinywl_toplevel *toplevel;
	wl_list_for_each(toplevel, &toplevels, link)
	{
		if(toplevel->output == output)
		{
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
	}

	return n;
}

static void tile(struct tinywl_output *output)
{
	active_constraint = NULL;

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
	struct tinywl_toplevel *toplevel;
	wl_list_for_each(toplevel, &toplevels, link)
	{
		if(toplevel->output == output && toplevel->workspace == output->active_workspace)
		{
			if(toplevel->xwayland_surface != NULL)
			{
				wlr_xwayland_surface_configure(toplevel->xwayland_surface, 0, 0, width, height);
			}
			else
			{
				wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
			}
			wlr_scene_node_set_position(&toplevel->scene_tree->node, output_x + width * i, output_y);
			i++;
		}
	}
}

static void switch_workspace(int workspace)
{
	struct wlr_output *wlr_output = wlr_output_layout_output_at(output_layout, cursor->x, cursor->y);
	struct tinywl_output *output = wlr_output->data;

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
	struct tinywl_output *output = wlr_output->data;

	struct tinywl_output *next_output;

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

	struct tinywl_output *prev_output = focused_toplevel->output;
	struct tinywl_output *next_output;

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
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct tinywl_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(seat, &keyboard->wlr_keyboard->modifiers);
}

static bool handle_keybinding(xkb_keysym_t sym)
{
	static const char *termcmd[] = {"foot", NULL};
	static const char *browsercmd[] = {"firefox", NULL};
	static const char *steamcmd[] = {"steam", NULL};
	
	switch (sym) 
	{
		case XKB_KEY_q:				spawn(termcmd);		break;
		case XKB_KEY_e:				spawn(browsercmd); 	break;
		case XKB_KEY_s:				spawn(steamcmd); 	break;

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
	/* This event is raised when a key is pressed or released. */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct wlr_keyboard_key_event *event = data;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;																												
	int nsyms = xkb_state_key_get_syms(
			keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* If alt is held down and this button was _pressed_, we attempt to
		 * process it as a compositor keybinding. */
		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(syms[i]);
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	/* This event is raised by the keyboard base wlr_input_device to signal
	 * the destruction of the wlr_keyboard. It will no longer receive events
	 * and should be destroyed.
	 */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct tinywl_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->wlr_keyboard = wlr_keyboard;

	/* We need to prepare an XKB keymap and assign it to the keyboard. This
	 * assumes the defaults (e.g. layout = "us"). */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	/* Here we set up listeners for keyboard events. */
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&keyboards, &keyboard->link);
}

static void server_new_pointer(struct wlr_input_device *device) {
	/* We don't do anything special with pointers. All of our pointer handling
	 * is proxied through wlr_cursor. On another compositor, you might take this
	 * opportunity to do libinput configuration on the device to set
	 * acceleration, etc. */
	wlr_cursor_attach_input_device(cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new input device becomes
	 * available. */
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
	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In TinyWL we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client = seat->pointer_state.focused_client;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. */
	if (focused_client == event->seat_client) {
		/* Once we've vetted the client, we can tell the cursor to use the
		 * provided surface as the cursor image. It will set the hardware cursor
		 * on the output that it's currently on and continue to do so as the
		 * cursor moves between outputs. */
		wlr_cursor_set_surface(cursor, event->surface, event->hotspot_x, event->hotspot_y);
	}
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	/* This event is raised when the pointer focus is changed, including when the
	 * client is closed. We set the cursor image to its default if target surface
	 * is NULL */
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL)
	{
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in tinywl we always honor
	 */
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(seat, event->source, event->serial);
}

static struct tinywl_toplevel *desktop_toplevel_at(double lx, double ly, struct wlr_surface **surface, double *sx, double *sy) {
	/* This returns the topmost node in the scene at the given layout coords.
	 * We only care about surface nodes as we are specifically looking for a
	 * surface in the surface tree of a tinywl_toplevel. */
	struct wlr_scene_node *node = wlr_scene_node_at(&scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;
	/* Find the node corresponding to the tinywl_toplevel at the root of this
	 * surface tree, it is the only one for which we set the data field. */
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	return tree->node.data;
}

static void process_cursor_motion(uint32_t time) {

	/* Otherwise, find the toplevel under the pointer and send the event along. */
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct wlr_pointer_constraint_v1 *constraint = NULL;
	struct tinywl_toplevel *toplevel = desktop_toplevel_at(cursor->x, cursor->y, &surface, &sx, &sy);
	if (!toplevel) {
		/* If there's no toplevel under the cursor, set the cursor image to a
		 * default. This is what makes the cursor image appear when you move it
		 * around the screen, not over any toplevels. */
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
	}
	else
	{
		focus_toplevel(toplevel);
	}
	if (surface) {
		constraint = wlr_pointer_constraints_v1_constraint_for_surface(wlr_pointer_constraints, surface, seat);
		/*
		 * Send pointer enter and motion events.
		 *
		 * The enter event gives the surface "pointer focus", which is distinct
		 * from keyboard focus. You get pointer focus by moving the pointer over
		 * a window.
		 *
		 * Note that wlroots will avoid sending duplicate enter/motion events if
		 * the surface has already has pointer focus or if the client is already
		 * aware of the coordinates passed.
		 */
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);

		if (constraint != active_constraint)
		{
			struct wlr_pointer_constraint_v1 *previous_constraint = active_constraint;
			active_constraint = NULL;

			if(previous_constraint) {
				wlr_pointer_constraint_v1_send_deactivated(previous_constraint);
			}

			if(constraint) {
				active_constraint = constraint;
				wlr_pointer_constraint_v1_send_activated(constraint);
			}
		}

	} else {
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct wlr_pointer_motion_event *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */

	
	if(!active_constraint || active_constraint->type != WLR_POINTER_CONSTRAINT_V1_LOCKED)
	{
		wlr_cursor_move(cursor, &event->pointer->base, event->delta_x, event->delta_y);
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
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. There is also some hardware which
	 * emits these events. */
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);
	process_cursor_motion(event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a button
	 * event. */
	struct wlr_pointer_button_event *event = data;
	/* Notify the client with pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct wlr_pointer_axis_event *event = data;
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(seat);
}

static void output_frame(struct wl_listener *listener, void *data) {
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	struct tinywl_output *output = wl_container_of(listener, output, frame);

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr_output);

	/* Render the scene if needed and commit the output */
	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct tinywl_output *output = wl_container_of(listener, output, destroy);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct wlr_output *wlr_output = data;

	/* Configures the output created by the backend to use our allocator
	 * and our renderer. Must be done once, before committing the output */
	wlr_output_init_render(wlr_output, allocator, renderer);

	/* The output may be disabled, switch it on. */
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

	/* Atomically applies the new output  */
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	/* Allocates and configures our state for this output */
	struct tinywl_output *output = calloc(1, sizeof(*output));
	
	output->wlr_output = wlr_output;
	output->active_workspace = 1;

	wlr_output->data = output;

	/* Sets up a listener for the frame event. */
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	/* Sets up a listener for the destroy event. */
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&outputs, &output->link);

	/* Adds this to the output layout. The add_auto function arranges outputs
	 * from left-to-right in the order they appear. A more sophisticated
	 * compositor would let the user configure the arrangement of outputs in the
	 * layout.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(output_layout, wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, wlr_output);
	wlr_scene_output_layout_add_output(scene_layout, l_output, scene_output);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, map);

	wl_list_insert(&toplevels, &toplevel->link);

	if(!(toplevel->xwayland_surface != NULL && toplevel->xwayland_surface->override_redirect))
	{
		focus_toplevel(toplevel);
		tile(toplevel->output);
	}
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	
	if (toplevel == focused_toplevel)
	{
		unfocus_toplevel();
	}
	wl_list_remove(&toplevel->link);

	tile(toplevel->output);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	/* Called when a new surface state is committed. */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

	int n = count_tiles(toplevel->output);
	int width = toplevel->output->wlr_output->width / (n+1);
	int height = toplevel->output->wlr_output->height;

	

	if (toplevel->xdg_toplevel->base->initial_commit) {
		/* When an xdg_surface performs an initial commit, the compositor must
		 * reply with a configure so the client can map the surface. tinywl
		 * configures the xdg_toplevel with 0,0 size to let the client pick the
		 * dimensions itself. */
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	/* Called when the xdg_toplevel is destroyed. */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_fullscreen.link);

	free(toplevel);
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
	
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, toplevel->xdg_toplevel->requested.fullscreen);
	}
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	/* This event is raised when a client creates a new toplevel (application window). */
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	/* Allocate a tinywl_toplevel for this surface */
	struct tinywl_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->xdg_toplevel = xdg_toplevel;
	
	struct wlr_output *wlr_output = wlr_output_layout_output_at(output_layout, cursor->x, cursor->y);
	struct tinywl_output *output = wlr_output->data;

	toplevel->output = output;
	toplevel->workspace = toplevel->output->active_workspace;

	toplevel->scene_tree = wlr_scene_xdg_surface_create(&scene->tree, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

	/* Listen to the various events it can emit */
	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	/* cotd */
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	/* Called when a new surface state is committed. */
	struct tinywl_popup *popup = wl_container_of(listener, popup, commit);

	if (popup->xdg_popup->base->initial_commit) {
		/* When an xdg_surface performs an initial commit, the compositor must
		 * reply with a configure so the client can map the surface.
		 * tinywl sends an empty configure. A more sophisticated compositor
		 * might change an xdg_popup's geometry to ensure it's not positioned
		 * off-screen, for example. */
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	/* Called when the xdg_popup is destroyed. */
	struct tinywl_popup *popup = wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);

	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	/* This event is raised when a client creates a new popup. */
	struct wlr_xdg_popup *xdg_popup = data;

	struct tinywl_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	/* We must add xdg popups to the scene graph so they get rendered. The
	 * wlroots scene graph provides a helper for this, but to use it we must
	 * provide the proper parent scene node of the xdg popup. To enable this,
	 * we always set the user data field of xdg_surfaces to the corresponding
	 * scene node. */
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
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, associate);

	struct wlr_surface *surface = toplevel->xwayland_surface->surface;

	toplevel->scene_tree = wlr_scene_subsurface_tree_create(&scene->tree, surface);
	toplevel->scene_tree->node.data = toplevel;
	toplevel->xwayland_surface->data = toplevel->scene_tree;

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&surface->events.map, &toplevel->map);

	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&surface->events.unmap, &toplevel->unmap);
}

static void xwayland_dissociate(struct wl_listener *listener, void *data)
{
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, dissociate);

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
	/* Called when the xwayland window is destroyed. */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, xwayland_destroy);

	wl_list_remove(&toplevel->associate.link);
	wl_list_remove(&toplevel->dissociate.link);
	wl_list_remove(&toplevel->xwayland_destroy.link);

	free(toplevel);
}

static void server_new_xwayland_surface(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client creates a new xwayland (application window). */
	struct wlr_xwayland_surface *xwayland_surface = data;

	/* Allocate a tinywl_toplevel for this surface */
	struct tinywl_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->xwayland_surface = xwayland_surface;
	
	struct wlr_output *wlr_output = wlr_output_layout_output_at(output_layout, cursor->x, cursor->y);
	struct tinywl_output *output = wlr_output->data;

	toplevel->output = output;
	toplevel->workspace = toplevel->output->active_workspace;

	/* Listen to the various events it can emit */

	toplevel->associate.notify = xwayland_associate;
	wl_signal_add(&xwayland_surface->events.associate, &toplevel->associate);


	toplevel->dissociate.notify = xwayland_dissociate;
	wl_signal_add(&xwayland_surface->events.dissociate, &toplevel->dissociate);

	toplevel->xwayland_destroy.notify = xwayland_destroy;
	wl_signal_add(&xwayland_surface->events.destroy, &toplevel->xwayland_destroy);
}

static void pointer_constraint_destroy(struct wl_listener *listener, void *data)
{
	struct tinywl_pointer_constraint *constraint = wl_container_of(listener, constraint, destroy);
	
	active_constraint = NULL;

	wl_list_remove(&constraint->destroy.link);

	free(constraint);
}

static void server_new_pointer_constraint(struct wl_listener *listener, void *data)
{
	struct tinywl_pointer_constraint *constraint = calloc(1, sizeof(*constraint));

	constraint->constraint = data;

	constraint->destroy.notify = pointer_constraint_destroy;
	wl_signal_add(&constraint->constraint->events.destroy, &constraint->destroy);
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_ERROR, NULL);

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, managing Wayland globals, and so on. */
	wl_display = wl_display_create();
	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	backend = wlr_backend_autocreate(wl_display_get_event_loop(wl_display), NULL);
	if (backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}

	/* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
	 * can also specify a renderer using the WLR_RENDERER env var.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	renderer = wlr_renderer_autocreate(backend);
	if (renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}

	wlr_renderer_init_wl_display(renderer, wl_display);

	/* Autocreates an allocator for us.
	 * The allocator is the bridge between the renderer and the backend. It
	 * handles the buffer creation, allowing wlroots to render onto the
	 * screen */
	allocator = wlr_allocator_autocreate(backend, renderer);
	if (allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces, the subcompositor allows to
	 * assign the role of subsurfaces to surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the handling of the request_set_selection event below.*/
	struct wlr_compositor *compositor = wlr_compositor_create(wl_display, 5, renderer);
	wlr_subcompositor_create(wl_display);
	wlr_data_device_manager_create(wl_display);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	output_layout = wlr_output_layout_create(wl_display);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&outputs);
	new_output.notify = server_new_output;
	wl_signal_add(&backend->events.new_output, &new_output);

	/* Create a scene graph. This is a wlroots abstraction that handles all
	 * rendering and damage tracking. All the compositor author needs to do
	 * is add things that should be rendered to the scene graph at the proper
	 * positions and then call wlr_scene_output_commit() to render a frame if
	 * necessary.
	 */
	scene = wlr_scene_create();
	scene_layout = wlr_scene_attach_output_layout(scene, output_layout);

	/* Set up xdg-shell version 3. The xdg-shell is a Wayland protocol which is
	 * used for application windows. For more detail on shells, refer to
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html.
	 */
	wl_list_init(&toplevels);
	xdg_shell = wlr_xdg_shell_create(wl_display, 3);
	new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&xdg_shell->events.new_toplevel, &new_xdg_toplevel);
	new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&xdg_shell->events.new_popup, &new_xdg_popup);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). */
	cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html.
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
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

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
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

	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(wl_display);
	if (!socket) {
		wlr_backend_destroy(backend);
		return 1;
	}

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(backend)) {
		wlr_backend_destroy(backend);
		wl_display_destroy(wl_display);
		return 1;
	}

	/* Set the WAYLAND_DISPLAY environment variable to our socket and run the
	 * startup command if requested. */
	setenv("WAYLAND_DISPLAY", socket, true);

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */


	//start xwayland
	xwayland = wlr_xwayland_create(
		wl_display,
		compositor,
		true
	);

	setenv("DISPLAY", xwayland->display_name, true);

	new_xwayland_surface.notify = server_new_xwayland_surface;
	wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

	//relative pointer
	relative_pointer_manager = wlr_relative_pointer_manager_v1_create(wl_display);

	//pointer constraints
	wlr_pointer_constraints = wlr_pointer_constraints_v1_create(wl_display);

	new_pointer_constraint.notify = server_new_pointer_constraint;
	wl_signal_add(&wlr_pointer_constraints->events.new_constraint, &new_pointer_constraint);

	wl_display_run(wl_display);


	/* Once wl_display_run returns, we destroy all clients then shut down the
	 *  */
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
