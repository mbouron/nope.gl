#define pi 3.141592653589793

#define c_displayMedia true
#define c_sideSpread 0.98
#define c_imageSize 1.0
#define c_imageRotation 0.0
#define c_noiseScale 3.0
#define c_noiseOffsetX 1.
#define c_noiseOffsetY 3.
#define c_maskSpread 0.05
#define c_lightPositionX -1.
#define c_lightPositionY 1.
#define c_lightIntensity 0.7
#define c_smallImpacts 1.0
#define c_bigImpacts 0.25

#define intro_duration 3.0

#define c_flickering_frequency 12.0

vec2 hash( vec2 x )  // replace this by something better
{
  const vec2 k = vec2( 0.3183099, 0.3678794 );
  x = x*k + k.yx;
  return -1.0 + 2.0*fract( 16.0 * k*fract( x.x*x.y*(x.x+x.y)) );
}

float noise( vec2 p )
{
  vec2 i = floor( p );
  vec2 f = fract( p );
	
	vec2 u = f*f*(3.0-2.0*f);

  return mix( mix( dot( hash( i + vec2(0.0,0.0) ), f - vec2(0.0,0.0) ), 
                    dot( hash( i + vec2(1.0,0.0) ), f - vec2(1.0,0.0) ), u.x),
              mix( dot( hash( i + vec2(0.0,1.0) ), f - vec2(0.0,1.0) ), 
                    dot( hash( i + vec2(1.0,1.0) ), f - vec2(1.0,1.0) ), u.x), u.y);
}

vec3 noiseDerivative(vec2 p )
{
  vec2 i = floor( p );
  vec2 f = fract( p );

  vec2 u = f*f*f*(f*(f*6.0-15.0)+10.0);
  vec2 du = 30.0*f*f*(f*(f-2.0)+1.0);

  vec2 ga = hash( i + vec2(0.0,0.0) );
  vec2 gb = hash( i + vec2(1.0,0.0) );
  vec2 gc = hash( i + vec2(0.0,1.0) );
  vec2 gd = hash( i + vec2(1.0,1.0) );
  
  float va = dot( ga, f - vec2(0.0,0.0) );
  float vb = dot( gb, f - vec2(1.0,0.0) );
  float vc = dot( gc, f - vec2(0.0,1.0) );
  float vd = dot( gd, f - vec2(1.0,1.0) );

  return vec3( va + u.x*(vb-va) + u.y*(vc-va) + u.x*u.y*(va-vb-vc+vd),   // value
                ga + u.x*(gb-ga) + u.y*(gc-ga) + u.x*u.y*(ga-gb-gc+gd) +  // derivatives
                du * (u.yx*(va-vb-vc+vd) + vec2(vb,vc) - va));
}

float fbm(vec2 x, float H )
{    
  float G = exp2(-H);
  float f = 1.0;
  float a = 1.0;
  float t = 0.0;
  for( int i=0; i < 10; i++ )
  {
    t += a*noise(f*x);
    f *= 2.0;
    a *= G;
  }
  return t;
}

vec3 fbmDerivatives(vec2 position ) 
{
  float f = 2.;
  float s = .9;
  float a = 0.0;
  float b = .5;
  vec2  d = vec2(0.0);
  mat2  m = mat2(1.0,0.0,
                  0.0,1.0);
  for( int i=0; i < 7; i++ )
  {
    vec3 n = noiseDerivative(position);
    a += b*n.x;          // accumulate values
    d += b*m*n.yz;      // accumulate derivatives
    b *= s;
    position = f*position;
    m = f*m;
  }
  return vec3( a, normalize(d) );
}

vec2 rotate(vec2 v, float a) {
  float s = sin(a);
  float c = cos(a);
  mat2 m = mat2(c, -s, s, c);
  return m * v;
}

float textureMask(vec2 coord, float spread, float aspectRatio) {
  return smoothstep(0., spread / aspectRatio, 1. - abs(coord.x)) * smoothstep(0., spread, 1. - abs(coord.y));
}

