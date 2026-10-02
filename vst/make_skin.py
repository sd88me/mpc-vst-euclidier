#!/usr/bin/env python3
"""Generate Euclidier's skin inputs: params.json, layout.conf and images/ (needs Pillow).

The step displays (8 lane rows, one circle) must resize with the lane's step count, but a skin is a fixed set of
components. So each display is built from "size classes": for every class capacity `cap` there are `cap` cell
parameters (hidden / off / on) and one ring parameter (the playhead position). The plugin (euclidier_vst.cpp)
polls each lane's `l<N>_pattern` from the engine and shows only the class that fits the lane's step count.

  g<lane>_<cap>_<slot>   lane row cell      (lane 1..8)         options: hidden, off, on, off+playhead, on+playhead
  c<cap>_<slot>          circle cell        (selected lane)     same options
The playhead is a cell state, not an overlay: MPC crops each picture to an opaque rectangle, so a ring image
laid over the cells hid them.

euclidier_vst.cpp parses these key names at startup, so keep the names in step with it.
Run:  docker run --rm -u $(id -u):$(id -g) -v $PWD:/w -w /w mpc-vst-html-art python3 make_skin.py
"""
import json
import math
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
IMG = os.path.join(HERE, "images")
os.makedirs(IMG, exist_ok=True)

CELL_OPTS = ["-", "OFF", "ON", "OFF >", "ON >"]
CELL_FILES = ["off", "on", "cur_off", "cur_on"]


def grid_opts(cap):
    return CELL_OPTS if cap <= GRID_CUR_MAX else CELL_OPTS[:3]
MAX_STEPS = 32   # the whole design is capped at 32 steps (the engine allows 64): fewer components, faster page changes
GRID_CAPS = [8, 16, 32]
CIRCLE_CAPS = [8, 12, 16, 24, 32]
GRID_CUR_MAX = 16   # lane-row classes up to this size show the play-head (a cell state); the 32-step row doesn't

# colours (match layout.conf's theme)
OFF, ON, CUR = (14, 17, 14), (230, 245, 200), (255, 255, 255)
OUTLINE = (60, 66, 58)

# lane rows (layout coords: skin y + 86)
ROW0, ROW_PITCH, CELL_H = 212, 62, 44
GX0, GW = 175, 820
# circle
CCX, CCY, CR = 331, 452, 148
CIRC_BOX = 344


def save(img, name):
    img.save(os.path.join(IMG, name))


def cell_img(w, h, color):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, w - 1, h - 1], fill=color + (255,), outline=OUTLINE + (255,))
    return im


def grid_geometry(cap):
    pitch = GW / cap
    cw = max(4, int(round(pitch)) - 3)
    return pitch, cw


def circle_dot_r(cap):
    return int(max(4, min(15, CR * math.sin(math.pi / cap) * 0.72)))


def circle_pos(cap, slot):
    a = -math.pi / 2 + 2 * math.pi * slot / cap
    return CCX + CR * math.cos(a), CCY + CR * math.sin(a)


def cur_cell_img(w, h, color):
    im = cell_img(w, h, color)
    ImageDraw.Draw(im).rectangle([0, 0, w - 1, h - 1], outline=CUR + (255,), width=4)
    return im


