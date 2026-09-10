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
#include <stdlib.h>
#include "resource.h"
#include "connection.h"
#include "channels.h"
#include "sensor.h"

#define CLIENT_WIDTH  1343
#define CLIENT_HEIGHT 702

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
#define HEADER_H     180 /* was 168 - Connection & Settings split Port
                            * from Refresh/Connect back onto separate
                            * rows and gained extra gap before Data
                            * Bits/Parity, so it's the taller card again
                            * (content bottoms out ~y=175) */

#define CONTENT_TOP  194 /* shifts down by the same 12px HEADER_H grew,
                            * keeping the usual 8px gap below the panel */

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
#define COLOR_APP_SHADOW    RGB(14, 15, 17)

/* "Direction B" from the UI design proposal, applied app-wide: rounded
 * corners everywhere instead of the old chamfer (panel_subclass_proc
 * below), plus a brighter/thicker border on a channel card while that
 * channel's output is on, standing in for a glow - GDI has no blurred
 * box-shadow, so a solid highlight border is the cheap approximation.
 * Same reasoning for PANEL/CARD_SHADOW_PX: a sliver of dark shadow
 * shown along the bottom-right, inset from the control's own bounds
 * rather than bleeding outside them (no layout changes needed). */
#define PANEL_CORNER_DIAMETER 24 /* header bar, sidebar */
#define CARD_CORNER_DIAMETER 16  /* the smaller 16 channel cards */
#define BTN_CORNER_DIAMETER 16   /* every owner-draw button (Set, ON/OFF,
                                   * Connect, Refresh, Clear, Reset) - big
                                   * enough to actually read as rounded on
                                   * an 18-24px-tall button, not just a
                                   * couple of softened pixels at the tips */
#define CARD_BORDER_ON_WIDTH 2
#define PANEL_SHADOW_PX 5
#define CARD_SHADOW_PX 3
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
#define CARD_H 110 /* was 102 - grown by what HEADER_H gave up above */
#define CARD_GAP 12 /* was 8 - "Direction B" wants more generous spacing */
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
static WNDPROC g_combo_edit_orig_proc;
static bool g_combo_edit_no_recurse;
static HBRUSH g_brush_panel;
static HBRUSH g_brush_page;
static HBRUSH g_brush_field;
static HBRUSH g_brush_accent;
static HBRUSH g_brush_accent_dis;
static HBRUSH g_brush_dot;
static HBRUSH g_brush_connected;
static HBRUSH g_brush_disconnected;
static HBRUSH g_brush_level_medium; /* matches ch_gauge_stop_color(LEVEL_MEDIUM) */
static HBRUSH g_brush_level_off;    /* matches ch_gauge_stop_color(LEVEL_OFF) */
static HBRUSH g_brush_shadow;
static const COLORREF g_shadow_color = COLOR_APP_SHADOW;
static HBRUSH g_brush_dot_pattern; /* tiled DOT_GRID_SPACING x DOT_GRID_SPACING bitmap brush */
static HBITMAP g_dot_pattern_bmp;

static Connection g_conn;
static Sensor g_sensor;
static bool g_kill_switch_tripped[MAX_CHANNELS];

/* Bulk Actions selection - click a card to toggle it in/out, then the
 * Bulk Actions bar applies to every selected channel at once. */
static bool g_channel_selected[MAX_CHANNELS];

/* Off by default - single-channel operation is the normal way to use
 * this app. IDC_BULK_TOGGLE_BTN flips this on/off (see
 * set_bulk_select_mode()); only then do the per-card checkboxes and the
 * rest of the Bulk Actions row become visible/clickable. */
static bool g_bulk_select_mode;

/* The Bulk Actions mode combo's readonly-theming overlay windows (see
 * make_combo_readonly_ex) - separate sibling windows, not children of
 * the combo, so ShowWindow on the combo alone leaves them on screen.
 * Tracked here so set_bulk_select_mode() can show/hide them too. */
static HWND g_bulk_combo_overlays[5];

/* Spectrum panel state - true shows the all-16 overview grid (the
 * default), false shows one channel's trace full-size, with
 * g_spectrum_unit (0..MAX_CHANNELS-1) picking which. */
static bool g_spectrum_show_all = true;
static int g_spectrum_unit;

/* Handles needed to reposition things on WM_SIZE that don't otherwise
 * have a retrievable control ID (channel_*_id() covers everything else
 * per-card - GetDlgItem() finds those directly). */
static HWND g_header_panel;
static HWND g_sidebar_panel;
static HWND g_card_panel[MAX_CHANNELS];
static HWND g_card_icon[MAX_CHANNELS];
static HWND g_card_header[MAX_CHANNELS];
static HWND g_card_mode_lbl[MAX_CHANNELS]; /* muted mode name next to "Unit N",
                                              * matching the design mockup's
                                              * card header - reflects the
                                              * applied mode (ch->mode), not
                                              * the dropdown's uncommitted
                                              * selection */
/* Each channel's mode combo's readonly-theming overlay windows (see
 * make_combo_readonly_ex) - separate sibling windows, not children of
 * the combo, so a bare InvalidateRect on the combo itself doesn't touch
 * them. Not tracking these was the actual cause of a reported "white
 * dropdown" - selecting/deselecting a card invalidates+repaints it via
 * ui_invalidate_card(), and without these in that list, repaint timing
 * could leave the overlay unpainted, exposing the native COMBOBOX's own
 * white arrow/bevel underneath instead of the dark themed one. */
static HWND g_card_combo_overlays[MAX_CHANNELS][5];
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

/* Dropdowns (mode/port/baud/data bits/parity) are CBS_DROPDOWN, not
 * CBS_DROPDOWNLIST - CBS_DROPDOWNLIST's closed display has no child
 * window backing it, so nothing can recolor it short of full owner-
 * draw, which turned out to be a known-flaky corner of Win32 (see git
 * history - first-click-doesn't-open, repaint gaps after the popup
 * closes). CBS_DROPDOWN backs the closed display with a real EDIT
 * control, which gets dark-themed for free - but via WM_CTLCOLORSTATIC,
 * not WM_CTLCOLOREDIT: an edit control switches to sending
 * WM_CTLCOLORSTATIC once it's read-only, confirmed by testing (the
 * WM_CTLCOLOREDIT case here never fired at all; WM_CTLCOLORSTATIC's
 * combo-edit-child check does). All native painting either way, no
 * owner-draw. EM_SETREADONLY blocks typing into it while leaving
 * click-to-open and list selection working normally.
 *
 * The sunken 3D bevel and dropdown-arrow button are covered now too -
 * see combo_edge_subclass_proc/combo_arrow_subclass_proc below - but
 * it took five attempts to get there: recoloring the combo box's own
 * WM_NCPAINT did nothing visible, same for its EDIT child's, and a
 * ring-shaped SetWindowRgn overlay sibling filled the whole control
 * solid blue instead of just a ring (the region didn't actually
 * restrict the paint). What finally worked: plain ordinary opaque
 * rectangles - one for the arrow button, four thin ones (top/bottom/
 * left/right) for the border - never a region trick. Confirmed the
 * border needed to be 4px, not 2px, by testing 2/4/8px directly. */
/* A plain rectangular overlay this time, not a region trick - the
 * SetWindowRgn ring attempt failed because the region didn't actually
 * restrict painting the way it should have; a full, ordinary opaque
 * rectangle sized to just the dropdown-arrow button avoids that
 * failure mode entirely (nothing relies on partial occlusion). Same
 * "sibling created after, so it's on top" positioning, WS_DISABLED so
 * clicks fall through to the real arrow button underneath (disabled
 * windows are skipped during hit-testing) to actually open the list. */
static LRESULT CALLBACK combo_arrow_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        HPEN pen, old_pen;
        HBRUSH old_brush;
        POINT tri[3];
        int cx, cy;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_brush_field);

        cx = (rc.left + rc.right) / 2;
        cy = (rc.top + rc.bottom) / 2;
        tri[0].x = cx - 4; tri[0].y = cy - 2;
        tri[1].x = cx + 4; tri[1].y = cy - 2;
        tri[2].x = cx;     tri[2].y = cy + 3;

        pen = CreatePen(PS_SOLID, 1, COLOR_APP_HEADER);
        old_pen = (HPEN)SelectObject(hdc, pen);
        old_brush = (HBRUSH)SelectObject(hdc, g_brush_accent);
        Polygon(hdc, tri, 3);
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

static HWND add_combo_arrow(HWND parent, int x, int y, int w, int h) {
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT | WS_DISABLED, x, y, w, h, 0);
    if (!ctrl) {
        return NULL;
    }
    if (!g_panel_orig_proc) {
        g_panel_orig_proc = (WNDPROC)GetWindowLongPtrA(ctrl, GWLP_WNDPROC);
    }
    SetWindowLongPtrA(ctrl, GWLP_WNDPROC, (LONG_PTR)combo_arrow_subclass_proc);
    return ctrl;
}

/* Same "plain opaque rectangle, not a region trick" pattern as the
 * arrow overlay - four separate thin strips (top/bottom/left/right)
 * instead of one ring-shaped SetWindowRgn window, which is the piece
 * that failed before. Each strip is a full ordinary rect, so there's
 * no region-clipping to get wrong. */
#define COMBO_BORDER_PX 4 /* the native sunken bevel is thicker than it
                             * looks - 2px left a visible white sliver,
                             * confirmed by testing at 2/4/8px */
static LRESULT CALLBACK combo_edge_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        /* Same dark field color as the text/arrow area, not a
         * separate border color - reads as one seamless dark box with
         * no visible frame, per direct request, instead of a themed
         * border. Still covers the native sunken bevel underneath. */
        FillRect(hdc, &rc, g_brush_field);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

static HWND add_combo_edge(HWND parent, int x, int y, int w, int h) {
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT | WS_DISABLED, x, y, w, h, 0);
    if (!ctrl) {
        return NULL;
    }
    if (!g_panel_orig_proc) {
        g_panel_orig_proc = (WNDPROC)GetWindowLongPtrA(ctrl, GWLP_WNDPROC);
    }
    SetWindowLongPtrA(ctrl, GWLP_WNDPROC, (LONG_PTR)combo_edge_subclass_proc);
    return ctrl;
}

/* Extra px of arrow-overlay width beyond SM_CXVSCROLL, to also cover a
 * native separator/bevel between the edit field and the dropdown button
 * that SM_CXVSCROLL alone doesn't account for - left a ~3px white sliver
 * uncovered, confirmed by pixel-sampling a screenshot. (Tried using
 * GetComboBoxInfo's cbi.rcButton instead of guessing - its rect came back
 * wrong under Wine and blew up several overlays to cover the whole
 * control; reverted, back to the SM_CXVSCROLL + padding approach.) */
#define ARROW_OVERLAY_PAD_PX 4

