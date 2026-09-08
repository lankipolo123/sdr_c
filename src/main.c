/* Digital Noise Configuration - Multi: 16-channel blind-send control panel.
 * Separate build from the single-channel app (../src) - shares the
 * protocol/connection/serial_port layer unchanged (copied in, not
 * touched), but uses its own channels.c blind-send logic instead of
 * device.c's wait-for-ACK model. See channels.h for why.
 *
 * No Qt, no pywebview, no vendor DLL - just user32/gdi32/kernel32/advapi32,
 * same as the single-channel app (plus msimg32 for GradientFill, used by
 * the temperature gauge).
 */
#define _WIN32_WINNT 0x0600 /* Vista+ - needed so windows.h declares
                              * GradientFill/TRIVERTEX/GRADIENT_RECT */
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "resource.h"
#include "connection.h"
#include "channels.h"
#include "sensor.h"

#define CLIENT_WIDTH  1343
#define CLIENT_HEIGHT 644

/* App-title header bar across the top, above the sidebar/grid content -
 * empty except for a title for now, room left for whatever gets added
 * to it later. HEADER_H is the bar's own height; CONTENT_TOP is where
 * the sidebar panels and channel grid start beneath it (same 6px top
 * margin and 8px panel-to-panel gap used everywhere else). */
#define HEADER_H     48
#define CONTENT_TOP  62

static const int BAUD_OPTIONS[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 2000000 };
#define BAUD_OPTIONS_COUNT 9
#define BAUD_DEFAULT_INDEX 4 /* 115200 */

static const int DATABITS_OPTIONS[] = { 5, 6, 7, 8 };
#define DATABITS_OPTIONS_COUNT 4
#define DATABITS_DEFAULT_INDEX 3 /* 8 */

static const char *const PARITY_LABELS[] = { "None", "Odd", "Even", "Mark", "Space" };
static const char PARITY_CODES[] = { 'N', 'O', 'E', 'M', 'S' };
#define PARITY_OPTIONS_COUNT 5

static const char *const LEVEL_LABELS[] = { "Off", "Low", "Medium", "High" };

/* Kill switch: forces a unit off if its sensor reports a dangerously
 * high temperature - its own unit only, not the other 15, since each
 * has its own independent sensor in per-unit mode (scan mode's single
 * shared reading is mirrored into every unit, so it naturally trips all
 * of them together instead needing separate logic for that case).
 * Manual reset only, deliberately, per unit - once tripped, that unit
 * stays off (and new ON/Set/level commands for it are blocked) even if
 * its temperature drops back down, until the user explicitly resets it.
 * Auto-resuming at the threshold would let it silently cycle on/off
 * right at the boundary, defeating the point of a safety cutoff. */
#define KILL_SWITCH_THRESHOLD_C 60.0f

#define LOG_MAX_ENTRIES 200

/* Modbus slave address each unit's own temperature sensor is wired to,
 * used in per-unit mode only (scan mode always polls SENSOR_SLAVE_ADDR).
 * Defaults to the unit number, 1-indexed - edit this table once the real
 * per-unit wiring is known, since it's very likely not sequential. Not
 * shown in the UI (see the card's Temp readout instead) - pushed into
 * the sensor at WM_CREATE via sensor_set_unit_address(). */
static const uint8_t UNIT_TEMP_ADDR[MAX_CHANNELS] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
};

/* MILITRONIX Dark palette - same as the single-channel app. */
#define COLOR_APP_PAGE_BG   RGB(32, 33, 36)
#define COLOR_APP_PANEL_BG  RGB(43, 45, 49)
#define COLOR_APP_TEXT      RGB(232, 233, 234)
#define COLOR_APP_MUTED     RGB(154, 156, 160)
#define COLOR_APP_ACCENT    RGB(26, 133, 184)
#define COLOR_APP_ACCENT_DIS RGB(58, 74, 82)
#define COLOR_APP_HEADER    RGB(58, 168, 221)
#define COLOR_APP_FIELD_BG  RGB(23, 24, 26)
#define COLOR_APP_CONNECTED RGB(58, 181, 94)
#define COLOR_APP_DISCONNECTED RGB(224, 90, 90)
#define COLOR_APP_DOT       RGB(50, 52, 57)
#define COLOR_APP_PANEL_BORDER RGB(63, 66, 71)
#define COLOR_APP_SILVER    RGB(176, 180, 186)

#define PANEL_CHAMFER 8
#define DOT_GRID_SPACING 8
#define DOT_GRID_SIZE 2

/* --- grid layout for the 16 channel cards --- */
#define GRID_COLS 4
#define GRID_ROWS 4
#define CARD_W 246
#define CARD_H 130
#define CARD_GAP 8
#define GRID_LEFT 325
#define GRID_TOP CONTENT_TOP

#define SIDEBAR_X 10
#define SIDEBAR_W 305

static HINSTANCE g_hinst;
static HWND g_hwnd;
static HFONT g_font;
static HFONT g_header_font;
static HFONT g_title_font; /* app-title header bar only - bigger than g_header_font's panel-title size */
static WNDPROC g_panel_orig_proc;
static HBRUSH g_brush_panel;
static HBRUSH g_brush_page;
static HBRUSH g_brush_field;
static HBRUSH g_brush_accent;
static HBRUSH g_brush_accent_dis;
static HBRUSH g_brush_dot;
static HBRUSH g_brush_connected;
static HBRUSH g_brush_disconnected;
static HBRUSH g_brush_silver;
static HBRUSH g_brush_dot_pattern; /* tiled DOT_GRID_SPACING x DOT_GRID_SPACING bitmap brush */
static HBITMAP g_dot_pattern_bmp;

static Connection g_conn;
static Sensor g_sensor;
static bool g_kill_switch_tripped[MAX_CHANNELS];

/* Handles needed to reposition things on WM_SIZE that don't otherwise
 * have a retrievable control ID (channel_*_id() covers everything else
 * per-card - GetDlgItem() finds those directly). */
static HWND g_header_panel;
static HWND g_title_ctrl;
static HWND g_sidebar_panel;
static HWND g_card_panel[MAX_CHANNELS];
static HWND g_card_icon[MAX_CHANNELS];
static HWND g_card_header[MAX_CHANNELS];
static HWND g_card_bandwidth_lbl[MAX_CHANNELS];
static bool g_layout_ready; /* true once build_controls() has run - WM_SIZE
                              * fires during window creation, before that */
static int g_last_client_w = -1; /* last size relayout_for_size() actually
                                    * ran for - skip WM_SIZE calls that don't
                                    * change this (see WM_SIZE below) */
static int g_last_client_h = -1;

/* ---- small control-creation helper ---- */

static HWND add_ctrl(HWND parent, LPCSTR cls, LPCSTR text, DWORD style, int x, int y, int w, int h, int id) {
    HWND ctrl = CreateWindowExA(0, cls, text, style | WS_CHILD | WS_VISIBLE,
                                 x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hinst, NULL);
    if (ctrl) {
        SendMessageA(ctrl, WM_SETFONT, (WPARAM)g_font, (LPARAM)TRUE);
    }
    return ctrl;
}

/* Chamfered-corner panel painting - same subclass pattern as the
 * single-channel app's panels, smaller chamfer to suit the compact
 * channel cards. */
static LRESULT CALLBACK panel_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        POINT pts[8];
        POINT tri[3];
        HBRUSH old_brush;
        HPEN pen, old_pen, silver_pen, old_silver_pen;
        int c = PANEL_CHAMFER;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);

        pts[0].x = rc.left;              pts[0].y = rc.top + c;
        pts[1].x = rc.left + c;          pts[1].y = rc.top;
        pts[2].x = rc.right - 1 - c;     pts[2].y = rc.top;
        pts[3].x = rc.right - 1;         pts[3].y = rc.top + c;
        pts[4].x = rc.right - 1;         pts[4].y = rc.bottom - 1 - c;
        pts[5].x = rc.right - 1 - c;     pts[5].y = rc.bottom - 1;
        pts[6].x = rc.left + c;          pts[6].y = rc.bottom - 1;
        pts[7].x = rc.left;              pts[7].y = rc.bottom - 1 - c;

        old_brush = (HBRUSH)SelectObject(hdc, g_brush_panel);
        pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
        old_pen = (HPEN)SelectObject(hdc, pen);

        Polygon(hdc, pts, 8);

        SelectObject(hdc, old_pen);
        DeleteObject(pen);

        /* A small silver triangle floating near each corner, inset from
         * the panel's chamfer edge - same accent as the single-channel
         * app's panels. */
        {
            int t = c - 2; /* smaller than the chamfer cut itself */
            if (t < 2) t = 2;

            silver_pen = CreatePen(PS_SOLID, 1, COLOR_APP_SILVER);
            old_silver_pen = (HPEN)SelectObject(hdc, silver_pen);
            SelectObject(hdc, g_brush_silver);

            tri[0].x = rc.left;       tri[0].y = rc.top;
            tri[1].x = rc.left + t;   tri[1].y = rc.top;
            tri[2].x = rc.left;       tri[2].y = rc.top + t;
            Polygon(hdc, tri, 3);

            tri[0].x = rc.right - 1;      tri[0].y = rc.top;
            tri[1].x = rc.right - 1 - t;  tri[1].y = rc.top;
            tri[2].x = rc.right - 1;      tri[2].y = rc.top + t;
            Polygon(hdc, tri, 3);

            tri[0].x = rc.right - 1;      tri[0].y = rc.bottom - 1;
            tri[1].x = rc.right - 1 - t;  tri[1].y = rc.bottom - 1;
            tri[2].x = rc.right - 1;      tri[2].y = rc.bottom - 1 - t;
            Polygon(hdc, tri, 3);

            tri[0].x = rc.left;       tri[0].y = rc.bottom - 1;
            tri[1].x = rc.left + t;   tri[1].y = rc.bottom - 1;
            tri[2].x = rc.left;       tri[2].y = rc.bottom - 1 - t;
            Polygon(hdc, tri, 3);

            SelectObject(hdc, old_silver_pen);
            DeleteObject(silver_pen);
        }

        SelectObject(hdc, old_brush);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

static HWND add_panel(HWND parent, int x, int y, int w, int h) {
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT, x, y, w, h, 0);
    if (ctrl) {
        if (!g_panel_orig_proc) {
            g_panel_orig_proc = (WNDPROC)GetWindowLongPtrA(ctrl, GWLP_WNDPROC);
        }
        SetWindowLongPtrA(ctrl, GWLP_WNDPROC, (LONG_PTR)panel_subclass_proc);
    }
    return ctrl;
}

static HWND add_header(HWND parent, LPCSTR text, int x, int y, int w, int h) {
    HWND ctrl = add_ctrl(parent, "STATIC", text, SS_LEFT, x, y, w, h, 0);
    if (ctrl && g_header_font) {
        SendMessageA(ctrl, WM_SETFONT, (WPARAM)g_header_font, (LPARAM)TRUE);
    }
    return ctrl;
}

/* App-title header bar's title text - same idea as add_header() but with
 * the larger g_title_font. */
static HWND add_title(HWND parent, LPCSTR text, int x, int y, int w, int h) {
    HWND ctrl = add_ctrl(parent, "STATIC", text, SS_LEFT, x, y, w, h, 0);
    if (ctrl && g_title_font) {
        SendMessageA(ctrl, WM_SETFONT, (WPARAM)g_title_font, (LPARAM)TRUE);
    }
    return ctrl;
}

