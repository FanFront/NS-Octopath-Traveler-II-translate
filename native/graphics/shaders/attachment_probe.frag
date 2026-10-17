#version 450
layout(set=0,binding=3) uniform sampler2D depth_texture;
layout(location=0) out vec4 color;
void main(){float z=texelFetch(depth_texture,ivec2(gl_FragCoord.xy),0).r;color=vec4(z,z,z,1);}