/* Picking an item from the list (or the initial CB_SETCURSEL at
 * creation) makes the combo's edit child select its entire displayed
 * text internally, same as any EDIT control after SetWindowText+
 * EM_SETSEL(0,-1) - invisible while unfocused (EDIT hides selection on
 * WM_KILLFOCUS by default), but the instant the combo has focus (which
 * it already does the moment you click its arrow to open the list) it
 * renders as a solid system-blue bar over the text, stomping the dark
 * theme - confirmed by screenshot, reproduced by opening any combo's
 * dropdown. Intercepting EM_SETSEL and immediately collapsing it back
 * to no-selection (guarded against re-entering itself) kills the
 * highlight before it's ever painted, regardless of whether it came
 * from the initial value or a later pick. */
static LRESULT CALLBACK combo_edit_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    LRESULT result = CallWindowProcA(g_combo_edit_orig_proc, hwnd, msg, wParam, lParam);
    if (msg == EM_SETSEL && !g_combo_edit_no_recurse) {
        g_combo_edit_no_recurse = true;
        SendMessageA(hwnd, EM_SETSEL, (WPARAM)-1, 0);
        g_combo_edit_no_recurse = false;
    }
    return result;
}

/* overlays_out, when non-NULL, receives the 5 overlay windows created
 * (arrow, top, bottom, left, right) - needed by callers that must
 * show/hide a combo dynamically later (ShowWindow on the combo itself
 * doesn't touch these separate sibling overlay windows, since they're
 * independent top-level siblings, not children of the combo). */
static void make_combo_readonly_ex(HWND combo, HWND overlays_out[5]) {
    COMBOBOXINFO cbi;
    RECT rc;
    HWND parent;
    int arrow_w, w, h;
    HWND arrow, edge_top, edge_bottom, edge_left, edge_right;

    cbi.cbSize = sizeof(cbi);
    if (GetComboBoxInfo(combo, &cbi) && cbi.hwndItem) {
        SendMessageA(cbi.hwndItem, EM_SETREADONLY, TRUE, 0);
        if (!g_combo_edit_orig_proc) {
            g_combo_edit_orig_proc = (WNDPROC)GetWindowLongPtrA(cbi.hwndItem, GWLP_WNDPROC);
        }
        SetWindowLongPtrA(cbi.hwndItem, GWLP_WNDPROC, (LONG_PTR)combo_edit_subclass_proc);
    }

    parent = GetParent(combo);
    GetWindowRect(combo, &rc);
    MapWindowPoints(HWND_DESKTOP, parent, (POINT *)&rc, 2);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;

    arrow_w = GetSystemMetrics(SM_CXVSCROLL) + ARROW_OVERLAY_PAD_PX;
    arrow = add_combo_arrow(parent, rc.right - arrow_w, rc.top, arrow_w, h);

    edge_top = add_combo_edge(parent, rc.left, rc.top, w, COMBO_BORDER_PX);
    edge_bottom = add_combo_edge(parent, rc.left, rc.bottom - COMBO_BORDER_PX, w, COMBO_BORDER_PX);
    edge_left = add_combo_edge(parent, rc.left, rc.top, COMBO_BORDER_PX, h);
    edge_right = add_combo_edge(parent, rc.right - COMBO_BORDER_PX, rc.top, COMBO_BORDER_PX, h);

    if (overlays_out) {
        overlays_out[0] = arrow;
        overlays_out[1] = edge_top;
        overlays_out[2] = edge_bottom;
        overlays_out[3] = edge_left;
        overlays_out[4] = edge_right;
    }
}

static void make_combo_readonly(HWND combo) {
    make_combo_readonly_ex(combo, NULL);
}

/* Rounded-corner panel painting (header bar, sidebar) - same subclass
 * pattern as the channel cards' card_panel_subclass_proc below, just
 * with no per-item on/off state to light the border with. */
static LRESULT CALLBACK panel_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        HBRUSH old_brush;
        HPEN pen, old_pen;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);

        /* Faked elevation: a dark shadow shape filling the whole rect,
         * then the real panel body drawn shrunk into its top-left,
         * leaving a few px of shadow showing along the bottom-right -
         * same "no true blur in GDI" approximation as the card glow,
         * but doesn't need any layout/window-size changes since it
         * stays entirely within the control's existing bounds. */
        old_brush = (HBRUSH)SelectObject(hdc, g_brush_shadow);
        pen = CreatePen(PS_SOLID, 1, g_shadow_color);
        old_pen = (HPEN)SelectObject(hdc, pen);
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom,
                  PANEL_CORNER_DIAMETER, PANEL_CORNER_DIAMETER);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);

        SelectObject(hdc, g_brush_panel);
        pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
        old_pen = (HPEN)SelectObject(hdc, pen);
        RoundRect(hdc, rc.left, rc.top, rc.right - PANEL_SHADOW_PX, rc.bottom - PANEL_SHADOW_PX,
                  PANEL_CORNER_DIAMETER, PANEL_CORNER_DIAMETER);

        SelectObject(hdc, old_pen);
        DeleteObject(pen);
        SelectObject(hdc, old_brush);

        /* A small circular "punch" cut out of each of the whole header
         * bar's 4 corners, sitting on the panel's own rounded-corner
         * ARC (not the sharp rectangular corner point the panel's
         * RoundRect calls never actually draw into) so it reads as a
         * notch cut into the bar's rounded edge, the way a die-cut hole
         * in a real card/ticket corner would - filled with the actual
         * tiled dot-pattern brush (not a flat color) so the page's own
         * dot texture visibly continues behind the hole, rather than a
         * plain disc sitting on top of the bar.
         *
         * Centering it exactly on the sharp corner (rc.left, rc.top
         * etc.) was tried first and was wrong two ways: visually, most
         * of the circle then sits in the square corner area the
         * rounded panel never fills in the first place, so it read as
         * a free-floating dot outside the panel rather than a notch
         * cut into it - and separately, Wine's Ellipse() with a
         * pattern-brush fill silently draws nothing at all once part
         * of its bounding box goes negative/out-of-window (confirmed by
         * testing: swapping to a plain solid-color fill made all 4
         * corners render fine at that same position, so a bounding box
         * that leaves the window is what broke pattern-brush
         * specifically, not a real "off"). Insetting onto the rounded
         * arc itself fixes both at once - r/sqrt(2)-ish along the
         * corner's own 45-degree diagonal keeps the whole ellipse
         * on-panel while still visibly overlapping the curve.
         *
         * Header only, not the sidebar panel below (same subclass
         * proc, but this is scoped to g_header_panel specifically) -
         * direct request was for the whole header, not the Bulk
         * Actions card nested inside it. */
        if (hwnd == g_header_panel) {
            const int r = 7;
            const int inset = 8; /* ~PANEL_CORNER_DIAMETER/2 * (1 - 1/sqrt(2)) */
            POINT corners[4];
            POINT old_org;
            int ci;
            corners[0].x = rc.left + inset;  corners[0].y = rc.top + inset;
            corners[1].x = rc.right - PANEL_SHADOW_PX - inset;  corners[1].y = rc.top + inset;
            corners[2].x = rc.left + inset;  corners[2].y = rc.bottom - PANEL_SHADOW_PX - inset;
            corners[3].x = rc.right - PANEL_SHADOW_PX - inset;  corners[3].y = rc.bottom - PANEL_SHADOW_PX - inset;

            /* This panel's own client (0,0) sits at (SIDEBAR_X, 6) in
             * the main window - phase-align the pattern brush to that
             * offset so the dots inside each hole are continuous with
             * the real background dots just outside the panel, instead
             * of the tile restarting at this window's own (0,0) and
             * visibly seaming against the surrounding pattern. */
            SetBrushOrgEx(hdc, -(SIDEBAR_X % DOT_GRID_SPACING), -(6 % DOT_GRID_SPACING), &old_org);
            old_brush = (HBRUSH)SelectObject(hdc, g_brush_dot_pattern ? g_brush_dot_pattern : g_brush_page);
            /* A visible ring around the hole (not NULL_PEN/borderless) -
             * a real die-cut hole has a defined edge, not just a patch
             * of texture with no boundary. */
            pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
            old_pen = (HPEN)SelectObject(hdc, pen);
            for (ci = 0; ci < 4; ci++) {
                Ellipse(hdc, corners[ci].x - r, corners[ci].y - r,
                        corners[ci].x + r, corners[ci].y + r);
            }
            SelectObject(hdc, old_pen);
            DeleteObject(pen);
            SelectObject(hdc, old_brush);
            SetBrushOrgEx(hdc, old_org.x, old_org.y, NULL);
        }

        /* Bulk Actions gets its own inset card within this same header
         * panel - drawn right here, as a second RoundRect in the same
         * WM_PAINT call, NOT as a second overlapping WS_CLIPSIBLINGS
         * sibling window (that was tried first: two same-class
         * WS_CLIPSIBLINGS panels fully overlapping each other clip each
         * other's drawable region down to nothing, since WS_CLIPSIBLINGS
         * excludes area covered by ANY overlapping sibling regardless of
         * z-order - confirmed by testing, it rendered completely blank,
         * no border, no shadow at all). Drawing it inline here instead
         * sidesteps sibling clipping entirely - it's the same window,
         * same HDC, just a second shape. Coordinates are relative to
         * this panel's own top-left (only g_header_panel is at
         * SIDEBAR_X=10, y=6, so these map to the absolute (454, 14) -
         * (894, 170) region build_controls() lays the Bulk Actions
         * controls out in). */
        if (hwnd == g_header_panel) {
            RECT brc;
            brc.left = 444;
            brc.top = 8;
            brc.right = 884;
            brc.bottom = 164;

            /* CARD_CORNER_DIAMETER/CARD_SHADOW_PX, not the PANEL_*
             * constants used above for the outer header panel itself -
             * direct request was for this to match a channel card's own
             * look exactly (smaller, tighter corner radius and a
             * shallower shadow than the bigger header/sidebar panels
             * use), not just "a border of some kind". */
            old_brush = (HBRUSH)SelectObject(hdc, g_brush_shadow);
            pen = CreatePen(PS_SOLID, 1, g_shadow_color);
            old_pen = (HPEN)SelectObject(hdc, pen);
            RoundRect(hdc, brc.left, brc.top, brc.right, brc.bottom,
                      CARD_CORNER_DIAMETER, CARD_CORNER_DIAMETER);
            SelectObject(hdc, old_pen);
            DeleteObject(pen);

            SelectObject(hdc, g_brush_panel);
            pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
            old_pen = (HPEN)SelectObject(hdc, pen);
            RoundRect(hdc, brc.left, brc.top, brc.right - CARD_SHADOW_PX, brc.bottom - CARD_SHADOW_PX,
                      CARD_CORNER_DIAMETER, CARD_CORNER_DIAMETER);
            SelectObject(hdc, old_pen);
            DeleteObject(pen);
            SelectObject(hdc, old_brush);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

static HWND add_panel(HWND parent, int x, int y, int w, int h) {
    /* WS_CLIPSIBLINGS - without it, this panel's own background repaint
     * isn't clipped away from higher-z-order sibling controls sitting
     * on top of it, so it can paint straight over them (see
     * add_card_panel()'s comment - same bug, same fix, just the header/
     * sidebar panels instead of a channel card). Only added here (and
     * on add_card_panel below), not app-wide - a blanket add broke
     * painting everywhere else, this app has too many controls placed
     * at genuinely overlapping positions expecting no clipping. */
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT | WS_CLIPSIBLINGS, x, y, w, h, 0);
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
 * live-read pattern as sensor_chip_subclass_proc. Also the Bulk Actions
 * click target: SS_NOTIFY sends STN_CLICKED to the parent on a click
 * anywhere on the card's background (not on one of the real buttons/
 * combo/gauge sitting on top of it - those get the click first, same as
 * any overlapping sibling), toggling that channel selected/deselected -
 * see the WM_COMMAND handling below and g_channel_selected. */
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
        bool selected = (index >= 0 && index < MAX_CHANNELS) && g_channel_selected[index];

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);

        old_brush = (HBRUSH)SelectObject(hdc, g_brush_shadow);
        pen = CreatePen(PS_SOLID, 1, g_shadow_color);
        old_pen = (HPEN)SelectObject(hdc, pen);
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom,
                  CARD_CORNER_DIAMETER, CARD_CORNER_DIAMETER);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);

        /* Plain fill with no outline normally (NULL_PEN, not a same-
         * color pen, so RoundRect doesn't draw an edge at all) - a
         * selected card gets a real accent-colored stroke instead, the
         * only visual cue for "this card is in the Bulk Actions
         * selection". */
        SelectObject(hdc, g_brush_panel);
        if (selected) {
            pen = CreatePen(PS_SOLID, 2, COLOR_APP_HEADER);
            old_pen = (HPEN)SelectObject(hdc, pen);
        } else {
            pen = NULL;
            old_pen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        }

        RoundRect(hdc, rc.left, rc.top, rc.right - CARD_SHADOW_PX, rc.bottom - CARD_SHADOW_PX,
                  CARD_CORNER_DIAMETER, CARD_CORNER_DIAMETER);

        SelectObject(hdc, old_pen);
        if (pen) {
            DeleteObject(pen);
        }
        SelectObject(hdc, old_brush);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

