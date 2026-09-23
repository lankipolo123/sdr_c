#pragma once

#define IDI_APP_ICON         100

#define IDC_PORT_COMBO       1001
#define IDC_REFRESH_BTN      1002
#define IDC_CONNECT_BTN      1003
#define IDC_CONN_STATUS_LBL  1004
#define IDC_BAUD_COMBO       1005
#define IDC_DATABITS_COMBO   1006
#define IDC_PARITY_COMBO     1007

#define IDC_SENSOR_PORT_COMBO    1010
#define IDC_SENSOR_CONNECT_BTN   1011
#define IDC_SENSOR_STATUS_LBL    1012
#define IDC_SENSOR_TEMP_LBL      1013
#define IDC_KILL_STATUS_LBL      1015
#define IDC_KILL_RESET_BTN       1016
#define IDC_SENSOR_REFRESH_BTN   1017
/* Opens sensor_log.csv (see sensor_log.h) in whatever's associated
 * with .csv - Excel if installed, else the OS's own picker/Notepad.
 * Direct request - the CSV logging existed with no way to actually
 * get to the file from inside the app. */
#define IDC_OPEN_LOG_BTN         1018
/* Light/Dark toggle - see apply_theme()/create_theme_brushes() in
 * main.c. Runtime-only + persisted to the .ini's [UI] LightMode key,
 * same GetPrivateProfileIntA/WritePrivateProfileStringA pattern as
 * every other saved setting (load_settings()/save_settings()). */
#define IDC_THEME_TOGGLE_BTN     1019
/* Manual trip - same slot as IDC_KILL_RESET_BTN, mutually exclusive
 * visibility (armed shows this, tripped shows Reset instead) - see
 * ui_refresh_kill_switch() in main.c. */
#define IDC_KILL_TRIP_BTN        1065
/* Small caption static above IDC_OPEN_LOG_BTN in the Quick Actions
 * panel ("Logs") - needs its own ID (unlike the Theme column's
 * caption) because it's right-anchored to the panel edge the same
 * way the button itself is, so relayout_for_size() has to reposition
 * it on resize too. Direct request - the three Quick Actions buttons
 * (Kill Switch/Reset, Open Log, Light Mode) didn't visually align
 * with each other; a caption row above each button lines them up.
 * No longer created (see build_controls()'s Command Panel comment -
 * Light Mode moved out, the panel became a single icon-button row
 * instead of captioned columns) - left defined rather than renumbering
 * everything after it. */
#define IDC_LOG_CAPTION_LBL      1066
/* Command Panel's rack-wide power actions - see on_close_all_clicked()/
 * on_open_all_clicked() in main.c. Same gating convention as Bulk
 * Actions (OFF always works even kill-switch-tripped, ON skips a
 * tripped channel) but acting on all 16 channels regardless of Bulk
 * Actions' own selection. */
#define IDC_CLOSE_ALL_BTN        1067
#define IDC_OPEN_ALL_BTN         1068
/* Direct request: "add the change icon here also" - a shortcut to the
 * same browse_and_set_logo() the header's own Change Logo button
 * (IDC_CHANGE_LOGO_BTN, behind the padlock badge) already calls. That
 * one stays as-is; this is an additional, always-visible way to reach
 * the same feature from the Command Panel. */
#define IDC_CMD_CHANGE_ICON_BTN  1069
/* Caption above the Summary card's Mode toggle (IDC_THEME_TOGGLE_BTN,
 * relocated there - see its own WM_DRAWITEM comment in main.c). Right-
 * anchored to the card's own right edge like the toggle itself, so it
 * needs its own ID for relayout_for_size() to reposition on resize. */
#define IDC_MODE_CAPTION_LBL     1070

#define IDC_LOG_LISTBOX      1020
#define IDC_LOG_CLEAR_BTN    1021
/* Opens a bigger, dedicated popup window showing the whole log as one
 * scrollable block of text (not owned by the small embedded listbox) -
 * direct request, the listbox itself is cramped for actually reading
 * back through a long session. Snapshot, not live-updating - see
 * on_log_view_clicked()'s own comment in main.c. */
