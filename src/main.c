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
#define CLIENT_HEIGHT 688

/* Header bar across the top, above the sidebar/grid content: the
 * "Connection & Settings" section - icon + heading, same as it had
 * back when this lived in the sidebar, then Port/Refresh/Connect,
 * status, and Baud/Data Bits/Parity stacked as separate rows beneath
 * it (same multi-row shape it had in the sidebar - a single wide row
 * read oddly stretched across the full header width) - moved up here
 * from the sidebar, along with Amplifier Temperature, so the sidebar
 * is free for other features (Activity Log moved out too - see
 * LOG_PANEL_Y - so it's not just those two anymore). Sized to fit all
 * of it snugly with real top/bottom padding, plus room for Amplifier
 * Temperature's Unit list row. HEADER_H is the bar's own height;
 * CONTENT_TOP is where the sidebar panels and channel grid start
 * beneath it (same 6px top margin and 8px panel-to-panel gap used
 * everywhere else). */
#define HEADER_H     230
#define CONTENT_TOP  244

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

/* Kill switch: rack-wide, not per-channel - there are only 6 physical
 * sensors scanning the area, not one per RF channel, so there's no
 * single channel's "own" reading to check individually anymore. Trips
 * every one of the 16 channels off at once when the average across all
 * 6 sensors (sensor_average_temperature()) crosses the threshold.
 * Manual reset only, deliberately - once tripped, a channel stays off
 * (and new ON/Set/level commands for it are blocked) even if the
 * average drops back down, until the user explicitly resets it (either
 * that one channel, or all of them via the sidebar Reset button).
 * Auto-resuming at the threshold would let it silently cycle on/off
 * right at the boundary, defeating the point of a safety cutoff. */
#define KILL_SWITCH_THRESHOLD_C 60.0f

/* Modbus slave address each of the 6 physical sensors is wired to.
 * Defaults to the unit number, 1-indexed - edit this table once the real
 * wiring is known, since it's very likely not sequential. Pushed into
 * the sensor at WM_CREATE via sensor_set_unit_address(). */
static const uint8_t UNIT_TEMP_ADDR[SENSOR_MAX_UNITS] = { 1, 2, 3, 4, 5, 6 };

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

/* Channel cards only ("Direction B" from the UI design proposal) -
 * rounded corners instead of the chamfer, plus a brighter/thicker
 * border while that channel's output is on, standing in for a glow -
 * GDI has no blurred box-shadow, so a solid highlight border is the
 * cheap approximation. Every other panel (header, sidebar) keeps the
 * chamfer via panel_subclass_proc, unchanged. */
#define CARD_CORNER_DIAMETER 16
#define CARD_BORDER_ON_WIDTH 2
#define DOT_GRID_SPACING 8
#define DOT_GRID_SIZE 2

/* --- grid layout for the 16 channel cards --- */
#define GRID_COLS 4
#define GRID_ROWS 4
/* Widened a little from the previous 200-wide design (see git history
 * for why). CARD_H sizes to fit the level gauge/tick-label column now
 * - the bottom-row Bandwidth/Address statics that used to extend it
 * are gone too, removed at the same time as the temperature/humidity
 * readouts before them. */
#define CARD_W 224
#define CARD_H 102
#define CARD_GAP 8
#define GRID_LEFT 380
#define GRID_TOP CONTENT_TOP

#define SIDEBAR_X 10
#define SIDEBAR_W 360

/* Flush against the bottom of the sidebar box (itself pinned to the
 * grid's height) rather than added below it - keeps the sidebar's
 * bottom edge exactly at the grid's bottom edge, no leftover empty
 * strip past it. */
#define LOG_PANEL_H 220
#define LOG_PANEL_Y (CONTENT_TOP + GRID_ROWS * CARD_H + (GRID_ROWS - 1) * CARD_GAP - LOG_PANEL_H)

static HINSTANCE g_hinst;
static HWND g_hwnd;
static HFONT g_font;
static HFONT g_header_font;
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
static HWND g_sidebar_panel;
static HWND g_card_panel[MAX_CHANNELS];
static HWND g_card_icon[MAX_CHANNELS];
static HWND g_card_header[MAX_CHANNELS];
static HWND g_sensor_chip[SENSOR_MAX_UNITS];
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

