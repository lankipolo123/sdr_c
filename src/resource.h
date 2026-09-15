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

#define IDC_LOG_LISTBOX      1020
#define IDC_LOG_CLEAR_BTN    1021

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

/* "1st row".."4th row" - one label per grid row, in the strip carved
 * out of the signal-wave mark's dead space (see ROW_LABEL_STRIP_W in
 * main.c) so it's obvious which row IDC_BULK_ROWSELECT_COMBO's "1st
 * Row" etc. actually selects. Repositioned alongside the cards
 * themselves in relayout_for_size() as the window resizes. */
#define IDC_GRID_ROW_LBL_1   1060
#define IDC_GRID_ROW_LBL_2   1061
#define IDC_GRID_ROW_LBL_3   1062
#define IDC_GRID_ROW_LBL_4   1063

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

/* Each of the 16 channel cards gets its controls at
 * IDC_CH_BASE + channel_index*IDC_CH_STRIDE + offset, rather than a
 * separate #define per control per channel. Layout matches the
 * sdr_react/sdr_app channel-card pattern: Mode combo + explicit Set
 * button (mode is not applied until Set is clicked), separate ON/OFF
 * power buttons, a status line, and a vertical level trackbar with
 * High/Medium/Low/Off tick labels. */
#define IDC_CH_BASE               2000
#define IDC_CH_STRIDE             12
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
