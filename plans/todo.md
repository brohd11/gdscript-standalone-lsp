
- some issues with const inheritance - dock_manager.gd, UFile in the inner class
- u_node.gd, has issues with global scope and object methods

- make the "(" insert for functions a config option. VS code doesn't work for example
- a request after "return |" is doing nothing -- this seems to be misdiagnosed, was in combination with the function declaration bug below


# invalid showing
- Nil

# invalid context
- "<const|var> |" - should not be suggesting in the name part of the declaration
- on topic of above "<func> |" - should not show standard things, only overidables



# connection closing - complete pending test
## gote

this is causeing lsp to close, cross plat
Related to the order, if the n refeence is in a function defined beloow get_nodes, this has no issues
However, other scenarios where it is "my_var.|" can cause it too, so unsure of exact cause
```
func test():
	var n = get_nodes()
	n.|

func get_nodes() -> Node:
	return Node.new()
```

## gote + vscode
```
func get_nodes()|
```
on completion entered, either in the parens or out (mac and windows are not behaving identical in regard to insert)
