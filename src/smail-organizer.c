/* smail-organizer.c — Calendar, Tasks and Reminders */
#include "smail-organizer.h"
#include "smail-ui.h"

/* =====================================================================
 *  Shared form dialog: header with Cancel/Save, a preferences page and
 *  an optional destructive Delete button.
 * ===================================================================== */

typedef struct _Form Form;
typedef gboolean (*FormSaveFunc)   (Form *form, GError **error);
typedef void     (*FormDeleteFunc) (Form *form);

struct _Form {
  AdwDialog      *dialog;
  GtkWidget      *page;
  GtkWidget      *banner;
  FormSaveFunc    save;
  FormDeleteFunc  remove;
  GCallback       done;      /* void (*)(gpointer owner) */
  gpointer        owner;     /* the page widget */
  gpointer        item;      /* the edited record */
  GDestroyNotify  item_free;
  GHashTable     *fields;    /* name -> widget */
};

static void
form_free (Form *f)
{
  if (f->item && f->item_free)
    f->item_free (f->item);
  g_hash_table_unref (f->fields);
  g_free (f);
}

static GtkWidget *
form_get (Form *f, const char *name)
{
  return g_hash_table_lookup (f->fields, name);
}

static void
form_finish (Form *f)
{
  ((void (*) (gpointer)) f->done) (f->owner);
  adw_dialog_force_close (f->dialog);
}

static void
on_form_save (GtkButton *b, Form *f)
{
  g_autoptr (GError) error = NULL;
  if (!f->save (f, &error)) {
    adw_banner_set_title (ADW_BANNER (f->banner), error ? error->message : "Couldn't save");
    adw_banner_set_revealed (ADW_BANNER (f->banner), TRUE);
    return;
  }
  form_finish (f);
}

static void
on_form_delete (GtkButton *b, Form *f)
{
  f->remove (f);
  form_finish (f);
}

static Form *
form_new (const char *title, gboolean editing, gpointer owner, GCallback done)
{
  Form *f = g_new0 (Form, 1);
  f->owner = owner;
  f->done = done;
  f->fields = g_hash_table_new (g_str_hash, g_str_equal);
  f->dialog = adw_dialog_new ();
  adw_dialog_set_title (f->dialog, title);
  adw_dialog_set_content_width (f->dialog, 480);
  g_object_set_data_full (G_OBJECT (f->dialog), "form", f, (GDestroyNotify) form_free);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  GtkWidget *cancel = gtk_button_new_with_mnemonic ("_Cancel");
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (adw_dialog_close), f->dialog);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);
  GtkWidget *save = gtk_button_new_with_mnemonic (editing ? "_Save" : "_Add");
  gtk_widget_add_css_class (save, "suggested-action");
  g_signal_connect (save, "clicked", G_CALLBACK (on_form_save), f);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), save);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  f->banner = adw_banner_new ("");
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), f->banner);

  f->page = adw_preferences_page_new ();
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), f->page);
  adw_dialog_set_child (f->dialog, toolbar);
  adw_dialog_set_default_widget (f->dialog, save);
  return f;
}

static AdwPreferencesGroup *
form_group (Form *f, const char *title)
{
  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  if (title)
    adw_preferences_group_set_title (g, title);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (f->page), g);
  return g;
}

static GtkWidget *
form_entry (Form *f, AdwPreferencesGroup *g, const char *name, const char *title, const char *value)
{
  GtkWidget *row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  gtk_editable_set_text (GTK_EDITABLE (row), value ? value : "");
  adw_preferences_group_add (g, row);
  g_hash_table_insert (f->fields, (gpointer) name, row);
  return row;
}

static GtkWidget *
form_datetime (Form *f, AdwPreferencesGroup *g, const char *name, const char *title, gint64 value)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  GtkWidget *picker = smail_date_time_new (value);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), picker);
  adw_preferences_group_add (g, row);
  g_hash_table_insert (f->fields, (gpointer) name, picker);
  return row;
}

static void
form_add_delete (Form *f, const char *label, FormDeleteFunc remove)
{
  f->remove = remove;
  AdwPreferencesGroup *g = form_group (f, NULL);
  GtkWidget *b = gtk_button_new_with_label (label);
  gtk_widget_add_css_class (b, "destructive-action");
  gtk_widget_add_css_class (b, "pill");
  gtk_widget_set_halign (b, GTK_ALIGN_CENTER);
  g_signal_connect (b, "clicked", G_CALLBACK (on_form_delete), f);
  adw_preferences_group_add (g, b);
}

