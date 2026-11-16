extends Node

var counts := PackedInt32Array()
var mutex := Mutex.new()

func _work(index: int) -> void:
	mutex.lock()
	counts[index] += 1
	mutex.unlock()

func _ready() -> void:
	var groups := 0
	var valid := true
	for repetition in range(64):
		for elements in [0, 1, 17]:
			for tasks in [1, 4]:
				counts.resize(elements)
				counts.fill(0)
				var group := WorkerThreadPool.add_group_task(_work, elements, tasks, repetition % 2 == 0, "Lifecycle regression")
				WorkerThreadPool.wait_for_group_task_completion(group)
				for value in counts:
					valid = valid and value == 1
				groups += 1
	var result := {"passed": valid, "groups": groups, "version": Engine.get_version_info()}
	JavaScriptBridge.eval("window.workerGroupResult=" + JSON.stringify(result))
