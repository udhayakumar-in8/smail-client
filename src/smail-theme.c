/* smail-theme.c — elementary-OS-inspired Matte White and Matte Black styles.
 *
 * Both styles use the Inter typeface, the "Blueberry" accent, softly
 * graded buttons with a fine border and drop shadow, borderless icon
 * buttons in header bars and coloured suggested/destructive buttons.
 * They work by overriding libadwaita's named colours plus a few widget rules. */
#include "smail-theme.h"

static GtkCssProvider *provider;
static char *current;

/* ---------------- palettes ---------------- */

static const char *matte_white =
  "@define-color accent_bg_color #3689e6;"
  "@define-color accent_color #0d52bf;"
  "@define-color accent_fg_color #ffffff;"
  "@define-color destructive_bg_color #c6262e;"
  "@define-color destructive_color #a10705;"
  "@define-color destructive_fg_color #ffffff;"
  "@define-color success_color #3a9104;"
  "@define-color warning_color #cc3b02;"
  "@define-color error_color #c6262e;"
  "@define-color window_bg_color #fafafa;"
  "@define-color window_fg_color #333333;"
  "@define-color view_bg_color #ffffff;"
  "@define-color view_fg_color #333333;"
  "@define-color headerbar_bg_color #f2f2f2;"
  "@define-color headerbar_fg_color #333333;"
  "@define-color headerbar_border_color #333333;"
  "@define-color headerbar_backdrop_color #fafafa;"
  "@define-color headerbar_shade_color rgba(0,0,0,0.12);"
  "@define-color sidebar_bg_color #f0f0f0;"
  "@define-color sidebar_fg_color #333333;"
  "@define-color sidebar_backdrop_color #f5f5f5;"
  "@define-color sidebar_shade_color rgba(0,0,0,0.08);"
  "@define-color secondary_sidebar_bg_color #f7f7f7;"
  "@define-color secondary_sidebar_fg_color #333333;"
  "@define-color secondary_sidebar_backdrop_color #fafafa;"
  "@define-color secondary_sidebar_shade_color rgba(0,0,0,0.07);"
  "@define-color card_bg_color #ffffff;"
  "@define-color card_fg_color #333333;"
  "@define-color card_shade_color rgba(0,0,0,0.08);"
  "@define-color dialog_bg_color #fafafa;"
  "@define-color dialog_fg_color #333333;"
  "@define-color popover_bg_color #ffffff;"
  "@define-color popover_fg_color #333333;"
  "@define-color popover_shade_color rgba(0,0,0,0.08);"
  "@define-color shade_color rgba(0,0,0,0.08);"
  "@define-color scrollbar_outline_color #ffffff;"
  /* button material */
  "@define-color smail_btn_top #ffffff;"
  "@define-color smail_btn_bottom #f0f0f0;"
  "@define-color smail_btn_hover_top #ffffff;"
  "@define-color smail_btn_hover_bottom #f7f7f7;"
  "@define-color smail_btn_active #e6e6e6;"
  "@define-color smail_btn_border rgba(0,0,0,0.20);"
  "@define-color smail_btn_highlight rgba(255,255,255,0.9);"
  "@define-color smail_btn_shadow rgba(0,0,0,0.08);"
  "@define-color smail_hb_top #f7f7f7;"
  "@define-color smail_hb_bottom #ebebeb;"
  "@define-color smail_hb_line rgba(0,0,0,0.13);"
  "@define-color smail_entry_bg #ffffff;"
  "@define-color smail_entry_border rgba(0,0,0,0.20);"
  "@define-color smail_selection rgba(54,137,230,0.16);";

