/**************************************************************************/
/*  webxr_composition_layer.h                                             */
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

#pragma once

#include "scene/3d/node_3d.h"

class Mesh;
class MeshInstance3D;
class SubViewport;

// A WebXR composition layer: a SubViewport handed to the browser's XR
// compositor as a layer of its own (the WebXR Layers module), so it is
// sampled once at display time instead of through the eye buffer. Mirrors
// the OpenXRCompositionLayer node family so a scene can carry either.
//
// The session must be requested with the "layers" feature. Outside a
// session, in the editor, and in sessions without the feature, a fallback
// mesh shows the viewport in the scene instead.
class WebXRCompositionLayer : public Node3D {
	GDCLASS(WebXRCompositionLayer, Node3D);

public:
	enum LayerType {
		LAYER_TYPE_QUAD,
		LAYER_TYPE_CYLINDER,
		LAYER_TYPE_EQUIRECT,
	};

	// Values handed to the browser once per frame (library_godot_webxr_ext.js
	// reads the same layout): pose in the reference space, four shape values
	// a subclass fills in, then the layer's blend, opacity, order and size.
	static const int PARAM_COUNT = 16;
	enum ParamIndex {
		PARAM_POSITION = 0,
		PARAM_ORIENTATION = 3,
		PARAM_SHAPE = 7,
		PARAM_ALPHA_BLEND = 11,
		PARAM_OPACITY = 12,
		PARAM_SORT_ORDER = 13,
		PARAM_VIEW_WIDTH = 14,
		PARAM_VIEW_HEIGHT = 15,
	};

private:
	SubViewport *layer_viewport = nullptr;
	int sort_order = 1;
	bool alpha_blend = false;
	float opacity = 1.0;

	MeshInstance3D *fallback = nullptr;
	bool should_update_fallback_mesh = false;
	bool registered = false;

	bool _should_use_fallback_node() const;
	void _create_fallback_node();
	void _remove_fallback_node();
	void _reset_fallback_material();
	void _register();
	void _unregister();
	void _push_state();

protected:
	static void _bind_methods();
	void _notification(int p_what);

	virtual Ref<Mesh> _create_fallback_mesh() = 0;
	virtual LayerType _get_layer_type() const = 0;
	virtual void _fill_shape_params(float *r_params) const = 0;

	void update_fallback_mesh();

public:
	PackedStringArray get_configuration_warnings() const override;

	void set_layer_viewport(SubViewport *p_viewport);
	SubViewport *get_layer_viewport() const;

	void set_sort_order(int p_order);
	int get_sort_order() const;

	void set_alpha_blend(bool p_alpha_blend);
	bool get_alpha_blend() const;

	void set_opacity(float p_opacity);
	float get_opacity() const;

	bool is_natively_supported() const;

	// For the interface: the layer's kind, this frame's values, and the
	// session having started or ended (the fallback mesh follows).
	LayerType get_layer_type() const;
	void fill_params(float *r_params) const;
	void session_state_changed();

	WebXRCompositionLayer();
	~WebXRCompositionLayer();
};
