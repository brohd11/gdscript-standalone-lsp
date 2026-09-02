extends RefCounted

class Product:
	var product_member: int
	var functions := {}

class Factory:
	static func make() -> Product:
		return Product.new()
