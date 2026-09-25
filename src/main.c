/* ECM Controller: 16-channel blind-send control panel.
 * Separate build from the single-channel app (../src) - shares the
 * protocol/connection/serial_port layer unchanged (copied in, not
 * touched), but uses its own channels.c blind-send logic instead of
 * device.c's wait-for-ACK model. See channels.h for why.
 *
 * No Qt, no pywebview, no vendor DLL - just user32/gdi32/kernel32/advapi32,
 * same as the single-channel app (plus msimg32 for GradientFill, used by
 * the temperature gauge, and gdiplus for decoding a picked PNG/JPEG logo -
 * see load_image_as_bitmap_gdiplus() - both are Windows-shipped DLLs, not
 * bundled files).
 */
#define _WIN32_WINNT 0x0600 /* Vista+ - needed so windows.h declares
                              * GradientFill/TRIVERTEX/GRADIENT_RECT */
#include <windows.h>
#include <commdlg.h> /* GetOpenFileNameA, for the custom-logo file picker */
#include <shellapi.h> /* ShellExecuteA - opens the CSV log in whatever's associated with .csv (Excel, if installed) */
#include <gdiplus.h> /* GdipCreateBitmapFromFile etc. - see load_image_as_bitmap_gdiplus() */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "resource.h"
#include "connection.h"
#include "channels.h"
#include "sensor.h"
#include "sensor_log.h"
#include "sensor_shared.h"

/* Widened 1403 -> 1660 so the header row always has room for the
 * Ambient Temperature heatmap + its moved controls (see
 * build_controls()) even at the window's minimum/design size. */
/* Was 1904 briefly (1804 + 100 more so the heatmap read wider), then
 * 1804 - both pushed the window's own MINIMUM size past what fits on a
 * real screen, so the window (which starts maximized, but can never
 * shrink below this) ended up wider than the desktop and sat partway
 * off-screen with no way to see or reach the clipped side - direct
 * report, real screen, not a Wine/sandbox quirk. Back to 1660 (the
 * width from before ANY of this session's heatmap/card changes) -
 * direct follow-up request to shrink the heatmap specifically: it
 * fills whatever's left of this width (see add_sensor_heatmap()'s own
 * call), so this also takes it back to its original ~430px size,
 * while CARD_W still fits inside 1660 with room to spare, see
 * GRID_RIGHT's own comment). */
#define CLIENT_WIDTH  1764 /* was 1660 - grown by the same 104px COMMANDS_COL_W
                             * grew by (176->280). This is the window's
                             * enforced MINIMUM width floor (WM_GETMINMAXINFO)
                             * - widening Commands shifted AVG TEMP/Highest
                             * Temp Today/Mode all 104px further right without
                             * this floor growing to match, so a real window
                             * sized near the old 1660 minimum genuinely ran
                             * out of room and Mode's icon got clipped against
                             * the panel's own right edge - direct report,
                             * confirmed on real hardware, never caught in
                             * this sandbox since testing always force-resizes
                             * to 1920x1200, well above either floor. */
#define CLIENT_HEIGHT 794 /* was 702, then 770 - grown by the Quick Actions
                            * panel's own footprint (QUICK_PANEL_H + CARD_GAP)
                            * above Spectrum, so the minimum window size still
                            * leaves Spectrum/Activity Log exactly as much
                            * room as before instead of shrinking to make
                            * space for the new panel (770 was QUICK_PANEL_H=56;
                            * 794 accounts for its 2nd row, QUICK_PANEL_H=80,
                            * added for the Light/Dark toggle). */

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
#define HEADER_H     180 /* back to its original size - the heatmap moved
                            * out to the dead-space strip, so this zone
                            * only needs to fit Ambient Temperature's
                            * commands again, same as Connection &
                            * Settings/Bulk Actions */

#define CONTENT_TOP  194 /* shifts down by the same 12px HEADER_H grew,
                            * keeping the usual 8px gap below the panel */

/* Connection & Settings, Ambient Temperature and Bulk Actions each get
 * a flat x-offset added to every one of that section's own coordinates
 * below, rather than hand-recomputing each one, so the original
 * per-control layout numbers stay visible/auditable in the source.
 *
 * Header row order (direct request): Connection & Settings, then
 * Ambient Temperature right beside it, then the heatmap filling the
 * gap between Ambient Temperature and Bulk Actions (so it reads as
 * centered), then Bulk Actions in the header's own right corner.
 * CONN_X_SHIFT is unchanged from before this reorder - Connection &
 * Settings stays put on the far left. AMBIENT_X_SHIFT and
 * BULK_X_SHIFT are calibrated against this app's actual maximized
 * width (1920 - see CLAUDE.md/this session's own testing, not the
 * CLIENT_WIDTH=1660 design-minimum): Bulk Actions is fixed-position
 * like the other two sections (not resize-aware the way the heatmap
 * is - see add_sensor_heatmap()'s own call below), so at exactly the
 * 1660px minimum window size it would run past the header's right
 * edge. Not a practical issue - SW_SHOWMAXIMIZED means the window is
 * always at the real screen width in normal use - but worth knowing
 * if this window is ever manually shrunk to its floor size.
 *
 * CONN_X_SHIFT still caps relative to AMBIENT_X_SHIFT the same way it
 * used to relative to BULK_X_SHIFT: Connection & Settings' widest
 * control (right edge at 301+CONN_X_SHIFT = 557) must stay clear of
 * Ambient Temperature's new leftmost control (1025+AMBIENT_X_SHIFT =
 * 597) - a 40px gap right now. Moving either shift needs the other
 * nudged to keep that gap, same as the old CONN/BULK relationship. */
#define CONN_X_SHIFT 256
#define AMBIENT_X_SHIFT (-428)
/* BULK_X_SHIFT was 1002 (right edge flush with the panel's own 30px
 * corner-rivet margin, same convention as the heatmap's old right
 * edge) - direct report the High/Mid/Low/Off column read as crowded
 * against the panel's rounded corner there. Pulled left 80px for real
 * breathing room (110px total from the panel's right edge now);
 * HEATMAP_RIGHT below moves the same 80px to keep the same 40px gap
 * to Bulk Actions' new left edge. */
#define BULK_X_SHIFT 922

/* Heatmap fills the gap between Ambient Temperature and Bulk Actions -
 * see add_sensor_heatmap()'s own call in build_controls() for the full
 * derivation of these two numbers. */
#define HEATMAP_LEFT 781
#define HEATMAP_RIGHT 1352

static const int BAUD_OPTIONS[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 2000000 };
#define BAUD_OPTIONS_COUNT 9
#define BAUD_DEFAULT_INDEX 4 /* 115200 */

static const int DATABITS_OPTIONS[] = { 5, 6, 7, 8 };
#define DATABITS_OPTIONS_COUNT 4
#define DATABITS_DEFAULT_INDEX 3 /* 8 */

static const char *const PARITY_LABELS[] = { "None", "Odd", "Even", "Mark", "Space" };
static const char PARITY_CODES[] = { 'N', 'O', 'E', 'M', 'S' };
#define PARITY_OPTIONS_COUNT 5

static const char *const LEVEL_LABELS[] = { "Off", "Low", "Mid", "High" };

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
#define KILL_SWITCH_THRESHOLD_C 40.0f

/* Modbus slave address each of the 4 physical sensors is wired to - now
 * defined in sensor.h (SENSOR_UNIT_ADDR_DEFAULT) since sensor_service.c
 * needs the exact same table; pushed into the sensor at WM_CREATE via
 * sensor_set_unit_address(). */

/* HelixDefender Dark palette - same as the single-channel app. Was a
 * block of #define constants; now runtime-mutable globals so
 * apply_theme() can swap every one of them to its Light equivalent on
 * a Light/Dark toggle (see IDC_THEME_TOGGLE_BTN) - values set once,
 * unconditionally, by apply_theme() at startup (WinMain, before the
 * window is even created) rather than duplicated here as initializers,
 * so there's exactly one place (apply_theme()) that ever needs both
 * palettes. Every one of these was checked for a compile-time-constant
 * use (static array initializer, switch case, #if) before converting -
 * there were none except g_shadow_color's own initializer just below,
 * which had to change from `static const` to plain `static` for the
 * same reason. */
static COLORREF COLOR_APP_PAGE_BG;
static COLORREF COLOR_APP_PANEL_BG;
static COLORREF COLOR_APP_TEXT;
static COLORREF COLOR_APP_MUTED;
static COLORREF COLOR_APP_ACCENT;
/* Light mode still keeps this a flat neutral gray, not a lighter shade
 * of COLOR_APP_ACCENT's blue - same reasoning as the dark palette's own
 * comment (a disabled/muted control must not read as "the same color"
 * as an enabled one). */
static COLORREF COLOR_APP_ACCENT_DIS;
static COLORREF COLOR_APP_HEADER;
static COLORREF COLOR_APP_FIELD_BG;
static COLORREF COLOR_APP_CONNECTED;
static COLORREF COLOR_APP_DISCONNECTED;
static COLORREF COLOR_APP_DOT;
static COLORREF COLOR_APP_PANEL_BORDER;
static COLORREF COLOR_APP_SHADOW;

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
#define CARD_W 272 /* was 236 - the HelixDefender mark + signal-wave pulse
                     * that used to animate in the dead space right of the
                     * grid are gone (direct request - it wasn't doing
                     * anything but decoration), so that space went into
                     * making the cards themselves a little bigger instead
                     * of sitting unused. */
#define CARD_H 110 /* was 102 - grown by what HEADER_H gave up above */
#define CARD_GAP 12 /* was 8 - "Direction B" wants more generous spacing */
#define GRID_LEFT 450 /* was 420 - shifted right by the same 30px SIDEBAR_W grew by.
                        * Base/minimum value now - see grid_left_for(). */
#define GRID_TOP CONTENT_TOP

/* Cards used to stay fixed at CARD_W x CARD_H no matter how tall (or
 * wide) the window got, leaving dead space below row 4 and/or right of
 * the grid on anything bigger than the designed minimum. Height-only
 * growth was tried once, alone, and reverted for looking oversized at
 * fullscreen - but that was before this app had anything else to do
 * with extra WIDTH (the dead strip used to hold the animated
 * HelixDefender mark/signal-wave pulse, since removed - direct report
 * of it just sitting there as wasted blank background once the strip's
 * only occupant was gone). channel_card_width()/sidebar_width_for()
 * below now grow CARD_W/SIDEBAR_W together with the window, each capped
 * by its own _MAX so a very wide window doesn't turn either into
 * something absurd - see relayout_for_size(). */
