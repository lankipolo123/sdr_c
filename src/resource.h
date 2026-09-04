#pragma once

#define IDI_APP_ICON         100

#define IDC_PORT_COMBO       1001
#define IDC_REFRESH_BTN      1002
#define IDC_CONNECT_BTN      1003
#define IDC_CONN_STATUS_LBL  1004
#define IDC_BAUD_COMBO       1005
#define IDC_DATABITS_COMBO   1006
#define IDC_PARITY_COMBO     1007

#define IDC_WARNING_LBL      1008

#define IDC_SENSOR_PORT_COMBO    1010
#define IDC_SENSOR_CONNECT_BTN   1011
#define IDC_SENSOR_STATUS_LBL    1012
#define IDC_SENSOR_TEMP_LBL      1013
#define IDC_SENSOR_HUMIDITY_LBL  1014

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
#define IDC_CH_STATUS_OFFSET      4
#define IDC_CH_TRACKBAR_OFFSET    5
#define IDC_CH_LBL_HIGH_OFFSET    6
#define IDC_CH_LBL_MEDIUM_OFFSET  7
#define IDC_CH_LBL_LOW_OFFSET     8
#define IDC_CH_LBL_OFF_OFFSET     9
