// Two flat bands with a sharp edge at the horizon, so a sample anywhere in a
// band has one exact expected colour.
shader_type sky;

uniform vec3 upper : source_color = vec3(0.1, 0.6, 0.7);
uniform vec3 lower : source_color = vec3(0.9, 0.5, 0.1);

void sky() {
	COLOR = EYEDIR.y > 0.0 ? upper : lower;
}