static char *
entry_value (Form *f, const char *name)
{
  return g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (form_get (f, name)))));
}

static gint64
datetime_value (Form *f, const char *name)
{
  return smail_date_time_get_unix (SMAIL_DATE_TIME (form_get (f, name)));
}

static GtkWidget *
delete_button (const char *tooltip)
{
  GtkWidget *b = smail_icon_button ("user-trash-symbolic", tooltip);
  gtk_widget_add_css_class (b, "flat");
  gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
  return b;
}

static SMailStore *
page_store (GtkWidget *page)
{
  return g_object_get_data (G_OBJECT (page), "store");
}

/* =====================================================================
 *  Calendar
 * ===================================================================== */

static const int remind_choices[] = { -1, 0, 5, 15, 30, 60, 120, 1440 };
static const char *remind_labels[] = { "None", "At start time", "5 minutes before", "15 minutes before",
                                       "30 minutes before", "1 hour before", "2 hours before", "1 day before", NULL };

typedef struct {
  SMailStore *store;
  GtkWidget    *calendar;
  GtkWidget    *day_title;
  GtkWidget    *day_list;
  GtkWidget    *upcoming;
} CalendarPage;

static void open_event_dialog (GtkWidget *page, SMailEvent *event);

static gint64
day_start (GDateTime *d)
{
  g_autoptr (GTimeZone) tz = g_time_zone_new_local ();
  g_autoptr (GDateTime) start = g_date_time_new (tz, g_date_time_get_year (d), g_date_time_get_month (d),
                                                 g_date_time_get_day_of_month (d), 0, 0, 0);
  return g_date_time_to_unix (start);
}

static void
on_event_row_activated (AdwActionRow *row, GtkWidget *page)
{
  SMailEvent *e = g_object_get_data (G_OBJECT (row), "event");
  open_event_dialog (page, smail_event_copy (e));
}

static GtkWidget *
event_row (GtkWidget *page, SMailEvent *e, gboolean with_date)
{
  GtkWidget *row = adw_action_row_new ();
  g_autofree char *title = g_markup_escape_text (e->title, -1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  g_autofree char *when = with_date ? smail_format_unix (e->start) : smail_format_unix_time (e->start);
  g_autofree char *end = smail_format_unix_time (e->end);
  g_autofree char *sub = e->location && *e->location
                         ? g_markup_printf_escaped ("%s – %s · %s", when, end, e->location)
                         : g_markup_printf_escaped ("%s – %s", when, end);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
  if (e->remind_minutes >= 0) {
    GtkWidget *bell = gtk_image_new_from_icon_name ("alarm-symbolic");
    gtk_widget_set_tooltip_text (bell, remind_labels[0]);
    for (gsize i = 0; i < G_N_ELEMENTS (remind_choices); i++)
      if (remind_choices[i] == e->remind_minutes)
        gtk_widget_set_tooltip_text (bell, remind_labels[i]);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), bell);
  }
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), gtk_image_new_from_icon_name ("go-next-symbolic"));
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  g_object_set_data_full (G_OBJECT (row), "event", smail_event_copy (e), (GDestroyNotify) smail_event_free);
  g_signal_connect (row, "activated", G_CALLBACK (on_event_row_activated), page);
  return row;
}

