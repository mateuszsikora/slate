/*
 * Material Design Icons codepoints used by this spike.
 *
 * Resolved from the CSS of the MDI release pinned in tools/fonts/generate.sh
 * (7.4.47), which is the same map the font generator itself uses — not typed
 * from memory. Every name below is on the committed list in tools/fonts/icons.txt,
 * so every one of these glyphs is really in the generated font; a codepoint that
 * is not in the font renders as an empty box, which on a render-performance
 * measurement would quietly remove the glyph work being measured.
 *
 * Regenerate with:
 *
 *   python3 - <<'PY'
 *   import re
 *   css = open('tools/fonts/.work/mdi.css', encoding='utf-8').read()
 *   m = dict(re.findall(r'\.mdi-([a-z0-9-]+)::before\s*\{\s*content:\s*"\\([0-9A-Fa-f]+)"', css))
 *   print(m['lightbulb-on'])
 *   PY
 */
#pragma once

/* System bar (§3.2) */
#define S2_ICON_CLOCK_OUTLINE        "\U000F0150"  /* mdi-clock-outline */
#define S2_ICON_WIFI_STRENGTH_4      "\U000F0928"  /* mdi-wifi-strength-4 */

/* light (§7.1) */
#define S2_ICON_LIGHTBULB_ON         "\U000F06E8"  /* mdi-lightbulb-on */
#define S2_ICON_LIGHTBULB_OUTLINE    "\U000F0336"  /* mdi-lightbulb-outline */
#define S2_ICON_CEILING_LIGHT        "\U000F0769"  /* mdi-ceiling-light */
#define S2_ICON_BRIGHTNESS_6         "\U000F00DF"  /* mdi-brightness-6 */

/* cover (§7.2) */
#define S2_ICON_WINDOW_SHUTTER       "\U000F111C"  /* mdi-window-shutter */
#define S2_ICON_WINDOW_SHUTTER_OPEN  "\U000F111E"  /* mdi-window-shutter-open */
#define S2_ICON_ARROW_UP             "\U000F005D"  /* mdi-arrow-up */
#define S2_ICON_ARROW_DOWN           "\U000F0045"  /* mdi-arrow-down */
#define S2_ICON_STOP                 "\U000F04DB"  /* mdi-stop */

/* sensor (§7.3) — device_class selects the icon */
#define S2_ICON_THERMOMETER          "\U000F050F"  /* mdi-thermometer */
#define S2_ICON_WATER_PERCENT        "\U000F058E"  /* mdi-water-percent */
#define S2_ICON_GAUGE                "\U000F029A"  /* mdi-gauge */
#define S2_ICON_WEATHER_SUNNY        "\U000F0599"  /* mdi-weather-sunny */

/* scene (§7.4) */
#define S2_ICON_SOFA_OUTLINE         "\U000F156D"  /* mdi-sofa-outline */
#define S2_ICON_MOVIE_OPEN_OUTLINE   "\U000F0FCF"  /* mdi-movie-open-outline */
#define S2_ICON_SLEEP                "\U000F04B2"  /* mdi-sleep */
#define S2_ICON_COFFEE_OUTLINE       "\U000F06CA"  /* mdi-coffee-outline */

/* Fallbacks (§3.1, §7.5) */
#define S2_ICON_HELP_CIRCLE_OUTLINE  "\U000F0625"  /* mdi-help-circle-outline */
#define S2_ICON_IMAGE_BROKEN_VARIANT "\U000F02EE"  /* mdi-image-broken-variant */