/* Rounded-corner card panel - channel index stashed in GWLP_USERDATA so
 * it can read that channel's own on/off state at paint time, same
 * live-read pattern as sensor_chip_subclass_proc. */
static LRESULT CALLBACK card_panel_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        HBRUSH old_brush;
        HPEN pen, old_pen;
        int index = (int)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        const ChannelState *ch = channels_get(index);
        bool on = ch->output_on;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);

        old_brush = (HBRUSH)SelectObject(hdc, g_brush_panel);
        pen = CreatePen(PS_SOLID, on ? CARD_BORDER_ON_WIDTH : 1,
                         on ? COLOR_APP_CONNECTED : COLOR_APP_PANEL_BORDER);
        old_pen = (HPEN)SelectObject(hdc, pen);

        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom,
                  CARD_CORNER_DIAMETER, CARD_CORNER_DIAMETER);

        SelectObject(hdc, old_pen);
        DeleteObject(pen);
        SelectObject(hdc, old_brush);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

static HWND add_card_panel(HWND parent, int x, int y, int w, int h, int index) {
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT, x, y, w, h, 0);
    if (ctrl) {
        if (!g_panel_orig_proc) {
            g_panel_orig_proc = (WNDPROC)GetWindowLongPtrA(ctrl, GWLP_WNDPROC);
        }
        SetWindowLongPtrA(ctrl, GWLP_USERDATA, (LONG_PTR)index);
        SetWindowLongPtrA(ctrl, GWLP_WNDPROC, (LONG_PTR)card_panel_subclass_proc);
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

/* Small rounded "mini card" for one sensor unit's address + reading -
 * reads live off g_sensor each paint (unit_index stashed in
 * GWLP_USERDATA at creation) rather than being fed text, same pattern
 * as the other self-drawing gauges above. Muted "-" when that unit
 * doesn't have a reading yet. */
static LRESULT CALLBACK sensor_chip_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc, addr_rc, val_rc;
        int unit_index = (int)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        const SensorState *st = sensor_get_state(&g_sensor, unit_index);
        char addr_text[16];
        char val_text[16];
        HBRUSH bg_brush;
        HPEN border_pen, old_pen;
        HGDIOBJ old_brush;
        HFONT old_font;
        COLORREF val_color;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);

        bg_brush = CreateSolidBrush(COLOR_APP_FIELD_BG);
        border_pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
        old_pen = (HPEN)SelectObject(hdc, border_pen);
        old_brush = SelectObject(hdc, bg_brush);
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(border_pen);
        DeleteObject(bg_brush);

        SetBkMode(hdc, TRANSPARENT);
        old_font = (HFONT)SelectObject(hdc, g_font);

        wsprintfA(addr_text, "Addr %d", sensor_get_unit_address(&g_sensor, unit_index));
        addr_rc = rc;
        addr_rc.top += 4;
        addr_rc.bottom = addr_rc.top + 14;
        SetTextColor(hdc, COLOR_APP_MUTED);
        DrawTextA(hdc, addr_text, -1, &addr_rc, DT_CENTER | DT_SINGLELINE);

        val_rc = rc;
        val_rc.top = addr_rc.bottom;
        val_rc.bottom = rc.bottom - 2;
        if (st->has_reading) {
            wsprintfA(val_text, "%d.%d C", (int)st->temperature_c, (int)(st->temperature_c * 10) % 10);
            val_color = temp_band_color(st->temperature_c);
        } else {
            lstrcpynA(val_text, "-", (int)sizeof(val_text));
            val_color = COLOR_APP_MUTED;
        }
        SetTextColor(hdc, val_color);
        DrawTextA(hdc, val_text, -1, &val_rc, DT_CENTER | DT_SINGLELINE);

        SelectObject(hdc, old_font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

static HWND add_sensor_chip(HWND parent, int x, int y, int w, int h, int unit_index) {
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT, x, y, w, h, 0);
    if (ctrl) {
        if (!g_panel_orig_proc) {
            g_panel_orig_proc = (WNDPROC)GetWindowLongPtrA(ctrl, GWLP_WNDPROC);
        }
        SetWindowLongPtrA(ctrl, GWLP_USERDATA, (LONG_PTR)unit_index);
        SetWindowLongPtrA(ctrl, GWLP_WNDPROC, (LONG_PTR)sensor_chip_subclass_proc);
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
static bool g_sensor_ui_has_reading;
static float g_sensor_ui_temp;
static int g_sensor_ui_count;

static bool g_sensor_connect_btn_valid;
static bool g_sensor_connect_btn_connected;

static void ui_refresh_sensor(void) {
    /* Rack-wide summary - the average across the 6 physical sensors that
     * currently have a reading (see UNIT_TEMP_ADDR) - not tied to the 16
     * RF channels. The Connect button has its own small change-detection
     * gate below, separate from the average. */
    bool connected = sensor_is_connected(&g_sensor);
    bool has_avg;
    float avg_c = 0.0f;
    int reading_count = 0;
    int i;
    char text[64];

    if (!g_sensor_connect_btn_valid || g_sensor_connect_btn_connected != connected) {
        EnableWindow(GetDlgItem(g_hwnd, IDC_SENSOR_CONNECT_BTN), TRUE);
        SetWindowTextA(GetDlgItem(g_hwnd, IDC_SENSOR_CONNECT_BTN), connected ? "Disconnect" : "Connect");
        g_sensor_connect_btn_valid = true;
        g_sensor_connect_btn_connected = connected;
    }

    has_avg = sensor_average_temperature(&g_sensor, &avg_c);
    for (i = 0; i < SENSOR_MAX_UNITS; i++) {
        if (sensor_get_state(&g_sensor, i)->has_reading) {
            reading_count++;
        }
    }

    if (g_sensor_ui_valid && g_sensor_ui_connected == connected &&
        g_sensor_ui_has_reading == has_avg && g_sensor_ui_temp == avg_c &&
        g_sensor_ui_count == reading_count) {
        return; /* nothing shown by this panel has changed */
    }

    if (!connected) {
        lstrcpynA(text, "Disconnected", (int)sizeof(text));
    } else if (has_avg) {
        lstrcpynA(text, "Online", (int)sizeof(text));
    } else {
        lstrcpynA(text, "Reading...", (int)sizeof(text));
    }
    SetDlgItemTextA(g_hwnd, IDC_SENSOR_STATUS_LBL, text);
    InvalidateRect(GetDlgItem(g_hwnd, IDC_SENSOR_STATUS_LBL), NULL, FALSE);

    if (has_avg) {
        wsprintfA(text, "Avg %d.%d C", (int)avg_c, (int)(avg_c * 10) % 10);
    } else {
        lstrcpynA(text, "Avg -", (int)sizeof(text));
    }
    SetDlgItemTextA(g_hwnd, IDC_SENSOR_TEMP_LBL, text);
    InvalidateRect(GetDlgItem(g_hwnd, IDC_SENSOR_TEMP_LBL), NULL, FALSE);

    /* Each unit's mini card reads live off g_sensor when it paints (see
     * sensor_chip_subclass_proc()) - just needs a repaint kicked off
     * here, not text pushed into it. */
    {
        int u;
        for (u = 0; u < SENSOR_MAX_UNITS; u++) {
            InvalidateRect(g_sensor_chip[u], NULL, FALSE);
        }
    }

    g_sensor_ui_valid = true;
    g_sensor_ui_connected = connected;
    g_sensor_ui_has_reading = has_avg;
    g_sensor_ui_temp = avg_c;
    g_sensor_ui_count = reading_count;
}

/* ---- kill switch ----
 * One trip flag per channel, but they all trip together off the same
 * rack-wide average (see KILL_SWITCH_THRESHOLD_C's comment for why) -
 * there's no per-channel sensor reading to check individually. */

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
 * up top for why. Call once per timer tick. Rack-wide: trips every
 * not-yet-tripped channel at once off the same average reading (there's
 * no per-channel sensor to check individually anymore - see
 * KILL_SWITCH_THRESHOLD_C). A channel reset individually while the
 * average is still over threshold will simply retrip on the next tick -
 * expected, not a bug: the underlying condition hasn't cleared. */
static void check_kill_switch(void) {
    bool has_avg;
    float avg_c;
    int i;
    int newly_tripped = 0;

    has_avg = sensor_average_temperature(&g_sensor, &avg_c);
    if (!has_avg || avg_c < KILL_SWITCH_THRESHOLD_C) {
        return;
    }

    for (i = 0; i < MAX_CHANNELS; i++) {
        if (!g_kill_switch_tripped[i]) {
            g_kill_switch_tripped[i] = true;
            channel_turn_output_off(i);
            newly_tripped++;
        }
    }

    if (newly_tripped > 0) {
        char msg[96];
        wsprintfA(msg, "KILL SWITCH TRIPPED (avg %d.%d C >= %d C) - %d channel%s forced OFF",
                  (int)avg_c, (int)(avg_c * 10) % 10, (int)KILL_SWITCH_THRESHOLD_C,
                  newly_tripped, newly_tripped == 1 ? "" : "s");
        log_add(msg);
    }
}

/* Sidebar's Reset button - resets every currently-tripped unit at once
 * (whichever ones happen to be over threshold) - a convenient "reset
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
 * Wired to a click on that unit's card status line while it's tripped
 * (see IDC_CH_STATUS_OFFSET's comment in resource.h). */
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
 * buttons, status line), a right column with the level gauge + tick
 * labels, then a bottom row of this unit's fixed config (bandwidth,
 * wired sensor address). */