/* Small thin-line icons next to each panel title - same set as the
 * single-channel app (plain GDI lines/rectangles, no arcs). ICON_WAVE is
 * used for each channel card (RF signal), ICON_PLUG for the connection
 * panel. */
#define ICON_PLUG 0
#define ICON_WAVE 2
#define ICON_LIST 5

static void draw_header_icon(HDC hdc, int x, int y, int type) {
    switch (type) {
        case ICON_PLUG:
            Rectangle(hdc, x + 3, y + 6, x + 11, y + 13);
            MoveToEx(hdc, x + 5, y + 6, NULL); LineTo(hdc, x + 5, y + 2);
            MoveToEx(hdc, x + 9, y + 6, NULL); LineTo(hdc, x + 9, y + 2);
            break;
        case ICON_WAVE: {
            POINT pts[6];
            pts[0].x = x + 1;  pts[0].y = y + 7;
            pts[1].x = x + 4;  pts[1].y = y + 2;
            pts[2].x = x + 7;  pts[2].y = y + 12;
            pts[3].x = x + 10; pts[3].y = y + 2;
            pts[4].x = x + 13; pts[4].y = y + 12;
            pts[5].x = x + 13; pts[5].y = y + 7;
            Polyline(hdc, pts, 5);
            break;
        }
        case ICON_LIST:
            MoveToEx(hdc, x + 2, y + 3, NULL);  LineTo(hdc, x + 12, y + 3);
            MoveToEx(hdc, x + 2, y + 7, NULL);  LineTo(hdc, x + 12, y + 7);
            MoveToEx(hdc, x + 2, y + 11, NULL); LineTo(hdc, x + 12, y + 11);
            break;
        default:
            break;
    }
}

static LRESULT CALLBACK icon_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        HPEN pen, old_pen;
        HBRUSH old_brush;
        int type;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_brush_panel);

        type = (int)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        pen = CreatePen(PS_SOLID, 1, COLOR_APP_HEADER);
        old_pen = (HPEN)SelectObject(hdc, pen);
        old_brush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

        draw_header_icon(hdc, rc.left, rc.top, type);

        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

static HWND add_header_icon(HWND parent, int x, int y, int type) {
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT, x, y, 14, 14, 0);
    if (ctrl) {
        if (!g_panel_orig_proc) {
            g_panel_orig_proc = (WNDPROC)GetWindowLongPtrA(ctrl, GWLP_WNDPROC);
        }
        SetWindowLongPtrA(ctrl, GWLP_USERDATA, (LONG_PTR)type);
        SetWindowLongPtrA(ctrl, GWLP_WNDPROC, (LONG_PTR)icon_subclass_proc);
    }
    return ctrl;
}

/* Builds an 8x8 (DOT_GRID_SPACING^2) tile bitmap with one dot in it and
 * wraps it as a pattern brush - GDI tiles a pattern brush automatically
 * on fill, so painting the whole background is ONE FillRect call
 * instead of a manual loop calling FillRect once per dot. On a
 * maximized real monitor (e.g. 1920x1080) the old loop was 30,000+
 * individual GDI calls on every single background erase (every resize,
 * every alt-tab back, every restore from minimized) - by far the
 * biggest cost in the whole app, nothing else came close. */
static void build_dot_pattern_brush(void) {
    HDC screen_dc, mem_dc;
    HBITMAP old_bmp;
    RECT tile, dot;

    screen_dc = GetDC(NULL);
    mem_dc = CreateCompatibleDC(screen_dc);
    g_dot_pattern_bmp = CreateCompatibleBitmap(screen_dc, DOT_GRID_SPACING, DOT_GRID_SPACING);
    ReleaseDC(NULL, screen_dc);

    old_bmp = (HBITMAP)SelectObject(mem_dc, g_dot_pattern_bmp);

    tile.left = 0;
    tile.top = 0;
    tile.right = DOT_GRID_SPACING;
    tile.bottom = DOT_GRID_SPACING;
    FillRect(mem_dc, &tile, g_brush_page);

    dot.left = DOT_GRID_SPACING / 2;
    dot.top = DOT_GRID_SPACING / 2;
    dot.right = dot.left + DOT_GRID_SIZE;
    dot.bottom = dot.top + DOT_GRID_SIZE;
    FillRect(mem_dc, &dot, g_brush_dot);

    SelectObject(mem_dc, old_bmp);
    DeleteDC(mem_dc);

    g_brush_dot_pattern = CreatePatternBrush(g_dot_pattern_bmp);
}

/* ---- temperature gauge ----
 * A horizontal gradient scale (not a fill-to-value bar): the full track
 * always shows the whole 0-GAUGE_MAX_C color range, white -> green ->
 * blue -> orange -> red, and a thin marker line shows where the current
 * reading sits on it. Matches the confirmed bands: 0-19 white, 20-39
 * green, 40-55 blue, 56-65 orange, 66+ red - the gradient stops sit at
 * each band's midpoint so the color sweep reads smoothly rather than in
 * hard steps. */
#define GAUGE_MAX_C 80.0f
#define GAUGE_STOP_COUNT 5

static const float GAUGE_STOP_TEMPS[GAUGE_STOP_COUNT] = { 0.0f, 20.0f, 48.0f, 61.0f, 80.0f };

static COLORREF gauge_stop_color(int i) {
    switch (i) {
        case 0: return RGB(255, 255, 255); /* white - freezing */
        case 1: return COLOR_APP_CONNECTED; /* green - low */
        case 2: return RGB(58, 133, 224);   /* blue */
        case 3: return RGB(224, 146, 34);   /* orange */
        default: return COLOR_APP_DISCONNECTED; /* red - hot */
    }
}

/* Which of the 5 confirmed bands a reading falls in - used to color the
 * position marker and the numeric readout beside the gauge, matching the
 * gradient it sits on. */
static COLORREF temp_band_color(float temp_c) {
    if (temp_c < 20.0f) return RGB(255, 255, 255);
    if (temp_c < 40.0f) return COLOR_APP_CONNECTED;
    if (temp_c < 56.0f) return RGB(58, 133, 224);
    if (temp_c < 66.0f) return RGB(224, 146, 34);
    return COLOR_APP_DISCONNECTED;
}

/* Shared by both gauges (temperature: horizontal, per-channel level:
 * vertical) - GradientFill only interpolates between 2 colors per call,
 * so a multi-stop sweep is just several of these back to back. */
static void gradient_fill_rect(HDC hdc, RECT r, COLORREF c0, COLORREF c1, bool vertical) {
    TRIVERTEX v[2];
    GRADIENT_RECT gr;

    if (r.right <= r.left || r.bottom <= r.top) {
        return;
    }

    v[0].x = r.left;  v[0].y = r.top;
    v[0].Red   = (COLOR16)(GetRValue(c0) << 8);
    v[0].Green = (COLOR16)(GetGValue(c0) << 8);
    v[0].Blue  = (COLOR16)(GetBValue(c0) << 8);
    v[0].Alpha = 0;

    v[1].x = r.right; v[1].y = r.bottom;
    v[1].Red   = (COLOR16)(GetRValue(c1) << 8);
    v[1].Green = (COLOR16)(GetGValue(c1) << 8);
    v[1].Blue  = (COLOR16)(GetBValue(c1) << 8);
    v[1].Alpha = 0;

    gr.UpperLeft = 0;
    gr.LowerRight = 1;
    GradientFill(hdc, v, 2, &gr, 1, vertical ? GRADIENT_FILL_RECT_V : GRADIENT_FILL_RECT_H);
}

static LRESULT CALLBACK gauge_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        const SensorState *st;
        int i;
        int w;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        w = rc.right - rc.left;

        /* Gradient stops mapped from temperature-space to pixel-space,
         * drawn as GAUGE_STOP_COUNT-1 back-to-back two-color segments -
         * GradientFill only interpolates between 2 colors per call, so a
         * multi-color sweep is just several of those in a row. */
        for (i = 0; i + 1 < GAUGE_STOP_COUNT; i++) {
            RECT seg = rc;
            seg.left = rc.left + (int)(GAUGE_STOP_TEMPS[i] / GAUGE_MAX_C * w);
            seg.right = rc.left + (int)(GAUGE_STOP_TEMPS[i + 1] / GAUGE_MAX_C * w);
            gradient_fill_rect(hdc, seg, gauge_stop_color(i), gauge_stop_color(i + 1), false);
        }

        st = sensor_get_state(&g_sensor, 0);
        if (st->has_reading) {
            float t = st->temperature_c;
            int marker_x;
            HPEN pen, old_pen;

            if (t < 0.0f) t = 0.0f;
            if (t > GAUGE_MAX_C) t = GAUGE_MAX_C;
            marker_x = rc.left + (int)(t / GAUGE_MAX_C * w);

            pen = CreatePen(PS_SOLID, 2, RGB(20, 20, 22));
            old_pen = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, marker_x, rc.top, NULL);
            LineTo(hdc, marker_x, rc.bottom);
            SelectObject(hdc, old_pen);
            DeleteObject(pen);
        }

        {
            HPEN pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
            HPEN old_pen = (HPEN)SelectObject(hdc, pen);
            HBRUSH old_brush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(hdc, old_brush);
            SelectObject(hdc, old_pen);
            DeleteObject(pen);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

static HWND add_gauge(HWND parent, int x, int y, int w, int h, int id) {
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT, x, y, w, h, id);
    if (ctrl) {
        if (!g_panel_orig_proc) {
            g_panel_orig_proc = (WNDPROC)GetWindowLongPtrA(ctrl, GWLP_WNDPROC);
        }
        SetWindowLongPtrA(ctrl, GWLP_WNDPROC, (LONG_PTR)gauge_subclass_proc);
    }
    return ctrl;
}

/* ---- activity log ---- */

static void log_add(const char *message) {
    HWND list = GetDlgItem(g_hwnd, IDC_LOG_LISTBOX);
    SYSTEMTIME st;
    char line[160];
    int count;

    GetLocalTime(&st);
    wsprintfA(line, "[%02d:%02d:%02d] %s", st.wHour, st.wMinute, st.wSecond, message);
    SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)line);

    count = (int)SendMessageA(list, LB_GETCOUNT, 0, 0);
    while (count > LOG_MAX_ENTRIES) {
        SendMessageA(list, LB_DELETESTRING, 0, 0);
        count--;
    }
    SendMessageA(list, LB_SETTOPINDEX, (WPARAM)(count > 0 ? count - 1 : 0), 0);
}

/* Errors used to pop into a separate ephemeral warning box; now they
 * just land in the same activity log everything else does (matching
 * sdr_app/sdr_react, which don't have a separate transient warning
 * panel either), marked with a "!" prefix so they stand out. */
static void ui_show_warning(const char *message) {
    char line[160];
    wsprintfA(line, "! %s", message);
    log_add(line);
}

/* ---- connection -> UI callbacks ---- */

static void conn_on_connected_changed(bool connected, void *ctx) {
    (void)ctx;
    SetDlgItemTextA(g_hwnd, IDC_CONN_STATUS_LBL, connected ? "Connected" : "Disconnected");
    EnableWindow(GetDlgItem(g_hwnd, IDC_CONNECT_BTN), TRUE);
    SetWindowTextA(GetDlgItem(g_hwnd, IDC_CONNECT_BTN), connected ? "Disconnect" : "Connect");
    InvalidateRect(GetDlgItem(g_hwnd, IDC_CONN_STATUS_LBL), NULL, TRUE);
}

