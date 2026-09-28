/**************************************************************************/
/*  webxr_composition_layer_equirect.cpp                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "webxr_composition_layer_equirect.h"

#include "core/object/class_db.h"

#include "scene/resources/mesh.h"

void WebXRCompositionLayerEquirect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &WebXRCompositionLayerEquirect::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &WebXRCompositionLayerEquirect::get_radius);
	ClassDB::bind_method(D_METHOD("set_central_horizontal_angle", "angle"), &WebXRCompositionLayerEquirect::set_central_horizontal_angle);
	ClassDB::bind_method(D_METHOD("get_central_horizontal_angle"), &WebXRCompositionLayerEquirect::get_central_horizontal_angle);
	ClassDB::bind_method(D_METHOD("set_upper_vertical_angle", "angle"), &WebXRCompositionLayerEquirect::set_upper_vertical_angle);
	ClassDB::bind_method(D_METHOD("get_upper_vertical_angle"), &WebXRCompositionLayerEquirect::get_upper_vertical_angle);
	ClassDB::bind_method(D_METHOD("set_lower_vertical_angle", "angle"), &WebXRCompositionLayerEquirect::set_lower_vertical_angle);
	ClassDB::bind_method(D_METHOD("get_lower_vertical_angle"), &WebXRCompositionLayerEquirect::get_lower_vertical_angle);
	ClassDB::bind_method(D_METHOD("set_fallback_segments", "segments"), &WebXRCompositionLayerEquirect::set_fallback_segments);
	ClassDB::bind_method(D_METHOD("get_fallback_segments"), &WebXRCompositionLayerEquirect::get_fallback_segments);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0.01,100.0,0.01,or_greater"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "central_horizontal_angle", PROPERTY_HINT_RANGE, "0.01,360,0.01,radians_as_degrees"), "set_central_horizontal_angle", "get_central_horizontal_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "upper_vertical_angle", PROPERTY_HINT_RANGE, "0.01,90,0.01,radians_as_degrees"), "set_upper_vertical_angle", "get_upper_vertical_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lower_vertical_angle", PROPERTY_HINT_RANGE, "0.01,90,0.01,radians_as_degrees"), "set_lower_vertical_angle", "get_lower_vertical_angle");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "fallback_segments", PROPERTY_HINT_RANGE, "1,128,1"), "set_fallback_segments", "get_fallback_segments");
}

Ref<Mesh> WebXRCompositionLayerEquirect::_create_fallback_mesh() {
	// Same construction as the OpenXR equirect layer's fallback: a patch of a
	// sphere around the node, facing inward.
	Ref<ArrayMesh> mesh;
	mesh.instantiate();

	Array arrays;
	arrays.resize(ArrayMesh::ARRAY_MAX);

	Vector<Vector3> vertices;
	Vector<Vector3> normals;
	Vector<Vector2> uvs;
	Vector<int> indices;

	const float step_horizontal = central_horizontal_angle / fallback_segments;
	const float step_vertical = (upper_vertical_angle + lower_vertical_angle) / fallback_segments;
	const float start_horizontal_angle = Math::PI - (central_horizontal_angle / 2.0);

	for (uint32_t i = 0; i < fallback_segments + 1; i++) {
		for (uint32_t j = 0; j < fallback_segments + 1; j++) {
			const float horizontal_angle = start_horizontal_angle + (step_horizontal * i);
			const float vertical_angle = -lower_vertical_angle + (step_vertical * j);

			const Vector3 vertex(
					radius * Math::cos(vertical_angle) * Math::sin(horizontal_angle),
					radius * Math::sin(vertical_angle),
					radius * Math::cos(vertical_angle) * Math::cos(horizontal_angle));

			vertices.push_back(vertex);
			normals.push_back(vertex.normalized());
			uvs.push_back(Vector2(1.0 - ((float)i / fallback_segments), 1.0 - (float(j) / fallback_segments)));
		}
	}

	for (uint32_t i = 0; i < fallback_segments; i++) {
		for (uint32_t j = 0; j < fallback_segments; j++) {
			const uint32_t index = i * (fallback_segments + 1) + j;
			indices.push_back(index);
			indices.push_back(index + fallback_segments + 1);
			indices.push_back(index + fallback_segments + 2);

			indices.push_back(index);
			indices.push_back(index + fallback_segments + 2);
			indices.push_back(index + 1);
		}
	}

	arrays[ArrayMesh::ARRAY_VERTEX] = vertices;
	arrays[ArrayMesh::ARRAY_NORMAL] = normals;
	arrays[ArrayMesh::ARRAY_TEX_UV] = uvs;
	arrays[ArrayMesh::ARRAY_INDEX] = indices;

	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

void WebXRCompositionLayerEquirect::_fill_shape_params(float *r_params) const {
	r_params[0] = radius;
	r_params[1] = central_horizontal_angle;
	r_params[2] = upper_vertical_angle;
	// WebXR measures the lower angle downward from the horizon as a negative
	// value; the property keeps the OpenXR node's positive convention.
	r_params[3] = -lower_vertical_angle;
}

void WebXRCompositionLayerEquirect::set_radius(float p_radius) {
	ERR_FAIL_COND(p_radius <= 0);
	radius = p_radius;
	update_fallback_mesh();
}

float WebXRCompositionLayerEquirect::get_radius() const {
	return radius;
}

void WebXRCompositionLayerEquirect::set_central_horizontal_angle(float p_angle) {
	ERR_FAIL_COND(p_angle <= 0);
	central_horizontal_angle = p_angle;
	update_fallback_mesh();
}

float WebXRCompositionLayerEquirect::get_central_horizontal_angle() const {
	return central_horizontal_angle;
}

void WebXRCompositionLayerEquirect::set_upper_vertical_angle(float p_angle) {
	ERR_FAIL_COND(p_angle <= 0);
	upper_vertical_angle = p_angle;
	update_fallback_mesh();
}

float WebXRCompositionLayerEquirect::get_upper_vertical_angle() const {
	return upper_vertical_angle;
}

void WebXRCompositionLayerEquirect::set_lower_vertical_angle(float p_angle) {
	ERR_FAIL_COND(p_angle <= 0);
	lower_vertical_angle = p_angle;
	update_fallback_mesh();
}

float WebXRCompositionLayerEquirect::get_lower_vertical_angle() const {
	return lower_vertical_angle;
}

void WebXRCompositionLayerEquirect::set_fallback_segments(uint32_t p_fallback_segments) {
	ERR_FAIL_COND(p_fallback_segments == 0);
	fallback_segments = p_fallback_segments;
	update_fallback_mesh();
}

uint32_t WebXRCompositionLayerEquirect::get_fallback_segments() const {
	return fallback_segments;
}
