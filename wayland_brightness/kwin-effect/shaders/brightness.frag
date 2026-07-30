#version 140

uniform sampler2D sampler;
uniform float brightness;

in vec2 texcoord0;

out vec4 fragColor;

void main()
{
    vec4 tex = texture(sampler, texcoord0);
    // Multiply RGB by the brightness factor; keep alpha intact.
    // Linear-space multiply is acceptable for software dimming on OLED
    // (matches the behaviour of xrandr --brightness and mutter SetCrtcGamma).
    tex.rgb *= brightness;
    fragColor = tex;
}