static void conn_on_frame(const ProtoParsedFrame *frame, void *ctx) {
    (void)ctx;
    (void)frame;
    /* Blind-send doesn't act on responses - channels_poll() applies
     * state optimistically on its own settle timer, regardless of
     * whatever comes back on the wire. */
}

static void conn_on_error(const char *message, void *ctx) {
    (void)ctx;
    ui_show_warning(message);
}

/* ---- port list / connect ---- */

static void refresh_combo_ports(HWND combo, int (*list_fn)(char[][16], int)) {
    char names[32][16];
    char prev[16];
    int n, i;
    LRESULT idx;

    /* Keep whatever port was selected (loaded from .ini, or picked by
     * hand) across a refresh instead of always jumping back to index 0 -
     * a plugged-in device shouldn't silently switch to a different port
     * just because the list got rebuilt. Falls back to index 0 only when
     * that port is no longer in the refreshed list (unplugged). */
    if (GetWindowTextA(combo, prev, sizeof(prev)) == 0) {
        prev[0] = '\0';
    }

    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    n = list_fn(names, 32);
    for (i = 0; i < n; i++) {
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)names[i]);
    }
    if (n == 0) {
        return;
    }

    idx = (prev[0] != '\0') ? SendMessageA(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)prev) : CB_ERR;
    SendMessageA(combo, CB_SETCURSEL, (idx != CB_ERR) ? (WPARAM)idx : 0, 0);
}

static void refresh_port_list(void) {
    refresh_combo_ports(GetDlgItem(g_hwnd, IDC_PORT_COMBO), conn_list_ports);
}

static void on_connect_clicked(void) {
    char port[16];
    int baud_idx, databits_idx;
    int baud, databits;
    char parity;

    if (conn_is_connected(&g_conn)) {
        conn_disconnect(&g_conn);
        return;
    }

    if (GetDlgItemTextA(g_hwnd, IDC_PORT_COMBO, port, sizeof(port)) == 0) {
        MessageBoxA(g_hwnd, "Select a port first", "No port", MB_OK | MB_ICONWARNING);
        return;
    }

    baud_idx = (int)SendDlgItemMessageA(g_hwnd, IDC_BAUD_COMBO, CB_GETCURSEL, 0, 0);
    baud = BAUD_OPTIONS[baud_idx];
    databits_idx = (int)SendDlgItemMessageA(g_hwnd, IDC_DATABITS_COMBO, CB_GETCURSEL, 0, 0);
    databits = DATABITS_OPTIONS[databits_idx];
    parity = PARITY_CODES[SendDlgItemMessageA(g_hwnd, IDC_PARITY_COMBO, CB_GETCURSEL, 0, 0)];

    conn_connect(&g_conn, port, (DWORD)baud, parity, (uint8_t)databits);
}

/* ---- temp/humidity sensor UI ----
 * Separate Port/Connect controls from the RS-422 side above - this is a
 * second, independent serial connection. Baud/parity/data bits aren't
 * user-editable here: they're fixed at the values confirmed against the
 * real XY-MD02 sensor (9600 8N1), so there's nothing to expose that would
 * ever need changing - fewer knobs, matching the "user friendly" ask. */
#define SENSOR_BAUD 9600
#define SENSOR_PARITY 'N'
#define SENSOR_DATABITS 8

static void refresh_sensor_port_list(void) {
    refresh_combo_ports(GetDlgItem(g_hwnd, IDC_SENSOR_PORT_COMBO), serial_list_ports);
}

static void on_sensor_connect_clicked(void) {
    char port[16];

    if (sensor_is_connected(&g_sensor)) {
        sensor_disconnect(&g_sensor);
        return;
    }

    if (GetDlgItemTextA(g_hwnd, IDC_SENSOR_PORT_COMBO, port, sizeof(port)) == 0) {
        MessageBoxA(g_hwnd, "Select a port first", "No port", MB_OK | MB_ICONWARNING);
        return;
    }

    if (!sensor_connect(&g_sensor, port, SENSOR_BAUD, SENSOR_PARITY, SENSOR_DATABITS)) {
        char msg[128];
        wsprintfA(msg, "Failed to open %s", port);
        ui_show_warning(msg);
    }
}

/* Cached last-shown values so the 100ms poll tick only touches the
 * controls when something actually changed - the multi-channel cards
 * originally force-redrew unconditionally every tick and that caused
 * visible flicker; this panel is built with the fix in from the start. */
static bool g_sensor_ui_valid;
static bool g_sensor_ui_connected;
static bool g_sensor_ui_online;
static bool g_sensor_ui_has_reading;
static float g_sensor_ui_temp;
static float g_sensor_ui_humidity;
static int g_sensor_ui_attempt_count;
static uint16_t g_sensor_ui_last_rx_len;

static bool g_sensor_connect_btn_valid;
static bool g_sensor_connect_btn_connected;

static void ui_refresh_sensor(void) {
    /* Sidebar always shows unit 0's reading - meaningful in scan mode
     * (the one shared reading, mirrored into every unit anyway) and
     * hidden entirely in per-unit mode (see ui_refresh_sensor_mode()) in
     * favor of each card showing its own. The Connect button itself is
     * mode-independent (same physical port either way) and has its own
     * small change-detection gate below, separate from the mode check. */
    const SensorState *st = sensor_get_state(&g_sensor, 0);
    bool connected = sensor_is_connected(&g_sensor);
    char text[64];

    if (!g_sensor_connect_btn_valid || g_sensor_connect_btn_connected != connected) {
        EnableWindow(GetDlgItem(g_hwnd, IDC_SENSOR_CONNECT_BTN), TRUE);
        SetWindowTextA(GetDlgItem(g_hwnd, IDC_SENSOR_CONNECT_BTN), connected ? "Disconnect" : "Connect");
        g_sensor_connect_btn_valid = true;
        g_sensor_connect_btn_connected = connected;
    }

    if (sensor_get_mode(&g_sensor) != SENSOR_MODE_SCAN) {
        return;
    }

    if (g_sensor_ui_valid && g_sensor_ui_connected == connected &&
        g_sensor_ui_online == st->online && g_sensor_ui_has_reading == st->has_reading &&
        g_sensor_ui_temp == st->temperature_c && g_sensor_ui_humidity == st->humidity_pct &&
        g_sensor_ui_attempt_count == st->attempt_count && g_sensor_ui_last_rx_len == st->last_rx_len) {
        return; /* nothing shown by this panel has changed */
    }

    if (!connected) {
        lstrcpynA(text, "Disconnected", (int)sizeof(text));
    } else if (st->online) {
        lstrcpynA(text, "Online", (int)sizeof(text));
    } else if (st->has_reading) {
        wsprintfA(text, "Not responding (try %d, last %d B)", st->attempt_count, st->last_rx_len);
    } else {
        /* Diagnostic counts shown even on the very first attempt, so a
         * stuck "Reading..." is debuggable without extra tools: 0 bytes
         * back after several tries means nothing is answering at all
         * (wiring/adapter/settings), while >0 bytes means something
         * replied but didn't parse as a valid Modbus frame. */
        wsprintfA(text, "Reading... (try %d, last %d B)", st->attempt_count, st->last_rx_len);
    }
    SetDlgItemTextA(g_hwnd, IDC_SENSOR_STATUS_LBL, text);
    InvalidateRect(GetDlgItem(g_hwnd, IDC_SENSOR_STATUS_LBL), NULL, FALSE);

    if (st->has_reading) {
        wsprintfA(text, "%d.%d C",
                  (int)st->temperature_c, (int)(st->temperature_c * 10) % 10);
    } else {
        lstrcpynA(text, "-", (int)sizeof(text));
    }
    SetDlgItemTextA(g_hwnd, IDC_SENSOR_TEMP_LBL, text);
    InvalidateRect(GetDlgItem(g_hwnd, IDC_SENSOR_TEMP_LBL), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, IDC_SENSOR_TEMP_GAUGE), NULL, FALSE);

    if (st->has_reading) {
        wsprintfA(text, "Humidity: %d.%d %%",
                  (int)st->humidity_pct, (int)(st->humidity_pct * 10) % 10);
    } else {
        lstrcpynA(text, "Humidity: -", (int)sizeof(text));
    }
    SetDlgItemTextA(g_hwnd, IDC_SENSOR_HUMIDITY_LBL, text);

    g_sensor_ui_valid = true;
    g_sensor_ui_connected = connected;
    g_sensor_ui_online = st->online;
    g_sensor_ui_has_reading = st->has_reading;
    g_sensor_ui_attempt_count = st->attempt_count;
    g_sensor_ui_last_rx_len = st->last_rx_len;
    g_sensor_ui_temp = st->temperature_c;
    g_sensor_ui_humidity = st->humidity_pct;
}

/* Switches which sidebar controls are visible for the current sensor
 * mode - the single-reading status/gauge/temp/humidity block only means
 * anything in scan mode (one shared reading); per-unit mode shows a
 * short note instead and lets each card speak for itself. Called once
 * right after a mode change (not every tick - the visible set doesn't
 * change again until the user picks a different mode). */
static void ui_refresh_sensor_mode(void) {
    bool scan = sensor_get_mode(&g_sensor) == SENSOR_MODE_SCAN;
    int i;

    ShowWindow(GetDlgItem(g_hwnd, IDC_SENSOR_STATUS_LBL), scan ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hwnd, IDC_SENSOR_TEMP_GAUGE), scan ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hwnd, IDC_SENSOR_TEMP_LBL), scan ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hwnd, IDC_SENSOR_HUMIDITY_LBL), scan ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hwnd, IDC_SENSOR_MODE_NOTE_LBL), scan ? SW_HIDE : SW_SHOW);

    InvalidateRect(GetDlgItem(g_hwnd, IDC_SENSOR_MODE_SCAN_BTN), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, IDC_SENSOR_MODE_UNIT_BTN), NULL, FALSE);

    /* Force the sidebar's own cache to re-evaluate on the next tick
     * (harmless when re-entering scan mode with an unchanged reading -
     * it'll just re-set the same text) and every card's temp readout to
     * repaint with its now-current (freshly cleared by
     * sensor_set_mode()) value instead of whatever was left over from
     * the previous mode. */
    g_sensor_ui_valid = false;
    for (i = 0; i < MAX_CHANNELS; i++) {
        InvalidateRect(GetDlgItem(g_hwnd, IDC_CH_BASE + i * IDC_CH_STRIDE + IDC_CH_TEMP_LBL_OFFSET), NULL, FALSE);
    }
}

/* ---- kill switch ----
 * One trip flag per unit (see KILL_SWITCH_THRESHOLD_C's comment for why
 * per-unit): scan mode's mirrored reading naturally trips every unit in
 * the same tick since they all cross the threshold together, so no
 * separate "global" code path is needed - this loop handles both modes
 * uniformly. */

static bool g_kill_ui_valid;
static int g_kill_ui_tripped_count;

static int count_kill_switch_tripped(void) {
    int i, n = 0;
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (g_kill_switch_tripped[i]) n++;
    }
    return n;
}

