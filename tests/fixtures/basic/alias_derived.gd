class_name AliasDerived
extends AliasNamespace.Bridge.BaseAlias

func inspect(value: Imported, code: ExitCode) -> void:
	inherited_alias_member()
	print(value, code)
