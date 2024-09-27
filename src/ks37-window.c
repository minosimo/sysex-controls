#include <alsa/asoundlib.h>

#include "config.h"

#include "ks37-book.h"
#include "ks37-control-value.h"

#include "ks37-window.h"
#include "ks37-midi.h"

#define KS37_MIDI_NAME "Arturia KeyStep 37"
#define CONTROLS_MAX_N 100

typedef struct {
	uint16_t id;
	uint8_t val;
	GtkWidget *widget;
} control_t;

struct _Ks37Window
{
	AdwApplicationWindow  parent_instance;

	snd_seq_t *seq;
	snd_seq_addr_t seq_addr;

	control_t controls[CONTROLS_MAX_N];
	int controls_n;

	/* Template widgets */
	AdwToastOverlay *toast_overlay;
	GtkStackSidebar *sidebar;
	AdwNavigationPage  *content_page;
	AdwToolbarView *content_view;
	AdwNavigationView  *navigation_view;
	AdwNavigationSplitView *split_view;
};

G_DEFINE_FINAL_TYPE (Ks37Window, ks37_window, ADW_TYPE_APPLICATION_WINDOW)

static void
ks37_io_problem(Ks37Window *self, const char *message)
{
	g_warning("%s", message);
	adw_toast_overlay_add_toast (self->toast_overlay, adw_toast_new (message));
	adw_navigation_view_replace_with_tags(self->navigation_view, (const char * const[]){"search"}, 1);
}

static control_t *
get_control_by_id(Ks37Window *self, uint16_t control_id)
{
	for(int i = 0; i < self->controls_n; ++i)
		if (self->controls[i].id == control_id)
			return &self->controls[i];

	return NULL;
}

static void
combo_row_change_cb(GObject * widget, GParamSpec *pspec, control_t *control) {
	Ks37Window *window = KS37_WINDOW (gtk_widget_get_root (GTK_WIDGET (control->widget)));
	Ks37ControlValue *item = KS37_CONTROL_VALUE(adw_combo_row_get_selected_item(ADW_COMBO_ROW (widget)));
	uint8_t val = ks37_control_value_get_value(item);

	if (control->val == val)
		return;

	g_debug("combo control change %02x: %02x -> %02x %s\n", control->id, control->val, val, ks37_control_value_get_name (item));
	if (ks37_midi_write_control(window->seq, window->seq_addr, control->id, val) < 0) {
		ks37_io_problem (window, "Control change failed\n");
		return;
	}

	control->val = val;
}

static void
switch_row_change_cb(GObject * widget, GParamSpec *pspec, control_t *control) {
	Ks37Window *window = KS37_WINDOW (gtk_widget_get_root (GTK_WIDGET (control->widget)));
	AdwSwitchRow *w = ADW_SWITCH_ROW (widget);
	uint8_t val = adw_switch_row_get_active (w);

	if (control->val == val)
		return;

	g_debug("switch control change %02x: %02x -> %02x\n", control->id, control->val, val);
	if (ks37_midi_write_control(window->seq, window->seq_addr, control->id, val) < 0) {
		ks37_io_problem (window, "Control change failed\n");
		return;
	}

	control->val = val;
}

static void
spin_row_change_cb(GObject * widget, GParamSpec *pspec, control_t *control) {
	Ks37Window *window = KS37_WINDOW (gtk_widget_get_root (GTK_WIDGET (control->widget)));
	AdwSpinRow *w = ADW_SPIN_ROW (control->widget);
	uint8_t val = (uint8_t)adw_spin_row_get_value (w);

	if (control->val == val) {
		return;
	}

	g_debug("spin control change %02x: %02x -> %02x\n", control->id, control->val, val);

	if (ks37_midi_write_control(window->seq, window->seq_addr, control->id, val) < 0) {
		ks37_io_problem (window, "Control change failed\n");

		/*
		 * When setting page replaced by search page this spins forever
		 * toggle sensitivity to stop, possibly a GTK bug?
		 *
		 * Now it emit this warning:
		 * GtkText - did not receive a focus-out event.
		 * If you handle this event, you must return
		 * GDK_EVENT_PROPAGATE so the default handler gets the event as well
		 */
		gtk_widget_set_sensitive (GTK_WIDGET (w), false);
		gtk_widget_set_sensitive (GTK_WIDGET (w), true);

		return;
	}

	control->val = val;
}