#define CARD_H_MAX 160 /* was 220 - direct request for shorter, wider cards
                         * instead of tall/squarish ones once extra window
                         * height was available to grow into. (Briefly
                         * lowered to 130 on a misreading of a later
                         * request about the Mode icon's size, not the
                         * grid's - reverted.) */
#define CARD_W_MAX 400 /* was 340 - raised alongside the grid's bigger share
                         * of extra width below (was 60%, now 75%). */
#define GRID_BOTTOM_MARGIN 20 /* matches the visual weight of CONTENT_TOP's own top margin */

/* Summary card lives below the 4x4 channel grid, in the main content
 * area (not the sidebar) - direct request: "create a large card below
 * [the channel cards] that i plan to make it like a summary". The
 * grid's own height clamps at CARD_H_MAX (see channel_card_height()),
 * so on anything taller than the CLIENT_HEIGHT design baseline (the
 * normal case - this app launches maximized) real empty space opens
 * up below row 4; this card fills it instead of leaving it dead, the
 * same problem log_panel_y_for()'s own comment describes for Spectrum/
 * Activity Log. GRID_BOTTOM/SUMMARY_PANEL_H here are only the
 * WM_CREATE-time guess built from the static design-time constants
 * (CARD_H, not the dynamic channel_card_height()) - same pattern
 * CARD_H/CARD_W's own initial channel-card placement already uses;
 * relayout_for_size() recomputes both for real once the window's
 * actual size is known, same as it does for the grid itself. */
#define GRID_BOTTOM (GRID_TOP + GRID_ROWS * CARD_H + (GRID_ROWS - 1) * CARD_GAP)
#define SUMMARY_PANEL_Y (GRID_BOTTOM + CARD_GAP)
#define SUMMARY_PANEL_H (CLIENT_HEIGHT - GRID_BOTTOM_MARGIN - SUMMARY_PANEL_Y)

/* Mode toggle (relocated IDC_THEME_TOGGLE_BTN - see its WM_DRAWITEM
 * comment) sits top-right in the Summary card, icon only, no button
 * background - direct request, large and deliberately prominent since
 * more content is planned to join it there. Right-anchored to the
 * card's own right edge, same margin convention as Open Log/its
 * caption in the Command Panel. */
#define MODE_ICON_SIZE 220 /* was 64, then 112 - still "too small" both
                             * times (direct feedback) - the whole white
                             * space around the icon in the Summary card
                             * was meant to BE the button, not padding
                             * around a small icon inside a mostly-empty
                             * card. Glyph dimensions in WM_DRAWITEM
                             * scale together with this, same reasoning
                             * as the 64->112 jump. */
#define MODE_ICON_MARGIN 60 /* was 12 - direct report of the icon getting
                              * clipped against the panel's own right edge;
                              * a lot more clearance here regardless of the
                              * exact cause, since this is strictly safer
                              * either way (mode_x just moves left). */

/* Summary card's own internal row layout, top to bottom: the header
 * (icon+"Summary", unchanged, y+10) - then ONE content row, Commands
 * as its leftmost column lined up with AVG TEMP/Highest Temp Today/
 * Mode (direct correction - it was sitting in its own separate spot
 * instead of matching "the others"). All 4 captions share the same Y
 * (SUMMARY_CONTENT_Y) and all 4 content blocks share the same Y
 * (SUMMARY_CONTENT_BLOCK_Y), same as AVG TEMP/Highest/Mode always did
 * with each other. Commands' own content is a 2x2 button grid (Close
 * All/Open All on row 1, Open Log/Icon on row 2) with Reset to Default
 * spanning both columns below it, all within COMMANDS_COL_W so AVG
 * TEMP starts right after it (see avg_right's own comment further
 * down, which already recenters Highest Temp off of AVG TEMP's real
 * start - inserting a column before it needed no other change there).
 * 3 rows of SUMMARY_CMD_ROW_H (64) with 14px gaps fill the exact same
 * MODE_ICON_SIZE (220) height as the pill/icon blocks below its own
 * caption (64*3 + 14*2 = 220) - direct correction, the buttons were
 * still the original single-row panel's 36px tall, leaving Commands
 * looking short with dead space under it next to the other 3 columns.
 * COMMANDS_COL_W itself grown 176 -> 280 (direct request - it read as
 * noticeably narrower than AVG TEMP/Highest Temp/Mode's own footprint
 * next to it) - SUMMARY_CMD_BTN_W/COL2_X derive the 2 button columns'
 * width/spacing from it instead of the old hardcoded 84/104, so this
 * is the only number to change to resize the whole block again. */
#define COMMANDS_COL_W 280
#define COMMANDS_COL_GAP 40
#define SUMMARY_CMD_BTN_GAP 8
#define SUMMARY_CMD_BTN_W ((COMMANDS_COL_W - SUMMARY_CMD_BTN_GAP) / 2)
#define SUMMARY_CMD_COL2_X (12 + SUMMARY_CMD_BTN_W + SUMMARY_CMD_BTN_GAP)
#define SUMMARY_CMD_ROW_H 64
#define SUMMARY_CMD_ROW1_Y 0
#define SUMMARY_CMD_ROW2_Y 78
#define SUMMARY_CMD_RESET_Y 156
#define SUMMARY_CONTENT_Y 40
#define SUMMARY_CONTENT_BLOCK_Y 60

/* Summary card's minimum content height - SUMMARY_CONTENT_BLOCK_Y (the
 * 4 content blocks' own top offset, which already accounts for the
 * header above it) + the MODE_ICON_SIZE square + a
 * bottom breather (20). Commands' own content (124px tall) fits well
 * inside that same MODE_ICON_SIZE budget. channel_card_height() reserves this (plus a
 * CARD_GAP) off the top of the grid's own budget so the grid shrinks
 * first on a short window instead of the Summary card being forced to
 * this floor with nowhere left to put it (direct report: worked at
 * 1920x1200, overflowed off the bottom of the window at 1920x1080 -
 * the grid was maxing cards out at CARD_H_MAX with no idea the Summary
 * card still needed room below it). relayout_for_size() still floors
 * summary_h at this value too, as a safety net for windows shorter
 * than even the reservation accounts for. */
#define MIN_SUMMARY_H (SUMMARY_CONTENT_BLOCK_Y + MODE_ICON_SIZE + 20)

/* Highest Temp Today block's own width - wider than MODE_ICON_SIZE
 * since "<temp>C - BAY<N>" is a much longer string than AVG TEMP's
 * bare number, even at the smaller g_big_font. Height stays
 * MODE_ICON_SIZE so its row still lines up with the other two. Grown
 * 460->560 alongside g_big_font's own bump (56->72, see its comment) -
 * same clipping risk AVG_TEMP_BLOCK_W's comment describes, just for
 * the longer "<temp>C - BAY<N>" string. */
#define HIGHEST_TEMP_BLOCK_W 560

/* AVG TEMP block's own width - direct report: "24.6C" at the enlarged
 * g_huge_font (-84, see its own comment) no longer fit inside
 * MODE_ICON_SIZE (220), clipping off the left edge (centered text
 * whose measured width exceeds its box centers into negative x, and a
 * control's own DC clips drawing to its own bounds regardless of what
 * the app tries to paint past them). Left-anchored at GRID_LEFT + 12
 * same as before, just wider - plenty of clearance before the
 * Highest Temp block, which is centered independently. Grown again
 * 380->420 alongside g_huge_font's own bump (84->100, see its
 * comment) - same reasoning, bigger text needs a bigger box. */
#define AVG_TEMP_BLOCK_W 420

#define SIDEBAR_X 10
#define SIDEBAR_W 430 /* was 400 - grew along with CARD_W once the signal-
                        * wave dead space wasn't needed for anything else;
                        * GRID_LEFT shifted right by the same 30px to keep
                        * its gap off the sidebar's own right edge. Base/
                        * minimum value now - see sidebar_width_for(). */
#define SIDEBAR_W_MAX 600

/* Command Panel used to be its own small panel here, above Spectrum -
 * moved onto the Summary card as its own row (direct request, see
 * COMMANDS_ROW_Y/COMMANDS_ROW_H below) - Spectrum now starts right
 * where it used to sit. */
#define SIDEBAR_CONTENT_TOP CONTENT_TOP

/* Bottom edge now tracks the window's own client height directly (see
 * log_panel_y_for()), not the channel grid's - the grid's height is
 * clamped by CARD_H_MAX, which used to leave Spectrum/Activity Log
 * stuck at a fixed max size while a much taller window (e.g. the real
 * 1920x1200 ThinkPad) still had a large dead strip below them. LOG_PANEL_H
 * itself grew 220 -> 260 for a real baseline increase on top of that -
 * both direct requests ("increase the logs height and the spectrum
 * height also now since theres too much space"). This macro is only the
 * WM_CREATE-time initial guess (matches log_panel_y_for(CLIENT_HEIGHT)
 * exactly) - relayout_for_size() immediately supersedes it with the
 * real client height once the window is shown. */
#define LOG_PANEL_H 260
#define LOG_PANEL_Y (CLIENT_HEIGHT - GRID_BOTTOM_MARGIN - LOG_PANEL_H)

static HINSTANCE g_hinst;
static HWND g_hwnd;
static HWND g_log_view_hwnd; /* NULL when no full-log popup is open - see
                               * on_log_view_clicked()/log_view_wnd_proc(). */
static HWND g_templog_view_hwnd; /* Same, for the Highest Temp Log popup -
                                   * see on_highest_temp_log_clicked()/
                                   * templog_view_wnd_proc(). */
static HFONT g_font;
static HFONT g_header_font;
static HFONT g_logo_font; /* bold, letter-spaced wordmark under the logo mark */
static HFONT g_mono_font; /* fixed-width, for numeric instrument readouts -
                            * per-channel uptime, the Avg pill, heatmap BAY
                            * labels - so digits align like real lab/rack
                            * instrumentation instead of proportional UI type */
static HFONT g_small_font; /* smaller than g_font - the per-channel
                             * frequency range label, direct request
                             * after the default size ran wide/large */
static HFONT g_huge_font; /* Summary card's AVG TEMP number - sized to
                            * visually match the Mode icon's footprint,
                            * direct request the two "sync well" as
                            * content on the same card. Fixed-width for
                            * the same digit-alignment reasoning as
                            * g_mono_font, just much bigger. Bumped again
                            * (direct request - "increase the text size
                            * of avg temp number") past what the 3rd,
                            * narrower Highest Temp block can also fit -
                            * see g_big_font below for that one. */
static HFONT g_big_font; /* Highest Temp Today's own number - the
                           * original (smaller) g_huge_font size, kept
                           * as its own font once g_huge_font itself
                           * grew past what fits next to a bay label. */
/* NULL = draw the built-in vector HelixDefender mark (the normal case).
 * Set by load_custom_logo() at startup (if branding.bmp exists next to
 * the .exe) or by browse_and_set_logo() (IDC_CHANGE_LOGO_BTN) - either
 * way, once non-NULL, the header paints this bitmap instead. See
 * get_branding_bmp_path()'s comment for why the file lives at a fixed
 * name rather than wherever the user originally picked it from. */
static HBITMAP g_custom_logo_bmp;
/* Taskbar/title-bar/alt-tab icons derived from g_custom_logo_bmp - see
 * apply_custom_app_icon(). NULL means the window is still showing the
 * icon it was created with (the embedded IDI_APP_ICON resource). */
static HICON g_custom_icon_big;
static HICON g_custom_icon_small;
/* The embedded IDI_APP_ICON handles WinMain originally loaded for the
 * window class - kept around (not just left local to WinMain) so
 * IDC_RESET_LOGO_BTN can hand them back to WM_SETICON and genuinely
 * restore the original icon, not just stop showing a custom one. */
static HICON g_default_icon_big;
static HICON g_default_icon_small;

/* The shared admin-unlock flag - gates both arming Continuous Wave (CW,
 * a fixed undithered carrier) via a channel's Set/Bulk Set, AND the
 * Command Panel's "Icon" button popup menu (Change Logo/Reset).
 * The real password comes from the vendor DLL itself (Transit.dll's
 * GetDllPassword export - confirmed to take no arguments and return a
 * pointer to a static string it already has baked in, not anything
 * hardware/dongle-dependent - see transit_dll.h's header comment),
 * never anything this app invents or stores on its own. Authorized once
 * per run - unlocking through either entry point covers both for the
 * rest of the session instead of re-prompting per click. See
 * unlock_cw(). */
static bool g_cw_authorized;
static char g_cw_pw_input[64]; /* transient scratch for cw_password_dlg_proc() */

/* Per-channel cumulative ON-time - an odometer, not an app-uptime
 * counter: each of the 16 channels tracks its OWN time actually
 * transmitting, not how long the app process itself has been open.
 * Loaded from the .ini per channel (alongside Mode/Level/Output) in
 * load_channel_settings(), ticked every WM_TIMER firing, shown live on
 * each card, and saved back periodically (not just at WM_DESTROY) so a
 * crash only loses a few seconds of credit. Uses GetTickCount64() rather
 * than GetTickCount() so a channel run spanning ~49.7 days doesn't wrap
 * around into a garbage elapsed time.
 *
 * g_channel_uptime_base_seconds[i] is everything accumulated BEFORE the
 * channel's current ON period (or the whole total, while it's off);
 * g_channel_on_since_ms[i] is when the current ON period started
 * (meaningful only while output_on); g_channel_on_prev[i] is last tick's
 * output_on, used only to detect the OFF->ON/ON->OFF edge. See
 * channel_uptime_seconds() and WM_TIMER's per-tick accounting. */
static ULONGLONG g_channel_uptime_base_seconds[MAX_CHANNELS];
static ULONGLONG g_channel_on_since_ms[MAX_CHANNELS];
static bool g_channel_on_prev[MAX_CHANNELS];
static int g_uptime_tick_counter;

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
static HBRUSH g_brush_reset_default; /* Reset to Default's own light-gray
                                       * fill (black text on top) - direct
                                       * request, distinct from the dark
                                       * COLOR_APP_MUTED every other
                                       * "off/neutral" fill here uses. */
static HBRUSH g_brush_shadow;
static COLORREF g_shadow_color; /* was `static const` - now set by
                                  * create_theme_brushes(), see its own
                                  * comment on why COLOR_APP_SHADOW can no
                                  * longer be a compile-time constant. */
static HBRUSH g_brush_dot_pattern; /* tiled DOT_GRID_SPACING x DOT_GRID_SPACING bitmap brush */
static HBITMAP g_dot_pattern_bmp;
static bool g_light_mode;

static Connection g_conn;
static Sensor g_sensor;
/* Read-only view onto the background sensor service's shared-memory
 * block (see sensor_shared.h) - opened lazily/retried below (the
 * service may install or start after this app already has), never
 * written to from here. When its data is fresh, WM_TIMER prefers it
 * over polling the sensor port directly, since the service (once
 * installed) owns that port exclusively. */
static SensorShared g_sensor_shared;
static bool g_kill_switch_tripped[MAX_CHANNELS];

/* Bulk Actions selection - click a card's checkbox to toggle it in/out,
 * then the Bulk Actions bar applies to every selected channel at once.
 * The bar and every card's checkbox are always visible - nothing hides
 * behind this. */
static bool g_channel_selected[MAX_CHANNELS];

/* Whether clicking a card's own plain background (not its checkbox)
 * also toggles selection - off by default so a stray click doesn't
 * silently select something. IDC_BULK_TOGGLE_BTN arms/disarms this;
 * the checkbox itself always works regardless.
 *
 * This does NOT go through the card panel's own click handling (see
 * card_panel_subclass_proc's comment for why an SS_NOTIFY-based
 * version of this broke every button on the card) - it's detected via
 * WM_LBUTTONDOWN on the main window instead (see that case in
 * WndProc): a click only reaches hwnd's own WM_LBUTTONDOWN when it
 * lands on truly empty space no child control claims, since the card
 * panel (no SS_NOTIFY) is transparent to hit-testing and every real
 * button/combo/gauge still claims its own clicks first, completely
 * unaffected by this. */
static bool g_bulk_select_mode;

/* Bulk ON/OFF are one-shot actions (see their WM_DRAWITEM comment) - no
 * real per-button "state" to reflect. Direct request was for whichever
 * one was clicked most recently to still show a lasting, visible marker
 * (a bright border - see the drawing code) distinguishing it from the
 * other, purely cosmetic/session-local, until the other one is clicked.
 * Reset on disconnect (set_channel_controls_enabled(false)) since a
 * stale marker from a previous session reads as a live state. */
enum { BULK_POWER_NONE, BULK_POWER_ON, BULK_POWER_OFF };
static int g_bulk_last_power_action = BULK_POWER_NONE;

/* Spectrum panel state - true shows the all-16 overview grid (the
 * default), false shows one channel's trace full-size, with
 * g_spectrum_unit (0..MAX_CHANNELS-1) picking which. */
static bool g_spectrum_show_all = true;
static int g_spectrum_unit;

/* Handles needed to reposition things on WM_SIZE that don't otherwise
 * have a retrievable control ID (channel_*_id() covers everything else
 * per-card - GetDlgItem() finds those directly). */
static HWND g_header_panel;
static HWND g_quick_panel_label; /* "Commands" - leftmost column of the same
                                   * content row as AVG TEMP/Highest Temp Today/
                                   * Mode, same Y as their captions, centered over
                                   * COMMANDS_COL_W (176, matching Reset to
                                   * Default's width below it) in plain g_font -
                                   * direct correction, it wasn't lining up with
                                   * them before. */
static HWND g_summary_panel; /* Placeholder "Summary" card, below the channel grid */
static HWND g_summary_header_icon; /* Same reposition-needs-its-own-handle
                                     * reasoning as g_log_header_icon/
                                     * g_log_header_lbl below - this card's Y
                                     * moves with the grid's actual height
                                     * too (see GRID_BOTTOM's comment). */
static HWND g_summary_header_lbl;
static HWND g_mode_caption_lbl; /* Right-anchored, needs its own handle to reposition - see MODE_ICON_SIZE's comment */
static HWND g_mode_toggle_btn;
static HWND g_avg_temp_caption_lbl; /* Left-anchored mirror of g_mode_caption_lbl/g_mode_toggle_btn - see avg_temp_block_subclass_proc() */
static HWND g_avg_temp_block;
static HWND g_highest_temp_caption_lbl; /* Centered between the two above - see highest_temp_block_subclass_proc() */
static HWND g_highest_temp_block;
/* Summary card's "Highest Temp Today" - a running max across all 4 bays
 * since local midnight, not just whatever the 4 live readings happen to
 * be on this exact tick (direct correction - "oh yeah Highest Temp
 * TODAY", not "highest right now"). Rolls over on the first tick that
 * sees a new local date, same "compare stored date, reset if it moved"
 * idiom sensor_log_start_new_week() uses for its own weekly rotation -
 * in-memory only, not persisted across an app restart (nothing asked
 * for that yet). Updated by update_highest_temp_today(), read by
 * highest_temp_block_subclass_proc(). */
static bool g_highest_temp_today_valid;
static float g_highest_temp_today_c;
static int g_highest_temp_today_bay = -1;
static WORD g_highest_temp_today_day, g_highest_temp_today_month, g_highest_temp_today_year;
static HWND g_sidebar_panel;
static HWND g_log_header_icon; /* "Activity Log" icon+label - pinned under
                                 * the Spectrum plot at a Y that moves with
                                 * the grid's actual height (see
                                 * log_panel_y_for()), so unlike most of
                                 * the sidebar's static labels these need
                                 * their own handle to reposition. */
static HWND g_log_header_lbl;
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
static HWND g_sensor_heatmap;
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

/* Forward declaration - defined further down, but panel_subclass_proc
 * below needs it for the header's logo mark. */
static void draw_app_logo_mark(HDC hdc, int cx, int cy, int scale);
static void draw_app_logo_silhouette(HDC hdc, int cx, int cy, int scale, COLORREF color);

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
         * in a real card/ticket corner would - filled solid silver
         * (like a rivet/bolt head), not the tiled dot-pattern brush
         * used before - direct request.
         *
         * Centering it exactly on the sharp corner (rc.left, rc.top
         * etc.) was tried first and was wrong two ways: visually, most
         * of the circle then sits in the square corner area the
         * rounded panel never fills in the first place, so it read as
         * a free-floating dot outside the panel rather than near it -
         * and separately, Wine's Ellipse() with a pattern-brush fill
         * silently draws nothing at all once part of its bounding box
         * goes negative/out-of-window (confirmed by testing: swapping
         * to a plain solid-color fill made all 4 corners render fine at
         * that same position, so a bounding box that leaves the window
         * is what broke pattern-brush specifically, not a real "off").
         *
         * Direct request is for the notch to sit NEAR the corner
         * without touching the panel's own rounded edge - not
         * overlapping it. The corner's rounded boundary is an arc of
         * radius PANEL_CORNER_DIAMETER/2 centered at (radius, radius)
         * from the sharp corner; a point at distance d from that arc
         * center has clearance (radius - d) to the boundary, which is
         * LARGEST when d is smallest, i.e. right at the arc's own
         * center - inset = radius places the notch's center exactly
         * there, giving it up to `radius` px of clearance in every
         * direction before it would ever reach the edge, while still
         * sitting inside the corner region rather than out toward the
         * panel's flat middle. r is kept comfortably smaller than that
         * radius so the whole circle stays inside with room to spare.
         *
         * Header only, not the sidebar panel below (same subclass
         * proc, but this is scoped to g_header_panel specifically) -
         * direct request was for the whole header, not the Bulk
         * Actions card nested inside it. */
        if (hwnd == g_header_panel) {
            const int r = 6;
            const int inset = PANEL_CORNER_DIAMETER / 2; /* = 12 */
            POINT corners[4];
            int ci;
            corners[0].x = rc.left + inset;  corners[0].y = rc.top + inset;
            corners[1].x = rc.right - PANEL_SHADOW_PX - inset;  corners[1].y = rc.top + inset;
            corners[2].x = rc.left + inset;  corners[2].y = rc.bottom - PANEL_SHADOW_PX - inset;
            corners[3].x = rc.right - PANEL_SHADOW_PX - inset;  corners[3].y = rc.bottom - PANEL_SHADOW_PX - inset;

            {
                HBRUSH silver_brush = CreateSolidBrush(RGB(196, 199, 204));
                old_brush = (HBRUSH)SelectObject(hdc, silver_brush);
                /* A visible ring around the hole (not NULL_PEN/borderless) -
                 * a real die-cut hole has a defined edge, not just a patch
                 * of texture with no boundary. */
                pen = CreatePen(PS_SOLID, 1, RGB(120, 123, 129));
                old_pen = (HPEN)SelectObject(hdc, pen);
                for (ci = 0; ci < 4; ci++) {
                    Ellipse(hdc, corners[ci].x - r, corners[ci].y - r,
                            corners[ci].x + r, corners[ci].y + r);
                }
                SelectObject(hdc, old_pen);
                DeleteObject(pen);
                SelectObject(hdc, old_brush);
                DeleteObject(silver_brush);
            }
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
            brc.left = 444 + BULK_X_SHIFT;
            brc.top = 8;
            brc.right = 884 + BULK_X_SHIFT;
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

        /* Main app logo, in the header's own left free space (the gap
         * between the panel's left edge and Connection & Settings' own
         * content, which starts around x=282 panel-relative once
         * CONN_X_SHIFT is folded in). A white halo hugging the mark's
         * own edges, not a separate card/box: the same silhouette
         * drawn once, solid white, slightly larger (106%) and directly
         * behind the real mark - reads as a white outline/shadow right
         * at the shape's boundary. The real mark's own colors (dark
         * gray + blue) are designed for a white surface, same as the
         * reference image itself, so it still needs *some* white
         * behind it to read correctly - a full white card was tried
         * first and swapped for this per direct request.
         *
         * Shifted up from the header's vertical center (was cy=90) to
         * leave room for the HelixDefender wordmark underneath it. */
        if (hwnd == g_header_panel) {
            RECT wm_rc;
            HFONT old_font;
            int old_extra;

            /* g_custom_logo_bmp overrides the vector mark once set (see
             * browse_and_set_logo()) - scaled to fit within a fixed
             * box, aspect ratio preserved (never stretched to a square
             * regardless of the source image's own shape), centered on
             * the same (135, 68) point the vector mark uses. */
            if (g_custom_logo_bmp) {
                BITMAP bm;
                if (GetObject(g_custom_logo_bmp, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
                    const int box = 96;
                    double sx = (double)box / bm.bmWidth;
                    double sy = (double)box / bm.bmHeight;
                    double s = sx < sy ? sx : sy;
                    int dw = (int)(bm.bmWidth * s + 0.5);
                    int dh = (int)(bm.bmHeight * s + 0.5);
                    HDC mem_dc = CreateCompatibleDC(hdc);
                    HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, g_custom_logo_bmp);
                    StretchBlt(hdc, 135 - dw / 2, 68 - dh / 2, dw, dh,
                               mem_dc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                    SelectObject(mem_dc, old_bmp);
                    DeleteDC(mem_dc);
                }
            } else {
                draw_app_logo_silhouette(hdc, 135, 68, 106, RGB(255, 255, 255));
                draw_app_logo_mark(hdc, 135, 68, 100);
            }

            wm_rc.left = 20; wm_rc.top = 118; wm_rc.right = 250; wm_rc.bottom = 142;
            old_font = (HFONT)SelectObject(hdc, g_logo_font);
            /* Letter-spacing - CreateFontA has no such parameter, this
             * is the actual mechanism (extra px added after every
             * glyph) - matches the reference wordmark's wide tracking,
             * a plain default-spaced draw reads noticeably tighter/
             * different from it. */
            old_extra = SetTextCharacterExtra(hdc, 3);
            SetTextColor(hdc, COLOR_APP_TEXT);
            SetBkMode(hdc, TRANSPARENT);
            DrawTextA(hdc, "HELIX DEFENSE", -1, &wm_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SetTextCharacterExtra(hdc, old_extra);
            SelectObject(hdc, old_font);
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
 * live-read pattern as sensor_chip_subclass_proc.
 *
 * Deliberately NOT SS_NOTIFY, unlike a plain background-select overlay
 * might suggest: SS_NOTIFY on this panel was tried (to send STN_CLICKED
 * on a background click, for a "click anywhere on the card to select
 * it" feature) and it broke every real BS_OWNERDRAW sibling sitting on
 * top of it - Set/ON/OFF/the checkbox all silently stopped receiving
 * their own clicks, which instead landed on this panel as id=0
 * STN_CLICKED, no matter how correctly they were enabled/positioned/
 * topmost in z-order (confirmed with WM_COMMAND-level tracing: the
 * click's real target reported IsWindowEnabled()==TRUE at the exact
 * clicked screen coordinate, and still didn't receive it). Removing
 * SS_NOTIFY fixed all of them immediately, and it stays removed here
 * for good - a real, working button beats a background click that
 * quietly breaks the rest of the card.
 *
 * Background-click-to-select did come back (IDC_BULK_TOGGLE_BTN arms
 * it), just not through this panel: see g_bulk_select_mode's comment
 * and the WM_LBUTTONDOWN case in WndProc, which detect the click via
 * coordinate math on the main window instead of SS_NOTIFY on this
 * panel. The checkbox (channel_select_id/IDC_CH_SELECT_OFFSET) still
 * always works too, regardless of that toggle's state. */
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
        /* Gated on actually being connected, not just output_on - a
         * channel restored from the .ini at launch (channel_restore_
         * saved(), main.c) has output_on true before a single byte's
         * ever been sent this run, and lighting the card up green from
         * that alone read as "this is live" when it's really just a
         * remembered value. Direct request: every card starts neutral
         * until the link is actually up. */
        bool on = (index >= 0 && index < MAX_CHANNELS) && channels_get(index)->output_on &&
                  conn_is_connected(&g_conn);

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
         * color pen, so RoundRect doesn't draw an edge at all). A
         * selected card gets an accent-colored stroke (Bulk Actions
         * selection, takes priority since it's the user's own deliberate
         * pick); otherwise a channel that's actually ON gets a green
         * stroke instead, direct request - the ON button alone lighting
         * up wasn't enough of a signal at a glance across all 16 cards. */
        SelectObject(hdc, g_brush_panel);
        if (selected) {
            pen = CreatePen(PS_SOLID, 2, COLOR_APP_HEADER);
            old_pen = (HPEN)SelectObject(hdc, pen);
        } else if (on) {
            pen = CreatePen(PS_SOLID, 2, COLOR_APP_CONNECTED);
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
     * next time the channel turned on or off.
     *
     * No SS_NOTIFY - see the subclass proc's own comment above for why
     * (it silently ate every sibling button's clicks). */
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT | WS_CLIPSIBLINGS, x, y, w, h, 0);
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

/* Two plain diamonds (top/outer/bottom/inner, symmetric) with a
 * triangle centered between them - the real logo mark, measured
 * directly off the actual reference image's pixel coordinates. Each
 * diamond's inner vertex sits exactly at center (0,0), same point as
 * the triangle's apex; each diamond's bottom vertex lines up exactly
 * with the triangle's base corner on that side - the three shapes
 * share those edges with no gap and no overlap. File-scope so both
 * draw_app_logo_mark() and its white-halo pass below use the same
 * points. */
static const POINT LOGO_LEFT_BASE[4] = {
    { -16, -32 }, { -32, 0 }, { -16, 32 }, { 0, 0 }
};
static const POINT LOGO_BEAM_BASE[3] = {
    { 0, 0 }, { -16, 32 }, { 16, 32 }
};

static void logo_points(int cx, int cy, int scale, POINT left_pts[4], POINT right_pts[4], POINT beam_pts[3]) {
    int i;
    for (i = 0; i < 4; i++) {
        left_pts[i].x = cx + LOGO_LEFT_BASE[i].x * scale / 100;
        left_pts[i].y = cy + LOGO_LEFT_BASE[i].y * scale / 100;
        right_pts[i].x = cx - LOGO_LEFT_BASE[i].x * scale / 100;
        right_pts[i].y = cy + LOGO_LEFT_BASE[i].y * scale / 100;
    }
    for (i = 0; i < 3; i++) {
        beam_pts[i].x = cx + LOGO_BEAM_BASE[i].x * scale / 100;
        beam_pts[i].y = cy + LOGO_BEAM_BASE[i].y * scale / 100;
    }
}

/* A single-color silhouette of the whole mark (both diamonds + the
 * triangle, all filled the same color) - drawn once, slightly larger
 * than the real mark and directly behind it, so it reads as a white
 * halo hugging the shape's own edges rather than a separate card/box.
 * Direct request: "a white shadow or box within the logo's edges", as
 * an alternative to the white card tried first. */
static void draw_app_logo_silhouette(HDC hdc, int cx, int cy, int scale, COLORREF color) {
    POINT left_pts[4], right_pts[4], beam_pts[3];
    HBRUSH brush, old_brush;
    HPEN old_pen;

    logo_points(cx, cy, scale, left_pts, right_pts, beam_pts);

    old_pen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
    brush = CreateSolidBrush(color);
    old_brush = (HBRUSH)SelectObject(hdc, brush);
    Polygon(hdc, left_pts, 4);
    Polygon(hdc, right_pts, 4);
    Polygon(hdc, beam_pts, 3);
    SelectObject(hdc, old_brush);
    DeleteObject(brush);
    SelectObject(hdc, old_pen);
}

/* Main app logo mark - two dark "signal peak" shapes flanking a blue
 * upward beam, matching src/app.ico (the title-bar/taskbar icon).
 * Drawn as vector polygons rather than stretching that .ico's bitmap -
 * it's only 16x16, which blurs badly once scaled up to badge size, so
 * this is a faithful redraw at whatever size is needed instead. scale
 * is in 100ths (100 = the size these base points were designed at). */
static void draw_app_logo_mark(HDC hdc, int cx, int cy, int scale) {
    POINT left_pts[4], right_pts[4], beam_pts[3];
    HBRUSH mark_brush, old_brush;
    HPEN old_pen;

    logo_points(cx, cy, scale, left_pts, right_pts, beam_pts);

    old_pen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));

    mark_brush = CreateSolidBrush(RGB(66, 66, 66));
    old_brush = (HBRUSH)SelectObject(hdc, mark_brush);
    Polygon(hdc, left_pts, 4);
    Polygon(hdc, right_pts, 4);
    SelectObject(hdc, old_brush);
    DeleteObject(mark_brush);

    mark_brush = CreateSolidBrush(COLOR_APP_ACCENT);
    SelectObject(hdc, mark_brush);
    Polygon(hdc, beam_pts, 3);
    SelectObject(hdc, old_brush);
    DeleteObject(mark_brush);

    SelectObject(hdc, old_pen);
}

/* Faded/idle rendering of the logo mark (halo + mark) - watermarked
 * over the Ambient Temperature heatmap's blend so that spot always
 * shows something rather than being totally blank. True per-pixel
 * alpha isn't available here (no PNG/32bpp-alpha pipeline in this
 * app), so this fakes constant-opacity fade the standard GDI way:
 * snapshot the real background into an off-screen bitmap, draw the
 * normal opaque mark on top of that snapshot, then AlphaBlend the
 * WHOLE snapshot back over the same spot at partial alpha. Pixels
 * that are just background blend with themselves (no visible change);
 * only the mark's own pixels actually fade in - which is what makes
 * this work without a real alpha channel. */
static void draw_app_logo_faded(HDC hdc, int cx, int cy, int scale, BYTE alpha) {
    const int size = 100;
    int ox = cx - size / 2;
    int oy = cy - size / 2;
    HDC mem_dc;
    HBITMAP mem_bmp, old_bmp;
    BLENDFUNCTION bf;

    mem_dc = CreateCompatibleDC(hdc);
    mem_bmp = CreateCompatibleBitmap(hdc, size, size);
    old_bmp = (HBITMAP)SelectObject(mem_dc, mem_bmp);

    BitBlt(mem_dc, 0, 0, size, size, hdc, ox, oy, SRCCOPY);
    draw_app_logo_silhouette(mem_dc, size / 2, size / 2, scale * 106 / 100, RGB(255, 255, 255));
    draw_app_logo_mark(mem_dc, size / 2, size / 2, scale);

    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = alpha;
    bf.AlphaFormat = 0;
    AlphaBlend(hdc, ox, oy, size, size, mem_dc, 0, 0, size, size, bf);

    SelectObject(mem_dc, old_bmp);
    DeleteObject(mem_bmp);
    DeleteDC(mem_dc);
}

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

/* Sets every COLOR_APP_* global to its Dark or Light value based on
 * g_light_mode - the only place either palette is spelled out, so a
 * color only ever needs updating here. Does NOT touch any GDI object
 * (brushes bake in a color at creation, they don't track a variable) -
 * callers always follow this with create_theme_brushes(). */
static void apply_theme(void) {
    if (g_light_mode) {
        COLOR_APP_PAGE_BG = RGB(242, 243, 245);
        COLOR_APP_PANEL_BG = RGB(255, 255, 255);
        COLOR_APP_TEXT = RGB(28, 30, 33);
        COLOR_APP_MUTED = RGB(110, 113, 118);
        COLOR_APP_ACCENT = RGB(15, 110, 158);
        COLOR_APP_ACCENT_DIS = RGB(206, 208, 212);
        COLOR_APP_HEADER = RGB(20, 120, 170);
        COLOR_APP_FIELD_BG = RGB(236, 237, 240);
        COLOR_APP_CONNECTED = RGB(46, 155, 78);
        COLOR_APP_DISCONNECTED = RGB(198, 60, 60);
        COLOR_APP_DOT = RGB(214, 216, 220);
        COLOR_APP_PANEL_BORDER = RGB(219, 221, 225);
        COLOR_APP_SHADOW = RGB(210, 211, 214);
    } else {
        COLOR_APP_PAGE_BG = RGB(32, 33, 36);
        COLOR_APP_PANEL_BG = RGB(43, 45, 49);
        COLOR_APP_TEXT = RGB(232, 233, 234);
        COLOR_APP_MUTED = RGB(154, 156, 160);
        COLOR_APP_ACCENT = RGB(26, 133, 184);
        COLOR_APP_ACCENT_DIS = RGB(90, 90, 94);
        COLOR_APP_HEADER = RGB(58, 168, 221);
        COLOR_APP_FIELD_BG = RGB(23, 24, 26);
        COLOR_APP_CONNECTED = RGB(58, 181, 94);
        COLOR_APP_DISCONNECTED = RGB(224, 90, 90);
        COLOR_APP_DOT = RGB(50, 52, 57);
        COLOR_APP_PANEL_BORDER = RGB(63, 66, 71);
        COLOR_APP_SHADOW = RGB(14, 15, 17);
    }
}

/* (Re)creates every persistent GDI object derived from the COLOR_APP_*
 * globals - safe to call again after a theme toggle (deletes the old
 * ones first if they exist) as well as once at startup. The many OTHER
 * CreateSolidBrush(COLOR_APP_*) calls scattered through this file are
 * all local/transient (created and destroyed within a single WM_PAINT/
 * WM_DRAWITEM), so they read the current value on their own with no
 * extra work - only these long-lived globals need explicit rebuilding.
 * g_brush_level_medium is deliberately NOT touched here - it's a fixed
 * status color (matches ch_gauge_stop_color(LEVEL_MEDIUM)), not part of
 * either theme, created once at real startup and left alone. */
static void create_theme_brushes(void) {
    if (g_brush_page) DeleteObject(g_brush_page);
    if (g_brush_panel) DeleteObject(g_brush_panel);
    if (g_brush_field) DeleteObject(g_brush_field);
    if (g_brush_accent) DeleteObject(g_brush_accent);
    if (g_brush_accent_dis) DeleteObject(g_brush_accent_dis);
    if (g_brush_dot) DeleteObject(g_brush_dot);
    if (g_brush_connected) DeleteObject(g_brush_connected);
    if (g_brush_disconnected) DeleteObject(g_brush_disconnected);
    if (g_brush_level_off) DeleteObject(g_brush_level_off);
    if (g_brush_reset_default) DeleteObject(g_brush_reset_default);
    if (g_brush_shadow) DeleteObject(g_brush_shadow);
    if (g_brush_dot_pattern) DeleteObject(g_brush_dot_pattern);
    if (g_dot_pattern_bmp) DeleteObject(g_dot_pattern_bmp);

    g_brush_page = CreateSolidBrush(COLOR_APP_PAGE_BG);
    g_brush_panel = CreateSolidBrush(COLOR_APP_PANEL_BG);
    g_brush_field = CreateSolidBrush(COLOR_APP_FIELD_BG);
    g_brush_accent = CreateSolidBrush(COLOR_APP_ACCENT);
    g_brush_accent_dis = CreateSolidBrush(COLOR_APP_ACCENT_DIS);
    g_brush_dot = CreateSolidBrush(COLOR_APP_DOT);
    g_brush_connected = CreateSolidBrush(COLOR_APP_CONNECTED);
    g_brush_disconnected = CreateSolidBrush(COLOR_APP_DISCONNECTED);
    g_brush_level_off = CreateSolidBrush(COLOR_APP_MUTED);
    g_brush_reset_default = CreateSolidBrush(RGB(200, 200, 200));
    g_brush_shadow = CreateSolidBrush(COLOR_APP_SHADOW);
    g_shadow_color = COLOR_APP_SHADOW;
    build_dot_pattern_brush();
}

/* Continuous 5-stop cool-to-hot hues
 * (green -> yellow -> orange -> darker orange -> red) - originally for
 * the heatmap only, now also the Summary card's AVG TEMP gradient text
 * (avg_temp_block_subclass_proc). A discrete band function is the
 * wrong tool for the heatmap specifically: 4 real bay
 * readings a couple degrees apart (the normal case) usually land in
 * the SAME band, so all 4 corners would get an identical color and the
 * "scan" would collapse into one flat fill - the exact bug this
 * replaces. t is 0..1 (clamped), not an absolute temperature - the
 * heatmap auto-scales to the current spread of the 4 live readings
 * (see sensor_heatmap_subclass_proc) so even a 1C difference between
 * bays stays visibly distinct instead of vanishing into one band.
 * Green (not blue) as the cool end - direct request/reference image:
 * a conventional green-safe/red-hot heatmap read, not this app's own
 * accent-blue used everywhere else in the UI (which read as just
 * another shade of the app's own chrome here, not "cool"). */
static COLORREF vivid_thermal_color(float t) {
    static const COLORREF stops[] = {
        RGB(70, 170, 90), RGB(210, 190, 60), RGB(224, 146, 34), RGB(196, 90, 24), RGB(214, 64, 56)
    };
    const int n = (int)(sizeof(stops) / sizeof(stops[0]));
    float scaled;
    int idx;
    float frac;

    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    scaled = t * (float)(n - 1);
    idx = (int)scaled;
    if (idx >= n - 1) idx = n - 2;
    frac = scaled - (float)idx;

    return RGB(
        GetRValue(stops[idx]) + (BYTE)((GetRValue(stops[idx + 1]) - GetRValue(stops[idx])) * frac),
        GetGValue(stops[idx]) + (BYTE)((GetGValue(stops[idx + 1]) - GetGValue(stops[idx])) * frac),
        GetBValue(stops[idx]) + (BYTE)((GetBValue(stops[idx + 1]) - GetBValue(stops[idx])) * frac));
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

/* Solid-color constant-alpha fill over r, respecting hdc's current
 * clip region (so a clip set to an ellipse region before calling this
 * yields a soft-edged translucent fill) - same 1x1-stretched-bitmap-
 * plus-AlphaBlend idiom draw_app_logo_faded() uses for its fade, just
 * filling flat color instead of a snapshot. Used by the heatmap's per-
 * bay radial glows. */
static void alpha_fill_rect(HDC hdc, RECT r, COLORREF color, BYTE alpha) {
    HDC mem_dc;
    HBITMAP mem_bmp, old_bmp;
    BLENDFUNCTION bf;
    int w = r.right - r.left, h = r.bottom - r.top;

    if (w <= 0 || h <= 0) {
        return;
    }

    mem_dc = CreateCompatibleDC(hdc);
    mem_bmp = CreateCompatibleBitmap(hdc, 1, 1);
    old_bmp = (HBITMAP)SelectObject(mem_dc, mem_bmp);
    SetPixelV(mem_dc, 0, 0, color);

    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = alpha;
    bf.AlphaFormat = 0;
    AlphaBlend(hdc, r.left, r.top, w, h, mem_dc, 0, 0, 1, 1, bf);

    SelectObject(mem_dc, old_bmp);
    DeleteObject(mem_bmp);
    DeleteDC(mem_dc);
}

/* Summary card's AVG TEMP block - left side, mirroring the Mode icon on
 * the right (same MODE_ICON_SIZE square footprint, same caption-above-
 * content row, direct request that the two "sync well" as content on
 * the same card). Direct correction - the gradient belongs on the
 * TEXT itself, not the block background (first version had it
 * backwards). Flat field-colored block, and the number's own glyph
 * shapes get filled with a real green-to-red gradient: BeginPath/
 * TextOutA/EndPath records the text's outline as the DC's current
 * path, PathToRegion turns that into a clip region shaped exactly
 * like the glyphs, then gradient_fill_rect paints through it - the
 * same building block SetTextColor can't do (GDI text is always a
 * single flat color otherwise). */
static LRESULT CALLBACK avg_temp_block_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        char text[16];
        int text_len;
        float avg_c;
        bool has_avg = sensor_average_temperature(&g_sensor, &avg_c);
        HFONT old_font;
        SIZE text_size;
        int text_x, text_y;
        HRGN text_rgn;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        /* No block/border - blends straight into the panel behind it,
         * same "icon only, no badge" treatment the Mode toggle got
         * (direct correction - the two sides looked inconsistent with
         * one boxed and one bare). */
        FillRect(hdc, &rc, g_brush_panel);

        if (has_avg) {
            old_font = (HFONT)SelectObject(hdc, g_huge_font);
            wsprintfA(text, "%d.%dC", (int)avg_c, (int)(avg_c * 10) % 10);
            text_len = lstrlenA(text);
            GetTextExtentPoint32A(hdc, text, text_len, &text_size);
            text_x = rc.left + ((rc.right - rc.left) - text_size.cx) / 2;
            text_y = rc.top + ((rc.bottom - rc.top) - text_size.cy) / 2;

            SetBkMode(hdc, TRANSPARENT);
            BeginPath(hdc);
            TextOutA(hdc, text_x, text_y, text, text_len);
            EndPath(hdc);
            text_rgn = PathToRegion(hdc);
            if (text_rgn) {
                SelectClipRgn(hdc, text_rgn);
                gradient_fill_rect(hdc, rc, vivid_thermal_color(0.0f), vivid_thermal_color(1.0f), false);
                SelectClipRgn(hdc, NULL);
                DeleteObject(text_rgn);
            }
        } else {
            /* Plain muted text at g_header_font (13pt, matches the "AVG
             * TEMP" caption above it), not g_huge_font (100pt) - direct
             * correction, "--.-C" at the same giant size a real reading
             * uses looked oversized/out of place for a placeholder. No
             * gradient-clip either, same reasoning as before (a single
             * "-" glyph run through that effect rendered as a tiny
             * unreadable colored smudge - direct report). */
            old_font = (HFONT)SelectObject(hdc, g_header_font);
            lstrcpynA(text, "--.-C", (int)sizeof(text));
            text_len = lstrlenA(text);
            GetTextExtentPoint32A(hdc, text, text_len, &text_size);
            text_x = rc.left + ((rc.right - rc.left) - text_size.cx) / 2;
            text_y = rc.top + ((rc.bottom - rc.top) - text_size.cy) / 2;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, COLOR_APP_MUTED);
            TextOutA(hdc, text_x, text_y, text, text_len);
        }
        SelectObject(hdc, old_font);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

/* Summary card's 3rd content block, centered between AVG TEMP and Mode -
 * "Highest Temp Today" (update_highest_temp_today()'s running max, not
 * a live snapshot - see that function's own comment for why). Same
 * bare-no-block, gradient-on-the-glyphs treatment as avg_temp_block_
 * subclass_proc, just a smaller font (g_big_font, not g_huge_font) so
 * "<temp>C - BAY<N>" still fits on one line. */
static LRESULT CALLBACK highest_temp_block_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        char text[32];
        int text_len;
        HFONT old_font;
        SIZE text_size;
        int text_x, text_y;
        HRGN text_rgn;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_brush_panel);

        if (g_highest_temp_today_valid) {
            old_font = (HFONT)SelectObject(hdc, g_big_font);
            wsprintfA(text, "%d.%dC - BAY%d", (int)g_highest_temp_today_c,
                      (int)(g_highest_temp_today_c * 10) % 10, g_highest_temp_today_bay + 1);
            text_len = lstrlenA(text);
            GetTextExtentPoint32A(hdc, text, text_len, &text_size);
            text_x = rc.left + ((rc.right - rc.left) - text_size.cx) / 2;
            text_y = rc.top + ((rc.bottom - rc.top) - text_size.cy) / 2;

            SetBkMode(hdc, TRANSPARENT);
            BeginPath(hdc);
            TextOutA(hdc, text_x, text_y, text, text_len);
            EndPath(hdc);
            text_rgn = PathToRegion(hdc);
            if (text_rgn) {
                SelectClipRgn(hdc, text_rgn);
                gradient_fill_rect(hdc, rc, vivid_thermal_color(0.0f), vivid_thermal_color(1.0f), false);
                SelectClipRgn(hdc, NULL);
                DeleteObject(text_rgn);
            }
        } else {
            /* Same fix as avg_temp_block_subclass_proc's own "-" glyph -
             * plain muted text at g_header_font (not g_big_font/72pt -
             * direct correction, looked oversized for a placeholder)
             * instead of a single dash run through the gradient-clip-
             * to-text-path effect (rendered as a tiny unreadable
             * colored smudge, direct report). */
            old_font = (HFONT)SelectObject(hdc, g_header_font);
            lstrcpynA(text, "No Data", (int)sizeof(text));
            text_len = lstrlenA(text);
            GetTextExtentPoint32A(hdc, text, text_len, &text_size);
            text_x = rc.left + ((rc.right - rc.left) - text_size.cx) / 2;
            text_y = rc.top + ((rc.bottom - rc.top) - text_size.cy) / 2;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, COLOR_APP_MUTED);
            TextOutA(hdc, text_x, text_y, text, text_len);
        }
        SelectObject(hdc, old_font);

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

/* "Vivid Thermal Scan" redesign - picked over a tactical HUD-bracket
 * treatment, a plain bilinear-blend surface, and a "glass/aurora"
 * glow-plus-chips treatment (all tried and rejected first) for an
 * actual thermal-camera look: one smooth full-bleed 4-corner color
 * blend (bilinear, same technique the very first version of this
 * heatmap used), rounded corners, floating readouts with a drop-
 * shadow instead of boxed chips. Corner colors come from
 * vivid_thermal_color() (continuous, auto-scaled to the current
 * spread of readings) rather than temp_band_color()'s discrete bands -
 * see that function's comment for why a discrete band function
 * flattens this into one solid color for real, close-together bay
 * readings. Reads live off g_sensor each paint, same pattern as the
 * gauges. */
static LRESULT CALLBACK sensor_heatmap_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        COLORREF corner[SENSOR_MAX_UNITS]; /* 0=BAY1 top-left, 1=BAY2 top-right,
                                              * 2=BAY3 bottom-left, 3=BAY4 bottom-right */
        int dot_x[SENSOR_MAX_UNITS], dot_y[SENSOR_MAX_UNITS]; /* see below, where blend_rc is set */
        HFONT label_font, num_font, old_font;
        HPEN pen, old_pen;
        HRGN panel_rgn;
        int i;
        float lo = 0.0f, hi = 0.0f;
        RECT blend_rc;
        bool any_reading = false; /* also drives the panel border color below - see its own comment */
        static const int panel_radius = 14;
        static const int legend_h = 22;
        static const int dot_r = 6;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);

        {
            for (i = 0; i < SENSOR_MAX_UNITS; i++) {
                const SensorState *st = sensor_get_state(&g_sensor, i);
                if (!st->has_reading) continue;
                if (!any_reading || st->temperature_c < lo) lo = st->temperature_c;
                if (!any_reading || st->temperature_c > hi) hi = st->temperature_c;
                any_reading = true;
            }

            if (hi - lo < 2.0f) {
                float mid = (hi + lo) / 2.0f;
                lo = mid - 1.0f;
                hi = mid + 1.0f;
            }

            /* No-reading placeholder color - NOT plain COLOR_APP_MUTED,
             * which is calibrated to read as a faint glow against Dark
             * mode's own dark field background. In Light mode
             * (RGB(110,113,118), a medium-dark gray) the same color
             * alpha-blended 6 rings deep at each of 4 corners (below)
             * stacks into a visibly dark/muddy patch instead of a subtle
             * wash - direct report ("why is the others aint white").
             * This lighter gray is calibrated for that same stacking in
             * Light mode specifically; Dark mode is untouched. */
            COLORREF idle_color = g_light_mode ? RGB(205, 207, 211) : COLOR_APP_MUTED;
            for (i = 0; i < SENSOR_MAX_UNITS; i++) {
                const SensorState *st = sensor_get_state(&g_sensor, i);
                corner[i] = st->has_reading
                    ? vivid_thermal_color((st->temperature_c - lo) / (hi - lo))
                    : idle_color;
            }
        }

        /* Colors are auto-scaled to the CURRENT spread of readings (see
         * above), not a fixed scale - a color alone no longer tells you
         * an absolute temperature. blend_rc carves out a strip at the
         * bottom for a legend bar spelling out what the current min/max
         * actually is, so the scan stays honest to read at a glance. */
        blend_rc = rc;
        blend_rc.bottom -= legend_h;

        /* Where each bay's sensor dot actually sits - independent of
         * where its "BAY N" label sits (the label is now pinned to the
         * panel's own corner, see the drawing loop below - the two are
         * deliberately NOT coupled). Pulled in further than the label's
         * corner margin specifically so the dot reads as its own free-
         * floating marker with real daylight around it, not a tag stuck
         * to the label. Shared by the heat blob origins below and the
         * dot itself, so both agree on exactly where "the sensor" is. */
        {
            int blend_w = blend_rc.right - blend_rc.left;
            int inset_x = blend_w * 34 / 100;
            /* Fixed (not a % of blend_h) - the panel's height never
             * actually varies (add_sensor_heatmap() is always called
             * with h=150), so a flat pixel inset is simpler. Kept small
             * so the two dot rows stay well spread apart vertically -
             * direct request ("make it spreadout and cleaner"). */
            int inset_y = 28;
            dot_x[0] = blend_rc.left + inset_x;  dot_y[0] = blend_rc.top + inset_y;    /* BAY1 */
            dot_x[1] = blend_rc.right - inset_x; dot_y[1] = blend_rc.top + inset_y;    /* BAY2 */
            dot_x[2] = blend_rc.left + inset_x;  dot_y[2] = blend_rc.bottom - inset_y; /* BAY3 */
            dot_x[3] = blend_rc.right - inset_x; dot_y[3] = blend_rc.bottom - inset_y; /* BAY4 */
        }

        /* Corners the round-rect clip cuts off still need to show the
         * surrounding panel's own background, not whatever the blend
         * would otherwise leave there - so fill the full (square) rect
         * with it first, underneath everything else. */
        {
            RECT full = rc;
            HBRUSH panel_brush = CreateSolidBrush(COLOR_APP_PANEL_BG);
            FillRect(hdc, &full, panel_brush);
            DeleteObject(panel_brush);
        }

        panel_rgn = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, panel_radius, panel_radius);
        SelectClipRgn(hdc, panel_rgn);

        /* Each bay gets its own radial "heat origin" centered on its own
         * corner - concentric alpha-blended circles, biggest/faintest
         * first so smaller/more-opaque rings layer on top, building up
         * a center-bright falloff (GDI has no native radial gradient or
         * blur, so this fakes both the same way draw_app_logo_faded()
         * fakes a fade). Replaces a flat bilinear blend that smeared all
         * 4 readings evenly across the whole panel with no sense of
         * WHERE each bay's own heat actually sits - direct request. */
        {
            RECT blob_full = { rc.left, rc.top, rc.right, blend_rc.bottom };
            HBRUSH base_brush = CreateSolidBrush(COLOR_APP_FIELD_BG);
            FillRect(hdc, &blob_full, base_brush);
            DeleteObject(base_brush);

            /* A soft, muted 4-corner wash UNDER the dot-centered rings
             * below - direct report/reference image: the panel's own
             * corners (behind each "BAY N" label, well outside any
             * ring's reach since the dots sit pulled inward from the
             * corners - see dot_x/dot_y above) were reading as flat
             * black instead of carrying even a faint tint of that
             * corner's own color, unlike the reference. Deliberately
             * muted (each corner's color mixed only 35% into the dark
             * field color, not the full saturated color) so this reads
             * as ambient wash, not a return to the flat, no-sense-of-
             * where-the-heat-is full-panel smear an earlier version of
             * this heatmap was replaced away from (see this block's own
             * earlier comment). GradientFill's TRIANGLE mode is GDI's
             * only native bilinear-ish corner blend - no alpha needed,
             * so the muted colors are pre-mixed instead. */
            {
                TRIVERTEX v[4];
                GRADIENT_TRIANGLE tri[2];
                int vi;
                COLORREF wash[SENSOR_MAX_UNITS];

                for (vi = 0; vi < SENSOR_MAX_UNITS; vi++) {
                    wash[vi] = RGB(
                        (GetRValue(corner[vi]) * 9 + GetRValue(COLOR_APP_FIELD_BG) * 91) / 100,
                        (GetGValue(corner[vi]) * 9 + GetGValue(COLOR_APP_FIELD_BG) * 91) / 100,
                        (GetBValue(corner[vi]) * 9 + GetBValue(COLOR_APP_FIELD_BG) * 91) / 100);
                }

                v[0].x = blob_full.left;  v[0].y = blob_full.top;    /* BAY1 */
                v[1].x = blob_full.right; v[1].y = blob_full.top;    /* BAY2 */
                v[2].x = blob_full.left;  v[2].y = blob_full.bottom; /* BAY3 */
                v[3].x = blob_full.right; v[3].y = blob_full.bottom; /* BAY4 */
                for (vi = 0; vi < 4; vi++) {
                    COLORREF col = wash[vi];
                    v[vi].Red   = (COLOR16)(GetRValue(col) << 8);
                    v[vi].Green = (COLOR16)(GetGValue(col) << 8);
                    v[vi].Blue  = (COLOR16)(GetBValue(col) << 8);
                    v[vi].Alpha = 0;
                }
                tri[0].Vertex1 = 0; tri[0].Vertex2 = 1; tri[0].Vertex3 = 2; /* BAY1-BAY2-BAY3 */
                tri[1].Vertex1 = 1; tri[1].Vertex2 = 3; tri[1].Vertex3 = 2; /* BAY2-BAY4-BAY3 */
                GradientFill(hdc, v, 4, tri, 2, GRADIENT_FILL_TRIANGLE);
            }
        }
        {
            /* Scaled back down from an earlier ~2x boost (which, combined
             * with the corner wash below, read as overly saturated across
             * the whole panel) - kept brighter near the dot than the
             * original so the readout still pops, but the overlapping
             * outer rings no longer blanket the corners on their own. */
            static const struct { int radius_pct; BYTE alpha; } rings[] = {
                { 100, 28 }, { 78, 34 }, { 58, 42 }, { 40, 55 }, { 24, 72 }, { 12, 92 }
            };
            int blend_w = blend_rc.right - blend_rc.left;
            int blend_h = blend_rc.bottom - blend_rc.top;
            /* Bounded by the SMALLER of width/height (not height alone) -
             * a panel narrower than it is tall (small window, or the
             * heatmap's width shrinks with the window while its height
             * stays fixed - see add_sensor_heatmap()) used to get a
             * radius sized for the height alone, wildly oversized for
             * the actual width. That, combined with SelectClipRgn below
             * only replacing the clip instead of intersecting it with
             * panel_rgn, let the glow visibly spill out past the
             * rounded panel edge into whatever sat next to it - both
             * fixed here: a sane bound plus a real intersection via
             * ExtSelectClipRgn so a blob's circle can never paint
             * outside the panel's own rounded bounds, however large its
             * radius is computed to be. */
            int blob_radius = (blend_w < blend_h ? blend_w : blend_h) * 7 / 10;
            int c, ri;

            for (c = 0; c < SENSOR_MAX_UNITS; c++) {
                for (ri = 0; ri < (int)(sizeof(rings) / sizeof(rings[0])); ri++) {
                    int r = blob_radius * rings[ri].radius_pct / 100;
                    RECT bounds;
                    HRGN blob_rgn = CreateEllipticRgn(dot_x[c] - r, dot_y[c] - r, dot_x[c] + r, dot_y[c] + r);
                    bounds.left = dot_x[c] - r; bounds.top = dot_y[c] - r;
                    bounds.right = dot_x[c] + r; bounds.bottom = dot_y[c] + r;
                    SelectClipRgn(hdc, panel_rgn);
                    ExtSelectClipRgn(hdc, blob_rgn, RGN_AND);
                    alpha_fill_rect(hdc, bounds, corner[c], rings[ri].alpha);
                    DeleteObject(blob_rgn);
                }
            }
            SelectClipRgn(hdc, panel_rgn);
        }

        /* App emblem watermark, centered over the blend - same faded
         * idiom draw_app_logo_faded() already uses for the idle signal-
         * wave area, direct request to put it here too. Drawn after the
         * heat blobs (so it reads as sitting over the scan, not painted
         * over by it) but before the BAY/reading text below, so that
         * text stays fully legible on top of it. */
        draw_app_logo_faded(hdc, (blend_rc.left + blend_rc.right) / 2,
                             (blend_rc.top + blend_rc.bottom) / 2, 70, 90);

        /* Sensor location marker - a soft accent-blue halo (same
         * elliptic-clip + alpha_fill_rect idiom the heat blobs above
         * use) behind a crisp white dot with an accent-blue ring, so it
         * reads as a real instrument marker (matching the accent color
         * used everywhere else in this app - Connect/Kill Switch/
         * section headings) instead of a plain flat sticker. Drawn on
         * top of the blobs/watermark, under the BAY/reading labels
         * below. */
        for (i = 0; i < SENSOR_MAX_UNITS; i++) {
            static const int halo_r = 13;
            RECT halo_bounds;
            HRGN halo_rgn;
            HBRUSH dot_brush;
            HPEN dot_pen;
            HBRUSH old_brush;
            HPEN old_dot_pen;

            halo_rgn = CreateEllipticRgn(dot_x[i] - halo_r, dot_y[i] - halo_r, dot_x[i] + halo_r, dot_y[i] + halo_r);
            halo_bounds.left = dot_x[i] - halo_r; halo_bounds.top = dot_y[i] - halo_r;
            halo_bounds.right = dot_x[i] + halo_r; halo_bounds.bottom = dot_y[i] + halo_r;
            SelectClipRgn(hdc, panel_rgn);
            ExtSelectClipRgn(hdc, halo_rgn, RGN_AND);
            alpha_fill_rect(hdc, halo_bounds, COLOR_APP_ACCENT, 110);
            DeleteObject(halo_rgn);
            SelectClipRgn(hdc, panel_rgn);

            dot_brush = CreateSolidBrush(RGB(255, 255, 255));
            dot_pen = CreatePen(PS_SOLID, 2, COLOR_APP_ACCENT);
            old_brush = (HBRUSH)SelectObject(hdc, dot_brush);
            old_dot_pen = (HPEN)SelectObject(hdc, dot_pen);
            Ellipse(hdc, dot_x[i] - dot_r, dot_y[i] - dot_r, dot_x[i] + dot_r, dot_y[i] + dot_r);
            SelectObject(hdc, old_brush);
            SelectObject(hdc, old_dot_pen);
            DeleteObject(dot_brush);
            DeleteObject(dot_pen);
        }

        /* Legend strip: the reserved bottom band, filled with the panel
         * background, then a thin multi-stop gradient bar (the same 4
         * vivid_thermal_color() stops, swept left to right) with the
         * current lo/hi readings labeled at each end - what "the left
         * end of this scan's color range" and "the right end" actually
         * mean in real degrees right now. */
        {
            RECT legend_rc = rc;
            RECT bar_rc;
            char lo_label[12], hi_label[12];
            HFONT legend_font;
            legend_rc.top = blend_rc.bottom;

            legend_font = CreateFontA(-9, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                       ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

            bar_rc.left = legend_rc.left + 40;
            bar_rc.right = legend_rc.right - 40;
            bar_rc.top = legend_rc.top + 8;
            bar_rc.bottom = bar_rc.top + 5;
            if (bar_rc.right > bar_rc.left) {
                /* Same 5 stops as vivid_thermal_color() above (duplicated,
                 * not shared - GradientFill needs real RECTs to sweep
                 * across, not a 0..1 t like the heatmap's per-pixel blend
                 * does) - keeps the legend bar an honest match for what
                 * the blobs above it actually show. */
                int seg, seg_w = (bar_rc.right - bar_rc.left) / 4;
                static const COLORREF stops[] = {
                    RGB(70, 170, 90), RGB(210, 190, 60), RGB(224, 146, 34), RGB(196, 90, 24), RGB(214, 64, 56)
                };
                for (seg = 0; seg < 4; seg++) {
                    RECT seg_rc = bar_rc;
                    seg_rc.left = bar_rc.left + seg * seg_w;
                    seg_rc.right = (seg == 3) ? bar_rc.right : seg_rc.left + seg_w;
                    gradient_fill_rect(hdc, seg_rc, stops[seg], stops[seg + 1], false);
                }
            }

            wsprintfA(lo_label, "%d.%dC", (int)lo, (int)(lo * 10) % 10);
            wsprintfA(hi_label, "%d.%dC", (int)hi, (int)(hi * 10) % 10);

            old_font = (HFONT)SelectObject(hdc, legend_font ? legend_font : g_font);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, COLOR_APP_MUTED);
            {
                RECT lo_rc = legend_rc; lo_rc.left += 6; lo_rc.right = bar_rc.left - 4;
                RECT hi_rc = legend_rc; hi_rc.left = bar_rc.right + 4; hi_rc.right -= 6;
                DrawTextA(hdc, lo_label, -1, &lo_rc, DT_SINGLELINE | DT_NOCLIP | DT_LEFT | DT_VCENTER);
                DrawTextA(hdc, hi_label, -1, &hi_rc, DT_SINGLELINE | DT_NOCLIP | DT_RIGHT | DT_VCENTER);
            }
            SelectObject(hdc, old_font);
            if (legend_font) DeleteObject(legend_font);
        }

        SelectClipRgn(hdc, NULL);
        DeleteObject(panel_rgn);

        /* Silver/light border while at least one bay has a real, live
         * reading - reads as "this panel is actively scanning" at a
         * glance, vs. every other panel's same muted COLOR_APP_PANEL_BORDER
         * the rest of the time (no readings yet - nothing to be lit up
         * about). Direct request/reference image. In Light mode the
         * silver (200,203,209) sits almost on top of Light mode's own
         * idle COLOR_APP_PANEL_BORDER (219,221,225) - barely
         * distinguishable, so "actively scanning" stopped reading as
         * lit up there. COLOR_APP_ACCENT gives it real contrast instead,
         * consistent with the accent blue this app already uses
         * everywhere else for "this is live/active". Dark mode keeps
         * the original silver, unchanged. */
        pen = CreatePen(PS_SOLID, 1, any_reading ? (g_light_mode ? COLOR_APP_ACCENT : RGB(200, 203, 209)) : COLOR_APP_PANEL_BORDER);
        old_pen = (HPEN)SelectObject(hdc, pen);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, panel_radius, panel_radius);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);

        /* "BAY N" / reading, sitting beside the dot (left column's text
         * to the left of its dot, right column's to the right) instead
         * of stacked above/below it - direct request/reference mockup:
         * a label right next to its dot, spread out and uncramped. The
         * two-line block (label over reading) is centered vertically on
         * dot_y, so it needs no extra top/bottom clearance beyond its
         * own half-height regardless of row - what lets inset_y above
         * stay small and the two rows genuinely spread apart. Every
         * string is drawn twice: once 1px offset in a shadow color,
         * then the real text on top, a cheap drop-shadow that keeps it
         * legible over both the light and dark ends of the blend.
         * Reading text + shadow both flip per theme - Dark mode draws
         * white text with a near-black shadow (original, unchanged);
         * Light mode draws dark text with a white halo instead, since
         * plain white text is invisible against Light mode's own light
         * field/corner-wash colors (direct report - the heatmap read as
         * broken/still-dark while the rest of the app went Light). */
        label_font = CreateFontA(-11, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                  ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        num_font = CreateFontA(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        SetBkMode(hdc, TRANSPARENT);
        {
            COLORREF reading_text_color = g_light_mode ? COLOR_APP_TEXT : RGB(255, 255, 255);
            COLORREF reading_shadow_color = g_light_mode ? RGB(255, 255, 255) : RGB(10, 10, 12);

        for (i = 0; i < SENSOR_MAX_UNITS; i++) {
            const SensorState *st = sensor_get_state(&g_sensor, i);
            char blabel[8], num_label[16];
            RECT lrc, nrc;
            bool left_col = (i == 0 || i == 2);
            bool top_row = (i == 0 || i == 1);
            UINT align = DT_SINGLELINE | DT_NOCLIP | DT_TOP | (left_col ? DT_LEFT : DT_RIGHT);
            static const int margin = 12; /* clear of the panel's own rounded corner */
            static const int block_w = 64;
            static const int num_h = 18;
            static const int label_h = 14;
            static const int gap_between = 2;

            wsprintfA(blabel, "BAY %d", sensor_get_unit_address(&g_sensor, i));
            if (st->has_reading) {
                wsprintfA(num_label, "%d.%dC", (int)st->temperature_c, (int)(st->temperature_c * 10) % 10);
            } else {
                lstrcpynA(num_label, "-", (int)sizeof(num_label));
            }

            /* Pinned to the panel's own corner - NOT to the dot's
             * position - so the label reads like a map legend entry
             * ("top-left = BAY 1") rather than a tag glued to the
             * marker. Direct correction: the reference mockup that
             * inspired the dot/heat-blob redesign never actually showed
             * the label touching the dot either - it sat off at the
             * panel's edge, which is what this restores. The label is
             * always the outermost line (right at the corner); the
             * reading sits tucked just inside it. */
            if (left_col) {
                lrc.left = blend_rc.left + margin;
                lrc.right = lrc.left + block_w;
            } else {
                lrc.right = blend_rc.right - margin;
                lrc.left = lrc.right - block_w;
            }
            nrc = lrc;

            if (top_row) {
                lrc.top = blend_rc.top + margin;
                lrc.bottom = lrc.top + label_h;
                nrc.top = lrc.bottom + gap_between;
                nrc.bottom = nrc.top + num_h;
            } else {
                lrc.bottom = blend_rc.bottom - margin;
                lrc.top = lrc.bottom - label_h;
                nrc.bottom = lrc.top - gap_between;
                nrc.top = nrc.bottom - num_h;
            }

            /* BAY label tinted with the same blue used for every other
             * section heading in this app (COLOR_APP_HEADER) instead of
             * plain white - reads as a caption for the bold white
             * reading below it, a clearer label/value hierarchy than
             * two same-weight white lines. */
            old_font = (HFONT)SelectObject(hdc, label_font ? label_font : g_font);
            SetTextColor(hdc, reading_shadow_color);
            OffsetRect(&lrc, 1, 1);
            DrawTextA(hdc, blabel, -1, &lrc, align);
            OffsetRect(&lrc, -1, -1);
            SetTextColor(hdc, COLOR_APP_HEADER);
            DrawTextA(hdc, blabel, -1, &lrc, align);

            SelectObject(hdc, num_font ? num_font : g_font);
            SetTextColor(hdc, reading_shadow_color);
            OffsetRect(&nrc, 1, 1);
            DrawTextA(hdc, num_label, -1, &nrc, align);
            OffsetRect(&nrc, -1, -1);
            SetTextColor(hdc, reading_text_color);
            DrawTextA(hdc, num_label, -1, &nrc, align);

            SelectObject(hdc, old_font);
        }
        }
        if (label_font) DeleteObject(label_font);
        if (num_font) DeleteObject(num_font);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

static HWND add_sensor_heatmap(HWND parent, int x, int y, int w, int h) {
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT, x, y, w, h, 0);
    if (ctrl) {
        if (!g_panel_orig_proc) {
            g_panel_orig_proc = (WNDPROC)GetWindowLongPtrA(ctrl, GWLP_WNDPROC);
        }
        SetWindowLongPtrA(ctrl, GWLP_WNDPROC, (LONG_PTR)sensor_heatmap_subclass_proc);
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

/* This app has no in-app accounts - only the Windows session it runs
 * in - so the machine name is the closest thing to "who" for an audit
 * trail. Cached after the first successful call; falls back to a
 * fixed string rather than retrying every time if GetComputerNameA
 * ever fails. */
static const char *get_computer_name_cached(void) {
    static char name[MAX_COMPUTERNAME_LENGTH + 1] = "";
    static bool resolved;
    DWORD len = sizeof(name);

    if (!resolved) {
        if (!GetComputerNameA(name, &len)) {
            lstrcpynA(name, "unknown-pc", (int)sizeof(name));
        }
        resolved = true;
    }
    return name;
}

/* Tags a status-changing log entry with the machine it happened on -
 * direct request ("view logs that shows who change the status"),
 * scoped to Kill Switch trip/reset and channel power/mode/level
 * changes only, not every log_add() line (connection, logo, admin
 * unlock stay untagged - see call sites). Reuses the existing
 * Activity Log / its "View Full" popup rather than a separate view. */
static void log_add_status_change(const char *message) {
    char tagged[144];
    wsprintfA(tagged, "[%s] %s", get_computer_name_cached(), message);
    log_add(tagged);
}

/* Forward declaration - defined below, needs log_add() to already
 * exist; on_log_view_clicked() (also below) needs it sooner. */
static void ui_show_warning(const char *message);

/* Forward declarations - defined below (need get_ini_path()), used
 * sooner by on_sensor_connect_clicked()/auto_connect_sensor_on_startup()/
 * WM_CREATE. Sensor Port is the one piece of .ini persistence brought
 * back after removing all of it (see save_settings()'s own removal
 * comment) - direct report: with no memory of which port is actually
 * the sensor, the combo just defaults to whatever enumerates first,
 * which isn't necessarily correct and isn't a fix, just luck. Scoped
 * to Sensor Port ONLY - not channel state, not RS422 settings, not
 * Light Mode - since this is hardware identity ("which port is the
 * sensor wired to"), not operational state that should reset for
 * safety the way channel mode/output now does. */
static void save_sensor_port(void);
static void load_sensor_port(void);

/* Full-log popup's own WndProc - a plain secondary window (WS_POPUP,
 * not a dialog resource), same "build it directly with CreateWindowEx"
 * approach every other control in this app already uses rather than
 * introducing a second, resource-file-based UI paradigm just for this.
 * Native (non-owner-drawn) Close button is intentional, not an
 * oversight - MessageBoxA's Kill Switch confirmation already establishes
 * "native OS chrome is fine for an occasional secondary/utility window"
 * as this app's own precedent. */
static LRESULT CALLBACK log_view_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wParam, &rc, g_brush_panel);
            return 1;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COLOR_APP_TEXT);
            SetBkColor(hdc, COLOR_APP_FIELD_BG);
            return (LRESULT)g_brush_field;
        }
        case WM_SIZE: {
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            MoveWindow(GetDlgItem(hwnd, IDC_LOG_VIEW_EDIT), 12, 12, w - 24, h - 52, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_LOG_VIEW_CLOSE_BTN), w - 12 - 80, h - 32, 80, 24, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_LOG_VIEW_CLOSE_BTN && HIWORD(wParam) == BN_CLICKED) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            g_log_view_hwnd = NULL;
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* Highest Temp Log popup's own WndProc - identical shape to
 * log_view_wnd_proc() above (same dark-themed, resizable secondary
 * window "View Full" already uses for the Activity Log - direct
 * request to match it exactly), just its own window class/controls/
 * global handle so the two popups can be open side by side. */
