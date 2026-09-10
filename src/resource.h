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

/* Spectrum preview panel - sits in the sidebar box above Activity Log
 * (the space that was always "reserved for other features" - see the
 * comment above g_sidebar_panel's creation in main.c). Not a real
 * capture, just a quick illustrative read on what's live: either all
 * 16 channels as a small-multiples grid, or one channel's mode-shaped
 * trace blown up full-size. */
#define IDC_SPECTRUM_PREV_BTN    1030
#define IDC_SPECTRUM_NEXT_BTN    1031
#define IDC_SPECTRUM_UNIT_LBL    1032
#define IDC_SPECTRUM_ALL_BTN     1033
#define IDC_SPECTRUM_PLOT        1034

#define ID_POLL_TIMER        1

/* Each of the 16 channel cards gets its controls at
 * IDC_CH_BASE + channel_index*IDC_CH_STRIDE + offset, rather than a
 * separate #define per control per channel. Layout matches the
 * sdr_react/sdr_app channel-card pattern: Mode combo + explicit Set
 * button (mode is not applied until Set is clicked), separate ON/OFF
 * power buttons, a status line, and a vertical level trackbar with
 * High/Medium/Low/Off tick labels. */
#define IDC_CH_BASE               2000
#define IDC_CH_STRIDE             10
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