void
ks37_window_register_control(Ks37Window *self, uint16_t control_id, GtkWidget *widget)
{
	control_t *control = get_control_by_id(self, control_id);

	if (control) {
		g_error("Control already added: 0x%02x\n", control_id);
		return;
	}

	if (self->controls_n == CONTROLS_MAX_N) {
		g_error("Control space exhausted\n");
		return;
	}

	control = &self->controls[self->controls_n];
	self->controls_n++;

	control->id = control_id;
	control->widget = widget;

	if (ADW_IS_COMBO_ROW(widget)) {
		g_signal_connect(G_OBJECT (widget), "notify::selected-item", G_CALLBACK(combo_row_change_cb), control);
	}
	else if (ADW_IS_SWITCH_ROW(widget)) {
		g_signal_connect(G_OBJECT (widget), "notify::active", G_CALLBACK(switch_row_change_cb), control);
	}
	else if (ADW_IS_SPIN_ROW(widget)) {
		g_signal_connect(G_OBJECT (widget), "notify::value", G_CALLBACK(spin_row_change_cb), control);
	}
	else {
		g_error("Unsupported control type: %s id: 0x%02x\n",
			gtk_widget_get_name(GTK_WIDGET (widget)),
			control_id);
	}
	g_debug("ks37_window_register_control %02x\n", control_id);
}

static void
update_gui_control(control_t *c) {
	if (ADW_IS_COMBO_ROW (c->widget)) {
		guint pos = GTK_INVALID_LIST_POSITION;
		AdwComboRow* combo_row = ADW_COMBO_ROW (c->widget);
		GListModel* list = adw_combo_row_get_model (combo_row);
		for(int i = 0; i < g_list_model_get_n_items(list); ++i) {
			Ks37ControlValue *kv = KS37_CONTROL_VALUE (g_list_model_get_item(list, i));
			if (ks37_control_value_get_value (kv) == c->val) {
				pos = i;
				break;
			}
		}

		if (pos != GTK_INVALID_LIST_POSITION)
			adw_combo_row_set_selected (combo_row, pos);

		g_debug("Set combo row id %02x to pos %02x\n", c->id, pos);
	} else if (ADW_IS_SWITCH_ROW(c->widget)) {
		AdwSwitchRow *switch_row = ADW_SWITCH_ROW (c->widget);
		adw_switch_row_set_active (switch_row, c->val);
		g_debug("Set switch row with id %02x to %02x\n", c->id, c->val);
	} else if (ADW_IS_SPIN_ROW(c->widget)) {
		AdwSpinRow *spin_row = ADW_SPIN_ROW (c->widget);
		adw_spin_row_set_value (spin_row, c->val);
		g_debug("Set spin row with id %02x to %02x\n", c->id, c->val);
	} else {
		g_error("Unsupported control type: %s id: 0x%02x\n",
			gtk_widget_get_name(GTK_WIDGET (c->widget)),
			c->id);
	}
}

/*static void
ks37_window_dump_values(Ks37Window *window)
{
	for(int i = 0;i < CONTROLS_N; ++i) {
		control_t *c = &window->controls[i];
		if (ADW_IS_COMBO_ROW (c->widget)) {
			Ks37ControlValue *item = KS37_CONTROL_VALUE(adw_combo_row_get_selected_item(ADW_COMBO_ROW (c->widget)));
			printf("%02x:%02x\n", c->id, ks37_control_value_get_value (item));
		} else if (ADW_IS_SWITCH_ROW(c->widget)) {
			printf("%02x:%02x\n", c->id, adw_switch_row_get_active (ADW_SWITCH_ROW (c->widget)));
		} else if (ADW_IS_SPIN_ROW(c->widget)) {
			printf("%02x:%02x\n", c->id, (int)adw_spin_row_get_value (ADW_SPIN_ROW (c->widget)));
		} else {
			fprintf(stderr, "Unsupported control type: %s id: 0x%02x\n",
				gtk_widget_get_name(GTK_WIDGET (c->widget)),
				c->id);
		}
	}
}*/

