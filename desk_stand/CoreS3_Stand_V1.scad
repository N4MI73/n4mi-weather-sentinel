// CoreS3 SE + M5GO-BOTTOM 3 desktop stand, V1. Units: mm.
// Print base-down. Angle is REARWARD FROM VERTICAL (screen 63 deg to desk).
// The assembled 23 mm measurement controls fit, not summed module specs.
device_width = 54;
device_height = 54;
device_thickness = 23;
thickness_clearance = 1.0; // TOTAL: cradle depth = 24 mm
view_angle = 27;
base_width = 76;
base_depth = 84;
base_thickness = 4;
corner_chamfer = 4;
rail_width = 7;
rail_spacing = 36; // center-to-center; supports lie inside device sides
frame_wall = 5;
support_height = 42; // along back of device, leaves top open
rear_extension = 8;
front_y = 12;
seat_lift = 4;
tab_height = 4;
tab_thickness = 3;
rubber_feet = false;
foot_diameter = 8;
foot_recess_depth = 1;
show_device = false; // preview only; excluded from STL via % modifier
$fn = 64;

depth = device_thickness + thickness_clearance;
origin_z = base_thickness + seat_lift + depth*sin(view_angle);
function p(n,u) = [front_y+n*cos(view_angle)+u*sin(view_angle),
                  origin_z-n*sin(view_angle)+u*cos(view_angle)];
a=p(depth,-1); // overlap lower seat to avoid point-only connections
b=p(depth,support_height);
c=[b[0]+rear_extension,base_thickness-0.2];
triangle=[a,b,c];
seat=[p(0,0),p(depth+0.5,0),[p(depth+0.5,0)[0],base_thickness-0.2],
      [front_y,base_thickness-0.2]];
tabs=[p(-tab_thickness,tab_height),p(0,tab_height),p(0,0),
      [front_y,base_thickness-0.2],
      [p(-tab_thickness,0)[0],base_thickness-0.2],p(-tab_thickness,0)];

assert(view_angle>=10 && view_angle<=40, "Use a 10-40 degree tilt for V1.");
assert(base_width>device_width && rail_spacing+rail_width<device_width);
assert(base_depth>c[0]+5, "Increase base_depth for this angle/height.");
assert(foot_recess_depth<base_thickness && thickness_clearance>=0);

module side_extrude(x) {
    // Local 2D coordinates are [y,z]; extrusion is along global x.
    translate([x-rail_width/2,0,0])
        multmatrix([[0,0,1,0],[1,0,0,0],[0,1,0,0],[0,0,0,1]])
            linear_extrude(height=rail_width) children();
}
module rear_brace() {
    translate([-(rail_spacing+rail_width-1)/2,0,0])
        multmatrix([[0,0,1,0],[1,0,0,0],[0,1,0,0],[0,0,0,1]])
            linear_extrude(height=rail_spacing+rail_width-1)
                polygon([p(depth+0.1,support_height-5),p(depth+2.8,support_height-5),
                         p(depth+2.8,support_height-1),p(depth+0.1,support_height-1)]);
}
module base() {
    w=base_width/2; d=base_depth; r=corner_chamfer;
    linear_extrude(height=base_thickness) difference() {
        polygon([[-w+r,0],[w-r,0],[w,r],[w,d-r],
                 [w-r,d],[-w+r,d],[-w,d-r],[-w,r]]);
        translate([-(rail_spacing-rail_width)/2,6])
            square([rail_spacing-rail_width,base_depth-12]);
    }
}
module stand() {
    difference() {
        union() {
            base();
            rear_brace(); // 29 mm clear bridge; inspect bridge paths in slicer
            for(x=[-rail_spacing/2,rail_spacing/2]) side_extrude(x) {
                polygon(seat);
                polygon(tabs);
                difference() {
                    polygon(triangle);
                    offset(delta=-frame_wall) polygon(triangle);
                }
            }
        }
        if(rubber_feet)
            for(x=[-base_width/2+10,base_width/2-10])
                for(y=[10,base_depth-10])
                    translate([x,y,-0.1])
                        cylinder(d=foot_diameter,h=foot_recess_depth+0.1);
    }
}
stand();
if(show_device) %color([0.25,0.45,0.65,0.35])
    translate([-device_width/2,front_y,origin_z])
        rotate([-view_angle,0,0])
            cube([device_width,device_thickness,device_height]);
