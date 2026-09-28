/**************************************************************************/
/*  webxr_composition_layer_cylinder.h                                    */
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

#include "webxr_composition_layer.h"

class WebXRCompositionLayerCylinder : public WebXRCompositionLayer {
	GDCLASS(WebXRCompositionLayerCylinder, WebXRCompositionLayer);

	float radius = 1.0;
	float aspect_ratio = 1.0;
	float central_angle = Math::PI / 2.0;
	uint32_t fallback_segments = 10;

protected:
	static void _bind_methods();

	virtual Ref<Mesh> _create_fallback_mesh() override;
	virtual LayerType _get_layer_type() const override { return LAYER_TYPE_CYLINDER; }
	virtual void _fill_shape_params(float *r_params) const override;

public:
	void set_radius(float p_radius);
	float get_radius() const;

	void set_aspect_ratio(float p_aspect_ratio);
	float get_aspect_ratio() const;

	void set_central_angle(float p_angle);
	float get_central_angle() const;

	void set_fallback_segments(uint32_t p_fallback_segments);
	uint32_t get_fallback_segments() const;
};