void
smail_calendar_page_refresh (GtkWidget *page)
{
  CalendarPage *p = g_object_get_data (G_OBJECT (page), "calendar-page");
  g_autoptr (GDateTime) sel = gtk_calendar_get_date (GTK_CALENDAR (p->calendar));

  /* marks for the visible month */
  gtk_calendar_clear_marks (GTK_CALENDAR (p->calendar));
  int year = gtk_calendar_get_year (GTK_CALENDAR (p->calendar));
  int month = gtk_calendar_get_month (GTK_CALENDAR (p->calendar)) + 1;
  g_autoptr (GTimeZone) tz = g_time_zone_new_local ();
  g_autoptr (GDateTime) m0 = g_date_time_new (tz, year, month, 1, 0, 0, 0);
  g_autoptr (GDateTime) m1 = g_date_time_add_months (m0, 1);
  g_autoptr (GPtrArray) month_events = smail_store_list_events (p->store, g_date_time_to_unix (m0),
                                                                  g_date_time_to_unix (m1));
  for (guint i = 0; i < month_events->len; i++) {
    SMailEvent *e = g_ptr_array_index (month_events, i);
    for (gint64 t = MAX (e->start, g_date_time_to_unix (m0)); t <= e->end && t < g_date_time_to_unix (m1); t += 86400) {
      g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (t);
      gtk_calendar_mark_day (GTK_CALENDAR (p->calendar), g_date_time_get_day_of_month (d));
      if (e->end - e->start < 86400)
        break;
    }
  }

  /* selected day */
  g_autofree char *title = g_date_time_format (sel, "%A, %-d %B");
  gtk_label_set_text (GTK_LABEL (p->day_title), title);
  gtk_list_box_remove_all (GTK_LIST_BOX (p->day_list));
  gint64 ds = day_start (sel);
  g_autoptr (GPtrArray) day = smail_store_list_events (p->store, ds, ds + 86400);
  for (guint i = 0; i < day->len; i++)
    gtk_list_box_append (GTK_LIST_BOX (p->day_list), event_row (page, g_ptr_array_index (day, i), FALSE));
  smail_boxed_list_done (p->day_list);

  /* next two weeks */
  gtk_list_box_remove_all (GTK_LIST_BOX (p->upcoming));
  gint64 now = smail_unix_now ();
  g_autoptr (GPtrArray) next = smail_store_list_events (p->store, now, now + 14 * 86400);
  for (guint i = 0; i < next->len && i < 20; i++)
    gtk_list_box_append (GTK_LIST_BOX (p->upcoming), event_row (page, g_ptr_array_index (next, i), TRUE));
  smail_boxed_list_done (p->upcoming);
}

static gboolean
save_event (Form *f, GError **error)
{
  SMailEvent *e = f->item;
  g_free (e->title);    e->title = entry_value (f, "title");
  g_free (e->location); e->location = entry_value (f, "location");
  g_free (e->notes);    e->notes = entry_value (f, "notes");
  e->start = datetime_value (f, "start");
  e->end = datetime_value (f, "end");
  if (e->end < e->start) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "The event ends before it starts");
    return FALSE;
  }
  guint sel = adw_combo_row_get_selected (ADW_COMBO_ROW (form_get (f, "remind")));
  e->remind_minutes = sel < G_N_ELEMENTS (remind_choices) ? remind_choices[sel] : -1;
  return smail_store_save_event (page_store (f->owner), e, error);
}

static void
delete_event (Form *f)
{
  SMailEvent *e = f->item;
  smail_store_delete_event (page_store (f->owner), e->id, NULL);
  smail_toast (f->owner, "Event deleted");
}

static void
open_event_dialog (GtkWidget *page, SMailEvent *e)
{
  Form *f = form_new (e->id ? "Edit Event" : "New Event", e->id != 0, page, G_CALLBACK (smail_calendar_page_refresh));
  f->item = e;
  f->item_free = (GDestroyNotify) smail_event_free;
  f->save = save_event;

  AdwPreferencesGroup *g = form_group (f, NULL);
  GtkWidget *title = form_entry (f, g, "title", "Title", e->title);
  form_entry (f, g, "location", "Location", e->location);

  g = form_group (f, NULL);
  form_datetime (f, g, "start", "Starts", e->start);
  form_datetime (f, g, "end", "Ends", e->end);
  GtkWidget *remind = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (remind), "Reminder");
  adw_combo_row_set_model (ADW_COMBO_ROW (remind), G_LIST_MODEL (gtk_string_list_new (remind_labels)));
  guint sel = 0;
  for (gsize i = 0; i < G_N_ELEMENTS (remind_choices); i++)
    if (remind_choices[i] == e->remind_minutes)
      sel = i;
  adw_combo_row_set_selected (ADW_COMBO_ROW (remind), sel);
  adw_preferences_group_add (g, remind);
  g_hash_table_insert (f->fields, "remind", remind);

  g = form_group (f, NULL);
  form_entry (f, g, "notes", "Notes", e->notes);
  if (e->id)
    form_add_delete (f, "Delete Event", delete_event);

  adw_dialog_present (f->dialog, page);
  gtk_widget_grab_focus (title);
}

