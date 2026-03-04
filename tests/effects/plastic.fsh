#define pi 3.141592653589793

#define c_displayNormal false
#define c_segmentsNumber 10.
#define c_seed 0.
#define c_specular 0.1
#define c_refractionIOR 0.003
#define c_reflectionIOR 0.05
#define c_reflectionIntensity 0.05
#define c_sideGlowSize 0.02
#define c_sideGlowIntensity 0.1
#define c_lightAnimationSpeed 1.
#define c_heightAnimationSpeed 2.
#define c_introAnimationDelay 0.
#define c_introAnimationSpeed 10.

float random(float n){return fract(sin(n) * 43758.5453123);}

float line (vec2 position, vec2 offset, float rotation, float curve, float spread, float height) {
  float s = sin(rotation + position.y * pi * curve);
    float c = cos(rotation + position.y * pi * curve);
    mat2 rotationMatrix = mat2(c, -s, s, c);
  position -= offset;
  position = rotationMatrix * position;
   
  float x = position.x * .3;
  x *= 1. + smoothstep(0., 1., position.y * .5 + .5) * 5.;
  float value = smoothstep(0., 1., 1. - abs(x * 3.)) * (cos(x * 30.) * .5 + .5);
  value *= smoothstep(0., 1., 1. - abs(position.y));
  // value = pow(value, 1. / spread);
  return value * height;
}

float merge(float d1, float d2) {
  return mix(d1, d2, d2);
}

float computeHeight(vec2 coord, float time, float play) {
  float height = 0.;
  for (float i = 0.; i <= 100.; i++) {
    if(i >= c_segmentsNumber) {
      break;
    }
    float seed = i + c_seed;
    vec2 position = vec2(random(seed), random(seed + 1.)) * 2. - 1.;
    float rotation = (random(seed * 3. + 2.) * 2. - 1.) * pi;
    float introAnimationRatio = smoothstep(0., 1., time - c_introAnimationDelay - seed / c_introAnimationSpeed * .1);
    float animationRatio = cos(time * c_heightAnimationSpeed + seed) * .5 + 1.;
    animationRatio *= introAnimationRatio;
    animationRatio = mix(1., animationRatio, play);
    height = merge(height, line(coord.xy, position, rotation, .05, .1, 1.) * animationRatio);
  }
  return height / pow(c_segmentsNumber, .7);
}

vec4 bumpFromDepth(vec2 position, vec2 resolution, float scale, float time, float play) {
  vec2 step = 1. / resolution;
    
  float depth = computeHeight(position, time, play);
    
  vec2 dxy = depth - vec2(
      computeHeight(position + vec2(step.x, 0.), time, play),
      computeHeight(position + vec2(0., step.y), time, play)
  );
    
  return vec4(depth, normalize(vec3(dxy * scale / step, 1.)));
}

void main() {
  vec3 position = vec3(v_tex_coord * 2. - 1., 0.);
  position.x *= u_size.x / u_size.y;

  float s_time = i_play * u_time;

  highp vec4 bump = bumpFromDepth(position.xy, vec2(1024.), 1., s_time, i_play);
  highp vec3 normal = bump.yzw;

  vec3 color = texture2D(u_texture, v_tex_coord + normal.xy * c_refractionIOR).rgb;
  vec3 reflexion = texture2D(u_texture, v_tex_coord + normal.xy * c_reflectionIOR).rgb;
  color = mix(color, reflexion, c_reflectionIntensity);

  float lightRatio = dot(normalize(vec3(cos(s_time * c_lightAnimationSpeed), sin(s_time * c_lightAnimationSpeed) * .5, 1.)), normal);
  color += (lightRatio * .5 + .5) * c_specular;

  float glow = smoothstep(.74 - c_sideGlowSize, .75 - c_sideGlowSize, lightRatio);
  glow *= smoothstep(.24 - c_sideGlowSize, .25 - c_sideGlowSize, 1. - lightRatio);
  color += glow * abs(normal.y + normal.x) * c_sideGlowIntensity;

  gl_FragColor = vec4(color, 1.0);
  if(c_displayNormal) {
    gl_FragColor = vec4(normal, 1.0);
  }
}
