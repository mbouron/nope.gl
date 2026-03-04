#define angle 0.2 // Angle between first and last iteration, in radians
#define scaleFactor 0.4 // Scale between first and last iteration
#define count 6 // number of iterations

vec2 transformUV(vec2 uv, float aspect, vec2 center, vec2 translate, vec2 scale, float rotate) {
  vec3 st = vec3(uv, 1.0);

  mat3 restoreAspectMatrix = mat3(
    1.0 / aspect, 0, 0,
    0, 0, 0,
    0, 0, 0
  );

  mat3 applyOriginMatrix = mat3(
    1, 0, -center.x,
    0, 1, -center.y,
    0, 0, 1
  );
  mat3 restoreOriginMatrix = mat3(
    1, 0, center.x,
    0, 1, center.y,
    0, 0, 1
  );

  mat3 translateMatrix = mat3(
    1, 0, -translate.x,
    0, 1, -translate.y,
    0, 0, 1
  );

  mat3 rotateMatrix = mat3(
    cos(rotate), sin(rotate), 0,
    -sin(rotate), cos(rotate), 0,
    0, 0, 1);

  mat3 scaleMatrix = mat3(
    1.0 / scale.x, 0, 0,
    0, 1.0 / scale.y, 0,
    0, 0, 1
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
  float alpha = smoothstep(uv.x, uv.x + aa, aa);
  alpha = max(alpha, smoothstep(1.0 - uv.x, 1.0 - uv.x + aa, aa));
  alpha = max(alpha, smoothstep(uv.y, uv.y + aa, aa));
  alpha = max(alpha, smoothstep(1.0 - uv.y, 1.0 - uv.y + aa, aa));
  return 1.0 - alpha;
}
//vec2 uvFromTextureSize(vec2 textureSize) {
//  vec2 uv = v_tex_coord * 2. - 1.;
//  uv.x *= min(1., (u_size.x * i_texture_size_1.y) / (u_size.y * i_texture_size_1.x));
//  uv.y *= min(1., (u_size.y * i_texture_size_1.x) / (u_size.x * i_texture_size_1.y));
//  uv = uv * .5 + .5;
//  return uv;
//}

vec4 layer(vec4 foreground, vec4 background) {
  return foreground * foreground.a + background * (1.0 - foreground.a);
}

float map(float value, float inMin, float inMax, float outMin, float outMax) {
  return outMin + (outMax - outMin) * (value - inMin) / (inMax - inMin);
}

float cmap(float value, float inMin, float inMax, float outMin, float outMax) {
  return clamp(outMin + (outMax - outMin) * (value - inMin) / (inMax - inMin), outMin, outMax);
}

float exponentialInOut(float t) {
  return t == 0.0 || t == 1.0
    ? t
    : t < 0.5
      ? +0.5 * pow(2.0, (20.0 * t) - 10.0)
      : -0.5 * pow(2.0, 10.0 - (t * 20.0)) + 1.0;
}

float exponentialIn(float t) {
  return t == 0.0 ? t : pow(2.0, 10.0 * (t - 1.0));
}

float exponentialOut(float t) {
  return t == 1.0 ? t : 1.0 - pow(2.0, -10.0 * t);
}

float qinticOut(float t) {
  float tt = 1.0 - t;
  return 1.0 - tt * tt * tt * tt * tt;
}

float qinticInOut(float t) {
  return t < 0.5
    ? +16.0 * pow(t, 5.0)
    : 0.5 * pow(2.0 * t - 2.0, 5.0) + 1.0;
}

float quadraticOut(float t) {
  return -t * (t - 2.0);
}

float quadraticInOut(float t) {
  float p = 2.0 * t * t;
  return t < 0.5 ? p : -p + (4.0 * t) - 1.0;
}

void main() {
  vec2 uv = v_tex_coord;

  float aspect = i_media_size.x / i_media_size.y;
  
  float introRatio = 0.7; // 70% of duration spent on intro
  float progress = clamp(mod(i_time, i_duration) / i_duration, 0.0, 1.0);
  float progressIn = qinticInOut(cmap(progress, 0.0, introRatio, 0.0, 1.0));
  float progressOut = qinticOut(1.0 - cmap(progress, introRatio, 1.0, 0.0, 1.0));
  progress = mix(progressIn, progressOut, step(introRatio, progress));

  vec4 color = vec4(0.0);
  for (int i = 0; i < count; i++) {
    float slice = progress * float(i) / float(count - 1);
    float rotate = mix(0.0, angle, slice);
    vec2 scale = vec2(mix(1.0, scaleFactor, slice));
    vec2 st = transformUV(uv, aspect, vec2(0.5), vec2(0.0), scale, rotate);
    vec4 tex = texture2D(u_texture, st);
    float alpha = getUvCropAlpha(tex, st, 0.0);
    tex = mix(vec4(0.0), tex, alpha);
    color = layer(tex, color);
  }

    gl_FragColor = vec4(color.rgb, 1.0);
}
