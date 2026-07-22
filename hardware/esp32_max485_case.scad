// =============================================================
//  Case for ESP32 DevKit + MAX485 (RS-485) module
// -------------------------------------------------------------
//  Parametric two-part enclosure (base + snap-fit lid, no screws).
//  Boards have NO mounting holes -> each sits on a raised GLUE PAD
//  (a small solid platform). The pad is inset from the board edges
//  so the downward header pins hang off the sides into a clearance
//  gap; you glue the PCB flat onto the pad's top. Cut-outs for:
//    - ESP32 micro-USB port (one short wall, at PCB-top level)
//    - MAX485 flexible output cable (slot in the opposite short wall)
//    - Ventilation slots in the lid
//
//  Units: millimetres. Print with 0.2mm layers, no supports.
//
//  ***  MEASURE YOUR OWN BOARDS AND ADJUST THE VARIABLES  ***
//  Defaults are for a 38-pin ESP32 DevKit V1 (~52 x 28 mm)
//  and a common MAX485 TTL->RS485 breakout (~44 x 14 mm).
// =============================================================

/* [Render selection] */
part = "both";      // "base", "lid", or "both" (both = exploded preview)

/* [Global] */
wall        = 2.0;   // outer wall thickness
floor_th    = 2.0;   // base floor thickness
lid_th      = 2.0;   // lid top thickness
gap         = 0.4;   // clearance around boards
corner_r    = 3.0;   // outer corner radius
$fn         = 48;

/* [Glue pads] */
// Pins hang ~8-9mm BELOW each board and dangle beside the pad, so the
// pad (and thus the clearance under the board) must be taller than that.
mount_h     = 11.0;  // pad height = board clearance above floor (clears the 8-9mm downward pins + a little for dupont housings)
esp_pad_ix  = 3.0;   // ESP32 pad inset at the short ends (X)
esp_pad_iy  = 5.0;   // ESP32 pad inset at the long edges (Y) -> clears the long pin rows
max_pad_iy     = 1.5;  // MAX485 pad inset at the long edges (Y)
max_pad_len    = 29.0; // MAX485 pad length along X (3mm shorter than before)
max_pad_end_ix = 1.5;  // MAX485 pad clearance from the far (high-X) end; smaller = pad sits further toward that end, away from the USB/cable

/* [Internal cavity height] */
// Boards ride 11mm up on the pads; leave room above for the ESP module,
// the downward pins below, and the snap lip.
cavity_h    = 24.5;  // clear space above the floor for boards + headers

/* [ESP32 board] */
// 38-pin DevKit V1, micro-USB. This length drives the internal case
// length (interior = esp_l + 2*gap).
esp_l       = 48.5;  // length  (along X)   trimmed 1.5mm to shorten the interior
esp_w       = 25.0;  // width   (along Y)   pins on the long (X) edges
// USB sits on the short edge, right on top of the PCB.
esp_usb_w   = 9.0;   // micro-USB opening width  (port body ~8mm + clearance)
esp_usb_h   = 5.5;   // micro-USB opening height
esp_usb_z   = 0.5;   // opening bottom relative to PCB-top

/* [MAX485 board] */
// Output is a flexible cable (~5mm), not a screw terminal -> just a cable slot.
max_l       = 44.0;  // length (along X)
max_w       = 15.0;  // width  (along Y)   pins on the short (Y) ends
max_term_w  = 8.0;   // cable-slot width  (for a ~5mm flexible cable + clearance)
max_term_h  = 6.0;   // cable-slot height

/* [Layout] */
// esp_w (25) + board_gap (0) + max_w (15) = 40 mm total width
board_gap   = 0.0;   // gap between the two boards (Y direction)

// ---------- derived interior size ----------
inner_w = esp_w + board_gap + max_w + 2*gap;      // Y
inner_l = max(esp_l, max_l) + 2*gap;              // X
inner_h = cavity_h;

outer_l = inner_l + 2*wall;
outer_w = inner_w + 2*wall;
base_h  = floor_th + inner_h;

// Board Y positions (measured from inner front wall, y=0 at inner face)
esp_y0  = gap;                       // ESP32 nearest the "front"
max_y0  = gap + esp_w + board_gap;   // MAX485 behind it

// =============================================================
//  Helpers
// =============================================================
module rrect(l, w, r) {
    hull() for (x=[r, l-r], y=[r, w-r]) translate([x,y]) circle(r);
}

// Raised cylindrical posts to glue a board onto. Inset from the board
// footprint so the header pins hang off the edges into a clearance gap.
// FOUR small posts per board leave open channels between them -- both along
// X and across Y -- for wires to route underneath the board. Post radius is
// fixed (post_r), independent of the footprint depth, so the posts stay
// small. The two X columns sit either near the footprint ENDS (default) or
// clustered near the CENTER (x_center=true) -- centre works better for the
// MAX485, whose pins/cable exit the short (X) ends, keeping those clear.
post_r      = 2.0;   // radius of each glue post (smaller = more room for wires)
post_ctr_dx = 6.0;   // centre-mode: half the X spacing between the two post columns
module glue_pad(x0, y0, l, w, ix, iy, x_center=false) {
    fw = l - 2*ix;   // available footprint length (X)
    fd = w - 2*iy;   // available footprint depth  (Y)
    r  = min(post_r, fd/2);   // never wider than the footprint depth
    xmid = x0 + ix + fw/2;
    xs = x_center ? [xmid - post_ctr_dx, xmid + post_ctr_dx]
                  : [x0 + ix + r, x0 + ix + fw - r];
    for (xc = xs)
        for (yc = [y0 + iy + r, y0 + iy + fd - r])
            translate([xc, yc, floor_th])
                cylinder(r = r, h = mount_h);
}