float random1d(float x) {
    return fract(sin(x)*100000.0);
}

float noise1d(float x)
{
    float i = floor(x);  // integer
    float f = fract(x);  // fraction
    return mix(random1d(i), random1d(i + 1.0), smoothstep(0.,1.,f));
}

void main() {
  float aspectRatio = u_size.x / u_size.y;
  vec2 position = v_tex_coord;
  position.x *= aspectRatio;
  vec2 positionCentered = v_tex_coord * 2. - 1.;
  positionCentered.x *= aspectRatio;

  vec2 noiseOffset = vec2(c_noiseOffsetX, c_noiseOffsetY);

  vec3 fbmNoiseDerivative = fbmDerivatives(positionCentered * c_noiseScale + noiseOffset);
  float fbmNoise = fbmNoiseDerivative.x * .5 + .5;
  vec3 normal = normalize(vec3(fbmNoiseDerivative.yz, 1.));

  float backgroundNoise = fbmNoise;
  float grainNoise = noise(position * 400.) * .5 + .5;
  backgroundNoise += 1. - smoothstep(0., 1. - position.y, backgroundNoise);
  backgroundNoise += 1. - smoothstep(0., position.x, backgroundNoise * .7);
  backgroundNoise = 1. - backgroundNoise;
  backgroundNoise += .5;

  normal.xy *= fbmNoise - .3;
  normal.xy *= 2.;
  normal = normalize(normal);
  
  float impactNoise = fbm(position * 200. + noiseOffset * 10., 1.) * .5 + .5;
  impactNoise = smoothstep(.6, 1., impactNoise);
  impactNoise *= smoothstep(0., 1., noise(position * 30.));
  impactNoise *= smoothstep(-1., 1., noise(position * 5.));
  impactNoise *= 10.;
  impactNoise = 1. - impactNoise * c_smallImpacts;

  float bigImpactNoise = fbmNoise;
  bigImpactNoise = smoothstep(.6, 1., bigImpactNoise);
  bigImpactNoise *= smoothstep(0., 1., noise(position * 10.));
  bigImpactNoise *= 6.;
  bigImpactNoise = 1. - bigImpactNoise * c_bigImpacts;

  vec3 color = vec3(.9);
  color *= .8 + backgroundNoise * .2;
  color *= impactNoise;
  color *= bigImpactNoise;

  float time_factor = pow(min(i_time / intro_duration, 1.0), 1.0 / 8.0);
  time_factor = mix(1.0, time_factor, i_play);
    
  float size_spread = mix(0.25, 1.0, time_factor) * c_sideSpread;
  float mask_spread = mix(1.0, c_maskSpread, time_factor);

  vec2 textureCoord = positionCentered;
  textureCoord = rotate(textureCoord, -c_imageRotation);
  textureCoord.x /= aspectRatio;
  textureCoord *= 1. / c_imageSize;
  vec3 texel = texture2D(u_texture, textureCoord * .5 + .5).rgb;
  float isTexture = textureMask(textureCoord, .05 + (1. - size_spread), aspectRatio);
  isTexture *= min(1., (fbmNoise * 2. - 1.) + (1. - mask_spread) * 2. - smoothstep(0., 1., length(positionCentered)));
  isTexture = smoothstep(.8, 1., isTexture + fbmNoise * size_spread);
  
  if(!c_displayMedia) {
    isTexture *= 0.;
  }

  color = mix(color, texel * color, isTexture);

  float light = dot(normal, normalize(vec3(c_lightPositionX, c_lightPositionY, 1.)));
  light = pow(light, 5.);
    
  float lightIntensity = c_lightIntensity * (1.0 + 1.0 * (noise1d(c_flickering_frequency * u_time) - 0.5));
  float light_amount = light * lightIntensity * 0.1 * (1. + isTexture * 2. * (1. - (color.r + color.g + color.b) / 3.));
  color = min(color + light_amount, 1.0);

  gl_FragColor = vec4(color, 1.0);
}