static void ui_refresh_kill_switch(void) {
    int tripped_count = count_kill_switch_tripped();
    bool any_tripped = tripped_count > 0;

    /* Compare the actual count, not just "any vs none" - going from say
     * 8 tripped to 7 stays "some tripped" either way, but the displayed
     * count still needs to move. */
    if (g_kill_ui_valid && g_kill_ui_tripped_count == tripped_count) {
        return;
    }
    if (any_tripped) {
        char text[48];
        if (tripped_count >= MAX_CHANNELS) {
            lstrcpynA(text, "KILL SWITCH TRIPPED - all units", (int)sizeof(text));
        } else {
            wsprintfA(text, "KILL SWITCH TRIPPED - %d unit%s", tripped_count, tripped_count == 1 ? "" : "s");
        }
        SetDlgItemTextA(g_hwnd, IDC_KILL_STATUS_LBL, text);
        ShowWindow(GetDlgItem(g_hwnd, IDC_KILL_STATUS_LBL), SW_SHOW);
        ShowWindow(GetDlgItem(g_hwnd, IDC_KILL_RESET_BTN), SW_SHOW);
    } else {
        ShowWindow(GetDlgItem(g_hwnd, IDC_KILL_STATUS_LBL), SW_HIDE);
        ShowWindow(GetDlgItem(g_hwnd, IDC_KILL_RESET_BTN), SW_HIDE);
    }
    g_kill_ui_valid = true;
    g_kill_ui_tripped_count = tripped_count;
}

/* Manual reset only, by design - see the KILL_SWITCH_THRESHOLD_C comment
 * up top for why. Call once per timer tick; no-ops per-unit once that
 * unit is already tripped or its temperature is unknown/below threshold. */
static void check_kill_switch(void) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        const SensorState *st;
        char msg[96];

        if (g_kill_switch_tripped[i]) {
            continue;
        }
        st = sensor_get_state(&g_sensor, i);
        if (!st->has_reading || st->temperature_c < KILL_SWITCH_THRESHOLD_C) {
            continue;
        }

        g_kill_switch_tripped[i] = true;
        channel_turn_output_off(i);
        wsprintfA(msg, "Unit %d: KILL SWITCH TRIPPED (%d.%d C >= %d C) - forced OFF", i + 1,
                  (int)st->temperature_c, (int)(st->temperature_c * 10) % 10, (int)KILL_SWITCH_THRESHOLD_C);
        log_add(msg);
    }
}

/* Sidebar's Reset button - resets every currently-tripped unit at once.
 * In scan mode that's normally all 16 (they trip together); in per-unit
 * mode it's whichever ones happen to be over - a convenient "reset
 * everything" alongside each card's own single-unit reset (see
 * on_unit_kill_reset()). */
static void on_kill_reset_clicked(void) {
    int i;
    bool any = false;
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (g_kill_switch_tripped[i]) {
            g_kill_switch_tripped[i] = false;
            any = true;
        }
    }
    if (any) {
        log_add("Kill switch reset by user (all units)");
    }
}

/* Per-unit reset - resets just this one unit, independent of the others.
 * Wired to a click on that unit's card temperature readout while it's
 * tripped (see IDC_CH_TEMP_LBL_OFFSET's comment in resource.h). */
static void on_unit_kill_reset(int idx) {
    char msg[32];
    if (!g_kill_switch_tripped[idx]) {
        return;
    }
    g_kill_switch_tripped[idx] = false;
    wsprintfA(msg, "Unit %d: kill switch reset by user", idx + 1);
    log_add(msg);
}

/* ---- channel card UI ---- */

static int channel_mode_id(int idx)       { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_MODE_OFFSET; }
static int channel_set_id(int idx)        { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_SET_OFFSET; }
static int channel_on_id(int idx)         { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_ON_OFFSET; }
static int channel_off_id(int idx)        { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_OFF_OFFSET; }
static int channel_status_id(int idx)     { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_STATUS_OFFSET; }
static int channel_track_id(int idx)      { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_TRACKBAR_OFFSET; }
static int channel_lbl_high_id(int idx)   { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_LBL_HIGH_OFFSET; }
static int channel_lbl_medium_id(int idx) { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_LBL_MEDIUM_OFFSET; }
static int channel_lbl_low_id(int idx)    { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_LBL_LOW_OFFSET; }
static int channel_lbl_off_id(int idx)    { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_LBL_OFF_OFFSET; }
static int channel_temp_id(int idx)       { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_TEMP_LBL_OFFSET; }

/* Maps a control ID back to its channel index, for any control that
 * belongs to a channel card. Returns false for IDs outside that range. */
static bool channel_index_from_id(int id, int *out_idx) {
    int rel, idx;
    if (id < IDC_CH_BASE) {
        return false;
    }
    rel = id - IDC_CH_BASE;
    idx = rel / IDC_CH_STRIDE;
    if (idx < 0 || idx >= MAX_CHANNELS) {
        return false;
    }
    *out_idx = idx;
    return true;
}

/* ---- per-channel level gradient gauge ----
 * Replaces the native trackbar with a custom-drawn vertical gradient
 * (muted gray -> green -> orange -> red, Off at bottom to High at top,
 * matching the High/Medium/Low/Off labels beside it) and a real slider
 * thumb - a rounded pill overhanging both edges with a soft drop
 * shadow, not a plain line that disappears into the gradient's own
 * dark stops, and not divider lines cutting the gradient into blocks
 * (tried that - looked blocky/cheap once the gauge got bigger). Click/
 * drag anywhere on it to set the level directly instead of dragging
 * the thumb by hand. */
static COLORREF ch_gauge_stop_color(int level) {
    switch (level) {
        case LEVEL_OFF:    return COLOR_APP_MUTED;
        case LEVEL_LOW:    return COLOR_APP_CONNECTED;
        case LEVEL_MEDIUM: return RGB(224, 146, 34);
        default:           return COLOR_APP_DISCONNECTED; /* LEVEL_HIGH */
    }
}

/* Maps a Y coordinate inside the gauge to a level (0-3), top=High,
 * bottom=Off - shared by painting the marker and by click/drag input. */
static int ch_gauge_level_from_y(int y, int h) {
    int band = (h > 0) ? (y * 4 / h) : 0;
    if (band < 0) band = 0;
    if (band > 3) band = 3;
    return 3 - band;
}

static void ch_gauge_apply_click(HWND hwnd, int y) {
    RECT rc;
    int idx, level;
    const ChannelState *ch;

    if (!channel_index_from_id(GetDlgCtrlID(hwnd), &idx)) {
        return;
    }
    ch = channels_get(idx);
    GetClientRect(hwnd, &rc);
    level = ch_gauge_level_from_y(y, rc.bottom - rc.top);

    /* The gauge only adjusts an already-running channel's level - it
     * won't power one on by itself. That's the ON button's job, same
     * as the reference app: level is something you dial in once
     * output is already active, not a way to sneak around ON/OFF. */
    if (level != LEVEL_OFF && !ch->output_on) {
        return;
    }

    /* Off is always allowed even kill-switch-tripped - same reasoning
     * as the ON/OFF buttons and mode Set. */
    if (level == LEVEL_OFF || !g_kill_switch_tripped[idx]) {
        channel_set_level(idx, level);
    }
}

static LRESULT CALLBACK channel_gauge_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        int idx;
        int h;
        int w;
        int i;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        h = rc.bottom - rc.top;
        w = rc.right - rc.left;

        /* Real volume-slider look: a narrow rounded track down the
         * middle (not the full control width - a full-width bar with a
         * thumb stuck on top never reads as "a slider", it reads as a
         * bar chart) with a round handle riding on it, instead of a
         * wide gradient block with a marker line/pill on top of it. */
        {
            int track_w = w / 4;
            int track_cx = rc.left + w / 2;
            RECT track;
            HRGN clip;
            int band, center_y, handle_d, handle_r;
            HBRUSH bg_brush, handle_brush, shadow_brush;
            HPEN track_pen, old_pen;
            HBRUSH old_brush;

            if (track_w < 6) track_w = 6;
            track.left = track_cx - track_w / 2;
            track.right = track.left + track_w;
            track.top = rc.top;
            track.bottom = rc.bottom;

            bg_brush = CreateSolidBrush(COLOR_APP_PANEL_BG);
            FillRect(hdc, &rc, bg_brush);
            DeleteObject(bg_brush);

            /* Clip the gradient to the rounded track shape so its
             * corners aren't square. */
            clip = CreateRoundRectRgn(track.left, track.top, track.right + 1, track.bottom + 1,
                                       track_w, track_w);
            SelectClipRgn(hdc, clip);
            for (i = 0; i < 3; i++) {
                RECT seg = track;
                seg.top = track.top + h * i / 3;
                seg.bottom = track.top + h * (i + 1) / 3;
                /* RECT.top is the smaller y (visually higher), so i=0
                 * is the TOP third on screen - paint it from
                 * stop(3)=HIGH/red downward, matching the labels'
                 * top-to-bottom order. Stop colors indexed 0=OFF..3=HIGH. */
                gradient_fill_rect(hdc, seg, ch_gauge_stop_color(3 - i), ch_gauge_stop_color(2 - i), true);
            }
            SelectClipRgn(hdc, NULL);
            DeleteObject(clip);

            track_pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
            old_pen = (HPEN)SelectObject(hdc, track_pen);
            SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, track.left, track.top, track.right, track.bottom, track_w, track_w);
            SelectObject(hdc, old_pen);
            DeleteObject(track_pen);

            band = -1; /* nothing selected -> handle stays hidden */
            if (channel_index_from_id(GetDlgCtrlID(hwnd), &idx)) {
                band = 3 - channels_get(idx)->level; /* 0=top/HIGH .. 3=bottom/OFF */
            }
            if (band < 0) {
                EndPaint(hwnd, &ps);
                return 0;
            }
            center_y = rc.top + (2 * band + 1) * h / 8;
            /* Small, proportionate to the track (not almost the full
             * card width) - a big handle on a thin track read as
             * heavy/clunky rather than clean. */
            handle_d = track_w + 10;
            handle_r = handle_d / 2;

            /* One soft 1px shadow, not a heavy offset black blob. */
            shadow_brush = CreateSolidBrush(COLOR_APP_DOT);
            old_brush = (HBRUSH)SelectObject(hdc, shadow_brush);
            SelectObject(hdc, GetStockObject(NULL_PEN));
            Ellipse(hdc, track_cx - handle_r, center_y - handle_r + 1, track_cx + handle_r, center_y + handle_r + 1);
            SelectObject(hdc, old_brush);
            DeleteObject(shadow_brush);

            /* Flat white fill, no border - crisper than an outlined
             * circle at this size. */
            handle_brush = CreateSolidBrush(COLOR_APP_TEXT);
            old_brush = (HBRUSH)SelectObject(hdc, handle_brush);
            old_pen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
            Ellipse(hdc, track_cx - handle_r, center_y - handle_r, track_cx + handle_r, center_y + handle_r);
            SelectObject(hdc, old_brush);
            SelectObject(hdc, old_pen);
            DeleteObject(handle_brush);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        SetCapture(hwnd);
        ch_gauge_apply_click(hwnd, (short)HIWORD(lParam));
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        if (GetCapture() == hwnd) {
            ch_gauge_apply_click(hwnd, (short)HIWORD(lParam));
        }
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

