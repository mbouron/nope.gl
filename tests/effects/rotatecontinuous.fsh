#define rotateSpeed -0.035 // Rotation speed, in radians
#define scaleFactor 1.2 // fixed scale of media
  
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
  float alpha = smoothstep(uv.x, uv.x + aa, aa);
  alpha = max(alpha, smoothstep(1.0 - uv.x, 1.0 - uv.x + aa, aa));
  alpha = max(alpha, smoothstep(uv.y, uv.y + aa, aa));
  alpha = max(alpha, smoothstep(1.0 - uv.y, 1.0 - uv.y + aa, aa));
  return 1.0 - alpha;
}

void main() {
  float rotateStart = -0.5 * rotateSpeed * i_duration;
  float rotate = rotateStart + i_time * rotateSpeed;
  vec2 uv = v_tex_coord;
  float aspect = i_media_size.x / i_media_size.y;
    
  //vec4 color_ori = texture2D(u_texture, uv);
  
  uv = transformUV(uv, aspect, vec2(0.5), vec2(0.0), vec2(scaleFactor), rotate);
  //float aa = 1.0 / i_size.x;
  //float alpha = getUvCropAlpha(color_ori, uv, aa);
  
  vec4 color = texture2D(u_texture, uv);
  //color = mix(vec4(0.0), color_ori, alpha);
  gl_FragColor = color;
}
