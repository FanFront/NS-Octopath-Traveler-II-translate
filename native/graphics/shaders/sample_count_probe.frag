#version 450
layout(location=0) noperspective in vec4 uv;
layout(location=1) flat in vec4 layer;
layout(location=0) out vec4 color;
void main(){if(uv.x<0.5)discard;color=vec4(float(floatBitsToUint(layer.x))/32.0,uv.x,uv.y,1);}