#define IDC_LOG_VIEW_BTN     1060
#define IDC_LOG_VIEW_CLOSE_BTN 1061
#define IDC_LOG_VIEW_EDIT    1062

/* Lets the user swap the header's HelixDefender mark for their own .bmp -
 * see load_custom_logo()/save_custom_logo() in main.c. The picked file
 * is copied to branding.bmp next to the .exe (not referenced by its
 * original path, which could move/disappear) - the .ini only records
 * the original filename picked, for display/reference, not as the
 * thing actually loaded on startup. */
#define IDC_CHANGE_LOGO_BTN  1022
/* Deletes branding.bmp and restores the built-in embedded icon/vector
 * mark - see reset_custom_logo() in main.c. */
#define IDC_RESET_LOGO_BTN   1023
/* Small round badge overlapping the logo's bottom-right corner, same
 * idea as a profile picture's edit badge in a modern app - Change Logo/
 * Reset stay hidden until this is clicked, instead of sitting there
 * permanently. Toggles between a closed and an open padlock glyph so
 * the badge itself shows which state it's in - see
 * IDC_CHANGE_LOGO_BTN's WM_DRAWITEM case and g_logo_options_visible in
 * main.c. */
#define IDC_LOGO_LOCK_BTN    1024

/* Bulk Actions bar, above the channel grid - click a card's checkbox to
 * select it (lit accent border), then one of these applies to every
 * selected channel at once instead of clicking through cards one at a
 * time. Same gating as each card's own controls: OFF always works even
 * kill-switch-tripped, ON/Set/level skip a tripped channel. Always
 * expanded - no toggle/collapse.
 *
 * IDC_BULK_TOGGLE_BTN does NOT hide anything - it arms/disarms
 * clicking a card's plain background as a second way to select it
 * (off by default; the checkbox itself always works regardless). It's
 * wired through WM_LBUTTONDOWN coordinate math on the main window, not
 * the card panel's own click handling - see g_bulk_select_mode's
 * comment in main.c for why. */
#define IDC_BULK_TOGGLE_BTN     1039
#define IDC_BULK_SELECTED_LBL   1040
#define IDC_BULK_CLEAR_BTN      1041
#define IDC_BULK_MODE_COMBO     1042
#define IDC_BULK_SET_BTN        1043
#define IDC_BULK_ON_BTN         1044
#define IDC_BULK_OFF_BTN        1045
#define IDC_BULK_HIGH_BTN       1046
#define IDC_BULK_MEDIUM_BTN     1047
#define IDC_BULK_LOW_BTN        1048
#define IDC_BULK_LEVEL_OFF_BTN  1049
#define IDC_BULK_SELECT_ALL_BTN 1050
/* Quick-select presets - 1st/2nd/3rd/4th Row (matching the 4x4 channel
 * grid's own rows), Select All (same effect as IDC_BULK_SELECT_ALL_BTN),
 * and Custom (a no-op - leaves whatever's currently selected alone, for
 * picking channels by hand via checkbox/Card Click). Applies on
 * CBN_SELCHANGE, no separate Set step - see bulk_select_row() in
 * main.c. */
#define IDC_BULK_ROWSELECT_COMBO 1038

/* Modal password prompt gating Continuous Wave mode - see unlock_cw()
 * and cw_password_dlg_proc() in main.c. The real password comes from
 * the vendor DLL's GetDllPassword export, not anything this app makes
 * up itself. */
#define IDD_CW_PASSWORD      1051
#define IDC_CW_PW_EDIT       1052

