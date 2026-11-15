#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

"""A menuconfig front end that draws the kernel's mconf screen.

Kconfiglib is used for the configuration model only -- visibility, values,
choices, writing the .config. Everything you see is drawn here, following the
geometry and palettes in the kernel's scripts/kconfig:

  mconf.c                  the dialog loop, item text, menu_instructions
  lxdialog/menubox.c       dialog_menu(): box sizes, button row, scroll arrows
  lxdialog/util.c          print_title/print_button/print_autowrap/draw_shadow,
                           and the classic / bluetitle / blackbg / mono themes
  lxdialog/dialog.h        ACS fallbacks and the minimum display sizes

The numbers below are transcribed from those files rather than eyeballed, so
the screen matches `make menuconfig` rather than merely resembling it.
"""

import curses
import os
import re
import sys
import textwrap

import kconfiglib
from kconfiglib import BOOL, COMMENT, HEX, INT, MENU, STRING, TRISTATE, Symbol

TYPE_NAME = {BOOL: "bool", TRISTATE: "tristate", STRING: "string",
             INT: "int", HEX: "hex"}

# mconf.c: menu_instructions[]
MENU_INSTRUCTIONS = (
    "Arrow keys navigate the menu.  "
    "<Enter> selects submenus ---> (or empty submenus ----).  "
    "Highlighted letters are hotkeys.  "
    "Pressing <Y> includes, <N> excludes, <M> modularizes features.  "
    "Press <Esc><Esc> to exit, <?> for Help, </> for Search.  "
    "Legend: [*] built-in  [ ] excluded  <M> module  < > module capable"
)

# dialog.h
MENUBOX_HEIGHT_MIN = 15
MENUBOX_WIDTH_MIN = 65
WINDOW_HEIGHT_MIN = 19
WINDOW_WIDTH_MIN = 80

# menubox.c: print_buttons() lays the row out at these offsets from width/2-28.
BUTTONS = ("Select", " Exit ", " Help ", " Save ", " Load ")
BUTTON_X0 = -28
BUTTON_STEP = 12

# util.c: first_alpha() skips these when picking the hotkey letter, because they
# are bound to y/n/m/h themselves.
HOTKEY_EXEMPT = "YyNnMmHh"


# ---------------------------------------------------------------------------
# Palettes -- lxdialog/util.c set_classic_theme() and friends
# ---------------------------------------------------------------------------

THEME_CLASSIC = {
    "screen": ("cyan", "blue", True),
    "shadow": ("black", "black", True),
    "dialog": ("black", "white", False),
    "title": ("yellow", "white", True),
    "border": ("white", "white", True),
    "button_active": ("white", "blue", True),
    "button_inactive": ("black", "white", False),
    "button_key_active": ("white", "blue", True),
    "button_key_inactive": ("red", "white", False),
    "button_label_active": ("yellow", "blue", True),
    "button_label_inactive": ("black", "white", True),
    "menubox": ("black", "white", False),
    "menubox_border": ("white", "white", True),
    "position_indicator": ("yellow", "white", True),
    "item": ("black", "white", False),
    "item_selected": ("white", "blue", True),
    "tag": ("yellow", "white", True),
    "tag_selected": ("yellow", "blue", True),
    "tag_key": ("yellow", "white", True),
    "tag_key_selected": ("yellow", "blue", True),
    "uarrow": ("green", "white", True),
    "darrow": ("green", "white", True),
}

# bluetitle is classic with these seven overridden, and it is the default.
THEME_BLUETITLE = dict(THEME_CLASSIC, **{
    "title": ("blue", "white", True),
    "button_key_active": ("yellow", "blue", True),
    "button_label_active": ("white", "blue", True),
    "position_indicator": ("blue", "white", True),
    "tag": ("blue", "white", True),
    "tag_key": ("blue", "white", True),
})

THEME_BLACKBG = {
    "screen": ("red", "black", True),
    "shadow": ("black", "black", False),
    "dialog": ("white", "black", False),
    "title": ("red", "black", False),
    "border": ("black", "black", True),
    "button_active": ("yellow", "red", False),
    "button_inactive": ("yellow", "black", False),
    "button_key_active": ("yellow", "red", True),
    "button_key_inactive": ("red", "black", False),
    "button_label_active": ("white", "red", False),
    "button_label_inactive": ("black", "black", True),
    "menubox": ("yellow", "black", False),
    "menubox_border": ("black", "black", True),
    "position_indicator": ("red", "black", False),
    "item": ("white", "black", False),
    "item_selected": ("white", "red", False),
    "tag": ("red", "black", False),
    "tag_selected": ("yellow", "red", True),
    "tag_key": ("red", "black", False),
    "tag_key_selected": ("yellow", "red", True),
    "uarrow": ("red", "black", False),
    "darrow": ("red", "black", False),
}

