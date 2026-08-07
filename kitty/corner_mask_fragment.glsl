// Fork-specific, used for the Wayland native titlebar tabs mode.
// Two modes, selected by border_color.a:
//  - border_color.a == 0: multiplies the framebuffer's premultiplied pixels
//    by the coverage of a circle, cutting rounded bottom corners.
//    rect = (center.x, center.y, radius, unused) in framebuffer coordinates.
//    Draw with glBlendFunc(GL_ZERO, GL_SRC_ALPHA).
//  - border_color.a > 0: emits border_color (premultiplied) scaled by the
//    coverage of a stroke band [radius - rect.w, radius] along the circle,
//    or solid when radius == 0. Used for the macOS-style light inner window
//    border. Draw with glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA).

uniform vec4 rect;
uniform vec4 border_color;
out vec4 output_color;

void main() {
    if (border_color.a == 0.0) {
        float dist = distance(gl_FragCoord.xy, rect.xy);
        float coverage = 1.0 - smoothstep(rect.z - 1.0, rect.z, dist);
        output_color = vec4(0.0, 0.0, 0.0, coverage);
    } else {
        float coverage = 1.0;
        if (rect.z > 0.0) {
            float dist = distance(gl_FragCoord.xy, rect.xy);
            coverage = smoothstep(rect.z - rect.w - 1.0, rect.z - rect.w, dist) * (1.0 - smoothstep(rect.z - 1.0, rect.z, dist));
        }
        output_color = border_color * coverage;
    }
}
