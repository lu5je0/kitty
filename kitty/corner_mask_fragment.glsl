// Fork-specific: multiplies the framebuffer's premultiplied pixels by the
// coverage of a circle, used to cut rounded bottom corners for the Wayland
// native titlebar tabs mode. rect = (center.x, center.y, radius, unused) in
// framebuffer coordinates. Draw with glBlendFunc(GL_ZERO, GL_SRC_ALPHA).

uniform vec4 rect;
out vec4 output_color;

void main() {
    float dist = distance(gl_FragCoord.xy, rect.xy);
    float coverage = 1.0 - smoothstep(rect.z - 1.0, rect.z, dist);
    output_color = vec4(0.0, 0.0, 0.0, coverage);
}