static LRESULT CALLBACK templog_view_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wParam, &rc, g_brush_panel);
            return 1;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COLOR_APP_TEXT);
            SetBkColor(hdc, COLOR_APP_FIELD_BG);
            return (LRESULT)g_brush_field;
        }
        case WM_SIZE: {
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            MoveWindow(GetDlgItem(hwnd, IDC_TEMPLOG_VIEW_EDIT), 12, 12, w - 24, h - 52, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_TEMPLOG_VIEW_CLOSE_BTN), w - 12 - 80, h - 32, 80, 24, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_TEMPLOG_VIEW_CLOSE_BTN && HIWORD(wParam) == BN_CLICKED) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            g_templog_view_hwnd = NULL;
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* One parsed sensor_log.csv row, reduced to just what the Highest Temp
 * Log view needs to sort/display - the peak of that row's 4 bay
 * readings (skipping any bay with no reading that tick), which bay it
 * came from, and the row's own timestamp column verbatim. */
typedef struct {
    char timestamp[24];
    float peak_c;
    int bay; /* 0-3 */
} TempLogRow;

static int templog_row_cmp(const void *a, const void *b) {
    const TempLogRow *ra = (const TempLogRow *)a;
    const TempLogRow *rb = (const TempLogRow *)b;
    if (ra->peak_c > rb->peak_c) return -1;
    if (ra->peak_c < rb->peak_c) return 1;
    return 0;
}

/* IDC_HIGHEST_TEMP_LOG_BTN's handler - reads sensor_log.csv (the same
 * weekly-rotating file "Open Log" already opens raw in Excel - see
 * on_open_log_clicked()), parses every row's 4 bay temperatures, and
 * shows them sorted highest-first in a dedicated popup, same "bigger
 * window" pattern as on_log_view_clicked() - direct request to match
 * "View Full" exactly. Scoped to whatever's currently in the CSV,
 * which is at most the current week's worth of readings -
 * sensor_log_start_new_week() (sensor_log.c) truncates it back to
 * just the header on each weekly rotation, so this view is never
 * showing more than one week deep by construction, no separate date
 * filtering needed here. */
static void on_highest_temp_log_clicked(void) {
    char path[MAX_PATH + 16];
    HANDLE file;
    DWORD size, read_bytes;
    char *raw;
    char *line;
    TempLogRow *rows;
    int row_count, row_cap;
    char *buf;
    size_t buf_cap, buf_len;
    int i;
    RECT screen_rc;
    int win_w = 700, win_h = 550;
    int x, y;
    HWND edit_ctrl, close_btn;
    static bool class_registered;

    if (g_templog_view_hwnd) {
        SetForegroundWindow(g_templog_view_hwnd);
        return;
    }

    get_sensor_log_path(path);
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        ui_show_warning("No sensor log yet - it's written a few seconds after the first sensor reading");
        return;
    }
    size = GetFileSize(file, NULL);
    raw = (char *)malloc((size_t)size + 1);
    if (!raw) {
        CloseHandle(file);
        ui_show_warning("Could not open the Highest Temp Log (out of memory)");
        return;
    }
    if (!ReadFile(file, raw, size, &read_bytes, NULL)) {
        read_bytes = 0;
    }
    CloseHandle(file);
    raw[read_bytes] = '\0';

    /* Skip the header row, then parse each data row: timestamp field,
     * then 4 temperature fields (humidity fields after them are never
     * needed here). A field left blank (no reading that unit that
     * tick - see sensor_log_append_row()) is just skipped, not treated
     * as 0.0 - a real 0.0C reading should still be able to win. */
    row_count = 0;
    row_cap = 64;
    rows = (TempLogRow *)malloc(sizeof(TempLogRow) * (size_t)row_cap);
    line = strchr(raw, '\n');
    line = line ? line + 1 : raw + lstrlenA(raw);
    while (*line && rows) {
        char *line_end = strchr(line, '\n');
        char row_buf[256];
        int row_len = (int)((line_end ? line_end : raw + lstrlenA(raw)) - line);
        char *field[5];
        int fi;
        char *p;
        bool have_peak = false;
        float peak = 0.0f;
        int peak_bay = 0;

        if (row_len <= 0 || row_len >= (int)sizeof(row_buf)) {
            line = line_end ? line_end + 1 : line + lstrlenA(line);
            continue;
        }
        memcpy(row_buf, line, (size_t)row_len);
        row_buf[row_len] = '\0';
        if (row_buf[row_len - 1] == '\r') {
            row_buf[row_len - 1] = '\0';
        }

        p = row_buf;
        field[0] = p;
        for (fi = 1; fi < 5 && p; fi++) {
            p = strchr(p, ',');
            if (p) {
                *p = '\0';
                p++;
                field[fi] = p;
            }
        }
        if (fi == 5) {
            for (fi = 1; fi < 5; fi++) {
                if (field[fi][0] != '\0') {
                    float v = (float)atof(field[fi]);
                    if (!have_peak || v > peak) {
                        peak = v;
                        peak_bay = fi - 1;
                        have_peak = true;
                    }
                }
            }
            if (have_peak) {
                if (row_count >= row_cap) {
                    TempLogRow *grown;
                    row_cap *= 2;
                    grown = (TempLogRow *)realloc(rows, sizeof(TempLogRow) * (size_t)row_cap);
                    if (!grown) {
                        free(rows);
                        rows = NULL;
                    } else {
                        rows = grown;
                    }
                }
                if (rows) {
                    lstrcpynA(rows[row_count].timestamp, field[0], (int)sizeof(rows[row_count].timestamp));
                    rows[row_count].peak_c = peak;
                    rows[row_count].bay = peak_bay;
                    row_count++;
                }
            }
        }
        line = line_end ? line_end + 1 : line + lstrlenA(line);
    }
    free(raw);

    if (!rows || row_count == 0) {
        free(rows);
        ui_show_warning("No sensor log entries yet - it's written a few seconds after the first sensor reading");
        return;
    }
    qsort(rows, (size_t)row_count, sizeof(TempLogRow), templog_row_cmp);

    buf_cap = 4096;
    buf = (char *)malloc(buf_cap);
    buf_len = 0;
    if (buf) {
        buf[0] = '\0';
    }
    for (i = 0; i < row_count && buf; i++) {
        char line_txt[64];
        size_t line_len;
        wsprintfA(line_txt, "%s   %d.%dC   BAY%d", rows[i].timestamp,
                  (int)rows[i].peak_c, (int)(rows[i].peak_c * 10) % 10, rows[i].bay + 1);
        line_len = (size_t)lstrlenA(line_txt);
        while (buf_len + line_len + 3 > buf_cap) {
            char *grown;
            buf_cap *= 2;
            grown = (char *)realloc(buf, buf_cap);
            if (!grown) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = grown;
        }
        if (!buf) break;
        lstrcpynA(buf + buf_len, line_txt, (int)(buf_cap - buf_len));
        buf_len += line_len;
        buf[buf_len++] = '\r';
        buf[buf_len++] = '\n';
        buf[buf_len] = '\0';
    }
    free(rows);
    if (!buf) {
        ui_show_warning("Could not open the Highest Temp Log (out of memory)");
        return;
    }

    if (!class_registered) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = templog_view_wnd_proc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorA(NULL, MAKEINTRESOURCEA(32512)); /* IDC_ARROW */
        wc.lpszClassName = "ECMTempLogViewWnd";
        RegisterClassExA(&wc);
        class_registered = true;
    }

    {
        RECT win_rc = { 0, 0, win_w, win_h };
        AdjustWindowRectEx(&win_rc, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, FALSE, 0);

        SystemParametersInfoA(SPI_GETWORKAREA, 0, &screen_rc, 0);
        x = screen_rc.left + ((screen_rc.right - screen_rc.left) - (win_rc.right - win_rc.left)) / 2;
        y = screen_rc.top + ((screen_rc.bottom - screen_rc.top) - (win_rc.bottom - win_rc.top)) / 2;

        g_templog_view_hwnd = CreateWindowExA(0, "ECMTempLogViewWnd", "Highest Temp Log - This Week",
                                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                                               x, y, win_rc.right - win_rc.left, win_rc.bottom - win_rc.top,
                                               g_hwnd, NULL, g_hinst, NULL);
    }
    if (!g_templog_view_hwnd) {
        free(buf);
        return;
    }

    edit_ctrl = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", buf,
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                 ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                 12, 12, win_w - 24, win_h - 52, g_templog_view_hwnd,
                                 (HMENU)(INT_PTR)IDC_TEMPLOG_VIEW_EDIT, g_hinst, NULL);
    if (edit_ctrl) {
        SendMessageA(edit_ctrl, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    }
    close_btn = CreateWindowExA(0, "BUTTON", "Close",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 win_w - 12 - 80, win_h - 32, 80, 24, g_templog_view_hwnd,
                                 (HMENU)(INT_PTR)IDC_TEMPLOG_VIEW_CLOSE_BTN, g_hinst, NULL);
    if (close_btn) {
        SendMessageA(close_btn, WM_SETFONT, (WPARAM)g_font, TRUE);
    }

    free(buf);
    ShowWindow(g_templog_view_hwnd, SW_SHOW);
    SetFocus(edit_ctrl);
}