// =============================================================
//  BASE
// =============================================================
module base() {
    difference() {
        union() {
            // outer shell
            linear_extrude(base_h)
                rrect(outer_l, outer_w, corner_r);
        }
        // hollow interior
        translate([wall, wall, floor_th])
            linear_extrude(inner_h + 1)
                rrect(inner_l, inner_w, max(0.1, corner_r-wall));

        // ---- ESP32 USB cut-out (front wall, -Y side, low X end) ----
        // opening centred on the ESP32 board, on the short wall at x = 0
        translate([-1,
                   wall + esp_y0 + esp_w/2 - esp_usb_w/2,
                   floor_th + mount_h + esp_usb_z])
            cube([wall+2, esp_usb_w, esp_usb_h]);

        // ---- MAX485 cable slot (SAME short wall as the USB, low X) ----
        translate([-1,
                   wall + max_y0 + max_w/2 - max_term_w/2,
                   floor_th + mount_h])
            cube([wall+2, max_term_w, max_term_h]);

        // ---- pry notch: bite out of the rim on the connector-free short
        //      wall (high X) so a fingernail/tool can lever the lid open ----
        translate([outer_l - wall - 1,
                   outer_w/2 - pry_w/2,
                   base_h - pry_depth])
            cube([wall + 2, pry_w, pry_depth + 1]);
    }

    // glue pads (one solid platform per board)
    glue_pad(wall + gap, wall + esp_y0, esp_l, esp_w, esp_pad_ix, esp_pad_iy);
    // TTL pad: shorter and shifted toward the far (high-X) end, away from the cable exit.
    // Four small posts clustered near the CENTER, so the short (X) ends stay
    // clear for the header pins / cable to exit.
    glue_pad(wall + gap + (max_l - max_pad_end_ix - max_pad_len),
             wall + max_y0, max_pad_len, max_w, 0, max_pad_iy, x_center=true);

    // snap catches on the inner walls that the lid skirt clicks under
    base_snaps();
}

/* [Snap-fit lid] */
snap_r        = 0.7;   // ridge radius = how far the catch protrudes (bigger = firmer, harder to open)
snap_from_top = 3.5;   // base catch height below the rim
snap_drop     = 1.6;   // how far below the base catch the lid catch seats (the "bite")
snap_len      = 14.0;  // length of each catch bar
lip_depth     = 6.5;   // how deep the lid skirt reaches into the base
skirt_t       = 1.6;   // lid skirt wall thickness (thin so it can flex to snap)
pry_w         = 12.0;  // width of the pry notch in the rim
pry_depth     = 1.0;   // how far the pry notch bites down from the rim (fingernail gap)

// Rounded catch bar running along X, centred at (xc, y_face, zc).
module snap_bar(xc, y_face, zc) {
    translate([xc - snap_len/2, y_face, zc])
        rotate([0, 90, 0]) cylinder(r=snap_r, h=snap_len, $fn=24);
}

// Two catches on each long inner wall (front & back).
module base_snaps() {
    zc = base_h - snap_from_top;
    for (xc = [outer_l*0.30, outer_l*0.70]) {
        snap_bar(xc, wall,            zc);   // front wall (inner face)
        snap_bar(xc, outer_w - wall,  zc);   // back wall  (inner face)
    }
}

// =============================================================
//  LID
// =============================================================
// skirt outer footprint (drops into the base with `gap` clearance to the walls)
skirt_ox = wall + gap;
skirt_oy = wall + gap;
skirt_ol = inner_l - 2*gap;
skirt_ow = inner_w - 2*gap;

module lid() {
    // outer faces of the skirt, where the catch ridges live
    front_face = skirt_oy;                 // -Y skirt face
    back_face  = skirt_oy + skirt_ow;      // +Y skirt face
    // lid catch height (local z): base catch is at world base_h - snap_from_top,
    // rim contact plane (local z=0) is world base_h, and the lid catch seats
    // snap_drop below the base catch so it is trapped under it.
    zc = -(snap_from_top + snap_drop);

    difference() {
        union() {
            // top plate
            linear_extrude(lid_th)
                rrect(outer_l, outer_w, corner_r);
            // flexing skirt (thin hollow ring)
            translate([0, 0, -lip_depth])
                linear_extrude(lip_depth)
                    difference() {
                        translate([skirt_ox, skirt_oy])
                            rrect(skirt_ol, skirt_ow, max(0.1, corner_r-wall-gap));
                        translate([skirt_ox+skirt_t, skirt_oy+skirt_t])
                            rrect(skirt_ol-2*skirt_t, skirt_ow-2*skirt_t,
                                  max(0.1, corner_r-wall-gap-skirt_t));
                    }
            // catch ridges on the skirt outer faces (snap under the base catches)
            for (xc = [outer_l*0.30, outer_l*0.70]) {
                snap_bar(xc, front_face, zc);
                snap_bar(xc, back_face,  zc);
            }
        }
        // ventilation slots in the top
        for (i=[0:5])
            translate([outer_l*0.25, outer_w*0.5 - 15 + i*6, -1])
                cube([outer_l*0.5, 2, lid_th+2]);
    }
}

// =============================================================
//  Assembly / render
// =============================================================
if (part == "base") base();
else if (part == "lid") lid();
else {
    base();
    // show lid floating above, flipped, for preview
    translate([0, outer_w, base_h + 18])
        rotate([180,0,0]) lid();
}
