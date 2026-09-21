/**************************************************************************/
/*  webxr_composition_layer_cylinder.cpp                                  */
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

#include "webxr_composition_layer_cylinder.h"

#include "core/object/class_db.h"

#include "scene/resources/mesh.h"

void WebXRCompositionLayerCylinder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &WebXRCompositionLayerCylinder::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &WebXRCompositionLayerCylinder::get_radius);
	ClassDB::bind_method(D_METHOD("set_aspect_ratio", "aspect_ratio"), &WebXRCompositionLayerCylinder::set_aspect_ratio);
	ClassDB::bind_method(D_METHOD("get_aspect_ratio"), &WebXRCompositionLayerCylinder::get_aspect_ratio);
	ClassDB::bind_method(D_METHOD("set_central_angle", "angle"), &WebXRCompositionLayerCylinder::set_central_angle);
	ClassDB::bind_method(D_METHOD("get_central_angle"), &WebXRCompositionLayerCylinder::get_central_angle);
	ClassDB::bind_method(D_METHOD("set_fallback_segments", "segments"), &WebXRCompositionLayerCylinder::set_fallback_segments);
	ClassDB::bind_method(D_METHOD("get_fallback_segments"), &WebXRCompositionLayerCylinder::get_fallback_segments);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0.01,100.0,0.01,or_greater"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "aspect_ratio", PROPERTY_HINT_RANGE, "0.01,100.0,0.01,or_greater"), "set_aspect_ratio", "get_aspect_ratio");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "central_angle", PROPERTY_HINT_RANGE, "0.01,360,0.01,radians_as_degrees"), "set_central_angle", "get_central_angle");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "fallback_segments", PROPERTY_HINT_RANGE, "1,128,1"), "set_fallback_segments", "get_fallback_segments");
}

Ref<Mesh> WebXRCompositionLayerCylinder::_create_fallback_mesh() {
	// Same construction as the OpenXR cylinder layer's fallback: an arc of
	// central_angle around the node, facing inward.
	Ref<ArrayMesh> mesh;
	mesh.instantiate();

	const float arc_length = radius * central_angle;
	const float half_height = ((1.0 / aspect_ratio) * arc_length) / 2.0;

	Array arrays;
	arrays.resize(ArrayMesh::ARRAY_MAX);

	Vector<Vector3> vertices;
	Vector<Vector3> normals;
	Vector<Vector2> uvs;
	Vector<int> indices;

	const float delta_angle = central_angle / fallback_segments;
	const float start_angle = (-Math::PI / 2.0) - (central_angle / 2.0);

	for (uint32_t i = 0; i < fallback_segments + 1; i++) {
		const float current_angle = start_angle + (delta_angle * i);
		const float x = radius * Math::cos(current_angle);
		const float z = radius * Math::sin(current_angle);
		const Vector3 normal(Math::cos(current_angle), 0, Math::sin(current_angle));

		vertices.push_back(Vector3(x, -half_height, z));
		normals.push_back(normal);
		uvs.push_back(Vector2((float)i / fallback_segments, 1));

		vertices.push_back(Vector3(x, half_height, z));
		normals.push_back(normal);
		uvs.push_back(Vector2((float)i / fallback_segments, 0));
	}

	for (uint32_t i = 0; i < fallback_segments; i++) {
		const uint32_t index = i * 2;
		indices.push_back(index);
		indices.push_back(index + 1);
		indices.push_back(index + 3);
		indices.push_back(index);
		indices.push_back(index + 3);
		indices.push_back(index + 2);
	}

	arrays[ArrayMesh::ARRAY_VERTEX] = vertices;
	arrays[ArrayMesh::ARRAY_NORMAL] = normals;
	arrays[ArrayMesh::ARRAY_TEX_UV] = uvs;
	arrays[ArrayMesh::ARRAY_INDEX] = indices;

	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

void WebXRCompositionLayerCylinder::_fill_shape_params(float *r_params) const {
	r_params[0] = radius;
	r_params[1] = central_angle;
	r_params[2] = aspect_ratio;
}

void WebXRCompositionLayerCylinder::set_radius(float p_radius) {
	ERR_FAIL_COND(p_radius <= 0);
	radius = p_radius;
	update_fallback_mesh();
}

float WebXRCompositionLayerCylinder::get_radius() const {
	return radius;
}

void WebXRCompositionLayerCylinder::set_aspect_ratio(float p_aspect_ratio) {
	ERR_FAIL_COND(p_aspect_ratio <= 0);
	aspect_ratio = p_aspect_ratio;
	update_fallback_mesh();
}

float WebXRCompositionLayerCylinder::get_aspect_ratio() const {
	return aspect_ratio;
}

void WebXRCompositionLayerCylinder::set_central_angle(float p_angle) {
	ERR_FAIL_COND(p_angle <= 0);
	central_angle = p_angle;
	update_fallback_mesh();
}

float WebXRCompositionLayerCylinder::get_central_angle() const {
	return central_angle;
}

void WebXRCompositionLayerCylinder::set_fallback_segments(uint32_t p_fallback_segments) {
	ERR_FAIL_COND(p_fallback_segments == 0);
	fallback_segments = p_fallback_segments;
	update_fallback_mesh();
}

uint32_t WebXRCompositionLayerCylinder::get_fallback_segments() const {
	return fallback_segments;
}