static const char *matte_black =
  "@define-color accent_bg_color #3689e6;"
  "@define-color accent_color #64baff;"
  "@define-color accent_fg_color #ffffff;"
  "@define-color destructive_bg_color #c6262e;"
  "@define-color destructive_color #ff8c82;"
  "@define-color destructive_fg_color #ffffff;"
  "@define-color success_color #9bdb4d;"
  "@define-color warning_color #ffa154;"
  "@define-color error_color #ed5353;"
  "@define-color window_bg_color #1c1c1c;"
  "@define-color window_fg_color #f2f2f2;"
  "@define-color view_bg_color #161616;"
  "@define-color view_fg_color #f2f2f2;"
  "@define-color headerbar_bg_color #262626;"
  "@define-color headerbar_fg_color #f2f2f2;"
  "@define-color headerbar_border_color #f2f2f2;"
  "@define-color headerbar_backdrop_color #1c1c1c;"
  "@define-color headerbar_shade_color rgba(0,0,0,0.45);"
  "@define-color sidebar_bg_color #202020;"
  "@define-color sidebar_fg_color #f2f2f2;"
  "@define-color sidebar_backdrop_color #1c1c1c;"
  "@define-color sidebar_shade_color rgba(0,0,0,0.35);"
  "@define-color secondary_sidebar_bg_color #1b1b1b;"
  "@define-color secondary_sidebar_fg_color #f2f2f2;"
  "@define-color secondary_sidebar_backdrop_color #191919;"
  "@define-color secondary_sidebar_shade_color rgba(0,0,0,0.35);"
  "@define-color card_bg_color #242424;"
  "@define-color card_fg_color #f2f2f2;"
  "@define-color card_shade_color rgba(0,0,0,0.40);"
  "@define-color dialog_bg_color #222222;"
  "@define-color dialog_fg_color #f2f2f2;"
  "@define-color popover_bg_color #2a2a2a;"
  "@define-color popover_fg_color #f2f2f2;"
  "@define-color popover_shade_color rgba(0,0,0,0.40);"
  "@define-color shade_color rgba(0,0,0,0.40);"
  "@define-color scrollbar_outline_color rgba(0,0,0,0.5);"
  "@define-color smail_btn_top #3a3a3a;"
  "@define-color smail_btn_bottom #303030;"
  "@define-color smail_btn_hover_top #444444;"
  "@define-color smail_btn_hover_bottom #393939;"
  "@define-color smail_btn_active #262626;"
  "@define-color smail_btn_border rgba(0,0,0,0.70);"
  "@define-color smail_btn_highlight rgba(255,255,255,0.07);"
  "@define-color smail_btn_shadow rgba(0,0,0,0.30);"
  "@define-color smail_hb_top #2b2b2b;"
  "@define-color smail_hb_bottom #232323;"
  "@define-color smail_hb_line rgba(0,0,0,0.60);"
  "@define-color smail_entry_bg #121212;"
  "@define-color smail_entry_border rgba(0,0,0,0.70);"
  "@define-color smail_selection rgba(54,137,230,0.28);";

/* ---------------- shared elementary-style rules ---------------- */

static const char *elementary_rules =
  /* typography */
  "window, dialog, popover, tooltip, .background {"
  "  font-family: 'Inter', 'Open Sans', 'Cantarell', sans-serif; font-size: 10pt;"
  "  font-feature-settings: 'cv11', 'ss03', 'tnum'; }"
  ".title-1, .title-2, .title-3, .title-4, .heading { font-weight: 700; letter-spacing: -0.01em; }"
  "headerbar .title { font-weight: 700; }"
  ".caption, .dim-label { letter-spacing: 0.01em; }"

  /* header bars: soft gradient, fine bottom line */
  "headerbar { background-image: linear-gradient(to bottom, @smail_hb_top, @smail_hb_bottom);"
  "  box-shadow: inset 0 -1px @smail_hb_line, inset 0 1px rgba(255,255,255,0.05); min-height: 46px; }"
  "headerbar:backdrop { background-image: none; background-color: @smail_hb_bottom; }"

  /* buttons: matte gradient + border + highlight + drop shadow */
  "button:not(.flat):not(.circular):not(.suggested-action):not(.destructive-action):not(.link):not(.image-button):not(.close):not(.minimize):not(.maximize),"
  "menubutton:not(.flat) > button:not(.image-button), dropdown > button, spinbutton > button, splitbutton > button {"
  "  border-radius: 5px; border: 1px solid @smail_btn_border;"
  "  background-image: linear-gradient(to bottom, @smail_btn_top, @smail_btn_bottom);"
  "  background-color: @smail_btn_bottom;"
  "  box-shadow: inset 0 1px @smail_btn_highlight, 0 1px 2px @smail_btn_shadow;"
  "  padding: 4px 12px; min-height: 22px; font-weight: 500; }"
  "button:not(.flat):not(.circular):not(.suggested-action):not(.destructive-action):not(.link):not(.image-button):not(.close):not(.minimize):not(.maximize):hover,"
  "menubutton:not(.flat) > button:not(.image-button):hover, dropdown > button:hover {"
  "  background-image: linear-gradient(to bottom, @smail_btn_hover_top, @smail_btn_hover_bottom); }"
  "button:not(.flat):not(.circular):not(.suggested-action):not(.destructive-action):not(.link):not(.image-button):not(.close):not(.minimize):not(.maximize):active,"
  "button:not(.flat):not(.circular):not(.suggested-action):not(.destructive-action):checked,"
  "menubutton:not(.flat) > button:not(.image-button):checked, dropdown > button:active {"
  "  background-image: none; background-color: @smail_btn_active;"
  "  box-shadow: inset 0 1px 2px @smail_btn_shadow; }"
  "button:disabled { opacity: 0.55; box-shadow: none; }"
  "spinbutton > button { padding: 2px 8px; border-radius: 0; box-shadow: none; }"
  "spinbutton > button:first-child { border-radius: 5px 0 0 5px; }"
  "spinbutton > button:last-child { border-radius: 0 5px 5px 0; }"
  "button.pill { border-radius: 999px; padding: 8px 28px; }"

  /* icon buttons in header bars are borderless, like elementary apps */
  "headerbar button.image-button:not(.suggested-action):not(.destructive-action),"
  "headerbar menubutton > button.image-button, headerbar button.toggle.image-button {"
  "  background: none; border-color: transparent; box-shadow: none; padding: 4px 7px; }"
  "headerbar button.image-button:not(.suggested-action):hover,"
  "headerbar menubutton > button.image-button:hover { background-color: alpha(currentColor, 0.08); }"
  "headerbar button.image-button:not(.suggested-action):checked,"
  "headerbar button.image-button:not(.suggested-action):active { background-color: alpha(currentColor, 0.14); }"
  "button.image-button:not(.suggested-action):not(.destructive-action):not(.flat):not(.circular) {"
  "  border-radius: 5px; }"
  "windowcontrols button { background: none; border: none; box-shadow: none; padding: 0 2px; }"

  /* coloured action buttons */
  "button.suggested-action {"
  "  border-radius: 5px; border: 1px solid #0d52bf; color: #ffffff;"
  "  background-image: linear-gradient(to bottom, #5ea8f2, #3689e6);"
  "  box-shadow: inset 0 1px rgba(255,255,255,0.25), 0 1px 2px rgba(0,0,0,0.18);"
  "  text-shadow: 0 1px rgba(0,0,0,0.25); font-weight: 600; }"
  "button.suggested-action:hover { background-image: linear-gradient(to bottom, #6db4f7, #4294ea); }"
  "button.suggested-action:active { background-image: none; background-color: #2a78cf; box-shadow: inset 0 1px 2px rgba(0,0,0,0.3); }"
  "button.destructive-action {"
  "  border-radius: 5px; border: 1px solid #a10705; color: #ffffff;"
  "  background-image: linear-gradient(to bottom, #ed5353, #c6262e);"
  "  box-shadow: inset 0 1px rgba(255,255,255,0.2), 0 1px 2px rgba(0,0,0,0.18);"
  "  text-shadow: 0 1px rgba(0,0,0,0.25); font-weight: 600; }"
  "button.destructive-action:active { background-image: none; background-color: #b11f27; }"
  "button.circular.suggested-action { border-radius: 999px; }"
  "button.pill.suggested-action, button.pill.destructive-action { border-radius: 999px; }"

  /* entries */
  "entry, searchbar entry, spinbutton {"
  "  background-color: @smail_entry_bg; border: 1px solid @smail_entry_border; border-radius: 5px;"
  "  box-shadow: inset 0 1px 1px rgba(0,0,0,0.06); min-height: 30px; }"
  "entry:focus-within, spinbutton:focus-within { border-color: @accent_bg_color;"
  "  box-shadow: 0 0 0 3px alpha(@accent_bg_color, 0.25); outline: none; }"

  /* selection colours */
  ".navigation-sidebar > row:selected { background-color: @smail_selection; color: @accent_color; font-weight: 600; }"
  ".navigation-sidebar > row:selected image { color: @accent_color; }"
  ".message-list > row:selected { background-color: @smail_selection; }"
  ".message-list > row { border-bottom: 1px solid alpha(currentColor, 0.06); }"

  /* cards and lists: crisp border plus soft shadow */
  ".boxed-list, .card { border: 1px solid alpha(black, 0.10);"
  "  box-shadow: 0 1px 3px @card_shade_color; border-radius: 8px; }"
  "popover > contents { border-radius: 8px; }"
  "switch { border: 1px solid @smail_btn_border; }"
  "switch:checked { background-color: @accent_bg_color; }";

