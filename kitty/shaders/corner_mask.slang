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

static const int2 vertex_pos_map[4] = {
    int2(0, 0),
    int2(1, 0),
    int2(1, 1),
    int2(0, 1),
};
static const float4 dest_rect = float4(-1, 1, 1, -1);

[shader("vertex")]
float4 vertex_main(uint vertex_id : SV_VertexID) : SV_Position {
    int2 pos = vertex_pos_map[vertex_id];
    return float4(dest_rect[pos.x], dest_rect[pos.y], 0, 1);
}

[shader("fragment")]
float4 fragment_main(
    uniform float4 rect,
    uniform float4 border_color,
    float4 frag_coord : SV_Position
) : SV_Target {
    float4 output_color;
    if (border_color.a == 0.0) {
        float dist = distance(frag_coord.xy, rect.xy);
        float coverage = 1.0 - smoothstep(rect.z - 1.0, rect.z, dist);
        output_color = float4(0.0, 0.0, 0.0, coverage);
    } else {
        float coverage = 1.0;
        if (rect.z > 0.0) {
            float dist = distance(frag_coord.xy, rect.xy);
            coverage = smoothstep(rect.z - rect.w - 1.0, rect.z - rect.w, dist) * (1.0 - smoothstep(rect.z - 1.0, rect.z, dist));
        }
        output_color = border_color * coverage;
    }
    return output_color;
}