static void
on_new_event (GtkButton *b, GtkWidget *page)
{
  CalendarPage *p = g_object_get_data (G_OBJECT (page), "calendar-page");
  g_autoptr (GDateTime) sel = gtk_calendar_get_date (GTK_CALENDAR (p->calendar));
  gint64 now = smail_unix_now ();
  gint64 start = day_start (sel) + 9 * 3600;            /* 09:00 on the chosen day */
  if (day_start (sel) <= now && now < day_start (sel) + 86400)
    start = smail_next_quarter_hour (now + 60);       /* today: next quarter hour */

  SMailEvent *e = g_new0 (SMailEvent, 1);
  e->start = start;
  e->end = start + 3600;
  e->remind_minutes = 15;
  open_event_dialog (page, e);
}

static void
on_today (GtkButton *b, GtkWidget *page)
{
  CalendarPage *p = g_object_get_data (G_OBJECT (page), "calendar-page");
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  gtk_calendar_select_day (GTK_CALENDAR (p->calendar), now);
  smail_calendar_page_refresh (page);
}

GtkWidget *
smail_calendar_page_new (SMailStore *store)
{
  CalendarPage *p = g_new0 (CalendarPage, 1);
  p->store = store;

  GtkWidget *box;
  GtkWidget *scroll = smail_clamped_column (&box);

  p->calendar = gtk_calendar_new ();
  gtk_widget_add_css_class (p->calendar, "card");
  gtk_box_append (GTK_BOX (box), p->calendar);

  p->day_title = smail_section_label ("");
  gtk_box_append (GTK_BOX (box), p->day_title);
  p->day_list = smail_boxed_list ("No events on this day");
  gtk_box_append (GTK_BOX (box), p->day_list);

  gtk_box_append (GTK_BOX (box), smail_section_label ("Next 14 Days"));
  p->upcoming = smail_boxed_list ("Nothing coming up");
  gtk_box_append (GTK_BOX (box), p->upcoming);

  GtkWidget *header;
  GtkWidget *page = smail_page_new ("Calendar", scroll, &header);
  g_object_set_data_full (G_OBJECT (page), "calendar-page", p, g_free);
  g_object_set_data (G_OBJECT (page), "store", store);

  GtkWidget *add = smail_icon_button ("list-add-symbolic", "New Event");
  g_signal_connect (add, "clicked", G_CALLBACK (on_new_event), page);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), add);
  GtkWidget *today = gtk_button_new_with_label ("Today");
  g_signal_connect (today, "clicked", G_CALLBACK (on_today), page);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), today);

  const char *sigs[] = { "day-selected", "next-month", "prev-month", "next-year", "prev-year" };
  for (gsize i = 0; i < G_N_ELEMENTS (sigs); i++)
    g_signal_connect_swapped (p->calendar, sigs[i], G_CALLBACK (smail_calendar_page_refresh), page);

  smail_calendar_page_refresh (page);
  return page;
}

/* =====================================================================
 *  Tasks
 * ===================================================================== */

typedef struct {
  SMailStore *store;
  GtkWidget    *add_row;
  GtkWidget    *open_list;
  GtkWidget    *done_list;
  GtkWidget    *done_label;
} TasksPage;

static void open_task_dialog (GtkWidget *page, SMailTodo *t);

static void
on_task_toggled (GtkCheckButton *check, GtkWidget *page)
{
  SMailTodo *t = g_object_get_data (G_OBJECT (check), "todo");
  t->done = gtk_check_button_get_active (check);
  g_autoptr (GError) error = NULL;
  if (!smail_store_save_todo (page_store (page), t, &error))
    smail_toast (page, error->message);
  /* let the check animation finish before the row moves */
  g_timeout_add_once (250, (GSourceOnceFunc) smail_tasks_page_refresh, page);
}

static void
on_task_edit (GtkButton *b, GtkWidget *page)
{
  open_task_dialog (page, smail_todo_copy (g_object_get_data (G_OBJECT (b), "todo")));
}

static void
on_task_delete (GtkButton *b, GtkWidget *page)
{
  SMailTodo *t = g_object_get_data (G_OBJECT (b), "todo");
  smail_store_delete_todo (page_store (page), t->id, NULL);
  smail_tasks_page_refresh (page);
  smail_toast (page, "Task deleted");
}