static HWND add_card_panel(HWND parent, int x, int y, int w, int h, int index) {
    /* WS_CLIPSIBLINGS - see add_panel()'s comment above. This is the
     * control that actually caused the reported bug: its repaint is
     * invalidated every time this channel's output_on flips (see
     * ui_refresh_channel()), and without this flag that repaint drew
     * straight over the title/mode label/mode combo/Set button sitting
     * on top of it, with nothing telling them to repaint themselves
     * afterward - "Unit N" and its mode row would just go blank the
     * next time the channel turned on or off. SS_NOTIFY so it can send
     * STN_CLICKED for Bulk Actions selection (see the proc above). */
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT | SS_NOTIFY | WS_CLIPSIBLINGS, x, y, w, h, 0);
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

/* A full pill (corner diameter = control height) filled with a
 * horizontal gradient from the dark field color into whatever color
 * the state is, clipped to the pill shape, then a thin matching
 * border and centered text on top. Shared by the sensor status pill
 * and the average-temperature pill below it.
 *
 * Text color is picked from the blend at the CENTER of the gradient
 * (roughly where the text itself sits), not hardcoded - temp_band_color()
 * returns pure white for cold readings, and near-white/light text on
 * top of that was invisible. Cheap perceptual luminance on the halfway
 * blend of field-bg and grad_to decides light-text-on-dark vs
 * dark-text-on-light. */
static void paint_gradient_pill(HDC hdc, RECT rc, COLORREF grad_to, const char *text) {
    HRGN clip;
    int diameter = rc.bottom - rc.top;
    HPEN pen, old_pen;
    HFONT old_font;
    COLORREF mid, text_color;
    int luma;

    clip = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, diameter, diameter);
    SelectClipRgn(hdc, clip);
    gradient_fill_rect(hdc, rc, COLOR_APP_FIELD_BG, grad_to, false);
    SelectClipRgn(hdc, NULL);
    DeleteObject(clip);

    pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
    old_pen = (HPEN)SelectObject(hdc, pen);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, diameter, diameter);
    SelectObject(hdc, old_pen);
    DeleteObject(pen);

    mid = RGB((GetRValue(COLOR_APP_FIELD_BG) + GetRValue(grad_to)) / 2,
              (GetGValue(COLOR_APP_FIELD_BG) + GetGValue(grad_to)) / 2,
              (GetBValue(COLOR_APP_FIELD_BG) + GetBValue(grad_to)) / 2);
    luma = (GetRValue(mid) * 299 + GetGValue(mid) * 587 + GetBValue(mid) * 114) / 1000;
    text_color = (luma > 150) ? RGB(20, 21, 23) : COLOR_APP_TEXT;

    SetBkMode(hdc, TRANSPARENT);
    old_font = (HFONT)SelectObject(hdc, g_font);
    SetTextColor(hdc, text_color);
    DrawTextA(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old_font);
}

static LRESULT CALLBACK sensor_avg_pill_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        char text[32];
        float avg_c;
        bool has_avg = sensor_average_temperature(&g_sensor, &avg_c);
        COLORREF band = has_avg ? temp_band_color(avg_c) : COLOR_APP_MUTED;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        GetWindowTextA(hwnd, text, sizeof(text));
        paint_gradient_pill(hdc, rc, band, text);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

static HWND add_pill(HWND parent, LPCSTR text, int x, int y, int w, int h, int id, WNDPROC pill_proc) {
    HWND ctrl = add_ctrl(parent, "STATIC", text, SS_LEFT | SS_NOPREFIX, x, y, w, h, id);
    if (ctrl) {
        if (!g_panel_orig_proc) {
            g_panel_orig_proc = (WNDPROC)GetWindowLongPtrA(ctrl, GWLP_WNDPROC);
        }
        SetWindowLongPtrA(ctrl, GWLP_WNDPROC, (LONG_PTR)pill_proc);
    }
    return ctrl;
}

/* Small rounded "mini card" for one sensor unit's address + reading -
 * reads live off g_sensor each paint (unit_index stashed in
 * GWLP_USERDATA at creation) rather than being fed text, same pattern
 * as the other self-drawing gauges above. Muted "-" when that unit
 * doesn't have a reading yet. */
/* No more chip/badge box - just the address and reading as plain text,
 * with a thin line underneath standing in for an "is this address
 * actually reporting" indicator: dim/muted (near-invisible against the
 * panel) until that unit has a real reading, then lit in the same
 * color as the reading itself. */
/* Real hierarchy instead of two same-weight lines: the address is a
 * small muted label (secondary - it's fixed wiring, rarely what you're
 * scanning for), the reading is the bold, bigger, primary number - the
 * thing actually worth looking at. Same transparent/no-box, line-lights-
 * when-read approach as before, just with weight put where it belongs. */
static LRESULT CALLBACK sensor_chip_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc, addr_rc, val_rc, line_rc;
        int unit_index = (int)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        const SensorState *st = sensor_get_state(&g_sensor, unit_index);
        char addr_text[16];
        char val_text[16];
        HBRUSH line_brush;
        HFONT old_font;
        COLORREF val_color, line_color;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);

        /* Real fill, not transparent - this control sits well inside the
         * Amplifier Temperature panel, nowhere near its rounded corners,
         * so a flat g_brush_panel fill looks identical to "background
         * bleeds through" here, but actually clears old glyph pixels.
         * Without this, WM_ERASEBKGND returning 1 (and the caller's
         * InvalidateRect(..., FALSE) skipping erase too) meant a changed
         * reading's new digits painted directly over the old ones -
         * "27.0" -> "27.1" left a smeared double-exposure of both,
         * confirmed by a real-hardware screenshot. DT_NOCLIP on the value
         * text below means glyphs can extend past its own rect, so this
         * clears the whole control, not just the text sub-rects. */
        FillRect(hdc, &rc, g_brush_panel);
        SetBkMode(hdc, TRANSPARENT);

        wsprintfA(addr_text, "BAY %d", sensor_get_unit_address(&g_sensor, unit_index));
        addr_rc = rc;
        addr_rc.top += 3;
        addr_rc.bottom = addr_rc.top + 12;
        old_font = (HFONT)SelectObject(hdc, g_font);
        SetTextColor(hdc, COLOR_APP_MUTED);
        DrawTextA(hdc, addr_text, -1, &addr_rc, DT_CENTER | DT_SINGLELINE);
        SelectObject(hdc, old_font);

        /* Value box needs real height for g_header_font (bold, -13) -
         * the previous 10px box was shorter than the font's own line
         * height, so DrawTextA's default clipping (no DT_NOCLIP) cut
         * the bottom off every glyph, including the decimal point -
         * "27.0" rendered with no visible "." at all. DT_VCENTER now
         * too, so it's not relying on exact pixel accounting to look
         * right. */
        val_rc = rc;
        val_rc.top = addr_rc.bottom + 1;
        val_rc.bottom = rc.bottom - 6;
        if (st->has_reading) {
            wsprintfA(val_text, "%d.%d C", (int)st->temperature_c, (int)(st->temperature_c * 10) % 10);
            val_color = temp_band_color(st->temperature_c);
            line_color = val_color;
        } else {
            lstrcpynA(val_text, "-", (int)sizeof(val_text));
            val_color = COLOR_APP_MUTED;
            line_color = COLOR_APP_PANEL_BORDER; /* "off" - dim, barely there */
        }
        /* Regular weight, not g_header_font (bold) - looked cramped/
         * smudgy on real hardware ClearType at this size, even though
         * it looked fine in Wine testing. Hierarchy still comes from
         * color + the line indicator, doesn't need the bold too. */
        old_font = (HFONT)SelectObject(hdc, g_font);
        SetTextColor(hdc, val_color);
        DrawTextA(hdc, val_text, -1, &val_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        SelectObject(hdc, old_font);

        {
            int inset = (rc.right - rc.left) / 4; /* short, centered - not
                                                     * edge-to-edge, but
                                                     * wider than a bare
                                                     * accent tick */
            line_rc.left = rc.left + inset;
            line_rc.right = rc.right - inset;
            line_rc.bottom = rc.bottom - 1;
            line_rc.top = line_rc.bottom - 2; /* 2px lit, reads as a real
                                                 * indicator, not a hairline */
        }
        line_brush = CreateSolidBrush(line_color);
        FillRect(hdc, &line_rc, line_brush);
        DeleteObject(line_brush);

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

/* Defined below, once channel_mode_id()/channel_set_id()/etc. exist -
 * forward-declared here so conn_on_connected_changed() can gate every
 * channel control on the RS422 link the instant it changes. */
static void set_channel_controls_enabled(bool enabled);

static void conn_on_connected_changed(bool connected, void *ctx) {
    (void)ctx;
    SetDlgItemTextA(g_hwnd, IDC_CONN_STATUS_LBL, connected ? "Connected" : "Disconnected");
    EnableWindow(GetDlgItem(g_hwnd, IDC_CONNECT_BTN), TRUE);
    SetWindowTextA(GetDlgItem(g_hwnd, IDC_CONNECT_BTN), connected ? "Disconnect" : "Connect");
    InvalidateRect(GetDlgItem(g_hwnd, IDC_CONN_STATUS_LBL), NULL, TRUE);
    set_channel_controls_enabled(connected);
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
static int channel_select_id(int idx)     { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_SELECT_OFFSET; }
static int channel_status_id(int idx)     { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_STATUS_OFFSET; }
static int channel_track_id(int idx)      { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_TRACKBAR_OFFSET; }
static int channel_lbl_high_id(int idx)   { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_LBL_HIGH_OFFSET; }
static int channel_lbl_medium_id(int idx) { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_LBL_MEDIUM_OFFSET; }
static int channel_lbl_low_id(int idx)    { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_LBL_LOW_OFFSET; }
static int channel_lbl_off_id(int idx)    { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_LBL_OFF_OFFSET; }

/* Invalidates a card's background panel AND every one of its own
 * foreground siblings (title, mode label, mode combo, Set, ON, OFF,
 * status, gauge, tick labels) together, every time. WS_CLIPSIBLINGS on
 * the panel (see add_card_panel()) is supposed to keep its repaint from
 * touching them at all - and does most of the time - but it isn't
 * reliably enough to trust alone (confirmed: a card clicked for Bulk
 * Actions selection could still end up erased under Wine even with the
 * flag set). Explicitly telling every sibling to repaint alongside the
 * panel is the actually-guaranteed fix, independent of z-order/clipping
 * timing. Call this instead of invalidating g_card_panel[index] alone,
 * anywhere a card's panel needs to repaint. */
static void ui_invalidate_card(int index) {
    int oi;
    InvalidateRect(g_card_panel[index], NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_select_id(index)), NULL, FALSE);
    InvalidateRect(g_card_icon[index], NULL, FALSE);
    InvalidateRect(g_card_header[index], NULL, FALSE);
    InvalidateRect(g_card_mode_lbl[index], NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_mode_id(index)), NULL, FALSE);
    for (oi = 0; oi < 5; oi++) {
        if (g_card_combo_overlays[index][oi]) {
            InvalidateRect(g_card_combo_overlays[index][oi], NULL, FALSE);
        }
    }
    InvalidateRect(GetDlgItem(g_hwnd, channel_set_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_on_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_off_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_status_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_track_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_lbl_high_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_lbl_medium_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_lbl_low_id(index)), NULL, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, channel_lbl_off_id(index)), NULL, FALSE);
}