static void add_channel_card(HWND hwnd, int index) {
    int col = index % GRID_COLS;
    int row = index / GRID_COLS;
    int x = GRID_LEFT + col * (CARD_W + CARD_GAP);
    int y = GRID_TOP + row * (CARD_H + CARD_GAP);
    char header[16];
    int i;
    HWND mode_combo;

    g_card_panel[index] = add_card_panel(hwnd, x, y, CARD_W, CARD_H, index);
    g_card_icon[index] = add_header_icon(hwnd, x + 8, y + 6, ICON_WAVE);
    wsprintfA(header, "Unit %d", index + 1);
    g_card_header[index] = add_header(hwnd, header, x + 26, y + 6, 170, 16);

    mode_combo = add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                           x + 8, y + 24, 82, 100, channel_mode_id(index));
    for (i = 0; i < PROTO_MODE_COUNT; i++) {
        const char *name = proto_mode_name((uint8_t)i);
        SendMessageA(mode_combo, CB_ADDSTRING, 0, (LPARAM)(name ? name : "?"));
    }
    SendMessageA(mode_combo, CB_SETCURSEL, PROTO_MODE_WHITE_NOISE, 0);
    SendMessageA(mode_combo, CB_SETDROPPEDWIDTH, 190, 0);

    add_ctrl(hwnd, "BUTTON", "Set", BS_OWNERDRAW | WS_TABSTOP,
             x + 94, y + 24, 40, 18, channel_set_id(index));

    add_ctrl(hwnd, "BUTTON", "ON", BS_OWNERDRAW | WS_TABSTOP,
             x + 8, y + 44, 60, 18, channel_on_id(index));
    add_ctrl(hwnd, "BUTTON", "OFF", BS_OWNERDRAW | WS_TABSTOP,
             x + 72, y + 44, 60, 18, channel_off_id(index));

    /* SS_NOTIFY: this label doubles as the per-unit kill-switch reset -
     * see IDC_CH_STATUS_OFFSET's comment in resource.h. */
    add_ctrl(hwnd, "STATIC", "STANDBY", SS_LEFT | SS_NOPREFIX | SS_NOTIFY,
             x + 8, y + 64, 130, 14, channel_status_id(index));

    /* Right column: custom gradient level gauge (Off at bottom, High at
     * top, like a volume slider) + tick labels. */
    add_channel_gauge(hwnd, x + 148, y + 24, 22, 72, channel_track_id(index));

    add_ctrl(hwnd, "STATIC", "High",   SS_LEFT | SS_NOPREFIX, x + 174, y + 24, 44, 14, channel_lbl_high_id(index));
    add_ctrl(hwnd, "STATIC", "Medium", SS_LEFT | SS_NOPREFIX, x + 174, y + 42, 44, 14, channel_lbl_medium_id(index));
    add_ctrl(hwnd, "STATIC", "Low",    SS_LEFT | SS_NOPREFIX, x + 174, y + 60, 44, 14, channel_lbl_low_id(index));
    add_ctrl(hwnd, "STATIC", "Off",    SS_LEFT | SS_NOPREFIX, x + 174, y + 78, 44, 14, channel_lbl_off_id(index));

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
    bool tripped;
} ChannelUiCache;