static HWND add_channel_gauge(HWND parent, int x, int y, int w, int h, int id) {
    /* SS_NOTIFY, not just SS_LEFT: a plain static's default WM_NCHITTEST
     * returns HTTRANSPARENT, so clicks fall through to the parent window
     * instead of reaching this control's subclass proc. */
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT | SS_NOTIFY, x, y, w, h, id);
    if (ctrl) {
        if (!g_panel_orig_proc) {
            g_panel_orig_proc = (WNDPROC)GetWindowLongPtrA(ctrl, GWLP_WNDPROC);
        }
        SetWindowLongPtrA(ctrl, GWLP_WNDPROC, (LONG_PTR)channel_gauge_subclass_proc);
    }
    return ctrl;
}

/* Card layout: a left column (Mode combo + Set button, ON/OFF power
 * buttons, status line), then a full-width horizontal level trackbar
 * with Off/Low/Medium/High labels underneath it. */
static void add_channel_card(HWND hwnd, int index) {
    int col = index % GRID_COLS;
    int row = index / GRID_COLS;
    int x = GRID_LEFT + col * (CARD_W + CARD_GAP);
    int y = GRID_TOP + row * (CARD_H + CARD_GAP);
    char header[16];
    int i;
    HWND mode_combo;

    g_card_panel[index] = add_panel(hwnd, x, y, CARD_W, CARD_H);
    g_card_icon[index] = add_header_icon(hwnd, x + 8, y + 6, ICON_WAVE);
    wsprintfA(header, "Unit %d", index + 1);
    g_card_header[index] = add_header(hwnd, header, x + 26, y + 6, 200, 16);

    /* Left column - compressed a bit (was y+26/52/78 with 22px-tall
     * buttons) to make clean room for the Bandwidth/Temp row below it,
     * rather than just relying on the slack the taller gauge column
     * already left underneath. */
    mode_combo = add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                           x + 8, y + 24, 96, 120, channel_mode_id(index));
    for (i = 0; i < PROTO_MODE_COUNT; i++) {
        const char *name = proto_mode_name((uint8_t)i);
        SendMessageA(mode_combo, CB_ADDSTRING, 0, (LPARAM)(name ? name : "?"));
    }
    SendMessageA(mode_combo, CB_SETCURSEL, PROTO_MODE_WHITE_NOISE, 0);
    SendMessageA(mode_combo, CB_SETDROPPEDWIDTH, 190, 0);

    add_ctrl(hwnd, "BUTTON", "Set", BS_OWNERDRAW | WS_TABSTOP,
             x + 108, y + 24, 46, 20, channel_set_id(index));

    add_ctrl(hwnd, "BUTTON", "ON", BS_OWNERDRAW | WS_TABSTOP,
             x + 8, y + 46, 71, 20, channel_on_id(index));
    add_ctrl(hwnd, "BUTTON", "OFF", BS_OWNERDRAW | WS_TABSTOP,
             x + 83, y + 46, 71, 20, channel_off_id(index));

    add_ctrl(hwnd, "STATIC", "STANDBY", SS_LEFT | SS_NOPREFIX,
             x + 8, y + 68, 146, 16, channel_status_id(index));

    /* Bottom row: this channel's (currently fixed/blind, not per-channel
     * configurable - see CHANNEL_BLIND_BANDWIDTH_MHZ in channels.h)
     * bandwidth, and this unit's own temperature - its own sensor
     * reading (mode 2 / per-unit) or the shared scan reading (mode 1) -
     * see IDC_CH_TEMP_LBL_OFFSET in resource.h for why it's also the
     * per-unit kill-switch reset. */
    /* Right column: custom gradient level gauge (Off at bottom, High at
     * top, like a volume slider) + tick labels. Trimmed from 88 to 78
     * tall (and labels re-spaced to match) to leave room below it for
     * the full-width Bandwidth/Temp row - it needs the whole card width,
     * so it has to sit below where this column ends, not beside it. */
    add_channel_gauge(hwnd, x + 164, y + 26, 26, 78, channel_track_id(index));

    add_ctrl(hwnd, "STATIC", "High",   SS_LEFT | SS_NOPREFIX, x + 194, y + 26, 44, 16, channel_lbl_high_id(index));
    add_ctrl(hwnd, "STATIC", "Medium", SS_LEFT | SS_NOPREFIX, x + 194, y + 45, 44, 16, channel_lbl_medium_id(index));
    add_ctrl(hwnd, "STATIC", "Low",    SS_LEFT | SS_NOPREFIX, x + 194, y + 64, 44, 16, channel_lbl_low_id(index));
    add_ctrl(hwnd, "STATIC", "Off",    SS_LEFT | SS_NOPREFIX, x + 194, y + 83, 44, 16, channel_lbl_off_id(index));

    /* Bottom row, full card width, below both columns: this channel's
     * (currently fixed/blind, not per-channel configurable - see
     * CHANNEL_BLIND_BANDWIDTH_MHZ in channels.h) bandwidth, and this
     * unit's own temperature - its own sensor reading (mode 2 / per-unit)
     * or the shared scan reading (mode 1) - see IDC_CH_TEMP_LBL_OFFSET in
     * resource.h for why it's also the per-unit kill-switch reset. */
    {
        char bw_text[24];
        wsprintfA(bw_text, "Bandwidth: %d", CHANNEL_BLIND_BANDWIDTH_MHZ);
        g_card_bandwidth_lbl[index] = add_ctrl(hwnd, "STATIC", bw_text, SS_LEFT | SS_NOPREFIX,
                                                x + 8, y + 108, 96, 16, 0);
    }
    add_ctrl(hwnd, "STATIC", "Temp: -", SS_LEFT | SS_NOPREFIX | SS_NOTIFY,
             x + 104, y + 108, 134, 16, channel_temp_id(index));
}

/* What was last actually painted for each channel card - lets the 10Hz
 * poll tick skip repainting anything that hasn't changed, instead of
 * force-erasing and redrawing all 6 per-channel controls every tick
 * regardless of whether their state moved. Force-erasing a plain STATIC
 * control flashes the system default background for a frame before
 * WM_CTLCOLORSTATIC repaints it correctly - at 10Hz across 16 channels
 * that reads as constant flicker/twitching. */
typedef struct {
    bool valid;
    bool busy;
    bool output_on;
    int level;
    bool has_reading;
    float temperature_c;
    bool tripped;
} ChannelUiCache;

static ChannelUiCache g_ui_cache[MAX_CHANNELS];

static void ui_refresh_channel(int index) {
    const ChannelState *ch = channels_get(index);
    const SensorState *st = sensor_get_state(&g_sensor, index);
    ChannelUiCache *cache = &g_ui_cache[index];
    HWND status_ctl;
    HWND track;
    HWND temp_ctl;
    char text[32];
    bool tripped = g_kill_switch_tripped[index];

    if (cache->valid && cache->busy == ch->busy &&
        cache->output_on == ch->output_on && cache->level == ch->level &&
        cache->has_reading == st->has_reading && cache->temperature_c == st->temperature_c &&
        cache->tripped == tripped) {
        return; /* nothing this channel's card shows has changed */
    }

    /* A send just settled (busy true -> false) - log what it applied,
     * matching sdr_app/sdr_react's TX/RX activity log. Every blind send
     * ends up "unconfirmed" by design (see channels.h), so that flag
     * isn't worth repeating on every single line here. */
    if (cache->valid && cache->busy && !ch->busy) {
        char log_line[64];
        wsprintfA(log_line, "Unit %d: %s", index + 1, ch->last_command);
        log_add(log_line);
    }

    status_ctl = GetDlgItem(g_hwnd, channel_status_id(index));
    track = GetDlgItem(g_hwnd, channel_track_id(index));
    temp_ctl = GetDlgItem(g_hwnd, channel_temp_id(index));

    /* Matches sdr_react's ChannelCard status text exactly:
     * busy -> SENDING..., on -> the level name, off -> STANDBY. */
    if (ch->busy) {
        lstrcpynA(text, "SENDING...", (int)sizeof(text));
    } else if (ch->output_on) {
        lstrcpynA(text, LEVEL_LABELS[ch->level], (int)sizeof(text));
        CharUpperA(text);
    } else {
        lstrcpynA(text, "STANDBY", (int)sizeof(text));
    }
    SetWindowTextA(status_ctl, text);
    InvalidateRect(status_ctl, NULL, FALSE);
    InvalidateRect(track, NULL, FALSE);

    if (tripped) {
        lstrcpynA(text, "Temp: TRIPPED - reset?", (int)sizeof(text));
    } else if (st->has_reading) {
        wsprintfA(text, "Temp: %d.%d C", (int)st->temperature_c, (int)(st->temperature_c * 10) % 10);
    } else {
        lstrcpynA(text, "Temp: -", (int)sizeof(text));
    }
    SetWindowTextA(temp_ctl, text);
    InvalidateRect(temp_ctl, NULL, FALSE);

    InvalidateRect(GetDlgItem(g_hwnd, channel_on_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_off_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_lbl_high_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_lbl_medium_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_lbl_low_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_lbl_off_id(index)), NULL, FALSE);

    cache->valid = true;
    cache->busy = ch->busy;
    cache->output_on = ch->output_on;
    cache->level = ch->level;
    cache->has_reading = st->has_reading;
    cache->temperature_c = st->temperature_c;
    cache->tripped = tripped;
}

static void ui_refresh_all_channels(void) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        ui_refresh_channel(i);
    }
}

/* ---- saved settings (port/baud/parity/data bits selections only - never
 * channel states or the kill switch, and never auto-connects anything.
 * A restart should never silently re-enable RF output on its own; it
 * just saves you re-picking the same COM port and baud every launch.
 * Stored next to the exe as a plain .ini, matching this app's
 * portable/no-installer approach - not AppData. ---- */

static void get_ini_path(char *path /* at least MAX_PATH + 8 bytes */) {
    char *dot;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    dot = strrchr(path, '.');
    if (dot) {
        *dot = '\0';
    }
    lstrcatA(path, ".ini");
}

static void select_combo_by_text(HWND combo, const char *text) {
    int idx = (int)SendMessageA(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)text);
    if (idx != CB_ERR) {
        SendMessageA(combo, CB_SETCURSEL, idx, 0);
    }
}

static void save_settings(void) {
    char path[MAX_PATH + 8];
    char buf[32];

    get_ini_path(path);

    GetDlgItemTextA(g_hwnd, IDC_PORT_COMBO, buf, sizeof(buf));
    WritePrivateProfileStringA("RS422", "Port", buf, path);
    GetDlgItemTextA(g_hwnd, IDC_BAUD_COMBO, buf, sizeof(buf));
    WritePrivateProfileStringA("RS422", "Baud", buf, path);
    GetDlgItemTextA(g_hwnd, IDC_DATABITS_COMBO, buf, sizeof(buf));
    WritePrivateProfileStringA("RS422", "DataBits", buf, path);
    GetDlgItemTextA(g_hwnd, IDC_PARITY_COMBO, buf, sizeof(buf));
    WritePrivateProfileStringA("RS422", "Parity", buf, path);

    GetDlgItemTextA(g_hwnd, IDC_SENSOR_PORT_COMBO, buf, sizeof(buf));
    WritePrivateProfileStringA("Sensor", "Port", buf, path);
}

/* Call after build_controls() has populated every combo's item list -
 * this only ever picks an existing item by matching text, never adds
 * one, so a saved port that's no longer plugged in just falls back to
 * whatever refresh_port_list() already defaulted to. */
