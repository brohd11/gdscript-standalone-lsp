
- some issues with const inheritance - dock_manager.gd, UFile in the inner class
- u_node.gd, has issues with global scope and object methods

- make the "(" insert for functions a config option. VS code doesn't work for example
- a request after "return |" is doing nothing -- this seems to be misdiagnosed, was in combination with the function declaration bug below


# invalid showing
- Nil


"new" constructor completion must find the closest referenc for the class

- tree sitter grammar seems too lenient for error, new lines don't seem to be regarded?

## goto

needs to go to actual definition, clicking Types.ExitCode should go to Types script, not the current declaration
```
const Types = preload("res://addons/addon_lib/gdsh/internal/types.gd")
const ExitCode = Types.ExitCode
```


Seems a class that is missing altogether silently fails