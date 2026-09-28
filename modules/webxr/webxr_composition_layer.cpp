/**************************************************************************/
/*  webxr_composition_layer.cpp                                           */
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

#include "webxr_composition_layer.h"

#include "core/object/class_db.h"

#include "core/config/engine.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "servers/xr/xr_server.h"

#ifdef WEB_ENABLED
#include "webxr_interface_js.h"

static WebXRInterfaceJS *_get_webxr_interface() {
	XRServer *xr_server = XRServer::get_singleton();
	if (xr_server == nullptr) {
		return nullptr;
	}
	Ref<XRInterface> interface = xr_server->find_interface("WebXR");
	return Object::cast_to<WebXRInterfaceJS>(interface.ptr());
}
#endif

void WebXRCompositionLayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_layer_viewport", "viewport"), &WebXRCompositionLayer::set_layer_viewport);
	ClassDB::bind_method(D_METHOD("get_layer_viewport"), &WebXRCompositionLayer::get_layer_viewport);
	ClassDB::bind_method(D_METHOD("set_sort_order", "order"), &WebXRCompositionLayer::set_sort_order);
	ClassDB::bind_method(D_METHOD("get_sort_order"), &WebXRCompositionLayer::get_sort_order);
	ClassDB::bind_method(D_METHOD("set_alpha_blend", "alpha_blend"), &WebXRCompositionLayer::set_alpha_blend);
	ClassDB::bind_method(D_METHOD("get_alpha_blend"), &WebXRCompositionLayer::get_alpha_blend);
	ClassDB::bind_method(D_METHOD("set_opacity", "opacity"), &WebXRCompositionLayer::set_opacity);
	ClassDB::bind_method(D_METHOD("get_opacity"), &WebXRCompositionLayer::get_opacity);
	ClassDB::bind_method(D_METHOD("is_natively_supported"), &WebXRCompositionLayer::is_natively_supported);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "layer_viewport", PROPERTY_HINT_NODE_TYPE, "SubViewport"), "set_layer_viewport", "get_layer_viewport");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sort_order", PROPERTY_HINT_NONE, ""), "set_sort_order", "get_sort_order");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "alpha_blend", PROPERTY_HINT_NONE, ""), "set_alpha_blend", "get_alpha_blend");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "opacity", PROPERTY_HINT_RANGE, "0.0,1.0,0.001"), "set_opacity", "get_opacity");
}

bool WebXRCompositionLayer::_should_use_fallback_node() const {
	if (Engine::get_singleton()->is_editor_hint()) {
		return true;
	}
	return !is_natively_supported();
}

void WebXRCompositionLayer::_create_fallback_node() {
	ERR_FAIL_COND(fallback);
	fallback = memnew(MeshInstance3D);
	fallback->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	add_child(fallback, false, INTERNAL_MODE_FRONT);
	should_update_fallback_mesh = true;
}

void WebXRCompositionLayer::_remove_fallback_node() {
	ERR_FAIL_NULL(fallback);
	remove_child(fallback);
	fallback->queue_free();
	fallback = nullptr;
}

void WebXRCompositionLayer::_reset_fallback_material() {
	if (fallback == nullptr) {
		return;
	}
	if (layer_viewport == nullptr) {
		fallback->set_material_override(Ref<Material>());
		return;
	}
	// The same unshaded viewport-textured material the OpenXR nodes use for
	// their fallback. On a WebGPU export it is a runtime-built material, so a
	// project that shows the fallback there bakes its shader through a
	// BakeAnchor twin; in sessions with the "layers" feature it never draws.
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	material->set_transparency(alpha_blend ? BaseMaterial3D::TRANSPARENCY_ALPHA : BaseMaterial3D::TRANSPARENCY_DISABLED);
	material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, layer_viewport->get_texture());
	fallback->set_material_override(material);
}

void WebXRCompositionLayer::_register() {
#ifdef WEB_ENABLED
	if (registered) {
		return;
	}
	WebXRInterfaceJS *webxr = _get_webxr_interface();
	if (webxr != nullptr) {
		webxr->composition_layer_register(this);
		registered = true;
	}
#endif
}

void WebXRCompositionLayer::_unregister() {
#ifdef WEB_ENABLED
	if (!registered) {
		return;
	}
	WebXRInterfaceJS *webxr = _get_webxr_interface();
	if (webxr != nullptr) {
		webxr->composition_layer_unregister(this);
	}
	registered = false;
#endif
}

void WebXRCompositionLayer::_push_state() {
#ifdef WEB_ENABLED
	if (!registered) {
		return;
	}
	WebXRInterfaceJS *webxr = _get_webxr_interface();
	if (webxr == nullptr) {
		return;
	}
	// The interface reads this snapshot when it prepares the frame, the same
	// hand-off the OpenXR layer nodes make to their extension.
	const bool active = layer_viewport != nullptr && is_visible_in_tree() && !Engine::get_singleton()->is_editor_hint();
	float params[PARAM_COUNT];
	fill_params(params);
	webxr->composition_layer_update(this, active, params,
			layer_viewport != nullptr ? layer_viewport->get_viewport_rid() : RID(),
			layer_viewport != nullptr ? layer_viewport->get_size() : Size2i());
#endif
}

