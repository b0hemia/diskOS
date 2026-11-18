# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Colour tokens, type scale, ttk styling, drawn controls and display assets for the installer window.

Every visual decision lives here so every screen (Welcome, confirm, flashing, result) shares it:
  * tokens: colours, spacing scale, type scale (px at 100%)
  * STYLES: one table with the colours of every ttk style in every state (the tests read it too)
  * apply(): turns the table into ttk styles; buttons are drawn pills (rose primary with a soft glow,
    hairline secondary), quiet text links, drawn radio/checkbox indicators
  * helpers: rule() hairlines, kicker() section labels, set_display() image titles, set_status() status dots,
    disclosure()/set_disclosure() chevrons, hero_disc() the pressed-disc image, wordmark()
  * assets (diskos_installer/assets): display sheet (titles + kickers set in the site's Archivo/Michroma,
    pre-rendered, so no font is installed on the user's machine), the hero disc, the wordmark. Every
    image has a text or drawn fallback: a missing or broken file never stops the window.
"""

BG = "#050506"
SURFACE = "#121216"
HOVER = "#1c1c20"
PRESSED = "#242428"
TEXT = "#f5f5f7"
SECONDARY = "#b9b8b4"
KICKER = "#908f8b"          # site --ink-3: section kickers, host line, quiet meta text
DIVIDER = "#29292f"         # hairline rules between sections
LINE = "#5f5f68"            # interactive outlines (entry, secondary buttons)
ACCENT = "#ff375f"
ACCENT_HOVER = "#ff6482"
ACCENT_PRESSED = "#f02f58"
FOCUS = "#ffb0c0"
ON_ACCENT = "#0a0a0b"
WARN = "#ffcc00"
ERR = "#ff453a"
OK = "#34c759"
DISABLED = "#85858d"        # disabled text: dimmer, still legible
PANEL = BG                  # sections sit on the ground, split by hairlines (no filled boxes)

# vertical rhythm (px at 100% scaling): one place to tighten or loosen the window
SP = {"xs": 2, "s": 4, "m": 8, "l": 12, "xl": 20, "xxl": 28}
GUTTER = 28                 # window side margin; every section aligns to it
SECTION = 12                # space between a hairline and the content on either side of it
PANEL_PAD = 0               # inner inset of a section (flat sections align with the gutter)
INDICATOR = 16              # radio / checkbox indicator size
INDICATOR_GAP = 8           # space between an indicator or status dot and its text
HANG = INDICATOR + INDICATOR_GAP   # at 100%; hang(widget) gives it at the current display scale
SCREEN_MARGIN = 80          # title bar + taskbar kept clear when sizing the window
HERO_RISE = 0               # the hero disc top lines up with the top of the brand row (wordmark)
HIDPI_FACTOR = 1.5          # dense(): 150% display scaling or more (Tk scaling / (96/72))

# type scale in px at 100% (converted to points per platform in make_fonts)
TYPE = {"title": 26, "h": 16, "b": 13, "sub": 12, "btn": 13, "brand": 15, "kicker": 10, "mono": 12}

# states in ttk precedence order (first match wins)
_STATE = {"disabled": ("disabled",), "pressed": ("pressed", "!disabled"),
          "hover": ("active", "!disabled"), "focus": ("focus", "!disabled"),
          "selected": ("selected", "!disabled")}

_CHOICE = {"normal": (TEXT, PANEL), "hover": (TEXT, PANEL), "pressed": (TEXT, PANEL),
           "disabled": (DISABLED, PANEL), "focus": (TEXT, PANEL), "selected": (TEXT, PANEL)}

# style -> {state: (foreground, background)}; "normal" is the base
STYLES = {
    # secondary: hairline pill on the ground
    "TButton": {"normal": (TEXT, BG), "hover": (TEXT, HOVER), "pressed": (TEXT, PRESSED),
                "disabled": (DISABLED, BG), "focus": (TEXT, BG)},
    # primary: rose pill
    "Accent.TButton": {"normal": (ON_ACCENT, ACCENT), "hover": (ON_ACCENT, ACCENT_HOVER),
                       "pressed": (ON_ACCENT, ACCENT_PRESSED), "disabled": (DISABLED, PRESSED),
                       "focus": (ON_ACCENT, ACCENT)},
    # quiet text action (other actions, Refresh, disclosures)
    "Link.TButton": {"normal": (SECONDARY, BG), "hover": (TEXT, BG), "pressed": (TEXT, BG),
                     "disabled": (DISABLED, BG), "focus": (TEXT, BG)},
    "TRadiobutton": dict(_CHOICE),
    "Panel.TRadiobutton": dict(_CHOICE),
    "TCheckbutton": dict(_CHOICE),
    "Panel.TCheckbutton": dict(_CHOICE),
    "TEntry": {"normal": (TEXT, BG), "hover": (TEXT, BG), "disabled": (DISABLED, SURFACE),
               "focus": (TEXT, BG), "selected": (TEXT, BG)},
    "TLabel": {"normal": (TEXT, BG)},
    "Sub.TLabel": {"normal": (SECONDARY, BG)},
    "Lede.TLabel": {"normal": (SECONDARY, BG)},
    "Meta.TLabel": {"normal": (KICKER, BG)},
    "Kicker.TLabel": {"normal": (KICKER, BG)},
    "H.TLabel": {"normal": (TEXT, BG)},
    "Title.TLabel": {"normal": (TEXT, BG)},
    "Panel.TLabel": {"normal": (TEXT, PANEL)},
    "PanelSub.TLabel": {"normal": (SECONDARY, PANEL)},
    "Brand.TLabel": {"normal": (SECONDARY, BG)},
    "TFrame": {"normal": (TEXT, BG)},
    "Panel.TFrame": {"normal": (TEXT, PANEL)},
}
_FONTS = {}                  # apply() keeps the fonts here for plain-tk widgets (banner)

# the log Text widget and the plain-tk banners
LOG = (SECONDARY, SURFACE)
BANNER_WARN = (ON_ACCENT, WARN)
BANNER_OK = (ON_ACCENT, OK)


def _lum(h):
    c = [int(h[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    c = [v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4 for v in c]
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def contrast(a, b):
    la, lb = sorted((_lum(a), _lum(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)


def _hex(c):
    return tuple(int(c[i:i + 2], 16) for i in (1, 3, 5))


# ---- type ---------------------------------------------------------------------------------------
def make_fonts(root):
    """Named UI fonts from the TYPE px scale. System family (no font is installed): Tk's default, or Segoe UI on
    Windows when Tk lists it. Points = px * 0.75 (96 dpi), except macOS where Tk treats 1 pt as 1 px."""
    import sys
    import tkinter.font as tkfont
    fam = tkfont.nametofont("TkDefaultFont").actual("family")
    if sys.platform.startswith("win") and "Segoe UI" in tkfont.families(root):
        fam = "Segoe UI"            # only when installed; else keep what Tk resolved
    k = 1.0 if sys.platform == "darwin" else 0.75

    def f(px, weight="normal", underline=0):
        return tkfont.Font(root=root, family=fam, size=max(6, round(px * k)), weight=weight, underline=underline)
    mono = tkfont.nametofont("TkFixedFont").copy()
    mono.configure(size=max(6, round(TYPE["mono"] * k)))
    return {"title": f(TYPE["title"]), "h": f(TYPE["h"]), "b": f(TYPE["b"]), "sub": f(TYPE["sub"]),
            "btn": f(TYPE["btn"], "bold"), "brand": f(TYPE["brand"], "bold"),
            "kicker": f(TYPE["kicker"], "bold"), "link": f(TYPE["b"]), "link_hover": f(TYPE["b"], underline=1),
            "mono": mono}


# ---- drawn images (no asset files) ---------------------------------------------------------------
def _cov(d):
    """Pixel coverage of the region d <= 0 for a signed distance d (1 px anti-aliasing)."""
    return min(1.0, max(0.0, 0.5 - d))


def _paint(root, w, h, shade, bg):
    """A PhotoImage whose pixels are shade(x, y) -> [(colour, alpha), ...] layers composited over bg."""
    import tkinter as tk
    img = tk.PhotoImage(master=root, width=w, height=h)
    base = _hex(bg)
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            c = list(base)
            for col, a in shade(x + 0.5, y + 0.5):
                if a > 0:
                    rgb = _hex(col)
                    c = [c[i] + (rgb[i] - c[i]) * a for i in range(3)]
            row.append("#%02x%02x%02x" % tuple(int(round(v)) for v in c))
        rows.append("{" + " ".join(row) + "}")
    img.put(" ".join(rows))
    return img


def _seg_d(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    ln = dx * dx + dy * dy
    t = 0.0 if ln == 0 else max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / ln))
    return ((px - x1 - t * dx) ** 2 + (py - y1 - t * dy) ** 2) ** 0.5


def _check_image(root, size, bg, fill, border, tick):
    """One checkbox indicator: rounded box with a 1.5px border and, when `tick` is given, a tick mark.
    Drawn by 4x4 supersampling so the edges are smooth; no asset files."""
    import tkinter as tk
    img = tk.PhotoImage(master=root, width=size, height=size)
    n = 4
    r = 3.0
    segs = ((size * 0.26, size * 0.52, size * 0.43, size * 0.70), (size * 0.43, size * 0.70, size * 0.76, size * 0.30))

    def box_d(px, py):          # signed distance to the rounded box (negative inside)
        hx = hy = size / 2 - 0.5
        qx, qy = abs(px - size / 2) - (hx - r), abs(py - size / 2) - (hy - r)
        return (max(qx, 0) ** 2 + max(qy, 0) ** 2) ** 0.5 + min(max(qx, qy), 0) - r
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            acc = [0.0, 0.0, 0.0]
            for j in range(n):
                for i in range(n):
                    px, py = x + (i + 0.5) / n, y + (j + 0.5) / n
                    d = box_d(px, py)
                    col = _hex(bg)
                    if d <= 0:
                        col = _hex(border if d > -1.5 else fill)
                    if tick and min(_seg_d(px, py, *s) for s in segs) <= 1.1:
                        col = _hex(tick)
                    for k in range(3):
                        acc[k] += col[k]
            row.append("#%02x%02x%02x" % tuple(int(v / (n * n)) for v in acc))
        rows.append("{" + " ".join(row) + "}")
    img.put(" ".join(rows))
    return img


def _radio_image(root, size, bg, ring, dot):
    """Radio indicator: 1.5px ring, and a centred dot when selected."""
    c = size / 2
    r = size / 2 - 1

    def shade(x, y):
        d = ((x - c) ** 2 + (y - c) ** 2) ** 0.5
        out = [(ring, _cov(d - r) - _cov(d - r + 1.5 * size / 16))]
        if dot:
            out.append((dot, _cov(d - size * 0.22)))
        return out
    return _paint(root, size, size, shade, bg)


def _pill_skin(root, w, h, m, fill, border, glow, ring, bg):
    """Button skin: a pill of height h - m[1] - m[3] inside margins m=(l, t, r, b), with an optional 1px border,
    a soft glow cast below it (site .btn: rose light under the button) and an optional focus ring."""
    ml, mt, mr, mb = m
    ph = h - mt - mb
    rad = ph / 2
    cy = mt + rad
    x1, x2 = ml + rad, w - mr - rad

    def pill_d(x, y, grow=0.0, dy=0.0):
        return _seg_d(x, y, x1, cy + dy, x2, cy + dy) - (rad + grow)

    def shade(x, y):
        out = []
        if glow:
            col, strength, spread = glow
            d = pill_d(x, y, -rad * 0.35, rad * 0.45)
            if d > -spread:
                t = max(0.0, min(1.0, (d + spread) / (spread * 2.2)))
                out.append((col, strength * (1 - t) ** 2.2))
        if ring:
            d = pill_d(x, y, 2.5)
            out.append((ring, _cov(d) - _cov(d + 1.5)))
        d = pill_d(x, y)
        if border:
            out.append((border, _cov(d) - _cov(d + 1.0)))
            if fill:
                out.append((fill, _cov(d + 1.0)))
        elif fill:
            out.append((fill, _cov(d)))
        return out
    return _paint(root, w, h, shade, bg)


def _chevron(root, size, colour, down, bg=BG):
    s = size

    def shade(x, y):
        if down:
            a = (s * 0.22, s * 0.38, s * 0.5, s * 0.66, s * 0.78, s * 0.38)
        else:
            a = (s * 0.38, s * 0.22, s * 0.66, s * 0.5, s * 0.38, s * 0.78)
        d = min(_seg_d(x, y, a[0], a[1], a[2], a[3]), _seg_d(x, y, a[2], a[3], a[4], a[5]))
        return [(colour, _cov(d - 0.7))]
    return _paint(root, s, s, shade, bg)


def _dot(root, size, colour, bg=BG, gap=0):
    """A dot in a size x size cell, plus `gap` px of ground on its right (the space before its text)."""
    c = size / 2

    def shade(x, y):
        return [(colour, _cov(((x - c) ** 2 + (y - c) ** 2) ** 0.5 - (size - 1) / 2))]
    return _paint(root, size + gap, size, shade, bg)


def _pad_right(root, img, gap, bg):
    """img with `gap` px of ground added on its right: the indicator-to-label space of radios/checkboxes."""
    import tkinter as tk
    w, h = img.width(), img.height()
    out = tk.PhotoImage(master=root, width=w + gap, height=h)
    out.put(bg, to=(0, 0, w + gap, h))
    out.tk.call(out, "copy", img, "-to", 0, 0)
    return out


def _cache(root):
    c = getattr(root, "_diskos_imgs", None)
    if c is None:
        c = root._diskos_imgs = {}
    return c


def _line_px(root, font_key="b"):
    fonts = getattr(root, "_diskos_fonts", None)
    if fonts and font_key in fonts:
        return fonts[font_key].metrics("linespace")
    return 16


def dot(widget, colour, bg=BG):
    """Small status dot with the gap before its text built in (cached per colour). Width = status_indent()."""
    root = widget._root()
    key = ("dot", colour, bg)
    c = _cache(root)
    if key not in c:
        c[key] = _dot(root, max(6, round(_line_px(root) * 0.42)), colour, bg, gap=round(INDICATOR_GAP * ui_scale(root)))
    return c[key]


def dot_top(widget):
    """Top padding that centres a dot on the first line of body text beside it."""
    return max(0, (_line_px(widget._root()) - dot(widget, TEXT).height()) // 2)


def status_indent(widget):
    """Left indent that lines text up with the text after a status dot."""
    return dot(widget, TEXT).width()


def chevron(widget, down, colour=SECONDARY, bg=BG):
    root = widget._root()
    key = ("chev", down, colour, bg)
    c = _cache(root)
    if key not in c:
        c[key] = _chevron(root, max(8, round(_line_px(root) * 0.62)), colour, down, bg)
    return c[key]


def ui_scale(widget):
    """Display scale relative to 100% (96 dpi), never below 1 (macOS Tk reports 72 dpi)."""
    try:
        return max(1.0, float(widget._root().tk.call("tk", "scaling")) / (96 / 72))
    except Exception:
        return 1.0


def indicator_size(widget):
    return round(INDICATOR * ui_scale(widget))


def hang(widget):
    """Indent that lines text up with the label of a radio/checkbox (indicator + gap) at this display scale."""
    return indicator_size(widget) + round(INDICATOR_GAP * ui_scale(widget))


def checkbox_style(root, st, fonts):
    """Replace clam's X indicator with a tick for Checkbutton and Panel.TCheckbutton."""
    size = indicator_size(root)
    gap = hang(root) - size
    keep = root._diskos_check_imgs = []
    for style, bg in (("TCheckbutton", BG), ("Panel.TCheckbutton", PANEL)):
        el = style.replace(".", "_") + ".indicator"
        make = lambda fill, border, tick: keep.append(
            _pad_right(root, _check_image(root, size, bg, fill, border, tick), gap, bg)) or keep[-1]
        off, on = make(BG, LINE, None), make(ACCENT, ACCENT, ON_ACCENT)
        foff, fon = make(BG, FOCUS, None), make(ACCENT, FOCUS, ON_ACCENT)
        doff, don = make(PRESSED, DIVIDER, None), make(PRESSED, DIVIDER, DISABLED)
        st.element_create(el, "image", off,
                          ("disabled", "selected", don), ("disabled", doff),
                          ("focus", "selected", fon), ("focus", foff), ("selected", on),
                          width=size + gap, height=size, sticky="")
        st.layout(style, [("Checkbutton.padding", {"sticky": "nswe", "children": [
            (el, {"side": "left", "sticky": ""}),
            ("Checkbutton.focus", {"side": "left", "sticky": "w", "children": [
                ("Checkbutton.label", {"sticky": "nswe"})]})]})])


def radio_style(root, st):
    """Drawn radio indicators: hairline ring, rose ring + dot when selected, pink ring on focus."""
    size = indicator_size(root)
    gap = hang(root) - size
    keep = root._diskos_radio_imgs = []
    for style, bg in (("TRadiobutton", BG), ("Panel.TRadiobutton", PANEL)):
        el = style.replace(".", "_") + ".indicator"
        make = lambda ring, dot_: keep.append(
            _pad_right(root, _radio_image(root, size, bg, ring, dot_), gap, bg)) or keep[-1]
        off, on = make(LINE, None), make(ACCENT, ACCENT)
        foff, fon = make(FOCUS, None), make(FOCUS, ACCENT)
        doff, don = make(DIVIDER, None), make(DIVIDER, DISABLED)
        st.element_create(el, "image", off,
                          ("disabled", "selected", don), ("disabled", doff),
                          ("focus", "selected", fon), ("focus", foff), ("selected", on),
                          width=size + gap, height=size, sticky="")
        st.layout(style, [("Radiobutton.padding", {"sticky": "nswe", "children": [
            (el, {"side": "left", "sticky": ""}),
            ("Radiobutton.focus", {"side": "left", "sticky": "w", "children": [
                ("Radiobutton.label", {"sticky": "nswe"})]})]})])


# button geometry: pill height = linespace * k; margins hold the glow and focus ring
BUTTONS = {
    "Accent.TButton": {"font": "btn", "k": 2.0, "m": (3, 2, 3, 9), "padx": 0.62},
    "TButton": {"font": "b", "k": 1.9, "m": (3, 3, 3, 3), "padx": 0.55},
    "Small.TButton": {"font": "sub", "k": 1.8, "m": (3, 3, 3, 3), "padx": 0.5},
}


def _button_skins(root, st, fonts):
    """Draw each button style's pill skins (sized from its font) and install them as the style's layout."""
    keep = root._diskos_button_imgs = []
    for name, g in BUTTONS.items():
        lh = fonts[g["font"]].metrics("linespace")
        s = lh / 16.0
        m = tuple(max(1, round(v * s)) for v in g["m"])
        ph = round(lh * g["k"])
        h = m[1] + ph + m[3]
        cap = m[0] + ph // 2 + 1
        w = cap * 2 + 1
        pad_x = m[0] + round(ph * g["padx"])
        pad_t = m[1] + (ph - lh) // 2
        pad_b = h - lh - pad_t
        if name == "Accent.TButton":
            glow = (ACCENT, 0.55, 9 * s)
            spec = {"normal": (ACCENT, None, glow, None),
                    "hover": (ACCENT_HOVER, None, (ACCENT, 0.8, 11 * s), None),
                    "pressed": (ACCENT_PRESSED, None, (ACCENT, 0.35, 7 * s), None),
                    "focus": (ACCENT, None, glow, FOCUS),
                    "hoverfocus": (ACCENT_HOVER, None, (ACCENT, 0.8, 11 * s), FOCUS),
                    "disabled": (PRESSED, None, None, None)}
        else:
            spec = {"normal": (BG, LINE, None, None), "hover": (HOVER, "#8a8a93", None, None),
                    "pressed": (PRESSED, "#8a8a93", None, None), "focus": (BG, LINE, None, FOCUS),
                    "hoverfocus": (HOVER, "#8a8a93", None, FOCUS), "disabled": (BG, DIVIDER, None, None)}
        imgs = {k: _pill_skin(root, w, h, m, f, b, gl, rg, BG) for k, (f, b, gl, rg) in spec.items()}
        keep.extend(imgs.values())
        el = name.replace(".", "_") + ".skin"
        st.element_create(el, "image", imgs["normal"],
                          ("disabled", imgs["disabled"]), ("pressed", imgs["pressed"]),
                          ("focus", "active", imgs["hoverfocus"]), ("active", imgs["hover"]),
                          ("focus", imgs["focus"]),
                          border=(cap, h // 2, cap, h - h // 2 - 1), padding=0, sticky="nsew")
        st.layout(name, [(el, {"sticky": "nswe", "children": [
            ("Button.padding", {"sticky": "nswe", "children": [("Button.label", {"sticky": "nswe"})]})]})])
        st.configure(name, padding=(pad_x, pad_t, pad_x, pad_b), anchor="center", width=0)


def apply(root, fonts):
    """Configure ttk (clam) for every widget class the installer uses, in every state."""
    from tkinter import ttk
    _FONTS["f"] = fonts
    st = ttk.Style(root)
    try:
        st.theme_use("clam")
    except Exception:
        pass
    root.configure(bg=BG)
    root._diskos_fonts = fonts
    # buttons: colours from STYLES; shape from the drawn skins (flat clam boxes if drawing fails)
    for name, font in (("TButton", fonts["b"]), ("Accent.TButton", fonts["btn"]), ("Small.TButton", fonts["sub"])):
        s = STYLES["TButton" if name == "Small.TButton" else name]
        st.configure(name, font=font, padding=(14, 5), foreground=s["normal"][0], background=s["normal"][1],
                     bordercolor=ACCENT if name == "Accent.TButton" else LINE,
                     lightcolor=s["normal"][1], darkcolor=s["normal"][1], focuscolor=FOCUS,
                     focusthickness=1, relief="flat")
        st.map(name,
               foreground=[(_STATE[k], v[0]) for k, v in s.items() if k in ("disabled", "pressed")],
               background=[(_STATE[k], s[k][1]) for k in ("disabled", "pressed", "hover") if k in s],
               bordercolor=[(("disabled",), DIVIDER), (("focus", "!disabled"), FOCUS)],
               lightcolor=[(_STATE[k], s[k][1]) for k in ("disabled", "pressed", "hover")],
               darkcolor=[(_STATE[k], s[k][1]) for k in ("disabled", "pressed", "hover")])
    try:
        _button_skins(root, st, fonts)
    except Exception:
        pass                         # keep clam's flat buttons rather than fail to start
    # quiet text action: no box; brighter and underlined on hover; a 1px focus ring for the keyboard
    s = STYLES["Link.TButton"]
    st.layout("Link.TButton", [("Button.padding", {"sticky": "nswe", "children": [
        ("Button.focus", {"sticky": "nswe", "children": [("Button.label", {"sticky": "nswe"})]})]})])
    st.configure("Link.TButton", font=fonts["link"], foreground=s["normal"][0], background=BG,
                 padding=(0, 2), focuscolor=FOCUS, focusthickness=1, relief="flat", anchor="w", width=0)
    st.map("Link.TButton", foreground=[(("disabled",), DISABLED), (("pressed",), TEXT), (("active",), TEXT),
                                       (("focus",), TEXT)],
           background=[(("active",), BG), (("pressed",), BG)],
           font=[(("active", "!disabled"), fonts["link_hover"])])
    for name in ("TRadiobutton", "Panel.TRadiobutton", "TCheckbutton", "Panel.TCheckbutton"):
        s = STYLES[name]
        bg = s["normal"][1]
        st.configure(name, font=fonts["b"], foreground=TEXT, background=bg, focuscolor=FOCUS,
                     indicatorbackground=bg, indicatorforeground=TEXT,
                     upperbordercolor=LINE, lowerbordercolor=LINE, indicatormargin=(0, 2, 8, 2), padding=(0, 2))
        st.map(name,
               foreground=[(("disabled",), DISABLED)],
               background=[(("disabled",), bg), (("active",), bg)],
               indicatorcolor=[(("disabled",), PRESSED), (("selected",), ACCENT), (("!selected",), BG)],
               indicatorbackground=[(("disabled",), PRESSED), (("selected",), ACCENT), (("!selected",), BG)],
               upperbordercolor=[(("disabled",), DIVIDER), (("focus", "!disabled"), FOCUS), (("selected",), ACCENT)],
               lowerbordercolor=[(("disabled",), DIVIDER), (("focus", "!disabled"), FOCUS), (("selected",), ACCENT)])
    try:
        checkbox_style(root, st, fonts)
    except Exception:
        pass                         # keep clam's own indicator rather than fail to start
    try:
        radio_style(root, st)
    except Exception:
        pass
    st.configure("TEntry", font=fonts["b"], foreground=TEXT, fieldbackground=BG, insertcolor=TEXT,
                 bordercolor=LINE, lightcolor=BG, darkcolor=BG, padding=(8, 5),
                 selectbackground=ACCENT, selectforeground=ON_ACCENT)
    st.map("TEntry", fieldbackground=[(("disabled",), SURFACE)], foreground=[(("disabled",), DISABLED)],
           bordercolor=[(("focus", "!disabled"), FOCUS), (("disabled",), DIVIDER)],
           lightcolor=[(("focus", "!disabled"), BG)], darkcolor=[(("focus", "!disabled"), BG)])
    st.configure("TCombobox", font=fonts["b"], foreground=TEXT, fieldbackground=BG, background=BG,
                 arrowcolor=SECONDARY, bordercolor=LINE, lightcolor=BG, darkcolor=BG, padding=(8, 5),
                 selectbackground=BG, selectforeground=TEXT)
    st.map("TCombobox", fieldbackground=[(("disabled",), SURFACE), (("readonly",), BG)],
           foreground=[(("disabled",), DISABLED)], arrowcolor=[(("disabled",), DISABLED)],
           bordercolor=[(("focus", "!disabled"), FOCUS), (("disabled",), DIVIDER)],
           selectbackground=[(("readonly",), BG)], selectforeground=[(("readonly",), TEXT)])
    for opt, val in (("background", BG), ("foreground", TEXT), ("selectBackground", ACCENT),
                     ("selectForeground", ON_ACCENT), ("font", fonts["b"])):
        root.option_add("*TCombobox*Listbox." + opt, val)      # the drop-down list is a plain Tk listbox
    st.configure("TFrame", background=BG)
    st.configure("Panel.TFrame", background=PANEL, bordercolor=DIVIDER, relief="flat")
    for name, font in (("TLabel", "b"), ("Sub.TLabel", "sub"), ("Lede.TLabel", "b"), ("Meta.TLabel", "sub"), ("Kicker.TLabel", "kicker"),
                       ("H.TLabel", "h"), ("Title.TLabel", "title"), ("Brand.TLabel", "brand"),
                       ("Panel.TLabel", "b"), ("PanelSub.TLabel", "sub")):
        fg, bg = STYLES[name]["normal"]
        st.configure(name, foreground=fg, background=bg, font=fonts[font])
    st.configure("diskos.Horizontal.TProgressbar", background=ACCENT, troughcolor=SURFACE,
                 bordercolor=SURFACE, lightcolor=ACCENT, darkcolor=ACCENT, thickness=4)
    st.configure("Vertical.TScrollbar", background=PRESSED, troughcolor=BG, bordercolor=BG,
                 arrowcolor=SECONDARY, lightcolor=PRESSED, darkcolor=PRESSED, arrowsize=12)
    st.map("Vertical.TScrollbar", background=[(("active",), LINE)], arrowcolor=[(("disabled",), DISABLED)])
    return st


def style_log(text):
    text.configure(bg=LOG[1], fg=LOG[0], insertbackground=TEXT, selectbackground=ACCENT,
                   selectforeground=ON_ACCENT, highlightthickness=1, highlightbackground=DIVIDER,
                   highlightcolor=FOCUS, padx=10, pady=8)


# ---- layout helpers -----------------------------------------------------------------------------
def rule(parent, colour=DIVIDER):
    """A 1px hairline (pack with fill="x"). Sections are split by these, never boxed."""
    import tkinter as tk
    return tk.Frame(parent, height=1, bg=colour, bd=0, highlightthickness=0)


def kicker(parent, text, style="Kicker.TLabel"):
    """Section label in the site's kicker voice (Michroma, uppercase, tracked), or small caps text."""
    from tkinter import ttk
    lbl = ttk.Label(parent, style=style)
    set_display(lbl, text, "kicker")
    return lbl


def set_display(label, text, kind="title"):
    """Show `text` on a ttk.Label as its pre-rendered image when the display sheet has it, else as text.
    The text stays in the widget either way (compound="image" only hides it)."""
    img = display(label, kind, text)
    if img is not None:
        label.configure(text=text, image=img, compound="image")
    else:
        label.configure(text=(text.upper() if kind == "kicker" else text), image="", compound="none")
    label._diskos_img = img
    return img is not None


def set_status(label, text, colour):
    """Status line: a small coloured dot, then the text (secondary when the colour is the quiet one)."""
    fg = SECONDARY if colour in (SECONDARY, KICKER) else TEXT
    try:
        img = dot(label, colour)
        label.configure(text=text, image=img, compound="left", foreground=fg)
    except Exception:
        label.configure(text=text, foreground=colour)


def disclosure(parent, text, command):
    """A quiet text button with a drawn chevron (closed). set_disclosure() turns it."""
    from tkinter import ttk
    b = ttk.Button(parent, text=text, style="Link.TButton", command=command)
    set_disclosure(b, False)
    return b


def set_disclosure(button, is_open):
    try:
        button.configure(image=(chevron(button, is_open), "active", chevron(button, is_open, TEXT)),
                         compound="left")
    except Exception:
        button.configure(text=("v " if is_open else "> ") + str(button.cget("text")).lstrip("v> "))
    button._diskos_open = bool(is_open)


def disc(parent, size, bg=BG):
    """Static disc mark on a Canvas: grooved rim, label, hole and a fixed highlight. No animation.
    Fallback for hero_disc() when its image cannot load."""
    import tkinter as tk
    c = tk.Canvas(parent, width=size, height=size, bg=bg, highlightthickness=0, bd=0)
    m, r = size / 2, size / 2 - 1

    def ring(rad, fill, line=""):
        c.create_oval(m - rad, m - rad, m + rad, m + rad, fill=fill, outline=line)
    ring(r, "#141417", DIVIDER)
    k = 0.92
    while k > 0.6:                               # grooves
        ring(r * k, "#101013", "#1b1b20")
        k -= 0.08
    ring(r * 0.54, "#202126", "")                # mirror band
    ring(r * 0.50, "#08080a", "#2a2a30")         # hub (the Disc's screen)
    c.create_arc(m - r * 0.4, m - r * 0.4, m + r * 0.4, m + r * 0.4, start=-135, extent=-195,
                 style="arc", outline=ACCENT, width=max(1, size // 50))
    return c


def hero_disc(parent):
    """The pressed disc with its spectral light (pre-rendered from the site's disc shader), as a label.
    Falls back to the drawn disc() mark."""
    from tkinter import ttk
    img = _asset_image(parent, "disc")
    if img is None:
        return disc(parent, 96)
    lbl = ttk.Label(parent, image=img, style="TLabel", padding=0)
    lbl._diskos_img = img
    return lbl


def hero_bleed(container, hero, top):
    """The hero disc at the right of `hero`, rising to the top of `top` (the brand row) so it sits behind the
    header like the site's disc. It is placed in `hero` (so it hides with it) as a child of `container`
    (so it is not clipped to the hero). The hero keeps a spacer as wide as the disc, so text never runs under
    it, and as tall as the part of the disc below the hero top, so nothing after the hero overlaps it.
    Falls back to the drawn disc() mark packed in the hero."""
    import tkinter as tk
    from tkinter import ttk
    img = _asset_image(container, "disc")
    if img is None:
        d = disc(hero, 96, bg=BG)
        d.pack(side="right", anchor="center")
        return d
    lbl = ttk.Label(container, image=img, style="TLabel", padding=0)
    lbl._diskos_img = img
    spacer = tk.Frame(hero, width=img.width(), height=1, bg=BG, bd=0, highlightthickness=0)
    spacer.pack(side="right", anchor="n")

    def sync(_e=None):
        dy = top.winfo_y() - hero.winfo_y() + HERO_RISE
        need = max(1, img.height() + dy)
        if int(spacer.cget("height")) != need:
            spacer.configure(height=need)
        lbl.place(in_=hero, relx=1.0, x=0, y=dy, anchor="ne")
        lbl.lift()
    hero.bind("<Configure>", sync, add="+")
    top.bind("<Configure>", sync, add="+")
    hero.update_idletasks()
    sync()
    return lbl


# ---- assets -------------------------------------------------------------------------------------
def asset_dirs():
    """Where the asset files may live: next to this module (source tree, tarball) or under the
    PyInstaller data root (sys._MEIPASS/diskos_installer/assets)."""
    import os
    import sys
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets")
    dirs = [here]
    base = getattr(sys, "_MEIPASS", None)
    if base:
        dirs.insert(0, os.path.join(base, "diskos_installer", "assets"))
    return dirs


def _find_asset(name):
    import os
    for d in asset_dirs():
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    return None


def dense(root):
    """True on screens at 150% scaling or more."""
    try:
        return float(root.tk.call("tk", "scaling")) / (96 / 72) >= HIDPI_FACTOR - 0.01   # Tk rounds scaling
    except Exception:
        return False


SCALES = ("1", "1.5", "2")     # asset scales; the wordmark has only 1 and 2


def asset_scales(widget):
    """Asset scales to try, nearest to the display scale first (ratio distance): 100% -> 1, 125%/150% -> 1.5,
    175% and up -> 2. On 100% screens the larger scales are never preferred."""
    import math
    f = ui_scale(widget)
    return sorted(SCALES, key=lambda k: (abs(math.log(f / float(k))), float(k)))


def _suffix(scale):
    return "" if scale == "1" else "@%sx" % scale


def _asset_image(widget, stem):
    """<stem>.png / <stem>@1.5x.png / <stem>@2x.png nearest the display scale, as a cached PhotoImage, or None."""
    import tkinter as tk
    root = widget._root()
    c = _cache(root)
    for scale in asset_scales(root):
        name = stem + _suffix(scale) + ".png"
        if name not in c:
            p = _find_asset(name)
            try:
                c[name] = tk.PhotoImage(master=root, file=p) if p else None
            except Exception:
                c[name] = None
        if c[name] is not None:
            return c[name]
    return None


def display(widget, kind, text):
    """The pre-rendered image of a display string ("title" or "kicker"), or None (callers fall back to text).
    One sheet per scale (display.png, display@1.5x.png, display@2x.png) indexed by display.json."""
    import json
    import tkinter as tk
    root = widget._root()
    c = _cache(root)
    key = kind + ":" + text
    scale = asset_scales(root)[0]
    ck = ("display", scale, key)
    if ck in c:
        return c[ck]
    img = None
    try:
        if "display.json" not in c:
            p = _find_asset("display.json")
            with open(p) as f:
                c["display.json"] = json.load(f)
        box = c["display.json"][scale].get(key)
        sheet = c.get("sheet" + scale)
        if sheet is None:
            p = _find_asset("display" + _suffix(scale) + ".png")
            sheet = c["sheet" + scale] = tk.PhotoImage(master=root, file=p) if p else None
        if box is not None and sheet is not None:
            x, y, w, h = box
            img = tk.PhotoImage(master=root, width=w, height=h)
            img.tk.call(img, "copy", sheet, "-from", x, y, x + w, y + h, "-to", 0, 0)
    except Exception:
        img = None
    c[ck] = img
    return img


def wordmark(root):
    """The site wordmark as a PhotoImage (nearest scale), or None so callers fall back to text."""
    try:
        return _asset_image(root, "wordmark")
    except Exception:
        return None


# ---- confirm, progress and result screens -------------------------------------------------------
# Same look as the welcome: hairlines, kickers and titles from the display sheet, drawn status dots.
DIALOG_W = 560              # confirm dialog width at 100% (scaled up on denser screens by the caller)
DIALOG_PAD = (GUTTER, 24)   # x, y
WRAP = 530                  # label wrap width in the run/result body
CARD_PAD = 14               # inner inset of a bordered card (next step / recovery)
KV_LABEL_W = 200            # confirm summary: kicker column
MAX_WINDOW_H = 700          # the whole window must fit a 1366x768 laptop; taller content scrolls

BORDER = {"quiet": DIVIDER, "line": LINE, "warn": WARN, "err": ERR}
STATUS_COLOUR = {"ok": OK, "err": ERR}


def title(parent, text="", level="h", wrap=None):
    """The one place a screen title is made. level 'title': the pre-rendered display image when the sheet has the
    string (text otherwise); 'h': a section heading in the system font (dynamic phase text)."""
    from tkinter import ttk
    kw = {"wraplength": wrap, "justify": "left"} if wrap else {}
    lbl = ttk.Label(parent, style=("Title.TLabel" if level == "title" else "H.TLabel"), **kw)
    retitle(lbl, text, level)
    return lbl


def retitle(label, text, level="h"):
    """Change an existing title label's text and level."""
    if level == "title":
        label.configure(style="Title.TLabel")
        set_display(label, text, "title")
    else:
        label.configure(text=text, style="H.TLabel", image="", compound="none")
        label._diskos_img = None


def status_word(parent):
    """The status line above a result title (dot + Success / Error E303). Set with set_status_word."""
    from tkinter import ttk
    return ttk.Label(parent, text="", style="TLabel")


def set_status_word(label, text, kind):
    set_status(label, text, STATUS_COLOUR[kind])


def banner(parent, wrap):
    """The keep-plugged-in note: a calm hairline card in the warn colour with a warn status dot and sentence-case
    text (not a filled slab). config(text=...) / cget("text") reach its label, so callers treat it like a label."""
    import tkinter as tk
    from tkinter import ttk

    class WarnNote(tk.Frame):
        def __init__(self):
            tk.Frame.__init__(self, parent, bg=PANEL, highlightthickness=1, highlightbackground=BORDER["warn"])
            self._lbl = ttk.Label(self, text="", style="Panel.TLabel", wraplength=wrap - 2 * CARD_PAD,
                                  justify="left")
            self._lbl.pack(anchor="w", padx=CARD_PAD, pady=SECTION)
            self._text = ""

        def config(self, **kw):
            if "text" in kw:
                self._text = kw.pop("text")
                set_status(self._lbl, self._text, WARN)
            if kw:
                tk.Frame.config(self, **kw)
        configure = config

        def cget(self, key):
            return self._text if key == "text" else tk.Frame.cget(self, key)

    return WarnNote()


def panel(parent, border="quiet"):
    """A hairline-bordered card on the ground (no filled box); add rows with panel_row()."""
    import tkinter as tk
    return tk.Frame(parent, bg=PANEL, highlightthickness=1, highlightbackground=BORDER[border])


def panel_row(box, head=None, text=None, head_kind="plain", wrap=WRAP):
    """A card heading (kicker, or a coloured status dot for warn/err) and its text."""
    from tkinter import ttk
    if head:
        if head_kind == "plain":
            lbl = kicker(box, head)
        else:
            lbl = ttk.Label(box, text=head, style="Panel.TLabel")
            set_status(lbl, head, WARN if head_kind == "warn" else ERR)
        lbl.pack(anchor="w", padx=CARD_PAD, pady=(SECTION, SP["s"]))
    if text:
        ttk.Label(box, text=text, style="Panel.TLabel", wraplength=wrap - 2 * CARD_PAD, justify="left").pack(
            anchor="w", padx=CARD_PAD, pady=(0 if head else SECTION, SECTION))


def acknowledgement(parent, text, variable, command, wrap=None):
    """The unticked 'I understand' box of the destructive gate: the drawn checkbox of the welcome."""
    from tkinter import ttk
    return ttk.Checkbutton(parent, variable=variable, text=text, command=command)


def style_dialog(dlg):
    dlg.configure(bg=BG, highlightthickness=1, highlightbackground=LINE)


def _fonts(_widget=None):
    """The named fonts apply() was given (plain-tk widgets need them explicitly)."""
    return _FONTS["f"]