/* Every control that can actually command a channel (mode Set, ON, OFF,
 * the level gauge) is disabled while RS422 isn't connected - there's no
 * bus to blind-send on, so letting the user "arm" 16 channels' worth of
 * mode/level/output first and have it silently go nowhere is worse than
 * just not letting them touch it yet. Mirrors conn_is_connected() on
 * every connect/disconnect and once at startup. EnableWindow() alone
 * already blocks all mouse input to a disabled window at the OS level
 * (WM_NCHITTEST returns HTTRANSPARENT for it) - the gauge's click/drag
 * handling in channel_gauge_subclass_proc never even runs disabled.
 *
 * The mode combo itself is deliberately left out: selecting a mode is
 * local/uncommitted until Set is clicked (see the WM_COMMAND handler),
 * so it's harmless while disconnected - and a disabled native COMBOBOX
 * stops sending WM_CTLCOLORSTATIC at all, falling back to Windows' own
 * plain white/gray disabled look, which would blow a bright hole
 * through the dark theme (that took five attempts to get right - see
 * the big comment above combo_arrow_subclass_proc). Set/ON/OFF/gauge
 * are all custom-painted, so they stay fully themed either way. */
static const int BULK_ACTION_BTN_IDS[] = {
    IDC_BULK_CLEAR_BTN, IDC_BULK_SELECT_ALL_BTN, IDC_BULK_SET_BTN, IDC_BULK_ON_BTN, IDC_BULK_OFF_BTN,
    IDC_BULK_HIGH_BTN, IDC_BULK_MEDIUM_BTN, IDC_BULK_LOW_BTN, IDC_BULK_LEVEL_OFF_BTN
};
#define BULK_ACTION_BTN_COUNT (sizeof(BULK_ACTION_BTN_IDS) / sizeof(BULK_ACTION_BTN_IDS[0]))

static void set_channel_controls_enabled(bool enabled) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        HWND set_btn = GetDlgItem(g_hwnd, channel_set_id(i));
        HWND on_btn = GetDlgItem(g_hwnd, channel_on_id(i));
        HWND off_btn = GetDlgItem(g_hwnd, channel_off_id(i));
        HWND gauge = GetDlgItem(g_hwnd, channel_track_id(i));

        EnableWindow(set_btn, enabled);
        EnableWindow(on_btn, enabled);
        EnableWindow(off_btn, enabled);
        EnableWindow(gauge, enabled);

        InvalidateRect(set_btn, NULL, FALSE);
        InvalidateRect(on_btn, NULL, FALSE);
        InvalidateRect(off_btn, NULL, FALSE);
        InvalidateRect(gauge, NULL, FALSE);
    }
    for (i = 0; i < (int)BULK_ACTION_BTN_COUNT; i++) {
        HWND btn = GetDlgItem(g_hwnd, BULK_ACTION_BTN_IDS[i]);
        EnableWindow(btn, enabled);
        InvalidateRect(btn, NULL, FALSE);
    }
}

/* ---- Bulk Actions ----
 * Click a card's background to select it (see card_panel_subclass_proc);
 * these apply to every selected channel at once. Same safety gating as
 * each card's own controls: OFF always works even kill-switch-tripped,
 * ON/Set/level skip a tripped channel; the level buttons additionally
 * skip a channel that isn't already on, matching the per-channel
 * gauge's own "only adjusts an already-running channel" rule. */

static void ui_refresh_bulk_selected_label(void) {
    int i, count = 0;
    char text[16];
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (g_channel_selected[i]) count++;
    }
    wsprintfA(text, "%d selected", count);
    SetDlgItemTextA(g_hwnd, IDC_BULK_SELECTED_LBL, text);
}

static void bulk_clear_selection(void) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (g_channel_selected[i]) {
            g_channel_selected[i] = false;
            ui_invalidate_card(i);
        }
    }
    ui_refresh_bulk_selected_label();
}

/* Select-all's real value isn't "apply to all 16 at once" (rare) - it's
 * making the opposite case fast: select all, then uncheck the handful
 * you actually want left out, instead of clicking 12+ individual
 * checkboxes to build the same set by hand. */
static void bulk_select_all(void) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (!g_channel_selected[i]) {
            g_channel_selected[i] = true;
            ui_invalidate_card(i);
        }
    }
    ui_refresh_bulk_selected_label();
}

static const int BULK_BAR_SHOWHIDE_IDS[] = {
    IDC_BULK_SELECTED_LBL, IDC_BULK_CLEAR_BTN, IDC_BULK_SELECT_ALL_BTN, IDC_BULK_MODE_COMBO, IDC_BULK_SET_BTN,
    IDC_BULK_ON_BTN, IDC_BULK_OFF_BTN, IDC_BULK_HIGH_BTN, IDC_BULK_MEDIUM_BTN,
    IDC_BULK_LOW_BTN, IDC_BULK_LEVEL_OFF_BTN
};
#define BULK_BAR_SHOWHIDE_COUNT (sizeof(BULK_BAR_SHOWHIDE_IDS) / sizeof(BULK_BAR_SHOWHIDE_IDS[0]))

/* Bulk select mode is off by default - single-channel operation is the
 * normal way to use this app, bulk is an occasional extra, not the main
 * way to operate it (see the header comment above where this is wired
 * in). Toggling it off always clears whatever was selected too, so
 * turning it back on later starts clean rather than resuming a stale,
 * invisible selection. */
static void set_bulk_select_mode(bool on) {
    int i;

    g_bulk_select_mode = on;
    if (!on) {
        bulk_clear_selection();
    }

    for (i = 0; i < (int)BULK_BAR_SHOWHIDE_COUNT; i++) {
        ShowWindow(GetDlgItem(g_hwnd, BULK_BAR_SHOWHIDE_IDS[i]), on ? SW_SHOW : SW_HIDE);
    }
    for (i = 0; i < 5; i++) {
        if (g_bulk_combo_overlays[i]) {
            ShowWindow(g_bulk_combo_overlays[i], on ? SW_SHOW : SW_HIDE);
        }
    }
    for (i = 0; i < MAX_CHANNELS; i++) {
        ShowWindow(GetDlgItem(g_hwnd, channel_select_id(i)), on ? SW_SHOW : SW_HIDE);
    }
    SetDlgItemTextA(g_hwnd, IDC_BULK_TOGGLE_BTN, on ? "Done" : "Select Channels");

    /* Collapsed, this button is the only thing in an otherwise empty
     * card, so it sits centered in it rather than pinned to the corner.
     * Expanded, it moves to the row-1 corner slot that matches a Unit
     * card's checkbox position (see build_controls()'s Bulk Actions
     * block) so the rest of the layout still mirrors a channel card. */
    MoveWindow(GetDlgItem(g_hwnd, IDC_BULK_TOGGLE_BTN),
               on ? 740 : 605, on ? 22 : 81, 138, 22, TRUE);
}

static void bulk_apply_mode(uint8_t mode) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (g_channel_selected[i] && !g_kill_switch_tripped[i]) {
            channel_set_mode(i, mode);
            SetWindowTextA(g_card_mode_lbl[i], proto_mode_name(mode));
            SendDlgItemMessageA(g_hwnd, channel_mode_id(i), CB_SETCURSEL, (WPARAM)mode, 0);
        }
    }
}

static void bulk_turn_output_on(void) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (g_channel_selected[i] && !g_kill_switch_tripped[i]) {
            channel_turn_output_on(i);
        }
    }
}

static void bulk_turn_output_off(void) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (g_channel_selected[i]) {
            channel_turn_output_off(i);
        }
    }
}