def make_images():
    for cap in GRID_CAPS:
        pitch, cw = grid_geometry(cap)
        save(cell_img(cw, CELL_H, OFF), "cell_%d_off.png" % cap)
        save(cell_img(cw, CELL_H, ON), "cell_%d_on.png" % cap)
        if cap <= GRID_CUR_MAX:
            save(cur_cell_img(cw, CELL_H, OFF), "cell_%d_cur_off.png" % cap)
            save(cur_cell_img(cw, CELL_H, ON), "cell_%d_cur_on.png" % cap)
    for cap in CIRCLE_CAPS:
        r = circle_dot_r(cap)
        for name, col in (("off", OFF), ("on", ON)):
            for cur in (False, True):
                im = Image.new("RGBA", (2 * r + 2, 2 * r + 2), (0, 0, 0, 0))
                d = ImageDraw.Draw(im)
                d.ellipse([0, 0, 2 * r, 2 * r], fill=col + (255,), outline=(CUR if cur else OUTLINE) + (255,), width=3 if cur else 1)
                save(im, "dot_%d_%s%s.png" % (cap, "cur_" if cur else "", name))
    for size in (44, 84):   # on/off toggles: lime lamp / dark lamp
        for name, face, ring in (("off", (44, 49, 44), (74, 79, 72)), ("on", (143, 217, 74), (185, 236, 124))):
            big = Image.new("RGBA", (size * 4, size * 4), (0, 0, 0, 0))
            d = ImageDraw.Draw(big)
            d.ellipse([4, 4, size * 4 - 5, size * 4 - 5], fill=ring + (255,))
            inset = size * 4 // 9
            d.ellipse([inset, inset, size * 4 - inset - 1, size * 4 - inset - 1], fill=face + (255,))
            save(big.resize((size, size), Image.LANCZOS), "lamp%d_%s.png" % (size, name))
    im = Image.new("RGBA", (CIRC_BOX, CIRC_BOX), (0, 0, 0, 0))   # guide circle
    c = CIRC_BOX // 2
    ImageDraw.Draw(im).ellipse([c - CR, c - CR, c + CR, c + CR], outline=(70, 76, 68, 255), width=2)
    save(im, "circle_guide.png")


def make_params():
    base = json.load(open(os.path.join(HERE, "module.json")))["chain_params"]
    params = [dict(p) for p in base]
    for p in params:   # the row label under each lane toggle is the parameter's name
        if p["key"].endswith("_enable") and p["key"][1:2].isdigit():
            p["name"] = "Lane " + p["key"][1]
        if p["key"].endswith("_ch"):
            p["name"] = "MID CH."
        if p["key"].rsplit("_", 1)[-1] in ("steps", "fill", "shift", "loop") and "max" in p:
            p["max"] = min(p["max"], MAX_STEPS)
    lane = {p["key"][3:]: p for p in base if p["key"].startswith("l1_")}
    params.append({"key": "sel", "name": "Lane", "options": ["LANE %d" % i for i in range(1, 9)]})
    for k, p in lane.items():   # the selected lane's own copy of every lane parameter
        q = dict(p)
        if k == "ch":
            q["name"] = "MID CH."
        if k in ("steps", "fill", "shift", "loop"):
            q["max"] = min(q["max"], MAX_STEPS)
        q["key"] = "sel_" + k
        params.append(q)
    for n in range(1, 9):   # the randomise lane picker: the engine's rand_l<N> flags (multi-select)
        params.append({"key": "rand_l%d" % n, "name": "Lane %d" % n, "options": ["OFF", "ON"]})
    params.append({"key": "all_drum", "name": "All Drum", "momentary": True})   # every lane to DRUM mode (handled in the plugin)
    for k, tgt, d in (("sel_prev", "sel", -1), ("sel_next", "sel", 1), ("sel_div_prev", "sel_div", -1), ("sel_div_next", "sel_div", 1)):
        params.append({"key": k, "name": k, "momentary": True, "step_of": tgt, "step_delta": d})
    for n in range(1, 9):
        params.append({"key": "l%d_info" % n, "name": "L%d Info" % n, "display": "string", "min": 0, "max": 1, "default": 0})
    for n in range(1, 9):
        for cap in GRID_CAPS:
            for s in range(cap):
                params.append({"key": "g%d_%d_%d" % (n, cap, s), "name": "G%d.%d.%d" % (n, cap, s), "options": grid_opts(cap), "default": 0})
    for cap in CIRCLE_CAPS:
        for s in range(cap):
            params.append({"key": "c%d_%d" % (cap, s), "name": "C%d.%d" % (cap, s), "options": CELL_OPTS, "default": 0})
    json.dump({"name": "Euclidier", "params": params}, open(os.path.join(HERE, "params.json"), "w"), indent=1)
    return len(params)