/* IDC_LOG_VIEW_BTN's handler - a bigger, dedicated window showing the
 * whole Activity Log as one scrollable block instead of the small
 * embedded listbox (direct request: "a full view dialog logs"). A
 * snapshot of whatever's in the listbox right now, not a live mirror -
 * simplest correct behavior for a "let me actually read back through
 * this" viewer; reopening it (or the existing window already stays
 * open on a second click - just refocused) picks up anything new. */
static void on_log_view_clicked(void) {
    HWND list;
    int count, i;
    char *buf;
    size_t buf_cap, buf_len;
    RECT screen_rc;
    int win_w = 900, win_h = 650;
    int x, y;
    HWND edit_ctrl, close_btn;
    static bool class_registered;

    if (g_log_view_hwnd) {
        SetForegroundWindow(g_log_view_hwnd);
        return;
    }

    if (!class_registered) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = log_view_wnd_proc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorA(NULL, MAKEINTRESOURCEA(32512)); /* IDC_ARROW */
        wc.lpszClassName = "ECMLogViewWnd";
        RegisterClassExA(&wc);
        class_registered = true;
    }

    list = GetDlgItem(g_hwnd, IDC_LOG_LISTBOX);
    count = (int)SendMessageA(list, LB_GETCOUNT, 0, 0);
    buf_cap = 4096;
    buf = (char *)malloc(buf_cap);
    buf_len = 0;
    if (buf) {
        buf[0] = '\0';
    }
    for (i = 0; i < count && buf; i++) {
        char line[160];
        size_t line_len;
        SendMessageA(list, LB_GETTEXT, (WPARAM)i, (LPARAM)line);
        line_len = (size_t)lstrlenA(line);
        while (buf_len + line_len + 3 > buf_cap) {
            char *grown;
            buf_cap *= 2;
            grown = (char *)realloc(buf, buf_cap);
            if (!grown) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = grown;
        }
        if (!buf) break;
        lstrcpynA(buf + buf_len, line, (int)(buf_cap - buf_len));
        buf_len += line_len;
        buf[buf_len++] = '\r';
        buf[buf_len++] = '\n';
        buf[buf_len] = '\0';
    }
    if (!buf) {
        ui_show_warning("Could not open the full log view (out of memory)");
        return;
    }

    {
        /* CreateWindowExA's w/h below is the OUTER window size (title
         * bar + borders included), but win_w/win_h and every child's
         * position are sized for the CLIENT area (900x650) - without
         * this adjustment the real client area comes out smaller than
         * that, pushing IDC_LOG_VIEW_CLOSE_BTN (positioned near the
         * client's own bottom edge) below the actual visible window
         * entirely, invisible. */
        RECT win_rc = { 0, 0, win_w, win_h };
        AdjustWindowRectEx(&win_rc, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, FALSE, 0);

        SystemParametersInfoA(SPI_GETWORKAREA, 0, &screen_rc, 0);
        x = screen_rc.left + ((screen_rc.right - screen_rc.left) - (win_rc.right - win_rc.left)) / 2;
        y = screen_rc.top + ((screen_rc.bottom - screen_rc.top) - (win_rc.bottom - win_rc.top)) / 2;

        g_log_view_hwnd = CreateWindowExA(0, "ECMLogViewWnd", "Activity Log - Full View",
                                           WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                                           x, y, win_rc.right - win_rc.left, win_rc.bottom - win_rc.top,
                                           g_hwnd, NULL, g_hinst, NULL);
    }
    if (!g_log_view_hwnd) {
        free(buf);
        return;
    }

    edit_ctrl = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", buf,
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                 ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                 12, 12, win_w - 24, win_h - 52, g_log_view_hwnd,
                                 (HMENU)(INT_PTR)IDC_LOG_VIEW_EDIT, g_hinst, NULL);
    if (edit_ctrl) {
        SendMessageA(edit_ctrl, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    }
    close_btn = CreateWindowExA(0, "BUTTON", "Close",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 win_w - 12 - 80, win_h - 32, 80, 24, g_log_view_hwnd,
                                 (HMENU)(INT_PTR)IDC_LOG_VIEW_CLOSE_BTN, g_hinst, NULL);
    if (close_btn) {
        SendMessageA(close_btn, WM_SETFONT, (WPARAM)g_font, TRUE);
    }

    free(buf);
    ShowWindow(g_log_view_hwnd, SW_SHOW);
    SetFocus(edit_ctrl);
}

/* ---- loading dialog (startup auto-connect, close-time save) ---- */

static LRESULT CALLBACK loading_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, g_brush_panel);
        return 1;
    }
    if (msg == WM_CTLCOLORSTATIC) {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, COLOR_APP_TEXT);
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* Small borderless popup, no title bar/Close of its own - genuinely
 * transient (a few hundred ms), not something the user is meant to
 * interact with or dismiss. Direct request: a loading dialog while the
 * app connects on open and while it saves on close ("fetch loading...
 * saving a load dialog and closing loading saving dialog"). Forces its
 * own paint via the PeekMessage pump below - normally a freshly shown
 * window's first paint waits for the next natural idle in the message
 * loop, which wouldn't come until AFTER the blocking connect/save call
 * below already finished, defeating the entire point. */
static HWND loading_dialog_show(const char *message) {
    static bool class_registered;
    RECT screen_rc;
    int w = 240, h = 80;
    int x, y;
    HWND hwnd;
    MSG msg;

    if (!class_registered) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = loading_wnd_proc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorA(NULL, MAKEINTRESOURCEA(32512)); /* IDC_ARROW */
        wc.lpszClassName = "ECMLoadingWnd";
        RegisterClassExA(&wc);
        class_registered = true;
    }

    SystemParametersInfoA(SPI_GETWORKAREA, 0, &screen_rc, 0);
    x = screen_rc.left + ((screen_rc.right - screen_rc.left) - w) / 2;
    y = screen_rc.top + ((screen_rc.bottom - screen_rc.top) - h) / 2;

    /* Owner is NULL, not g_hwnd - the close-time use of this
     * (WM_DESTROY's "Saving...") rendered nothing at all in this Wine/
     * Xvfb sandbox (no real window manager), which first looked like
     * the same bug the startup dialog had (own commit). It isn't: a
     * plain MessageBoxA(NULL, ...) placed in WM_DESTROY as a diagnostic
     * ALSO failed to render there, even though it has no owner at all
     * and blocked/waited correctly at the Win32 level the whole time -
     * so this is this sandbox's own inability to show ANY new window
     * during the main window's WM_DESTROY, not something about g_hwnd
     * as an owner specifically. Real Windows (the actual deployment
     * target) shows dialogs from WM_DESTROY/WM_CLOSE routinely - this
     * couldn't be verified visually in-sandbox, only that the code
     * itself runs correctly (blocked and returned as expected - the
     * WM_DESTROY call this was originally verified against was
     * save_settings(), since removed; WM_DESTROY's "Restoring to
     * default..." now wraps on_reset_to_default_clicked() instead,
     * same blocking-call shape). NULL owner is kept
     * anyway since it's the more correct/defensive choice regardless -
     * no window should depend on an owner that might be mid-teardown -
     * just don't read it as "the fix" for the black-screen report. */
    hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, "ECMLoadingWnd", "",
                            WS_POPUP | WS_BORDER, x, y, w, h, NULL, NULL, g_hinst, NULL);
    if (!hwnd) {
        return NULL;
    }
    add_ctrl(hwnd, "STATIC", message, SS_CENTER | SS_NOPREFIX, 10, 30, w - 20, 20, 0);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return hwnd;
}

/* Enforces a minimum visible time from when it was first shown - the
 * actual connect/save work this wraps is often genuinely instant (a
 * local port open, a few .ini writes), which would make the dialog
 * flash for one paint and vanish, defeating "let the user actually see
 * this happened". A deliberate short Sleep() on the UI thread, not a
 * timer/animation - this app has no threading infrastructure, and the
 * whole point here is a brief, honest pause, not a spinner. */
static void loading_dialog_hide(HWND hwnd, DWORD shown_tick) {
    DWORD elapsed;
    if (!hwnd) {
        return;
    }
    elapsed = GetTickCount() - shown_tick;
    if (elapsed < 500) {
        Sleep(500 - elapsed);
    }
    DestroyWindow(hwnd);
}

/* Startup-only variant of on_sensor_connect_clicked() - same connect
 * logic, but never pops the "Select a port first" MessageBoxA a manual
 * click uses: an unattended auto-connect attempt with nothing plugged
 * in yet (a fresh install, or between hardware swaps) should just stay
 * disconnected quietly, not block app launch on a dialog the user has
 * to dismiss before they can do anything else. */
static void auto_connect_sensor_on_startup(void) {
    char port[16];

    if (sensor_is_connected(&g_sensor)) {
        return;
    }
    if (GetDlgItemTextA(g_hwnd, IDC_SENSOR_PORT_COMBO, port, sizeof(port)) == 0) {
        return;
    }
    if (!sensor_connect(&g_sensor, port, SENSOR_BAUD, SENSOR_PARITY, SENSOR_DATABITS)) {
        char msg[128];
        wsprintfA(msg, "Failed to open %s", port);
        ui_show_warning(msg);
    } else {
        save_sensor_port();
    }
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

/* Same as ui_show_warning(), but appends the real Win32 reason
 * (GetLastError(), via FormatMessageA) in parens when there is one -
 * "Could not copy..." alone doesn't say WHY (permissions? sharing
 * violation? something else?), and that's exactly what's needed to
 * tell "app folder isn't writable" apart from anything else. Must be
 * called right after the failing API, before any other call can
 * clobber GetLastError(). */
static void ui_show_warning_with_last_error(const char *prefix) {
    char msg[200];
    DWORD err = GetLastError();
    lstrcpynA(msg, prefix, (int)sizeof(msg));
    if (err != 0) {
        char errbuf[128];
        if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err,
                            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), errbuf, sizeof(errbuf), NULL) > 0) {
            int len = lstrlenA(errbuf);
            while (len > 0 && (errbuf[len - 1] == '\n' || errbuf[len - 1] == '\r')) {
                errbuf[--len] = '\0';
            }
            if (len > 0) {
                wsprintfA(msg + lstrlenA(msg), " (%s)", errbuf);
            }
        }
    }
    ui_show_warning(msg);
}

/* ---- connection -> UI callbacks ---- */

/* Defined below, once channel_mode_id()/channel_set_id()/etc. exist -
 * forward-declared here so conn_on_connected_changed() can gate every
 * channel control on the RS422 link the instant it changes. */
static void set_channel_controls_enabled(bool enabled);
/* Also defined below (needs channel_select_id()) - the selection
 * checkbox is gated on conn_is_connected() too (see its own comment),
 * so a connect/disconnect has to refresh every card's checkbox the
 * same way it refreshes every other control. */
static void ui_update_all_select_checkbox_visibility(void);
/* Also defined below (needs ChannelUiCache/g_ui_cache) - the status
 * text and card border are now gated on conn_is_connected() too (see
 * their own comments), which ui_refresh_channel()'s per-field cache
 * doesn't track on its own: output_on/level/busy/tripped can all stay
 * exactly the same across a connect/disconnect, so without forcing the
 * cache stale here the text would keep showing STANDBY (or a stale
 * level) after connecting until something else happened to change. */
static void ui_invalidate_all_channel_cache(void);
static void ui_refresh_all_channels(void);
/* Also defined below (needs channel_set_mode()/channel_turn_output_on()
 * already declared via channels.h, and g_kill_switch_tripped) - RS422
 * connecting for the first time this session fires the same "power
 * everything on" sequence Reset to Default and close now both use (see
 * that function's own comment on the 3-way scope decision). */
static void on_reset_to_default_clicked(void);
static bool g_launch_power_on_done; /* one-shot - only the FIRST RS422
                                      * connect this session triggers
                                      * the auto power-on, not every
                                      * reconnect (e.g. after a manual
                                      * Disconnect/Connect mid-session,
                                      * which shouldn't silently power
                                      * everything back on unasked). */

static void conn_on_connected_changed(bool connected, void *ctx) {
    (void)ctx;
    SetDlgItemTextA(g_hwnd, IDC_CONN_STATUS_LBL, connected ? "Connected" : "Disconnected");
    EnableWindow(GetDlgItem(g_hwnd, IDC_CONNECT_BTN), TRUE);
    SetWindowTextA(GetDlgItem(g_hwnd, IDC_CONNECT_BTN), connected ? "Disconnect" : "Connect");
    InvalidateRect(GetDlgItem(g_hwnd, IDC_CONN_STATUS_LBL), NULL, TRUE);
    set_channel_controls_enabled(connected);
    ui_update_all_select_checkbox_visibility();
    /* Forces the cache stale so ui_refresh_all_channels() actually
     * redraws every card's status text/border even though output_on/
     * level/busy/tripped may not have changed - see the forward
     * declaration's own comment. ui_refresh_channel() already calls
     * ui_invalidate_card() itself whenever it finds a stale cache, so
     * that's the border covered too, not just the status text. */
    ui_invalidate_all_channel_cache();
    ui_refresh_all_channels();

    /* Direct instruction: the first time RS422 connects this session
     * (launch's own auto-connect, or the background retry catching up
     * a few seconds later - either way, whichever actually succeeds
     * first), power every channel on with Pseudo Random Noise - so
     * opening the app and having it connect is enough on its own,
     * nothing left to click before it's ready to demo. */
    if (connected && !g_launch_power_on_done) {
        g_launch_power_on_done = true;
        on_reset_to_default_clicked();
    }
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

/* Direct revert - the exclude-the-other-dropdown's-port filter this
 * used to take risked hiding the ONE correct port from the Sensor
 * list if RS422's own (functionally-unused, per on_connect_clicked()'s
 * own comment) Port combo happened to be showing that same port name.
 * Back to no cross-filtering - each dropdown just lists everything it
 * finds. */
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

    /* No "select a port first" gate here on purpose - unlike the sensor
     * connection below, this one's port_name is accepted only for
     * interface compatibility and never actually used (see
     * connection.h's comment on conn_connect(): AutoConnectSDR finds
     * the RS422 dongle itself). The dongle isn't a standard Windows COM
     * port device, so on a machine with nothing else serial attached,
     * IDC_PORT_COMBO is correctly empty - requiring text here used to
     * block Connect entirely on exactly that machine, over a field the
     * DLL never reads. */
    GetDlgItemTextA(g_hwnd, IDC_PORT_COMBO, port, sizeof(port));

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
 * ever need changing - fewer knobs, matching the "user friendly" ask.
 * SENSOR_BAUD/PARITY/DATABITS themselves now live in sensor.h - the
 * background service (sensor_service.c) needs the exact same values to
 * open the same hardware the same way. */

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
    } else {
        save_sensor_port();
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

static void update_highest_temp_today(void) {
    SYSTEMTIME st;
    int i;

    GetLocalTime(&st);
    if (!g_highest_temp_today_valid ||
        st.wDay != g_highest_temp_today_day ||
        st.wMonth != g_highest_temp_today_month ||
        st.wYear != g_highest_temp_today_year) {
        g_highest_temp_today_valid = false;
        g_highest_temp_today_bay = -1;
        g_highest_temp_today_day = st.wDay;
        g_highest_temp_today_month = st.wMonth;
        g_highest_temp_today_year = st.wYear;
    }

    for (i = 0; i < SENSOR_MAX_UNITS; i++) {
        const SensorState *unit = sensor_get_state(&g_sensor, i);
        if (!unit->has_reading) {
            continue;
        }
        if (!g_highest_temp_today_valid || unit->temperature_c > g_highest_temp_today_c) {
            g_highest_temp_today_c = unit->temperature_c;
            g_highest_temp_today_bay = i;
            g_highest_temp_today_valid = true;
        }
    }
}

static void ui_refresh_sensor(void) {
    update_highest_temp_today();
    /* Unconditional, ahead of this function's own "nothing changed"
     * gate below - a new daily-max doesn't always move the rack-wide
     * average (see that gate's own check), but should still repaint. */
    InvalidateRect(g_highest_temp_block, NULL, FALSE);
    /* Rack-wide summary - the average across the 6 physical sensors that
     * currently have a reading (see SENSOR_UNIT_ADDR_DEFAULT) - not tied to the 16
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

    /* The AVG TEMP block (avg_temp_block_subclass_proc) and the heatmap
     * (sensor_heatmap_subclass_proc) both read live off g_sensor when
     * they paint - just need a repaint kicked off here, not text pushed
     * into either one. */
    InvalidateRect(g_avg_temp_block, NULL, FALSE);
    InvalidateRect(g_sensor_heatmap, NULL, FALSE);

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

static int count_kill_switch_tripped(void) {
    int i, n = 0;
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (g_kill_switch_tripped[i]) n++;
    }
    return n;
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
        log_add_status_change(msg);
    }
}

/* Opens sensor_log.csv in whatever's associated with .csv (Excel, if
 * installed). ShellExecuteA rather than a fixed "excel.exe" path -
 * this app has no business assuming what's installed, same reasoning
 * as everywhere else "just open the thing" is the goal. Its return
 * value (cast from an HINSTANCE) IS the error code on failure, <= 32 -
 * NOT something GetLastError() reflects, unlike this file's other
 * warning helpers, so this checks the specific codes itself instead of
 * calling ui_show_warning_with_last_error(). */
static void on_open_log_clicked(void) {
    char path[MAX_PATH + 16];
    INT_PTR result;
    get_sensor_log_path(path);
    result = (INT_PTR)ShellExecuteA(g_hwnd, "open", path, NULL, NULL, SW_SHOWNORMAL);
    if (result == SE_ERR_FNF || result == SE_ERR_PNF) {
        ui_show_warning("No sensor log yet - it's written a few seconds after the first sensor reading");
    } else if (result == SE_ERR_NOASSOC) {
        ui_show_warning("Could not open sensor_log.csv - no application is associated with .csv files");
    } else if (result <= 32) {
        ui_show_warning("Could not open sensor_log.csv");
    }
}

/* Flips g_light_mode, rebuilds every theme color + the GDI objects
 * derived from them, updates the button's own label, then repaints
 * everything - RDW_ALLCHILDREN same as relayout_for_size() uses, since
 * this changes what practically every control on screen paints with,
 * not just this one control's own rect. */
static void on_theme_toggle_clicked(void) {
    g_light_mode = !g_light_mode;
    apply_theme();
    create_theme_brushes();
    SetWindowTextA(GetDlgItem(g_hwnd, IDC_THEME_TOGGLE_BTN), g_light_mode ? "Dark Mode" : "Light Mode");
    RedrawWindow(g_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

/* Per-unit reset - resets just this one unit, independent of the others.
 * Wired to a click on that unit's card status line while it's tripped
 * (see IDC_CH_STATUS_OFFSET's comment in resource.h). */
static void on_unit_kill_reset(int idx) {
    char msg[48];
    if (!g_kill_switch_tripped[idx]) {
        return;
    }
    g_kill_switch_tripped[idx] = false;
    wsprintfA(msg, "Unit %d: kill switch reset by user", idx + 1);
    log_add_status_change(msg);
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
static int channel_uptime_id(int idx)     { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_UPTIME_OFFSET; }
static int channel_freq_lbl_id(int idx)   { return IDC_CH_BASE + idx * IDC_CH_STRIDE + IDC_CH_FREQ_OFFSET; }


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
    InvalidateRect(GetDlgItem(g_hwnd, channel_uptime_id(index)), NULL, FALSE);
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
/* Gated on the connection alone, not on there being a selection (unlike
 * BULK_TARGET_BTN_IDS below) - Clear/Select All act on the selection
 * itself, and Card Click (IDC_BULK_TOGGLE_BTN) just arms a way to
 * build one. Card Click used to stay enabled even while disconnected -
 * direct request: no bus means no real "card on" state to reflect, so
 * it shouldn't be armable at all until connected. */
static const int BULK_ALWAYS_BTN_IDS[] = {
    IDC_BULK_CLEAR_BTN, IDC_BULK_SELECT_ALL_BTN, IDC_BULK_TOGGLE_BTN
};
#define BULK_ALWAYS_BTN_COUNT (sizeof(BULK_ALWAYS_BTN_IDS) / sizeof(BULK_ALWAYS_BTN_IDS[0]))

/* Set/ON/OFF/High/Medium/Low/Off - these are the ones that actually DO
 * something to the selected channels, unlike Clear/Select All (which act
 * on the selection itself, not through it). Lighting up in full color
 * the instant you connect - before picking a single channel - read as
 * "ready to fire" when clicking any of them would just be a no-op
 * (bulk_apply_mode() etc. already skip everything when nothing's
 * selected). Gated on bulk_has_selection() too now (see
 * ui_refresh_bulk_target_buttons_enabled()) so they only look armed once
 * there's actually something for them to act on - reuses the exact same
 * EnableWindow+ODS_DISABLED dimming every other button in this app
 * already gets while disconnected, just gated on selection too. */
static const int BULK_TARGET_BTN_IDS[] = {
    IDC_BULK_SET_BTN, IDC_BULK_ON_BTN, IDC_BULK_OFF_BTN,
    IDC_BULK_HIGH_BTN, IDC_BULK_MEDIUM_BTN, IDC_BULK_LOW_BTN, IDC_BULK_LEVEL_OFF_BTN
};
#define BULK_TARGET_BTN_COUNT (sizeof(BULK_TARGET_BTN_IDS) / sizeof(BULK_TARGET_BTN_IDS[0]))

/* True while any selected channel's send from a bulk action is still
 * queued/settling - drives IDC_BULK_SELECTED_LBL's "Sending..." text
 * (see ui_refresh_bulk_selected_label()), since a bulk click otherwise
 * gives no feedback of its own that anything happened - the only sign
 * used to be watching every individual card change. */
static bool bulk_any_selected_busy(void) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (g_channel_selected[i] && channels_get(i)->busy) {
            return true;
        }
    }
    return false;
}

static void ui_refresh_bulk_target_buttons_enabled(void) {
    /* Direct request: Card Click being On (plus connected) is enough by
     * itself to light these up - used to also require an actual
     * selection, which read as "nothing happens when I turn Card Click
     * on" since 0-selected is the normal starting state. Matches
     * BULK_ALWAYS_BTN_IDS' own gating now. A click with nothing selected
     * is still a no-op in practice (bulk_apply_mode() etc. skip an empty
     * selection) - this only changes how the buttons LOOK, not what a
     * click with nothing picked actually does. */
    bool enabled = conn_is_connected(&g_conn) && g_bulk_select_mode;
    unsigned i;
    for (i = 0; i < BULK_TARGET_BTN_COUNT; i++) {
        HWND btn = GetDlgItem(g_hwnd, BULK_TARGET_BTN_IDS[i]);
        EnableWindow(btn, enabled);
        InvalidateRect(btn, NULL, FALSE);
    }
}

static void set_channel_controls_enabled(bool enabled) {
    int i;
    if (!enabled) {
        g_bulk_last_power_action = BULK_POWER_NONE;
        /* No bus, no real "card on" state to reflect - Card Click
         * itself gets disabled below (BULK_ALWAYS_BTN_IDS), but force
         * it back to Off too so a disconnect doesn't leave it stuck
         * showing "On" for a mode that's no longer armable. */
        if (g_bulk_select_mode) {
            g_bulk_select_mode = false;
            SetDlgItemTextA(g_hwnd, IDC_BULK_TOGGLE_BTN, "Card Click: Off");
            ui_update_all_select_checkbox_visibility();
        }
    }
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
    for (i = 0; i < (int)BULK_ALWAYS_BTN_COUNT; i++) {
        HWND btn = GetDlgItem(g_hwnd, BULK_ALWAYS_BTN_IDS[i]);
        EnableWindow(btn, enabled);
        InvalidateRect(btn, NULL, FALSE);
    }
    ui_refresh_bulk_target_buttons_enabled();
}

/* Selection checkbox is gated on two things, both required: the RS422
 * link is actually connected (no real "on" without a port to send it
 * over), and the user has opted into bulk-select by arming Card Click
 * (IDC_BULK_TOGGLE_BTN) - it shouldn't appear as an available control
 * until they've asked for bulk selection at all. Used to also require
 * the channel itself be on, which made the checkbox appear/disappear
 * per-card depending on each one's own state - reported as looking
 * inconsistent/buggy rather than as the intentional gating it was;
 * dropped so every card shows it consistently once Card Click is
 * armed, matching every other Bulk Actions control's own "connected is
 * enough" gating. Call this anywhere either of those two can have
 * changed: IDC_BULK_TOGGLE_BTN's handler and conn_on_connected_changed()
 * need the all-channels form below since they affect every card at
 * once. Hiding it doesn't disable selection itself - the checkbox in
 * channel_select_id() still exists, and background-click selection
 * (see g_bulk_select_mode) is separate either way. */
static void ui_update_select_checkbox_visibility(int index) {
    bool show = g_bulk_select_mode && conn_is_connected(&g_conn);
    ShowWindow(GetDlgItem(g_hwnd, channel_select_id(index)), show ? SW_SHOW : SW_HIDE);
}

static void ui_update_all_select_checkbox_visibility(void) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        ui_update_select_checkbox_visibility(i);
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
    if (bulk_any_selected_busy()) {
        lstrcpynA(text, "Sending...", (int)sizeof(text));
    } else {
        wsprintfA(text, "%d selected", count);
    }
    SetDlgItemTextA(g_hwnd, IDC_BULK_SELECTED_LBL, text);
    ui_refresh_bulk_target_buttons_enabled();
}

/* Keeps IDC_BULK_ROWSELECT_COMBO's displayed choice honest. Picking a
 * preset from it should show that preset's name - but the underlying
 * selection can also change through other means that don't match any
 * preset (a single checkbox, the background-click toggle, Clear), and
 * nothing was updating the combo when that happened - it kept showing
 * whatever preset was last picked even once the real selection no
 * longer matched it at all. CB_SETCURSEL doesn't fire CBN_SELCHANGE,
 * so calling this from inside bulk_select_row()/bulk_select_all() is
 * safe even though those are themselves called FROM that combo's own
 * handler. index is 0..GRID_ROWS-1 for a row, GRID_ROWS for "Select
 * All", GRID_ROWS+1 for "Custom". */
static void bulk_set_rowselect_combo(int index) {
    SendDlgItemMessageA(g_hwnd, IDC_BULK_ROWSELECT_COMBO, CB_SETCURSEL, (WPARAM)index, 0);
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
    bulk_set_rowselect_combo(GRID_ROWS + 1); /* Custom - cleared doesn't match any preset */
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
    bulk_set_rowselect_combo(GRID_ROWS); /* "Select All" - also reached from IDC_BULK_SELECT_ALL_BTN directly */
}

/* IDC_BULK_ROWSELECT_COMBO's "1st/2nd/3rd/4th Row" options - replaces
 * the whole selection with exactly that row of the 4x4 grid (row 0 =
 * Units 1-4, row 1 = Units 5-8, ...), same as picking a fresh set by
 * hand would, not additive on top of whatever was already selected. */