THEMES = {
    "classic": THEME_CLASSIC,
    "bluetitle": THEME_BLUETITLE,
    "blackbg": THEME_BLACKBG,
}

# util.c set_mono_theme(): everything is A_NORMAL except these.
MONO = {
    "screen": 0, "shadow": 0, "dialog": 0, "title": curses.A_BOLD, "border": 0,
    "button_active": curses.A_REVERSE, "button_inactive": curses.A_DIM,
    "button_key_active": curses.A_REVERSE, "button_key_inactive": curses.A_BOLD,
    "button_label_active": curses.A_REVERSE, "button_label_inactive": 0,
    "menubox": 0, "menubox_border": 0, "position_indicator": curses.A_BOLD,
    "item": 0, "item_selected": curses.A_REVERSE,
    "tag": curses.A_BOLD, "tag_selected": curses.A_REVERSE,
    "tag_key": curses.A_BOLD, "tag_key_selected": curses.A_REVERSE,
    "uarrow": curses.A_BOLD, "darrow": curses.A_BOLD,
}


def build_attrs(theme_name):
    """Resolve a palette into curses attributes, exactly like init_dialog_colors."""
    if theme_name == "mono" or not curses.has_colors():
        return dict(MONO)

    table = THEMES.get(theme_name) or THEME_BLUETITLE
    curses.start_color()
    curses.use_default_colors() if hasattr(curses, "use_default_colors") else None

    attrs = {}
    for pair, (name, (fg, bg, hl)) in enumerate(table.items(), start=1):
        curses.init_pair(pair, getattr(curses, "COLOR_" + fg.upper()),
                         getattr(curses, "COLOR_" + bg.upper()))
        atr = curses.color_pair(pair)
        attrs[name] = atr | (curses.A_BOLD if hl else 0)
    return attrs


# ---------------------------------------------------------------------------
# Text metrics borrowed from lxdialog/util.c
# ---------------------------------------------------------------------------

def first_alpha(string, exempt=HOTKEY_EXEMPT):
    """Index of the first alphabetic char outside brackets -- the hotkey letter."""
    in_paren = 0
    for i, ch in enumerate(string):
        low = ch.lower()
        if low in "<[(":
            in_paren += 1
        if low in ">])" and in_paren > 0:
            in_paren -= 1
        if not in_paren and low.isalpha() and low not in exempt:
            return i
    return 0


def autowrap_lines(text, width, x):
    """util.c print_autowrap(): a centred line when the text is short, else a
    greedy space-wrap at the dialog's inner width.

    Wrapping greedily at ``width - 2`` reproduces the kernel's line breaks for
    menu_instructions byte for byte; the kernel's word loop looks more
    complicated than it behaves because it also tracks double spaces.
    """
    if len(text) <= width - x * 2:
        return [text]
    return textwrap.wrap(text, width, break_long_words=False,
                         break_on_hyphens=False) or [""]


# ---------------------------------------------------------------------------
# Model helpers
# ---------------------------------------------------------------------------

def node_visible(node):
    return bool(node.prompt) and bool(node.prompt[1])


def menu_is_empty(node):
    return not node.list


def build_items(menu, show_all=False):
    """mconf.c build_conf(): the rows for the menu now being entered.

    With show_all set, entries hidden by `depends on` are listed too -- that is
    what <Z> toggles.
    """
    items = []

    node = menu.list
    while node:
        if show_all or node_visible(node):
            if node.prompt:
                items.append(_item_for(node))
        node = node.next
    return items