static void load_settings(void) {
    char path[MAX_PATH + 8];
    char buf[32];

    get_ini_path(path);

    if (GetPrivateProfileStringA("RS422", "Port", "", buf, sizeof(buf), path) > 0) {
        select_combo_by_text(GetDlgItem(g_hwnd, IDC_PORT_COMBO), buf);
    }
    if (GetPrivateProfileStringA("RS422", "Baud", "", buf, sizeof(buf), path) > 0) {
        select_combo_by_text(GetDlgItem(g_hwnd, IDC_BAUD_COMBO), buf);
    }
    if (GetPrivateProfileStringA("RS422", "DataBits", "", buf, sizeof(buf), path) > 0) {
        select_combo_by_text(GetDlgItem(g_hwnd, IDC_DATABITS_COMBO), buf);
    }
    if (GetPrivateProfileStringA("RS422", "Parity", "", buf, sizeof(buf), path) > 0) {
        select_combo_by_text(GetDlgItem(g_hwnd, IDC_PARITY_COMBO), buf);
    }
    if (GetPrivateProfileStringA("Sensor", "Port", "", buf, sizeof(buf), path) > 0) {
        select_combo_by_text(GetDlgItem(g_hwnd, IDC_SENSOR_PORT_COMBO), buf);
    }
}

/* ---- layout ---- */

static void build_controls(HWND hwnd) {
    unsigned i;
    int idx;

    /* App-title header bar - empty except for the title for now, more
     * gets added here later. Full width, same 6px top margin and 8px
     * gap-before-content as every other panel-to-panel spacing below. */
    g_header_panel = add_panel(hwnd, SIDEBAR_X, 6, CLIENT_WIDTH - 2 * SIDEBAR_X, HEADER_H);
    g_title_ctrl = add_title(hwnd, "Digital Noise Configuration - Multi", 22, 16, CLIENT_WIDTH - 2 * SIDEBAR_X - 32, 28);

    /* Sidebar: one tall box spanning the channel grid's full height,
     * Connection & Settings / Amplifier Temperature / Activity Log
     * stacked inside it as sections (headers only, no separate borders
     * between them) instead of 3 separately-bordered panels. */
    g_sidebar_panel = add_panel(hwnd, SIDEBAR_X, CONTENT_TOP, SIDEBAR_W, GRID_ROWS * CARD_H + (GRID_ROWS - 1) * CARD_GAP);

    add_header_icon(hwnd, 22, 70, ICON_PLUG);
    add_header(hwnd, "Connection && Settings", 40, 70, 260, 18);
    add_ctrl(hwnd, "STATIC", "Port:", SS_LEFT, 22, 92, 32, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 56, 90, 112, 160, IDC_PORT_COMBO);
    add_ctrl(hwnd, "BUTTON", "Refresh", BS_OWNERDRAW | WS_TABSTOP, 174, 90, 56, 22, IDC_REFRESH_BTN);
    add_ctrl(hwnd, "BUTTON", "Connect", BS_OWNERDRAW | WS_TABSTOP, 234, 90, 66, 22, IDC_CONNECT_BTN);
    add_ctrl(hwnd, "STATIC", "Disconnected", SS_LEFT, 22, 116, 290, 16, IDC_CONN_STATUS_LBL);

    add_ctrl(hwnd, "STATIC", "Baud:", SS_LEFT, 22, 140, 34, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 58, 138, 90, 140, IDC_BAUD_COMBO);
    add_ctrl(hwnd, "STATIC", "Data Bits:", SS_LEFT, 22, 164, 60, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 86, 162, 45, 100, IDC_DATABITS_COMBO);
    add_ctrl(hwnd, "STATIC", "Parity:", SS_LEFT, 142, 164, 40, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 184, 162, 70, 100, IDC_PARITY_COMBO);

    add_header_icon(hwnd, 22, 204, ICON_WAVE);
    add_header(hwnd, "Amplifier Temperature", 40, 204, 260, 18);
    add_ctrl(hwnd, "STATIC", "Port:", SS_LEFT, 22, 226, 32, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 56, 224, 90, 160, IDC_SENSOR_PORT_COMBO);
    add_ctrl(hwnd, "BUTTON", "Refresh", BS_OWNERDRAW | WS_TABSTOP, 150, 224, 56, 22, IDC_SENSOR_REFRESH_BTN);
    add_ctrl(hwnd, "BUTTON", "Connect", BS_OWNERDRAW | WS_TABSTOP, 210, 224, 66, 22, IDC_SENSOR_CONNECT_BTN);
    /* Scan: one sensor for the whole rack (address 1), mirrored to every
     * unit's card. Per-Unit: one sensor per unit, address == unit number -
     * each card shows and protects only its own reading. */
    add_ctrl(hwnd, "STATIC", "Mode:", SS_LEFT, 22, 250, 38, 16, 0);
    add_ctrl(hwnd, "BUTTON", "Scan", BS_OWNERDRAW | WS_TABSTOP, 62, 246, 66, 22, IDC_SENSOR_MODE_SCAN_BTN);
    add_ctrl(hwnd, "BUTTON", "Per-Unit", BS_OWNERDRAW | WS_TABSTOP, 132, 246, 74, 22, IDC_SENSOR_MODE_UNIT_BTN);
    add_ctrl(hwnd, "STATIC", "Disconnected", SS_LEFT, 22, 276, 270, 16, IDC_SENSOR_STATUS_LBL);
    add_gauge(hwnd, 22, 298, 200, 20, IDC_SENSOR_TEMP_GAUGE);
    add_ctrl(hwnd, "STATIC", "-", SS_LEFT | SS_NOPREFIX, 228, 298, 72, 20, IDC_SENSOR_TEMP_LBL);
    add_ctrl(hwnd, "STATIC", "Humidity: -", SS_LEFT | SS_NOPREFIX, 22, 322, 270, 16, IDC_SENSOR_HUMIDITY_LBL);
    add_ctrl(hwnd, "STATIC", "Per-unit mode: each unit's own reading shows on its own card above.",
             SS_LEFT, 22, 276, 270, 44, IDC_SENSOR_MODE_NOTE_LBL);
    add_ctrl(hwnd, "STATIC", "", SS_LEFT | SS_NOPREFIX, 22, 346, 190, 16, IDC_KILL_STATUS_LBL);
    add_ctrl(hwnd, "BUTTON", "Reset", BS_OWNERDRAW | WS_TABSTOP, 218, 344, 80, 22, IDC_KILL_RESET_BTN);
    ShowWindow(GetDlgItem(hwnd, IDC_KILL_STATUS_LBL), SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_KILL_RESET_BTN), SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_SENSOR_MODE_NOTE_LBL), SW_HIDE); /* default mode is Scan */

    add_header_icon(hwnd, 22, 394, ICON_LIST);
    add_header(hwnd, "Activity Log", 40, 394, 200, 18);
    add_ctrl(hwnd, "BUTTON", "Clear", BS_OWNERDRAW | WS_TABSTOP, 243, 392, 60, 20, IDC_LOG_CLEAR_BTN);
    add_ctrl(hwnd, "LISTBOX", NULL, LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP | WS_BORDER,
             22, 416, 281, 176, IDC_LOG_LISTBOX);

    for (idx = 0; idx < MAX_CHANNELS; idx++) {
        add_channel_card(hwnd, idx);
    }

    for (i = 0; i < BAUD_OPTIONS_COUNT; i++) {
        char label[16];
        wsprintfA(label, "%d", BAUD_OPTIONS[i]);
        SendDlgItemMessageA(hwnd, IDC_BAUD_COMBO, CB_ADDSTRING, 0, (LPARAM)label);
        SendDlgItemMessageA(hwnd, IDC_BAUD_COMBO, CB_SETITEMDATA, i, (LPARAM)BAUD_OPTIONS[i]);
    }
    SendDlgItemMessageA(hwnd, IDC_BAUD_COMBO, CB_SETCURSEL, BAUD_DEFAULT_INDEX, 0);

    for (i = 0; i < DATABITS_OPTIONS_COUNT; i++) {
        char label[4];
        wsprintfA(label, "%d", DATABITS_OPTIONS[i]);
        SendDlgItemMessageA(hwnd, IDC_DATABITS_COMBO, CB_ADDSTRING, 0, (LPARAM)label);
    }
    SendDlgItemMessageA(hwnd, IDC_DATABITS_COMBO, CB_SETCURSEL, DATABITS_DEFAULT_INDEX, 0);

    for (i = 0; i < PARITY_OPTIONS_COUNT; i++) {
        SendDlgItemMessageA(hwnd, IDC_PARITY_COMBO, CB_ADDSTRING, 0, (LPARAM)PARITY_LABELS[i]);
    }
    SendDlgItemMessageA(hwnd, IDC_PARITY_COMBO, CB_SETCURSEL, 0, 0);

    g_layout_ready = true;
}

/* Moves AND resizes one channel card to a new rect, scaling every
 * control inside it proportionally (card_w/card_h vs. the designed
 * CARD_W/CARD_H) rather than leaving them their designed size - so
 * extra window space actually gets used by the cards themselves
 * instead of sitting empty as gaps between them. The icon stays a
 * fixed 14x14 (icons scaling blurrily is worse than a small icon in a
 * bigger card) and text stays the system font's normal size (buttons/
 * labels just get more padding) - everything else's position and size
 * scales. Every offset here must match add_channel_card()'s creation
 * offsets exactly - keep the two in sync if either changes.
 *
 * Plain MoveWindow with bRepaint=FALSE - relayout_for_size() does one
 * InvalidateRect over the whole window after moving everything, so
 * Windows coalesces it into a single WM_PAINT pass instead of each
 * control repainting itself individually. (A DeferWindowPos batch was
 * tried here for the same reason - don't: on real hardware it silently
 * failed to reposition/repaint every custom-drawn control, subclassed
 * panel, and owner-draw button, leaving only stock controls like the
 * mode combo boxes visible. Plain MoveWindow is what actually works.) */
static void position_channel_card(HWND hwnd, int index, int x, int y, int card_w, int card_h) {
    double sx = (double)card_w / CARD_W;
    double sy = (double)card_h / CARD_H;
#define SX(v) ((int)((v) * sx + 0.5))
#define SY(v) ((int)((v) * sy + 0.5))
#define PLACE(win, dx, dy, dw, dh) MoveWindow((win), (dx), (dy), (dw), (dh), FALSE)

    PLACE(g_card_panel[index], x, y, card_w, card_h);
    PLACE(g_card_icon[index], x + SX(8), y + SY(6), 14, 14);
    PLACE(g_card_header[index], x + SX(26), y + SY(6), SX(200), SY(16));

    PLACE(GetDlgItem(hwnd, channel_mode_id(index)), x + SX(8), y + SY(24), SX(96), 120);
    PLACE(GetDlgItem(hwnd, channel_set_id(index)), x + SX(108), y + SY(24), SX(46), SY(20));
    PLACE(GetDlgItem(hwnd, channel_on_id(index)), x + SX(8), y + SY(46), SX(71), SY(20));
    PLACE(GetDlgItem(hwnd, channel_off_id(index)), x + SX(83), y + SY(46), SX(71), SY(20));
    PLACE(GetDlgItem(hwnd, channel_status_id(index)), x + SX(8), y + SY(68), SX(146), SY(16));

    PLACE(GetDlgItem(hwnd, channel_track_id(index)), x + SX(164), y + SY(26), SX(26), SY(78));
    PLACE(GetDlgItem(hwnd, channel_lbl_high_id(index)), x + SX(194), y + SY(26), SX(44), SY(16));
    PLACE(GetDlgItem(hwnd, channel_lbl_medium_id(index)), x + SX(194), y + SY(45), SX(44), SY(16));
    PLACE(GetDlgItem(hwnd, channel_lbl_low_id(index)), x + SX(194), y + SY(64), SX(44), SY(16));
    PLACE(GetDlgItem(hwnd, channel_lbl_off_id(index)), x + SX(194), y + SY(83), SX(44), SY(16));

    PLACE(g_card_bandwidth_lbl[index], x + SX(8), y + SY(108), SX(96), SY(16));
    PLACE(GetDlgItem(hwnd, channel_temp_id(index)), x + SX(104), y + SY(108), SX(134), SY(16));

#undef SX
#undef SY
#undef PLACE
}