static ChannelUiCache g_ui_cache[MAX_CHANNELS];

static void ui_refresh_channel(int index) {
    const ChannelState *ch = channels_get(index);
    ChannelUiCache *cache = &g_ui_cache[index];
    HWND status_ctl;
    HWND track;
    char text[32];
    bool tripped = g_kill_switch_tripped[index];

    if (cache->valid && cache->busy == ch->busy &&
        cache->output_on == ch->output_on && cache->level == ch->level &&
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

    /* Matches sdr_react's ChannelCard status text exactly: busy ->
     * SENDING..., on -> the level name, off -> STANDBY - except while
     * tripped, which takes over the same line (see
     * IDC_CH_STATUS_OFFSET's comment in resource.h for why it's also
     * the per-unit kill-switch reset). */
    if (tripped) {
        lstrcpynA(text, "TRIPPED - reset?", (int)sizeof(text));
    } else if (ch->busy) {
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

    InvalidateRect(GetDlgItem(g_hwnd, channel_on_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_off_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_lbl_high_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_lbl_medium_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_lbl_low_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_lbl_off_id(index)), NULL, FALSE);

    /* Card border reads output_on too now (rounded + lit border while
     * on) - only repaint it on the one field it actually depends on. */
    if (!cache->valid || cache->output_on != ch->output_on) {
        InvalidateRect(g_card_panel[index], NULL, FALSE);
    }

    cache->valid = true;
    cache->busy = ch->busy;
    cache->output_on = ch->output_on;
    cache->level = ch->level;
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

    /* App header bar: "Connection & Settings" heading (icon + title,
     * same as it had back when this section lived in the sidebar), then
     * its controls stacked as separate rows the same way they were in
     * the sidebar - lengthy, not widy, rather than one row spread thin
     * across the full header width. Panel itself still spans the full
     * width (same 6px top margin and 8px gap-before-content as every
     * other panel-to-panel spacing below) - the content just doesn't
     * try to fill it. */
    g_header_panel = add_panel(hwnd, SIDEBAR_X, 6, CLIENT_WIDTH - 2 * SIDEBAR_X, HEADER_H);

    /* Right-aligned like Amplifier Temperature, tucked right up against
     * it (24px gap, matching the tighter spacing used everywhere else
     * here) instead of floating apart with a big gap between them -
     * reads as one grouped pair anchored to the header's right edge. */
    add_header_icon(hwnd, 719, 16, ICON_PLUG);
    add_header(hwnd, "Connection && Settings", 737, 16, 260, 18);

    add_ctrl(hwnd, "STATIC", "Port:", SS_LEFT, 719, 40, 32, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 753, 38, 112, 160, IDC_PORT_COMBO);
    add_ctrl(hwnd, "BUTTON", "Refresh", BS_OWNERDRAW | WS_TABSTOP, 871, 38, 56, 22, IDC_REFRESH_BTN);
    add_ctrl(hwnd, "BUTTON", "Connect", BS_OWNERDRAW | WS_TABSTOP, 931, 38, 66, 22, IDC_CONNECT_BTN);
    add_ctrl(hwnd, "STATIC", "Disconnected", SS_LEFT, 719, 64, 290, 16, IDC_CONN_STATUS_LBL);

    add_ctrl(hwnd, "STATIC", "Baud:", SS_LEFT, 719, 88, 34, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 755, 86, 90, 140, IDC_BAUD_COMBO);
    add_ctrl(hwnd, "STATIC", "Data Bits:", SS_LEFT, 719, 112, 60, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 783, 110, 45, 100, IDC_DATABITS_COMBO);
    add_ctrl(hwnd, "STATIC", "Parity:", SS_LEFT, 839, 112, 40, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 881, 110, 70, 100, IDC_PARITY_COMBO);

    /* Amplifier Temperature, right-aligned in the same header bar
     * rather than below it in the sidebar - same row shape as
     * Connection & Settings, just anchored to the header's right edge
     * instead of sitting bunched up next to it. */
    add_header_icon(hwnd, 1033, 16, ICON_WAVE);
    add_header(hwnd, "Amplifier Temperature", 1051, 16, 260, 18);
    add_ctrl(hwnd, "STATIC", "Port:", SS_LEFT, 1033, 40, 32, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 1067, 38, 90, 160, IDC_SENSOR_PORT_COMBO);
    add_ctrl(hwnd, "BUTTON", "Refresh", BS_OWNERDRAW | WS_TABSTOP, 1161, 38, 56, 22, IDC_SENSOR_REFRESH_BTN);
    add_ctrl(hwnd, "BUTTON", "Connect", BS_OWNERDRAW | WS_TABSTOP, 1221, 38, 66, 22, IDC_SENSOR_CONNECT_BTN);
    /* 6 physical sensors scanning the rack area, each at its own
     * address (see UNIT_TEMP_ADDR) - not one per RF channel. This
     * status/readout is the average across whichever of the 6 currently
     * have a reading. (The gradient gauge bar that used to sit here is
     * gone - dropped for now, something else is going in its place
     * later.) */
    add_ctrl(hwnd, "STATIC", "Disconnected", SS_LEFT, 1033, 64, 270, 16, IDC_SENSOR_STATUS_LBL);
    add_ctrl(hwnd, "STATIC", "-", SS_LEFT | SS_NOPREFIX, 1033, 86, 260, 20, IDC_SENSOR_TEMP_LBL);
    /* One small rounded "mini card" per physical sensor unit (address +
     * live reading), 3 columns x 2 rows instead of a plain text list -
     * see sensor_chip_subclass_proc(). */
    {
        int chip;
        for (chip = 0; chip < SENSOR_MAX_UNITS; chip++) {
            int col = chip % 3;
            int row = chip / 3;
            int cx = 1033 + col * (88 + 6);
            int cy = 108 + row * (36 + 6);
            g_sensor_chip[chip] = add_sensor_chip(hwnd, cx, cy, 88, 36, chip);
        }
    }
    add_ctrl(hwnd, "STATIC", "", SS_LEFT | SS_NOPREFIX, 1033, 196, 190, 16, IDC_KILL_STATUS_LBL);
    add_ctrl(hwnd, "BUTTON", "Reset", BS_OWNERDRAW | WS_TABSTOP, 1229, 194, 80, 22, IDC_KILL_RESET_BTN);
    ShowWindow(GetDlgItem(hwnd, IDC_KILL_STATUS_LBL), SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_KILL_RESET_BTN), SW_HIDE);

    /* Sidebar: one tall box - top part empty (reserved for other
     * features), Activity Log below that in the SAME box, not a
     * separate panel. Connection & Settings and Amplifier Temperature
     * moved up into the header above. */
    g_sidebar_panel = add_panel(hwnd, SIDEBAR_X, CONTENT_TOP, SIDEBAR_W, LOG_PANEL_Y + LOG_PANEL_H - CONTENT_TOP);

    add_header_icon(hwnd, 22, LOG_PANEL_Y + 10, ICON_LIST);
    add_header(hwnd, "Activity Log", 40, LOG_PANEL_Y + 10, 200, 18);
    /* Right edge of both the Clear button and the listbox is pinned to
     * the same margin (12px in from the panel's own right edge,
     * matching the 12px left margin: content starts at x=22, panel at
     * SIDEBAR_X=10) - they used to use different margins, leaving the
     * listbox 10px short of the button above it. */
    add_ctrl(hwnd, "BUTTON", "Clear", BS_OWNERDRAW | WS_TABSTOP,
             SIDEBAR_X + SIDEBAR_W - 12 - 60, LOG_PANEL_Y + 8, 60, 20, IDC_LOG_CLEAR_BTN);
    add_ctrl(hwnd, "LISTBOX", NULL, LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP | WS_BORDER,
             22, LOG_PANEL_Y + 34, SIDEBAR_W + SIDEBAR_X - 34, LOG_PANEL_H - 46, IDC_LOG_LISTBOX);

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
    PLACE(g_card_header[index], x + SX(26), y + SY(6), SX(170), SY(16));

    PLACE(GetDlgItem(hwnd, channel_mode_id(index)), x + SX(8), y + SY(24), SX(82), 100);
    PLACE(GetDlgItem(hwnd, channel_set_id(index)), x + SX(94), y + SY(24), SX(40), SY(18));
    PLACE(GetDlgItem(hwnd, channel_on_id(index)), x + SX(8), y + SY(44), SX(60), SY(18));
    PLACE(GetDlgItem(hwnd, channel_off_id(index)), x + SX(72), y + SY(44), SX(60), SY(18));
    PLACE(GetDlgItem(hwnd, channel_status_id(index)), x + SX(8), y + SY(64), SX(130), SY(14));

    PLACE(GetDlgItem(hwnd, channel_track_id(index)), x + SX(148), y + SY(24), SX(22), SY(72));
    PLACE(GetDlgItem(hwnd, channel_lbl_high_id(index)), x + SX(174), y + SY(24), SX(44), SY(14));
    PLACE(GetDlgItem(hwnd, channel_lbl_medium_id(index)), x + SX(174), y + SY(42), SX(44), SY(14));
    PLACE(GetDlgItem(hwnd, channel_lbl_low_id(index)), x + SX(174), y + SY(60), SX(44), SY(14));
    PLACE(GetDlgItem(hwnd, channel_lbl_off_id(index)), x + SX(174), y + SY(78), SX(44), SY(14));

#undef SX
#undef SY
#undef PLACE
}

/* Recomputes the whole layout for a new client size: only the header
 * bar and sidebar panel stretch horizontally to fill the wider client
 * area - the 16 cards stay fixed at the designed CARD_W x CARD_H no
 * matter how big the window gets (maximized/fullscreen included).
 * Extra window space just stays empty background rather than growing
 * the cards - keeps the grid compact and readable on a large monitor
 * instead of every card ballooning to fill it. (Cards used to grow to
 * fill the available space; that's what was making them look oversized
 * at fullscreen - removed.) Never shrinks below the designed CARD_W x
 * CARD_H (see WM_GETMINMAXINFO, which stops the window itself getting
 * that small). */
static void relayout_for_size(HWND hwnd, int client_w, int client_h) {
    int i;
    (void)client_h; /* cards no longer grow to fill vertical space, so the
                      * new client height doesn't factor into this layout -
                      * kept as a parameter since callers still have it and
                      * WM_SIZE's (w, h) pairing reads naturally at call sites. */

    if (!g_layout_ready) {
        return;
    }

    MoveWindow(g_header_panel, SIDEBAR_X, 6, client_w - 2 * SIDEBAR_X, HEADER_H, FALSE);
    MoveWindow(g_sidebar_panel, SIDEBAR_X, CONTENT_TOP, SIDEBAR_W, LOG_PANEL_Y + LOG_PANEL_H - CONTENT_TOP, FALSE);

    MoveWindow(GetDlgItem(hwnd, IDC_LOG_LISTBOX), 22, LOG_PANEL_Y + 34, SIDEBAR_W + SIDEBAR_X - 34, LOG_PANEL_H - 46, FALSE);
    MoveWindow(GetDlgItem(hwnd, IDC_LOG_CLEAR_BTN), SIDEBAR_X + SIDEBAR_W - 12 - 60, LOG_PANEL_Y + 8, 60, 20, FALSE);

    for (i = 0; i < MAX_CHANNELS; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        int card_x = GRID_LEFT + col * (CARD_W + CARD_GAP);
        int card_y = CONTENT_TOP + row * (CARD_H + CARD_GAP);
        position_channel_card(hwnd, i, card_x, card_y, CARD_W, CARD_H);
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
                for (addr_i = 0; addr_i < SENSOR_MAX_UNITS; addr_i++) {
                    sensor_set_unit_address(&g_sensor, addr_i, UNIT_TEMP_ADDR[addr_i]);
                }
            }
            SetTimer(hwnd, ID_POLL_TIMER, 100, NULL);
            ui_refresh_all_channels();
            ui_refresh_sensor();
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
                    } else if (offset == IDC_CH_STATUS_OFFSET && code == STN_CLICKED) {
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
                float avg_c;
                bool has_avg = sensor_average_temperature(&g_sensor, &avg_c);
                COLORREF col = !sensor_is_connected(&g_sensor) ? COLOR_APP_DISCONNECTED
                             : has_avg ? COLOR_APP_CONNECTED
                             : COLOR_APP_ACCENT;
                SetTextColor(hdc, col);
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)g_brush_panel;
            }
            if (ctl == GetDlgItem(hwnd, IDC_SENSOR_TEMP_LBL)) {
                float avg_c;
                bool has_avg = sensor_average_temperature(&g_sensor, &avg_c);
                SetTextColor(hdc, has_avg ? temp_band_color(avg_c) : COLOR_APP_MUTED);
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
                    COLORREF col = g_kill_switch_tripped[idx] ? COLOR_APP_DISCONNECTED /* red - click to reset */
                                  : ch->busy ? COLOR_APP_ACCENT
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
            }
            {
                HFONT ctl_font = (HFONT)SendMessageA(ctl, WM_GETFONT, 0, 0);
                if (ctl_font == g_header_font) {
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