static void
ks37_load_task(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable)
{
	Ks37Window *self = KS37_WINDOW (source_obj);
	g_debug("ks37_load_task start\n");
	for(int i = 0; i < self->controls_n; ++i) {
		control_t *c = &self->controls[i];
		int err = ks37_midi_read_control(self->seq, self->seq_addr, c->id, &c->val);
		if (err < 0) {
			g_task_return_new_error_literal(task, G_IO_ERROR, G_IO_ERROR_FAILED, "control value read failed\n");
			return;
		}
	}
	g_task_return_boolean (task, true);
	g_debug("ks37_load_task end\n");
}

static void
ks37_load_task_finish(GObject* source_object, GAsyncResult* res, gpointer data)
{
	Ks37Window *self = KS37_WINDOW (source_object);
	GError *error = NULL;
	g_task_propagate_boolean(G_TASK (res), &error);

	if (error) {
		ks37_io_problem(self, error->message);
		return;
	}
	for(int i = 0; i < self->controls_n; ++i) {
		control_t *c = &self->controls[i];
		update_gui_control (c);
	}
	adw_navigation_view_replace_with_tags(self->navigation_view, (const char * const[]){"setting"}, 1);
}

static int
ks37_create_load_task(void *self)
{
	GTask *task = g_task_new(self, NULL, ks37_load_task_finish, NULL);
	g_task_run_in_thread(task, ks37_load_task);
	g_object_unref(task);
	return false;
}

static void
notify_visible_child_cb (GtkStack* stack, GParamSpec *pspec, Ks37Window *self)
{
	GtkWidget *child = gtk_stack_get_visible_child (stack);
	GtkStackPage *page = gtk_stack_get_page (stack, child);

	adw_navigation_page_set_title (self->content_page, gtk_stack_page_get_title (page));
	adw_navigation_split_view_set_show_content (self->split_view, TRUE);
}


static void
ks37_window_set_book(Ks37Window *self, AdwBin *book)
{
	GtkStack *stack = GTK_STACK (adw_bin_get_child(book));
	self->controls_n = 0;
	adw_toolbar_view_set_content(self->content_view, GTK_WIDGET (book));
	gtk_stack_sidebar_set_stack (self->sidebar, stack);

	g_signal_connect(stack, "notify::visible-child", G_CALLBACK(notify_visible_child_cb), self);

	notify_visible_child_cb (stack, NULL, self);
}

static void
ks37_midi_init(Ks37Window *self)
{
	if (self->seq)
		ks37_midi_close(&self->seq);

	if (ks37_midi_open(&self->seq) < 0) {
		g_warning("Failed to open midi\n");
		return;
	}

	if (ks37_midi_get_address(self->seq, KS37_MIDI_NAME, &self->seq_addr) < 0) {
		g_warning("Failed to find controller\n");
		return;
	}

	if (ks37_midi_connect(self->seq, self->seq_addr) < 0) {
		g_warning("Failed to connect %d:%d\n", self->seq_addr.client, self->seq_addr.port);
		return;
	}

	ks37_window_set_book(self, ADW_BIN (ks37_book_new()));

	adw_navigation_view_replace_with_tags(self->navigation_view, (const char * const[]){"load"}, 1);
	g_idle_add(ks37_create_load_task, self);
}

static void
refresh_button_click_cb (Ks37Window *self)
{
	ks37_midi_init(KS37_WINDOW(self));
}

static void
ks37_window_class_init (Ks37WindowClass *klass)
{
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	gtk_widget_class_set_template_from_resource (widget_class, "/hu/irl/keystep37-settings/ks37-window.ui");
	gtk_widget_class_bind_template_child (widget_class, Ks37Window, toast_overlay);
	gtk_widget_class_bind_template_child (widget_class, Ks37Window, sidebar);
	gtk_widget_class_bind_template_child (widget_class, Ks37Window, content_page);
	gtk_widget_class_bind_template_child (widget_class, Ks37Window, content_view);
	gtk_widget_class_bind_template_child (widget_class, Ks37Window, navigation_view);
	gtk_widget_class_bind_template_child (widget_class, Ks37Window, split_view);
	gtk_widget_class_bind_template_callback (widget_class, refresh_button_click_cb);
}

static void
ks37_window_init (Ks37Window *self)
{
	g_type_ensure (KS37_TYPE_BOOK);

	gtk_widget_init_template (GTK_WIDGET (self));

	// remove the unecessary right border
	gtk_widget_remove_css_class (GTK_WIDGET (self->sidebar), "sidebar");

	ks37_midi_init (self);
}