/* Recomputes the whole layout for a new client size: header bar and
 * sidebar stretch to fill (the sidebar's own content stays fixed size -
 * only its and the log's height change), and the 16 cards themselves
 * grow to fill the rest of the space (gap between them stays the
 * designed CARD_GAP) - extra window space becomes bigger cards, not
 * empty gaps. Never shrinks below the designed CARD_W x CARD_H (see
 * WM_GETMINMAXINFO, which stops the window itself getting that small). */
static void relayout_for_size(HWND hwnd, int client_w, int client_h) {
    int avail_w, avail_h, card_w, card_h, sidebar_h, extra_log_h, i;
    HWND listbox;

    if (!g_layout_ready) {
        return;
    }

    avail_w = client_w - GRID_LEFT - SIDEBAR_X;
    avail_h = client_h - CONTENT_TOP - 12;

    card_w = (avail_w - (GRID_COLS - 1) * CARD_GAP) / GRID_COLS;
    if (card_w < CARD_W) card_w = CARD_W;
    card_h = (avail_h - (GRID_ROWS - 1) * CARD_GAP) / GRID_ROWS;
    if (card_h < CARD_H) card_h = CARD_H;

    MoveWindow(g_header_panel, SIDEBAR_X, 6, client_w - 2 * SIDEBAR_X, HEADER_H, FALSE);
    MoveWindow(g_title_ctrl, 22, 16, client_w - 2 * SIDEBAR_X - 32, 28, FALSE);

    sidebar_h = GRID_ROWS * card_h + (GRID_ROWS - 1) * CARD_GAP;
    MoveWindow(g_sidebar_panel, SIDEBAR_X, CONTENT_TOP, SIDEBAR_W, sidebar_h, FALSE);

    extra_log_h = sidebar_h - (GRID_ROWS * CARD_H + (GRID_ROWS - 1) * CARD_GAP);
    listbox = GetDlgItem(hwnd, IDC_LOG_LISTBOX);
    if (listbox) {
        MoveWindow(listbox, 22, 416, 281, 176 + extra_log_h, FALSE);
    }

    for (i = 0; i < MAX_CHANNELS; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        int card_x = GRID_LEFT + col * (card_w + CARD_GAP);
        int card_y = CONTENT_TOP + row * (card_h + CARD_GAP);
        position_channel_card(hwnd, i, card_x, card_y, card_w, card_h);
    }

    /* One coalesced repaint for the whole window AND every child control
     * in it, instead of each of the ~240 moved controls repainting
     * itself individually (that's what made resizing feel unresponsive
     * originally). RDW_ALLCHILDREN is the important part here - plain
     * InvalidateRect(hwnd, ...) only invalidates hwnd's own client area,
     * NOT its children's, so combo boxes and other child controls could
     * end up not repainting at all (stayed blank until they happened to
     * get focus) despite having been correctly moved. */
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

/* ---- WndProc ---- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            ConnectionCallbacks ccb;

            g_hwnd = hwnd;
            g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            g_header_font = CreateFontA(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                         ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            if (!g_header_font) {
                g_header_font = g_font;
            }

            g_title_font = CreateFontA(-24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            if (!g_title_font) {
                g_title_font = g_header_font;
            }

            build_controls(hwnd);
            refresh_port_list();
            refresh_sensor_port_list();
            load_settings();

            memset(&ccb, 0, sizeof(ccb));
            ccb.on_connected_changed = conn_on_connected_changed;
            ccb.on_frame = conn_on_frame;
            ccb.on_error = conn_on_error;
            conn_init(&g_conn, ccb);
            channels_init(&g_conn);
            sensor_init(&g_sensor);
            {
                int addr_i;
                for (addr_i = 0; addr_i < MAX_CHANNELS; addr_i++) {
                    sensor_set_unit_address(&g_sensor, addr_i, UNIT_TEMP_ADDR[addr_i]);
                }
            }

            SetTimer(hwnd, ID_POLL_TIMER, 100, NULL);
            ui_refresh_all_channels();
            ui_refresh_sensor();
            ui_refresh_sensor_mode();
            return 0;
        }

        case WM_GETMINMAXINFO: {
            /* Never let the window shrink below its designed layout size -
             * relayout_for_size() only ever grows gaps to fill extra space,
             * never shrinks cards, so a smaller client area would start
             * overlapping them. */
            MINMAXINFO *mmi = (MINMAXINFO *)lParam;
            RECT rect;
            rect.left = 0;
            rect.top = 0;
            rect.right = CLIENT_WIDTH;
            rect.bottom = CLIENT_HEIGHT;
            AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME, FALSE, 0);
            mmi->ptMinTrackSize.x = rect.right - rect.left;
            mmi->ptMinTrackSize.y = rect.bottom - rect.top;
            return 0;
        }

        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                /* Windows can send WM_SIZE with unchanged dimensions in
                 * some edge cases (activation, DPI/monitor changes) -
                 * skip the ~240-control relayout pass when the size
                 * didn't actually change, including the redundant one
                 * right after the startup pre-layout already handled
                 * this exact size before the window was ever shown. */
                int new_w = LOWORD(lParam);
                int new_h = HIWORD(lParam);
                if (new_w != g_last_client_w || new_h != g_last_client_h) {
                    g_last_client_w = new_w;
                    g_last_client_h = new_h;
                    relayout_for_size(hwnd, new_w, new_h);
                }
            }
            return 0;

        case WM_TIMER:
            if (wParam == ID_POLL_TIMER) {
                conn_poll(&g_conn);
                channels_poll();
                ui_refresh_all_channels();
                sensor_poll(&g_sensor);
                ui_refresh_sensor();
                check_kill_switch();
                ui_refresh_kill_switch();
            }
            return 0;

        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hwnd, &rc);
            /* Pattern brush already encodes the page background color
             * in its tile - no separate full-rect FillRect needed. */
            FillRect(hdc, &rc, g_brush_dot_pattern ? g_brush_dot_pattern : g_brush_page);
            return 1;
        }

        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            WORD code = HIWORD(wParam);

            if (id == IDC_REFRESH_BTN && code == BN_CLICKED) {
                refresh_port_list();
                return 0;
            }
            if (id == IDC_CONNECT_BTN && code == BN_CLICKED) {
                on_connect_clicked();
                return 0;
            }
            if (id == IDC_SENSOR_REFRESH_BTN && code == BN_CLICKED) {
                refresh_sensor_port_list();
                return 0;
            }
            if (id == IDC_SENSOR_CONNECT_BTN && code == BN_CLICKED) {
                on_sensor_connect_clicked();
                return 0;
            }
            if (id == IDC_SENSOR_MODE_SCAN_BTN && code == BN_CLICKED) {
                sensor_set_mode(&g_sensor, SENSOR_MODE_SCAN);
                ui_refresh_sensor_mode();
                return 0;
            }
            if (id == IDC_SENSOR_MODE_UNIT_BTN && code == BN_CLICKED) {
                sensor_set_mode(&g_sensor, SENSOR_MODE_PER_UNIT);
                ui_refresh_sensor_mode();
                return 0;
            }
            if (id == IDC_KILL_RESET_BTN && code == BN_CLICKED) {
                on_kill_reset_clicked();
                return 0;
            }
            if (id == IDC_LOG_CLEAR_BTN && code == BN_CLICKED) {
                SendDlgItemMessageA(hwnd, IDC_LOG_LISTBOX, LB_RESETCONTENT, 0, 0);
                return 0;
            }
            {
                int idx;
                if (channel_index_from_id(id, &idx)) {
                    int offset = (id - IDC_CH_BASE) % IDC_CH_STRIDE;
                    /* Mode selection is local/uncommitted until Set is
                     * clicked - matches the reference apps exactly
                     * (selecting a mode does NOT apply it by itself). */
                    /* OFF always works, even tripped - turning things off
                     * is never unsafe. SET/ON are blocked while tripped so
                     * the kill switch can't be trivially defeated by just
                     * clicking a channel back on before acknowledging it -
                     * that's the whole point of "manual reset only". */
                    if (offset == IDC_CH_SET_OFFSET && code == BN_CLICKED) {
                        if (!g_kill_switch_tripped[idx]) {
                            int sel = (int)SendDlgItemMessageA(hwnd, channel_mode_id(idx), CB_GETCURSEL, 0, 0);
                            if (sel >= 0) {
                                channel_set_mode(idx, (uint8_t)sel);
                            }
                        }
                    } else if (offset == IDC_CH_ON_OFFSET && code == BN_CLICKED) {
                        if (!g_kill_switch_tripped[idx]) {
                            channel_turn_output_on(idx);
                        }
                    } else if (offset == IDC_CH_OFF_OFFSET && code == BN_CLICKED) {
                        channel_turn_output_off(idx);
                    } else if (offset == IDC_CH_TEMP_LBL_OFFSET && code == STN_CLICKED) {
                        on_unit_kill_reset(idx);
                    }
                    return 0;
                }
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HWND ctl = (HWND)lParam;
            HDC hdc = (HDC)wParam;
            int ctl_id = GetDlgCtrlID(ctl);
            int idx;
            if (ctl == GetDlgItem(hwnd, IDC_CONN_STATUS_LBL)) {
                SetTextColor(hdc, conn_is_connected(&g_conn) ? COLOR_APP_CONNECTED : COLOR_APP_DISCONNECTED);
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)g_brush_panel;
            }
            if (ctl == GetDlgItem(hwnd, IDC_SENSOR_STATUS_LBL)) {
                const SensorState *st = sensor_get_state(&g_sensor, 0);
                COLORREF col = !sensor_is_connected(&g_sensor) ? COLOR_APP_DISCONNECTED
                             : st->online ? COLOR_APP_CONNECTED
                             : COLOR_APP_ACCENT;
                SetTextColor(hdc, col);
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)g_brush_panel;
            }
            if (ctl == GetDlgItem(hwnd, IDC_SENSOR_TEMP_LBL)) {
                const SensorState *st = sensor_get_state(&g_sensor, 0);
                SetTextColor(hdc, st->has_reading ? temp_band_color(st->temperature_c) : COLOR_APP_MUTED);
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)g_brush_panel;
            }
            if (ctl == GetDlgItem(hwnd, IDC_KILL_STATUS_LBL)) {
                SetTextColor(hdc, COLOR_APP_DISCONNECTED);
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)g_brush_panel;
            }
            if (channel_index_from_id(ctl_id, &idx)) {
                int offset = (ctl_id - IDC_CH_BASE) % IDC_CH_STRIDE;
                const ChannelState *ch = channels_get(idx);
                if (offset == IDC_CH_STATUS_OFFSET) {
                    COLORREF col = ch->busy ? COLOR_APP_ACCENT
                                   : (ch->output_on ? COLOR_APP_CONNECTED : COLOR_APP_MUTED);
                    SetTextColor(hdc, col);
                    SetBkMode(hdc, TRANSPARENT);
                    return (LRESULT)g_brush_panel;
                }
                if (offset == IDC_CH_LBL_HIGH_OFFSET || offset == IDC_CH_LBL_MEDIUM_OFFSET ||
                    offset == IDC_CH_LBL_LOW_OFFSET || offset == IDC_CH_LBL_OFF_OFFSET) {
                    int lvl_for_label = (offset == IDC_CH_LBL_HIGH_OFFSET)   ? LEVEL_HIGH
                                       : (offset == IDC_CH_LBL_MEDIUM_OFFSET) ? LEVEL_MEDIUM
                                       : (offset == IDC_CH_LBL_LOW_OFFSET)    ? LEVEL_LOW
                                                                               : LEVEL_OFF;
                    SetTextColor(hdc, (ch->level == lvl_for_label) ? COLOR_APP_HEADER : COLOR_APP_MUTED);
                    SetBkMode(hdc, TRANSPARENT);
                    return (LRESULT)g_brush_panel;
                }
                if (offset == IDC_CH_TEMP_LBL_OFFSET) {
                    COLORREF col;
                    if (g_kill_switch_tripped[idx]) {
                        col = COLOR_APP_DISCONNECTED; /* red - click to reset */
                    } else {
                        const SensorState *st = sensor_get_state(&g_sensor, idx);
                        col = st->has_reading ? temp_band_color(st->temperature_c) : COLOR_APP_MUTED;
                    }
                    SetTextColor(hdc, col);
                    SetBkMode(hdc, TRANSPARENT);
                    return (LRESULT)g_brush_panel;
                }
            }
            {
                HFONT ctl_font = (HFONT)SendMessageA(ctl, WM_GETFONT, 0, 0);
                if (ctl_font == g_header_font || ctl_font == g_title_font) {
                    SetTextColor(hdc, COLOR_APP_HEADER);
                } else {
                    SetTextColor(hdc, COLOR_APP_MUTED);
                }
            }
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)g_brush_panel;
        }

        case WM_CTLCOLORBTN: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COLOR_APP_TEXT);
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)g_brush_panel;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COLOR_APP_TEXT);
            SetBkColor(hdc, COLOR_APP_FIELD_BG);
            SetBkMode(hdc, OPAQUE);
            return (LRESULT)g_brush_field;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) {
                char text[64];
                bool disabled = (dis->itemState & ODS_DISABLED) != 0;
                RECT rc = dis->rcItem;
                int idx;
                int offset = -1;

                if (channel_index_from_id((int)dis->CtlID, &idx)) {
                    offset = ((int)dis->CtlID - IDC_CH_BASE) % IDC_CH_STRIDE;
                }

                /* ON/OFF are two real, separate buttons (not one toggle) -
                 * whichever is active gets a solid fill (green ON / red
                 * OFF), matching the reference apps' PowerButton exactly. */
                if (offset == IDC_CH_ON_OFFSET || offset == IDC_CH_OFF_OFFSET) {
                    const ChannelState *ch = channels_get(idx);
                    bool active = (offset == IDC_CH_ON_OFFSET) ? ch->output_on : !ch->output_on;
                    HBRUSH fill = active
                        ? (offset == IDC_CH_ON_OFFSET ? g_brush_connected : g_brush_disconnected)
                        : g_brush_panel;
                    HPEN pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
                    HPEN old_pen = (HPEN)SelectObject(dis->hDC, pen);
                    HBRUSH old_brush = (HBRUSH)SelectObject(dis->hDC, fill);

                    Rectangle(dis->hDC, rc.left, rc.top, rc.right, rc.bottom);
                    SelectObject(dis->hDC, old_brush);
                    SelectObject(dis->hDC, old_pen);
                    DeleteObject(pen);

                    SetTextColor(dis->hDC, active ? RGB(255, 255, 255) : COLOR_APP_MUTED);
                    SetBkMode(dis->hDC, TRANSPARENT);
                    GetWindowTextA(dis->hwndItem, text, sizeof(text));
                    DrawTextA(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    return TRUE;
                }

                /* Scan / Per-Unit sensor mode - same active/inactive
                 * pattern as ON/OFF above (solid fill for the active
                 * one), just both sharing one accent color instead of
                 * green/red since neither reading is inherently good or
                 * bad the way power on/off is. */
                if (dis->CtlID == IDC_SENSOR_MODE_SCAN_BTN || dis->CtlID == IDC_SENSOR_MODE_UNIT_BTN) {
                    bool active = (dis->CtlID == IDC_SENSOR_MODE_SCAN_BTN)
                        ? (sensor_get_mode(&g_sensor) == SENSOR_MODE_SCAN)
                        : (sensor_get_mode(&g_sensor) == SENSOR_MODE_PER_UNIT);
                    HBRUSH fill = active ? g_brush_accent : g_brush_panel;
                    HPEN pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
                    HPEN old_pen = (HPEN)SelectObject(dis->hDC, pen);
                    HBRUSH old_brush = (HBRUSH)SelectObject(dis->hDC, fill);

                    Rectangle(dis->hDC, rc.left, rc.top, rc.right, rc.bottom);
                    SelectObject(dis->hDC, old_brush);
                    SelectObject(dis->hDC, old_pen);
                    DeleteObject(pen);

                    SetTextColor(dis->hDC, active ? RGB(255, 255, 255) : COLOR_APP_MUTED);
                    SetBkMode(dis->hDC, TRANSPARENT);
                    GetWindowTextA(dis->hwndItem, text, sizeof(text));
                    DrawTextA(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    return TRUE;
                }

                FillRect(dis->hDC, &rc, disabled ? g_brush_accent_dis : g_brush_accent);
                SetTextColor(dis->hDC, RGB(255, 255, 255));
                SetBkMode(dis->hDC, TRANSPARENT);
                GetWindowTextA(dis->hwndItem, text, sizeof(text));
                DrawTextA(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (dis->itemState & ODS_FOCUS) {
                    RECT focus_rc = rc;
                    InflateRect(&focus_rc, -3, -3);
                    DrawFocusRect(dis->hDC, &focus_rc);
                }
                return TRUE;
            }
            break;
        }

        case WM_DESTROY:
            save_settings();
            KillTimer(hwnd, ID_POLL_TIMER);
            if (conn_is_connected(&g_conn)) {
                conn_disconnect(&g_conn);
            }
            if (sensor_is_connected(&g_sensor)) {
                sensor_disconnect(&g_sensor);
            }
            if (g_brush_panel) DeleteObject(g_brush_panel);
            if (g_brush_page) DeleteObject(g_brush_page);
            if (g_brush_field) DeleteObject(g_brush_field);
            if (g_brush_accent) DeleteObject(g_brush_accent);
            if (g_brush_accent_dis) DeleteObject(g_brush_accent_dis);
            if (g_brush_dot) DeleteObject(g_brush_dot);
            if (g_brush_connected) DeleteObject(g_brush_connected);
            if (g_brush_disconnected) DeleteObject(g_brush_disconnected);
            if (g_brush_silver) DeleteObject(g_brush_silver);
            if (g_brush_dot_pattern) DeleteObject(g_brush_dot_pattern);
            if (g_dot_pattern_bmp) DeleteObject(g_dot_pattern_bmp);
            if (g_header_font && g_header_font != g_font) DeleteObject(g_header_font);
            if (g_title_font && g_title_font != g_header_font) DeleteObject(g_title_font);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEXA wc;
    RECT rect;
    HWND hwnd;
    MSG msg;

    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow; /* always starts maximized instead - see SW_SHOWMAXIMIZED below */

    g_hinst = hInstance;

    g_brush_page = CreateSolidBrush(COLOR_APP_PAGE_BG);
    g_brush_panel = CreateSolidBrush(COLOR_APP_PANEL_BG);
    g_brush_field = CreateSolidBrush(COLOR_APP_FIELD_BG);
    g_brush_accent = CreateSolidBrush(COLOR_APP_ACCENT);
    g_brush_accent_dis = CreateSolidBrush(COLOR_APP_ACCENT_DIS);
    g_brush_dot = CreateSolidBrush(COLOR_APP_DOT);
    g_brush_connected = CreateSolidBrush(COLOR_APP_CONNECTED);
    g_brush_disconnected = CreateSolidBrush(COLOR_APP_DISCONNECTED);
    g_brush_silver = CreateSolidBrush(COLOR_APP_SILVER);
    build_dot_pattern_brush();

    /* NOT CS_HREDRAW | CS_VREDRAW - that forces the ENTIRE window to
     * repaint on every resize, on top of the explicit repaint
     * relayout_for_size() already does itself; on a live resize drag
     * that was two full-window erase+repaints per frame instead of
     * one. relayout_for_size() invalidating what it just changed is
     * enough. */
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!wc.hIcon) {
        wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    }
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = g_brush_page;
    wc.lpszClassName = "DigitalNoiseConfigMultiMainWindow";
    RegisterClassExA(&wc);

    rect.left = 0;
    rect.top = 0;
    rect.right = CLIENT_WIDTH;
    rect.bottom = CLIENT_HEIGHT;
    /* WS_THICKFRAME + WS_MAXIMIZEBOX: resizable and maximizable, not just
     * a fixed-size dialog-style window - starts maximized (below) since
     * the app is meant to run fullscreen, but the user can still restore/
     * resize it manually. The fixed-pixel content layout itself doesn't
     * yet reflow to fill extra space - it just sits anchored top-left at
     * its designed size within whatever the window's actual size is. */
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME, FALSE, 0);

    /* NOT WS_EX_COMPOSITED - tried it here to smooth out full repaints,
     * but with 240+ child controls it made DWM's per-child compositing
     * overhead worse, not better (real hardware showed panels rendering
     * with all their content missing/delayed). The actual cost was
     * draw_dot_grid() below - fixed properly there instead. */
    hwnd = CreateWindowExA(0, "DigitalNoiseConfigMultiMainWindow", "Digital Noise Configuration - Multi",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            rect.right - rect.left, rect.bottom - rect.top,
                            NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        return 0;
    }

    /* The window is still hidden here (no WS_VISIBLE) - WM_CREATE has
     * already run build_controls(), so every control exists at its
     * small design-size position. Lay everything out for the maximized
     * size RIGHT NOW, before the window is ever shown, instead of
     * showing it small first and visibly snapping/rearranging to fill
     * the screen once WM_SIZE's own relayout runs a moment later. */
    {
        RECT work_area;
        RECT chrome;
        int client_w, client_h;

        if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &work_area, 0)) {
            chrome.left = 0;
            chrome.top = 0;
            chrome.right = 0;
            chrome.bottom = 0;
            AdjustWindowRectEx(&chrome, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME, FALSE, 0);
            client_w = (work_area.right - work_area.left) - (chrome.right - chrome.left);
            client_h = (work_area.bottom - work_area.top) - (chrome.bottom - chrome.top);
            relayout_for_size(hwnd, client_w, client_h);
            g_last_client_w = client_w;
            g_last_client_h = client_h;
        }
    }

    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return (int)msg.wParam;
}