static void bulk_apply_level(int level) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (!g_channel_selected[i]) continue;
        if (level == LEVEL_OFF) {
            channel_turn_output_off(i);
        } else if (channels_get(i)->output_on && !g_kill_switch_tripped[i]) {
            channel_set_level(i, level);
        }
    }
}

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

        /* Disconnected: no gradient, no handle - just a flat muted
         * track, matching every other channel control's disabled look
         * (see set_channel_controls_enabled()). Nothing to dial in
         * before there's a connection to send it over. */
        if (!IsWindowEnabled(hwnd)) {
            int track_w = w / 4;
            int track_cx = rc.left + w / 2;
            RECT track;
            HBRUSH bg_brush, track_brush;
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

            track_pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
            old_pen = (HPEN)SelectObject(hdc, track_pen);
            track_brush = CreateSolidBrush(COLOR_APP_MUTED);
            old_brush = (HBRUSH)SelectObject(hdc, track_brush);
            RoundRect(hdc, track.left, track.top, track.right, track.bottom, track_w, track_w);
            SelectObject(hdc, old_brush);
            SelectObject(hdc, old_pen);
            DeleteObject(track_brush);
            DeleteObject(track_pen);

            EndPaint(hwnd, &ps);
            return 0;
        }

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
    /* Bulk Actions selection checkbox - a real, always-there, always-
     * empty-around-it click target in the card's top-right corner, not
     * "click the card's background somewhere" (unreliable once
     * connected - every other control on the card is itself clickable
     * by then and swallows the click first). Owner-drawn like every
     * other button here, not BS_AUTOCHECKBOX - a native checkbox is
     * plain white/system-themed, and would be the one control on this
     * whole app that doesn't match the dark theme. */
    add_ctrl(hwnd, "BUTTON", NULL, BS_OWNERDRAW | WS_TABSTOP, x + CARD_W - 24, y + 6, 16, 16, channel_select_id(index));
    g_card_icon[index] = add_header_icon(hwnd, x + 8, y + 6, ICON_WAVE);
    wsprintfA(header, "Unit %d", index + 1);
    g_card_header[index] = add_header(hwnd, header, x + 26, y + 6, 58, 16);
    /* Muted mode name next to the header, matching the design mockup's
     * card title row - shows the applied mode (ch->mode), updated in
     * WM_COMMAND when Set is clicked, not the dropdown's uncommitted
     * selection. SS_END_ELLIPSIS since the longer mode names won't all
     * fit in the space left before the gauge column. */
    g_card_mode_lbl[index] = add_ctrl(hwnd, "STATIC", proto_mode_name(PROTO_MODE_WHITE_NOISE),
                                        SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, x + 88, y + 8, 60, 14, 0);

    mode_combo = add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP,
                           x + 8, y + 24, 82, 100, channel_mode_id(index));
    for (i = 0; i < PROTO_MODE_COUNT; i++) {
        const char *name = proto_mode_name((uint8_t)i);
        SendMessageA(mode_combo, CB_ADDSTRING, 0, (LPARAM)(name ? name : "?"));
    }
    SendMessageA(mode_combo, CB_SETCURSEL, PROTO_MODE_WHITE_NOISE, 0);
    SendMessageA(mode_combo, CB_SETDROPPEDWIDTH, 190, 0);
    make_combo_readonly_ex(mode_combo, g_card_combo_overlays[index]);

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
     * on) - only repaint it on the one field it actually depends on.
     * Uses ui_invalidate_card(), not a bare InvalidateRect() on just the
     * panel - see that function's comment for why the panel's own
     * repaint can't be trusted alone to leave its siblings alone. */
    if (!cache->valid || cache->output_on != ch->output_on) {
        ui_invalidate_card(index);
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

/* ---- spectrum panel ----
 * Not a capture - this app has no receiver - but not a guess either:
 * every channel's mode/level/on-off is exactly what THIS APP
 * commanded, so both the trace shape and the caption's dBm/MHz come
 * straight from real, known state. The trace shape per mode was
 * matched against a real ZS-407 capture earlier this session (PRN =
 * noisy flat block + wide harmonic, Comb = many narrow teeth + medium
 * harmonic, Linear Sweep = fewer/tighter teeth + narrow harmonic, CW =
 * one clean spike + one clean harmonic) - drawn as an actual jagged
 * trace line, not solid bars, so it reads as a spectrum plot rather
 * than a bar chart. Off channels just show a flat floor line. */

static int spectrum_peak_pct(int level) {
    switch (level) {
        case LEVEL_LOW:    return 40;
        case LEVEL_MEDIUM: return 70;
        case LEVEL_HIGH:   return 100;
        default:           return 0;
    }
}

/* Small random wobble, not true per-frame chaos - called with a fixed
 * amplitude per role (noise floor vs. tooth peak) so the trace has
 * live texture without the underlying shape ever looking unstable. */
static int spectrum_jitter(int amplitude) {
    if (amplitude <= 0) return 0;
    return (rand() % (2 * amplitude + 1)) - amplitude;
}

/* Height (0..100, % of `area`'s vertical span above the floor) of the
 * trace at pixel column `px` of `w` - the per-mode shape. `peak_pct`
 * is the channel's level-scaled ceiling (see spectrum_peak_pct). */
static int spectrum_height_pct(uint8_t mode, int px, int w, int peak_pct) {
    int fund_lo = w * 28 / 100, fund_hi = w * 46 / 100;
    int harm_lo = w * 66 / 100, harm_hi = w * 80 / 100;
    int harm_mid = (harm_lo + harm_hi) / 2;
    int val = 1 + spectrum_jitter(2); /* near-floor noise everywhere */

    switch (mode) {
        case PROTO_MODE_WHITE_NOISE: /* Pseudo Random Noise */
            if (px >= fund_lo && px <= fund_hi) {
                val = peak_pct - 6 + spectrum_jitter(9);
            } else if (px >= harm_lo && px <= harm_hi) {
                val = peak_pct * 28 / 100 + spectrum_jitter(6);
            }
            break;
        case PROTO_MODE_LINEAR_SWEEP: {
            const int tooth_w = 6;
            if (px >= fund_lo && px <= fund_hi && (px - fund_lo) % tooth_w <= 1) {
                int tooth_idx = (px - fund_lo) / tooth_w;
                int h = peak_pct - (tooth_idx % 3) * 18;
                if (h < 20) h = 20;
                val = h + spectrum_jitter(6);
            } else if (px >= harm_mid - 1 && px <= harm_mid + 1) {
                val = peak_pct * 16 / 100 + spectrum_jitter(4);
            }
            break;
        }
        case PROTO_MODE_COMB_SPECTRUM: {
            int band_lo = fund_lo - w * 4 / 100;
            int band_hi = fund_hi + w * 4 / 100;
            const int tooth_w = 4;
            if (px >= band_lo && px <= band_hi && (px - band_lo) % tooth_w <= 1) {
                int tooth_idx = (px - band_lo) / tooth_w;
                int h = peak_pct - (tooth_idx % 4) * 14;
                if (h < 15) h = 15;
                val = h + spectrum_jitter(6);
            } else if (px >= harm_lo && px <= harm_hi) {
                val = peak_pct * 42 / 100 + spectrum_jitter(8);
            }
            break;
        }
        case PROTO_MODE_SINGLE: /* Continuous Wave */
        default: {
            int fund_mid = (fund_lo + fund_hi) / 2;
            if (px >= fund_mid - 1 && px <= fund_mid + 1) {
                val = peak_pct + spectrum_jitter(3);
            } else if (px >= harm_mid - 1 && px <= harm_mid + 1) {
                val = peak_pct * 20 / 100 + spectrum_jitter(4);
            }
            break;
        }
    }
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    return val;
}

#define SPECTRUM_MAX_TRACE_PTS 360

/* Draws one channel's trace into `area` - flat muted floor when off, a
 * real jagged trace line (mode-shaped fundamental + 2nd-harmonic bump,
 * scaled by level) when on. `label` (may be NULL) prints a small unit
 * number in the corner, for the all-16 grid; `caption` (may be NULL)
 * prints the exact real mode/frequency/power along the bottom, for the
 * single-channel view. */
static void draw_channel_spectrum(HDC hdc, RECT area, const ChannelState *ch,
                                   const char *label, const char *caption) {
    int w = area.right - area.left;
    int h = area.bottom - area.top;
    int floor_y = area.bottom - 2;
    HPEN floor_pen, old_pen;
    HFONT old_font;

    floor_pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
    old_pen = (HPEN)SelectObject(hdc, floor_pen);
    MoveToEx(hdc, area.left, floor_y, NULL);
    LineTo(hdc, area.right, floor_y);
    SelectObject(hdc, old_pen);
    DeleteObject(floor_pen);

    if (label) {
        RECT lbl_rc = area;
        lbl_rc.bottom = lbl_rc.top + 12;
        old_font = (HFONT)SelectObject(hdc, g_font);
        SetTextColor(hdc, ch->output_on ? COLOR_APP_TEXT : COLOR_APP_MUTED);
        SetBkMode(hdc, TRANSPARENT);
        DrawTextA(hdc, label, -1, &lbl_rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
        SelectObject(hdc, old_font);
    }

    if (caption) {
        RECT cap_rc = area;
        cap_rc.bottom = cap_rc.top + 14;
        old_font = (HFONT)SelectObject(hdc, g_font);
        SetTextColor(hdc, COLOR_APP_MUTED);
        SetBkMode(hdc, TRANSPARENT);
        DrawTextA(hdc, caption, -1, &cap_rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
        SelectObject(hdc, old_font);
    }

    if (!ch->output_on || w < 12 || h < 10) {
        return;
    }

    {
        int peak_pct = spectrum_peak_pct(ch->level);
        HPEN trace_pen = CreatePen(PS_SOLID, 1, ch_gauge_stop_color(ch->level));
        HPEN old_trace_pen = (HPEN)SelectObject(hdc, trace_pen);
        int n = w;
        int px;
        if (n > SPECTRUM_MAX_TRACE_PTS) n = SPECTRUM_MAX_TRACE_PTS;

        MoveToEx(hdc, area.left, floor_y, NULL);
        for (px = 0; px < n; px++) {
            int sample_px = px * w / n;
            int pct = spectrum_height_pct(ch->mode, sample_px, w, peak_pct);
            int y = floor_y - h * pct / 100;
            LineTo(hdc, area.left + sample_px, y);
        }
        LineTo(hdc, area.right, floor_y);

        SelectObject(hdc, old_trace_pen);
        DeleteObject(trace_pen);
    }
}

/* Faint reference grid across the whole plot, underneath every trace -
 * 3 horizontal divisions + 4 vertical, same idea as a real spectrum
 * analyzer's graticule. Purely a visual reference, not real scale
 * markings (this app has no receiver - see draw_channel_spectrum's own
 * comment). */
static void spectrum_draw_grid(HDC hdc, RECT rc) {
    /* COLOR_APP_MUTED, not COLOR_APP_PANEL_BORDER - the border color
     * turned out too close to the field background to actually notice
     * ("wheres the damn lines" - they genuinely weren't visible enough
     * on real hardware). This is clearly visible without being as loud
     * as real text. */
    HPEN pen = CreatePen(PS_SOLID, 1, COLOR_APP_MUTED);
    HPEN old_pen = (HPEN)SelectObject(hdc, pen);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int i;

    for (i = 1; i < 4; i++) {
        int y = rc.top + h * i / 4;
        MoveToEx(hdc, rc.left, y, NULL);
        LineTo(hdc, rc.right, y);
    }
    /* Matches the 4 columns of channel cells drawn above - this used to
     * divide into 5 vertical sections while the content above only ever
     * fills 4 columns, so the rightmost 1/5th of the plot was
     * permanently empty gridded space with nothing drawn in it. */
    for (i = 1; i < 4; i++) {
        int x = rc.left + w * i / 4;
        MoveToEx(hdc, x, rc.top, NULL);
        LineTo(hdc, x, rc.bottom);
    }

    SelectObject(hdc, old_pen);
    DeleteObject(pen);
}

static LRESULT CALLBACK spectrum_plot_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_brush_field);
        spectrum_draw_grid(hdc, rc);

        if (g_spectrum_show_all) {
            const int cols = 4;
            const int rows = 4;
            int total_w = rc.right - rc.left;
            int total_h = rc.bottom - rc.top;
            int i;
            /* Each boundary computed straight from the total (col * w /
             * cols), not by accumulating a once-truncated per-cell width
             * (col * (w / cols)) - the latter loses w % cols pixels off
             * the right/bottom edge entirely (the last column's right
             * edge lands short of rc.right whenever w isn't an exact
             * multiple of 4), leaving a blank strip with no content in
             * it. This way the last column/row's far edge is always
             * exactly rc.right/rc.bottom. */
            for (i = 0; i < MAX_CHANNELS; i++) {
                RECT cell;
                char label[4];
                int col = i % cols;
                int row = i / cols;
                cell.left = rc.left + col * total_w / cols + 3;
                cell.right = rc.left + (col + 1) * total_w / cols - 3;
                cell.top = rc.top + row * total_h / rows + 2;
                cell.bottom = rc.top + (row + 1) * total_h / rows - 2;
                wsprintfA(label, "%d", i + 1);
                draw_channel_spectrum(hdc, cell, channels_get(i), label, NULL);
            }
        } else {
            const ChannelState *ch = channels_get(g_spectrum_unit);
            char caption[64];
            if (ch->output_on) {
                wsprintfA(caption, "%s", proto_mode_name(ch->mode));
            } else {
                wsprintfA(caption, "%s - STANDBY", proto_mode_name(ch->mode));
            }
            draw_channel_spectrum(hdc, rc, ch, NULL, caption);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

static HWND add_spectrum_plot(HWND parent, int x, int y, int w, int h, int id) {
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT, x, y, w, h, id);
    if (ctrl) {
        if (!g_panel_orig_proc) {
            g_panel_orig_proc = (WNDPROC)GetWindowLongPtrA(ctrl, GWLP_WNDPROC);
        }
        SetWindowLongPtrA(ctrl, GWLP_WNDPROC, (LONG_PTR)spectrum_plot_subclass_proc);
    }
    return ctrl;
}

/* ---- saved settings (port/baud/parity/data bits, per-channel mode/
 * level/output_on - never the kill switch, and never auto-connects
 * anything). Restoring output_on does not transmit anything on its own -
 * see channel_restore_saved()'s comment. Stored next to the exe as a
 * plain .ini, matching this app's portable/no-installer approach - not
 * AppData. ---- */

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

    /* Per-channel mode + resume-to level + output_on, never saved before -
     * reopening the app silently reset every channel back to White Noise/
     * no level/OFF with no way to get a saved setup back. Saving output_on
     * does NOT mean this app auto-resumes transmission on launch - see
     * channel_restore_saved()'s comment: the amplifier hardware holds its
     * own commanded state independently of whether this app is running,
     * so restoring output_on into the UI just keeps it honest about what's
     * actually still out there, without sending anything to get there. */
    {
        int i;
        char section[8];
        for (i = 0; i < MAX_CHANNELS; i++) {
            const ChannelState *ch = channels_get(i);
            wsprintfA(section, "Ch%d", i + 1);
            wsprintfA(buf, "%u", (unsigned)ch->mode);
            WritePrivateProfileStringA(section, "Mode", buf, path);
            wsprintfA(buf, "%d", ch->last_level);
            WritePrivateProfileStringA(section, "Level", buf, path);
            wsprintfA(buf, "%d", ch->output_on ? 1 : 0);
            WritePrivateProfileStringA(section, "Output", buf, path);
        }
    }
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

/* Must run AFTER channels_init() (which sets every channel back to its
 * hardcoded defaults) and after build_controls() (which needs the mode
 * combos/labels to already exist) - restores each channel's saved mode/
 * resume-to level/output_on via channel_restore_saved() (data only, no
 * serial send - see its own comment), then mirrors mode into the mode
 * combo's selection and the card's mode label so the UI actually shows
 * it. output_on/level need no manual mirroring here - ui_refresh_all_
 * channels() (called right after this, in WM_CREATE) reads them straight
 * off channels_get() same as any other state change.
 * GetPrivateProfileIntA's own default (-1) means "key missing", so a
 * channel with no saved entry is left exactly as channels_init() set it.
 * Output defaults to 0 (off) when missing - an .ini saved before this
 * field existed should not suddenly claim a channel is transmitting. */
static void load_channel_settings(void) {
    char path[MAX_PATH + 8];
    char section[8];
    int i;

    get_ini_path(path);

    for (i = 0; i < MAX_CHANNELS; i++) {
        int mode, level, output_on;
        wsprintfA(section, "Ch%d", i + 1);
        mode = GetPrivateProfileIntA(section, "Mode", -1, path);
        level = GetPrivateProfileIntA(section, "Level", -1, path);
        output_on = GetPrivateProfileIntA(section, "Output", 0, path);
        if (mode < 0 || level < 0) {
            continue;
        }
        channel_restore_saved(i, (uint8_t)mode, level, output_on != 0);
        SendDlgItemMessageA(g_hwnd, channel_mode_id(i), CB_SETCURSEL, (WPARAM)mode, 0);
        SetWindowTextA(g_card_mode_lbl[i], proto_mode_name((uint8_t)mode));
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

    /* Left-aligned against the header panel's own left edge, matching
     * every other section's left margin (22px) - was right-of-center
     * (tucked up against Amplifier Temperature), leaving the whole left
     * half of the header empty. Bulk Actions now takes the middle
     * column, Amplifier Temperature stays right-aligned. */
    add_header_icon(hwnd, 36, 14, ICON_PLUG);
    add_header(hwnd, "Connection && Settings", 54, 14, 260, 18);

    /* Every row below is centered within this section's own ~310px-wide
     * span (roughly x=22-332) instead of flush against its left edge -
     * each row's total width is computed, then its start x is
     * (span_width - row_width) / 2 past the span's left edge.
     * Refresh/Connect on their own row below Port. Port combo kept
     * compact (90px, matching Baud below it) - direct request was
     * just "com port, connect/disconnect", not a wide dropdown.
     * Connected/Disconnected on the same row as Port itself. Row
     * pitch is a modest ~8px gap between a row's visual bottom and
     * the next row's top - not the cramped 3-4px pitch from before
     * (too tight), not the old ~30-40px gaps either (too spacious). */
    add_ctrl(hwnd, "STATIC", "Port:", SS_LEFT, 63, 36, 32, 16, 0);
    make_combo_readonly(add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 99, 34, 90, 140, IDC_PORT_COMBO));
    add_ctrl(hwnd, "STATIC", "Disconnected", SS_LEFT | SS_NOPREFIX, 201, 36, 100, 16, IDC_CONN_STATUS_LBL);
    add_ctrl(hwnd, "BUTTON", "Refresh", BS_OWNERDRAW | WS_TABSTOP, 105, 63, 64, 18, IDC_REFRESH_BTN);
    add_ctrl(hwnd, "BUTTON", "Connect", BS_OWNERDRAW | WS_TABSTOP, 177, 63, 72, 18, IDC_CONNECT_BTN);

    add_ctrl(hwnd, "STATIC", "Baud:", SS_LEFT, 113, 91, 34, 16, 0);
    make_combo_readonly(add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 151, 89, 90, 140, IDC_BAUD_COMBO));
    add_ctrl(hwnd, "STATIC", "Data Bits:", SS_LEFT, 57, 120, 60, 16, 0);
    make_combo_readonly(add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 121, 118, 45, 100, IDC_DATABITS_COMBO));
    add_ctrl(hwnd, "STATIC", "Parity:", SS_LEFT, 182, 120, 40, 16, 0);
    make_combo_readonly(add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 226, 118, 70, 100, IDC_PARITY_COMBO));

    /* Bulk Actions - middle column of the header, between Connection &
     * Settings (left) and Amplifier Temperature (right). Off by default -
     * single-channel operation is the normal way to use this app, bulk
     * is an occasional extra, not the main way to operate it. Only the
     * icon/title and the toggle button are visible until it's clicked;
     * that reveals the rest of this bar AND every card's selection
     * checkbox (see set_bulk_select_mode()). Click a card's checkbox to
     * select it (lit accent border), then one of these applies to every
     * selected channel at once. Same safety gating as each card's own
     * controls: OFF always works even kill-switch-tripped, ON/Set/level
     * skip a tripped channel. The action controls are also disabled
     * alongside every per-channel control until RS422 connects - see
     * set_channel_controls_enabled(). Two rows, same row-pitch as
     * Connection & Settings' own rows (y=36/63). */
    {
        HWND bulk_mode_combo;
        int mi;

        /* Bulk Actions' own inset card border/shadow is drawn by
         * panel_subclass_proc itself (see the hwnd == g_header_panel
         * branch there), not a second panel window here - the one
         * section in this header meant to read as a distinct,
         * occasional-use card rather than blending into the shared
         * header background like Connection & Settings and Amplifier
         * Temperature do.
         *
         * Layout is a bigger version of a channel card's own layout
         * (see add_channel_card()), not an unrelated arrangement: icon +
         * title + a caption + a corner control on row 1 (title/mode-
         * name/selection-checkbox there -> title/selected-count/toggle-
         * button here), combo + a button on row 2 (mode combo + Set,
         * same on both), a primary on/off row on row 3, a status-line
         * row at the bottom-left on row 4 (STANDBY there -> Clear here),
         * and a right-side vertical column spanning rows 2-4 (the level
         * gauge + High/Medium/Low/Off tick labels there -> the same 4
         * levels as actual buttons here, since bulk applies a level with
         * a click rather than a drag). */
        add_header_icon(hwnd, 470, 24, ICON_LIST);
        add_header(hwnd, "Bulk Actions", 488, 24, 150, 18);
        add_ctrl(hwnd, "STATIC", "0 selected", SS_LEFT | SS_NOPREFIX,
                 648, 26, 84, 16, IDC_BULK_SELECTED_LBL);
        /* Starting position matches the collapsed (off) state - centered
         * in the card, since that's all there is to look at until it's
         * clicked. set_bulk_select_mode() moves it to the row-1 corner
         * slot (matching a Unit card's checkbox position) once expanded,
         * and back here when collapsed again. */
        add_ctrl(hwnd, "BUTTON", "Select Channels", BS_OWNERDRAW | WS_TABSTOP,
                 605, 81, 138, 22, IDC_BULK_TOGGLE_BTN);

        bulk_mode_combo = add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP,
                                    470, 54, 230, 140, IDC_BULK_MODE_COMBO);
        for (mi = 0; mi < PROTO_MODE_COUNT; mi++) {
            const char *name = proto_mode_name((uint8_t)mi);
            SendMessageA(bulk_mode_combo, CB_ADDSTRING, 0, (LPARAM)(name ? name : "?"));
        }
        SendMessageA(bulk_mode_combo, CB_SETCURSEL, PROTO_MODE_WHITE_NOISE, 0);
        make_combo_readonly_ex(bulk_mode_combo, g_bulk_combo_overlays);
        add_ctrl(hwnd, "BUTTON", "Set", BS_OWNERDRAW | WS_TABSTOP,
                 710, 54, 60, 20, IDC_BULK_SET_BTN);

        add_ctrl(hwnd, "BUTTON", "ON", BS_OWNERDRAW | WS_TABSTOP,
                 470, 84, 110, 22, IDC_BULK_ON_BTN);
        add_ctrl(hwnd, "BUTTON", "OFF", BS_OWNERDRAW | WS_TABSTOP,
                 590, 84, 110, 22, IDC_BULK_OFF_BTN);

        add_ctrl(hwnd, "BUTTON", "Clear", BS_OWNERDRAW | WS_TABSTOP,
                 470, 116, 90, 20, IDC_BULK_CLEAR_BTN);
        /* Select All's real value is the opposite case: select all,
         * then uncheck the few you want left out, instead of clicking
         * 12+ individual checkboxes by hand. */
        add_ctrl(hwnd, "BUTTON", "Select All", BS_OWNERDRAW | WS_TABSTOP,
                 568, 116, 90, 20, IDC_BULK_SELECT_ALL_BTN);

        add_ctrl(hwnd, "BUTTON", "High", BS_OWNERDRAW | WS_TABSTOP,
                 790, 54, 84, 18, IDC_BULK_HIGH_BTN);
        add_ctrl(hwnd, "BUTTON", "Medium", BS_OWNERDRAW | WS_TABSTOP,
                 790, 76, 84, 18, IDC_BULK_MEDIUM_BTN);
        add_ctrl(hwnd, "BUTTON", "Low", BS_OWNERDRAW | WS_TABSTOP,
                 790, 98, 84, 18, IDC_BULK_LOW_BTN);
        add_ctrl(hwnd, "BUTTON", "Off", BS_OWNERDRAW | WS_TABSTOP,
                 790, 120, 84, 18, IDC_BULK_LEVEL_OFF_BTN);
    }

    /* Amplifier Temperature, right-aligned in the same header bar
     * rather than below it in the sidebar - same row shape as
     * Connection & Settings, just anchored to the header's right edge
     * instead of sitting bunched up next to it. Port/Refresh/Connect
     * stay on one combined row here (unlike Connection & Settings'
     * split rows) - splitting them would make this the taller of the
     * two cards, working against making it smaller. */
    add_header_icon(hwnd, 1033, 14, ICON_WAVE);
    add_header(hwnd, "Ambient Temperature", 1051, 14, 260, 18);
    add_ctrl(hwnd, "STATIC", "Port:", SS_LEFT, 1025, 36, 32, 16, 0);
    make_combo_readonly(add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 1059, 34, 82, 140, IDC_SENSOR_PORT_COMBO));
    add_ctrl(hwnd, "BUTTON", "Refresh", BS_OWNERDRAW | WS_TABSTOP, 1147, 35, 64, 18, IDC_SENSOR_REFRESH_BTN);
    add_ctrl(hwnd, "BUTTON", "Connect", BS_OWNERDRAW | WS_TABSTOP, 1215, 35, 72, 18, IDC_SENSOR_CONNECT_BTN);
    /* 6 physical sensors scanning the rack area, each at its own
     * address (see UNIT_TEMP_ADDR) - not one per RF channel. Status and
     * the rack-wide average (across whichever of the 6 currently have a
     * reading) are one aligned row of two gradient pills instead of two
     * stacked plain-text lines - width matches the chip grid below (88
     * *3 + 6*2 = 276) so the whole column reads as one aligned block. */
    /* Plain text, not a pill - only the temperature reading gets that
     * treatment. Still on the same row/aligned with the Avg pill next
     * to it, just left-aligned status text like every other connection
     * status label in this app (Connection & Settings' own status,
     * left as-is, is the same style). Row centered as a group within
     * the card zone, not flush left/right against its edges. */
    add_ctrl(hwnd, "STATIC", "Disconnected", SS_LEFT, 1029, 60, 100, 16, IDC_SENSOR_STATUS_LBL);
    add_pill(hwnd, "Avg -", 1149, 56, 134, 22, IDC_SENSOR_TEMP_LBL, (WNDPROC)sensor_avg_pill_subclass_proc);
    /* Address + reading per physical sensor unit, 3 columns x 2 rows -
     * plain text (no box), see sensor_chip_subclass_proc(). Chip height
     * trimmed 38 -> 32: the value text draws with DT_NOCLIP now (see
     * sensor_chip_subclass_proc), which is what actually fixed the old
     * decimal-point clipping bug, not the taller box - DT_NOCLIP draws
     * outside a short rect instead of cutting the glyphs off, so the
     * box itself can shrink safely. Width also trimmed 88 -> 84 and
     * the column gap 6 -> 4 so the grid fits inside the card zone's
     * narrower right margin. Centered within the zone (start x=1026,
     * not flush against 1023). */
    {
        int chip;
        for (chip = 0; chip < SENSOR_MAX_UNITS; chip++) {
            int col = chip % 3;
            int row = chip / 3;
            int cx = 1026 + col * (84 + 4);
            int cy = 88 + row * (32 + 4);
            g_sensor_chip[chip] = add_sensor_chip(hwnd, cx, cy, 84, 32, chip);
        }
    }
    add_ctrl(hwnd, "STATIC", "", SS_LEFT | SS_NOPREFIX, 1033, 174, 190, 16, IDC_KILL_STATUS_LBL);
    add_ctrl(hwnd, "BUTTON", "Reset", BS_OWNERDRAW | WS_TABSTOP, 1229, 172, 80, 22, IDC_KILL_RESET_BTN);
    ShowWindow(GetDlgItem(hwnd, IDC_KILL_STATUS_LBL), SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_KILL_RESET_BTN), SW_HIDE);

    /* Sidebar: one tall box - Spectrum up top (the space that used to
     * just be "reserved for other features"), Activity Log below that
     * in the SAME box, not a separate panel. Connection & Settings and
     * Amplifier Temperature moved up into the header above. */
    g_sidebar_panel = add_panel(hwnd, SIDEBAR_X, CONTENT_TOP, SIDEBAR_W, LOG_PANEL_Y + LOG_PANEL_H - CONTENT_TOP);

    add_header_icon(hwnd, 22, CONTENT_TOP + 10, ICON_WAVE);
    add_header(hwnd, "Spectrum", 40, CONTENT_TOP + 10, 188, 18);
    make_combo_readonly(add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP,
                                  236, CONTENT_TOP + 8, 56, 140, IDC_SPECTRUM_UNIT_COMBO));
    add_ctrl(hwnd, "BUTTON", "All", BS_OWNERDRAW | WS_TABSTOP,
             SIDEBAR_X + SIDEBAR_W - 12 - 60, CONTENT_TOP + 8, 60, 20, IDC_SPECTRUM_ALL_BTN);
    add_spectrum_plot(hwnd, 22, CONTENT_TOP + 34, SIDEBAR_W + SIDEBAR_X - 34, LOG_PANEL_Y - 12 - (CONTENT_TOP + 34),
                       IDC_SPECTRUM_PLOT);
    {
        int u;
        char item[4];
        HWND combo = GetDlgItem(hwnd, IDC_SPECTRUM_UNIT_COMBO);
        for (u = 0; u < MAX_CHANNELS; u++) {
            wsprintfA(item, "%d", u + 1);
            SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)item);
        }
        SendMessageA(combo, CB_SETCURSEL, (WPARAM)g_spectrum_unit, 0);
    }

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

    /* Must run after add_channel_card() has created every card's
     * selection checkbox above - set_bulk_select_mode() hides them by
     * ID via GetDlgItem, which finds nothing (and so hides nothing) for
     * a checkbox that doesn't exist yet. */
    set_bulk_select_mode(false);

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
    PLACE(GetDlgItem(hwnd, channel_select_id(index)), x + card_w - SX(24), y + SY(6), 16, 16);
    PLACE(g_card_icon[index], x + SX(8), y + SY(6), 14, 14);
    PLACE(g_card_header[index], x + SX(26), y + SY(6), SX(58), SY(16));
    PLACE(g_card_mode_lbl[index], x + SX(88), y + SY(8), SX(60), SY(14));

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
    MoveWindow(GetDlgItem(hwnd, IDC_SPECTRUM_UNIT_COMBO), 236, CONTENT_TOP + 8, 56, 140, FALSE);
    MoveWindow(GetDlgItem(hwnd, IDC_SPECTRUM_ALL_BTN), SIDEBAR_X + SIDEBAR_W - 12 - 60, CONTENT_TOP + 8, 60, 20, FALSE);
    MoveWindow(GetDlgItem(hwnd, IDC_SPECTRUM_PLOT), 22, CONTENT_TOP + 34,
               SIDEBAR_W + SIDEBAR_X - 34, LOG_PANEL_Y - 12 - (CONTENT_TOP + 34), FALSE);

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
            /* Segoe UI instead of the stock ~8pt system font - "Direction
             * B" from the UI design proposal calls for noticeably bigger,
             * more modern type than the cramped default; -12 is a modest
             * enough bump that it still fits the existing fixed-size
             * control heights without clipping. */
            g_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            if (!g_font) {
                g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            }

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
            load_channel_settings(); /* after channels_init(), which it would otherwise overwrite */
            sensor_init(&g_sensor);
            {
                int addr_i;
                for (addr_i = 0; addr_i < SENSOR_MAX_UNITS; addr_i++) {
                    sensor_set_unit_address(&g_sensor, addr_i, UNIT_TEMP_ADDR[addr_i]);
                }
            }
            SetTimer(hwnd, ID_POLL_TIMER, 100, NULL);
            /* Starts disconnected - every channel control starts
             * disabled too, same as conn_on_connected_changed() would
             * set once Connect is actually clicked. */
            set_channel_controls_enabled(false);
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
                InvalidateRect(GetDlgItem(hwnd, IDC_SPECTRUM_PLOT), NULL, FALSE);
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
            if (id == 0 && code == STN_CLICKED && g_bulk_select_mode) {
                /* A card panel's background was clicked - id is 0 for
                 * every add_panel()/add_card_panel() control, so match
                 * by HWND against g_card_panel instead. Only live while
                 * bulk select mode is on - otherwise this would let a
                 * stray click on a card's empty background silently
                 * select a channel with no visible feedback (the
                 * checkbox that shows it is hidden outside bulk mode). */
                HWND ctl = (HWND)lParam;
                int idx;
                for (idx = 0; idx < MAX_CHANNELS; idx++) {
                    if (ctl == g_card_panel[idx]) {
                        g_channel_selected[idx] = !g_channel_selected[idx];
                        ui_invalidate_card(idx);
                        ui_refresh_bulk_selected_label();
                        break;
                    }
                }
                return 0;
            }
            if (id == IDC_BULK_TOGGLE_BTN && code == BN_CLICKED) {
                set_bulk_select_mode(!g_bulk_select_mode);
                return 0;
            }
            if (id == IDC_BULK_CLEAR_BTN && code == BN_CLICKED) {
                bulk_clear_selection();
                return 0;
            }
            if (id == IDC_BULK_SELECT_ALL_BTN && code == BN_CLICKED) {
                bulk_select_all();
                return 0;
            }
            if (id == IDC_BULK_SET_BTN && code == BN_CLICKED) {
                int sel = (int)SendDlgItemMessageA(hwnd, IDC_BULK_MODE_COMBO, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    bulk_apply_mode((uint8_t)sel);
                }
                return 0;
            }
            if (id == IDC_BULK_ON_BTN && code == BN_CLICKED) {
                bulk_turn_output_on();
                return 0;
            }
            if (id == IDC_BULK_OFF_BTN && code == BN_CLICKED) {
                bulk_turn_output_off();
                return 0;
            }
            if (id == IDC_BULK_HIGH_BTN && code == BN_CLICKED) {
                bulk_apply_level(LEVEL_HIGH);
                return 0;
            }
            if (id == IDC_BULK_MEDIUM_BTN && code == BN_CLICKED) {
                bulk_apply_level(LEVEL_MEDIUM);
                return 0;
            }
            if (id == IDC_BULK_LOW_BTN && code == BN_CLICKED) {
                bulk_apply_level(LEVEL_LOW);
                return 0;
            }
            if (id == IDC_BULK_LEVEL_OFF_BTN && code == BN_CLICKED) {
                bulk_apply_level(LEVEL_OFF);
                return 0;
            }
            if (id == IDC_SPECTRUM_UNIT_COMBO && code == CBN_SELCHANGE) {
                int sel = (int)SendDlgItemMessageA(hwnd, IDC_SPECTRUM_UNIT_COMBO, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    g_spectrum_unit = sel;
                    g_spectrum_show_all = false;
                    InvalidateRect(GetDlgItem(hwnd, IDC_SPECTRUM_PLOT), NULL, FALSE);
                }
                return 0;
            }
            if (id == IDC_SPECTRUM_ALL_BTN && code == BN_CLICKED) {
                g_spectrum_show_all = true;
                InvalidateRect(GetDlgItem(hwnd, IDC_SPECTRUM_PLOT), NULL, FALSE);
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
                                SetWindowTextA(g_card_mode_lbl[idx], proto_mode_name((uint8_t)sel));
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
                    } else if (offset == IDC_CH_SELECT_OFFSET && code == BN_CLICKED) {
                        g_channel_selected[idx] = !g_channel_selected[idx];
                        ui_invalidate_card(idx);
                        ui_refresh_bulk_selected_label();
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
            /* Plain text, not a pill (see add_ctrl call site) - only the
             * Avg reading next to it is a pill
             * (sensor_avg_pill_subclass_proc, which bypasses
             * WM_CTLCOLORSTATIC entirely and doesn't need a case here). */
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
            /* A read-only combo box's internal edit child (the closed
             * field every dropdown shows) sends WM_CTLCOLORSTATIC, not
             * WM_CTLCOLOREDIT - that's specific to ES_READONLY-style
             * edit controls, confirmed by testing (WM_CTLCOLOREDIT
             * handling here never took effect; this does). Give it the
             * same dark field look as everything else in FIELD_BG,
             * with accent-colored text so it still reads as
             * interactive/clickable, distinct from plain labels. */
            {
                HWND parent = GetParent(ctl);
                char cls[16];
                if (parent && GetClassNameA(parent, cls, sizeof(cls)) && lstrcmpiA(cls, "ComboBox") == 0) {
                    SetTextColor(hdc, COLOR_APP_HEADER);
                    SetBkColor(hdc, COLOR_APP_FIELD_BG);
                    SetBkMode(hdc, OPAQUE);
                    return (LRESULT)g_brush_field;
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

        /* No plain (non-combo) edit control exists in this app, and a
         * read-only combo edit's closed field is colored above under
         * WM_CTLCOLORSTATIC instead (see why there) - WM_CTLCOLOREDIT
         * never actually fires here, so there's nothing to handle. */

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

                /* Bulk Actions selection checkbox - a small square,
                 * solid accent fill + a checkmark when selected, just an
                 * outline when not. Never disabled (selecting channels
                 * for a bulk action is allowed before connecting too -
                 * only the actual bulk action buttons are gated on
                 * connection). */
                if (offset == IDC_CH_SELECT_OFFSET) {
                    bool selected = g_channel_selected[idx];
                    HPEN pen, old_pen;

                    /* Plain square, not RoundRect - guaranteed full-pixel
                     * coverage with a solid FillRect first (RoundRect's
                     * corner curvature can leave the tiniest sliver of
                     * whatever's underneath showing at the very corners
                     * on some renderers - this rules that out entirely
                     * as a cause of the reported white patch). */
                    FillRect(dis->hDC, &rc, selected ? g_brush_accent : g_brush_panel);
                    pen = CreatePen(PS_SOLID, 1, COLOR_APP_HEADER);
                    old_pen = (HPEN)SelectObject(dis->hDC, pen);
                    SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                    Rectangle(dis->hDC, rc.left, rc.top, rc.right, rc.bottom);
                    SelectObject(dis->hDC, old_pen);
                    DeleteObject(pen);

                    if (selected) {
                        HPEN check_pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
                        HPEN old_check_pen = (HPEN)SelectObject(dis->hDC, check_pen);
                        MoveToEx(dis->hDC, rc.left + 3, rc.top + 8, NULL);
                        LineTo(dis->hDC, rc.left + 6, rc.bottom - 4);
                        LineTo(dis->hDC, rc.right - 3, rc.top + 3);
                        SelectObject(dis->hDC, old_check_pen);
                        DeleteObject(check_pen);
                    }
                    return TRUE;
                }

                /* ON/OFF are two real, separate buttons (not one toggle) -
                 * whichever is active gets a solid fill (green ON / red
                 * OFF), matching the reference apps' PowerButton exactly. */
                if (offset == IDC_CH_ON_OFFSET || offset == IDC_CH_OFF_OFFSET) {
                    const ChannelState *ch = channels_get(idx);
                    bool active = (offset == IDC_CH_ON_OFFSET) ? ch->output_on : !ch->output_on;
                    HBRUSH fill = (active && !disabled)
                        ? (offset == IDC_CH_ON_OFFSET ? g_brush_connected : g_brush_disconnected)
                        : g_brush_panel;
                    HPEN pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
                    HPEN old_pen = (HPEN)SelectObject(dis->hDC, pen);
                    HBRUSH old_brush = (HBRUSH)SelectObject(dis->hDC, fill);

                    RoundRect(dis->hDC, rc.left, rc.top, rc.right, rc.bottom, BTN_CORNER_DIAMETER, BTN_CORNER_DIAMETER);
                    SelectObject(dis->hDC, old_brush);
                    SelectObject(dis->hDC, old_pen);
                    DeleteObject(pen);

                    SetTextColor(dis->hDC, (active && !disabled) ? RGB(255, 255, 255) : COLOR_APP_MUTED);
                    SetBkMode(dis->hDC, TRANSPARENT);
                    GetWindowTextA(dis->hwndItem, text, sizeof(text));
                    DrawTextA(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    return TRUE;
                }

                /* Rounded to match the rest of "Direction B" (panels,
                 * cards) instead of the old hard-cornered FillRect - a
                 * borderless RoundRect via NULL_PEN keeps the same flat
                 * look, just with soft corners. The Connect/Disconnect
                 * toggle buttons (they swap their own text between the
                 * two - see ui_refresh_sensor()/the connection callback)
                 * get green/red instead of the generic accent color, so
                 * which action a click will take is visible without
                 * reading the label - same green-ON/red-OFF distinction
                 * the channel cards' power buttons already use. */
                {
                    HBRUSH fill = g_brush_accent;
                    if (dis->CtlID == IDC_CONNECT_BTN || dis->CtlID == IDC_SENSOR_CONNECT_BTN) {
                        GetWindowTextA(dis->hwndItem, text, sizeof(text));
                        fill = (lstrcmpiA(text, "Disconnect") == 0) ? g_brush_disconnected : g_brush_connected;
                    } else if (dis->CtlID == IDC_BULK_ON_BTN) {
                        /* Same green/red as a channel card's own ON/OFF
                         * pair - Bulk's ON/OFF are one-shot actions
                         * rather than a toggle reflecting a single
                         * channel's current state, so there's no "which
                         * one is active" to light up; they just stay
                         * green/red always, so the color still reads as
                         * "what this does" the same way it does on every
                         * card below. */
                        fill = g_brush_connected;
                    } else if (dis->CtlID == IDC_BULK_OFF_BTN) {
                        fill = g_brush_disconnected;
                    } else if (dis->CtlID == IDC_BULK_HIGH_BTN) {
                        /* Same 4 stop colors as a channel card's own
                         * level gauge (see ch_gauge_stop_color()) -
                         * High/Medium/Low/Off here are 4 separate
                         * buttons instead of a draggable gradient, but
                         * direct request was for them to still read as
                         * "the same colors", top-to-bottom, as the real
                         * gauge does. */
                        fill = g_brush_disconnected; /* red, same as LEVEL_HIGH */
                    } else if (dis->CtlID == IDC_BULK_MEDIUM_BTN) {
                        fill = g_brush_level_medium; /* orange */
                    } else if (dis->CtlID == IDC_BULK_LOW_BTN) {
                        fill = g_brush_connected; /* green, same as LEVEL_LOW */
                    } else if (dis->CtlID == IDC_BULK_LEVEL_OFF_BTN) {
                        fill = g_brush_level_off; /* muted gray, same as LEVEL_OFF */
                    }
                    {
                        HPEN old_pen = (HPEN)SelectObject(dis->hDC, GetStockObject(NULL_PEN));
                        HBRUSH old_brush = (HBRUSH)SelectObject(dis->hDC, disabled ? g_brush_accent_dis : fill);
                        RoundRect(dis->hDC, rc.left, rc.top, rc.right, rc.bottom, BTN_CORNER_DIAMETER, BTN_CORNER_DIAMETER);
                        SelectObject(dis->hDC, old_brush);
                        SelectObject(dis->hDC, old_pen);
                    }
                }
                SetTextColor(dis->hDC, RGB(255, 255, 255));
                SetBkMode(dis->hDC, TRANSPARENT);
                GetWindowTextA(dis->hwndItem, text, sizeof(text));
                DrawTextA(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                /* No dashed focus-rect after a click - the fill color
                 * already shows which button is active/current, the
                 * extra dotted outline just read as a stray line. */
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
            if (g_brush_level_medium) DeleteObject(g_brush_level_medium);
            if (g_brush_level_off) DeleteObject(g_brush_level_off);
            if (g_brush_shadow) DeleteObject(g_brush_shadow);
            if (g_brush_dot_pattern) DeleteObject(g_brush_dot_pattern);
            if (g_dot_pattern_bmp) DeleteObject(g_dot_pattern_bmp);
            if (g_header_font && g_header_font != g_font) DeleteObject(g_header_font);
            /* Only delete g_font if it's the CreateFontA() result, not
             * the GetStockObject() fallback - stock objects must never
             * be passed to DeleteObject(). */
            if (g_font && g_font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(g_font);
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
    g_brush_level_medium = CreateSolidBrush(RGB(224, 146, 34));
    g_brush_level_off = CreateSolidBrush(COLOR_APP_MUTED);
    g_brush_shadow = CreateSolidBrush(COLOR_APP_SHADOW);
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