/* ---------------- apply ---------------- */

gboolean
smail_theme_is_valid (const char *name)
{
  return g_strcmp0 (name, SMAIL_THEME_SYSTEM) == 0 ||
         g_strcmp0 (name, SMAIL_THEME_MATTE_WHITE) == 0 ||
         g_strcmp0 (name, SMAIL_THEME_MATTE_BLACK) == 0;
}

const char *
smail_theme_current (void)
{
  return current ? current : SMAIL_THEME_SYSTEM;
}

void
smail_theme_apply (const char *name)
{
  if (!smail_theme_is_valid (name))
    name = SMAIL_THEME_SYSTEM;
  g_free (current);
  current = g_strdup (name);

  GdkDisplay *display = gdk_display_get_default ();
  if (!display)
    return;
  if (!provider) {
    provider = gtk_css_provider_new ();
    /* above the app's own CSS, below user overrides */
    gtk_style_context_add_provider_for_display (display, GTK_STYLE_PROVIDER (provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  }

  AdwStyleManager *sm = adw_style_manager_get_default ();
  GtkSettings *settings = gtk_settings_get_default ();

  if (g_str_equal (name, SMAIL_THEME_SYSTEM)) {
    gtk_css_provider_load_from_string (provider, "");
    adw_style_manager_set_color_scheme (sm, ADW_COLOR_SCHEME_DEFAULT);
    gtk_settings_reset_property (settings, "gtk-font-name");
    return;
  }

  gboolean dark = g_str_equal (name, SMAIL_THEME_MATTE_BLACK);
  adw_style_manager_set_color_scheme (sm, dark ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_FORCE_LIGHT);
  g_autofree char *css = g_strconcat (dark ? matte_black : matte_white, elementary_rules, NULL);
  gtk_css_provider_load_from_string (provider, css);
  g_object_set (settings, "gtk-font-name", "Inter 10", NULL);
}