static GtkWidget *
task_row (GtkWidget *page, SMailTodo *t)
{
  SMailTodo *copy = smail_todo_copy (t);
  GtkWidget *row = adw_action_row_new ();
  g_object_set_data_full (G_OBJECT (row), "todo", copy, (GDestroyNotify) smail_todo_free);
  g_autofree char *title = g_markup_escape_text (t->title, -1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (t->done)
    gtk_widget_add_css_class (row, "task-done");

  if (t->due) {
    g_autofree char *when = smail_format_unix (t->due);
    gboolean overdue = !t->done && t->due < smail_unix_now ();
    g_autofree char *sub = g_strdup_printf ("%s%s%s", overdue ? "Overdue · " : "Due ", when,
                                            t->remind && !t->done ? " · 🔔" : "");
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
    if (overdue)
      gtk_widget_add_css_class (row, "overdue");
  } else if (t->notes && *t->notes) {
    g_autofree char *notes = g_markup_escape_text (t->notes, -1);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), notes);
  }

  GtkWidget *check = gtk_check_button_new ();
  gtk_check_button_set_active (GTK_CHECK_BUTTON (check), t->done);
  gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (check, "selection-mode");
  gtk_accessible_update_property (GTK_ACCESSIBLE (check), GTK_ACCESSIBLE_PROPERTY_LABEL, "Done", -1);
  g_object_set_data (G_OBJECT (check), "todo", copy);
  g_signal_connect (check, "toggled", G_CALLBACK (on_task_toggled), page);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), check);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), check);

  GtkWidget *edit = smail_icon_button ("document-edit-symbolic", "Edit Task");
  gtk_widget_add_css_class (edit, "flat");
  gtk_widget_set_valign (edit, GTK_ALIGN_CENTER);
  g_object_set_data (G_OBJECT (edit), "todo", copy);
  g_signal_connect (edit, "clicked", G_CALLBACK (on_task_edit), page);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), edit);

  GtkWidget *del = delete_button ("Delete Task");
  g_object_set_data (G_OBJECT (del), "todo", copy);
  g_signal_connect (del, "clicked", G_CALLBACK (on_task_delete), page);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), del);
  return row;
}

void
smail_tasks_page_refresh (GtkWidget *page)
{
  TasksPage *p = g_object_get_data (G_OBJECT (page), "tasks-page");
  gtk_list_box_remove_all (GTK_LIST_BOX (p->open_list));
  gtk_list_box_remove_all (GTK_LIST_BOX (p->done_list));
  g_autoptr (GPtrArray) open = smail_store_list_todos (p->store, FALSE);
  g_autoptr (GPtrArray) done = smail_store_list_todos (p->store, TRUE);
  for (guint i = 0; i < open->len; i++)
    gtk_list_box_append (GTK_LIST_BOX (p->open_list), task_row (page, g_ptr_array_index (open, i)));
  for (guint i = 0; i < done->len; i++)
    gtk_list_box_append (GTK_LIST_BOX (p->done_list), task_row (page, g_ptr_array_index (done, i)));
  smail_boxed_list_done (p->open_list);
  gtk_widget_set_visible (p->done_list, done->len > 0);
  gtk_widget_set_visible (p->done_label, done->len > 0);
}

static void
on_quick_add (AdwEntryRow *row, GtkWidget *page)
{
  g_autofree char *text = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (row))));
  if (!*text)
    return;
  SMailTodo t = { 0 };
  t.title = text;
  g_autoptr (GError) error = NULL;
  if (!smail_store_save_todo (page_store (page), &t, &error)) {
    smail_toast (page, error->message);
    return;
  }
  gtk_editable_set_text (GTK_EDITABLE (row), "");
  smail_tasks_page_refresh (page);
}

static gboolean
save_task (Form *f, GError **error)
{
  SMailTodo *t = f->item;
  g_free (t->title); t->title = entry_value (f, "title");
  g_free (t->notes); t->notes = entry_value (f, "notes");
  gboolean has_due = adw_expander_row_get_enable_expansion (ADW_EXPANDER_ROW (form_get (f, "due-row")));
  t->due = has_due ? datetime_value (f, "due") : 0;
  t->remind = has_due && adw_switch_row_get_active (ADW_SWITCH_ROW (form_get (f, "remind")));
  return smail_store_save_todo (page_store (f->owner), t, error);
}

static void
delete_task (Form *f)
{
  smail_store_delete_todo (page_store (f->owner), ((SMailTodo *) f->item)->id, NULL);
}