def _item_for(node):
    """One row: the value column, the prompt, and the ---> arrow."""
    prompt = node.prompt[0]
    arrow = "  ----" if menu_is_empty(node) else "  --->"  # mconf: "%s  %s"

    if node.item == MENU:
        return ("   " + prompt + arrow, "m", node)

    if node.item == COMMENT:
        return ("   *** {} ***".format(prompt), ":", node)

    sym = node.item
    if not isinstance(sym, Symbol):
        return ("   " + prompt, ":", node)

    if sym.type == BOOL:
        text = "[*]" if sym.tri_value == 2 else "[ ]"
    else:
        # Nothing in this tree is a bool, but do not crash if one appears.
        text = "(?)"

    text += " " + prompt
    if sym.user_value is None:
        text += " (NEW)"
    if node.list or node.item is not None and node.is_menuconfig:
        text += arrow

    return (text, "t", node)


def help_text(node):
    return getattr(node, "help", "") or ""


# ---------------------------------------------------------------------------
# The dialog
# ---------------------------------------------------------------------------

class DisplayTooSmall(Exception):
    """Raised when the terminal cannot hold the dialog."""


class Mconf(object):

    def __init__(self, kconf, conf_path):
        self.kconf = kconf
        self.conf_path = conf_path
        self.atr = build_attrs(os.getenv("MENUCONFIG_COLOR", "bluetitle"))
        self.trail = []            # subtitle breadcrumbs, root first
        self.status = ""
        self.changed = False
        self.show_all = False

    # -- drawing primitives ------------------------------------------------

    @staticmethod
    def put(scr, y, x, text, atr=0):
        """addnstr that cannot raise on the last column of the last row."""
        h, w = scr.getmaxyx()
        if y < 0 or y >= h or x >= w or not text:
            return
        room = w - x - 1
        if room <= 0:
            return
        scr.addnstr(y, x, text, min(len(text), room), atr)

    def putc(self, scr, y, x, ch, atr=0):
        h, w = scr.getmaxyx()
        if 0 <= y < h - 1 and 0 <= x < w - 1:
            scr.addch(y, x, ch, atr)

    def hline(self, scr, y, x, n, ch, atr=0):
        for i in range(n):
            self.putc(scr, y, x + i, ch, atr)

    def vline(self, scr, y, x, n, ch, atr=0):
        for i in range(n):
            self.putc(scr, y + i, x, ch, atr)

    def draw_box(self, scr, y, x, height, width, box, border):
        """util.c draw_box(): corners take one attribute, edges another."""
        if height < 1 or width < 2:
            return
        for i in range(height):
            for j in range(width):
                if not i and not j:
                    ch, atr = curses.ACS_ULCORNER, border
                elif i == height - 1 and not j:
                    ch, atr = curses.ACS_LLCORNER, border
                elif not i and j == width - 1:
                    ch, atr = curses.ACS_URCORNER, box
                elif i == height - 1 and j == width - 1:
                    ch, atr = curses.ACS_LRCORNER, box
                elif not i or i == height - 1:
                    ch, atr = curses.ACS_HLINE, border if not i else box
                elif not j or j == width - 1:
                    ch, atr = curses.ACS_VLINE, border if not j else box
                else:
                    ch, atr = " ", box
                self.putc(scr, y + i, x + j, ch, atr)

    def draw_shadow(self, scr, y, x, height, width):
        """util.c draw_shadow(): repaint the cells under the shadow attribute."""
        if not curses.has_colors():
            return
        for j in range(width):
            self._shadow_cell(scr, y + height, x + 2 + j)
        for i in range(y + 1, y + height + 1):
            self._shadow_cell(scr, i, x + width)
            self._shadow_cell(scr, i, x + width + 1)

    def _shadow_cell(self, scr, y, x):
        h, w = scr.getmaxyx()
        if not (0 <= y < h and 0 <= x < w - 1):
            return
        # Keep whatever glyph is already there, only shifting its colour, which
        # is what draw_shadow() does with winch(win) & A_CHARTEXT.
        code = scr.inch(y, x) & 0xFF
        ch = chr(code) if 32 <= code < 127 else " "
        scr.addch(y, x, ch, self.atr["shadow"])

    def print_title(self, scr, y, x, title, width, atr, border_atr):
        """util.c print_title(): centred on the top border, padded with spaces."""
        if not title:
            return
        tlen = min(width - 2, len(title))
        start = (width - tlen) // 2
        self.putc(scr, y, x + start - 1, " ", atr)
        self.put(scr, y, x + start, title[:tlen], atr)
        self.putc(scr, y, x + start + tlen, " ", atr)

    def print_button(self, scr, y, x, label, selected):
        """util.c print_button(): '<' + label + '>' with a highlighted hotkey."""
        a = self.atr
        act = "button_active" if selected else "button_inactive"
        lbl = "button_label_active" if selected else "button_label_inactive"
        key = "button_key_active" if selected else "button_key_inactive"

        self.putc(scr, y, x, "<", a[act])
        pad = len(label) - len(label.lstrip(" "))
        stripped = label.lstrip(" ")
        for i in range(pad):
            self.putc(scr, y, x + 1 + i, " ", a[lbl])
        cx = x + 1 + pad
        if stripped:
            self.putc(scr, y, cx, stripped[0], a[key])
            self.put(scr, y, cx + 1, stripped[1:], a[lbl])
        self.putc(scr, y, x + 1 + len(label), ">", a[act])

    def print_buttons(self, scr, y, width, selected):
        x = width // 2 + BUTTON_X0
        for i, label in enumerate(BUTTONS):
            self.print_button(scr, y, x + i * BUTTON_STEP, label, selected == i)

    # -- the screen --------------------------------------------------------

    def dialog_clear(self, scr):
        """util.c dialog_clear(): background, back title, subtitle rule."""
        h, w = scr.getmaxyx()
        # bkgd() + erase() paints every cell; writing a full-width run by hand
        # would touch the last cell of the last row and scroll the terminal.
        scr.bkgd(" ", self.atr["screen"])
        scr.erase()

        backtitle = "{} - {}".format(
            self.conf_path, self.kconf.mainmenu_text or "Main menu")
        self.put(scr, 0, 1, backtitle, self.atr["screen"])

        # Row 1 is the breadcrumb trail; it doubles as a rule when the trail is
        # empty, because the fill loop below runs from len+1 either way.
        self.put(scr, 1, 1, "", self.atr["screen"])
        col = 1
        for part in self.trail:
            self.putc(scr, 1, col, curses.ACS_RARROW, self.atr["screen"])
            self.putc(scr, 1, col + 1, " ", self.atr["screen"])
            self.put(scr, 1, col + 2, part, self.atr["screen"])
            col += len(part) + 3
        self.hline(scr, 1, col, w - 1 - col, curses.ACS_HLINE,
                   self.atr["screen"])

    def dialog_menu(self, scr, title, menu, selected_node):
        """lxdialog/menubox.c dialog_menu(). Returns (result, node)."""
        h, w = scr.getmaxyx()
        if h < MENUBOX_HEIGHT_MIN or w < MENUBOX_WIDTH_MIN:
            return -1, None

        items = build_items(menu, self.show_all)
        if not items:
            return -1, None

        height = h - 4
        width = w - 5
        menu_height = height - 10
        max_choice = min(menu_height, len(items))

        x = (w - width) // 2
        y = (h - height) // 2

        self.draw_shadow(scr, y, x, height, width)
        self.draw_box(scr, y, x, height, width,
                      self.atr["dialog"], self.atr["border"])

        # Separator between the button row and everything above it.
        self.putc(scr, y + height - 3, x, curses.ACS_LTEE, self.atr["border"])
        self.hline(scr, y + height - 3, x + 1, width - 2, curses.ACS_HLINE,
                   self.atr["border"])
        self.putc(scr, y + height - 3, x + width - 1, curses.ACS_RTEE,
                  self.atr["border"])

        self.print_title(scr, y, x, title, width, self.atr["title"],
                         self.atr["border"])

        # Instructions, wrapped and centred the way print_autowrap does it.
        # print_autowrap() draws at x=3 inside a window that includes its own
        # border, so the text has to stay clear of the last column.
        lines = autowrap_lines(MENU_INSTRUCTIONS, width - 5, 3)
        for i, line in enumerate(lines):
            self.put(scr, y + 1 + i, x + 3, line, self.atr["dialog"])

        menu_width = width - 6
        box_y = height - menu_height - 5
        box_x = (width - menu_width) // 2 - 1
        item_x = (menu_width - 70) // 2 if menu_width >= 80 else 4

        self.draw_box(scr, y + box_y, x + box_x, menu_height + 2,
                      menu_width + 2, self.atr["menubox_border"],
                      self.atr["menubox"])

        choice = 0
        for i, (_, _, node) in enumerate(items):
            if node is selected_node:
                choice = i

        scroll = 0
        if choice >= max_choice:
            if choice >= len(items) - max_choice // 2:
                scroll = len(items) - max_choice
            else:
                scroll = choice - max_choice // 2
            choice -= scroll
        self.status = ""
        return self._menu_loop(scr, y, x, height, width, menu_height, menu_width,
                               box_y, box_x, item_x, items, scroll, choice,
                               selected_node)

    def _menu_loop(self, scr, y, x, height, width, menu_height, menu_width,
                   box_y, box_x, item_x, items, scroll, choice, selected_node):
        button = 0
        max_choice = min(menu_height, len(items))

        while True:
            self._draw_menu_items(scr, y, x, menu_height, menu_width, box_y,
                                  box_x, item_x, items, scroll, choice)
            self._print_arrows(scr, y, x, box_y, box_x, item_x, menu_height,
                               len(items), scroll)
            self.print_buttons(scr, y + height - 2, width, button)
            self._draw_status(scr, y, x, height, width)
            scr.refresh()

            key = scr.getch()

            if key == curses.KEY_RESIZE:
                return -2, None
            if key in (27,):
                return 1, items[scroll + choice][2]
            if key in (ord("e"), ord("x")):
                return 1, items[scroll + choice][2]

            if key in (9, curses.KEY_LEFT, curses.KEY_RIGHT):
                if key == curses.KEY_LEFT:
                    button -= 1
                else:
                    button += 1
                button = 4 if button < 0 else (0 if button > 4 else button)
                continue
            if key in (10, 13):
                return button, items[scroll + choice][2]

            if key in (curses.KEY_UP, ord("-")):
                if choice < 2 and scroll:
                    scroll -= 1
                else:
                    choice = max(choice - 1, 0)
                continue
            if key in (curses.KEY_DOWN, ord("+")):
                if choice > max_choice - 3 and scroll + max_choice < len(items):
                    scroll += 1
                else:
                    choice = min(choice + 1, max_choice - 1)
                continue
            if key == curses.KEY_PPAGE:
                choice = max(choice - max_choice, 0)
                scroll = max(scroll - max_choice, 0)
                continue
            if key == curses.KEY_NPAGE:
                choice = min(choice + max_choice, max_choice - 1)
                scroll = min(scroll + max_choice,
                             max(0, len(items) - max_choice))
                continue

            if key in (ord(" "), ord("y")):
                return 8, items[scroll + choice][2]
            if key == ord("n"):
                return 6, items[scroll + choice][2]
            if key == ord("m"):
                return 7, items[scroll + choice][2]
            if key in (ord("h"), ord("?")):
                return 2, items[scroll + choice][2]
            if key == ord("/"):
                return 9, items[scroll + choice][2]
            if key == ord("z"):
                return 10, items[scroll + choice][2]

            # Hotkey jump: the web of first_alpha() letters, YyNnMmHh exempt.
            if 0 < key < 256 and chr(key).isalpha():
                target = chr(key).lower()
                for i in range(max_choice):
                    if i >= len(items) - scroll:
                        break
                    text = items[scroll + i][0]
                    idx = first_alpha(text)
                    if idx < len(text) and text[idx].lower() == target:
                        choice = i
                        break

    def _draw_menu_items(self, scr, y, x, menu_height, menu_width, box_y, box_x,
                         item_x, items, scroll, choice):
        """menubox.c do_print_item(): text, then the hotkey letter on top."""
        inner_y = y + box_y + 1
        inner_x = x + box_x + 1

        for i in range(min(menu_height, len(items) - scroll)):
            text, tag, _ = items[scroll + i]
            row = inner_y + i
            selected = (i == choice)
            base = "item_selected" if selected else "item"

            # menubox.c do_print_item(): wclrtoeol() clears the row with the
            # menubox attribute, then mvwaddstr() paints *only the entry text*
            # with item_selected. The highlight is therefore exactly as long as
            # the text -- the rest of the row stays plain background.
            self.put(scr, row, inner_x, " " * menu_width, self.atr["menubox"])
            clipped = text[: menu_width - item_x]
            self.put(scr, row, inner_x + item_x, clipped, self.atr[base])

            # The hotkey letter goes last so the fill above cannot hide it.
            if tag != ":":
                idx = first_alpha(clipped)
                if 0 <= idx < len(clipped):
                    hot = "tag_key_selected" if selected else "tag_key"
                    self.putc(scr, row, inner_x + item_x + idx, clipped[idx],
                              self.atr[hot])

    def _print_arrows(self, scr, y, x, box_y, box_x, item_x, menu_height,
                      item_count, scroll):
        ax = x + box_x + item_x + 1
        top = y + box_y
        bottom = y + box_y + menu_height + 1

        if scroll > 0:
            self.putc(scr, top, ax, curses.ACS_UARROW, self.atr["uarrow"])
            self.put(scr, top, ax + 1, "(-)", self.atr["uarrow"])
        else:
            self.hline(scr, top, ax, 4, curses.ACS_HLINE,
                       self.atr["menubox_border"])

        if menu_height < item_count and scroll + menu_height < item_count:
            self.putc(scr, bottom, ax, curses.ACS_DARROW, self.atr["darrow"])
            self.put(scr, bottom, ax + 1, "(+)", self.atr["darrow"])
        else:
            self.hline(scr, bottom, ax, 4, curses.ACS_HLINE,
                       self.atr["menubox"])

    def _draw_status(self, scr, y, x, height, width):
        if not self.status:
            return
        self.put(scr, y + height - 4, x + 1, self.status[: width - 2],
                 self.atr["dialog"])

    # -- textbox (Help) ----------------------------------------------------

    def textbox(self, scr, title, body):
        """lxdialog/dialog_textbox(), reduced to what a help window needs."""
        h, w = scr.getmaxyx()
        lines = []
        for para in body.split("\n"):
            lines.append(para)

        height = min(len(lines) + 8, h - 4)
        width = min(w - 4, max(60, max((len(l) for l in lines), default=40) + 6))
        x = (w - width) // 2
        y = (h - height) // 2
        inner = height - 6
        scroll = 0

        while True:
            self.dialog_clear(scr)
            self.draw_shadow(scr, y, x, height, width)
            self.draw_box(scr, y, x, height, width,
                          self.atr["dialog"], self.atr["border"])
            self.print_title(scr, y, x, title, width, self.atr["title"],
                             self.atr["border"])

            for i in range(inner):
                idx = scroll + i
                if idx < len(lines):
                    self.put(scr, y + 2 + i, x + 2,
                             lines[idx][: width - 4], self.atr["dialog"])

            if scroll > 0:
                self.putc(scr, y, x + width - 5, curses.ACS_UARROW,
                          self.atr["position_indicator"])
            if scroll + inner < len(lines):
                self.putc(scr, y + height - 1, x + width - 5,
                          curses.ACS_DARROW, self.atr["position_indicator"])

            bx = width // 2 - 14
            self.print_button(scr, y + height - 3, x + bx, " Exit ", False)
            self.print_button(scr, y + height - 3, x + bx + 16, " Help ", False)
            scr.refresh()

            key = scr.getch()
            if key in (27, ord("e"), ord("x"), ord("q"), 10, 13):
                return
            if key in (curses.KEY_DOWN, ord("j"), ord(" ")):
                scroll = min(scroll + 1, max(0, len(lines) - inner))
            elif key in (curses.KEY_UP, ord("k")):
                scroll = max(scroll - 1, 0)
            elif key == curses.KEY_NPAGE:
                scroll = min(scroll + inner, max(0, len(lines) - inner))
            elif key == curses.KEY_PPAGE:
                scroll = max(scroll - inner, 0)

    def msgbox(self, scr, title, body):
        self.textbox(scr, title, body)

    def yesno(self, scr, title, prompt):
        """A centred Yes/No box, laid out like dialog_yesno()."""
        h, w = scr.getmaxyx()
        width = min(len(prompt) + 8, w - 4)
        height = 8
        x = (w - width) // 2
        y = (h - height) // 2
        sel = 0

        while True:
            self.dialog_clear(scr)
            self.draw_shadow(scr, y, x, height, width)
            self.draw_box(scr, y, x, height, width,
                          self.atr["dialog"], self.atr["border"])
            self.putc(scr, y + height - 2, x, curses.ACS_LTEE,
                      self.atr["border"])
            self.hline(scr, y + height - 2, x + 1, width - 2,
                       curses.ACS_HLINE, self.atr["border"])
            self.putc(scr, y + height - 2, x + width - 1, curses.ACS_RTEE,
                      self.atr["border"])
            self.print_title(scr, y, x, title, width, self.atr["title"],
                             self.atr["border"])
            for i, line in enumerate(autowrap_lines(prompt, width - 4, 0)):
                self.put(scr, y + 2 + i, x + 3, line, self.atr["dialog"])

            bx = width // 2 - 10
            self.print_button(scr, y + height - 3, x + bx, " Yes ", sel == 0)
            self.print_button(scr, y + height - 3, x + bx + 12, " No ", sel == 1)
            scr.refresh()

            key = scr.getch()
            if key in (9, curses.KEY_LEFT, curses.KEY_RIGHT):
                sel = 1 - sel
            elif key in (ord("y"), ord("Y")):
                return True
            elif key in (ord("n"), ord("N"), 27):
                return False
            elif key == curses.KEY_RESIZE:
                return False
            elif key in (10, 13):
                return sel == 0

    # -- the loop ----------------------------------------------------------

    def run(self, scr):
        curses.curs_set(0)
        scr.keypad(True)
        curses.set_escdelay(25)
        self.trail = []

        while True:
            menu = self.kconf.top_node
            for part in self.trail:
                menu = self._find_child(menu, part)
                if menu is None:
                    self.trail = []
                    menu = self.kconf.top_node
                    break
            title = menu.prompt[0] if menu.prompt else "Main Menu"

            selected = None
            while True:
                self.dialog_clear(scr)
                res, node = self.dialog_menu(scr, title, menu, selected)
                if res == -2:
                    continue
                if res in (-1, 1):
                    break
                selected = node

                if res == 0:
                    if node is not None and node.list:
                        self.trail.append(node.prompt[0])
                        title = node.prompt[0]
                        menu = node
                        selected = None
                        continue
                elif res == 2:
                    self._show_help(scr, node, menu)
                elif res == 3:
                    self.save(scr)
                elif res == 4:
                    self.load(scr)
                elif res == 8:
                    self.toggle(node)
                elif res in (5, 6, 7):
                    self.set_bool(node, 2 if res == 5 else 0)
                elif res == 9:
                    self.search(scr)
                elif res == 10:
                    self.show_all = not self.show_all
                    selected = node

            if not self.trail:
                if self.changed and not self.yesno(
                        scr, "Save configuration?",
                        "Do you wish to save your new configuration?"):
                    return
                if self.changed:
                    self.save(scr, quiet=True)
                return
            self.trail.pop()
            selected = None

    def _find_child(self, menu, prompt):
        node = menu.list
        while node:
            if node.prompt and node.prompt[0] == prompt:
                return node
            node = node.next
        return None

    def search(self, scr):
        """mconf.c search_conf(): find symbols, then show their relations."""
        query = self.inputbox(scr, "Search Configuration Parameter",
                              "Enter (sub)string or regexp to search for "
                              "(with or without \"CONFIG_\")")
        if not query:
            return
        if query.upper().startswith("CONFIG_"):
            query = query[len("CONFIG_"):]

        try:
            pattern = re.compile(query)
        except re.error as exc:
            self.textbox(scr, "Search Results",
                         "Bad regular expression: {}".format(exc))
            return
        hits = [sym for name, sym in sorted(self.kconf.syms.items())
                if pattern.search(name)]

        if not hits:
            self.textbox(scr, "Search Results",
                         "Nothing matched '{}'.".format(query))
            return

        lines = []
        for sym in hits[:40]:
            value = "y" if sym.tri_value == 2 else "n"
            lines.append("Symbol: {} [={}]".format(sym.name, value))
            lines.append("Type  : {}".format(TYPE_NAME.get(sym.type, "unknown")))
            if sym.nodes and sym.nodes[0].prompt:
                lines.append("Prompt: {}".format(sym.nodes[0].prompt[0]))
            path = self._path_to(sym)
            if path:
                lines.append("  Location:")
                lines.append("    -> " + " -> ".join(path))
            lines.append("")
        self.textbox(scr, "Search Results", "\n".join(lines))

    def _path_to(self, sym):
        """The menu trail from the root to a symbol's first node."""
        for node in sym.nodes:
            parts = []
            parent = node
            while parent is not None and parent is not self.kconf.top_node:
                if parent.prompt:
                    parts.append(parent.prompt[0])
                parent = parent.parent
            if parts:
                parts.reverse()
                return parts
        return []

    def inputbox(self, scr, title, prompt):
        """lxdialog/dialog_inputbox(), enough for the search query."""
        h, w = scr.getmaxyx()
        width = min(75, w - 4)
        height = 8
        x = (w - width) // 2
        y = (h - height) // 2
        text = []

        while True:
            self.dialog_clear(scr)
            self.draw_shadow(scr, y, x, height, width)
            self.draw_box(scr, y, x, height, width,
                          self.atr["dialog"], self.atr["border"])
            self.putc(scr, y + height - 2, x, curses.ACS_LTEE,
                      self.atr["border"])
            self.hline(scr, y + height - 2, x + 1, width - 2,
                       curses.ACS_HLINE, self.atr["border"])
            self.putc(scr, y + height - 2, x + width - 1, curses.ACS_RTEE,
                      self.atr["border"])
            self.print_title(scr, y, x, title, width, self.atr["title"],
                             self.atr["border"])
            for i, line in enumerate(textwrap.wrap(prompt, width - 6)):
                self.put(scr, y + 2 + i, x + 3, line, self.atr["dialog"])

            # Rows: 0 title, 1 blank, 2..3 prompt, 4 input field, 5 separator,
            # 6 buttons, 7 bottom border.
            field = "".join(text)[: width - 6]
            self.put(scr, y + 4, x + 2, field.ljust(width - 4),
                     self.atr["item_selected"])
            bx = width // 2 - 10
            self.print_button(scr, y + height - 2, x + bx, " OK ", True)
            self.print_button(scr, y + height - 2, x + bx + 12, " Help ", False)
            scr.refresh()

            key = scr.getch()
            if key in (10, 13):
                return "".join(text)
            if key in (27,):
                return None
            if key in (curses.KEY_BACKSPACE, 127, 8):
                if text:
                    text.pop()
            elif 32 <= key < 127:
                text.append(chr(key))

    def _show_help(self, scr, node, menu):
        if node is not None and node.prompt:
            title = node.prompt[0]
            body = help_text(node) or "No help available for this entry."
        else:
            title = "README"
            body = "Navigate with the arrow keys; <Enter> enters a submenu."
        self.textbox(scr, title, body)

    def toggle(self, node):
        if node is None or node.item == MENU or node.item == COMMENT:
            return
        sym = node.item
        if isinstance(sym, Symbol) and sym.type == BOOL:
            sym.set_value(0 if sym.tri_value == 2 else 2)
            self.changed = True

    def set_bool(self, node, value):
        if node is None or not isinstance(node.item, Symbol):
            return
        if node.item.type == BOOL:
            node.item.set_value(value)
            self.changed = True

    def save(self, scr=None, quiet=False):
        try:
            self.kconf.write_config(self.conf_path)
            self.changed = False
            self.status = "Configuration saved to {}".format(self.conf_path)
        except OSError as exc:
            self.status = "Save failed: {}".format(exc)
        if scr is not None and not quiet:
            self.msgbox(scr, "Save", self.status)

    def load(self, scr=None):
        try:
            self.kconf.load_config(self.conf_path, replace=False)
            self.changed = False
            self.status = "Loaded {}".format(self.conf_path)
        except OSError as exc:
            self.status = "Load failed: {}".format(exc)
        if scr is not None:
            self.msgbox(scr, "Load", self.status)


# ---------------------------------------------------------------------------

def main(path):
    kconf = kconfiglib.Kconfig(path, suppress_traceback=False)
    conf_path = os.getenv("KCONFIG_CONFIG", ".config")
    try:
        kconf.load_config(conf_path, replace=False)
    except (IOError, OSError):
        pass

    def run(scr):
        # lxdialog init_dialog() refuses a display smaller than this instead of
        # drawing a garbled dialog.
        height, width = scr.getmaxyx()
        if height < WINDOW_HEIGHT_MIN or width < WINDOW_WIDTH_MIN:
            raise DisplayTooSmall()
        Mconf(kconf, conf_path).run(scr)

    try:
        curses.wrapper(run)
    except DisplayTooSmall:
        # curses is already torn down here, so this lands on a normal screen.
        sys.stderr.write(
            "Your display is too small to run Menuconfig!\n"
            "It must be at least {} lines by {} columns.\n".format(
                WINDOW_HEIGHT_MIN, WINDOW_WIDTH_MIN))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "Kconfig"))