static void bulk_select_row(int row) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        bool want = (i / GRID_COLS) == row;
        if (g_channel_selected[i] != want) {
            g_channel_selected[i] = want;
            ui_invalidate_card(i);
        }
    }
    ui_refresh_bulk_selected_label();
    bulk_set_rowselect_combo(row);
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

/* Command Panel's Close All/Open All - same gating convention as Bulk
 * Actions above (OFF always works even kill-switch-tripped, ON skips
 * a tripped channel), but acting on every one of the 16 channels
 * regardless of Bulk Actions' own selection - a blanket rack-wide
 * action, not a selection-based one. Each individual channel's own
 * "Unit N: ..." completion line still logs itself once its send
 * settles (see ui_refresh_channel()); this adds one audit-tagged
 * summary line for the action as a whole, same pattern the kill
 * switch's own trip/reset already uses. */
static void on_close_all_clicked(void) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        channel_turn_output_off(i);
    }
    log_add_status_change("Emergency Shutdown: every channel commanded OFF");
}

static void on_open_all_clicked(void) {
    int i;
    int skipped = 0;
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (!g_kill_switch_tripped[i]) {
            channel_turn_output_on(i);
        } else {
            skipped++;
        }
    }
    if (skipped > 0) {
        /* 96, not the 64 this used to be - "Global Activate: channels
         * commanded ON (16 skipped - kill switch tripped)" alone is 73
         * chars + NUL, already past 64 (the old "Open All:" wording was
         * already right at that edge too - wsprintfA doesn't bounds-
         * check, so this was silently one rename away from overflowing
         * the stack). */
        char msg[96];
        wsprintfA(msg, "Global Activate: channels commanded ON (%d skipped - kill switch tripped)", skipped);
        log_add_status_change(msg);
    } else {
        log_add_status_change("Global Activate: every channel commanded ON");
    }
}

/* Summary card's Reset to Default - every channel back to its true
 * factory default (mode White Noise, output off - matches
 * channels_init()'s own values), sent as real commands the same way
 * Close All/Open All above do, not a silent local-only reset. Runs
 * regardless of kill switch trips or Bulk Actions' selection - a
 * rack-wide action like Close All, not a selection-based one. Also
 * resets each card's own mode combo back to White Noise (index 0) so
 * it reads as "what was just sent", matching a fresh card's own
 * initial selection in add_channel_card(). */
/* Direct decision - the "default" state changed from White Noise/OFF
 * to White Noise/ON: every channel powered on with Pseudo Random
 * Noise. This is the SAME function used by the Reset to Default
 * button, the close-time safety reset (WM_DESTROY), and once after a
 * successful launch auto-connect (see conn_on_connected_changed()) -
 * all 3 now power on instead of off, a direct instruction acknowledging
 * this means closing the app no longer leaves every channel OFF.
 * Kill-switch-tripped channels are skipped, same convention
 * on_open_all_clicked()/bulk_turn_output_on() already use - a tripped
 * channel never gets powered back on by an automatic/bulk action. */
static void on_reset_to_default_clicked(void) {
    int i;
    int skipped = 0;
    for (i = 0; i < MAX_CHANNELS; i++) {
        channel_set_mode(i, PROTO_MODE_WHITE_NOISE);
        SendMessageA(GetDlgItem(g_hwnd, channel_mode_id(i)), CB_SETCURSEL, PROTO_MODE_WHITE_NOISE, 0);
        if (!g_kill_switch_tripped[i]) {
            channel_turn_output_on(i);
        } else {
            skipped++;
        }
    }
    if (skipped > 0) {
        /* 128, not 96 - this exact message (97 chars + NUL) would have
         * overflowed a 96-byte buffer; measured before shipping this
         * time instead of finding out the hard way (see Global
         * Activate's own comment on the same class of bug). */
        char msg[128];
        wsprintfA(msg, "Reset to Default: every channel set to Pseudo Random Noise, ON (%d skipped - kill switch tripped)", skipped);
        log_add_status_change(msg);
    } else {
        log_add_status_change("Reset to Default: every channel set to Pseudo Random Noise, ON");
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
    /* Muted mode name next to the header - kept as a real control (still
     * updated via SetWindowTextA everywhere the applied mode changes),
     * just hidden: direct request was to remove it from the card, and
     * the mode is already shown in full in the combo directly below it,
     * so the truncated "Pseudo ..." repeat here was redundant. */
    g_card_mode_lbl[index] = add_ctrl(hwnd, "STATIC", proto_mode_name(PROTO_MODE_WHITE_NOISE),
                                        SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, x + 88, y + 8, 60, 14, 0);
    ShowWindow(g_card_mode_lbl[index], SW_HIDE);

    mode_combo = add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP,
                           x + 8, y + 24, 98, 100, channel_mode_id(index));
    for (i = 0; i < PROTO_MODE_COUNT; i++) {
        const char *name = proto_mode_name((uint8_t)i);
        SendMessageA(mode_combo, CB_ADDSTRING, 0, (LPARAM)(name ? name : "?"));
    }
    SendMessageA(mode_combo, CB_SETCURSEL, PROTO_MODE_WHITE_NOISE, 0);
    SendMessageA(mode_combo, CB_SETDROPPEDWIDTH, 190, 0);
    make_combo_readonly_ex(mode_combo, g_card_combo_overlays[index]);

    /* Set narrowed from 40 to 32 and the gap before the gauge column
     * trimmed from 14 to 6 - both borrowed to give the mode combo above
     * (previously truncating every mode name) the extra width instead. */
    add_ctrl(hwnd, "BUTTON", "Set", BS_OWNERDRAW | WS_TABSTOP,
             x + 110, y + 24, 32, 21, channel_set_id(index));

    add_ctrl(hwnd, "BUTTON", "ON", BS_OWNERDRAW | WS_TABSTOP,
             x + 8, y + 47, 60, 21, channel_on_id(index));
    add_ctrl(hwnd, "BUTTON", "OFF", BS_OWNERDRAW | WS_TABSTOP,
             x + 72, y + 47, 60, 21, channel_off_id(index));

    /* SS_NOTIFY: this label doubles as the per-unit kill-switch reset -
     * see IDC_CH_STATUS_OFFSET's comment in resource.h. */
    add_ctrl(hwnd, "STATIC", "STANDBY", SS_LEFT | SS_NOPREFIX | SS_NOTIFY,
             x + 8, y + 70, 130, 14, channel_status_id(index));

    /* Cumulative ON-time odometer, right below the status line - see
     * IDC_CH_UPTIME_OFFSET's comment in resource.h. Monospace so the
     * digits don't jitter/reflow width as they tick over. */
    {
        HWND uptime_ctrl = add_ctrl(hwnd, "STATIC", "Up 00:00:00", SS_LEFT | SS_NOPREFIX,
                                     x + 8, y + 86, 130, 12, channel_uptime_id(index));
        if (uptime_ctrl) {
            SendMessageA(uptime_ctrl, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
        }
    }

    /* Real operating frequency RANGE (not just the center), above the
     * gauge column - white text, direct request. low/high from the
     * same channel_freq_mhz()/channel_bandwidth_mhz() the spectrum
     * plot's axis uses (see draw_spectrum_freq_axis()), so this and
     * that stay consistent. Static per card - these never change for a
     * given unit - set once here rather than refreshed per tick. */
    {
        char freq_label[24];
        int freq = channel_freq_mhz(index);
        int half_bw = channel_bandwidth_mhz(index) / 2;
        HWND freq_ctrl;
        wsprintfA(freq_label, "%d-%d MHz", freq - half_bw, freq + half_bw);
        /* Left-aligned right next to the "Unit N" header (which ends at
         * x+84), same line - direct request, replacing the old
         * right-aligned x+90..x+238 box. That box's right edge sat only
         * ~10px clear of the selection checkbox (x+CARD_W-24) at
         * CARD_W's own minimum (272) - fine at the card's usual wider
         * runtime size, but tight enough to actually overlap/get
         * covered by the checkbox at smaller widths (direct report).
         * Anchored to the header's own fixed left-side position instead
         * of the checkbox's card-width-relative one, this can't
         * collide with it at any card width the grid ever produces. */
        freq_ctrl = add_ctrl(hwnd, "STATIC", freq_label, SS_LEFT | SS_NOPREFIX,
                              x + 88, y + 8, 110, 14, channel_freq_lbl_id(index));
        if (freq_ctrl) {
            SendMessageA(freq_ctrl, WM_SETFONT, (WPARAM)g_small_font, TRUE);
        }
    }

    /* Right column: custom gradient level gauge (Off at bottom, High at
     * top, like a volume slider) + tick labels. Track widened 22->40 -
     * the slider draws itself relative to its own control width
     * (track_w = w/4 in channel_gauge_subclass_proc, click handling is
     * y-only), so it's a real wider element, not just extra blank
     * padding - direct complaint that the right side of the card sat
     * mostly empty next to the gauge. Labels shifted right to follow. */
    add_channel_gauge(hwnd, x + 148, y + 24, 40, 72, channel_track_id(index));

    add_ctrl(hwnd, "STATIC", "High",   SS_LEFT | SS_NOPREFIX, x + 194, y + 24, 44, 14, channel_lbl_high_id(index));
    add_ctrl(hwnd, "STATIC", "Mid",    SS_LEFT | SS_NOPREFIX, x + 194, y + 42, 44, 14, channel_lbl_medium_id(index));
    add_ctrl(hwnd, "STATIC", "Low",    SS_LEFT | SS_NOPREFIX, x + 194, y + 60, 44, 14, channel_lbl_low_id(index));
    add_ctrl(hwnd, "STATIC", "Off",    SS_LEFT | SS_NOPREFIX, x + 194, y + 78, 44, 14, channel_lbl_off_id(index));

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

static void ui_invalidate_all_channel_cache(void) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++) {
        g_ui_cache[i].valid = false;
    }
}

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
        log_add_status_change(log_line);
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
    } else if (ch->output_on && conn_is_connected(&g_conn)) {
        /* Gated on connection too, same reasoning as the card border
         * (card_panel_subclass_proc) - a level restored from the .ini
         * shouldn't read as "currently HIGH" before the link's even up. */
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
        ui_update_select_checkbox_visibility(index);
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

    /* Trace only draws while actually connected - ch->output_on is this
     * app's own remembered state (can still read true after a
     * disconnect, e.g. restored from the .ini or just not turned off
     * before unplugging), not whether there's a live RS422 link right
     * now. The whole point of this trace is that it's real, commanded
     * state, not a guess - once disconnected, this app isn't actually
     * commanding anything anymore, so there's nothing real left to
     * show. */
    if (!ch->output_on || !conn_is_connected(&g_conn) || w < 12 || h < 10) {
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

/* Real frequency ticks under the single-channel plot - left edge,
 * center, right edge of THIS channel's actual band (channel_freq_mhz()/
 * channel_bandwidth_mhz(), the same confirmed real values the caption
 * and the actual transmitted signal use - see draw_channel_spectrum's
 * comment). Direct fix: the grid/trace are an honest shape (real mode/
 * level), but the x-axis previously had no numbers on it at all, so it
 * couldn't "match" any real frequency - now it does. Only drawn for a
 * single selected channel; the all-16 grid has no single axis to
 * label (each of the 16 cells is its own unrelated band). */
static void draw_spectrum_freq_axis(HDC hdc, RECT axis_rc, RECT grid_rc, int channel_index) {
    int freq = channel_freq_mhz(channel_index);
    int half_bw = channel_bandwidth_mhz(channel_index) / 2;
    char lo_label[16], hi_label[16];
    HFONT old_font;

    wsprintfA(lo_label, "%d", freq - half_bw);
    wsprintfA(hi_label, "%d", freq + half_bw);

    old_font = (HFONT)SelectObject(hdc, g_font);
    SetTextColor(hdc, COLOR_APP_MUTED);
    SetBkMode(hdc, TRANSPARENT);
    {
        RECT lo_rc = axis_rc; lo_rc.left = grid_rc.left; lo_rc.right = grid_rc.left + 60;
        RECT hi_rc = axis_rc; hi_rc.right = grid_rc.right; hi_rc.left = hi_rc.right - 60;
        DrawTextA(hdc, lo_label, -1, &lo_rc, DT_SINGLELINE | DT_NOCLIP | DT_LEFT | DT_VCENTER);
        DrawTextA(hdc, hi_label, -1, &hi_rc, DT_SINGLELINE | DT_NOCLIP | DT_RIGHT | DT_VCENTER);
    }
    SelectObject(hdc, old_font);
}

static LRESULT CALLBACK spectrum_plot_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc, grid_rc;
        static const int axis_h = 14;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_brush_field);

        grid_rc = rc;
        if (!g_spectrum_show_all) {
            grid_rc.bottom -= axis_h;
        }
        spectrum_draw_grid(hdc, grid_rc);

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
            RECT axis_rc = rc;
            axis_rc.top = grid_rc.bottom;
            if (ch->output_on) {
                wsprintfA(caption, "%s", proto_mode_name(ch->mode));
            } else {
                wsprintfA(caption, "%s - STANDBY", proto_mode_name(ch->mode));
            }
            draw_channel_spectrum(hdc, grid_rc, ch, NULL, caption);
            draw_spectrum_freq_axis(hdc, axis_rc, grid_rc, g_spectrum_unit);
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

/* Writes whatever's currently in the Sensor Port combo - called right
 * after a successful sensor_connect() (manual click or startup auto-
 * connect), not on close, so it's remembered even if the app never
 * gets a clean exit. See these functions' own forward-declaration
 * comment for why only this one field still persists. */
static void save_sensor_port(void) {
    char path[MAX_PATH + 8];
    char buf[32];

    get_ini_path(path);
    GetDlgItemTextA(g_hwnd, IDC_SENSOR_PORT_COMBO, buf, sizeof(buf));
    WritePrivateProfileStringA("Sensor", "Port", buf, path);
}

/* Call after refresh_sensor_port_list() has populated the combo -
 * selects the remembered port by name if it's still in the list
 * (matches whatever real port name got saved, any COM number); a
 * port no longer present (unplugged, or none ever saved) just leaves
 * refresh_combo_ports()'s own index-0 default in place. */
static void load_sensor_port(void) {
    char path[MAX_PATH + 8];
    char buf[32];
    HWND combo;
    LRESULT idx;

    get_ini_path(path);
    if (GetPrivateProfileStringA("Sensor", "Port", "", buf, sizeof(buf), path) == 0) {
        return;
    }
    combo = GetDlgItem(g_hwnd, IDC_SENSOR_PORT_COMBO);
    idx = SendMessageA(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)buf);
    if (idx != CB_ERR) {
        SendMessageA(combo, CB_SETCURSEL, (WPARAM)idx, 0);
    }
}

/* branding.bmp, sibling to the .exe and .ini - a fixed name/location,
 * not the user's originally-picked file's own path. Copying into a
 * name this app owns (rather than just remembering their path) means
 * the logo doesn't silently revert to default the next time they move,
 * rename, or delete whatever they picked it from - same portable,
 * no-installer reasoning as get_ini_path(). */
static void get_branding_bmp_path(char *path /* at least MAX_PATH + 12 bytes */) {
    char *dot;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    dot = strrchr(path, '\\');
    if (dot) {
        dot[1] = '\0';
    } else {
        path[0] = '\0';
    }
    lstrcatA(path, "branding.bmp");
}

/* branding\icon.ico, sibling to the .exe - a dedicated icon file,
 * separate from branding.bmp (the in-app header logo): that one gets
 * cropped/scaled onto a solid square to double as the taskbar icon
 * (build_custom_app_icon()), which never looks as clean as a real
 * purpose-made .ico. Direct request for a real icon override, in its
 * own branding\ folder so it reads as "drop your icon here" rather
 * than one more loose file next to the exe. Takes priority over the
 * logo-derived icon when both exist (see WM_CREATE - applied after
 * apply_custom_app_icon(), so it's the one left showing). */
static void get_branding_icon_path(char *path /* at least MAX_PATH + 20 bytes */) {
    char *dot;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    dot = strrchr(path, '\\');
    if (dot) {
        dot[1] = '\0';
    } else {
        path[0] = '\0';
    }
    lstrcatA(path, "branding\\icon.ico");
}

/* Called once at startup, after apply_custom_app_icon() - overrides the
 * window/taskbar/alt-tab icon with branding\icon.ico if present, same
 * "next to the exe" portability as branding.bmp. Falls back to leaving
 * whatever icon is already showing (the built-in one, or the logo-
 * derived one) if the file is missing or LoadImageA can't read it -
 * never a startup error, just nothing to override with. Records
 * whether a custom icon was actually applied this run in the .ini
 * (informational - direct request), same section/key checked again
 * next launch by nothing else; the file's own presence is still what
 * actually drives the override, matching branding.bmp's own convention
 * of the file itself being the state, not a separate settings flag. */
static void load_branding_icon(HWND hwnd) {
    char path[MAX_PATH + 20];
    char ini_path[MAX_PATH + 8];
    HICON big, small;
    bool applied = false;

    get_branding_icon_path(path);
    big = (HICON)LoadImageA(NULL, path, IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
    if (big) {
        SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
        applied = true;
    }
    small = (HICON)LoadImageA(NULL, path, IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
    if (small) {
        SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
        applied = true;
    }

    get_ini_path(ini_path);
    WritePrivateProfileStringA("Branding", "CustomIcon", applied ? "1" : "0", ini_path);
}

/* Called once at startup - if a previous browse_and_set_logo() left a
 * branding.bmp behind, load it so the custom logo survives a restart.
 * Silently falls back to the built-in vector mark (g_custom_logo_bmp
 * stays NULL) if the file's missing or LoadImageA can't read it - a
 * corrupt/foreign file here should never be a startup error, just a
 * reason to fall back. */
static void load_custom_logo(void) {
    char path[MAX_PATH + 16];
    get_branding_bmp_path(path);
    g_custom_logo_bmp = (HBITMAP)LoadImageA(NULL, path, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
}

/* Builds an icon_size x icon_size HICON from g_custom_logo_bmp - scaled
 * to fit preserving aspect ratio and centered on a solid
 * COLOR_APP_PANEL_BG square (same footprint math as the header's own
 * StretchBlt), so the taskbar/title-bar icon can match a custom logo
 * too, not just the header. NULL if g_custom_logo_bmp isn't set or a
 * GDI call along the way fails - caller falls back to leaving whatever
 * icon is already showing. The mask is a plain all-opaque monochrome
 * bitmap (no transparency - this app has no real alpha pipeline), so
 * the icon reads as a small solid square, same as any other app icon
 * with a filled background. */
static HICON build_custom_app_icon(int icon_size) {
    BITMAP bm;
    HDC screen_dc, mem_dc, src_dc, mask_dc;
    HBITMAP color_bmp, mask_bmp, old_mem, old_src, old_mask;
    HBRUSH bg_brush;
    RECT rc;
    ICONINFO ii;
    HICON icon;

    if (!g_custom_logo_bmp || !GetObject(g_custom_logo_bmp, sizeof(bm), &bm) ||
        bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        return NULL;
    }

    screen_dc = GetDC(NULL);
    mem_dc = CreateCompatibleDC(screen_dc);
    color_bmp = CreateCompatibleBitmap(screen_dc, icon_size, icon_size);
    old_mem = (HBITMAP)SelectObject(mem_dc, color_bmp);

    rc.left = 0; rc.top = 0; rc.right = icon_size; rc.bottom = icon_size;
    bg_brush = CreateSolidBrush(COLOR_APP_PANEL_BG);
    FillRect(mem_dc, &rc, bg_brush);
    DeleteObject(bg_brush);

    {
        double sx = (double)icon_size / bm.bmWidth;
        double sy = (double)icon_size / bm.bmHeight;
        double s = sx < sy ? sx : sy;
        int dw = (int)(bm.bmWidth * s + 0.5);
        int dh = (int)(bm.bmHeight * s + 0.5);
        src_dc = CreateCompatibleDC(screen_dc);
        old_src = (HBITMAP)SelectObject(src_dc, g_custom_logo_bmp);
        StretchBlt(mem_dc, (icon_size - dw) / 2, (icon_size - dh) / 2, dw, dh,
                   src_dc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
        SelectObject(src_dc, old_src);
        DeleteDC(src_dc);
    }
    SelectObject(mem_dc, old_mem);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);

    mask_bmp = CreateBitmap(icon_size, icon_size, 1, 1, NULL);
    mask_dc = CreateCompatibleDC(NULL);
    old_mask = (HBITMAP)SelectObject(mask_dc, mask_bmp);
    FillRect(mask_dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH)); /* AND-mask 0 = opaque everywhere */
    SelectObject(mask_dc, old_mask);
    DeleteDC(mask_dc);

    ii.fIcon = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask = mask_bmp;
    ii.hbmColor = color_bmp;
    icon = CreateIconIndirect(&ii);

    DeleteObject(color_bmp);
    DeleteObject(mask_bmp);
    return icon;
}

/* Swaps the window's title-bar/taskbar/alt-tab icons (WM_SETICON, not
 * the window CLASS icon - this app only ever has the one window, so a
 * per-window icon is simpler than touching the class) to match
 * g_custom_logo_bmp. No-op if it isn't set - the window keeps showing
 * whatever icon it already has (the embedded resource one at first
 * launch). Call after g_custom_logo_bmp changes: load_custom_logo() at
 * startup and browse_and_set_logo() after a pick. */
static void apply_custom_app_icon(HWND hwnd) {
    HICON big, small;

    if (!g_custom_logo_bmp) {
        return;
    }
    big = build_custom_app_icon(32);
    if (big) {
        SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (g_custom_icon_big) {
            DestroyIcon(g_custom_icon_big);
        }
        g_custom_icon_big = big;
    }
    small = build_custom_app_icon(16);
    if (small) {
        SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
        if (g_custom_icon_small) {
            DestroyIcon(g_custom_icon_small);
        }
        g_custom_icon_small = small;
    }
}

/* Writes an HBITMAP out as a plain 24-bit BMP file - lets a picked
 * .ico (see load_icon_as_bitmap() below) land on disk through the
 * exact same branding.bmp path/format load_custom_logo() already
 * knows how to read at startup, no change needed anywhere else. */
static bool save_hbitmap_as_bmp(HBITMAP bmp, const char *path) {
    BITMAP bm;
    BITMAPFILEHEADER bfh;
    BITMAPINFOHEADER bih;
    HDC hdc;
    void *bits;
    DWORD row_bytes, image_size, written;
    HANDLE file;
    bool ok = false;

    if (!GetObject(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        return false;
    }

    row_bytes = ((DWORD)bm.bmWidth * 3 + 3) & ~3u; /* 24bpp, DWORD-aligned rows */
    image_size = row_bytes * (DWORD)bm.bmHeight;
    bits = malloc(image_size);
    if (!bits) {
        return false;
    }

    ZeroMemory(&bih, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = bm.bmWidth;
    bih.biHeight = bm.bmHeight; /* bottom-up, standard BMP */
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = image_size;

    hdc = GetDC(NULL);
    if (GetDIBits(hdc, bmp, 0, (UINT)bm.bmHeight, bits, (BITMAPINFO *)&bih, DIB_RGB_COLORS) == 0) {
        ReleaseDC(NULL, hdc);
        free(bits);
        return false;
    }
    ReleaseDC(NULL, hdc);

    ZeroMemory(&bfh, sizeof(bfh));
    bfh.bfType = 0x4D42; /* 'BM' */
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
    bfh.bfSize = bfh.bfOffBits + image_size;

    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        if (WriteFile(file, &bfh, sizeof(bfh), &written, NULL) &&
            WriteFile(file, &bih, sizeof(bih), &written, NULL) &&
            WriteFile(file, bits, image_size, &written, NULL)) {
            ok = true;
        }
        CloseHandle(file);
    }
    free(bits);
    return ok;
}

/* Loads a .ico file through Windows' own icon loader and pulls out its
 * color bitmap - the same trick app.ico itself relies on (see that
 * file's comment): the OS icon loader already decodes a PNG-compressed
 * frame if the .ico has one, so this gets PNG-sourced logos working
 * without linking GDI+ or another image library just for this one
 * picker. Tries progressively smaller requested sizes since an .ico
 * missing a frame at one size isn't a hard failure - LoadImageA just
 * picks its closest match. Returns NULL (caller reports a load error)
 * if nothing usable comes back at any size. */
static HBITMAP load_icon_as_bitmap(const char *path) {
    static const int sizes[] = { 256, 128, 64, 48, 32, 16 };
    size_t i;
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        HICON icon = (HICON)LoadImageA(NULL, path, IMAGE_ICON, sizes[i], sizes[i], LR_LOADFROMFILE);
        if (icon) {
            ICONINFO ii;
            HBITMAP color = NULL;
            if (GetIconInfo(icon, &ii)) {
                color = ii.hbmColor;
                if (ii.hbmMask) {
                    DeleteObject(ii.hbmMask);
                }
            }
            DestroyIcon(icon);
            if (color) {
                return color;
            }
        }
    }
    return NULL;
}

static bool has_extension(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    return dot && lstrcmpiA(dot, ext) == 0;
}

/* Sniffs the real file signature rather than trusting the extension -
 * a browser's "Save Image As" routinely hands back a WebP file with a
 * .bmp/.jpg/.png extension on it (confirmed directly: a user-reported
 * "not a loadable BMP" turned out to be a file starting with
 * "RIFF....WEBP", the standard WebP container signature). GDI+'s
 * built-in codec set is BMP/GIF/JPEG/PNG/TIFF only - no WebP - so
 * load_image_as_bitmap_gdiplus() would just fail on one of these with
 * the same generic "Could not read that image file" as any other
 * corrupt/unsupported file, giving no hint that the real problem is
 * "this isn't actually the format its extension claims". Checking this
 * up front means the warning can say exactly that instead. */
static bool is_webp_file(const char *path) {
    HANDLE file;
    unsigned char header[12];
    DWORD read_len = 0;
    bool result;

    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    result = ReadFile(file, header, sizeof(header), &read_len, NULL) && read_len == sizeof(header) &&
             memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WEBP", 4) == 0;
    CloseHandle(file);
    return result;
}

/* Loads a .png/.jpg/.jpeg through GDI+ (gdiplus.dll, shipped with
 * Windows since XP - a system DLL this links against, same as
 * user32/gdi32, not a file this app has to bundle) and returns its
 * color bitmap, ready for save_hbitmap_as_bmp() exactly like
 * load_icon_as_bitmap()'s result above. Starts/stops GDI+ around just
 * this one call rather than keeping it running for the app's whole
 * lifetime, since logo-picking is the only thing that ever needs it.
 * Composited onto COLOR_APP_PANEL_BG while decoding - this app has no
 * real alpha-blit pipeline for a logo bitmap once loaded (see
 * draw_app_logo_faded()'s comment on why that's a special-cased
 * exception, not the norm), so a transparent PNG needs to flatten onto
 * *something* now rather than go solid black. Returns NULL (caller
 * reports a load error) on any failure - unrecognized/corrupt file,
 * GDI+ missing, whatever. */
static HBITMAP load_image_as_bitmap_gdiplus(const char *path) {
    ULONG_PTR token;
    GdiplusStartupInput input;
    GpBitmap *image = NULL;
    HBITMAP hbmp = NULL;
    WCHAR wpath[MAX_PATH];
    ARGB bg;

    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH) == 0) {
        return NULL;
    }

    ZeroMemory(&input, sizeof(input));
    input.GdiplusVersion = 1;
    if (GdiplusStartup(&token, &input, NULL) != Ok) {
        return NULL;
    }

    bg = 0xFF000000u
        | ((ARGB)GetRValue(COLOR_APP_PANEL_BG) << 16)
        | ((ARGB)GetGValue(COLOR_APP_PANEL_BG) << 8)
        | (ARGB)GetBValue(COLOR_APP_PANEL_BG);

    if (GdipCreateBitmapFromFile(wpath, &image) == Ok && image) {
        GdipCreateHBITMAPFromBitmap(image, &hbmp, bg);
        GdipDisposeImage((GpImage *)image);
    }

    GdiplusShutdown(token);
    return hbmp;
}

/* IDC_CHANGE_LOGO_BTN's handler - browse for a .bmp/.ico/.png/.jpg,
 * land it as branding.bmp (see that function's comment for why a
 * copy, not just remembering the path), reload it, and record the
 * originally-picked filename in the .ini purely for display/reference
 * (SourceFile is never read back to decide what to load -
 * branding.bmp's own presence is the one thing that decides that, so
 * the two can never disagree with each other).
 *
 * Every raster format (.bmp included) goes through GDI+
 * (load_image_as_bitmap_gdiplus()) rather than a raw CopyFileA for
 * .bmp - a real-world .bmp can be RLE-compressed, a V4/V5-header
 * variant, or some other flavor plain LoadImageA(IMAGE_BITMAP) can't
 * parse even though the file is perfectly valid (confirmed: a real
 * user's .bmp reloaded here as "not a loadable BMP" after copying
 * through fine). GDI+ decodes all of that far more thoroughly, and
 * since it's re-saved through save_hbitmap_as_bmp() either way, the
 * result is always our own simple, guaranteed-loadable 24bpp output -
 * so this class of failure can't recur regardless of the source BMP's
 * own internal format. Only .ico is still special-cased (via
 * load_icon_as_bitmap()'s Windows-icon-loader trick) since GDI+ itself
 * doesn't decode the .ico container format. */
