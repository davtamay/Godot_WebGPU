shader_type canvas_item;
uniform sampler2DArray sensor : filter_linear;
uniform vec2 sensor_size = vec2(4.0, 2.0);
uniform float sensor_layer = 0.0;
uniform float expected_meters = 2.0;
void fragment() {
 float meters = texture(sensor, vec3(vec2(0.5) / sensor_size, sensor_layer)).r;
 bool correct = abs(meters - expected_meters) < max(0.001, abs(expected_meters) * 0.002);
 COLOR = correct ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
}
