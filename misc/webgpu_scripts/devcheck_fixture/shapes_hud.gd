# Canvas shapes redrawn every frame (the Dino Pals HUD pattern): each shape
# becomes a canvas polygon built through the staging path.
extends Control

func _process(_delta: float) -> void:
	queue_redraw()

func _draw() -> void:
	var s := get_viewport_rect().size
	draw_rect(Rect2(Vector2.ZERO, s), Color(0.15, 0.15, 0.2))
	draw_circle(s * Vector2(0.25, 0.5), s.y * 0.3, Color(1.0, 0.8, 0.0))
	draw_colored_polygon(PackedVector2Array([s * Vector2(0.55, 0.2), s * Vector2(0.95, 0.2), s * Vector2(0.95, 0.8), s * Vector2(0.55, 0.8)]), Color(0.0, 0.7, 1.0))
	for i in 30:
		draw_arc(s * Vector2(0.25, 0.5), s.y * 0.35 + i, 0.0, PI * 0.5, 16, Color.WHITE, 1.0)