static void browse_and_set_logo(HWND hwnd) {
    char picked[MAX_PATH];
    char branding_path[MAX_PATH + 16];
    OPENFILENAMEA ofn;
    HBITMAP loaded;

    picked[0] = '\0';
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "Image Files (*.bmp;*.ico;*.png;*.jpg;*.jpeg)\0*.bmp;*.ico;*.png;*.jpg;*.jpeg\0All Files\0*.*\0";
    ofn.lpstrFile = picked;
    ofn.nMaxFile = sizeof(picked);
    ofn.lpstrTitle = "Choose a Logo";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameA(&ofn)) {
        return; /* cancelled - no error, nothing to do */
    }

    get_branding_bmp_path(branding_path);
    /* Re-picking the file that's already branding.bmp itself (the .ini's
     * "SourceFile" display invites exactly that) needs to be a no-op -
     * GDI+ can't usefully "decode and re-save" a file onto itself
     * either (opens it, then tries to write the same path while GDI+
     * still holds it open for read). */
    if (lstrcmpiA(picked, branding_path) == 0) {
        /* already in place */
    } else if (has_extension(picked, ".ico")) {
        HBITMAP extracted = load_icon_as_bitmap(picked);
        bool saved = extracted && save_hbitmap_as_bmp(extracted, branding_path);
        if (extracted) {
            DeleteObject(extracted);
        }
        if (!saved) {
            ui_show_warning_with_last_error("Could not read that .ico file");
            return;
        }
    } else if (is_webp_file(picked)) {
        /* GDI+ would just fail on this too (no WebP codec) - catching
         * it here first means the warning can say what's actually
         * wrong instead of a generic decode failure. Kept short - both
         * ui_show_warning() and log_add() use a fixed 160-byte buffer
         * and wsprintfA (unlike snprintf) doesn't truncate on overflow. */
        ui_show_warning("That's actually a WebP image (common from browser saves) "
                         "- GDI+ can't decode WebP. Convert to PNG/JPG/BMP first.");
        return;
    } else {
        HBITMAP extracted = load_image_as_bitmap_gdiplus(picked);
        bool saved = extracted && save_hbitmap_as_bmp(extracted, branding_path);
        if (extracted) {
            DeleteObject(extracted);
        }
        if (!saved) {
            ui_show_warning_with_last_error("Could not read that image file");
            return;
        }
    }

    loaded = (HBITMAP)LoadImageA(NULL, branding_path, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
    if (!loaded) {
        ui_show_warning_with_last_error("That file isn't a loadable BMP");
        return;
    }
    if (g_custom_logo_bmp) {
        DeleteObject(g_custom_logo_bmp);
    }
    g_custom_logo_bmp = loaded;
    apply_custom_app_icon(hwnd);

    {
        const char *base = strrchr(picked, '\\');
        char ini_path[MAX_PATH + 8];
        get_ini_path(ini_path);
        WritePrivateProfileStringA("Branding", "SourceFile", base ? base + 1 : picked, ini_path);
    }

    /* Plain InvalidateRect(g_header_panel, ...) was tried first here and
     * left every sibling control in the header (Connection & Settings,
     * Bulk Actions, Ambient Temperature) blank until it next happened
     * to repaint on its own - same failure relayout_for_size() already
     * documents needing RDW_ALLCHILDREN for. Match that fix: invalidate
     * the whole window, not just this one panel. */
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

/* IDC_RESET_LOGO_BTN's handler - undoes browse_and_set_logo(): deletes
 * branding.bmp (so a future startup's load_custom_logo() has nothing
 * to find), frees the in-memory bitmap, hands the taskbar/title-bar
 * icon back to the original embedded IDI_APP_ICON handles (see
 * g_default_icon_big/small), and clears the .ini's SourceFile display
 * field. No-op (but still harmless) if there was never a custom logo
 * to begin with - DeleteFileA on a file that isn't there just fails
 * quietly, nothing here treats that as an error worth surfacing. */
static void reset_custom_logo(HWND hwnd) {
    char branding_path[MAX_PATH + 16];
    char ini_path[MAX_PATH + 8];

    get_branding_bmp_path(branding_path);
    DeleteFileA(branding_path);

    if (g_custom_logo_bmp) {
        DeleteObject(g_custom_logo_bmp);
        g_custom_logo_bmp = NULL;
    }

    SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_default_icon_big);
    SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_default_icon_small);
    if (g_custom_icon_big) {
        DestroyIcon(g_custom_icon_big);
        g_custom_icon_big = NULL;
    }
    if (g_custom_icon_small) {
        DestroyIcon(g_custom_icon_small);
        g_custom_icon_small = NULL;
    }

    get_ini_path(ini_path);
    WritePrivateProfileStringA("Branding", "SourceFile", NULL, ini_path);

    log_add("Logo reset to default");
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

/* 'Xd HH:MM:SS' past a day, else plain 'HH:MM:SS' - see the uptime
 * globals' comment. out must be at least 20 bytes (wsprintfA itself has
 * no length limit to pass through, so that's on the caller). */
static void format_uptime(char *out, ULONGLONG total_seconds) {
    ULONGLONG days = total_seconds / 86400;
    unsigned hours = (unsigned)((total_seconds % 86400) / 3600);
    unsigned minutes = (unsigned)((total_seconds % 3600) / 60);
    unsigned seconds = (unsigned)(total_seconds % 60);
    if (days > 0) {
        wsprintfA(out, "%ud %02u:%02u:%02u", (unsigned)days, hours, minutes, seconds);
    } else {
        wsprintfA(out, "%02u:%02u:%02u", hours, minutes, seconds);
    }
}

/* idx's real-time total: everything already accumulated, plus (while
 * currently on) however long the CURRENT on-period has run so far. */
static ULONGLONG channel_uptime_seconds(int idx) {
    ULONGLONG total = g_channel_uptime_base_seconds[idx];
    if (channels_get(idx)->output_on) {
        total += (GetTickCount64() - g_channel_on_since_ms[idx]) / 1000;
    }
    return total;
}

/* save_settings()/load_settings() (RS422 Port/Baud/DataBits/Parity,
 * Sensor Port, Light Mode, and per-channel Mode/Level/Output/Uptime,
 * read/written to the .ini next to the exe) were removed here - direct
 * decision, same reasoning as load_channel_settings() above: nothing
 * persists across a close/reopen anymore, every launch starts at
 * defaults and whatever port actually gets auto-connected, not a
 * remembered one. Close now resets every channel to default for real
 * (see WM_CLOSE/WM_DESTROY) instead of writing state to disk. Full
 * original implementation is recoverable from git history if this
 * ever needs reverting. */

/* load_channel_settings() (restored each channel's saved mode/level/
 * output_on/uptime from the .ini on launch) was removed here - direct
 * decision: every launch now starts every channel at its channels_init()
 * defaults, ignoring whatever was saved before closing. Full original
 * implementation (including why restored level was capped to LEVEL_LOW)
 * is recoverable from git history if this ever needs reverting. */

/* IDD_CW_PASSWORD's DLGPROC - just collects whatever was typed into
 * IDC_CW_PW_EDIT on OK, leaves g_cw_pw_input untouched on Cancel (caller
 * checks the DialogBoxParamA return value to tell the two apart). */
static INT_PTR CALLBACK cw_password_dlg_proc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (msg) {
        case WM_INITDIALOG:
            SendDlgItemMessageA(hDlg, IDC_CW_PW_EDIT, EM_LIMITTEXT, sizeof(g_cw_pw_input) - 1, 0);
            return TRUE; /* let the dialog manager focus the first tab stop (the edit box) */
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                GetDlgItemTextA(hDlg, IDC_CW_PW_EDIT, g_cw_pw_input, sizeof(g_cw_pw_input));
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
            break;
        default:
            break;
    }
    return FALSE;
}

/* The shared admin gate - originally just for arming Continuous Wave
 * (a fixed, undithered carrier - the one mode this app gates behind a
 * password before a channel's Set/Bulk Set can arm it), now also
 * covering the Command Panel's "Icon" button (IDC_CMD_CHANGE_ICON_BTN,
 * whose popup menu offers Change Logo/Reset - see its own WM_COMMAND
 * handler; this used to gate a separate lock badge that revealed those
 * two as plain buttons, before they moved onto that popup menu).
 * Checked against Transit.dll itself, not anything this
 * app invents or stores - but which export does that depends on the DLL
 * build (see transit_dll.h): the older build exports GetDllPassword
 * (fetch the real password, compare locally), a newer one drops that
 * and exports ValidateDllPassword instead (best guess: hand it the
 * candidate password, it tells you if that's correct) - so the prompt
 * has to come first here and the two paths diverge only in how the
 * entered text gets checked. Authorized once per run: unlocking through
 * either entry point covers both for the rest of the session, via the
 * one shared g_cw_authorized flag. */