/* Spectrum panel - sits in the sidebar box above Activity Log (the
 * space that was always "reserved for other features" - see the
 * comment above g_sidebar_panel's creation in main.c). Not a capture -
 * this app has no receiver - but not a guess either: every channel's
 * mode/level/on-off is exactly what this app itself commanded, so the
 * trace shape (matched against a real ZS-407 capture per mode earlier)
 * and the exact dBm/MHz caption both come straight from real, known
 * state. IDC_SPECTRUM_UNIT_COMBO jumps directly to one of 1-16;
 * IDC_SPECTRUM_ALL_BTN goes back to the all-16 overview grid. */
#define IDC_SPECTRUM_UNIT_COMBO  1030
#define IDC_SPECTRUM_ALL_BTN     1031
#define IDC_SPECTRUM_PLOT        1032

#define ID_POLL_TIMER        1

/* Posted from WM_CREATE instead of running the startup auto-connect
 * inline there - WM_CREATE fires DURING CreateWindowExA, well before
 * WinMain's own ShowWindow() call, so the loading dialog would appear
 * over an empty/black desktop with the main window not drawn yet if it
 * ran there directly (reported directly - "why the app has to be
 * black when a dialog just load"). A posted message is only handled
 * once the message loop actually runs, which is after ShowWindow(). */
#define WM_APP_AUTOCONNECT   (WM_APP + 1)

/* Each of the 16 channel cards gets its controls at
 * IDC_CH_BASE + channel_index*IDC_CH_STRIDE + offset, rather than a
 * separate #define per control per channel. Layout matches the
 * sdr_react/sdr_app channel-card pattern: Mode combo + explicit Set
 * button (mode is not applied until Set is clicked), separate ON/OFF
 * power buttons, a status line, and a vertical level trackbar with
 * High/Medium/Low/Off tick labels. */
#define IDC_CH_BASE               2000
#define IDC_CH_STRIDE             13
#define IDC_CH_MODE_OFFSET        0
#define IDC_CH_SET_OFFSET         1
#define IDC_CH_ON_OFFSET          2
#define IDC_CH_OFF_OFFSET         3
/* Status line (SENDING.../level name/STANDBY). Also doubles as that
 * unit's kill-switch reset control: while tripped it shows "TRIPPED -
 * reset?" in red - click it to reset just this one channel. The kill
 * switch itself is rack-wide (see KILL_SWITCH_THRESHOLD_C in main.c) -
 * there are only 6 physical sensors scanning the area, not one per
 * channel - so a channel reset this way will retrip on the next tick
 * if the rack-wide average is still over threshold. (No per-channel
 * temperature/humidity readout on the card itself anymore - tried,
 * dropped again; the rack-wide average still shows in the header.) */
#define IDC_CH_STATUS_OFFSET      4
#define IDC_CH_TRACKBAR_OFFSET    5
#define IDC_CH_LBL_HIGH_OFFSET    6
#define IDC_CH_LBL_MEDIUM_OFFSET  7
#define IDC_CH_LBL_LOW_OFFSET     8
#define IDC_CH_LBL_OFF_OFFSET     9
/* Bulk Actions selection checkbox, top-right corner of the card - a
 * real dedicated click target, not "click somewhere on the card's
 * background that isn't already covered by a real control" (that
 * turned out to be genuinely hard to hit once connected, when every
 * other control on the card is itself clickable and swallows the
 * click first - reported as "this is unclickable"). */
#define IDC_CH_SELECT_OFFSET      10
/* Cumulative time this channel has actually been ON (transmitting) -
 * an odometer, not a session timer: keeps counting across app restarts
 * (persisted in the .ini alongside Mode/Level/Output), frozen (not
 * reset) while the channel is off. See channel_uptime_seconds() and the
 * per-tick accounting in WM_TIMER, main.c. */
#define IDC_CH_UPTIME_OFFSET      11
/* Real operating frequency (channel_freq_mhz(), main.c/channels.c) -
 * static per card, set once at creation, white text above the level
 * gauge. Direct request, added alongside the spectrum plot's real
 * frequency axis labels. */
#define IDC_CH_FREQ_OFFSET        12
