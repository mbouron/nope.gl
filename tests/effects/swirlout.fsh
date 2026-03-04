#define angle 0.2 // Angle between first and last iteration, in radians
#define scaleFactor 0.4 // Scale between first and last iteration
#define count 6 // number of iterations

vec2 transformUV(vec2 uv, float aspect, vec2 center, vec2 translate, vec2 scale, float rotate) {
  vec3 st = vec3(uv, 1.0);

  mat3 restoreAspectMatrix = mat3(
    1.0 / aspect, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0
  );

  mat3 applyOriginMatrix = mat3(
    1.0, 0.0, -center.x,
    0.0, 1.0, -center.y,
    0.0, 0.0, 1.0
  );
  mat3 restoreOriginMatrix = mat3(
    1.0, 0.0, center.x,
    0.0, 1.0, center.y,
    0.0, 0.0, 1.0
  );

  mat3 translateMatrix = mat3(
    1.0, 0.0, -translate.x,
    0.0, 1.0, -translate.y,
    0.0, 0.0, 1.0
  );

  mat3 rotateMatrix = mat3(
    cos(rotate), sin(rotate), 0.0,
    -sin(rotate), cos(rotate), 0.0,
    0.0, 0.0, 1.0);

  mat3 scaleMatrix = mat3(
    1.0 / scale.x, 0.0, 0.0,
    0.0, 1.0 / scale.y, 0.0,
    0.0, 0.0, 1.0
  );

  st = st * translateMatrix;

  // Rotation needs origin applied + aspect ratio corrected
  st = st * applyOriginMatrix;
  st.x *= aspect;
  st = st * rotateMatrix;
  st.x /= aspect;
  st = st * restoreOriginMatrix;

  // Scale needs origin applied
  st = st * applyOriginMatrix;
  st = st * scaleMatrix;
  st = st * restoreOriginMatrix;
  
  return st.xy;
}

float getUvCropAlpha(vec4 tex, vec2 uv, float aa) {
  float alpha = smoothstep(uv.x - aa, uv.x, aa);
  alpha = max(alpha, smoothstep(1.0 - uv.x - aa, 1.0 - uv.x, aa));
  alpha = max(alpha, smoothstep(uv.y - aa, uv.y, aa));
  alpha = max(alpha, smoothstep(1.0 - uv.y - aa, 1.0 - uv.y, aa));
  return 1.0 - alpha;
}

vec4 layer(vec4 foreground, vec4 background) {
  return foreground * foreground.a + background * (1.0 - foreground.a);
}

float qinticOut(float t) {
  float tt = 1.0 - t;
  return 1.0 - tt * tt * tt * tt * tt;
}

void main() {
    vec2 uv = v_tex_coord;
    float aspect = i_size.x / i_size.y;

    float progress = clamp(mod(i_time, i_duration) / i_duration, 0.0, 1.0);
    progress = 1.0 - qinticOut(progress);

    vec4 color = vec4(0.0);
    for (int i = 0; i < count; i++) {
        float slice = progress * float(i) / float(count - 1);
        float rotate = mix(0.0, angle, slice);
        vec2 scale = vec2(mix(1.0, scaleFactor, slice));
        vec2 st = transformUV(uv, aspect, vec2(0.5), vec2(0.0), scale, rotate);
        vec4 tex = texture2D(u_texture, st);
        float alpha = getUvCropAlpha(tex, st, 0.1 / i_size.x);
        tex = mix(vec4(0.0), tex, alpha);
        color = layer(tex, color);
  }

    gl_FragColor = vec4(color.rgb, 1.0);
}