def layout():
    o = []
    a = o.append
    theme = open(os.path.join(HERE, "layout.theme")).read().rstrip()
    a(theme)
    a("")
    # ---- DETAIL
    a("[tab MAIN]")
    a('text cx=44 cy=118 label="EUCLIDIER" align=left weight=700 size=3 spacing=3')
    a('frame x=36 y=150 w=590 h=548 title="PATTERN"')
    a('stepper cx=331 cy=214 w=320 h=44 label="" key=sel prev=sel_prev next=sel_next label_align=center')
    a('art file="images/circle_guide.png" x=%d y=%d w=%d h=%d fit=stretch' % (CCX - CIRC_BOX // 2, CCY - CIRC_BOX // 2, CIRC_BOX, CIRC_BOX))
    for cap in CIRCLE_CAPS:
        r = circle_dot_r(cap)
        for s in range(cap):
            x, y = circle_pos(cap, s)
            a('picture x=%d y=%d w=%d h=%d key=c%d_%d files=",%s"'
              % (round(x - r), round(y - r), 2 * r + 2, 2 * r + 2, cap, s, ",".join("images/dot_%d_%s.png" % (cap, k) for k in CELL_FILES)))
    a('frame x=646 y=150 w=597 h=548 title="LANE PARAMETERS"')
    cols, rows = (760, 945, 1130), (230, 363, 496)
    knobs = ["steps", "fill", "shift", "loop", "gate", "vel", "velh", "note", "ch"]
    names = ["STEPS", "FILL", "SHIFT", "LOOP", "GATE", "VELOCITY", "HUMANIZE", "NOTE", "MID CH."]
    for i, (k, nm) in enumerate(zip(knobs, names)):
        a('knob cx=%d cy=%d r=34 label="%s" key=sel_%s' % (cols[i % 3], rows[i // 3], nm, k))
    a('stepper cx=800 cy=628 w=260 h=44 label="DIV" key=sel_div prev=sel_div_prev next=sel_div_next label_align=center')
    a('enum_h cx=1040 cy=628 label="MODE" key=sel_mode sw=80')
    a('toggle cx=1190 cy=628 label="ON" key=sel_enable img=images/lamp44_off.png img_on=images/lamp44_on.png')
    a('qlinks "MAIN" = sel_steps,sel_fill,sel_shift,sel_loop,sel_gate,sel_vel,sel_note,sel_ch')
    a("")
    # ---- LANES
    a("[tab LANES]")
    a('text cx=44 cy=118 label="EUCLIDIER" align=left weight=700 size=3 spacing=3')
    a('button cx=1130 cy=118 label="ALL LANES DRUM" key=all_drum')
    a('frame x=36 y=150 w=1207 h=548 title="LANES"')
    for n in range(1, 9):
        cy = ROW0 + (n - 1) * ROW_PITCH
        a('toggle cx=98 cy=%d label="%d" key=l%d_enable img=images/lamp44_off.png img_on=images/lamp44_on.png' % (cy, n, n))
        for cap in GRID_CAPS:
            pitch, cw = grid_geometry(cap)
            for s in range(cap):
                x = GX0 + int(round(s * pitch))
                a('picture x=%d y=%d w=%d h=%d key=g%d_%d_%d files=",%s"'
                  % (x, cy - CELL_H // 2, cw, CELL_H, n, cap, s, ",".join("images/cell_%d_%s.png" % (cap, k) for k in CELL_FILES[:len(grid_opts(cap)) - 1])))
        a('readout cx=1135 cy=%d w=190 h=40 label="" key=l%d_info label_align=center' % (cy, n))
    a('qlinks "LANES" = ' + ",".join("l%d_enable" % n for n in range(1, 9)))
    a("")
    # ---- RANDOMISE
    a("[tab RANDOMISE]")
    a('text cx=44 cy=118 label="EUCLIDIER" align=left weight=700 size=3 spacing=3')
    a('frame x=36 y=150 w=1207 h=548 title="RANDOMISE"')
    a('text cx=640 cy=225 label="PICK THE LANES TO RANDOMISE (NONE PICKED = ALL)" align=center weight=600 size=1.6 spacing=2')
    for n in range(1, 9):
        a('toggle cx=%d cy=330 label="%d" key=rand_l%d img=images/lamp84_off.png img_on=images/lamp84_on.png' % (130 + (n - 1) * 145, n, n))
    a('button cx=640 cy=520 label="RANDOMISE" key=rand_go')
    a('qlinks "RANDOMISE" = ' + ",".join("rand_l%d" % n for n in range(1, 9)) + "")
    return "\n".join(o) + "\n"


if __name__ == "__main__":
    make_images()
    n = make_params()
    open(os.path.join(HERE, "layout.conf"), "w").write(layout())
    print("params:", n, "images:", len(os.listdir(IMG)))