void WebXRCompositionLayer::update_fallback_mesh() {
	should_update_fallback_mesh = true;
}

void WebXRCompositionLayer::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (!fallback && _should_use_fallback_node()) {
				_create_fallback_node();
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (fallback && should_update_fallback_mesh) {
				fallback->set_mesh(_create_fallback_mesh());
				_reset_fallback_material();
				should_update_fallback_mesh = false;
			}
			_push_state();
		} break;
		case NOTIFICATION_ENTER_TREE: {
			_register();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_unregister();
		} break;
	}
}

PackedStringArray WebXRCompositionLayer::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (layer_viewport == nullptr) {
		warnings.push_back(RTR("A SubViewport must be assigned as the layer viewport for this layer to show anything."));
	}
	return warnings;
}

void WebXRCompositionLayer::set_layer_viewport(SubViewport *p_viewport) {
	if (layer_viewport == p_viewport) {
		return;
	}
	layer_viewport = p_viewport;
	if (layer_viewport != nullptr) {
		// A layer is submitted every frame, so its viewport must render every
		// frame too: the visibility-driven modes never see the layer as
		// visible.
		SubViewport::UpdateMode update_mode = layer_viewport->get_update_mode();
		if (update_mode == SubViewport::UPDATE_WHEN_VISIBLE || update_mode == SubViewport::UPDATE_WHEN_PARENT_VISIBLE) {
			WARN_PRINT_ONCE("WebXR composition layers cannot use SubViewports with UPDATE_WHEN_VISIBLE or UPDATE_WHEN_PARENT_VISIBLE; switching to UPDATE_ALWAYS.");
			layer_viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
		}
	}
	_reset_fallback_material();
	update_configuration_warnings();
}

SubViewport *WebXRCompositionLayer::get_layer_viewport() const {
	return layer_viewport;
}

void WebXRCompositionLayer::set_sort_order(int p_order) {
	sort_order = p_order;
}

int WebXRCompositionLayer::get_sort_order() const {
	return sort_order;
}

void WebXRCompositionLayer::set_alpha_blend(bool p_alpha_blend) {
	alpha_blend = p_alpha_blend;
	_reset_fallback_material();
}

bool WebXRCompositionLayer::get_alpha_blend() const {
	return alpha_blend;
}

void WebXRCompositionLayer::set_opacity(float p_opacity) {
	opacity = CLAMP(p_opacity, 0.0f, 1.0f);
}

float WebXRCompositionLayer::get_opacity() const {
	return opacity;
}

bool WebXRCompositionLayer::is_natively_supported() const {
#ifdef WEB_ENABLED
	WebXRInterfaceJS *webxr = _get_webxr_interface();
	return webxr != nullptr && webxr->composition_layers_supported();
#else
	return false;
#endif
}

WebXRCompositionLayer::LayerType WebXRCompositionLayer::get_layer_type() const {
	return _get_layer_type();
}

void WebXRCompositionLayer::fill_params(float *r_params) const {
	// The layer's pose lives in the session's reference space, which the
	// XROrigin3D maps to the world origin.
	Transform3D xf = get_transform();
	XRServer *xr_server = XRServer::get_singleton();
	if (is_inside_tree() && xr_server != nullptr) {
		xf = xr_server->get_world_origin().affine_inverse() * get_global_transform();
	}
	const Quaternion orientation = xf.basis.get_rotation_quaternion();
	r_params[PARAM_POSITION + 0] = xf.origin.x;
	r_params[PARAM_POSITION + 1] = xf.origin.y;
	r_params[PARAM_POSITION + 2] = xf.origin.z;
	r_params[PARAM_ORIENTATION + 0] = orientation.x;
	r_params[PARAM_ORIENTATION + 1] = orientation.y;
	r_params[PARAM_ORIENTATION + 2] = orientation.z;
	r_params[PARAM_ORIENTATION + 3] = orientation.w;
	for (int i = 0; i < 4; i++) {
		r_params[PARAM_SHAPE + i] = 0.0f;
	}
	_fill_shape_params(r_params + PARAM_SHAPE);
	r_params[PARAM_ALPHA_BLEND] = alpha_blend ? 1.0f : 0.0f;
	r_params[PARAM_OPACITY] = opacity;
	r_params[PARAM_SORT_ORDER] = (float)sort_order;
	const Size2i size = layer_viewport != nullptr ? layer_viewport->get_size() : Size2i();
	r_params[PARAM_VIEW_WIDTH] = (float)size.width;
	r_params[PARAM_VIEW_HEIGHT] = (float)size.height;
}

void WebXRCompositionLayer::session_state_changed() {
	if (_should_use_fallback_node()) {
		if (fallback == nullptr) {
			_create_fallback_node();
		}
	} else if (fallback != nullptr) {
		_remove_fallback_node();
	}
}

WebXRCompositionLayer::WebXRCompositionLayer() {
	set_process_internal(true);
}

WebXRCompositionLayer::~WebXRCompositionLayer() {
	_unregister();
}