static void
open_task_dialog (GtkWidget *page, SMailTodo *t)
{
  Form *f = form_new (t->id ? "Edit Task" : "New Task", t->id != 0, page, G_CALLBACK (smail_tasks_page_refresh));
  f->item = t;
  f->item_free = (GDestroyNotify) smail_todo_free;
  f->save = save_task;

  AdwPreferencesGroup *g = form_group (f, NULL);
  GtkWidget *title = form_entry (f, g, "title", "Title", t->title);
  form_entry (f, g, "notes", "Notes", t->notes);

  g = form_group (f, NULL);
  GtkWidget *due_row = adw_expander_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (due_row), "Due Date");
  adw_expander_row_set_show_enable_switch (ADW_EXPANDER_ROW (due_row), TRUE);
  adw_expander_row_set_enable_expansion (ADW_EXPANDER_ROW (due_row), t->due != 0);
  adw_expander_row_set_expanded (ADW_EXPANDER_ROW (due_row), t->due != 0);
  GtkWidget *when = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (when), "Due");
  GtkWidget *picker = smail_date_time_new (t->due ? t->due
                                                    : smail_next_quarter_hour (smail_unix_now () + 3600));
  adw_action_row_add_suffix (ADW_ACTION_ROW (when), picker);
  adw_expander_row_add_row (ADW_EXPANDER_ROW (due_row), when);
  GtkWidget *remind = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (remind), "Remind Me");
  adw_switch_row_set_active (ADW_SWITCH_ROW (remind), t->due ? t->remind : TRUE);
  adw_expander_row_add_row (ADW_EXPANDER_ROW (due_row), remind);
  adw_preferences_group_add (g, due_row);
  g_hash_table_insert (f->fields, "due-row", due_row);
  g_hash_table_insert (f->fields, "due", picker);
  g_hash_table_insert (f->fields, "remind", remind);

  if (t->id)
    form_add_delete (f, "Delete Task", delete_task);
  adw_dialog_present (f->dialog, page);
  gtk_widget_grab_focus (title);
}

static void
on_new_task (GtkButton *b, GtkWidget *page)
{
  open_task_dialog (page, g_new0 (SMailTodo, 1));
}

GtkWidget *
smail_tasks_page_new (SMailStore *store)
{
  TasksPage *p = g_new0 (TasksPage, 1);
  p->store = store;

  GtkWidget *box;
  GtkWidget *scroll = smail_clamped_column (&box);

  GtkWidget *quick = smail_boxed_list (NULL);
  p->add_row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (p->add_row), "Add a task");
  adw_entry_row_set_show_apply_button (ADW_ENTRY_ROW (p->add_row), TRUE);
  gtk_list_box_append (GTK_LIST_BOX (quick), p->add_row);
  gtk_box_append (GTK_BOX (box), quick);

  gtk_box_append (GTK_BOX (box), smail_section_label ("To Do"));
  p->open_list = smail_boxed_list ("Nothing to do 🎉");
  gtk_box_append (GTK_BOX (box), p->open_list);
  p->done_label = smail_section_label ("Completed");
  gtk_box_append (GTK_BOX (box), p->done_label);
  p->done_list = smail_boxed_list (NULL);
  gtk_box_append (GTK_BOX (box), p->done_list);

  GtkWidget *header;
  GtkWidget *page = smail_page_new ("Tasks", scroll, &header);
  g_object_set_data_full (G_OBJECT (page), "tasks-page", p, g_free);
  g_object_set_data (G_OBJECT (page), "store", store);
  g_signal_connect (p->add_row, "apply", G_CALLBACK (on_quick_add), page);
  g_signal_connect (p->add_row, "entry-activated", G_CALLBACK (on_quick_add), page);

  GtkWidget *add = smail_icon_button ("list-add-symbolic", "New Task with Due Date");
  g_signal_connect (add, "clicked", G_CALLBACK (on_new_task), page);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), add);

  smail_tasks_page_refresh (page);
  return page;
}

/* =====================================================================
 *  Reminders
 * ===================================================================== */

typedef struct {
  SMailStore *store;
  GtkWidget    *upcoming;
  GtkWidget    *past;
} RemindersPage;

static void
on_reminder_delete (GtkButton *b, GtkWidget *page)
{
  gint64 id = GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (b), "id"));
  smail_store_delete_reminder (page_store (page), id, NULL);
  smail_reminders_page_refresh (page);
}

