// Copyright 2026 AlgorithMagic

class_name ValidBasic
extends Node


## Called when the node enters the scene tree.
func _ready() -> void:
	print("ready")


func add_numbers(left: int, right: int) -> int:
	return left + right