static bool unlock_cw(HWND hwnd) {
    INT_PTR result;

    if (g_cw_authorized) {
        return true;
    }

    if (!transit_dll_is_loaded(&g_conn.dll) ||
        (g_conn.dll.get_dll_password == NULL && g_conn.dll.validate_dll_password == NULL)) {
        ui_show_warning("Admin password check needs Transit.dll loaded first - "
                         "connect to the RS422 dongle, then try again.");
        return false;
    }

    if (g_conn.dll.get_dll_password != NULL) {
        const char *real_password = g_conn.dll.get_dll_password();
        if (real_password == NULL || real_password[0] == '\0') {
            ui_show_warning("Admin password check failed - GetDllPassword returned nothing.");
            return false;
        }

        g_cw_pw_input[0] = '\0';
        result = DialogBoxParamA(GetModuleHandle(NULL), MAKEINTRESOURCE(IDD_CW_PASSWORD), hwnd, cw_password_dlg_proc, 0);
        if (result != IDOK) {
            return false; /* cancelled */
        }

        if (lstrcmpA(g_cw_pw_input, real_password) != 0) {
            ui_show_warning("Wrong password.");
            SecureZeroMemory(g_cw_pw_input, sizeof(g_cw_pw_input));
            return false;
        }
    } else {
        long valid;

        g_cw_pw_input[0] = '\0';
        result = DialogBoxParamA(GetModuleHandle(NULL), MAKEINTRESOURCE(IDD_CW_PASSWORD), hwnd, cw_password_dlg_proc, 0);
        if (result != IDOK) {
            return false; /* cancelled */
        }

        valid = g_conn.dll.validate_dll_password(g_cw_pw_input);
        if (valid == 0) {
            ui_show_warning("Wrong password.");
            SecureZeroMemory(g_cw_pw_input, sizeof(g_cw_pw_input));
            return false;
        }
    }

    SecureZeroMemory(g_cw_pw_input, sizeof(g_cw_pw_input));
    g_cw_authorized = true;
    log_add("Admin access unlocked for this session.");
    return true;
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

    /* Change Logo/Reset no longer live as their own buttons under the
     * wordmark, and the lock badge that used to gate/reveal them is
     * gone too (direct request) - both actions moved onto the Command
     * Panel's "Icon" button instead (IDC_CMD_CHANGE_ICON_BTN's own
     * WM_COMMAND handler below): a click there still goes through the
     * same unlock_cw() admin gate the lock badge used to, then shows a
     * small popup menu with both actions, reusing IDC_CHANGE_LOGO_BTN/
     * IDC_RESET_LOGO_BTN as the popup's own menu-item IDs (a menu
     * selection's WM_COMMAND has HIWORD(wParam)==0, same as
     * BN_CLICKED, so the existing handlers for those two IDs fire
     * unchanged - no new IDs needed). */

    /* Left-aligned against the header panel's own left edge, matching
     * every other section's left margin (22px) - was right-of-center
     * (tucked up against Amplifier Temperature), leaving the whole left
     * half of the header empty. Bulk Actions now takes the middle
     * column, Amplifier Temperature stays right-aligned. */
    add_header_icon(hwnd, 36 + CONN_X_SHIFT, 14, ICON_PLUG);
    add_header(hwnd, "Connection && Settings", 54 + CONN_X_SHIFT, 14, 260, 18);

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
    add_ctrl(hwnd, "STATIC", "Port:", SS_LEFT, 63 + CONN_X_SHIFT, 36, 32, 16, 0);
    make_combo_readonly(add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 99 + CONN_X_SHIFT, 34, 90, 140, IDC_PORT_COMBO));
    add_ctrl(hwnd, "STATIC", "Disconnected", SS_LEFT | SS_NOPREFIX, 201 + CONN_X_SHIFT, 36, 100, 16, IDC_CONN_STATUS_LBL);
    add_ctrl(hwnd, "BUTTON", "Refresh", BS_OWNERDRAW | WS_TABSTOP, 105 + CONN_X_SHIFT, 63, 64, 18, IDC_REFRESH_BTN);
    add_ctrl(hwnd, "BUTTON", "Connect", BS_OWNERDRAW | WS_TABSTOP, 177 + CONN_X_SHIFT, 63, 72, 18, IDC_CONNECT_BTN);

    add_ctrl(hwnd, "STATIC", "Baud:", SS_LEFT, 113 + CONN_X_SHIFT, 91, 34, 16, 0);
    make_combo_readonly(add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 151 + CONN_X_SHIFT, 89, 90, 140, IDC_BAUD_COMBO));
    add_ctrl(hwnd, "STATIC", "Data Bits:", SS_LEFT, 57 + CONN_X_SHIFT, 120, 60, 16, 0);
    make_combo_readonly(add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 121 + CONN_X_SHIFT, 118, 45, 100, IDC_DATABITS_COMBO));
    add_ctrl(hwnd, "STATIC", "Parity:", SS_LEFT, 182 + CONN_X_SHIFT, 120, 40, 16, 0);
    make_combo_readonly(add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 226 + CONN_X_SHIFT, 118, 70, 100, IDC_PARITY_COMBO));

    /* Bulk Actions - middle column of the header, between Connection &
     * Settings (left) and Amplifier Temperature (right). Always
     * expanded - the combo, Set, ON/OFF, Clear/Select All, level
     * buttons, and every card's selection checkbox are all visible
     * from launch; nothing here hides. Click a card's checkbox to
     * select it (lit accent border), then one of these applies to
     * every selected channel at once - IDC_BULK_TOGGLE_BTN just
     * arms/disarms an extra convenience (see its own comment below),
     * it doesn't reveal anything. Same safety gating as each card's
     * own controls: OFF always works even kill-switch-tripped, ON/Set/
     * level skip a tripped channel. The action controls are also
     * disabled alongside every per-channel control until RS422
     * connects - see set_channel_controls_enabled(). Two rows, same
     * row-pitch as Connection & Settings' own rows (y=36/63). */
    {
        HWND bulk_mode_combo;
        HWND row_select_combo;
        int mi;
        int ri;
        static const char *const row_select_items[] = {
            "1st Row", "2nd Row", "3rd Row", "4th Row", "Select All", "Custom"
        };

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
         * name/selection-checkbox there -> title/selected-count/arm-
         * toggle button here), combo + a button on row 2 (mode combo +
         * Set, same on both), a primary on/off row on row 3, a status-
         * line row at the bottom-left on row 4 (STANDBY there -> Clear
         * here), and a right-side vertical column spanning rows 2-4 (the
         * level gauge + High/Medium/Low/Off tick labels there -> the
         * same 4 levels as actual buttons here, since bulk applies a
         * level with a click rather than a drag). */
        add_header_icon(hwnd, 470 + BULK_X_SHIFT, 24, ICON_LIST);
        add_header(hwnd, "Bulk Actions", 488 + BULK_X_SHIFT, 24, 150, 18);
        add_ctrl(hwnd, "STATIC", "0 selected", SS_LEFT | SS_NOPREFIX,
                 648 + BULK_X_SHIFT, 26, 84, 16, IDC_BULK_SELECTED_LBL);
        /* Fixed in the row-1 corner slot, matching a Unit card's
         * checkbox position - arms/disarms clicking a card's plain
         * background to toggle its selection (the checkbox itself
         * always works either way). Off by default so a stray click
         * on a card doesn't silently select it. */
        add_ctrl(hwnd, "BUTTON", "Card Click: Off", BS_OWNERDRAW | WS_TABSTOP,
                 740 + BULK_X_SHIFT, 22, 138, 22, IDC_BULK_TOGGLE_BTN);

        bulk_mode_combo = add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP,
                                    470 + BULK_X_SHIFT, 54, 230, 140, IDC_BULK_MODE_COMBO);
        for (mi = 0; mi < PROTO_MODE_COUNT; mi++) {
            const char *name = proto_mode_name((uint8_t)mi);
            SendMessageA(bulk_mode_combo, CB_ADDSTRING, 0, (LPARAM)(name ? name : "?"));
        }
        SendMessageA(bulk_mode_combo, CB_SETCURSEL, PROTO_MODE_WHITE_NOISE, 0);
        make_combo_readonly(bulk_mode_combo);
        add_ctrl(hwnd, "BUTTON", "Set", BS_OWNERDRAW | WS_TABSTOP,
                 710 + BULK_X_SHIFT, 54, 60, 20, IDC_BULK_SET_BTN);

        add_ctrl(hwnd, "BUTTON", "ON", BS_OWNERDRAW | WS_TABSTOP,
                 470 + BULK_X_SHIFT, 84, 110, 22, IDC_BULK_ON_BTN);
        add_ctrl(hwnd, "BUTTON", "OFF", BS_OWNERDRAW | WS_TABSTOP,
                 590 + BULK_X_SHIFT, 84, 110, 22, IDC_BULK_OFF_BTN);

        add_ctrl(hwnd, "BUTTON", "Clear", BS_OWNERDRAW | WS_TABSTOP,
                 470 + BULK_X_SHIFT, 116, 90, 20, IDC_BULK_CLEAR_BTN);
        /* Select All's real value is the opposite case: select all,
         * then uncheck the few you want left out, instead of clicking
         * 12+ individual checkboxes by hand. */
        add_ctrl(hwnd, "BUTTON", "Select All", BS_OWNERDRAW | WS_TABSTOP,
                 568 + BULK_X_SHIFT, 116, 90, 20, IDC_BULK_SELECT_ALL_BTN);

        /* Quick-select presets - see bulk_select_row() and
         * IDC_BULK_ROWSELECT_COMBO's comment in resource.h. Applies
         * immediately on CBN_SELCHANGE, same as the Spectrum unit
         * combo does - no separate Set step needed for a pick this
         * simple. Starts on "Custom" (last item) so it doesn't fire a
         * selection change the instant the window opens. */
        row_select_combo = add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP,
                                     470 + BULK_X_SHIFT, 140, 188, 120, IDC_BULK_ROWSELECT_COMBO);
        for (ri = 0; ri < (int)(sizeof(row_select_items) / sizeof(row_select_items[0])); ri++) {
            SendMessageA(row_select_combo, CB_ADDSTRING, 0, (LPARAM)row_select_items[ri]);
        }
        SendMessageA(row_select_combo, CB_SETCURSEL,
                     (WPARAM)(sizeof(row_select_items) / sizeof(row_select_items[0]) - 1), 0);
        make_combo_readonly(row_select_combo);

        add_ctrl(hwnd, "BUTTON", "High", BS_OWNERDRAW | WS_TABSTOP,
                 790 + BULK_X_SHIFT, 54, 84, 18, IDC_BULK_HIGH_BTN);
        add_ctrl(hwnd, "BUTTON", "Mid", BS_OWNERDRAW | WS_TABSTOP,
                 790 + BULK_X_SHIFT, 76, 84, 18, IDC_BULK_MEDIUM_BTN);
        add_ctrl(hwnd, "BUTTON", "Low", BS_OWNERDRAW | WS_TABSTOP,
                 790 + BULK_X_SHIFT, 98, 84, 18, IDC_BULK_LOW_BTN);
        add_ctrl(hwnd, "BUTTON", "Off", BS_OWNERDRAW | WS_TABSTOP,
                 790 + BULK_X_SHIFT, 120, 84, 18, IDC_BULK_LEVEL_OFF_BTN);
    }

    /* Ambient Temperature's commands - narrower, taller: each row split
     * onto its own line (same "Port on its own row, Refresh/Connect
     * below it" shape Connection & Settings already uses) instead of
     * Port+Refresh+Connect sharing one wide row - direct request to
     * take up less width and more height, freeing more of the header's
     * resize gap for the heatmap next to it. */
    add_header_icon(hwnd, 1033 + AMBIENT_X_SHIFT, 14, ICON_WAVE);
    add_header(hwnd, "Ambient Temperature", 1051 + AMBIENT_X_SHIFT, 14, 260, 18);
    add_ctrl(hwnd, "STATIC", "Port:", SS_LEFT, 1025 + AMBIENT_X_SHIFT, 36, 32, 16, 0);
    make_combo_readonly(add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 1059 + AMBIENT_X_SHIFT, 34, 90, 140, IDC_SENSOR_PORT_COMBO));
    add_ctrl(hwnd, "BUTTON", "Refresh", BS_OWNERDRAW | WS_TABSTOP, 1025 + AMBIENT_X_SHIFT, 58, 64, 18, IDC_SENSOR_REFRESH_BTN);
    add_ctrl(hwnd, "BUTTON", "Connect", BS_OWNERDRAW | WS_TABSTOP, 1097 + AMBIENT_X_SHIFT, 58, 72, 18, IDC_SENSOR_CONNECT_BTN);
    add_ctrl(hwnd, "STATIC", "Disconnected", SS_LEFT, 1025 + AMBIENT_X_SHIFT, 82, 130, 16, IDC_SENSOR_STATUS_LBL);
    /* Opens the Highest Temp Log popup (on_highest_temp_log_clicked())
     * below the rest of this panel's own controls - direct request,
     * "list highest temp logs below [Ambient Temperature]". Comfortable
     * room for it here: this panel's lowest other control (the status
     * label just above) ends at y=98, and the header panel itself
     * doesn't end until y=186 (HEADER_H=180, panel starts at y=6). */
    add_ctrl(hwnd, "BUTTON", "Highest Temps", BS_OWNERDRAW | WS_TABSTOP,
             1025 + AMBIENT_X_SHIFT, 110, 140, 24, IDC_HIGHEST_TEMP_LOG_BTN);
    /* Kill Switch status/trip/reset moved out into each card's own
     * status line. The Avg pill and the Command Panel (Close All/Open
     * All/Open Log/Icon/Reset to Default) both moved out too - the
     * Avg pill is now the large AVG TEMP block, Commands is now its
     * own row, both on the Summary card (see IDC_SENSOR_TEMP_LBL
     * below, mode_x's comment, and the Summary card section further
     * down). */

    /* The heatmap fills the gap between Ambient Temperature (left) and
     * Bulk Actions (right), so it reads as centered in the header row -
     * direct request for this reorder (was: Connection & Settings,
     * Bulk Actions, Ambient Temperature, heatmap off to the right of
     * everything). Both HEATMAP_LEFT and HEATMAP_RIGHT below are fixed,
     * not resize-aware, matching Bulk Actions' own now-fixed position
     * (see BULK_X_SHIFT's comment) - left edge is Ambient Temperature's
     * widest control (Connect button, 1097+AMBIENT_X_SHIFT+72=741) plus
     * the same 40px gap used elsewhere in this header; right edge is
     * Bulk Actions' leftmost control (470+BULK_X_SHIFT=1472) minus that
     * same 40px gap. */
    g_sensor_heatmap = add_sensor_heatmap(hwnd, HEATMAP_LEFT, 14, HEATMAP_RIGHT - HEATMAP_LEFT, 150);

    /* Sidebar: one tall box - Spectrum up top (the space that used to
     * just be "reserved for other features"), Activity Log below that
     * in the SAME box, not a separate panel. Connection & Settings and
     * Amplifier Temperature moved up into the header above. */
    g_sidebar_panel = add_panel(hwnd, SIDEBAR_X, SIDEBAR_CONTENT_TOP, SIDEBAR_W, LOG_PANEL_Y + LOG_PANEL_H - SIDEBAR_CONTENT_TOP);

    add_header_icon(hwnd, 22, SIDEBAR_CONTENT_TOP + 10, ICON_WAVE);
    add_header(hwnd, "Spectrum", 40, SIDEBAR_CONTENT_TOP + 10, 188, 18);
    make_combo_readonly(add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP,
                                  236, SIDEBAR_CONTENT_TOP + 8, 56, 140, IDC_SPECTRUM_UNIT_COMBO));
    add_ctrl(hwnd, "BUTTON", "All", BS_OWNERDRAW | WS_TABSTOP,
             SIDEBAR_X + SIDEBAR_W - 12 - 60, SIDEBAR_CONTENT_TOP + 8, 60, 20, IDC_SPECTRUM_ALL_BTN);
    add_spectrum_plot(hwnd, 22, SIDEBAR_CONTENT_TOP + 34, SIDEBAR_W + SIDEBAR_X - 34, LOG_PANEL_Y - 12 - (SIDEBAR_CONTENT_TOP + 34),
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

    g_log_header_icon = add_header_icon(hwnd, 22, LOG_PANEL_Y + 10, ICON_LIST);
    g_log_header_lbl = add_header(hwnd, "Activity Log", 40, LOG_PANEL_Y + 10, 200, 18);
    /* Right edge of both the Clear button and the listbox is pinned to
     * the same margin (12px in from the panel's own right edge,
     * matching the 12px left margin: content starts at x=22, panel at
     * SIDEBAR_X=10) - they used to use different margins, leaving the
     * listbox 10px short of the button above it. */
    add_ctrl(hwnd, "BUTTON", "Clear", BS_OWNERDRAW | WS_TABSTOP,
             SIDEBAR_X + SIDEBAR_W - 12 - 60, LOG_PANEL_Y + 8, 60, 20, IDC_LOG_CLEAR_BTN);
    /* Opens the whole log in its own bigger window - see
     * on_log_view_clicked(). Left of Clear, same 8px gap pattern as
     * everywhere else two buttons sit side by side on this panel. */
    add_ctrl(hwnd, "BUTTON", "View Full", BS_OWNERDRAW | WS_TABSTOP,
             SIDEBAR_X + SIDEBAR_W - 12 - 60 - 8 - 76, LOG_PANEL_Y + 8, 76, 20, IDC_LOG_VIEW_BTN);
    add_ctrl(hwnd, "LISTBOX", NULL, LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP | WS_BORDER,
             22, LOG_PANEL_Y + 34, SIDEBAR_W + SIDEBAR_X - 34, LOG_PANEL_H - 46, IDC_LOG_LISTBOX);
    /* WS_HSCROLL alone does nothing on a listbox until it's told how far
     * there is to scroll - a long line (like a warning with a Win32
     * error message appended, see ui_show_warning_with_last_error())
     * just gets silently clipped at the control's own width otherwise,
     * with no way to see the rest of it. 900px is generous headroom
     * well past anything this app actually logs. */
    SendDlgItemMessageA(hwnd, IDC_LOG_LISTBOX, LB_SETHORIZONTALEXTENT, 900, 0);

    for (idx = 0; idx < MAX_CHANNELS; idx++) {
        add_channel_card(hwnd, idx);
    }

    /* Summary card - placeholder only for now (direct request: "create
     * a large card below [the channel cards] that i plan to make it
     * like a summary" - content TBD, this just reserves the space and
     * gives it a header so the eventual real content has a home).
     * Lives below the 4x4 grid in the main content area, same width as
     * the grid itself (GRID_LEFT to the header panel's own right
     * margin) - NOT the narrow sidebar column (see GRID_BOTTOM's own
     * comment for why there's room here at all). */
    g_summary_panel = add_panel(hwnd, GRID_LEFT, SUMMARY_PANEL_Y, CLIENT_WIDTH - SIDEBAR_X - GRID_LEFT, SUMMARY_PANEL_H);
    g_summary_header_icon = add_header_icon(hwnd, GRID_LEFT + 12, SUMMARY_PANEL_Y + 10, ICON_WAVE);
    g_summary_header_lbl = add_header(hwnd, "Summary", GRID_LEFT + 30, SUMMARY_PANEL_Y + 10, 188, 18);
    /* Command Panel, relocated onto the Summary card (direct request) -
     * a 2x2 grid (Close All/Open All on row 1, Open Log/Icon on row 2 -
     * direct correction, replacing an earlier single-row version) with
     * Reset to Default (rack-wide, every channel back to its factory
     * default - see on_reset_to_default_clicked()) spanning both
     * columns below it. "Commands" is the leftmost column of the same
     * content row as AVG TEMP/Highest Temp Today/Mode below (direct
     * correction - it wasn't lining up with them before), so AVG TEMP
     * now starts after COMMANDS_COL_W + COMMANDS_COL_GAP instead of
     * right at GRID_LEFT + 12. */
    g_quick_panel_label = add_ctrl(hwnd, "STATIC", "Commands", SS_CENTER | SS_NOPREFIX,
                                    GRID_LEFT + 12, SUMMARY_PANEL_Y + SUMMARY_CONTENT_Y, COMMANDS_COL_W, 16, 0);
    add_ctrl(hwnd, "BUTTON", "Emergency Shutdown", BS_OWNERDRAW | WS_TABSTOP,
             GRID_LEFT + 12, SUMMARY_PANEL_Y + SUMMARY_CONTENT_BLOCK_Y + SUMMARY_CMD_ROW1_Y, SUMMARY_CMD_BTN_W, SUMMARY_CMD_ROW_H, IDC_CLOSE_ALL_BTN);
    add_ctrl(hwnd, "BUTTON", "Global Activate", BS_OWNERDRAW | WS_TABSTOP,
             GRID_LEFT + SUMMARY_CMD_COL2_X, SUMMARY_PANEL_Y + SUMMARY_CONTENT_BLOCK_Y + SUMMARY_CMD_ROW1_Y, SUMMARY_CMD_BTN_W, SUMMARY_CMD_ROW_H, IDC_OPEN_ALL_BTN);
    add_ctrl(hwnd, "BUTTON", "Open Csv Logs", BS_OWNERDRAW | WS_TABSTOP,
             GRID_LEFT + 12, SUMMARY_PANEL_Y + SUMMARY_CONTENT_BLOCK_Y + SUMMARY_CMD_ROW2_Y, SUMMARY_CMD_BTN_W, SUMMARY_CMD_ROW_H, IDC_OPEN_LOG_BTN);
    add_ctrl(hwnd, "BUTTON", "Icon", BS_OWNERDRAW | WS_TABSTOP,
             GRID_LEFT + SUMMARY_CMD_COL2_X, SUMMARY_PANEL_Y + SUMMARY_CONTENT_BLOCK_Y + SUMMARY_CMD_ROW2_Y, SUMMARY_CMD_BTN_W, SUMMARY_CMD_ROW_H, IDC_CMD_CHANGE_ICON_BTN);
    add_ctrl(hwnd, "BUTTON", "Reset to Default", BS_OWNERDRAW | WS_TABSTOP,
             GRID_LEFT + 12, SUMMARY_PANEL_Y + SUMMARY_CONTENT_BLOCK_Y + SUMMARY_CMD_RESET_Y, COMMANDS_COL_W, SUMMARY_CMD_ROW_H, IDC_RESET_TO_DEFAULT_BTN);
    /* AVG TEMP/Mode/Highest Temp Today's shared content row - Commands
     * (above) is now this same row's leftmost column, so AVG TEMP
     * starts after it instead of at GRID_LEFT + 12 directly. Same
     * caption-row-above-a-MODE_ICON_SIZE-square-content treatment on
     * both, direct request that they "sync well". */
    g_avg_temp_caption_lbl = add_ctrl(hwnd, "STATIC", "AVG TEMP", SS_CENTER | SS_NOPREFIX,
                                       GRID_LEFT + 12 + COMMANDS_COL_W + COMMANDS_COL_GAP, SUMMARY_PANEL_Y + SUMMARY_CONTENT_Y, AVG_TEMP_BLOCK_W, 16, IDC_AVG_TEMP_CAPTION_LBL);
    g_avg_temp_block = add_pill(hwnd, "", GRID_LEFT + 12 + COMMANDS_COL_W + COMMANDS_COL_GAP, SUMMARY_PANEL_Y + SUMMARY_CONTENT_BLOCK_Y,
                                 AVG_TEMP_BLOCK_W, MODE_ICON_SIZE, IDC_SENSOR_TEMP_LBL, (WNDPROC)avg_temp_block_subclass_proc);
    g_mode_caption_lbl = add_ctrl(hwnd, "STATIC", "Mode", SS_CENTER | SS_NOPREFIX,
                                   CLIENT_WIDTH - SIDEBAR_X - MODE_ICON_MARGIN - MODE_ICON_SIZE, SUMMARY_PANEL_Y + SUMMARY_CONTENT_Y,
                                   MODE_ICON_SIZE, 16, IDC_MODE_CAPTION_LBL);
    g_mode_toggle_btn = add_ctrl(hwnd, "BUTTON", g_light_mode ? "Dark Mode" : "Light Mode", BS_OWNERDRAW | WS_TABSTOP,
                                  CLIENT_WIDTH - SIDEBAR_X - MODE_ICON_MARGIN - MODE_ICON_SIZE, SUMMARY_PANEL_Y + SUMMARY_CONTENT_BLOCK_Y,
                                  MODE_ICON_SIZE, MODE_ICON_SIZE, IDC_THEME_TOGGLE_BTN);
    /* 3rd content block, centered between the two above - not on the
     * summary card's full width, which would let a wider AVG TEMP or
     * Highest Temp block collide into the other (direct report - see
     * relayout_for_size()'s matching highest_x comment for the full
     * reasoning, including why this is now clamped rather than a bare
     * centering formula). Same WM_CREATE-time CLIENT_WIDTH guess as
     * everything else in this card; relayout_for_size() recomputes
     * against the real client_w once the window's actual size is known. */
    {
        int wm_create_mode_x = CLIENT_WIDTH - SIDEBAR_X - MODE_ICON_MARGIN - MODE_ICON_SIZE;
        int wm_create_avg_right = GRID_LEFT + 12 + COMMANDS_COL_W + COMMANDS_COL_GAP + AVG_TEMP_BLOCK_W;
        int wm_create_highest_x = (wm_create_avg_right + wm_create_mode_x) / 2 - HIGHEST_TEMP_BLOCK_W / 2;
        if (wm_create_highest_x + HIGHEST_TEMP_BLOCK_W > wm_create_mode_x) {
            wm_create_highest_x = wm_create_mode_x - HIGHEST_TEMP_BLOCK_W;
        }
        g_highest_temp_caption_lbl = add_ctrl(hwnd, "STATIC", "Highest Temp Today", SS_CENTER | SS_NOPREFIX,
                                               wm_create_highest_x,
                                               SUMMARY_PANEL_Y + SUMMARY_CONTENT_Y, HIGHEST_TEMP_BLOCK_W, 16, IDC_HIGHEST_TEMP_CAPTION_LBL);
        g_highest_temp_block = add_pill(hwnd, "",
                                         wm_create_highest_x,
                                         SUMMARY_PANEL_Y + SUMMARY_CONTENT_BLOCK_Y, HIGHEST_TEMP_BLOCK_W, MODE_ICON_SIZE,
                                         IDC_HIGHEST_TEMP_BLOCK, (WNDPROC)highest_temp_block_subclass_proc);
    }
    if (g_quick_panel_label && g_header_font) {
        SendMessageA(g_quick_panel_label, WM_SETFONT, (WPARAM)g_header_font, (LPARAM)TRUE);
    }
    if (g_avg_temp_caption_lbl && g_header_font) {
        SendMessageA(g_avg_temp_caption_lbl, WM_SETFONT, (WPARAM)g_header_font, (LPARAM)TRUE);
    }
    if (g_mode_caption_lbl && g_header_font) {
        SendMessageA(g_mode_caption_lbl, WM_SETFONT, (WPARAM)g_header_font, (LPARAM)TRUE);
    }
    if (g_highest_temp_caption_lbl && g_header_font) {
        SendMessageA(g_highest_temp_caption_lbl, WM_SETFONT, (WPARAM)g_header_font, (LPARAM)TRUE);
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
 * card_w and card_h both vary now - see channel_card_width()/
 * channel_card_height() - which is exactly what sx/sy above exist for.
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

    PLACE(GetDlgItem(hwnd, channel_mode_id(index)), x + SX(8), y + SY(24), SX(98), 100);
    /* The mode combo's readonly-theming overlays (arrow + 4 border
     * strips, see make_combo_readonly_ex) are separate sibling windows,
     * not children of the combo, so moving the combo above does NOT
     * move them - left in place, they'd sit at the combo's OLD rect
     * while the real (undecorated) combo shows through at the new one.
     * Re-derive their rect from the combo's own post-move GetWindowRect
     * rather than re-deriving via SX/SY here too, since MoveWindow's
     * closed-box height isn't simply SY(18) - it's whatever the font
     * actually renders, same as make_combo_readonly_ex's own math. */
    {
        HWND combo = GetDlgItem(hwnd, channel_mode_id(index));
        RECT crc;
        int cw, ch2, arrow_w;
        GetWindowRect(combo, &crc);
        MapWindowPoints(HWND_DESKTOP, hwnd, (POINT *)&crc, 2);
        cw = crc.right - crc.left;
        ch2 = crc.bottom - crc.top;
        arrow_w = GetSystemMetrics(SM_CXVSCROLL) + ARROW_OVERLAY_PAD_PX;
        if (g_card_combo_overlays[index][0]) {
            MoveWindow(g_card_combo_overlays[index][0], crc.right - arrow_w, crc.top, arrow_w, ch2, FALSE);
        }
        if (g_card_combo_overlays[index][1]) {
            MoveWindow(g_card_combo_overlays[index][1], crc.left, crc.top, cw, COMBO_BORDER_PX, FALSE);
        }
        if (g_card_combo_overlays[index][2]) {
            MoveWindow(g_card_combo_overlays[index][2], crc.left, crc.bottom - COMBO_BORDER_PX, cw, COMBO_BORDER_PX, FALSE);
        }
        if (g_card_combo_overlays[index][3]) {
            MoveWindow(g_card_combo_overlays[index][3], crc.left, crc.top, COMBO_BORDER_PX, ch2, FALSE);
        }
        if (g_card_combo_overlays[index][4]) {
            MoveWindow(g_card_combo_overlays[index][4], crc.right - COMBO_BORDER_PX, crc.top, COMBO_BORDER_PX, ch2, FALSE);
        }
    }
    PLACE(GetDlgItem(hwnd, channel_set_id(index)), x + SX(110), y + SY(24), SX(32), SY(21));
    PLACE(GetDlgItem(hwnd, channel_on_id(index)), x + SX(8), y + SY(47), SX(60), SY(21));
    PLACE(GetDlgItem(hwnd, channel_off_id(index)), x + SX(72), y + SY(47), SX(60), SY(21));
    PLACE(GetDlgItem(hwnd, channel_status_id(index)), x + SX(8), y + SY(70), SX(130), SY(14));
    PLACE(GetDlgItem(hwnd, channel_uptime_id(index)), x + SX(8), y + SY(86), SX(130), SY(12));

    PLACE(GetDlgItem(hwnd, channel_freq_lbl_id(index)), x + SX(88), y + SY(8), SX(110), SY(14));
    PLACE(GetDlgItem(hwnd, channel_track_id(index)), x + SX(148), y + SY(24), SX(40), SY(72));
    PLACE(GetDlgItem(hwnd, channel_lbl_high_id(index)), x + SX(194), y + SY(24), SX(44), SY(14));
    PLACE(GetDlgItem(hwnd, channel_lbl_medium_id(index)), x + SX(194), y + SY(42), SX(44), SY(14));
    PLACE(GetDlgItem(hwnd, channel_lbl_low_id(index)), x + SX(194), y + SY(60), SX(44), SY(14));
    PLACE(GetDlgItem(hwnd, channel_lbl_off_id(index)), x + SX(194), y + SY(78), SX(44), SY(14));

#undef SX
#undef SY
#undef PLACE
}

/* How tall a channel card should be to make the 4-row grid's bottom
 * edge land GRID_BOTTOM_MARGIN above the client area's bottom, for a
 * given client height - clamped to [CARD_H, CARD_H_MAX] (see that
 * constant's comment for why growth is capped). Reserves the Summary
 * card's own minimum footprint (MIN_SUMMARY_H + a CARD_GAP) off the top
 * of the available space first, so on a short window (1920x1080 vs. the
 * 1920x1200 this was tuned against) the grid shrinks to make room
 * instead of the Summary card overflowing past the window's bottom
 * edge - see MIN_SUMMARY_H's own comment. */
static int channel_card_height(int client_h) {
    int avail = client_h - CONTENT_TOP - GRID_BOTTOM_MARGIN - (GRID_ROWS - 1) * CARD_GAP
                - MIN_SUMMARY_H - CARD_GAP;
    int h = avail / GRID_ROWS;
    if (h < CARD_H) {
        h = CARD_H;
    } else if (h > CARD_H_MAX) {
        h = CARD_H_MAX;
    }
    return h;
}

/* How wide a channel card should be for a given client width - derived
 * to exactly fill the grid from grid_left (already grown along with the
 * sidebar, see sidebar_width_for()/grid_left_for()) out to the same
 * right margin (SIDEBAR_X) the header panel itself stretches to,
 * instead of leaving a dead strip right of the grid (see CARD_W's own
 * comment - direct report: cards weren't actually reaching the window's
 * right edge at 1920 width, off by roughly a card-gap's worth of slop
 * from an earlier version of this function that grew card_w by a fixed
 * percentage of the extra width rather than solving for "fills the
 * space" directly). Clamped to [CARD_W, CARD_W_MAX].
 * position_channel_card() already scales every control inside a card
 * proportionally to whatever width it's given. */
static int channel_card_width(int client_w, int grid_left) {
    int available = client_w - SIDEBAR_X - grid_left - (GRID_COLS - 1) * CARD_GAP;
    int w = available / GRID_COLS;
    if (w < CARD_W) {
        w = CARD_W;
    }
    if (w > CARD_W_MAX) {
        w = CARD_W_MAX;
    }
    return w;
}

/* How wide the sidebar (Spectrum + Activity Log) should be for a given
 * client width - the other 25% of any extra width beyond CLIENT_WIDTH,
 * see channel_card_width(). Clamped to [SIDEBAR_W, SIDEBAR_W_MAX]. */
static int sidebar_width_for(int client_w) {
    int extra = client_w - CLIENT_WIDTH;
    int w = SIDEBAR_W;
    if (extra > 0) {
        w += extra * 25 / 100;
    }
    if (w > SIDEBAR_W_MAX) {
        w = SIDEBAR_W_MAX;
    }
    return w;
}

/* Left edge of the channel grid for a given (already computed) sidebar
 * width - the same 10px gap off the sidebar's right edge GRID_LEFT's
 * own comment describes, just following sidebar_width_for() instead of
 * the fixed SIDEBAR_W when the sidebar has grown. */
static int grid_left_for(int sidebar_w) {
    return SIDEBAR_X + sidebar_w + 10;
}

/* Sidebar's (Spectrum + Activity Log) bottom edge, straight off the
 * window's own client height - not the channel grid's, unlike this
 * function's own former version. The grid's height is clamped by
 * CARD_H_MAX, which used to cap the sidebar's growth at the same point,
 * leaving a large dead strip below Spectrum/Activity Log on a much
 * taller window (direct report at the real 1920x1200 ThinkPad size).
 * GRID_BOTTOM_MARGIN doubles as this margin too - same visual weight,
 * and it's exactly what the grid's own bottom edge lands on whenever
 * the grid ISN'T clamped, so the two stay aligned in the common case
 * and only diverge (on purpose) once the grid maxes out. */
static int log_panel_y_for(int client_h) {
    return client_h - GRID_BOTTOM_MARGIN - LOG_PANEL_H;
}

/* Recomputes the whole layout for a new client size: the header bar
 * stretches horizontally to fill the wider client area, the 16 cards
 * grow both taller (channel_card_height()) and wider
 * (channel_card_width()) to use up extra space instead of leaving it
 * empty below row 4 or as a dead strip right of the grid, and the
 * sidebar (Spectrum + Activity Log) grows too (sidebar_width_for()),
 * its bottom edge now tracking the window's own client height via
 * log_panel_y_for() - not the grid's, see that function's own comment.
 * Never shrinks below the designed CARD_W x CARD_H (see WM_GETMINMAXINFO,
 * which stops the window itself getting that small). */
static void relayout_for_size(HWND hwnd, int client_w, int client_h) {
    int i;
    int card_h = channel_card_height(client_h);
    int sidebar_w = sidebar_width_for(client_w);
    int grid_left = grid_left_for(sidebar_w);
    int card_w = channel_card_width(client_w, grid_left);
    int log_y = log_panel_y_for(client_h);

    if (!g_layout_ready) {
        return;
    }

    MoveWindow(g_header_panel, SIDEBAR_X, 6, client_w - 2 * SIDEBAR_X, HEADER_H, FALSE);
    /* Fixed between Ambient Temperature and Bulk Actions, same as its
     * own creation in build_controls() - see HEATMAP_LEFT/HEATMAP_RIGHT
     * there. Not resize-aware, same as Bulk Actions itself now isn't
     * (see BULK_X_SHIFT's comment) - this only ever moves because the
     * header panel it's sitting in also moves position (SIDEBAR_X, 6
     * never actually change), not because it resizes with the window. */
    MoveWindow(g_sensor_heatmap, HEATMAP_LEFT, 14, HEATMAP_RIGHT - HEATMAP_LEFT, 150, FALSE);
    MoveWindow(g_sidebar_panel, SIDEBAR_X, SIDEBAR_CONTENT_TOP, sidebar_w, log_y + LOG_PANEL_H - SIDEBAR_CONTENT_TOP, FALSE);

    /* Summary card's real position - below the grid's actual (dynamic,
     * CARD_H_MAX-clamped) bottom edge, not the CARD_H-based WM_CREATE-
     * time guess build_controls() used (see GRID_BOTTOM's comment).
     * Floored at 60px tall so a smaller-than-baseline window never
     * hands MoveWindow a negative/zero height. */
    {
        int grid_bottom = GRID_TOP + GRID_ROWS * card_h + (GRID_ROWS - 1) * CARD_GAP;
        int summary_y = grid_bottom + CARD_GAP;
        int summary_h = client_h - GRID_BOTTOM_MARGIN - summary_y;
        int summary_w = client_w - SIDEBAR_X - grid_left;
        int mode_x = client_w - SIDEBAR_X - MODE_ICON_MARGIN - MODE_ICON_SIZE;
        /* Centered between AVG TEMP's right edge and Mode's left edge,
         * not on the summary card's full width - direct request to
         * grow both AVG TEMP and Highest Temp Today's fonts (and their
         * blocks, to still fit the bigger text) meant centering on the
         * whole row would have overlapped the two blocks together,
         * since a wider AVG TEMP block pushes its own right edge deeper
         * into where a "centered on everything" Highest Temp block
         * would start. This formula keeps equal breathing room on both
         * sides of Highest Temp regardless of how wide either
         * neighbor's block is. */
        int avg_x = grid_left + 12 + COMMANDS_COL_W + COMMANDS_COL_GAP;
        int avg_right = avg_x + AVG_TEMP_BLOCK_W;
        int highest_x = (avg_right + mode_x) / 2 - HIGHEST_TEMP_BLOCK_W / 2;
        /* Direct report, actually root-caused this time: Highest Temp
         * Today is created AFTER the Mode icon (see build_controls()),
         * so it paints on top of it in z-order - widening Commands
         * shrank the avg_right..mode_x gap this block centers in by the
         * same 104px, and once that gap got smaller than
         * HIGHEST_TEMP_BLOCK_W, this block started overlapping and
         * painting over the icon (icon only looked right right after
         * a click forced its own redraw, until the next periodic
         * repaint covered it again - never a crescent-shape bug at
         * all). Hard-clamp so this block can never cross into Mode's
         * own space, regardless of how tight the gap gets. */
        if (highest_x + HIGHEST_TEMP_BLOCK_W > mode_x) {
            highest_x = mode_x - HIGHEST_TEMP_BLOCK_W;
        }
        if (summary_h < MIN_SUMMARY_H) {
            summary_h = MIN_SUMMARY_H;
        }
        MoveWindow(g_summary_panel, grid_left, summary_y, summary_w, summary_h, FALSE);
        MoveWindow(g_summary_header_icon, grid_left + 12, summary_y + 10, 14, 14, FALSE);
        MoveWindow(g_summary_header_lbl, grid_left + 30, summary_y + 10, 188, 18, FALSE);
        MoveWindow(g_quick_panel_label, grid_left + 12, summary_y + SUMMARY_CONTENT_Y, COMMANDS_COL_W, 16, FALSE);
        MoveWindow(GetDlgItem(hwnd, IDC_CLOSE_ALL_BTN), grid_left + 12, summary_y + SUMMARY_CONTENT_BLOCK_Y + SUMMARY_CMD_ROW1_Y, SUMMARY_CMD_BTN_W, SUMMARY_CMD_ROW_H, FALSE);
        MoveWindow(GetDlgItem(hwnd, IDC_OPEN_ALL_BTN), grid_left + SUMMARY_CMD_COL2_X, summary_y + SUMMARY_CONTENT_BLOCK_Y + SUMMARY_CMD_ROW1_Y, SUMMARY_CMD_BTN_W, SUMMARY_CMD_ROW_H, FALSE);
        MoveWindow(GetDlgItem(hwnd, IDC_OPEN_LOG_BTN), grid_left + 12, summary_y + SUMMARY_CONTENT_BLOCK_Y + SUMMARY_CMD_ROW2_Y, SUMMARY_CMD_BTN_W, SUMMARY_CMD_ROW_H, FALSE);
        MoveWindow(GetDlgItem(hwnd, IDC_CMD_CHANGE_ICON_BTN), grid_left + SUMMARY_CMD_COL2_X, summary_y + SUMMARY_CONTENT_BLOCK_Y + SUMMARY_CMD_ROW2_Y, SUMMARY_CMD_BTN_W, SUMMARY_CMD_ROW_H, FALSE);
        MoveWindow(GetDlgItem(hwnd, IDC_RESET_TO_DEFAULT_BTN), grid_left + 12, summary_y + SUMMARY_CONTENT_BLOCK_Y + SUMMARY_CMD_RESET_Y, COMMANDS_COL_W, SUMMARY_CMD_ROW_H, FALSE);
        MoveWindow(g_avg_temp_caption_lbl, avg_x, summary_y + SUMMARY_CONTENT_Y, AVG_TEMP_BLOCK_W, 16, FALSE);
        MoveWindow(g_avg_temp_block, avg_x, summary_y + SUMMARY_CONTENT_BLOCK_Y, AVG_TEMP_BLOCK_W, MODE_ICON_SIZE, FALSE);
        MoveWindow(g_highest_temp_caption_lbl, highest_x, summary_y + SUMMARY_CONTENT_Y, HIGHEST_TEMP_BLOCK_W, 16, FALSE);
        MoveWindow(g_highest_temp_block, highest_x, summary_y + SUMMARY_CONTENT_BLOCK_Y, HIGHEST_TEMP_BLOCK_W, MODE_ICON_SIZE, FALSE);
        MoveWindow(g_mode_caption_lbl, mode_x, summary_y + SUMMARY_CONTENT_Y, MODE_ICON_SIZE, 16, FALSE);
        MoveWindow(g_mode_toggle_btn, mode_x, summary_y + SUMMARY_CONTENT_BLOCK_Y, MODE_ICON_SIZE, MODE_ICON_SIZE, FALSE);
    }

    MoveWindow(g_log_header_icon, 22, log_y + 10, 14, 14, FALSE);
    MoveWindow(g_log_header_lbl, 40, log_y + 10, 200, 18, FALSE);
    MoveWindow(GetDlgItem(hwnd, IDC_LOG_LISTBOX), 22, log_y + 34, sidebar_w + SIDEBAR_X - 34, LOG_PANEL_H - 46, FALSE);
    MoveWindow(GetDlgItem(hwnd, IDC_LOG_CLEAR_BTN), SIDEBAR_X + sidebar_w - 12 - 60, log_y + 8, 60, 20, FALSE);
    MoveWindow(GetDlgItem(hwnd, IDC_LOG_VIEW_BTN), SIDEBAR_X + sidebar_w - 12 - 60 - 8 - 76, log_y + 8, 76, 20, FALSE);
    MoveWindow(GetDlgItem(hwnd, IDC_SPECTRUM_UNIT_COMBO), 236, SIDEBAR_CONTENT_TOP + 8, 56, 140, FALSE);
    MoveWindow(GetDlgItem(hwnd, IDC_SPECTRUM_ALL_BTN), SIDEBAR_X + sidebar_w - 12 - 60, SIDEBAR_CONTENT_TOP + 8, 60, 20, FALSE);
    MoveWindow(GetDlgItem(hwnd, IDC_SPECTRUM_PLOT), 22, SIDEBAR_CONTENT_TOP + 34,
               sidebar_w + SIDEBAR_X - 34, log_y - 12 - (SIDEBAR_CONTENT_TOP + 34), FALSE);

    for (i = 0; i < MAX_CHANNELS; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        int card_x = grid_left + col * (card_w + CARD_GAP);
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

            /* Bold + wide (FW_BLACK) for the HelixDefender wordmark under
             * the logo mark - the reference's own lettering reads as a
             * heavy geometric sans, not a normal-weight label; letter-
             * spacing is added separately at draw time via
             * SetTextCharacterExtra, not something CreateFontA controls. */
            g_logo_font = CreateFontA(-16, 0, 0, 0, FW_BLACK, FALSE, FALSE, FALSE,
                                       ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            if (!g_logo_font) {
                g_logo_font = g_header_font;
            }

            g_mono_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
            if (!g_mono_font) {
                g_mono_font = g_font;
            }

            g_big_font = CreateFontA(-72, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
            if (!g_big_font) {
                g_big_font = g_mono_font;
            }

            g_huge_font = CreateFontA(-100, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                       ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
            if (!g_huge_font) {
                g_huge_font = g_big_font;
            }

            g_small_font = CreateFontA(-9, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            if (!g_small_font) {
                g_small_font = g_font;
            }

            build_controls(hwnd);
            refresh_port_list();
            refresh_sensor_port_list();
            load_sensor_port();
            load_custom_logo();
            apply_custom_app_icon(hwnd);
            load_branding_icon(hwnd);

            memset(&ccb, 0, sizeof(ccb));
            ccb.on_connected_changed = conn_on_connected_changed;
            ccb.on_frame = conn_on_frame;
            ccb.on_error = conn_on_error;
            conn_init(&g_conn, ccb);
            channels_init(&g_conn);
            /* load_channel_settings() intentionally NOT called - direct
             * decision: every launch starts every channel at its plain
             * channels_init() defaults (White Noise, OFF) regardless of
             * whatever was saved before closing - this is only the
             * split-second in-memory state before RS422 actually
             * connects, though; once it does, conn_on_connected_changed()
             * fires on_reset_to_default_clicked() once (see its own
             * comment), which now powers everything ON instead. Nothing
             * is persisted to
             * the .ini at all anymore now (save_settings() itself is
             * gone too, see its own removal comment) - Close performs
             * the same reset for real instead of writing it to disk. */
            sensor_init(&g_sensor);
            {
                int addr_i;
                for (addr_i = 0; addr_i < SENSOR_MAX_UNITS; addr_i++) {
                    sensor_set_unit_address(&g_sensor, addr_i, SENSOR_UNIT_ADDR_DEFAULT[addr_i]);
                }
            }
            sensor_log_load_state();
            /* Fine if this fails (returns false, g_sensor_shared stays
             * zeroed) - the service may not be installed at all, or
             * hasn't started yet; WM_TIMER retries this periodically. */
            sensor_shared_open_reader(&g_sensor_shared);
            SetTimer(hwnd, ID_POLL_TIMER, 100, NULL);
            /* Starts disconnected - every channel control starts
             * disabled too, same as conn_on_connected_changed() would
             * set once Connect is actually clicked. */
            set_channel_controls_enabled(false);
            ui_refresh_all_channels();
            ui_refresh_sensor();

            /* Auto-connect on launch (direct request - the app "needs to
             * connect to retrieve data" on open, not wait for a manual
             * Connect click) is deferred to WM_APP_AUTOCONNECT, not run
             * here directly - see that constant's own comment in
             * resource.h for why (WM_CREATE fires before the window is
             * actually shown, so a loading dialog here would float over
             * an empty desktop with the main window not drawn yet). */
            PostMessageA(hwnd, WM_APP_AUTOCONNECT, 0, 0);
            return 0;
        }

        case WM_APP_AUTOCONNECT: {
            /* Runs once the message loop actually reaches this posted
             * message - after WinMain's ShowWindow()/UpdateWindow(), so
             * the main window is already fully drawn behind the
             * "Connecting..." dialog. Deliberately after the
             * disabled-state refreshes WM_CREATE already did (those
             * unconditionally paint the disconnected look, so an
             * auto-connect success here needs to come after them, not
             * before, or it'd be immediately overwritten). conn_connect()/
             * sensor_connect() themselves already drive
             * conn_on_connected_changed()/ui_refresh_sensor() again on
             * success, correctly flipping the UI to connected. RS422
             * reuses on_connect_clicked() as-is (no port-required gate
             * to worry about - see its own comment); the sensor side
             * uses the startup-safe variant so a fresh install with
             * nothing plugged in yet doesn't block launch on a "select a
             * port" dialog. */
            DWORD loading_t0 = GetTickCount();
            HWND loading_hwnd = loading_dialog_show("Connecting...");
            on_connect_clicked();
            auto_connect_sensor_on_startup();
            loading_dialog_hide(loading_hwnd, loading_t0);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            /* Never let the window shrink below its designed layout size -
             * relayout_for_size() only ever grows cards/gaps to fill extra
             * space, never shrinks them, so a smaller client area would
             * start overlapping them. */
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
                /* Background retry while disconnected - the RS422 dongle
                 * previously only ever got one AutoConnectSDR() attempt,
                 * at launch (WM_APP_AUTOCONNECT). If Windows hadn't
                 * finished enumerating it yet at that exact instant (a
                 * real race - USB enumeration vs. this app's own
                 * startup), that one shot failed and nothing ever tried
                 * again without a manual Connect click. Every 5s (50
                 * ticks @ 100ms) while still disconnected, retry the
                 * same on_connect_clicked() path silently in the
                 * background - covers both the startup race AND
                 * plugging the dongle in after the app's already open.
                 * ui_show_warning()'s failure logging is a plain Activity
                 * Log line (see conn_on_error()), not a blocking dialog,
                 * so this is safe to run unattended. Gated strictly on
                 * !conn_is_connected() - on_connect_clicked() itself
                 * toggles (Connect vs. Disconnect), so calling it while
                 * already connected would disconnect it, the opposite of
                 * what this is for. */
                {
                    static int rs422_retry_counter = 0;
                    if (!conn_is_connected(&g_conn)) {
                        rs422_retry_counter++;
                        if (rs422_retry_counter >= 50) {
                            rs422_retry_counter = 0;
                            on_connect_clicked();
                        }
                    } else {
                        rs422_retry_counter = 0;
                    }
                }
                channels_poll();
                ui_refresh_all_channels();
                /* The background sensor service (once installed) polls
                 * the sensor port itself and publishes readings via
                 * shared memory - prefer that when it's fresh instead
                 * of also opening the same port here (a serial port
                 * can't be opened by two processes at once). Falls back
                 * to this app's own direct poll whenever the service
                 * isn't installed, hasn't started yet, or has gone
                 * stale (crashed/stopped) - sensor_poll() is already a
                 * cheap no-op if this app's own Connect was never
                 * clicked, so this is safe either way. */
                if (g_sensor_shared.mapping == NULL) {
                    static int retry_counter = 0;
                    /* Retried roughly once a second, not every 100ms
                     * tick - OpenFileMappingA failing is cheap, but no
                     * reason to hammer it while no service is installed
                     * at all (the common case for anyone who hasn't
                     * opted into the background logger). */
                    retry_counter++;
                    if (retry_counter >= 10) {
                        retry_counter = 0;
                        sensor_shared_open_reader(&g_sensor_shared);
                    }
                }
                {
                    bool using_service_data = sensor_shared_read_if_fresh(&g_sensor_shared, &g_sensor);
                    if (!using_service_data) {
                        sensor_poll(&g_sensor);
                    }
                    /* The service does its own CSV logging continuously
                     * (GUI open or not) once it owns the data - this
                     * app only logs when IT'S the one actually polling,
                     * or two processes would race truncating/appending
                     * the same file every week. */
                    if (!using_service_data) {
                        sensor_log_tick(&g_sensor);
                    }
                }
                ui_refresh_sensor();
                check_kill_switch();

                /* Per-channel uptime accounting - every tick (cheap: just
                 * comparing output_on to last tick's value), so the
                 * OFF->ON/ON->OFF edge is never missed regardless of how
                 * often the display/persist steps below run. */
                {
                    int ci;
                    for (ci = 0; ci < MAX_CHANNELS; ci++) {
                        bool on = channels_get(ci)->output_on;
                        if (on && !g_channel_on_prev[ci]) {
                            g_channel_on_since_ms[ci] = GetTickCount64();
                        } else if (!on && g_channel_on_prev[ci]) {
                            g_channel_uptime_base_seconds[ci] += (GetTickCount64() - g_channel_on_since_ms[ci]) / 1000;
                        }
                        g_channel_on_prev[ci] = on;
                    }
                }

                /* ID_POLL_TIMER fires every 100ms, so every 10th tick is
                 * ~1s (display update). Used to also persist to the
                 * .ini every 300th tick (~30s) via save_settings() - no
                 * longer, see that removal's own comment; uptime is
                 * in-memory only now, same as every other channel
                 * value, reset to 0 by Close's real hardware reset. */
                g_uptime_tick_counter++;
                if (g_uptime_tick_counter % 10 == 0) {
                    int ci;
                    for (ci = 0; ci < MAX_CHANNELS; ci++) {
                        char uptime_str[20];
                        char label[32];
                        format_uptime(uptime_str, channel_uptime_seconds(ci));
                        wsprintfA(label, "Up %s", uptime_str);
                        SetDlgItemTextA(hwnd, channel_uptime_id(ci), label);
                    }
                }

                InvalidateRect(GetDlgItem(hwnd, IDC_SPECTRUM_PLOT), NULL, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            /* Only ever reaches here for a click on truly empty client
             * area that no child control claims - every real button/
             * combo/gauge/label still gets its own click first and
             * never falls through to this (the card panel returns
             * HTTRANSPARENT for itself since it has no SS_NOTIFY - see
             * g_bulk_select_mode's comment for the full reasoning on
             * why this coordinate-math approach exists instead of the
             * card panel handling its own background clicks). */
            if (g_bulk_select_mode) {
                int x = (short)LOWORD(lParam);
                int y = (short)HIWORD(lParam);
                /* Cards can be taller AND wider than the designed
                 * CARD_H/CARD_W now (see channel_card_height()/
                 * channel_card_width()) - reuse the same actual size
                 * relayout_for_size() last computed, not the fixed
                 * design constants, or clicks on any row/column past the
                 * first would hit-test against the wrong rect. */
                int card_h = channel_card_height(g_last_client_h);
                int grid_left = grid_left_for(sidebar_width_for(g_last_client_w));
                int card_w = channel_card_width(g_last_client_w, grid_left);
                int idx;
                for (idx = 0; idx < MAX_CHANNELS; idx++) {
                    int col = idx % GRID_COLS;
                    int row = idx / GRID_COLS;
                    int cx = grid_left + col * (card_w + CARD_GAP);
                    int cy = CONTENT_TOP + row * (card_h + CARD_GAP);
                    if (x >= cx && x < cx + card_w && y >= cy && y < cy + card_h) {
                        g_channel_selected[idx] = !g_channel_selected[idx];
                        ui_invalidate_card(idx);
                        ui_refresh_bulk_selected_label();
                        bulk_set_rowselect_combo(GRID_ROWS + 1); /* Custom */
                        break;
                    }
                }
            }
            return 0;
        }

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
            if (id == IDC_CLOSE_ALL_BTN && code == BN_CLICKED) {
                on_close_all_clicked();
                return 0;
            }
            if (id == IDC_OPEN_ALL_BTN && code == BN_CLICKED) {
                on_open_all_clicked();
                return 0;
            }
            if (id == IDC_OPEN_LOG_BTN && code == BN_CLICKED) {
                on_open_log_clicked();
                return 0;
            }
            if (id == IDC_HIGHEST_TEMP_LOG_BTN && code == BN_CLICKED) {
                on_highest_temp_log_clicked();
                return 0;
            }
            if (id == IDC_THEME_TOGGLE_BTN && code == BN_CLICKED) {
                on_theme_toggle_clicked();
                return 0;
            }
            if (id == IDC_LOG_CLEAR_BTN && code == BN_CLICKED) {
                SendDlgItemMessageA(hwnd, IDC_LOG_LISTBOX, LB_RESETCONTENT, 0, 0);
                return 0;
            }
            if (id == IDC_LOG_VIEW_BTN && code == BN_CLICKED) {
                on_log_view_clicked();
                return 0;
            }
            /* Reused as the popup menu's own item IDs below (a menu
             * selection's WM_COMMAND has HIWORD(wParam)==0, same as
             * BN_CLICKED, so these fire the same way a real button
             * click would) - see IDC_CMD_CHANGE_ICON_BTN's handler. */
            if (id == IDC_CHANGE_LOGO_BTN && code == BN_CLICKED) {
                browse_and_set_logo(hwnd);
                return 0;
            }
            if (id == IDC_RESET_LOGO_BTN && code == BN_CLICKED) {
                reset_custom_logo(hwnd);
                return 0;
            }
            if (id == IDC_CMD_CHANGE_ICON_BTN && code == BN_CLICKED) {
                /* Password gate removed (direct request) - Change Logo/
                 * Reset behind this menu used to require unlock_cw()
                 * first (the same admin password Continuous Wave uses),
                 * folded in from the old lock badge. Now opens straight
                 * to the menu, no gate. */
                HMENU menu;
                RECT btn_rc;
                MENUINFO mi;

                menu = CreatePopupMenu();
                /* Owner-drawn (see WM_MEASUREITEM/WM_DRAWITEM's own
                 * ODT_MENU branches below) so this reads as part of the
                 * app's own dark theme instead of a plain stock Windows
                 * menu (direct report) - MF_OWNERDRAW's lpNewItem isn't
                 * the item's text, it's an opaque item-data value
                 * (documented AppendMenuA behavior), so the label
                 * strings are stashed there as raw pointers, read back
                 * via itemData/dwItemData in the measure/draw handlers.
                 * SetMenuInfo's hbrBack recolors the popup's own
                 * background (the part around/between items that isn't
                 * covered by owner-draw at all) to match. */
                mi.cbSize = sizeof(mi);
                mi.fMask = MIM_BACKGROUND;
                mi.hbrBack = g_brush_panel;
                SetMenuInfo(menu, &mi);
                AppendMenuA(menu, MF_OWNERDRAW, IDC_CHANGE_LOGO_BTN, (LPCSTR)"Change Logo");
                AppendMenuA(menu, MF_OWNERDRAW, IDC_RESET_LOGO_BTN, (LPCSTR)"Reset to Default");
                GetWindowRect(GetDlgItem(hwnd, IDC_CMD_CHANGE_ICON_BTN), &btn_rc);
                SetForegroundWindow(hwnd);
                TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, btn_rc.left, btn_rc.bottom, 0, hwnd, NULL);
                PostMessageA(hwnd, WM_NULL, 0, 0); /* MSDN-recommended after TrackPopupMenu */
                DestroyMenu(menu);
                return 0;
            }
            if (id == IDC_RESET_TO_DEFAULT_BTN && code == BN_CLICKED) {
                on_reset_to_default_clicked();
                return 0;
            }
            if (id == IDC_BULK_TOGGLE_BTN && code == BN_CLICKED) {
                g_bulk_select_mode = !g_bulk_select_mode;
                SetDlgItemTextA(hwnd, IDC_BULK_TOGGLE_BTN,
                                g_bulk_select_mode ? "Card Click: On" : "Card Click: Off");
                ui_update_all_select_checkbox_visibility();
                /* Set/ON/OFF/High/Medium/Low/Off's enabled state depends
                 * on g_bulk_select_mode now too - refresh immediately,
                 * not just on the next selection change. */
                ui_refresh_bulk_target_buttons_enabled();
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
            if (id == IDC_BULK_ROWSELECT_COMBO && code == CBN_SELCHANGE) {
                int sel = (int)SendDlgItemMessageA(hwnd, IDC_BULK_ROWSELECT_COMBO, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < GRID_ROWS) {
                    bulk_select_row(sel);
                } else if (sel == GRID_ROWS) {
                    bulk_select_all();
                }
                /* sel == GRID_ROWS+1 is "Custom" - a deliberate no-op,
                 * leaves the current selection exactly as it was. */
                return 0;
            }
            if (id == IDC_BULK_SET_BTN && code == BN_CLICKED) {
                int sel = (int)SendDlgItemMessageA(hwnd, IDC_BULK_MODE_COMBO, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && (sel != PROTO_MODE_SINGLE || unlock_cw(hwnd))) {
                    bulk_apply_mode((uint8_t)sel);
                }
                return 0;
            }
            if (id == IDC_BULK_ON_BTN && code == BN_CLICKED) {
                bulk_turn_output_on();
                g_bulk_last_power_action = BULK_POWER_ON;
                InvalidateRect(GetDlgItem(hwnd, IDC_BULK_ON_BTN), NULL, FALSE);
                InvalidateRect(GetDlgItem(hwnd, IDC_BULK_OFF_BTN), NULL, FALSE);
                return 0;
            }
            if (id == IDC_BULK_OFF_BTN && code == BN_CLICKED) {
                bulk_turn_output_off();
                g_bulk_last_power_action = BULK_POWER_OFF;
                InvalidateRect(GetDlgItem(hwnd, IDC_BULK_ON_BTN), NULL, FALSE);
                InvalidateRect(GetDlgItem(hwnd, IDC_BULK_OFF_BTN), NULL, FALSE);
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
                            if (sel >= 0 && (sel != PROTO_MODE_SINGLE || unlock_cw(hwnd))) {
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
                        bulk_set_rowselect_combo(GRID_ROWS + 1); /* Custom */
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
            /* Plain text, not a pill (see add_ctrl call site) - the Avg
             * reading itself moved to the Summary card's own
             * avg_temp_block_subclass_proc, which bypasses
             * WM_CTLCOLORSTATIC entirely and doesn't need a case here. */
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
                SetTextColor(hdc, count_kill_switch_tripped() > 0 ? COLOR_APP_DISCONNECTED : COLOR_APP_CONNECTED);
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
                if (offset == IDC_CH_FREQ_OFFSET) {
                    SetTextColor(hdc, COLOR_APP_TEXT);
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

        /* Sizes the "Icon" button's owner-drawn popup menu items (see
         * that button's WM_COMMAND handler) - measured against a real
         * DC/font instead of a guessed fixed size, so it's never too
         * tight for the actual text. */
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
            if (mis->CtlType == ODT_MENU) {
                const char *text = (const char *)mis->itemData;
                HDC hdc = GetDC(hwnd);
                HFONT old_font = (HFONT)SelectObject(hdc, g_font);
                SIZE sz;
                GetTextExtentPoint32A(hdc, text, lstrlenA(text), &sz);
                SelectObject(hdc, old_font);
                ReleaseDC(hwnd, hdc);
                mis->itemWidth = (UINT)sz.cx + 40;
                mis->itemHeight = (UINT)sz.cy + 16;
                return TRUE;
            }
            break;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
            /* The "Icon" button's popup menu (Change Logo/Reset to
             * Default) - dark panel fill, accent highlight on hover,
             * matching the rest of the app's theme instead of a plain
             * stock Windows menu (direct report). itemData is the
             * label text pointer stashed by AppendMenuA(MF_OWNERDRAW)
             * back in that button's WM_COMMAND handler. */
            if (dis->CtlType == ODT_MENU) {
                const char *text = (const char *)dis->itemData;
                bool hot = (dis->itemState & ODS_SELECTED) != 0;
                RECT rc = dis->rcItem;
                HFONT old_font = (HFONT)SelectObject(dis->hDC, g_font);

                FillRect(dis->hDC, &rc, hot ? g_brush_accent : g_brush_panel);
                SetTextColor(dis->hDC, hot ? RGB(255, 255, 255) : COLOR_APP_TEXT);
                SetBkMode(dis->hDC, TRANSPARENT);
                rc.left += 16;
                DrawTextA(dis->hDC, text, lstrlenA(text), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dis->hDC, old_font);
                return TRUE;
            }
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

                /* Summary card's Mode toggle - icon only, no button
                 * background/badge (direct request), same blend-into-
                 * panel FillRect trick as the padlock above. Much
                 * bigger than the old text-button version of this same
                 * control used to be (that one, and its "Light Mode"/
                 * "Dark Mode" label, lived in the Command Panel; this
                 * replaces it entirely - same IDC_THEME_TOGGLE_BTN ID,
                 * same on_theme_toggle_clicked()/g_light_mode-driven
                 * icon choice, just relocated and redrawn). Direct
                 * correction - the icon now shows the CURRENT mode
                 * (sun while in Light mode, moon while in Dark mode),
                 * not the destination clicking it switches to - the
                 * opposite of the Connect/Disconnect convention this
                 * used to follow. No trig (avoids pulling in math.h/-lm
                 * for 8 lines) - the diagonal rays are a few px short
                 * of true 45 deg, invisible at this size. */
                if (dis->CtlID == IDC_THEME_TOGGLE_BTN) {
                    int icx = (rc.left + rc.right) / 2;
                    int icy = (rc.top + rc.bottom) / 2;
                    /* Theme-aware, NOT plain white - this button's own
                     * FillRect blends it into the panel background,
                     * which is white in Light mode, so a hardcoded white
                     * glyph would paint invisibly (confirmed by testing:
                     * flipped to Light mode, the icon vanished entirely,
                     * same class of bug the heatmap's idle color had).
                     * COLOR_APP_HEADER matches the padlock glyph's own
                     * color above, for the same reason - used for the
                     * moon (Dark mode). The sun (Light mode) gets its
                     * own warm yellow instead - direct request - which
                     * reads fine on both the white Light-mode panel and
                     * (moot, since the sun only ever shows in Light
                     * mode) the dark one. */
                    HBRUSH glyph_brush = CreateSolidBrush(COLOR_APP_HEADER);
                    HPEN old_pen_i;
                    HBRUSH old_brush_i;

                    FillRect(dis->hDC, &rc, g_brush_panel);
                    old_pen_i = (HPEN)SelectObject(dis->hDC, GetStockObject(NULL_PEN));
                    old_brush_i = (HBRUSH)SelectObject(dis->hDC, glyph_brush);

                    if (!g_light_mode) {
                        /* Moon: a disc, then a second disc offset
                         * up-right and filled in the panel's own
                         * background color, masking a crescent out of
                         * it - same layering trick the padlock's
                         * shackle uses above. Shown while in Dark
                         * mode (current state, not destination).
                         * Direct report (confirmed on real hardware, not
                         * just this sandbox) - a SAME-radius mask/main
                         * pair (the previous "verified" attempt) is
                         * mathematically incapable of a thin crescent -
                         * as the offset shrinks the covered arc grows,
                         * but never exceeds 180 degrees, so at best half
                         * the disc stays visible (a fat bite, not a
                         * sliver); that was misjudged as "good enough"
                         * against a Pillow render without comparing to
                         * an actual thin reference. Mask now genuinely
                         * bigger than main (82 vs. 64) with the whole
                         * construction shifted (+20,-20) to keep the
                         * resulting crescent - not just the mask's own
                         * bounding box - centered in this 220x220
                         * button. Re-verified pixel-for-pixel locally
                         * against this exact call before shipping. */
                        Ellipse(dis->hDC, icx - 44, icy - 84, icx + 84, icy + 44);
                        SelectObject(dis->hDC, g_brush_panel);
                        Ellipse(dis->hDC, icx - 38, icy - 126, icx + 126, icy + 38);
                    } else {
                        static const int ray_dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
                        static const int ray_dy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
                        HBRUSH sun_brush = CreateSolidBrush(RGB(255, 196, 0));
                        HPEN ray_pen = CreatePen(PS_SOLID, 10, RGB(255, 196, 0));
                        int ri;

                        SelectObject(dis->hDC, sun_brush);
                        Ellipse(dis->hDC, icx - 36, icy - 36, icx + 36, icy + 36);
                        SelectObject(dis->hDC, ray_pen);
                        for (ri = 0; ri < 8; ri++) {
                            MoveToEx(dis->hDC, icx + ray_dx[ri] * 46, icy + ray_dy[ri] * 46, NULL);
                            LineTo(dis->hDC, icx + ray_dx[ri] * 66, icy + ray_dy[ri] * 66);
                        }
                        DeleteObject(ray_pen);
                        DeleteObject(sun_brush);
                    }
                    SelectObject(dis->hDC, old_brush_i);
                    SelectObject(dis->hDC, old_pen_i);
                    DeleteObject(glyph_brush);
                    return TRUE;
                }

                /* ON/OFF are two real, separate buttons (not one toggle).
                 * Only ON gets a strong color when active (solid green) -
                 * OFF being the channel's normal/idle state doesn't get
                 * red anymore (that read as an alarm on every one of the
                 * usually-many idle cards, competing with ON's green and
                 * making it hard to tell which state a card was actually
                 * in - reported directly). OFF-active now gets a muted
                 * gray fill instead: still visibly "the current state",
                 * just not shouting about it. */
                if (offset == IDC_CH_ON_OFFSET || offset == IDC_CH_OFF_OFFSET) {
                    const ChannelState *ch = channels_get(idx);
                    bool active = (offset == IDC_CH_ON_OFFSET) ? ch->output_on : !ch->output_on;
                    HBRUSH fill = !active ? g_brush_panel
                        : disabled ? g_brush_panel
                        : (offset == IDC_CH_ON_OFFSET ? g_brush_connected : g_brush_level_off);
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
                    } else if (dis->CtlID == IDC_BULK_TOGGLE_BTN) {
                        /* Same green-when-armed convention as everything
                         * else here - the text already swaps "On"/"Off"
                         * but the button looked identical either way,
                         * direct complaint it didn't visibly "light up". */
                        fill = g_bulk_select_mode ? g_brush_connected : g_brush_accent;
                    } else if (dis->CtlID == IDC_CLOSE_ALL_BTN) {
                        /* Command Panel's 4 buttons each get their own
                         * color now (direct request) instead of all
                         * sharing the generic accent blue - Close/Open
                         * All reuse the same green-ON/red-OFF convention
                         * Bulk Actions' own ON/OFF buttons use above,
                         * since they're the same action at rack scale. */
                        fill = g_brush_disconnected;
                    } else if (dis->CtlID == IDC_OPEN_ALL_BTN) {
                        fill = g_brush_connected;
                    } else if (dis->CtlID == IDC_OPEN_LOG_BTN) {
                        fill = g_brush_accent;
                    } else if (dis->CtlID == IDC_CMD_CHANGE_ICON_BTN) {
                        fill = g_brush_level_medium; /* orange - distinct from the other 3 */
                    } else if (dis->CtlID == IDC_RESET_TO_DEFAULT_BTN) {
                        /* Light gray + black text (below) instead of the
                         * dark COLOR_APP_MUTED every other "off/neutral"
                         * fill uses - direct request, reads as a plain
                         * default/reset control rather than another
                         * colored action next to Emergency Shutdown/
                         * Global Activate/Open Csv Logs/Icon. */
                        fill = g_brush_reset_default;
                    }
                    {
                        HPEN old_pen = (HPEN)SelectObject(dis->hDC, GetStockObject(NULL_PEN));
                        HBRUSH old_brush = (HBRUSH)SelectObject(dis->hDC, disabled ? g_brush_accent_dis : fill);
                        RoundRect(dis->hDC, rc.left, rc.top, rc.right, rc.bottom, BTN_CORNER_DIAMETER, BTN_CORNER_DIAMETER);
                        SelectObject(dis->hDC, old_brush);
                        SelectObject(dis->hDC, old_pen);
                    }
                    /* See g_bulk_last_power_action's comment - a bright
                     * border traces whichever of Bulk ON/OFF was clicked
                     * most recently, so it stays visibly distinct from
                     * the other even though both are one-shot actions
                     * with no real per-button state to reflect. */
                    if (!disabled &&
                        ((dis->CtlID == IDC_BULK_ON_BTN && g_bulk_last_power_action == BULK_POWER_ON) ||
                         (dis->CtlID == IDC_BULK_OFF_BTN && g_bulk_last_power_action == BULK_POWER_OFF))) {
                        HPEN hl_pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
                        HPEN old_hl_pen = (HPEN)SelectObject(dis->hDC, hl_pen);
                        HBRUSH old_hl_brush = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                        RoundRect(dis->hDC, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1,
                                  BTN_CORNER_DIAMETER, BTN_CORNER_DIAMETER);
                        SelectObject(dis->hDC, old_hl_brush);
                        SelectObject(dis->hDC, old_hl_pen);
                        DeleteObject(hl_pen);
                    }
                }
                /* Command Panel's 4 icon buttons - on/off dot (hollow vs
                 * filled circle, same convention a lot of hardware power
                 * switches use), a warning triangle shared by Kill
                 * Switch and Reset (same safety system, same glyph), and
                 * a small document/lines icon for Open Log. Same
                 * plain-white, drawn-before-text, rc.left-shifted-after
                 * pattern as the theme toggle's sun/moon above, just a
                 * smaller 22px offset - these buttons are only 90px
                 * wide, not 110-130. */
                if (dis->CtlID == IDC_CLOSE_ALL_BTN || dis->CtlID == IDC_OPEN_ALL_BTN) {
                    int icx = rc.left + 13;
                    int icy = (rc.top + rc.bottom) / 2;
                    HPEN white_pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
                    HPEN old_pen_i = (HPEN)SelectObject(dis->hDC, white_pen);
                    HBRUSH old_brush_i = (HBRUSH)SelectObject(dis->hDC,
                        dis->CtlID == IDC_OPEN_ALL_BTN ? (HBRUSH)GetStockObject(WHITE_BRUSH)
                                                        : (HBRUSH)GetStockObject(NULL_BRUSH));
                    Ellipse(dis->hDC, icx - 5, icy - 5, icx + 5, icy + 5);
                    SelectObject(dis->hDC, old_brush_i);
                    SelectObject(dis->hDC, old_pen_i);
                    DeleteObject(white_pen);
                    rc.left += 22;
                }
                if (dis->CtlID == IDC_OPEN_LOG_BTN) {
                    int icx = rc.left + 13;
                    int icy = (rc.top + rc.bottom) / 2;
                    HPEN white_pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
                    HPEN old_pen_i = (HPEN)SelectObject(dis->hDC, white_pen);
                    HBRUSH old_brush_i = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                    Rectangle(dis->hDC, icx - 5, icy - 6, icx + 5, icy + 6);
                    MoveToEx(dis->hDC, icx - 3, icy - 2, NULL);
                    LineTo(dis->hDC, icx + 3, icy - 2);
                    MoveToEx(dis->hDC, icx - 3, icy + 2, NULL);
                    LineTo(dis->hDC, icx + 3, icy + 2);
                    SelectObject(dis->hDC, old_brush_i);
                    SelectObject(dis->hDC, old_pen_i);
                    DeleteObject(white_pen);
                    rc.left += 22;
                }
                /* Same "picture" glyph (frame + sun + mountain
                 * silhouette) any image-picker UI uses - a frame outline,
                 * a small filled dot for the sun, one triangular peak
                 * crossing the frame's bottom edge for the mountain. */
                if (dis->CtlID == IDC_CMD_CHANGE_ICON_BTN) {
                    int icx = rc.left + 13;
                    int icy = (rc.top + rc.bottom) / 2;
                    HPEN white_pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
                    HPEN old_pen_i = (HPEN)SelectObject(dis->hDC, white_pen);
                    HBRUSH white_brush = CreateSolidBrush(RGB(255, 255, 255));
                    HBRUSH old_brush_i = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                    Rectangle(dis->hDC, icx - 6, icy - 5, icx + 6, icy + 6);
                    SelectObject(dis->hDC, white_brush);
                    Ellipse(dis->hDC, icx - 3, icy - 3, icx, icy);
                    MoveToEx(dis->hDC, icx - 5, icy + 4, NULL);
                    LineTo(dis->hDC, icx, icy - 1);
                    LineTo(dis->hDC, icx + 5, icy + 4);
                    SelectObject(dis->hDC, old_brush_i);
                    SelectObject(dis->hDC, old_pen_i);
                    DeleteObject(white_pen);
                    DeleteObject(white_brush);
                    rc.left += 22;
                }
                /* Reset/refresh glyph (circular arrow, open on one side,
                 * with a small arrowhead at the open end) - the one
                 * button in this row with no icon before, direct
                 * request. Same left-inset/rc.left-shift convention as
                 * every other icon here. */
                if (dis->CtlID == IDC_RESET_TO_DEFAULT_BTN) {
                    int icx = rc.left + 13;
                    int icy = (rc.top + rc.bottom) / 2;
                    /* Black, not white - this button's fill is now light
                     * gray (g_brush_reset_default), same reasoning as its
                     * text below. */
                    HPEN black_pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
                    HPEN old_pen_i = (HPEN)SelectObject(dis->hDC, black_pen);
                    HBRUSH old_brush_i = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                    Arc(dis->hDC, icx - 6, icy - 6, icx + 6, icy + 6, icx, icy - 6, icx + 2, icy - 6);
                    MoveToEx(dis->hDC, icx - 3, icy - 9, NULL);
                    LineTo(dis->hDC, icx + 2, icy - 6);
                    LineTo(dis->hDC, icx - 2, icy - 3);
                    SelectObject(dis->hDC, old_brush_i);
                    SelectObject(dis->hDC, old_pen_i);
                    DeleteObject(black_pen);
                    rc.left += 22;
                }
                /* Dimmed text on top of the dimmed fill - white text on
                 * a gray disabled button still read as "basically the
                 * same brightness" as white text on a bright enabled
                 * one from a few feet away. Reset to Default is the one
                 * exception - its fill is light gray, not a dark theme
                 * color, so it gets black text instead (direct request),
                 * still muted-gray when disabled like everything else. */
                SetTextColor(dis->hDC, disabled ? COLOR_APP_MUTED
                              : (dis->CtlID == IDC_RESET_TO_DEFAULT_BTN) ? RGB(0, 0, 0)
                              : RGB(255, 255, 255));
                SetBkMode(dis->hDC, TRANSPARENT);
                GetWindowTextA(dis->hwndItem, text, sizeof(text));
                /* "Emergency Shutdown"/"Global Activate" don't fit this
                 * row's 84px buttons on one line - word-wrap to 2 lines
                 * instead of DT_SINGLELINE, then center that wrapped
                 * block vertically by hand (DT_VCENTER only works with
                 * DT_SINGLELINE). Everything else's text still fits on
                 * one line, so DT_CALCRECT's height comes back the same
                 * as a single line and this is a no-op for them. */
                {
                    RECT calc_rc = rc;
                    int text_h;
                    DrawTextA(dis->hDC, text, -1, &calc_rc, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
                    text_h = calc_rc.bottom - calc_rc.top;
                    calc_rc = rc;
                    calc_rc.top += ((rc.bottom - rc.top) - text_h) / 2;
                    calc_rc.bottom = calc_rc.top + text_h;
                    DrawTextA(dis->hDC, text, -1, &calc_rc, DT_CENTER | DT_WORDBREAK);
                }
                /* No dashed focus-rect after a click - the fill color
                 * already shows which button is active/current, the
                 * extra dotted outline just read as a stray line. */
                return TRUE;
            }
            break;
        }

        case WM_CLOSE: {
            /* Direct decision: closing now resets every channel to its
             * default settings for real (same as the Reset to Default
             * button), not just a local .ini write - so it needs a
             * confirmation before doing it, unlike the old silent
             * close. Cancel just swallows WM_CLOSE (no DestroyWindow
             * call), which is the standard Win32 "stay open" idiom. */
            int result = MessageBoxA(hwnd,
                "Every channel will be set to Pseudo Random Noise and turned ON before the app closes - it will keep transmitting after you exit.\n\nClose ECM Controller?",
                "Terminate ECM Controller", MB_OKCANCEL | MB_ICONWARNING);
            if (result == IDOK) {
                DestroyWindow(hwnd);
            }
            return 0;
        }

        case WM_DESTROY:
            /* Direct decision - replaces the old "Saving..." .ini write
             * (save_settings() removed, see its own comment) with an
             * actual hardware reset: every channel back to White
             * Noise/OFF, same real-send path the Reset to Default
             * button uses, run while the connection is still open
             * (before conn_disconnect() below). Loading dialog matches
             * the one on launch/the old Saving one - same minimum-
             * visible-time reasoning, this is genuinely fast too. */
            {
                DWORD loading_t0 = GetTickCount();
                HWND loading_hwnd = loading_dialog_show("Restoring to default...");
                on_reset_to_default_clicked();
                loading_dialog_hide(loading_hwnd, loading_t0);
            }
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
            if (g_logo_font && g_logo_font != g_header_font && g_logo_font != g_font) DeleteObject(g_logo_font);
            if (g_mono_font && g_mono_font != g_font) DeleteObject(g_mono_font);
            if (g_small_font && g_small_font != g_font) DeleteObject(g_small_font);
            if (g_big_font && g_big_font != g_mono_font) DeleteObject(g_big_font);
            if (g_huge_font && g_huge_font != g_big_font) DeleteObject(g_huge_font);
            if (g_custom_logo_bmp) DeleteObject(g_custom_logo_bmp);
            if (g_custom_icon_big) DestroyIcon(g_custom_icon_big);
            if (g_custom_icon_small) DestroyIcon(g_custom_icon_small);
            if (g_default_icon_big) DestroyIcon(g_default_icon_big);
            if (g_default_icon_small) DestroyIcon(g_default_icon_small);
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

    /* g_light_mode defaults false (dark) - the real "which theme did the
     * user last pick" answer isn't known until load_settings() reads
     * the .ini, well after this point (needs build_controls()'s combos
     * to already exist). This first call just gets every brush into a
     * valid, self-consistent dark state for the very first paint;
     * WM_CREATE re-applies the theme afterward if the saved setting
     * turns out to be Light. g_brush_level_medium is NOT part of either
     * theme (see create_theme_brushes()'s own comment) - created once,
     * here, and never touched again. */
    apply_theme();
    create_theme_brushes();
    g_brush_level_medium = CreateSolidBrush(RGB(224, 146, 34));

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
    g_default_icon_big = wc.hIcon;
    g_default_icon_small = wc.hIconSm;
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
    hwnd = CreateWindowExA(0, "DigitalNoiseConfigMultiMainWindow", "ECM Controller",
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