static GtkWidget *
reminder_row (GtkWidget *page, SMailReminder *r)
{
  GtkWidget *row = adw_action_row_new ();
  g_autofree char *title = g_markup_escape_text (r->title, -1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  g_autofree char *when = smail_format_unix (r->at);
  const char *src = g_strcmp0 (r->source, "event") == 0 ? " · from Calendar"
                  : g_strcmp0 (r->source, "todo") == 0 ? " · from Tasks" : "";
  g_autofree char *sub = g_strconcat (when, src, NULL);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), gtk_image_new_from_icon_name ("alarm-symbolic"));
  GtkWidget *del = delete_button ("Delete Reminder");
  g_object_set_data (G_OBJECT (del), "id", GSIZE_TO_POINTER ((gsize) r->id));
  g_signal_connect (del, "clicked", G_CALLBACK (on_reminder_delete), page);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), del);
  return row;
}

void
smail_reminders_page_refresh (GtkWidget *page)
{
  RemindersPage *p = g_object_get_data (G_OBJECT (page), "reminders-page");
  gtk_list_box_remove_all (GTK_LIST_BOX (p->upcoming));
  gtk_list_box_remove_all (GTK_LIST_BOX (p->past));
  g_autoptr (GPtrArray) up = smail_store_list_reminders (p->store, FALSE, 200);
  g_autoptr (GPtrArray) past = smail_store_list_reminders (p->store, TRUE, 20);
  for (guint i = 0; i < up->len; i++)
    gtk_list_box_append (GTK_LIST_BOX (p->upcoming), reminder_row (page, g_ptr_array_index (up, i)));
  for (guint i = 0; i < past->len; i++)
    gtk_list_box_append (GTK_LIST_BOX (p->past), reminder_row (page, g_ptr_array_index (past, i)));
  smail_boxed_list_done (p->upcoming);
  smail_boxed_list_done (p->past);
}

static gboolean
save_reminder (Form *f, GError **error)
{
  SMailReminder *r = f->item;
  g_free (r->title);
  r->title = entry_value (f, "title");
  r->at = datetime_value (f, "at");
  if (!*r->title) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "What should I remind you about?");
    return FALSE;
  }
  if (r->at <= smail_unix_now ()) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Pick a time in the future");
    return FALSE;
  }
  return smail_store_save_reminder (page_store (f->owner), r, error);
}

static void
on_new_reminder (GtkButton *b, GtkWidget *page)
{
  SMailReminder *r = g_new0 (SMailReminder, 1);
  Form *f = form_new ("New Reminder", FALSE, page, G_CALLBACK (smail_reminders_page_refresh));
  f->item = r;
  f->item_free = (GDestroyNotify) smail_reminder_free;
  f->save = save_reminder;
  AdwPreferencesGroup *g = form_group (f, NULL);
  GtkWidget *title = form_entry (f, g, "title", "Remind me to…", NULL);
  form_datetime (f, g, "at", "When", smail_next_quarter_hour (smail_unix_now () + 30 * 60));
  adw_dialog_present (f->dialog, page);
  gtk_widget_grab_focus (title);
}

GtkWidget *
smail_reminders_page_new (SMailStore *store)
{
  RemindersPage *p = g_new0 (RemindersPage, 1);
  p->store = store;

  GtkWidget *box;
  GtkWidget *scroll = smail_clamped_column (&box);
  GtkWidget *hint = gtk_label_new ("You'll get a notification at the chosen time while SMail is running. "
                                   "Events and tasks with a due date add their reminders here too.");
  gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_xalign (GTK_LABEL (hint), 0);
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_box_append (GTK_BOX (box), hint);
  gtk_box_append (GTK_BOX (box), smail_section_label ("Upcoming"));
  p->upcoming = smail_boxed_list ("No reminders set");
  gtk_box_append (GTK_BOX (box), p->upcoming);
  gtk_box_append (GTK_BOX (box), smail_section_label ("Recent"));
  p->past = smail_boxed_list ("Nothing yet");
  gtk_box_append (GTK_BOX (box), p->past);

  GtkWidget *header;
  GtkWidget *page = smail_page_new ("Reminders", scroll, &header);
  g_object_set_data_full (G_OBJECT (page), "reminders-page", p, g_free);
  g_object_set_data (G_OBJECT (page), "store", store);
  GtkWidget *add = smail_icon_button ("list-add-symbolic", "New Reminder");
  g_signal_connect (add, "clicked", G_CALLBACK (on_new_reminder), page);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), add);

  smail_reminders_page_refresh (page);
  return page;
}
