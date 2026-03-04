void main()
{
    vec2 uv = v_tex_coord;

    float t = i_time;

    vec2 uv_3y = vec2 (uv.x, (1.0 / 3.0) * fract(3.0 * uv.y) + 0.5);
    vec2 uv_2y = vec2 (uv.x, (1.0 / 2.0) * fract(2.0 * uv.y) + 0.25);
    vec2 uv_2x = vec2((1.0 / 2.0) * fract(2.0 * uv.x) + 0.25, uv.y);

    vec2 uv_double = clamp(2.0 * uv - vec2(0.5), 0.0, 1.0);

    float center_cut = step(0.25, uv.x) * step(0.25, uv.y) * step(uv.x, 0.75) * step(uv.y, 0.75);

    vec3 time_factor_1 = step(0.0, vec3(t)) * step(t, vec3(2.7));
    vec3 time_factor_2 = step(2.7, vec3(t)) * step(t, vec3(5.0));
    vec3 time_factor_3 = step(5.0, vec3(t)) * step(t, vec3(7.5));
    vec3 time_factor_4 = step(7.5, vec3(t)) * step(t, vec3(8.25));
    vec3 time_factor_5 = step(8.25, vec3(t)) * step(t, vec3(9.5));
    vec3 time_factor_6 = step(9.5, vec3(t)) * step(t, vec3(15.0));

    float white_frame = step(0.03, uv.x) * step(0.017, uv.y) * step(uv.x, 0.97) * step(uv.y, 0.983);

    vec3 color = time_factor_1 * center_cut * texture2D(u_texture, uv_double).rgb;
    color += time_factor_1 * (1.0 - center_cut) * texture2D(u_texture, uv).rgb;
    color += time_factor_2 * texture2D(u_texture, uv_3y).rgb;
    color += time_factor_3 * texture2D(u_texture, uv).rgb;
    color += time_factor_4 * texture2D(u_texture, uv_2x).rgb;
    color += time_factor_5 * texture2D(u_texture, uv_2y).rgb;
    color += time_factor_6 * texture2D(u_texture, uv).rgb;
    color += 1.0 - white_frame;

    gl_FragColor = vec4(color, 1.0);
}