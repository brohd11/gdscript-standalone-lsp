extends RefCounted

const Namespace = preload("res://return_factory.gd")
const FactoryAlias = Namespace.Factory

func inspect() -> void:
	var item = Namespace.Factory.make()
	item.product_member
	item = 1
	var alias_item = FactoryAlias.make()
	alias_item.product_member
	Namespace.Factory.make().product_member
	Namespace.Factory.make().functions.keys()
	self.inspect()
	var products: Array[Namespace.Product] = []
	products[0].product_member
	(Namespace.Factory.make()).product_member
	Namespace.Factory.make(
	).product_member
